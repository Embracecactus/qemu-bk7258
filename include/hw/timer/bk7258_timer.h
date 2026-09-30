/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_BK7258_TIMER_H
#define HW_TIMER_BK7258_TIMER_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"

#define TYPE_BK7258_TIMER "bk7258-timer"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258TimerState, BK7258_TIMER)

struct BK7258TimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t global;
    uint32_t control;
    uint32_t end[3];
    uint32_t counter[3];
    uint32_t read_control;
    uint32_t read_value;
    unsigned prescale;
    uint64_t phase;
    int64_t last_ns;
};

#endif
