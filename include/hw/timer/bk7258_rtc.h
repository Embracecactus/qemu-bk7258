/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_BK7258_RTC_H
#define HW_TIMER_BK7258_RTC_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"

#define TYPE_BK7258_RTC "bk7258-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258RTCState, BK7258_RTC)

struct BK7258RTCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t control;
    /* Upper low/high, tick low/high; APB staging and synchronized values. */
    uint32_t staging[4];
    uint32_t synchronized[4];
    uint8_t sync_left[4];
    uint64_t counter;
    uint64_t phase;
    int64_t last_ns;
};

#endif
