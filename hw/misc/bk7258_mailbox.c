/*
 * BK7258 MBOX0 v2: three core-owned channels, eight shared message slots.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register facts: pinned SDK mbox0_{struct,ll}.h and mbox0_hal.c.
 * Messages are two opaque words plus source channel. No software protocol,
 * pointer dereference, acknowledgement generation or legacy MBOX1 emulation.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/bk7258_mailbox.h"
#include "hw/core/irq.h"

#define WRERR (1U << 16)
#define RDERR (1U << 17)
#define WRFULL (1U << 18)
#define ERRORS (WRERR | RDERR | WRFULL)
#define CONFIG_MASK 0x0f3f

static unsigned bk7258_mailbox_length(BK7258MailboxChannel *c)
{
    return (c->control >> 1) & 0x3f;
}

static void bk7258_mailbox_update(BK7258MailboxState *s)
{
    s->irq_status = 0;
    for (unsigned i = 0; i < BK7258_MAILBOX_CORES; i++) {
        BK7258MailboxChannel *c = &s->channel[i];
        bool level = (s->global & 1) && (c->control & 1) &&
                     (((c->config & (1U << 8)) && c->count) ||
                      ((c->errors & WRERR) && (c->config & (1U << 9))) ||
                      ((c->errors & RDERR) && (c->config & (1U << 10))) ||
                      ((c->errors & WRFULL) && (c->config & (1U << 11))));

        s->irq_status |= level << i;
        qemu_set_irq(s->irq[i], level);
    }
}

static bool bk7258_mailbox_permitted(BK7258MailboxPort *p, unsigned channel)
{
    return (p->device->global & 4) || p->master == channel;
}

static bool bk7258_mailbox_partition(BK7258MailboxState *s, unsigned channel,
                                    unsigned start, unsigned length)
{
    if (!length || start + length > BK7258_MAILBOX_SLOTS) {
        return false;
    }
    for (unsigned i = 0; i < BK7258_MAILBOX_CORES; i++) {
        BK7258MailboxChannel *other = &s->channel[i];
        unsigned begin = other->config & 0x3f;
        unsigned end = begin + bk7258_mailbox_length(other);

        if (i != channel && ((other->control & 1) || other->count) &&
            start < end && begin < start + length) {
            return false;
        }
    }
    return true;
}

static void bk7258_mailbox_send(BK7258MailboxPort *p, unsigned channel,
                               unsigned destination)
{
    BK7258MailboxState *s = p->device;
    BK7258MailboxChannel *from = &s->channel[channel];
    BK7258MailboxChannel *to;
    BK7258MailboxMessage *message;
    unsigned length;

    from->destination = destination;
    if (!bk7258_mailbox_permitted(p, channel) ||
        destination >= BK7258_MAILBOX_CORES || !(from->control & 1)) {
        from->errors |= WRERR;
        return;
    }
    to = &s->channel[destination];
    length = bk7258_mailbox_length(to);
    if (!(to->control & 1) || !length) {
        /* Unavailable destinations are rejected, never silently delivered. */
        from->errors |= WRERR;
        return;
    }
    if (to->count == length) {
        from->errors |= WRFULL;
        return;
    }
    message = &s->slot[(to->config & 0x3f) +
                       (to->head + to->count) % length];
    message->data[0] = from->tx[0];
    message->data[1] = from->tx[1];
    message->source = channel;
    to->count++;
}

static uint32_t bk7258_mailbox_receive(BK7258MailboxPort *p, unsigned channel)
{
    BK7258MailboxState *s = p->device;
    BK7258MailboxChannel *c = &s->channel[channel];

    if (!bk7258_mailbox_permitted(p, channel)) {
        c->errors |= RDERR;
        return 0xf;
    }
    if (!(s->global & 1) || !(c->control & 1) || !c->count) {
        /* Explicit diagnostic empty-read policy; silicon return is unknown. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bk7258-mailbox: empty/disabled SID read\n");
        return 0xf;
    }
    c->rx = s->slot[(c->config & 0x3f) + c->head];
    c->head = (c->head + 1) % bk7258_mailbox_length(c);
    c->count--;
    return c->rx.source;
}

static MemTxResult bk7258_mailbox_read(void *opaque, hwaddr addr,
                                      uint64_t *value, unsigned size,
                                      MemTxAttrs attrs)
{
    BK7258MailboxPort *p = opaque;
    BK7258MailboxState *s = p->device;
    unsigned channel = (addr - 0x40) / 0x40;
    BK7258MailboxChannel *c;

    switch (addr) {
    case 0x00:
        *value = 0x6d61696c;
        return MEMTX_OK;
    case 0x04:
        *value = 0x00020000;
        return MEMTX_OK;
    case 0x08:
        *value = s->global;
        return MEMTX_OK;
    case 0x0c:
        *value = s->irq_status;
        return MEMTX_OK;
    }
    if (addr < 0x40 || channel >= BK7258_MAILBOX_CORES) {
        goto unimplemented;
    }
    c = &s->channel[channel];
    switch (addr % 0x40) {
    case 0x00:
        *value = c->config | c->errors | (c->count ? 1U << 19 : 0);
        break;
    case 0x04:
        *value = c->control;
        break;
    case 0x08:
    case 0x0c:
        *value = c->tx[(addr % 0x40 - 8) / 4];
        break;
    case 0x10:
        *value = c->destination;
        break;
    case 0x14:
        *value = bk7258_mailbox_receive(p, channel);
        bk7258_mailbox_update(s);
        break;
    case 0x18:
    case 0x1c:
        *value = c->rx.data[(addr % 0x40 - 0x18) / 4];
        break;
    case 0x20:
        *value = (c->count << 2) | (!c->count ? 2 : 0) |
                 (c->count && c->count == bk7258_mailbox_length(c) ? 1 : 0);
        break;
    default:
        goto unimplemented;
    }
    return MEMTX_OK;

unimplemented:
    qemu_log_mask(LOG_UNIMP, "bk7258-mailbox: read offset 0x%" HWADDR_PRIx
                  " is not implemented\n", addr);
    return MEMTX_ERROR;
}

static void bk7258_mailbox_reset(DeviceState *dev)
{
    BK7258MailboxState *s = BK7258_MAILBOX(dev);

    s->global = 0;
    memset(s->channel, 0, sizeof(s->channel));
    memset(s->slot, 0, sizeof(s->slot));
    for (unsigned i = 0; i < BK7258_MAILBOX_CORES; i++) {
        s->channel[i].config = (1U << 8) | (1U << 11);
    }
    bk7258_mailbox_update(s);
}

static MemTxResult bk7258_mailbox_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned size,
                                       MemTxAttrs attrs)
{
    BK7258MailboxPort *p = opaque;
    BK7258MailboxState *s = p->device;
    unsigned channel = (addr - 0x40) / 0x40;
    BK7258MailboxChannel *c;
    unsigned offset = addr % 0x40;
    unsigned start, length;

    if (addr == 8) {
        if (!(value & 1)) {
            bk7258_mailbox_reset(DEVICE(s));
        }
        s->global = value & 7;
        bk7258_mailbox_update(s);
        return MEMTX_OK;
    }
    if (addr < 0x40 || channel >= BK7258_MAILBOX_CORES) {
        goto unimplemented;
    }
    if (!(s->global & 1)) {
        return MEMTX_OK;
    }
    c = &s->channel[channel];
    if (offset == 0x10) {
        bk7258_mailbox_send(p, channel, value & 0xf);
        bk7258_mailbox_update(s);
        return MEMTX_OK;
    }
    if (!bk7258_mailbox_permitted(p, channel)) {
        qemu_log_mask(LOG_UNIMP,
                      "bk7258-mailbox: protected configuration/data write "
                      "by CPU%u to channel%u\n", p->master, channel);
        return MEMTX_ERROR;
    }
    switch (offset) {
    case 0x00:
        start = value & 0x3f;
        if (start != (c->config & 0x3f) && ((c->control & 1) || c->count)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bk7258-mailbox: cannot move an active/nonempty FIFO\n");
            break;
        }
        if (start != (c->config & 0x3f)) {
            c->head = 0;
        }
        c->errors &= ~(value & ERRORS);
        c->config = value & CONFIG_MASK;
        break;
    case 0x04:
        length = (value >> 1) & 0x3f;
        if ((length != bk7258_mailbox_length(c) &&
             ((c->control & 1) || c->count)) ||
            ((value & 1) &&
             !bk7258_mailbox_partition(s, channel, c->config & 0x3f, length))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bk7258-mailbox: invalid or active FIFO partition\n");
            break;
        }
        if (length != bk7258_mailbox_length(c)) {
            c->head = 0;
        }
        /* Disable masks traffic/IRQ but preserves queued messages. */
        c->control = value & 0x7f;
        break;
    case 0x08:
    case 0x0c:
        c->tx[(offset - 8) / 4] = value;
        break;
    default:
        goto unimplemented;
    }
    bk7258_mailbox_update(s);
    return MEMTX_OK;

unimplemented:
    qemu_log_mask(LOG_UNIMP, "bk7258-mailbox: write offset 0x%" HWADDR_PRIx
                  " is not implemented\n", addr);
    return MEMTX_ERROR;
}

static const MemoryRegionOps bk7258_mailbox_ops = {
    .read_with_attrs = bk7258_mailbox_read,
    .write_with_attrs = bk7258_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_mailbox_init(Object *obj)
{
    BK7258MailboxState *s = BK7258_MAILBOX(obj);

    for (unsigned i = 0; i < BK7258_MAILBOX_CORES; i++) {
        g_autofree char *name = g_strdup_printf("bk7258-mailbox-cpu%u", i);

        s->port[i].device = s;
        s->port[i].master = i;
        memory_region_init_io(&s->iomem[i], obj, &bk7258_mailbox_ops,
                              &s->port[i], name, 0x100);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem[i]);
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void bk7258_mailbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bk7258_mailbox_reset);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_mailbox_info = {
    .name = TYPE_BK7258_MAILBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258MailboxState),
    .instance_init = bk7258_mailbox_init,
    .class_init = bk7258_mailbox_class_init,
};

static void bk7258_mailbox_register_types(void)
{
    type_register_static(&bk7258_mailbox_info);
}

type_init(bk7258_mailbox_register_types)
