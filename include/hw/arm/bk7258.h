/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_BK7258_H
#define HW_ARM_BK7258_H

#include "hw/arm/armv7m.h"
#include "hw/char/bk7258_uart.h"

#define TYPE_BK7258_SOC "bk7258-soc"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258State, BK7258_SOC)

struct BK7258State {
    SysBusDevice parent_obj;
    ARMv7MState cpu[3];
    BK7258UARTState uart[3];
    MemoryRegion cpu_memory[3];
    MemoryRegion shared_alias[3];
    MemoryRegion itcm[3];
    MemoryRegion dtcm[3];
    MemoryRegion itcm_ns[3];
    MemoryRegion dtcm_ns[3];
    MemoryRegion flash;
    MemoryRegion flash_ns;
    MemoryRegion sram;
    MemoryRegion sram_alias[3];
    MemoryRegion sysctrl;
    MemoryRegion sysctrl_ns;
    MemoryRegion uart_ns[3];
    Clock *cpuclk;
    Clock *refclk[3];
    uint32_t cpu_control[3];
    uint32_t irq_enable[3][2];
    uint32_t uart_levels;
    uint32_t flash_size;
    uint32_t boot_vector;
};

#endif
