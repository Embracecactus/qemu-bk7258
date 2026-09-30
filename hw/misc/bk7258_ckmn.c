/*
 * BK7258 RC32K measurement against the digital 26 MHz reference.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Facts: pinned SDK ckmn_{struct,ll}.h and driver/pwr_clk/rosc_32k.c.
 * Clock correction/automatic switching and physical oscillator tuning are
 * deliberately absent. This is a downstream functional measurement model.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/bk7258_ckmn.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"

static void bk7258_ckmn_update_irq(BK7258CKMNState *s)
{
    qemu_set_irq(s->irq, (s->global & 1) &&
                 (((s->status & 1) && (s->control & 2)) ||
                  ((s->status & 2) && (s->correction & (1U << 8))) ||
                  ((s->status & 4) && (s->correction & (1U << 9)))));
}

static void bk7258_ckmn_cancel(BK7258CKMNState *s)
{
    timer_del(s->timer);
    s->active = false;
}

static void bk7258_ckmn_complete(void *opaque)
{
    BK7258CKMNState *s = opaque;

    if (!s->active) {
        return;
    }
    s->active = false;
    s->result = s->pending_result;
    s->status |= 1;
    bk7258_ckmn_update_irq(s);
}

static void bk7258_ckmn_start(BK7258CKMNState *s)
{
    uint64_t reference = clock_get_hz(s->reference);
    uint64_t measured = clock_get_hz(s->measured);
    uint64_t count;

    if (!s->window || !reference || !measured) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bk7258-ckmn: measurement requires a nonzero window "
                      "and two running clocks\n");
        return;
    }
    count = reference * s->window / measured;
    if (count > 0xfffff) {
        qemu_log_mask(LOG_UNIMP,
                      "bk7258-ckmn: measurement counter overflow is unsupported\n");
        return;
    }
    s->pending_result = count;
    s->active = true;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              DIV_ROUND_UP((uint64_t)s->window * NANOSECONDS_PER_SECOND,
                           measured));
}

static void bk7258_ckmn_clock_changed(void *opaque, ClockEvent event)
{
    BK7258CKMNState *s = opaque;

    /*
     * A stable-clock window is the supported contract. A clock transition
     * invalidates an in-flight measurement; software must disable/rearm it.
     * Do not invent a successful count across an undocumented clock-loss
     * synchronizer or oscillator startup interval.
     */
    if (s->active) {
        bk7258_ckmn_cancel(s);
    }
}

static void bk7258_ckmn_reset(DeviceState *dev)
{
    BK7258CKMNState *s = BK7258_CKMN(dev);

    bk7258_ckmn_cancel(s);
    s->global = s->window = s->control = s->result = 0;
    s->correction = s->status = s->pending_result = 0;
    bk7258_ckmn_update_irq(s);
}

static MemTxResult bk7258_ckmn_read(void *opaque, hwaddr addr, uint64_t *value,
                                   unsigned size, MemTxAttrs attrs)
{
    BK7258CKMNState *s = opaque;

    switch (addr) {
    case 0x08:
        *value = s->global;
        break;
    case 0x10:
        *value = s->window;
        break;
    case 0x14:
        *value = s->control;
        break;
    case 0x18:
        *value = s->result;
        break;
    case 0x1c:
        *value = s->correction;
        break;
    case 0x20:
        *value = s->status;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "bk7258-ckmn: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bk7258_ckmn_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size, MemTxAttrs attrs)
{
    BK7258CKMNState *s = opaque;
    uint32_t old;

    if (addr == 8) {
        if (!(value & 1)) {
            bk7258_ckmn_reset(DEVICE(s));
        }
        s->global = value & 3;
        bk7258_ckmn_update_irq(s);
        return MEMTX_OK;
    }
    if (!(s->global & 1)) {
        return MEMTX_OK;
    }
    switch (addr) {
    case 0x10:
        if (s->active) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bk7258-ckmn: cannot change an active window\n");
        } else {
            s->window = value & 0x3ff;
        }
        break;
    case 0x14:
        old = s->control;
        s->control = value & 3;
        if (!(value & 1)) {
            bk7258_ckmn_cancel(s);
        } else if (!(old & 1)) {
            bk7258_ckmn_start(s);
        }
        break;
    case 0x1c:
        s->correction = value & 0x3ff;
        if (value & 0xcc) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-ckmn: correction/detection and automatic "
                          "clock switching are not implemented\n");
        }
        break;
    case 0x20:
        s->status &= ~(value & 7);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "bk7258-ckmn: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    bk7258_ckmn_update_irq(s);
    return MEMTX_OK;
}

static const MemoryRegionOps bk7258_ckmn_ops = {
    .read_with_attrs = bk7258_ckmn_read,
    .write_with_attrs = bk7258_ckmn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_ckmn_init(Object *obj)
{
    BK7258CKMNState *s = BK7258_CKMN(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_ckmn_complete, s);
    s->reference = qdev_init_clock_in(DEVICE(obj), "reference",
        bk7258_ckmn_clock_changed, s, ClockUpdate);
    s->measured = qdev_init_clock_in(DEVICE(obj), "measured",
        bk7258_ckmn_clock_changed, s, ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_ckmn_ops, s,
                          "bk7258-ckmn", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void bk7258_ckmn_finalize(Object *obj)
{
    timer_free(BK7258_CKMN(obj)->timer);
}

static void bk7258_ckmn_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bk7258_ckmn_reset);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_ckmn_info = {
    .name = TYPE_BK7258_CKMN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258CKMNState),
    .instance_init = bk7258_ckmn_init,
    .instance_finalize = bk7258_ckmn_finalize,
    .class_init = bk7258_ckmn_class_init,
};

static void bk7258_ckmn_register_types(void)
{
    type_register_static(&bk7258_ckmn_info);
}

type_init(bk7258_ckmn_register_types)
