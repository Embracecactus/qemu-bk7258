/*
 * BK7258 UART functional model (RX idle timing, no TX wire timing or DMA).
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
#define RX_FINISH_IRQ (1U << 6)
#define SUPPORTED_IRQS (RX_IRQ | TX_DONE_IRQ | RX_FINISH_IRQ)

/* The current board integration uses the SDK's 26 MHz XTAL UART source. */
#define UART_CLOCK_HZ 26000000

static bool bk7258_uart_rx_enabled(BK7258UARTState *s)
{
    return (s->global_ctrl & 1) && (s->config & 2);
}

static void bk7258_uart_update(BK7258UARTState *s)
{
    unsigned threshold = (s->fifo_config >> 8) & 0xff;

    if (s->rx_count && s->rx_count >= MAX(threshold, 1)) {
        s->int_status |= RX_IRQ;
    }
    qemu_set_irq(s->irq, (s->global_ctrl & 1) &&
                 (s->int_status & s->int_enable));
}

static void bk7258_uart_rx_idle(void *opaque)
{
    BK7258UARTState *s = opaque;

    if (bk7258_uart_rx_enabled(s) && s->rx_count) {
        s->int_status |= RX_FINISH_IRQ;
        bk7258_uart_update(s);
    }
}

static void bk7258_uart_schedule_rx_idle(BK7258UARTState *s)
{
    uint64_t divider = ((s->config >> 8) & 0xffff) + 1;
    uint64_t idle_bits = 32U << ((s->fifo_config >> 16) & 3);
    uint64_t delay;

    if (!bk7258_uart_rx_enabled(s) || !s->rx_count) {
        timer_del(s->rx_idle_timer);
        return;
    }

    /*
     * uart_hal_set_baud_rate() programs clk_div = source / baud - 1.
     * uart_struct.h defines RX stop detection as 32, 64, 128 or 256
     * bit-times. Chardev input represents complete received bytes;
     * this timer models the subsequent idle interval, not wire framing.
     */
    delay = DIV_ROUND_UP(idle_bits * divider * NANOSECONDS_PER_SECOND,
                        UART_CLOCK_HZ);
    timer_mod(s->rx_idle_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static int bk7258_uart_can_receive(void *opaque)
{
    BK7258UARTState *s = opaque;

    if (!bk7258_uart_rx_enabled(s)) {
        return 0;
    }
    return sizeof(s->rx_fifo) - s->rx_count;
}

static void bk7258_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    BK7258UARTState *s = opaque;
    int i;

    if (!bk7258_uart_rx_enabled(s)) {
        return;
    }

    for (i = 0; i < size && s->rx_count < sizeof(s->rx_fifo); i++) {
        s->rx_fifo[(s->rx_head + s->rx_count) % sizeof(s->rx_fifo)] = buf[i];
        s->rx_count++;
    }
    if (i) {
        bk7258_uart_schedule_rx_idle(s);
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
        if (!s->rx_count) {
            timer_del(s->rx_idle_timer);
        }
        bk7258_uart_update(s);
        qemu_chr_fe_accept_input(&s->chr);
        return value;
    case 0x20:
        return s->int_enable;
    case 0x24:
        return s->int_status;
    case 0x28:
        return s->flow_config;
    case 0x2c:
        return s->wake_config;
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
            timer_del(s->rx_idle_timer);
            s->rx_head = s->rx_count = 0;
            s->int_status = 0;
        }
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case 0x10:
        s->config = value;
        bk7258_uart_schedule_rx_idle(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case 0x14:
        s->fifo_config = value;
        bk7258_uart_schedule_rx_idle(s);
        break;
    case 0x1c:
        if ((s->global_ctrl & 1) && (s->config & 1)) {
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
            s->int_status |= TX_DONE_IRQ;
        }
        break;
    case 0x20:
        if (value & ~SUPPORTED_IRQS) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-uart: interrupt sources 0x%02x unsupported\n",
                          (unsigned)value & ~SUPPORTED_IRQS);
        }
        s->int_enable = value & 0xff;
        break;
    case 0x24:
        s->int_status &= ~value;
        break;
    case 0x28:
        s->flow_config = value & 0x7ffff;
        if (s->flow_config & (1U << 16)) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-uart: physical RTS/CTS flow control "
                          "is not implemented\n");
        }
        break;
    case 0x2c:
        s->wake_config = value & 0x7fffff;
        if (s->wake_config & (7U << 20)) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-uart: UART sleep/wake signaling "
                          "is not implemented\n");
        }
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

    timer_del(s->rx_idle_timer);
    s->global_ctrl = 0;
    s->config = 0;
    s->fifo_config = 0;
    s->flow_config = s->wake_config = 0;
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

    s->rx_idle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     bk7258_uart_rx_idle, s);
    memory_region_init_io(&s->iomem, obj, &bk7258_uart_ops, s,
                          "bk7258-uart", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void bk7258_uart_finalize(Object *obj)
{
    BK7258UARTState *s = BK7258_UART(obj);

    timer_del(s->rx_idle_timer);
    timer_free(s->rx_idle_timer);
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
    .instance_finalize = bk7258_uart_finalize,
    .class_init = bk7258_uart_class_init,
};

static void bk7258_uart_register_types(void)
{
    type_register_static(&bk7258_uart_info);
}

type_init(bk7258_uart_register_types)
