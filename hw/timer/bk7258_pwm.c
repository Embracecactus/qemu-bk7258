/*
 * BK7258 legacy PWM counter/compare slice, not waveform/pad emulation.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register facts: pinned SDK PWM V1PX LL/HAL and BK7258 datasheet.
 * Unmeasured edge policies and missing modes: docs/system/arm/bk7258.rst.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/int128.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/timer/bk7258_pwm.h"

#define WRAP (UINT64_C(1) << 32)
#define EVENTS 0xfffU
#define CONTROL_MASK (0x1ffU | (7U << 26))

static bool bk7258_pwm_enabled(BK7258PWMState *s, unsigned i)
{
    return s->control & (1U << (2 - i));
}

static unsigned bk7258_pwm_divisor(BK7258PWMState *s, unsigned i)
{
    return ((s->divider >> (8 * i)) & 255) + 1;
}

static void bk7258_pwm_irq(BK7258PWMState *s)
{
    qemu_set_irq(s->irq, (s->global & 1) && (s->status & s->mask));
}

static void bk7258_pwm_preload(BK7258PWMState *s, unsigned i)
{
    BK7258PWMCounter *c = &s->counter[i];

    c->arr_shadow = c->arr;
    memcpy(c->ccr_shadow, c->ccr, sizeof(c->ccr));
}

static uint64_t bk7258_pwm_to_update(BK7258PWMCounter *c)
{
    /* Lowering a live ARR below CNT does not clamp the existing count. */
    return (uint32_t)(c->arr_shadow - c->count) + UINT64_C(1);
}

static void bk7258_pwm_segment(BK7258PWMState *s, unsigned i, uint64_t ticks)
{
    BK7258PWMCounter *c = &s->counter[i];

    for (unsigned n = 0; n < 3; n++) {
        uint32_t value = c->ccr_shadow[n];
        uint32_t distance = value - c->count;

        /* Zero denotes an unused compare in this deliberately narrow slice. */
        if (value && (distance ? distance : WRAP) <= ticks) {
            s->status |= 1U << (3 * i + n);
        }
    }
    c->count += ticks;
}

/* Account for large virtual-time jumps without iterating each PWM period. */
static void bk7258_pwm_count(BK7258PWMState *s, unsigned i, Int128 cycles)
{
    BK7258PWMCounter *c = &s->counter[i];
    unsigned div = bk7258_pwm_divisor(s, i);
    Int128 scaled = int128_add(cycles, int128_make64(c->prescale));
    Int128 ticks = int128_divu(scaled, int128_make64(div));
    uint64_t first = bk7258_pwm_to_update(c), period;

    c->prescale = int128_get64(int128_remu(scaled, int128_make64(div)));
    if (int128_ult(ticks, int128_make64(first))) {
        bk7258_pwm_segment(s, i, int128_get64(ticks));
        return;
    }
    bk7258_pwm_segment(s, i, first);
    s->status |= 1U << (9 + i);
    c->count = 0;
    bk7258_pwm_preload(s, i);
    ticks = int128_sub(ticks, int128_make64(first));
    period = (uint64_t)c->arr_shadow + 1;
    if (int128_uge(ticks, int128_make64(period))) {
        /* Every supported, nonzero compare is inside this complete period. */
        for (unsigned n = 0; n < 3; n++) {
            if (c->ccr_shadow[n]) {
                s->status |= 1U << (3 * i + n);
            }
        }
        ticks = int128_remu(ticks, int128_make64(period));
    }
    bk7258_pwm_segment(s, i, int128_get64(ticks));
}

static void bk7258_pwm_update(BK7258PWMState *s)
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
    for (unsigned i = 0; i < 3; i++) {
        Int128 count_cycles = cycles;

        if ((s->pending_update & (1U << i)) && int128_nz(cycles)) {
            BK7258PWMCounter *c = &s->counter[i];

            /* Functional CDC policy: UG completes at one live input edge. */
            bk7258_pwm_preload(s, i);
            c->count = c->prescale = 0;
            s->pending_update &= ~(1U << i);
            if (!(s->control & (1U << (28 - i)))) {
                s->status |= 1U << (9 + i);
            }
            count_cycles = int128_sub(cycles, int128_one());
        }
        if (bk7258_pwm_enabled(s, i)) {
            bk7258_pwm_count(s, i, count_cycles);
        }
    }
    bk7258_pwm_irq(s);
}

static void bk7258_pwm_schedule(BK7258PWMState *s)
{
    uint64_t period = clock_get(s->clk), next = 0, lo, hi;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    Int128 delay;

    timer_del(s->timer);
    if (!(s->global & 1) || !period) {
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        BK7258PWMCounter *c = &s->counter[i];
        uint64_t distance = bk7258_pwm_to_update(c), candidate = 0;

        if (!bk7258_pwm_enabled(s, i)) {
            continue;
        }
        if (!(s->status & (1U << (9 + i))) ||
            c->arr != c->arr_shadow ||
            memcmp(c->ccr, c->ccr_shadow, sizeof(c->ccr))) {
            candidate = distance;
        }
        for (unsigned n = 0; n < 3; n++) {
            uint32_t delta = c->ccr_shadow[n] - c->count;
            uint64_t d = delta ? delta : WRAP;

            if (c->ccr_shadow[n] && !(s->status & (1U << (3 * i + n)))) {
                d = d <= distance ? d : distance + c->ccr_shadow[n];
                candidate = candidate ? MIN(candidate, d) : d;
            }
        }
        if (candidate) {
            candidate = candidate * bk7258_pwm_divisor(s, i) - c->prescale;
            next = next ? MIN(next, candidate) : candidate;
        }
    }
    if (s->pending_update) {
        next = 1;
    }
    if (!next) {
        return;
    }
    mulu64(&lo, &hi, next, period);
    delay = int128_sub(int128_make128(lo, hi), int128_make64(s->phase));
    if (int128_ult(int128_lshift(int128_make64(INT64_MAX - now), 32), delay)) {
        return;
    }
    delay = int128_urshift(int128_add(delay, int128_make64(UINT32_MAX)), 32);
    timer_mod(s->timer, now + int128_get64(delay));
}

static void bk7258_pwm_event(void *opaque)
{
    BK7258PWMState *s = opaque;

    bk7258_pwm_update(s);
    bk7258_pwm_schedule(s);
}

static void bk7258_pwm_clock(void *opaque, ClockEvent event)
{
    BK7258PWMState *s = opaque;

    if (event == ClockPreUpdate) {
        bk7258_pwm_update(s);
    } else {
        /* Preserve whole prescaler cycles, not fractional source phase. */
        s->phase = 0;
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        bk7258_pwm_schedule(s);
    }
}

static bool bk7258_pwm_valid_compares(uint32_t arr, const uint32_t *ccr)
{
    for (unsigned n = 0; n < 3; n++) {
        if (ccr[n] && (ccr[n] < 2 || ccr[n] > arr)) {
            return false;
        }
    }
    return true;
}

static MemTxResult bk7258_pwm_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    BK7258PWMState *s = opaque;

    bk7258_pwm_update(s);
    switch (offset) {
    case 0x08:
        *value = s->global;
        break;
    case 0x10:
        *value = s->control;
        break;
    /* Trigger, slave/capture, pad output and deadtime are unsupported. */
    case 0x14:
    case 0x18:
    case 0x28:
    case 0x48 ... 0x50:
    case 0x78:
    case 0x88 ... 0x90:
    case 0xb8:
        *value = 0;
        break;
    case 0x1c:
        *value = s->mask;
        break;
    case 0x20:
        *value = s->status;
        break;
    case 0x24:
        *value = s->pending_update << 9;
        break;
    case 0x2c ... 0x34:
        *value = s->counter[(offset - 0x2c) / 4].count;
        break;
    case 0x38:
        *value = s->divider;
        break;
    case 0x3c ... 0x44:
        *value = s->counter[(offset - 0x3c) / 4].arr;
        break;
    case 0x54 ... 0x74:
        *value = s->counter[(offset - 0x54) / 12].ccr[
            ((offset - 0x54) / 4) % 3];
        break;
    case 0x7c ... 0x84:
        *value = s->counter[(offset - 0x7c) / 4].arr_shadow;
        break;
    case 0x94 ... 0xb4:
        *value = s->counter[(offset - 0x94) / 12].ccr_shadow[
            ((offset - 0x94) / 4) % 3];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-pwm: unsupported read at 0x%"
                      HWADDR_PRIx "\n", offset);
        bk7258_pwm_schedule(s);
        return MEMTX_ERROR;
    }
    bk7258_pwm_schedule(s);
    return MEMTX_OK;
}

static void bk7258_pwm_reset(DeviceState *dev)
{
    BK7258PWMState *s = BK7258_PWM(dev);

    s->global = s->control = s->divider = s->mask = s->status = 0;
    s->pending_update = 0;
    memset(s->counter, 0, sizeof(s->counter));
    s->phase = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->timer);
    bk7258_pwm_irq(s);
}

static MemTxResult bk7258_pwm_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    BK7258PWMState *s = opaque;
    unsigned i, n;

    bk7258_pwm_update(s);
    switch (offset) {
    case 0x08:
        if (value & ~3U) {
            goto unsupported;
        }
        if (!(value & 1)) {
            bk7258_pwm_reset(DEVICE(s));
        }
        s->global = value;
        break;
    case 0x10:
        if (value & ~CONTROL_MASK) {
            goto unsupported;
        }
        for (i = 0; i < 3; i++) {
            BK7258PWMCounter *c = &s->counter[i];

            if ((value & (1U << (2 - i))) &&
                (!(s->global & 1) ||
                 !bk7258_pwm_valid_compares(c->arr, c->ccr) ||
                 !bk7258_pwm_valid_compares(c->arr_shadow, c->ccr_shadow))) {
                goto unsupported;
            }
        }
        /* Changing preload policy on a running counter is outside the slice. */
        if ((s->control & 7) && ((s->control ^ value) & 0x1f8)) {
            goto unsupported;
        }
        s->control = value;
        for (i = 0; i < 3; i++) {
            BK7258PWMCounter *c = &s->counter[i];

            if (!(value & (1U << (5 - i)))) {
                c->arr_shadow = c->arr;
            }
            if (!(value & (1U << (8 - i)))) {
                memcpy(c->ccr_shadow, c->ccr, sizeof(c->ccr));
            }
        }
        break;
    case 0x14:
    case 0x18:
    case 0x28:
    case 0x48 ... 0x50:
    case 0x78:
        if (value) {
            goto unsupported;
        }
        break;
    case 0x1c:
        if (value & ~EVENTS) {
            goto unsupported;
        }
        s->mask = value;
        break;
    case 0x20:
        s->status &= ~(value & EVENTS);
        break;
    case 0x24:
        if ((value & ~0xe00U) || !(s->global & 1)) {
            goto unsupported;
        }
        for (i = 0; i < 3; i++) {
            if ((value & (1U << (9 + i))) &&
                !bk7258_pwm_valid_compares(s->counter[i].arr,
                                           s->counter[i].ccr)) {
                goto unsupported;
            }
        }
        s->pending_update |= value >> 9;
        break;
    case 0x38:
        if ((value & 0xff000000) || (s->control & 7)) {
            goto unsupported;
        }
        s->divider = value;
        for (i = 0; i < 3; i++) {
            s->counter[i].prescale = 0;
        }
        break;
    case 0x3c ... 0x44:
        i = (offset - 0x3c) / 4;
        if (bk7258_pwm_enabled(s, i) &&
            (!bk7258_pwm_valid_compares(value, s->counter[i].ccr) ||
             (!(s->control & (1U << (5 - i))) &&
              !bk7258_pwm_valid_compares(value, s->counter[i].ccr_shadow)))) {
            goto unsupported;
        }
        s->counter[i].arr = value;
        if (!(s->control & (1U << (5 - i)))) {
            s->counter[i].arr_shadow = value;
        }
        break;
    case 0x54 ... 0x74:
        i = (offset - 0x54) / 12;
        n = ((offset - 0x54) / 4) % 3;
        if (bk7258_pwm_enabled(s, i) && value &&
            (value < 2 || value > s->counter[i].arr ||
             (!(s->control & (1U << (8 - i))) &&
              value > s->counter[i].arr_shadow))) {
            goto unsupported;
        }
        s->counter[i].ccr[n] = value;
        if (!(s->control & (1U << (8 - i)))) {
            s->counter[i].ccr_shadow[n] = value;
        }
        break;
    default:
        goto unsupported;
    }
    bk7258_pwm_irq(s);
    bk7258_pwm_schedule(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-pwm: unsupported write 0x%" PRIx64
                  " at 0x%" HWADDR_PRIx "\n", value, offset);
    bk7258_pwm_schedule(s);
    return MEMTX_ERROR;
}

static const MemoryRegionOps bk7258_pwm_ops = {
    .read_with_attrs = bk7258_pwm_read,
    .write_with_attrs = bk7258_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_pwm_init(Object *obj)
{
    BK7258PWMState *s = BK7258_PWM(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_pwm_event, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "pclk", bk7258_pwm_clock, s,
                                ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_pwm_ops, s,
                          "bk7258-pwm", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void bk7258_pwm_finalize(Object *obj)
{
    timer_free(BK7258_PWM(obj)->timer);
}

static void bk7258_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bk7258_pwm_reset);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_pwm_info = {
    .name = TYPE_BK7258_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258PWMState),
    .instance_init = bk7258_pwm_init,
    .instance_finalize = bk7258_pwm_finalize,
    .class_init = bk7258_pwm_class_init,
};

static void bk7258_pwm_register_types(void)
{
    type_register_static(&bk7258_pwm_info);
}

type_init(bk7258_pwm_register_types)
