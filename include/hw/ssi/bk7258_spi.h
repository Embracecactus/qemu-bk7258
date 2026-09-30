/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_BK7258_SPI_H
#define HW_SSI_BK7258_SPI_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/ssi/ssi.h"
#include "qemu/timer.h"

#define TYPE_BK7258_SPI "bk7258-spi"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258SPIState, BK7258_SPI)

struct BK7258SPIState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    SSIBus *bus;
    char *bus_name;
    qemu_irq irq;
    qemu_irq cs;
    QEMUTimer *timer;
    uint32_t global;
    uint32_t control;
    uint32_t config;
    uint32_t status;
    uint8_t tx[64];
    uint8_t rx[64];
    unsigned tx_head, tx_count, rx_head, rx_count;
    unsigned tx_left, rx_left;
    unsigned hz;
    uint64_t cycles;
    int64_t deadline;
    bool active;
    bool pending;
    bool started;
};

#endif
