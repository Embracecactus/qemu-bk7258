/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_BK7258_QSPI_H
#define HW_SSI_BK7258_QSPI_H
#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/ssi/ssi.h"
#include "qemu/timer.h"
#define TYPE_BK7258_QSPI "bk7258-qspi"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258QSPIState, BK7258_QSPI)
struct BK7258QSPIState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *clk;
    SSIBus *bus;
    char *bus_name;
    qemu_irq cs;
    QEMUTimer *timer;
    uint32_t cmd[2][4], config, done;
    uint8_t data[36], command[4];
    unsigned bank, command_len, length, pos, hz;
    uint64_t cycles;
    int64_t deadline;
    bool active, selected;
};
#endif
