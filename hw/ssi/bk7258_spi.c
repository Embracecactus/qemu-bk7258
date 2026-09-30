/*
 * BK7258 bounded 8-bit SPI master using the native QEMU SSI bus.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Sources and explicit functional policies: docs/system/arm/bk7258.rst.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/bk7258_spi.h"

#define TYPE_BK7258_SPI_BUS "bk7258-spi-bus"
#define MASTER (1U << 22)
#define ENABLE (1U << 23)
#define TX_LEVEL (1U << 8)
#define RX_LEVEL (1U << 9)
#define TX_UNDERFLOW (1U << 11)
#define RX_OVERFLOW (1U << 12)
#define TX_FINISH (1U << 13)
#define RX_FINISH (1U << 14)
#define STATUS_MASK 0x7f00U

static unsigned bk7258_spi_threshold(unsigned encoding)
{
    static const unsigned thresholds[] = { 1, 16, 32, 48 };

    return thresholds[encoding & 3];
}

static void bk7258_spi_irq(BK7258SPIState *s)
{
    uint32_t enabled = ((s->control & 0xc0) << 2) |
                       ((s->control & 0x30) << 7) |
                       ((s->config & 0xc) << 11);

    if ((s->global & 1) && (s->control & ENABLE) && (s->config & 1) &&
        s->tx_count < bk7258_spi_threshold(s->control)) {
        s->status |= TX_LEVEL;
    }
    if (s->rx_count >= bk7258_spi_threshold(s->control >> 2)) {
        s->status |= RX_LEVEL;
    }
    qemu_set_irq(s->irq, (s->global & 1) && (s->status & enabled));
}

/* Resolve the externally supplied CS0 endpoint without choosing its type. */
static bool bk7258_spi_cs(BK7258SPIState *s, bool high)
{
    DeviceState *dev = ssi_get_cs(s->bus, 0);

    if (dev && SSI_PERIPHERAL_GET_CLASS(dev)->cs_polarity == SSI_CS_LOW) {
        if (!object_property_find(OBJECT(dev), SSI_GPIO_CS "[0]")) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-spi: endpoint lacks standard CS input\n");
            return false;
        }
        qdev_connect_gpio_out_named(DEVICE(s), "cs", 0,
                                   qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0));
    } else {
        qdev_connect_gpio_out_named(DEVICE(s), "cs", 0, NULL);
    }
    qemu_set_irq(s->cs, high);
    return true;
}

static void bk7258_spi_arm(BK7258SPIState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t delay;

    timer_del(s->timer);
    if (!s->pending || !s->hz) {
        return;
    }
    delay = DIV_ROUND_UP(s->cycles * NANOSECONDS_PER_SECOND, s->hz);
    if (delay <= INT64_MAX - now) {
        s->deadline = now + delay;
        timer_mod(s->timer, s->deadline);
    }
}

static void bk7258_spi_pause(BK7258SPIState *s)
{
    if (s->pending && s->hz && timer_pending(s->timer)) {
        int64_t left = MAX(s->deadline -
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 0);

        s->cycles = DIV_ROUND_UP((uint64_t)left * s->hz,
                                 NANOSECONDS_PER_SECOND);
    }
}

static void bk7258_spi_clock_update(BK7258SPIState *s)
{
    s->hz = (s->global & 1) && (s->control & ENABLE) ?
            clock_get_hz(s->clk) : 0;
    bk7258_spi_arm(s);
}

static void bk7258_spi_clock(void *opaque, ClockEvent event)
{
    BK7258SPIState *s = opaque;

    if (event == ClockPreUpdate) {
        bk7258_spi_pause(s);
    } else {
        bk7258_spi_clock_update(s);
    }
}

static void bk7258_spi_next(BK7258SPIState *s)
{
    unsigned divider = (s->control >> 8) & 0xff;
    unsigned interval = (s->control >> 24) & 0x3f;

    if (!s->active || s->pending || !s->tx_left) {
        return;
    }
    if (!s->tx_count) {
        if (s->started) {
            s->status |= TX_UNDERFLOW;
        }
        bk7258_spi_irq(s);
        return;
    }
    s->cycles = (8 + interval) * 2 * divider;
    s->pending = true;
    bk7258_spi_arm(s);
}

static void bk7258_spi_complete(void *opaque)
{
    BK7258SPIState *s = opaque;
    uint8_t received;

    assert(s->active && s->pending && s->tx_count && s->tx_left);
    s->pending = false;
    s->cycles = 0;
    received = ssi_transfer(s->bus, s->tx[s->tx_head]);
    s->tx_head = (s->tx_head + 1) % sizeof(s->tx);
    s->tx_count--;
    s->tx_left--;
    s->started = true;
    if (s->rx_left) {
        if (s->rx_count == sizeof(s->rx)) {
            s->status |= RX_OVERFLOW; /* Functional policy: drop newest. */
        } else {
            s->rx[(s->rx_head + s->rx_count) % sizeof(s->rx)] = received;
            s->rx_count++;
        }
        if (!--s->rx_left) {
            s->status |= RX_FINISH;
        }
    }
    if (!s->tx_left) {
        s->status |= TX_FINISH;
        s->active = false;
        bk7258_spi_cs(s, true);
    } else {
        bk7258_spi_next(s);
    }
    bk7258_spi_irq(s);
}

static void bk7258_spi_abort(BK7258SPIState *s)
{
    timer_del(s->timer);
    s->active = s->pending = s->started = false;
    s->cycles = 0;
    s->tx_left = s->rx_left = 0;
    if (s->bus) {
        bk7258_spi_cs(s, true);
    }
}

static MemTxResult bk7258_spi_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    BK7258SPIState *s = opaque;

    switch (offset) {
    case 0x08:
        *value = s->global;
        break;
    case 0x10:
        *value = s->control;
        break;
    case 0x14:
        *value = s->config;
        break;
    case 0x18:
        *value = s->status |
                 ((s->global & 1) && (s->control & ENABLE) &&
                  s->tx_count < sizeof(s->tx) ? 2 : 0) |
                 (s->rx_count ? 4 : 0);
        break;
    case 0x1c:
        if (!s->rx_count) {
            goto unsupported;
        }
        *value = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % sizeof(s->rx);
        s->rx_count--;
        bk7258_spi_irq(s);
        break;
    default:
        goto unsupported;
    }
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-spi: unsupported read at 0x%"
                  HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static MemTxResult bk7258_spi_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    BK7258SPIState *s = opaque;
    unsigned tx_len, rx_len;
    uint32_t old;

    switch (offset) {
    case 0x08:
        if (value & ~3U) {
            goto unsupported;
        }
        old = s->global;
        s->global = value;
        if (!(value & 1)) {
            bk7258_spi_abort(s);
            s->control = s->config = s->status = 0;
            s->tx_head = s->tx_count = s->rx_head = s->rx_count = 0;
        }
        if ((old ^ value) & 1 || !(value & 1)) {
            bk7258_spi_clock_update(s);
        }
        break;
    case 0x10:
        /* The initial slice is master, 8-bit, MSB-first and four-wire. */
        if ((value & ENABLE) &&
            (!(value & MASTER) || (value & (7U << 17)) ||
             !((value >> 8) & 0xff))) {
            goto unsupported;
        }
        if (s->active && (value & ENABLE) &&
            ((value ^ s->control) & ~0x100ffU)) {
            goto unsupported;
        }
        old = s->control;
        s->control = value;
        if (!(value & ENABLE)) {
            bk7258_spi_abort(s);
        }
        if ((old ^ value) & ENABLE) {
            bk7258_spi_clock_update(s);
        }
        break;
    case 0x14:
        if (value & 0xf0) {
            goto unsupported;
        }
        tx_len = (value >> 8) & 0xfff;
        rx_len = value >> 20;
        if (!(value & 3)) {
            bk7258_spi_abort(s);
        } else if (s->active) {
            /* Only interrupt-mask changes are supported during a frame. */
            if ((s->config ^ value) & ~0xcU) {
                goto unsupported;
            }
        } else if (!(s->config & 3)) {
            if (!(s->global & 1) || !(s->control & ENABLE) ||
                !(value & 1) || !tx_len ||
                ((value & 2) && (!rx_len || rx_len != tx_len))) {
                goto unsupported;
            }
            if (!bk7258_spi_cs(s, false)) {
                goto unsupported;
            }
            s->tx_left = tx_len;
            s->rx_left = value & 2 ? rx_len : 0;
            s->active = true;
            s->started = false;
        }
        s->config = value;
        bk7258_spi_next(s);
        break;
    case 0x18:
        if (value & ~0x37f06U) {
            goto unsupported;
        }
        s->status &= ~(value & STATUS_MASK);
        if (value & (1U << 16)) {
            /* Cancel the pending word rather than send stale FIFO contents. */
            timer_del(s->timer);
            s->pending = false;
            s->cycles = 0;
            s->tx_head = s->tx_count = 0;
        }
        if (value & (1U << 17)) {
            s->rx_head = s->rx_count = 0;
        }
        bk7258_spi_next(s);
        break;
    case 0x1c:
        if (value > 0xff || s->tx_count == sizeof(s->tx)) {
            goto unsupported;
        }
        s->tx[(s->tx_head + s->tx_count) % sizeof(s->tx)] = value;
        s->tx_count++;
        bk7258_spi_next(s);
        break;
    default:
        goto unsupported;
    }
    bk7258_spi_irq(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-spi: unsupported write 0x%" PRIx64
                  " at 0x%" HWADDR_PRIx "\n", value, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps bk7258_spi_ops = {
    .read_with_attrs = bk7258_spi_read,
    .write_with_attrs = bk7258_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_spi_reset(DeviceState *dev)
{
    BK7258SPIState *s = BK7258_SPI(dev);

    bk7258_spi_abort(s);
    s->global = s->control = s->config = s->status = 0;
    s->tx_head = s->tx_count = s->rx_head = s->rx_count = 0;
    bk7258_spi_clock_update(s);
    bk7258_spi_irq(s);
}

static void bk7258_spi_realize(DeviceState *dev, Error **errp)
{
    BK7258SPIState *s = BK7258_SPI(dev);

    s->bus = (SSIBus *)qbus_new(TYPE_BK7258_SPI_BUS, dev, s->bus_name);
}

static void bk7258_spi_init(Object *obj)
{
    BK7258SPIState *s = BK7258_SPI(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_spi_complete, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "pclk", bk7258_spi_clock, s,
                                ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_spi_ops, s,
                          "bk7258-spi", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cs, "cs", 1);
}

static void bk7258_spi_finalize(Object *obj)
{
    timer_free(BK7258_SPI(obj)->timer);
}

static const Property bk7258_spi_properties[] = {
    DEFINE_PROP_STRING("bus-name", BK7258SPIState, bus_name),
};

static void bk7258_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_spi_realize;
    device_class_set_legacy_reset(dc, bk7258_spi_reset);
    device_class_set_props(dc, bk7258_spi_properties);
    dc->user_creatable = false;
}

static bool bk7258_spi_bus_check(BusState *bus, DeviceState *dev, Error **errp)
{
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_GET_CLASS(dev);

    if (SSI_PERIPHERAL(dev)->cs_index || !QTAILQ_EMPTY(&bus->children) ||
        ssc->cs_polarity == SSI_CS_HIGH) {
        error_setg(errp, "BK7258 SPI supports one CS0 low-active or CS-less "
                   "endpoint per bus");
        return false;
    }
    return true;
}

static void bk7258_spi_bus_class_init(ObjectClass *klass, const void *data)
{
    BusClass *bc = BUS_CLASS(klass);

    bc->max_dev = 1;
    bc->check_address = bk7258_spi_bus_check;
}

static const TypeInfo bk7258_spi_types[] = {
    {
        .name = TYPE_BK7258_SPI_BUS,
        .parent = "SSI",
        .class_init = bk7258_spi_bus_class_init,
    }, {
        .name = TYPE_BK7258_SPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BK7258SPIState),
        .instance_init = bk7258_spi_init,
        .instance_finalize = bk7258_spi_finalize,
        .class_init = bk7258_spi_class_init,
    },
};

DEFINE_TYPES(bk7258_spi_types)
