/*
 * BK7258 single-step SARADC digital experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Facts: pinned SDK adc_{struct,ll}.h, adc_hal.c and BK7258 SARADC guide.
 * Explicit raw inputs, no calibration/voltage/IRQ or full-SDK claim.
 * Timing/reset/unsupported-access policies: docs/system/arm/bk7258.rst.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/adc/bk7258_saradc.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/bk7258_clock.h"

#define MODE_MASK 3U
#define ENABLE (1U << 2)
#define CONTROL_MASK 0x7effU
#define STATUS_MASK (7U << 29)
#define BUSY (1U << 29)
#define EMPTY (1U << 30)
#define BYPASS_CALIBRATION (1U << 10)
#define STEADY_MASK (BYPASS_CALIBRATION | (7U << 5))

static void bk7258_saradc_cancel(BK7258SARADCState *s)
{
    timer_del(s->timer);
    s->active = false;
    s->cycles = 0;
}

static void bk7258_saradc_complete(void *opaque)
{
    BK7258SARADCState *s = opaque;

    if (!s->active) {
        return;
    }
    bk7258_saradc_cancel(s);
    s->result = s->pending_result;
    s->valid = true;
    /* The pinned HAL waits for sleep BEFORE reading ADC_DATA_16. */
    s->control &= ~MODE_MASK;
}

static void bk7258_saradc_schedule(BK7258SARADCState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t delay;

    timer_del(s->timer);
    if (!s->active || !s->hz) {
        return;
    }
    delay = DIV_ROUND_UP(s->cycles * NANOSECONDS_PER_SECOND, s->hz);
    if (delay <= INT64_MAX - now) {
        timer_mod(s->timer, now + delay);
    }
}

static void bk7258_saradc_clock(void *opaque, ClockEvent event)
{
    BK7258SARADCState *s = opaque;

    if (event == ClockPreUpdate) {
        if (s->active && timer_pending(s->timer)) {
            s->cycles = bk7258_remaining_cycles(s->cycles, s->hz,
                timer_expire_time_ns(s->timer),
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            if (!s->cycles) {
                bk7258_saradc_complete(s);
            }
        }
        timer_del(s->timer);
    } else {
        s->hz = clock_get_hz(s->clk);
        bk7258_saradc_schedule(s);
    }
}

static bool bk7258_saradc_start(BK7258SARADCState *s, uint32_t control)
{
    unsigned channel = (control >> 3) & 15;
    unsigned divisor = 2 * (((control >> 9) & 63) + 1);
    unsigned wait = control & (1U << 7) ? 8 : 4;
    unsigned steady = (((s->steady >> 5) & 7) + 1) * 8;

    if (channel < 1 || channel > 6 ||
        s->input[channel - 1] == UINT32_MAX || s->valid ||
        !(s->steady & BYPASS_CALIBRATION) || s->saturation != 7) {
        return false;
    }
    s->control = control;
    s->pending_result = s->input[channel - 1];
    /*
     * Functional policy: serialize the documented stage budgets. Their
     * physical overlap and analog settling are not established by the SDK.
     * Snapshot the explicitly supplied digital code, not an analog waveform.
     */
    s->cycles = (16 + wait + steady) * divisor;
    s->active = true;
    bk7258_saradc_schedule(s);
    return true;
}

static MemTxResult bk7258_saradc_read(void *opaque, hwaddr addr,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    BK7258SARADCState *s = opaque;

    *value = 0;
    switch (addr) {
    case 0x10:
        *value = s->control | (s->active ? BUSY : 0) |
                 (s->valid ? 0 : EMPTY);
        break;
    case 0x18:
        *value = s->steady;
        break;
    case 0x1c:
        *value = s->saturation;
        break;
    case 0x20:
        if (!s->valid) {
            goto unsupported;
        }
        *value = s->result;
        s->valid = false;
        break;
    default:
        goto unsupported;
    }
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP,
                  "bk7258-saradc: unsupported or empty read at 0x%"
                  HWADDR_PRIx "\n", addr);
    return MEMTX_ERROR;
}

static MemTxResult bk7258_saradc_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    BK7258SARADCState *s = opaque;
    uint32_t control = value & ~STATUS_MASK;

    switch (addr) {
    case 0x10:
        if (control & ~CONTROL_MASK || (control & MODE_MASK) > 1 ||
            ((control >> 3) & 15) > 6) {
            goto unsupported;
        }
        if (s->active) {
            if (control == s->control) {
                return MEMTX_OK;
            }
            if (((control ^ s->control) & ~(ENABLE | MODE_MASK)) ||
                ((control & ENABLE) && (control & MODE_MASK))) {
                goto unsupported;
            }
            bk7258_saradc_cancel(s);
        } else if ((control & ENABLE) && (control & MODE_MASK)) {
            if (!bk7258_saradc_start(s, control)) {
                goto unsupported;
            }
            return MEMTX_OK;
        }
        s->control = control;
        break;
    case 0x18:
        if (s->active || (value & ~STEADY_MASK)) {
            goto unsupported;
        }
        s->steady = value;
        break;
    case 0x1c:
        if (s->active || (value & ~7U)) {
            goto unsupported;
        }
        s->saturation = value;
        break;
    default:
        /* Includes the unresolved software-reset control at +0x08. */
        goto unsupported;
    }
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP,
                  "bk7258-saradc: unsupported write 0x%" PRIx64
                  " at 0x%" HWADDR_PRIx "\n", value, addr);
    return MEMTX_ERROR;
}

static void bk7258_saradc_reset(DeviceState *dev)
{
    BK7258SARADCState *s = BK7258_SARADC(dev);

    bk7258_saradc_cancel(s);
    s->control = s->steady = s->saturation = 0;
    s->result = s->pending_result = 0;
    s->valid = false;
    /* Explicit external inputs survive this functional device reset. */
}

static const MemoryRegionOps bk7258_saradc_ops = {
    .read_with_attrs = bk7258_saradc_read,
    .write_with_attrs = bk7258_saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_saradc_realize(DeviceState *dev, Error **errp)
{
    BK7258SARADCState *s = BK7258_SARADC(dev);

    for (unsigned i = 0; i < ARRAY_SIZE(s->input); i++) {
        if (s->input[i] != UINT32_MAX && s->input[i] > 4095) {
            error_setg(errp, "BK7258 SARADC input%u must be a 12-bit raw code",
                       i + 1);
            return;
        }
    }
    s->hz = clock_get_hz(s->clk);
}

static void bk7258_saradc_init(Object *obj)
{
    BK7258SARADCState *s = BK7258_SARADC(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_saradc_complete, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", bk7258_saradc_clock, s,
                                ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_saradc_ops, s,
                          TYPE_BK7258_SARADC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void bk7258_saradc_finalize(Object *obj)
{
    timer_free(BK7258_SARADC(obj)->timer);
}

static const Property bk7258_saradc_properties[] = {
    DEFINE_PROP_UINT32("input1", BK7258SARADCState, input[0], UINT32_MAX),
    DEFINE_PROP_UINT32("input2", BK7258SARADCState, input[1], UINT32_MAX),
    DEFINE_PROP_UINT32("input3", BK7258SARADCState, input[2], UINT32_MAX),
    DEFINE_PROP_UINT32("input4", BK7258SARADCState, input[3], UINT32_MAX),
    DEFINE_PROP_UINT32("input5", BK7258SARADCState, input[4], UINT32_MAX),
    DEFINE_PROP_UINT32("input6", BK7258SARADCState, input[5], UINT32_MAX),
};

static void bk7258_saradc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_saradc_realize;
    device_class_set_legacy_reset(dc, bk7258_saradc_reset);
    device_class_set_props(dc, bk7258_saradc_properties);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_saradc_info = {
    .name = TYPE_BK7258_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258SARADCState),
    .instance_init = bk7258_saradc_init,
    .instance_finalize = bk7258_saradc_finalize,
    .class_init = bk7258_saradc_class_init,
};

static void bk7258_saradc_register_types(void)
{
    type_register_static(&bk7258_saradc_info);
}

type_init(bk7258_saradc_register_types)
