/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_BK7258_AON_H
#define HW_MISC_BK7258_AON_H

#include "hw/core/sysbus.h"

#define TYPE_BK7258_AON "bk7258-aon"
#define BK7258_GPIO_COUNT 56
/* gpio-in accepts driven levels 0/1, or -1 to release an external contact. */
#define BK7258_GPIO_FLOAT (-1)
OBJECT_DECLARE_SIMPLE_TYPE(BK7258AONState, BK7258_AON)

struct BK7258AONState {
    SysBusDevice parent_obj;
    MemoryRegion pmu;
    MemoryRegion gpio;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t sleep_config;
    uint32_t wake_config;
    uint32_t retained;
    uint32_t chip_id;
    uint32_t pin[BK7258_GPIO_COUNT];
    uint32_t latched_pin[BK7258_GPIO_COUNT];
    uint64_t inputs;
    uint64_t connected;
    uint64_t irq_status;
    bool commit_key;
    bool gpio_locked;
    qemu_irq pin_out[BK7258_GPIO_COUNT];
    qemu_irq irq;
};

#endif
