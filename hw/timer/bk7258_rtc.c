/*
 * BK7258 AON RTC functional counter/compare model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register facts: pinned SDK aon_rtc_{struct,ll}.h and 64-bit driver.
 * Unmeasured edge/synchronizer policies: docs/system/arm/bk7258.rst.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/int128.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/timer/bk7258_rtc.h"

#define CNT_RESET 1U
#define CNT_STOP 2U
#define UPPER_IE 4U
#define TICK_IE 8U
#define UPPER_STATUS 16U
#define TICK_STATUS 32U
#define CLOCK_ENABLE 64U
#define STATUS_MASK (UPPER_STATUS | TICK_STATUS)

static uint64_t bk7258_rtc_upper(BK7258RTCState *s)
{
    return s->synchronized[0] | (uint64_t)s->synchronized[1] << 32;
}

static uint64_t bk7258_rtc_tick(BK7258RTCState *s)
{
    return s->synchronized[2] | (uint64_t)s->synchronized[3] << 32;
}

static void bk7258_rtc_irq(BK7258RTCState *s)
{
    qemu_set_irq(s->irq,
                 ((s->control & UPPER_IE) && (s->control & UPPER_STATUS)) ||
                 ((s->control & TICK_IE) && (s->control & TICK_STATUS)));
}

/* Zero means unreachable; 2^64 is a valid distance, not integer overflow. */
static Int128 bk7258_rtc_distance(BK7258RTCState *s, uint64_t target)
{
    uint64_t upper = bk7258_rtc_upper(s);

    if (target > upper && !(s->counter > upper && target > s->counter)) {
        return int128_zero();
    }
    if (target > s->counter) {
        return int128_make64(target - s->counter);
    }
    if (s->counter <= upper) {
        return int128_add(int128_make64(upper - s->counter),
                          int128_add(int128_make64(target), int128_one()));
    }
    /* A lowered upper threshold is first encountered after natural wrap. */
    return int128_add(int128_sub(int128_2_64(), int128_make64(s->counter)),
                      int128_make64(target));
}

static void bk7258_rtc_count(BK7258RTCState *s, Int128 ticks)
{
    uint64_t upper = bk7258_rtc_upper(s);
    Int128 distance, period;

    if ((s->control & (CNT_RESET | CNT_STOP)) || !int128_nz(ticks)) {
        return;
    }
    distance = bk7258_rtc_distance(s, upper);
    if (int128_uge(ticks, distance)) {
        s->control |= UPPER_STATUS;
    }
    distance = bk7258_rtc_distance(s, bk7258_rtc_tick(s));
    if (int128_nz(distance) && int128_uge(ticks, distance)) {
        s->control |= TICK_STATUS;
    }
    if (s->counter > upper) {
        distance = int128_sub(int128_2_64(), int128_make64(s->counter));
        if (int128_ult(ticks, distance)) {
            s->counter += int128_get64(ticks);
            return;
        }
        ticks = int128_sub(ticks, distance);
        s->counter = 0;
    }
    /* Inclusive upper: visit upper, then wrap to zero on the next edge. */
    period = int128_add(int128_make64(upper), int128_one());
    s->counter = int128_get64(int128_remu(
        int128_add(int128_make64(s->counter), ticks), period));
}

static unsigned bk7258_rtc_pending(BK7258RTCState *s)
{
    unsigned next = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(s->sync_left); i++) {
        if (s->sync_left[i] && (!next || s->sync_left[i] < next)) {
            next = s->sync_left[i];
        }
    }
    return next;
}

static void bk7258_rtc_update(BK7258RTCState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t period = clock_get(s->clk);
    Int128 elapsed, ticks, chunk;
    unsigned pending;

    if (!(s->control & CLOCK_ENABLE) || !period) {
        s->last_ns = now;
        return;
    }
    elapsed = int128_add(int128_lshift(int128_make64(now - s->last_ns), 32),
                         int128_make64(s->phase));
    ticks = int128_divu(elapsed, int128_make64(period));
    s->phase = int128_get64(int128_remu(elapsed, int128_make64(period)));
    s->last_ns = now;
    while (int128_nz(ticks)) {
        pending = bk7258_rtc_pending(s);
        chunk = pending ? int128_min(ticks, int128_make64(pending)) : ticks;
        bk7258_rtc_count(s, chunk);
        ticks = int128_sub(ticks, chunk);
        if (pending) {
            for (unsigned i = 0; i < ARRAY_SIZE(s->sync_left); i++) {
                if (s->sync_left[i]) {
                    s->sync_left[i] -= int128_get64(chunk);
                    if (!s->sync_left[i]) {
                        s->synchronized[i] = s->staging[i];
                    }
                }
            }
        }
    }
    bk7258_rtc_irq(s);
}

static void bk7258_rtc_schedule(BK7258RTCState *s)
{
    uint64_t period = clock_get(s->clk), lo, hi;
    unsigned pending = bk7258_rtc_pending(s);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    Int128 next = int128_make64(pending), distance, delay, available;

    timer_del(s->timer);
    if (!(s->control & CLOCK_ENABLE) || !period) {
        return;
    }
    if (!(s->control & (CNT_STOP | CNT_RESET))) {
        if (!(s->control & UPPER_STATUS)) {
            distance = bk7258_rtc_distance(s, bk7258_rtc_upper(s));
            if (!int128_nz(next) || int128_ult(distance, next)) {
                next = distance;
            }
        }
        if (!(s->control & TICK_STATUS)) {
            distance = bk7258_rtc_distance(s, bk7258_rtc_tick(s));
            if (int128_nz(distance) &&
                (!int128_nz(next) || int128_ult(distance, next))) {
                next = distance;
            }
        }
    }
    if (!int128_nz(next)) {
        return;
    }
    if (int128_gethi(next)) { /* The only wide distance is exactly 2^64. */
        delay = int128_make128(0, period);
    } else {
        mulu64(&lo, &hi, int128_get64(next), period);
        delay = int128_make128(lo, hi);
    }
    delay = int128_sub(delay, int128_make64(s->phase));
    available = int128_lshift(int128_make64(INT64_MAX - now), 32);
    if (int128_ult(available, delay)) {
        return; /* Do not wrap or repeatedly schedule at saturated time. */
    }
    delay = int128_urshift(int128_add(delay, int128_make64(UINT32_MAX)), 32);
    timer_mod(s->timer, now + int128_get64(delay));
}

static void bk7258_rtc_event(void *opaque)
{
    BK7258RTCState *s = opaque;

    bk7258_rtc_update(s);
    bk7258_rtc_schedule(s);
}

static void bk7258_rtc_clock(void *opaque, ClockEvent event)
{
    BK7258RTCState *s = opaque;

    if (event == ClockPreUpdate) {
        bk7258_rtc_update(s);
    } else {
        /* Whole ticks survive source changes; fractional phase is unmodeled. */
        s->phase = 0;
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        bk7258_rtc_schedule(s);
    }
}

static int bk7258_rtc_index(hwaddr offset)
{
    switch (offset) {
    case 0x04:
        return 0;
    case 0x18:
        return 1;
    case 0x08:
        return 2;
    case 0x1c:
        return 3;
    default:
        return -1;
    }
}

static MemTxResult bk7258_rtc_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    BK7258RTCState *s = opaque;
    int index = bk7258_rtc_index(offset);

    bk7258_rtc_update(s);
    if (index >= 0) {
        *value = s->staging[index];
    } else {
        switch (offset) {
        case 0x00:
            *value = s->control;
            break;
        case 0x0c:
            *value = (uint32_t)s->counter;
            break;
        case 0x28:
            *value = s->counter >> 32;
            break;
        case 0x10:
            *value = s->synchronized[0];
            break;
        case 0x20:
            *value = s->synchronized[1];
            break;
        case 0x14:
            *value = s->synchronized[2];
            break;
        case 0x24:
            *value = s->synchronized[3];
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "bk7258-rtc: read offset 0x%" HWADDR_PRIx
                          " is not implemented\n", offset);
            return MEMTX_ERROR;
        }
    }
    bk7258_rtc_schedule(s);
    return MEMTX_OK;
}

static MemTxResult bk7258_rtc_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    BK7258RTCState *s = opaque;
    int index = bk7258_rtc_index(offset);

    bk7258_rtc_update(s);
    if (index >= 0) {
        if (s->sync_left[index]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bk7258-rtc: threshold register is synchronizing\n");
        } else {
            s->staging[index] = value;
            s->sync_left[index] = 3;
        }
    } else if (offset == 0) {
        uint32_t status = s->control & STATUS_MASK & ~value;

        s->control = (value & 0x4f) | status;
        if (value & CNT_RESET) {
            s->counter = 0;
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "bk7258-rtc: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return MEMTX_ERROR;
    }
    bk7258_rtc_irq(s);
    bk7258_rtc_schedule(s);
    return MEMTX_OK;
}

static const MemoryRegionOps bk7258_rtc_ops = {
    .read_with_attrs = bk7258_rtc_read,
    .write_with_attrs = bk7258_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_rtc_reset_enter(Object *obj, ResetType type)
{
    BK7258RTCState *s = BK7258_RTC(obj);

    s->control = 0;
    s->counter = s->phase = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memset(s->staging, 0, sizeof(s->staging));
    memset(s->synchronized, 0, sizeof(s->synchronized));
    memset(s->sync_left, 0, sizeof(s->sync_left));
    timer_del(s->timer);
}

static void bk7258_rtc_reset_hold(Object *obj, ResetType type)
{
    bk7258_rtc_irq(BK7258_RTC(obj));
}

static void bk7258_rtc_init(Object *obj)
{
    BK7258RTCState *s = BK7258_RTC(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_rtc_event, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "lpo", bk7258_rtc_clock, s,
                                ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_rtc_ops, s,
                          "bk7258-rtc", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void bk7258_rtc_finalize(Object *obj)
{
    timer_free(BK7258_RTC(obj)->timer);
}

static void bk7258_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = bk7258_rtc_reset_enter;
    rc->phases.hold = bk7258_rtc_reset_hold;
    dc->user_creatable = false;
}

static const TypeInfo bk7258_rtc_info = {
    .name = TYPE_BK7258_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258RTCState),
    .instance_init = bk7258_rtc_init,
    .instance_finalize = bk7258_rtc_finalize,
    .class_init = bk7258_rtc_class_init,
};

static void bk7258_rtc_register_types(void)
{
    type_register_static(&bk7258_rtc_info);
}

type_init(bk7258_rtc_register_types)
