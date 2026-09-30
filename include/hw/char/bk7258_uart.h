/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CHAR_BK7258_UART_H
#define HW_CHAR_BK7258_UART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_BK7258_UART "bk7258-uart"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258UARTState, BK7258_UART)

struct BK7258UARTState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
    uint32_t global_ctrl;
    uint32_t config;
    uint32_t fifo_config;
    uint32_t int_enable;
    uint32_t int_status;
    uint8_t rx_fifo[128];
    uint32_t rx_head;
    uint32_t rx_count;
};

#endif
