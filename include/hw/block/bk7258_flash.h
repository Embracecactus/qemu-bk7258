/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_BLOCK_BK7258_FLASH_H
#define HW_BLOCK_BK7258_FLASH_H

#include "hw/core/sysbus.h"
#include "hw/block/bk7258_nor.h"
#include "qemu/timer.h"

#define TYPE_BK7258_FLASH "bk7258-flash"
#define BK7258_XIP_SIZE ((BK7258_NOR_SIZE / 34) * 32)
OBJECT_DECLARE_SIMPLE_TYPE(BK7258FlashState, BK7258_FLASH)

struct BK7258FlashState {
    SysBusDevice parent_obj;
    BK7258NORState nor;
    MemoryRegion regs;
    MemoryRegion xip;
    QEMUTimer *timer;
    uint32_t global;
    uint32_t wp;
    uint32_t command_config;
    uint32_t config;
    uint32_t state_config;
    uint32_t command;
    uint32_t id;
    uint8_t status;
    uint8_t crc_errors;
    uint8_t tx[32];
    uint8_t rx[32];
    unsigned tx_words;
    unsigned rx_words;
    unsigned rx_index;
    bool busy;
    bool continuous;
    unsigned pending_operation;
    uint32_t pending_address;
    uint32_t pending_config;
    uint32_t pending_commands;
    uint8_t pending_data[32];
};

#endif
