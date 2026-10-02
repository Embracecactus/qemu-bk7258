/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_BK7258_ENTRY_PROBE_H
#define HW_MISC_BK7258_ENTRY_PROBE_H

#include "hw/core/sysbus.h"

#define TYPE_BK7258_ENTRY_PROBE "bk7258-entry-probe"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258EntryProbeState, BK7258_ENTRY_PROBE)

/* Diagnostic snapshot lifetime, deliberately not an OTP hardware FSM. */
typedef enum BK7258EntryProbePhase {
    BK7258_ENTRY_PENDING,
    BK7258_ENTRY_VISIBLE,
    BK7258_ENTRY_INVALID,
} BK7258EntryProbePhase;

struct BK7258EntryProbeState {
    SysBusDevice parent_obj;
    MemoryRegion otp;
    MemoryRegion r7a;
    char *otp_control_input;
    char *otp_status_input;
    char *otp_word242_input;
    char *r7a_input;
    uint32_t otp_control;
    uint32_t otp_status;
    uint32_t otp_word242;
    uint32_t r7a_value;
    bool has_otp;
    bool has_r7a;
    bool reset_seen;
    bool r7a_valid;
    BK7258EntryProbePhase phase;
};

void bk7258_entry_probe_ready(BK7258EntryProbeState *s, bool ready);

#endif
