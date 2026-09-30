/*
 * BK7258 UART functional model (no baud-clock timing or DMA).
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register provenance: docs/system/arm/bk7258.rst.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/char/bk7258_uart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"

#define RX_IRQ (1U << 1)
#define TX_DONE_IRQ (1U << 5)

static void bk7258_uart_update(BK7258UARTState *s)
{
    unsigned threshold = (s->fifo_config >> 8) & 0xff;

    if (s->rx_count && s->rx_count >= MAX(threshold, 1)) {
        s->int_status |= RX_IRQ;
    }
    qemu_set_irq(s->irq, (s->global_ctrl & 1) &&
                 (s->int_status & s->int_enable));
}

static int bk7258_uart_can_receive(void *opaque)
{
    BK7258UARTState *s = opaque;

    if (!(s->global_ctrl & 1) || !(s->config & 2)) {
        return 0;
    }
    return sizeof(s->rx_fifo) - s->rx_count;
}

static void bk7258_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    BK7258UARTState *s = opaque;
    int i;

    for (i = 0; i < size && s->rx_count < sizeof(s->rx_fifo); i++) {
        s->rx_fifo[(s->rx_head + s->rx_count) % sizeof(s->rx_fifo)] = buf[i];
        s->rx_count++;
    }
    bk7258_uart_update(s);
}

static uint64_t bk7258_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    BK7258UARTState *s = opaque;
    uint32_t value;

    switch (offset) {
    case 0x08:
        return s->global_ctrl;
    case 0x10:
        return s->config;
    case 0x14:
        return s->fifo_config;
    case 0x18:
        /* TX is drained synchronously; RX remains a bounded FIFO. */
        return (s->rx_count << 8) | (1U << 17) | (1U << 20) |
               (s->rx_count ? (1U << 21) : (1U << 19)) |
               (s->rx_count == sizeof(s->rx_fifo) ? (1U << 18) : 0);
    case 0x1c:
        if (!s->rx_count) {
            return 0;
        }
        value = s->rx_fifo[s->rx_head] << 8;
        s->rx_head = (s->rx_head + 1) % sizeof(s->rx_fifo);
        s->rx_count--;
        bk7258_uart_update(s);
        qemu_chr_fe_accept_input(&s->chr);
        return value;
    case 0x20:
        return s->int_enable;
    case 0x24:
        return s->int_status;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-uart: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return 0;
    }
}

static void bk7258_uart_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    BK7258UARTState *s = opaque;
    uint8_t ch = value;

    switch (offset) {
    case 0x08:
        s->global_ctrl = value;
        if (!(value & 1)) {
            s->rx_head = s->rx_count = 0;
            s->int_status = 0;
        }
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case 0x10:
        s->config = value;
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case 0x14:
        s->fifo_config = value;
        break;
    case 0x1c:
        if ((s->global_ctrl & 1) && (s->config & 1)) {
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
            s->int_status |= TX_DONE_IRQ;
        }
        break;
    case 0x20:
        if (value & ~(RX_IRQ | TX_DONE_IRQ)) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-uart: interrupt sources 0x%02x unsupported\n",
                          (unsigned)value & ~(RX_IRQ | TX_DONE_IRQ));
        }
        s->int_enable = value & 0xff;
        break;
    case 0x24:
        s->int_status &= ~value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-uart: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return;
    }
    bk7258_uart_update(s);
}

static const MemoryRegionOps bk7258_uart_ops = {
    .read = bk7258_uart_read,
    .write = bk7258_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_uart_reset(DeviceState *dev)
{
    BK7258UARTState *s = BK7258_UART(dev);

    s->global_ctrl = 0;
    s->config = 0;
    s->fifo_config = 0;
    s->int_enable = s->int_status = 0;
    s->rx_head = s->rx_count = 0;
    bk7258_uart_update(s);
}

static void bk7258_uart_realize(DeviceState *dev, Error **errp)
{
    BK7258UARTState *s = BK7258_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr, bk7258_uart_can_receive,
                            bk7258_uart_receive, NULL, NULL, s, NULL, true);
}

static void bk7258_uart_init(Object *obj)
{
    BK7258UARTState *s = BK7258_UART(obj);

    memory_region_init_io(&s->iomem, obj, &bk7258_uart_ops, s,
                          "bk7258-uart", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property bk7258_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", BK7258UARTState, chr),
};

static void bk7258_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_uart_realize;
    device_class_set_legacy_reset(dc, bk7258_uart_reset);
    device_class_set_props(dc, bk7258_uart_properties);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_uart_info = {
    .name = TYPE_BK7258_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258UARTState),
    .instance_init = bk7258_uart_init,
    .class_init = bk7258_uart_class_init,
};

static void bk7258_uart_register_types(void)
{
    type_register_static(&bk7258_uart_info);
}

type_init(bk7258_uart_register_types)
