/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ADC_BK7258_SARADC_H
#define HW_ADC_BK7258_SARADC_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"

#define TYPE_BK7258_SARADC "bk7258-saradc"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258SARADCState, BK7258_SARADC)

struct BK7258SARADCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    QEMUTimer *timer;
    uint32_t input[6];
    uint32_t control;
    uint32_t steady;
    uint32_t saturation;
    uint64_t cycles;
    unsigned hz;
    uint16_t result;
    uint16_t pending_result;
    bool active;
    bool valid;
};

#endif
