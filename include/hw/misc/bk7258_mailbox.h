/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_BK7258_MAILBOX_H
#define HW_MISC_BK7258_MAILBOX_H

#include "hw/core/sysbus.h"

#define TYPE_BK7258_MAILBOX "bk7258-mailbox"
#define BK7258_MAILBOX_CORES 3
#define BK7258_MAILBOX_SLOTS 8
OBJECT_DECLARE_SIMPLE_TYPE(BK7258MailboxState, BK7258_MAILBOX)

typedef struct BK7258MailboxMessage {
    uint32_t data[2];
    uint32_t source;
} BK7258MailboxMessage;

typedef struct BK7258MailboxChannel {
    uint32_t config;
    uint32_t control;
    uint32_t errors;
    uint32_t tx[2];
    uint32_t destination;
    BK7258MailboxMessage rx;
    unsigned head;
    unsigned count;
} BK7258MailboxChannel;

typedef struct BK7258MailboxPort {
    BK7258MailboxState *device;
    unsigned master;
} BK7258MailboxPort;

struct BK7258MailboxState {
    SysBusDevice parent_obj;
    MemoryRegion iomem[BK7258_MAILBOX_CORES];
    BK7258MailboxPort port[BK7258_MAILBOX_CORES];
    BK7258MailboxChannel channel[BK7258_MAILBOX_CORES];
    BK7258MailboxMessage slot[BK7258_MAILBOX_SLOTS];
    qemu_irq irq[BK7258_MAILBOX_CORES];
    uint32_t global;
    uint32_t irq_status;
};

#endif
