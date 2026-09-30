/*
 * BK7258 bounded 7-bit master, using the native QEMU I2C bus.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register/handshake facts: pinned SDK I2C structure, LL, HAL and driver.
 * Model policies and absent features: docs/system/arm/bk7258.rst.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/bk7258_clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/bk7258_i2c.h"

#define SM_INT 1U
#define ACK (1U << 8)
#define STOP (1U << 9)
#define START (1U << 10)
#define MODE (3U << 6)
#define CFG_ENABLE (1U << 31)
#define STATUS_WRITABLE (SM_INT | ACK | STOP | START | MODE)

enum {
    OP_NONE,
    OP_ADDRESS,
    OP_SEND,
    OP_RECEIVE,
    OP_STOP,
};

static void bk7258_i2c_irq(BK7258I2CState *s)
{
    qemu_set_irq(s->irq, (s->global & 1) && (s->config & CFG_ENABLE) &&
                 (s->status & SM_INT));
}

static void bk7258_i2c_arm(BK7258I2CState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t delay;

    timer_del(s->timer);
    if (!s->operation || !s->hz) {
        return;
    }
    delay = DIV_ROUND_UP(s->cycles * NANOSECONDS_PER_SECOND, s->hz);
    if (delay <= INT64_MAX - now) {
        s->deadline = now + delay;
        timer_mod(s->timer, s->deadline);
    }
}

static void bk7258_i2c_pause(BK7258I2CState *s)
{
    if (s->operation && s->hz && timer_pending(s->timer)) {
        s->cycles = bk7258_remaining_cycles(
            s->cycles, s->hz, s->deadline,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void bk7258_i2c_clock_update(BK7258I2CState *s)
{
    /* Only the HAL's XTAL selection (3) has a known source here. */
    s->hz = (s->global & 1) && (s->config & CFG_ENABLE) &&
            ((s->config >> 26) & 3) == 3 ? clock_get_hz(s->clk) : 0;
    bk7258_i2c_arm(s);
}

static void bk7258_i2c_clock(void *opaque, ClockEvent event)
{
    BK7258I2CState *s = opaque;

    if (event == ClockPreUpdate) {
        bk7258_i2c_pause(s);
    } else {
        bk7258_i2c_clock_update(s);
    }
}

static void bk7258_i2c_begin(BK7258I2CState *s, unsigned operation)
{
    unsigned scl_cycles = 3 * (((s->config >> 6) & 0x3ff) + 1) + 6;

    s->operation = operation;
    /* Nine SCL periods per address/data byte; one for functional STOP. */
    s->cycles = scl_cycles * (operation == OP_STOP ? 1 : 9);
    bk7258_i2c_arm(s);
}

static uint8_t bk7258_i2c_pop_tx(BK7258I2CState *s)
{
    uint8_t data = s->tx[s->tx_head];

    assert(s->tx_count);
    s->tx_head = (s->tx_head + 1) % sizeof(s->tx);
    s->tx_count--;
    return data;
}

static unsigned bk7258_i2c_rx_threshold(BK7258I2CState *s)
{
    static const unsigned levels[] = { 12, 8, 4, 1 };

    return levels[(s->status >> 6) & 3];
}

static void bk7258_i2c_complete(void *opaque)
{
    BK7258I2CState *s = opaque;
    unsigned operation = s->operation;
    bool ack;

    s->operation = OP_NONE;
    s->cycles = 0;
    switch (operation) {
    case OP_ADDRESS: {
        uint8_t byte;

        if (!s->bus_owned && i2c_bus_busy(s->bus)) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-i2c: external-master contention unsupported\n");
            /* No fabricated ACK/arbitration IRQ or automatic retry. */
            s->status &= ~ACK;
            break;
        }
        byte = bk7258_i2c_pop_tx(s);
        s->address = byte >> 1;
        s->receiving = byte & 1;
        s->read_nacked = false;
        s->selected = !i2c_start_transfer(s->bus, s->address, s->receiving);
        s->bus_owned = i2c_bus_busy(s->bus);
        s->status = (s->status & ~ACK) | SM_INT | START |
                    (s->selected ? ACK : 0);
        break;
    }
    case OP_SEND:
        ack = !i2c_send(s->bus, bk7258_i2c_pop_tx(s));
        s->status = (s->status & ~ACK) | (ack ? ACK : 0);
        /* Functional TX service thresholds: 0, 4, 8, or 12 bytes remain. */
        if (!ack || s->tx_count <= ((s->status >> 6) & 3) * 4) {
            s->status |= SM_INT;
        } else {
            bk7258_i2c_begin(s, OP_SEND);
        }
        break;
    case OP_RECEIVE:
        assert(s->rx_count < sizeof(s->rx));
        s->rx[(s->rx_head + s->rx_count) % sizeof(s->rx)] = i2c_recv(s->bus);
        s->rx_count++;
        if (s->rx_count >= bk7258_i2c_rx_threshold(s)) {
            s->status |= SM_INT;
        } else {
            bk7258_i2c_begin(s, OP_RECEIVE);
        }
        break;
    case OP_STOP:
        if (s->bus_owned) {
            i2c_end_transfer(s->bus);
        }
        s->bus_owned = false;
        s->busy = s->selected = s->receiving = s->read_nacked = false;
        s->status &= ~(STOP | START | SM_INT);
        break;
    default:
        g_assert_not_reached();
    }
    bk7258_i2c_irq(s);
}

static void bk7258_i2c_abort(BK7258I2CState *s)
{
    timer_del(s->timer);
    if (s->bus_owned) {
        i2c_end_transfer(s->bus);
    }
    s->bus_owned = false;
    s->busy = s->selected = s->receiving = s->read_nacked = false;
    s->operation = OP_NONE;
    s->cycles = 0;
    s->tx_head = s->tx_count = s->rx_head = s->rx_count = 0;
    s->status = 0;
}

static MemTxResult bk7258_i2c_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    BK7258I2CState *s = opaque;

    switch (offset) {
    case 0x08:
        *value = s->global;
        break;
    case 0x10:
        *value = s->config;
        break;
    case 0x14:
        *value = s->status | (!s->rx_count ? 1U << 4 : 0) |
                 (s->tx_count == sizeof(s->tx) ? 1U << 5 : 0) |
                 (s->busy ? (3U << 14) | (!s->receiving ? 1U << 13 : 0) : 0);
        break;
    case 0x18:
        if (!s->rx_count) {
            goto unsupported;
        }
        *value = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % sizeof(s->rx);
        s->rx_count--;
        break;
    case 0x1c:
        *value = s->extra;
        break;
    default:
        goto unsupported;
    }
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-i2c: unsupported read at 0x%"
                  HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static bool bk7258_i2c_status_write(BK7258I2CState *s, uint32_t value)
{
    uint32_t old = s->status;
    bool service = (old & SM_INT) && !(value & SM_INT);
    bool start = (value & START) && !(value & SM_INT) &&
                 (service || !(old & START));

    if ((value & (START | STOP)) == (START | STOP) ||
        ((value & STOP) && !s->busy)) {
        return false;
    }
    if (s->operation && ((old ^ value) & (ACK | START | STOP))) {
        return false;
    }
    if (!s->operation && start) {
        unsigned address;

        if (!s->tx_count || !(s->global & 1) || !(s->config & CFG_ENABLE)) {
            return false;
        }
        address = s->tx[s->tx_head] >> 1;
        /* Core I2C API does not rescan a selected bus on repeated START. */
        if (!address || address >= 0x78 ||
            (i2c_bus_busy(s->bus) && !s->bus_owned) ||
            (s->bus_owned && address != s->address)) {
            return false;
        }
    }
    /* SM_INT is W0C. A write-one cannot manufacture an interrupt. */
    s->status = (old & value & SM_INT) | (value & (STATUS_WRITABLE & ~SM_INT));
    if (old & value & SM_INT) {
        /* Preserve the actual response until software services the event. */
        s->status = (s->status & ~ACK) | (old & ACK);
    }
    if (s->operation) {
        return true; /* Threshold updates may accompany the initial START. */
    }
    if (start) {
        if (service && s->bus_owned && s->receiving &&
            !(old & START) && !(value & ACK) &&
            !s->read_nacked) {
            i2c_nack(s->bus);
        }
        s->busy = true;
        bk7258_i2c_begin(s, OP_ADDRESS);
    } else if (service && s->busy) {
        if (s->bus_owned && s->receiving &&
            !(old & START) && !(value & ACK)) {
            i2c_nack(s->bus);
            s->read_nacked = true;
        }
        if (value & STOP) {
            bk7258_i2c_begin(s, OP_STOP);
        } else if (!s->selected) {
            /* A software ACK bit cannot turn an absent slave into success. */
            s->status = (s->status & ~ACK) | SM_INT;
        } else if (s->receiving) {
            if (!s->read_nacked) {
                if (s->rx_count >= bk7258_i2c_rx_threshold(s)) {
                    s->status |= SM_INT;
                } else {
                    bk7258_i2c_begin(s, OP_RECEIVE);
                }
            }
        } else if (s->tx_count) {
            bk7258_i2c_begin(s, OP_SEND);
        } else {
            /* Empty TX still requires service; no new slave ACK occurred. */
            s->status = (s->status & ~ACK) | (old & ACK) | SM_INT;
        }
    }
    return true;
}

static MemTxResult bk7258_i2c_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    BK7258I2CState *s = opaque;

    switch (offset) {
    case 0x08:
        if (value & ~3U) {
            goto unsupported;
        }
        bk7258_i2c_pause(s);
        s->global = value;
        if (!(value & 1)) {
            bk7258_i2c_abort(s);
            s->config = s->extra = 0;
        }
        bk7258_i2c_clock_update(s);
        break;
    case 0x10:
        if (s->operation && (value & CFG_ENABLE) &&
            ((s->config ^ value) & 0x0c00ffc0)) {
            goto unsupported;
        }
        bk7258_i2c_pause(s);
        s->config = value;
        if (!(value & CFG_ENABLE)) {
            bk7258_i2c_abort(s);
        } else if (((value >> 26) & 3) != 3) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-i2c: selected clock source is not modeled\n");
        }
        bk7258_i2c_clock_update(s);
        break;
    case 0x14:
        if (!bk7258_i2c_status_write(s, value)) {
            goto unsupported;
        }
        break;
    case 0x18:
        if (value > 0xff || s->tx_count == sizeof(s->tx)) {
            goto unsupported;
        }
        s->tx[(s->tx_head + s->tx_count) % sizeof(s->tx)] = value;
        s->tx_count++;
        break;
    case 0x1c:
        if (value & ~3U) { /* Byte-interval timing is not established. */
            goto unsupported;
        }
        s->extra = value;
        break;
    default:
        goto unsupported;
    }
    bk7258_i2c_irq(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-i2c: unsupported write 0x%" PRIx64
                  " at 0x%" HWADDR_PRIx "\n", value, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps bk7258_i2c_ops = {
    .read_with_attrs = bk7258_i2c_read,
    .write_with_attrs = bk7258_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_i2c_reset(DeviceState *dev)
{
    BK7258I2CState *s = BK7258_I2C(dev);

    bk7258_i2c_abort(s);
    s->global = s->config = s->extra = 0;
    bk7258_i2c_clock_update(s);
    bk7258_i2c_irq(s);
}

static void bk7258_i2c_realize(DeviceState *dev, Error **errp)
{
    BK7258I2CState *s = BK7258_I2C(dev);

    s->bus = i2c_init_bus(dev, s->bus_name);
}

static void bk7258_i2c_init(Object *obj)
{
    BK7258I2CState *s = BK7258_I2C(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_i2c_complete, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "pclk", bk7258_i2c_clock, s,
                                ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_i2c_ops, s,
                          "bk7258-i2c", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void bk7258_i2c_finalize(Object *obj)
{
    timer_free(BK7258_I2C(obj)->timer);
}

static const Property bk7258_i2c_properties[] = {
    DEFINE_PROP_STRING("bus-name", BK7258I2CState, bus_name),
};

static void bk7258_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_i2c_realize;
    device_class_set_legacy_reset(dc, bk7258_i2c_reset);
    device_class_set_props(dc, bk7258_i2c_properties);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_i2c_info = {
    .name = TYPE_BK7258_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258I2CState),
    .instance_init = bk7258_i2c_init,
    .instance_finalize = bk7258_i2c_finalize,
    .class_init = bk7258_i2c_class_init,
};

static void bk7258_i2c_register_types(void)
{
    type_register_static(&bk7258_i2c_info);
}

type_init(bk7258_i2c_register_types)
