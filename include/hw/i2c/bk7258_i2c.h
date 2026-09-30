/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_BK7258_I2C_H
#define HW_I2C_BK7258_I2C_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/i2c/i2c.h"
#include "qemu/timer.h"

#define TYPE_BK7258_I2C "bk7258-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258I2CState, BK7258_I2C)

struct BK7258I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    I2CBus *bus;
    char *bus_name;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t global;
    uint32_t config;
    uint32_t status;
    uint32_t extra;
    uint8_t tx[16];
    uint8_t rx[16];
    unsigned tx_head, tx_count, rx_head, rx_count;
    unsigned operation;
    unsigned hz;
    uint64_t cycles;
    int64_t deadline;
    uint8_t address;
    bool busy;
    bool selected;
    bool bus_owned;
    bool receiving;
    bool read_nacked;
};

#endif
