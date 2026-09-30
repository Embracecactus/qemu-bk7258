/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_BK7258_CKMN_H
#define HW_MISC_BK7258_CKMN_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"

#define TYPE_BK7258_CKMN "bk7258-ckmn"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258CKMNState, BK7258_CKMN)

struct BK7258CKMNState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *reference;
    Clock *measured;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t global;
    uint32_t window;
    uint32_t control;
    uint32_t result;
    uint32_t correction;
    uint32_t status;
    uint32_t pending_result;
    bool active;
};

#endif
