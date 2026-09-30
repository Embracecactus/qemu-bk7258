/*
 * BK7258 RAM-only nonsecure DMA functional slice.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register facts and deliberately bounded policies: docs/system/arm/bk7258.rst.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/dma/bk7258_dma.h"
#include "system/dma.h"

#define FINISH (1U << 18)
#define HALF (1U << 19)
#define BUS_ERROR (1U << 20)
#define STATUS_FLAGS (FINISH | HALF | BUS_ERROR)
#define BUS_ERROR_ENABLE (1U << 22)
#define CHANNEL_ENABLE 1U
#define CHANNEL_IRQ_ENABLE 6U

static uint32_t bk7258_dma_pending(BK7258DMAState *s)
{
    uint32_t pending = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
        BK7258DMAChannel *c = &s->channel[i];

        if (((c->flags & FINISH) && (c->control & 2)) ||
            ((c->flags & HALF) && (c->control & 4)) ||
            ((c->flags & BUS_ERROR) && (c->request & BUS_ERROR_ENABLE))) {
            pending |= 1U << i;
        }
    }
    return pending;
}

static void bk7258_dma_irq(BK7258DMAState *s)
{
    qemu_set_irq(s->irq_ns, (s->global & 1) && bk7258_dma_pending(s));
}

static bool bk7258_dma_active(BK7258DMAState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
        if (s->channel[i].control & CHANNEL_ENABLE) {
            return true;
        }
    }
    return false;
}

static void bk7258_dma_arm_at(BK7258DMAState *s, int64_t base)
{
    uint64_t delay;

    timer_del(s->timer);
    if (!(s->global & 1) || !s->phase_valid || !s->hz) {
        return;
    }
    delay = DIV_ROUND_UP(s->cycles * NANOSECONDS_PER_SECOND, s->hz);
    if (delay <= INT64_MAX - base) {
        s->deadline = base + delay;
        timer_mod(s->timer, s->deadline);
    }
}

static void bk7258_dma_schedule(BK7258DMAState *s)
{
    if (!bk7258_dma_active(s)) {
        s->phase_valid = false;
        timer_del(s->timer);
        return;
    }
    if (!s->phase_valid) {
        s->cycles = 2; /* Functional read+write beat, not measured bandwidth. */
        s->phase_valid = true;
    }
    if (!timer_pending(s->timer)) {
        bk7258_dma_arm_at(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void bk7258_dma_clock(void *opaque, ClockEvent event)
{
    BK7258DMAState *s = opaque;

    if (event == ClockPreUpdate) {
        if (s->phase_valid && s->hz && timer_pending(s->timer)) {
            int64_t left = MAX(s->deadline -
                               qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 0);

            s->cycles = DIV_ROUND_UP((uint64_t)left * s->hz,
                                     NANOSECONDS_PER_SECOND);
        }
        timer_del(s->timer);
    } else {
        s->hz = clock_get_hz(s->clk);
        bk7258_dma_schedule(s);
    }
}

static void bk7258_dma_beat(BK7258DMAState *s, unsigned index)
{
    BK7258DMAChannel *c = &s->channel[index];
    unsigned width = 1U << ((c->control >> 4) & 3);
    uint8_t data[4];
    MemTxResult result;
    /*
     * These tags are internal model identities, not measured AHB HMASTER IDs.
     * The initial slice permits only nonsecure, unprivileged normal memory.
     * In particular, never inherit CPU0's private address space or identity.
     */
    MemTxAttrs attrs = {
        .secure = false,
        .user = true,
        .memory = true,
        .requester_id = 0x100 + 8 * s->unit + index,
    };

    assert(width <= sizeof(data) && c->remaining >= width);
    WITH_RCU_READ_LOCK_GUARD() {
        result = dma_memory_read(&s->as, c->source, data, width, attrs);
        if (result == MEMTX_OK) {
            result = dma_memory_write(&s->as, c->destination, data, width,
                                       attrs);
        }
    }
    if (result != MEMTX_OK) {
        c->flags |= BUS_ERROR;
        c->control &= ~CHANNEL_ENABLE;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bk7258-dma: unit %u channel %u bus error\n",
                      s->unit, index);
        return;
    }
    c->remaining -= width;
    if (c->control & (1U << 8)) {
        c->source += width;
    }
    if (c->control & (1U << 9)) {
        c->destination += width;
    }
    if (!c->half_seen && c->total - c->remaining >= (c->total + 1) / 2) {
        c->half_seen = true;
        c->flags |= HALF;
    }
    if (!c->remaining) {
        c->flags |= FINISH;
        c->control &= ~CHANNEL_ENABLE;
    }
}

static void bk7258_dma_event(void *opaque)
{
    BK7258DMAState *s = opaque;
    int64_t previous = s->deadline;

    for (unsigned n = 0; n < ARRAY_SIZE(s->channel); n++) {
        unsigned index = (s->next_channel + n) % ARRAY_SIZE(s->channel);

        if (s->channel[index].control & CHANNEL_ENABLE) {
            bk7258_dma_beat(s, index);
            s->next_channel = (index + 1) % ARRAY_SIZE(s->channel);
            break;
        }
    }
    bk7258_dma_irq(s);
    if (bk7258_dma_active(s)) {
        s->cycles = 2;
        /* Preserve elapsed beats when a qtest advances virtual time far. */
        bk7258_dma_arm_at(s, previous);
    } else {
        s->phase_valid = false;
    }
}

static bool bk7258_dma_start(BK7258DMAState *s, BK7258DMAChannel *c,
                             uint32_t value)
{
    unsigned src_width = (value >> 4) & 3;
    unsigned dst_width = (value >> 6) & 3;
    uint32_t total = (value >> 16) + 1;
    unsigned width = 1U << src_width;

    /* Single block, equal widths, no loops/priorities/peripheral requests. */
    if (!(s->global & 1) || (value & 0xfc08) || src_width > 2 ||
        src_width != dst_width || (c->request & ~BUS_ERROR_ENABLE) ||
        c->pause[0] || c->pause[1] || c->flags ||
        ((total | c->address[0] | c->address[1]) & (width - 1))) {
        return false;
    }
    c->source = c->address[1];
    c->destination = c->address[0];
    c->remaining = c->total = total;
    c->half_seen = false;
    return true;
}

static MemTxResult bk7258_dma_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    BK7258DMAState *s = opaque;
    unsigned index, reg;
    BK7258DMAChannel *c;

    switch (offset) {
    case 0x08:
        *value = s->global;
        return MEMTX_OK;
    case 0x10: /* Secure channels are outside this initial slice. */
    case 0x14: /* Privileged channels likewise. */
    case 0x18: /* Secure enabled-pending summary. */
    case 0x28: /* Non-default interrupt allocation is unsupported. */
        *value = 0;
        return MEMTX_OK;
    case 0x1c:
        *value = bk7258_dma_pending(s);
        return MEMTX_OK;
    }
    if (offset < 0x40 || offset >= 0x240) {
        goto unsupported;
    }
    index = (offset - 0x40) / 0x40;
    reg = (offset - 0x40) % 0x40;
    c = &s->channel[index];
    switch (reg) {
    case 0x00:
        *value = c->control;
        break;
    case 0x04 ... 0x18:
        *value = c->address[(reg - 4) / 4];
        break;
    case 0x1c:
        *value = c->request;
        break;
    case 0x20:
    case 0x24:
        *value = c->pause[(reg - 0x20) / 4];
        break;
    case 0x28:
        *value = c->source;
        break;
    case 0x2c:
        *value = c->destination;
        break;
    case 0x30:
        /* Only one unacknowledged event of each type is supported. */
        *value = c->remaining | c->flags |
                 (c->flags & FINISH ? 1U << 24 : 0) |
                 (c->flags & HALF ? 1U << 28 : 0);
        break;
    default:
        goto unsupported;
    }
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-dma: unsupported read at 0x%"
                  HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static MemTxResult bk7258_dma_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    BK7258DMAState *s = opaque;
    unsigned index, reg;
    BK7258DMAChannel *c;

    switch (offset) {
    case 0x08:
        if (value & ~3U) { /* Fixed-priority mode is not modeled yet. */
            goto unsupported;
        }
        s->global = value;
        if (!(value & 1)) {
            memset(s->channel, 0, sizeof(s->channel));
            s->next_channel = 0;
            s->phase_valid = false;
            timer_del(s->timer);
        }
        goto update;
    case 0x10:
    case 0x14:
    case 0x28:
        if (value) {
            goto unsupported; /* Do not pretend security/allocation works. */
        }
        return MEMTX_OK;
    }
    if (offset < 0x40 || offset >= 0x240) {
        goto unsupported;
    }
    index = (offset - 0x40) / 0x40;
    reg = (offset - 0x40) % 0x40;
    c = &s->channel[index];
    if ((c->control & CHANNEL_ENABLE) && reg != 0 && reg != 0x1c &&
        reg != 0x30) {
        goto unsupported;
    }
    switch (reg) {
    case 0x00:
        if ((c->control & value & CHANNEL_ENABLE) &&
            ((c->control ^ value) & ~CHANNEL_IRQ_ENABLE)) {
            goto unsupported;
        }
        if ((value & CHANNEL_ENABLE) && !(c->control & CHANNEL_ENABLE) &&
            !bk7258_dma_start(s, c, value)) {
            goto unsupported;
        }
        c->control = value;
        break;
    case 0x04 ... 0x18:
        c->address[(reg - 4) / 4] = value;
        break;
    case 0x1c:
        if ((c->control & CHANNEL_ENABLE) &&
            ((c->request ^ value) & ~BUS_ERROR_ENABLE)) {
            goto unsupported;
        }
        c->request = value;
        break;
    case 0x20:
    case 0x24:
        c->pause[(reg - 0x20) / 4] = value;
        break;
    case 0x30:
        if (value & ((1U << 17) | (3U << 22))) {
            goto unsupported; /* Flush and repeat-pause are not modeled. */
        }
        /* Status flags are W1C; there is no prefetched source buffer. */
        c->flags &= ~(value & STATUS_FLAGS);
        break;
    default:
        goto unsupported;
    }
update:
    bk7258_dma_irq(s);
    bk7258_dma_schedule(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "bk7258-dma: unsupported write 0x%" PRIx64
                  " at 0x%" HWADDR_PRIx "\n", value, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps bk7258_dma_ops = {
    .read_with_attrs = bk7258_dma_read,
    .write_with_attrs = bk7258_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_dma_reset(DeviceState *dev)
{
    BK7258DMAState *s = BK7258_DMA(dev);

    s->global = 0;
    memset(s->channel, 0, sizeof(s->channel));
    s->next_channel = 0;
    s->phase_valid = false;
    timer_del(s->timer);
    bk7258_dma_irq(s);
}

static void bk7258_dma_realize(DeviceState *dev, Error **errp)
{
    BK7258DMAState *s = BK7258_DMA(dev);

    if (!s->memory || s->unit > 1 || !clock_has_source(s->clk)) {
        error_setg(errp, "BK7258 DMA requires memory, HCLK and unit 0 or 1");
        return;
    }
    address_space_init(&s->as, s->memory, "bk7258-dma-memory");
    s->as_initialized = true;
    s->hz = clock_get_hz(s->clk);
}

static void bk7258_dma_init(Object *obj)
{
    BK7258DMAState *s = BK7258_DMA(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_dma_event, s);
    s->clk = qdev_init_clock_in(DEVICE(obj), "hclk", bk7258_dma_clock, s,
                                ClockPreUpdate | ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &bk7258_dma_ops, s,
                          "bk7258-dma", 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_ns);
}

static void bk7258_dma_finalize(Object *obj)
{
    BK7258DMAState *s = BK7258_DMA(obj);

    timer_free(s->timer);
    if (s->as_initialized) {
        address_space_destroy(&s->as);
    }
}

static const Property bk7258_dma_properties[] = {
    DEFINE_PROP_LINK("dma-memory", BK7258DMAState, memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT8("unit", BK7258DMAState, unit, 0),
};

static void bk7258_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_dma_realize;
    device_class_set_legacy_reset(dc, bk7258_dma_reset);
    device_class_set_props(dc, bk7258_dma_properties);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_dma_info = {
    .name = TYPE_BK7258_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258DMAState),
    .instance_init = bk7258_dma_init,
    .instance_finalize = bk7258_dma_finalize,
    .class_init = bk7258_dma_class_init,
};

static void bk7258_dma_register_types(void)
{
    type_register_static(&bk7258_dma_info);
}

type_init(bk7258_dma_register_types)
