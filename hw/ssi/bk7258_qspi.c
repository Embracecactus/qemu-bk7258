/*
 * BK7258 bounded single-line indirect PIO via native SSI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Contract: docs/system/arm/bk7258-qspi-v5.rst (extends V4).
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/bk7258_clock.h"
#include "hw/ssi/bk7258_qspi.h"
#include "migration/vmstate.h"

#define TYPE_BK7258_QSPI_BUS "bk7258-qspi-bus"

static void qspi_cs(BK7258QSPIState *s, bool high)
{
    DeviceState *dev = ssi_get_cs(s->bus, 0);

    if (dev) {
        qdev_connect_gpio_out_named(DEVICE(s), "cs", 0,
                                   qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0));
    }
    qemu_set_irq(s->cs, high);
}

static void qspi_arm(BK7258QSPIState *s)
{
    timer_del(s->timer);
    if (s->active && s->hz) {
        s->deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     DIV_ROUND_UP(s->cycles * NANOSECONDS_PER_SECOND, s->hz);
        timer_mod(s->timer, s->deadline);
    }
}

/* Input is post-SYS/pre-local clock, not final SCK. Finite tuples only. */
static void qspi_update_rate(BK7258QSPIState *s)
{
    unsigned input = clock_get_hz(s->clk);
    unsigned local = (s->config >> 8) & 255;

    s->hz = input == 48000000 && local == 0 ? 48000000 :
            input == 96000000 && local == 2 ? 24000000 : 0;
    qspi_arm(s);
}

static void qspi_clock(void *opaque, ClockEvent event)
{
    BK7258QSPIState *s = opaque;

    if (event == ClockPreUpdate) {
        if (s->active && s->hz && timer_pending(s->timer)) {
            s->cycles = bk7258_remaining_cycles(s->cycles, s->hz,
                          s->deadline, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
    } else {
        qspi_update_rate(s);
    }
}

static void qspi_cancel(BK7258QSPIState *s)
{
    timer_del(s->timer);
    if (s->bus) {
        qspi_cs(s, true);
    }
    s->active = s->selected = false;
    s->cmd[0][3] &= ~1U;
    s->cmd[1][3] &= ~1U;
    s->cycles = 0;
}

static void qspi_byte(void *opaque)
{
    BK7258QSPIState *s = opaque;
    uint8_t tx, rx;

    assert(s->active && s->hz);
    if (!s->selected) {
        qspi_cs(s, false);
        s->selected = true;
    }
    tx = s->pos < s->command_len ? s->command[s->pos] :
         (s->bank ? 0xff : s->data[s->pos - s->command_len]);
    rx = ssi_transfer(s->bus, tx);
    if (s->bank && s->pos >= s->command_len) {
        s->data[s->pos - s->command_len] = rx;
    }
    if (++s->pos == s->command_len + s->length) {
        qspi_cancel(s);
        /* SDK polling completion only. Not RX/TX/QSPI done or IRQ. */
        s->done = 4;
    } else {
        s->cycles = 8;
        qspi_arm(s);
    }
}

static MemTxResult qspi_read(void *opaque, hwaddr off, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    BK7258QSPIState *s = opaque;

    if (off >= 0x40 && off < 0x60) {
        *value = s->cmd[(off - 0x40) / 16][(off / 4) & 3];
    } else if (off == 0x60) {
        *value = s->config;
    } else if (off == 0x6c) {
        *value = 0;
    } else if (off == 0x70) {
        *value = s->done;
    } else if (off >= 0x100 && off < 0x200) {
        /* SDK 64-word PIO access; software-visible, not silicon capacity. */
        *value = ldl_le_p(s->data + off - 0x100);
    } else {
        qemu_log_mask(LOG_UNIMP, "bk7258-qspi: unsupported read 0x%"
                      HWADDR_PRIx "\n", off);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult qspi_write(void *opaque, hwaddr off, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    BK7258QSPIState *s = opaque;
    unsigned bank, reg, n, len;
    uint32_t cfg;

    if (off == 0x60) {
        /* Verified local divider 0 or 2, mode 0, MSB, optional IO2/3. */
        if (value & ~0x209U || (s->active && (value & 1))) {
            goto unsupported;
        }
        if (!(value & 1)) {
            qspi_cancel(s);
        }
        s->config = value;
        qspi_update_rate(s);
    } else if (off == 0x6c) {
        if (value & ~4U) {
            goto unsupported;
        }
        s->done &= ~value;
    } else if (off >= 0x100 && off < 0x200) {
        if (s->active) {
            goto unsupported;
        }
        stl_le_p(s->data + off - 0x100, value);
    } else if (off >= 0x40 && off < 0x60) {
        if (s->active) {
            goto unsupported;
        }
        bank = (off - 0x40) / 16;
        reg = (off / 4) & 3;
        if (reg == 3 && (value & ~0xffdU)) {
            goto unsupported; /* no dummy, multi-line, or reserved bits */
        }
        if (reg != 3 || !(value & 1)) {
            s->cmd[bank][reg] = value;
            return MEMTX_OK;
        }
        cfg = s->cmd[bank][2];
        /* Proven LL command lengths: 1..4 bytes, all single-line. */
        for (n = 1; n <= 4; n++) {
            if (cfg == (3U << (2 * n))) {
                break;
            }
        }
        len = (value >> 2) & 1023;
        if (n > 4 || len > sizeof(s->data) || (bank && !len) ||
            !(s->config & 1) || s->done) {
            goto unsupported;
        }
        s->cmd[bank][3] = value;
        for (unsigned i = 0; i < n; i++) {
            s->command[i] = s->cmd[bank][1] >> (8 * i);
        }
        s->bank = bank;
        s->command_len = n;
        s->length = len;
        s->pos = 0;
        s->cycles = 8;
        s->active = true;
        qspi_arm(s);
    } else {
        goto unsupported;
    }
    return MEMTX_OK;
unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-qspi: unsupported write 0x%"
                  HWADDR_PRIx " value 0x%08" PRIx64 "\n", off, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps qspi_ops = {
    .read_with_attrs = qspi_read,
    .write_with_attrs = qspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void qspi_reset(DeviceState *dev)
{
    BK7258QSPIState *s = BK7258_QSPI(dev);
    qspi_cancel(s);
    memset(s->cmd, 0, sizeof(s->cmd));
    memset(s->data, 0, sizeof(s->data));
    s->config = s->done = 0;
    qspi_update_rate(s);
}

static void qspi_realize(DeviceState *dev, Error **errp)
{
    BK7258QSPIState *s = BK7258_QSPI(dev);
    s->bus = (SSIBus *)qbus_new(TYPE_BK7258_QSPI_BUS, dev, s->bus_name);
}
static void qspi_init(Object *obj)
{
    BK7258QSPIState *s = BK7258_QSPI(obj);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, qspi_byte, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "sclk", qspi_clock, s,
                               ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &qspi_ops, s, "bk7258-qspi", 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cs, "cs", 1);
}
static void qspi_finalize(Object *obj)
{
    timer_free(BK7258_QSPI(obj)->timer);
}
static const Property qspi_properties[] = {
    DEFINE_PROP_STRING("bus-name", BK7258QSPIState, bus_name),
};
static const VMStateDescription qspi_vmstate = {
    .name = TYPE_BK7258_QSPI,
    .unmigratable = 1,
};
static void qspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = qspi_realize;
    dc->vmsd = &qspi_vmstate;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, qspi_reset);
    device_class_set_props(dc, qspi_properties);
}
static bool qspi_bus_check(BusState *bus, DeviceState *dev, Error **errp)
{
    if (SSI_PERIPHERAL(dev)->cs_index || !QTAILQ_EMPTY(&bus->children) ||
        SSI_PERIPHERAL_GET_CLASS(dev)->cs_polarity != SSI_CS_LOW) {
        error_setg(errp, "BK7258 QSPI requires one native low-active "
                   "CS0 endpoint");
        return false;
    }
    return true;
}
static void qspi_bus_class_init(ObjectClass *klass, const void *data)
{
    BUS_CLASS(klass)->max_dev = 1;
    BUS_CLASS(klass)->check_address = qspi_bus_check;
}
static const TypeInfo qspi_types[] = {
    { .name = TYPE_BK7258_QSPI_BUS, .parent = "SSI",
      .class_init = qspi_bus_class_init },
    { .name = TYPE_BK7258_QSPI, .parent = TYPE_SYS_BUS_DEVICE,
      .instance_size = sizeof(BK7258QSPIState), .instance_init = qspi_init,
      .instance_finalize = qspi_finalize, .class_init = qspi_class_init },
};
DEFINE_TYPES(qspi_types)
