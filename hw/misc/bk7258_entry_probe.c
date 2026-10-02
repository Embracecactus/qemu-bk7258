/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Explicit diagnostic entry-state injection, NOT an OTP/reset controller.
 * AI-assisted downstream experiment, not an upstream contribution.
 *
 * Addresses/fields: BK7258 SDK cb080de otp_struct.h and reset_reason.c.
 * Visibility, invalidation and rejected writes are probe policies, not
 * claimed silicon behavior. No snapshot is supplied or mapped by default.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/bk7258_entry_probe.h"
#include "trace.h"

void bk7258_entry_probe_ready(BK7258EntryProbeState *s, bool ready)
{
    if (!s->reset_seen || !s->has_otp) {
        return;
    }
    if (s->phase == BK7258_ENTRY_PENDING && ready) {
        s->phase = BK7258_ENTRY_VISIBLE;
        trace_bk7258_entry_probe_phase(s->phase);
    } else if (s->phase == BK7258_ENTRY_VISIBLE && !ready) {
        s->phase = BK7258_ENTRY_INVALID;
        trace_bk7258_entry_probe_phase(s->phase);
    }
}

static MemTxResult entry_otp_read(void *opaque, hwaddr addr, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    BK7258EntryProbeState *s = opaque;

    if (s->phase == BK7258_ENTRY_VISIBLE) {
        switch (addr) {
        case 0x2c8:
            *value = s->otp_control;
            trace_bk7258_entry_probe_otp_read(addr, *value);
            return MEMTX_OK;
        case 0x2c4:
            *value = s->otp_status;
            trace_bk7258_entry_probe_otp_read(addr, *value);
            return MEMTX_OK;
        case 0x7c8:
            if (!s->otp_status) {
                *value = s->otp_word242;
                trace_bk7258_entry_probe_otp_read(addr, *value);
                return MEMTX_OK;
            }
            break;
        }
    }
    qemu_log_mask(LOG_UNIMP, "bk7258-entry-probe: rejected OTP read at "
                  "0x%" HWADDR_PRIx " (phase %u)\n", addr, s->phase);
    return MEMTX_ERROR;
}

static MemTxResult entry_otp_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    BK7258EntryProbeState *s = opaque;

    /* Idempotent observation only: never emulate an activation transition. */
    if (s->phase == BK7258_ENTRY_VISIBLE && addr == 0x2c8 &&
        value == s->otp_control) {
        trace_bk7258_entry_probe_otp_same_write(addr, value);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "bk7258-entry-probe: rejected OTP write at "
                  "0x%" HWADDR_PRIx " (phase %u)\n", addr, s->phase);
    return MEMTX_ERROR;
}

static MemTxResult entry_r7a_read(void *opaque, hwaddr addr, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    BK7258EntryProbeState *s = opaque;

    if (s->r7a_valid) {
        *value = s->r7a_value;
        trace_bk7258_entry_probe_r7a_read(*value);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "bk7258-entry-probe: invalid R7A snapshot\n");
    return MEMTX_ERROR;
}

static MemTxResult entry_r7a_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "bk7258-entry-probe: rejected R7A write\n");
    return MEMTX_ERROR;
}

static const MemoryRegionOps entry_otp_ops = {
    .read_with_attrs = entry_otp_read,
    .write_with_attrs = entry_otp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4,
               .unaligned = false },
};

static const MemoryRegionOps entry_r7a_ops = {
    .read_with_attrs = entry_r7a_read,
    .write_with_attrs = entry_r7a_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4,
               .unaligned = false },
};

static void entry_probe_reset(DeviceState *dev)
{
    BK7258EntryProbeState *s = BK7258_ENTRY_PROBE(dev);

    /* Initial construction provides one epoch; later reset never reloads it. */
    s->phase = s->reset_seen ? BK7258_ENTRY_INVALID : BK7258_ENTRY_PENDING;
    s->r7a_valid = s->has_r7a && !s->reset_seen;
    s->reset_seen = true;
    if (s->has_otp || s->has_r7a) {
        trace_bk7258_entry_probe_phase(s->phase);
    }
}

static bool entry_input_word(const char *input, uint32_t *word)
{
    uint64_t value;

    if (!input || parse_uint_full(input, 0, &value) < 0 ||
        value > UINT32_MAX) {
        return false;
    }
    *word = value;
    return true;
}

static void entry_probe_realize(DeviceState *dev, Error **errp)
{
    BK7258EntryProbeState *s = BK7258_ENTRY_PROBE(dev);
    bool any_otp = s->otp_control_input || s->otp_status_input ||
                   s->otp_word242_input;

    if (any_otp && (!entry_input_word(s->otp_control_input, &s->otp_control) ||
                   !entry_input_word(s->otp_status_input, &s->otp_status) ||
                   !entry_input_word(s->otp_word242_input, &s->otp_word242) ||
                   s->otp_control != 3 || s->otp_status > 1)) {
        error_setg(errp, "entry probe requires explicit otp-control=3, "
                   "otp-status=0 or 1 and a 32-bit otp-word242");
        return;
    }
    if (s->r7a_input && !entry_input_word(s->r7a_input, &s->r7a_value)) {
        error_setg(errp, "entry probe r7a must be an explicit 32-bit word");
        return;
    }
    s->has_otp = any_otp;
    s->has_r7a = s->r7a_input != NULL;
    if (s->has_otp && s->has_r7a) {
        error_setg(errp, "entry probe requires separate OTP and R7A runs");
        return;
    }
    if (s->has_otp || s->has_r7a) {
        warn_report("BK7258 diagnostic entry-state injection enabled; "
                    "not OTP activation, reset-capture or cold-boot coverage");
    }
}

static void entry_probe_init(Object *obj)
{
    BK7258EntryProbeState *s = BK7258_ENTRY_PROBE(obj);

    memory_region_init_io(&s->otp, obj, &entry_otp_ops, s,
                          "bk7258.diagnostic-otp-snapshot", 0x800);
    memory_region_init_io(&s->r7a, obj, &entry_r7a_ops, s,
                          "bk7258.diagnostic-r7a-snapshot", 4);
}

static const Property entry_probe_properties[] = {
    DEFINE_PROP_STRING("otp-control", BK7258EntryProbeState, otp_control_input),
    DEFINE_PROP_STRING("otp-status", BK7258EntryProbeState, otp_status_input),
    DEFINE_PROP_STRING("otp-word242", BK7258EntryProbeState, otp_word242_input),
    DEFINE_PROP_STRING("r7a", BK7258EntryProbeState, r7a_input),
};

static void entry_probe_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = entry_probe_realize;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, entry_probe_reset);
    device_class_set_props(dc, entry_probe_properties);
}

static const TypeInfo entry_probe_type = {
    .name = TYPE_BK7258_ENTRY_PROBE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258EntryProbeState),
    .instance_init = entry_probe_init,
    .class_init = entry_probe_class_init,
};

static void entry_probe_register_types(void)
{
    type_register_static(&entry_probe_type);
}

type_init(entry_probe_register_types)
