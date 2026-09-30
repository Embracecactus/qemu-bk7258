/*
 * BK7258 keyed AON/APB watchdogs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register facts: pinned Beken SDK wdt_ll.h, aon_wdt_ll.h and wdt_struct.h.
 * The AON millisecond tick follows the driver's timeout_ms API contract;
 * oscillator tolerance and undocumented key-error behavior are not modeled.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/watchdog/bk7258_wdt.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/watchdog.h"

static void bk7258_wdt_schedule(BK7258WDTState *s)
{
    timer_del(s->timer);
    if (s->armed && s->hz) {
        uint64_t delay = DIV_ROUND_UP(
            (uint64_t)s->remaining * NANOSECONDS_PER_SECOND, s->hz);
        s->deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay;
        timer_mod(s->timer, s->deadline);
    }
}

static void bk7258_wdt_expire(void *opaque)
{
    BK7258WDTState *s = opaque;

    s->armed = false;
    s->remaining = 0;
    if (s->aon) {
        watchdog_perform_action();
    } else {
        /* The APB watchdog enters CP's NMI handler; firmware owns reboot. */
        qemu_irq_pulse(s->nmi);
    }
}

static void bk7258_wdt_clock(void *opaque, ClockEvent event)
{
    BK7258WDTState *s = opaque;

    if (event == ClockPreUpdate) {
        if (s->armed && s->hz && timer_pending(s->timer)) {
            int64_t delta = MAX(s->deadline -
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 0);
            s->remaining = DIV_ROUND_UP((uint64_t)delta * s->hz,
                                       NANOSECONDS_PER_SECOND);
        }
    } else {
        s->hz = clock_get_hz(s->clk);
        bk7258_wdt_schedule(s);
    }
}

static void bk7258_wdt_reset(DeviceState *dev)
{
    BK7258WDTState *s = BK7258_WDT(dev);

    timer_del(s->timer);
    s->global_ctrl = 0;
    s->period = s->pending_period = s->remaining = 0;
    s->keyed = s->armed = false;
    qemu_irq_lower(s->nmi);
}

static uint64_t bk7258_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    BK7258WDTState *s = opaque;

    if (addr == (s->aon ? 0 : 0x10)) {
        return s->period;
    }
    if (!s->aon && addr == 8) {
        return s->global_ctrl;
    }
    qemu_log_mask(LOG_UNIMP, "bk7258-wdt: read offset 0x%" HWADDR_PRIx
                  " is not implemented\n", addr);
    return 0;
}

static void bk7258_wdt_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    BK7258WDTState *s = opaque;
    uint32_t period = value & 0xffff;
    unsigned key = (value >> 16) & 0xff;

    if (!s->aon && addr == 8) {
        if (!(value & 1)) {
            bk7258_wdt_reset(DEVICE(s));
        }
        s->global_ctrl = value & 3;
        return;
    }
    if (addr != (s->aon ? 0 : 0x10)) {
        qemu_log_mask(LOG_UNIMP, "bk7258-wdt: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return;
    }
    if (!s->aon && !(s->global_ctrl & 1) && period) {
        s->keyed = false;
        return;
    }
    if (key == 0x5a) {
        s->pending_period = period;
        s->keyed = true;
    } else {
        if (key == 0xa5 && s->keyed && s->pending_period == period) {
            s->period = s->remaining = period;
            s->armed = period != 0;
            bk7258_wdt_schedule(s);
        }
        s->keyed = false;
    }
}

static const MemoryRegionOps bk7258_wdt_ops = {
    .read = bk7258_wdt_read,
    .write = bk7258_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_wdt_realize(DeviceState *dev, Error **errp)
{
    BK7258WDTState *s = BK7258_WDT(dev);

    s->hz = clock_get_hz(s->clk);
    memory_region_init_io(&s->iomem, OBJECT(dev), &bk7258_wdt_ops, s,
                          "bk7258-wdt", s->aon ? 4 : 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void bk7258_wdt_init(Object *obj)
{
    BK7258WDTState *s = BK7258_WDT(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->nmi);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_wdt_expire, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", bk7258_wdt_clock, s,
                               ClockPreUpdate | ClockUpdate);
}

static void bk7258_wdt_finalize(Object *obj)
{
    timer_free(BK7258_WDT(obj)->timer);
}

static const Property bk7258_wdt_properties[] = {
    DEFINE_PROP_BOOL("aon", BK7258WDTState, aon, false),
};

static void bk7258_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_wdt_realize;
    device_class_set_legacy_reset(dc, bk7258_wdt_reset);
    device_class_set_props(dc, bk7258_wdt_properties);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_wdt_info = {
    .name = TYPE_BK7258_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258WDTState),
    .instance_init = bk7258_wdt_init,
    .instance_finalize = bk7258_wdt_finalize,
    .class_init = bk7258_wdt_class_init,
};

static void bk7258_wdt_register_types(void)
{
    type_register_static(&bk7258_wdt_info);
}

type_init(bk7258_wdt_register_types)
