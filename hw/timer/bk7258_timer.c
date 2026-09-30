/*
 * BK7258 three-channel timer group functional model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register facts: pinned SDK timer_{struct,ll}.h and timer driver.
 * Unmeasured edge/reset policies: docs/system/arm/bk7258.rst.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/int128.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/timer/bk7258_timer.h"

#define STATUS_MASK 0x380U
#define WRAP (UINT64_C(1) << 32)

static unsigned bk7258_timer_divisor(BK7258TimerState *s)
{
    return ((s->control >> 3) & 15) + 1;
}

static void bk7258_timer_irq(BK7258TimerState *s)
{
    qemu_set_irq(s->irq, !!(s->control & STATUS_MASK));
}

static uint64_t bk7258_timer_distance(BK7258TimerState *s, unsigned i)
{
    uint32_t distance = s->end[i] - s->counter[i];

    return distance ? distance : WRAP;
}

/* Coalesce elapsed periods: a latched IRQ must not cause a host timer storm. */
static void bk7258_timer_count(BK7258TimerState *s, Int128 cycles)
{
    unsigned divisor = bk7258_timer_divisor(s);
    Int128 scaled = int128_add(cycles, int128_make64(s->prescale));
    Int128 ticks = int128_divu(scaled, int128_make64(divisor));

    s->prescale = int128_get64(int128_remu(scaled, int128_make64(divisor)));
    for (unsigned i = 0; i < 3; i++) {
        uint64_t distance = bk7258_timer_distance(s, i);
        uint64_t period = s->end[i] ? s->end[i] : WRAP;

        if (!(s->control & (1U << i))) {
            continue;
        }
        if (int128_uge(ticks, int128_make64(distance))) {
            s->control |= 1U << (7 + i);
            s->counter[i] = int128_get64(int128_remu(
                int128_sub(ticks, int128_make64(distance)),
                int128_make64(period)));
        } else {
            s->counter[i] += int128_get64(ticks);
        }
    }
}

static void bk7258_timer_update(BK7258TimerState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t period = clock_get(s->clk);
    Int128 elapsed, cycles;

    if (!(s->global & 1) || !period) {
        s->last_ns = now;
        return;
    }
    elapsed = int128_add(int128_lshift(int128_make64(now - s->last_ns), 32),
                         int128_make64(s->phase));
    cycles = int128_divu(elapsed, int128_make64(period));
    s->phase = int128_get64(int128_remu(elapsed, int128_make64(period)));
    s->last_ns = now;
    if ((s->read_control & 1) && int128_nz(cycles)) {
        /* Functional CDC policy: capture after one live input clock edge. */
        bk7258_timer_count(s, int128_one());
        s->read_value = s->counter[(s->read_control >> 2) & 3];
        s->read_control &= ~1U;
        cycles = int128_sub(cycles, int128_one());
    }
    bk7258_timer_count(s, cycles);
    bk7258_timer_irq(s);
}

static void bk7258_timer_schedule(BK7258TimerState *s)
{
    uint64_t period = clock_get(s->clk), next = 0, lo, hi;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    Int128 delay;

    timer_del(s->timer);
    if (!(s->global & 1) || !period) {
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        if ((s->control & (1U << i)) &&
            !(s->control & (1U << (7 + i)))) {
            uint64_t distance = bk7258_timer_distance(s, i) *
                                bk7258_timer_divisor(s) - s->prescale;

            if (!next || distance < next) {
                next = distance;
            }
        }
    }
    if (s->read_control & 1) {
        next = 1;
    }
    if (!next) {
        return;
    }
    mulu64(&lo, &hi, next, period);
    delay = int128_sub(int128_make128(lo, hi), int128_make64(s->phase));
    if (int128_ult(int128_lshift(int128_make64(INT64_MAX - now), 32),
                   delay)) {
        return;
    }
    delay = int128_urshift(int128_add(delay, int128_make64(UINT32_MAX)), 32);
    timer_mod(s->timer, now + int128_get64(delay));
}

static void bk7258_timer_event(void *opaque)
{
    BK7258TimerState *s = opaque;

    bk7258_timer_update(s);
    bk7258_timer_schedule(s);
}

static void bk7258_timer_clock(void *opaque, ClockEvent event)
{
    BK7258TimerState *s = opaque;

    if (event == ClockPreUpdate) {
        bk7258_timer_update(s);
    } else {
        /* Retain whole input clocks; fractional source phase is unmodeled. */
        s->phase = 0;
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        bk7258_timer_schedule(s);
    }
}

static void bk7258_timer_clear(BK7258TimerState *s)
{
    s->control = s->read_control = s->read_value = 0;
    memset(s->end, 0, sizeof(s->end));
    memset(s->counter, 0, sizeof(s->counter));
    s->prescale = s->phase = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->timer);
}

static MemTxResult bk7258_timer_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    BK7258TimerState *s = opaque;

    bk7258_timer_update(s);
    switch (offset) {
    case 0x08:
        *value = s->global;
        break;
    case 0x10 ... 0x18:
        *value = s->end[(offset - 0x10) / 4];
        break;
    case 0x1c:
        *value = s->control;
        break;
    case 0x20:
        *value = s->read_control;
        break;
    case 0x24:
        *value = s->read_value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-timer: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bk7258_timer_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    BK7258TimerState *s = opaque;
    uint32_t old;

    bk7258_timer_update(s);
    switch (offset) {
    case 0x08:
        if (value & ~3U) {
            goto unsupported;
        }
        s->global = value;
        if (!(value & 1)) {
            bk7258_timer_clear(s);
        }
        break;
    case 0x10 ... 0x18:
        /* Do not clamp a running counter when its end value is lowered. */
        s->end[(offset - 0x10) / 4] = value;
        break;
    case 0x1c:
        if (value & ~0x3ffU) {
            goto unsupported;
        }
        old = s->control;
        s->control = (value & 0x7f) | (old & STATUS_MASK & ~value);
        if ((old ^ value) & 0x78) {
            s->prescale = 0;
        }
        for (unsigned i = 0; i < 3; i++) {
            if (!(value & (1U << i))) {
                s->counter[i] = 0;
            }
        }
        break;
    case 0x20:
        if ((value & ~0xdU) || ((value >> 2) & 3) == 3 ||
            (s->read_control & 1)) {
            goto unsupported;
        }
        s->read_control = value;
        break;
    case 0x24:
        if (value) {
            goto unsupported;
        }
        /* SDK clears this latch; a running counter is not a writable value. */
        s->read_value = 0;
        break;
    default:
        goto unsupported;
    }
    bk7258_timer_irq(s);
    bk7258_timer_schedule(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-timer: unsupported write 0x%" PRIx64
                  " at 0x%" HWADDR_PRIx "\n", value, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps bk7258_timer_ops = {
    .read_with_attrs = bk7258_timer_read,
    .write_with_attrs = bk7258_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_timer_reset_enter(Object *obj, ResetType type)
{
    BK7258TimerState *s = BK7258_TIMER(obj);

    s->global = 0;
    bk7258_timer_clear(s);
}

static void bk7258_timer_reset_hold(Object *obj, ResetType type)
{
    bk7258_timer_irq(BK7258_TIMER(obj));
}

static void bk7258_timer_init(Object *obj)
{
    BK7258TimerState *s = BK7258_TIMER(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_timer_event, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "pclk", bk7258_timer_clock, s,
                                ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_timer_ops, s,
                          "bk7258-timer", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void bk7258_timer_finalize(Object *obj)
{
    timer_free(BK7258_TIMER(obj)->timer);
}

static void bk7258_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = bk7258_timer_reset_enter;
    rc->phases.hold = bk7258_timer_reset_hold;
    dc->user_creatable = false;
}

static const TypeInfo bk7258_timer_info = {
    .name = TYPE_BK7258_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258TimerState),
    .instance_init = bk7258_timer_init,
    .instance_finalize = bk7258_timer_finalize,
    .class_init = bk7258_timer_class_init,
};

static void bk7258_timer_register_types(void)
{
    type_register_static(&bk7258_timer_info);
}

type_init(bk7258_timer_register_types)
