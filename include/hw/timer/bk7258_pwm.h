/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_BK7258_PWM_H
#define HW_TIMER_BK7258_PWM_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"

#define TYPE_BK7258_PWM "bk7258-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258PWMState, BK7258_PWM)

typedef struct BK7258PWMCounter {
    uint32_t count;
    uint32_t arr;
    uint32_t arr_shadow;
    uint32_t ccr[3];
    uint32_t ccr_shadow[3];
    unsigned prescale;
} BK7258PWMCounter;

struct BK7258PWMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    QEMUTimer *timer;
    qemu_irq irq;
    BK7258PWMCounter counter[3];
    uint32_t global;
    uint32_t control;
    uint32_t divider;
    uint32_t mask;
    uint32_t status;
    uint32_t pending_update;
    uint64_t phase;
    int64_t last_ns;
};

#endif
