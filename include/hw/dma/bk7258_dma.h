/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DMA_BK7258_DMA_H
#define HW_DMA_BK7258_DMA_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "system/memory.h"
#include "qemu/timer.h"

#define TYPE_BK7258_DMA "bk7258-dma"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258DMAState, BK7258_DMA)

typedef struct BK7258DMAChannel {
    uint32_t control;
    uint32_t address[6]; /* destination, source, then their loop bounds */
    uint32_t request;
    uint32_t pause[2];
    uint32_t source;
    uint32_t destination;
    uint32_t remaining;
    uint32_t total;
    uint32_t flags;
    bool half_seen;
} BK7258DMAChannel;

struct BK7258DMAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion *memory;
    AddressSpace as;
    bool as_initialized;
    Clock *clk;
    QEMUTimer *timer;
    qemu_irq irq_ns;
    BK7258DMAChannel channel[8];
    uint32_t global;
    uint8_t unit;
    unsigned next_channel;
    unsigned hz;
    uint64_t cycles;
    int64_t deadline;
    bool phase_valid;
};

#endif
