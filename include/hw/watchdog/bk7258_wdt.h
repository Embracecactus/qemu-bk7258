/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_WATCHDOG_BK7258_WDT_H
#define HW_WATCHDOG_BK7258_WDT_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"

#define TYPE_BK7258_WDT "bk7258-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258WDTState, BK7258_WDT)

struct BK7258WDTState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    QEMUTimer *timer;
    qemu_irq nmi;
    uint32_t global_ctrl;
    uint32_t period;
    uint32_t pending_period;
    uint32_t remaining;
    uint64_t hz;
    int64_t deadline;
    bool keyed;
    bool armed;
    bool aon;
};

#endif
