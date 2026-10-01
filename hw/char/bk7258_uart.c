/*
 * BK7258 UART FIFO/character timing model (no pad bitstream or DMA).
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register provenance: docs/system/arm/bk7258.rst.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/bk7258_clock.h"
#include "hw/char/bk7258_uart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties-system.h"

#define TX_IRQ 1U
#define RX_IRQ (1U << 1)
#define TX_DONE_IRQ (1U << 5)
#define RX_FINISH_IRQ (1U << 6)
#define SUPPORTED_IRQS (TX_IRQ | RX_IRQ | TX_DONE_IRQ | RX_FINISH_IRQ)

static bool bk7258_uart_tx_enabled(BK7258UARTState *s)
{
    return s->hz && (s->global_ctrl & 1) && (s->config & 1);
}

static bool bk7258_uart_rx_enabled(BK7258UARTState *s)
{
    return s->hz && (s->global_ctrl & 1) && (s->config & 2);
}

static void bk7258_uart_update(BK7258UARTState *s)
{
    unsigned threshold = (s->fifo_config >> 8) & 0xff;

    /* The SDK describes TX service as count below the selected threshold. */
    if (bk7258_uart_tx_enabled(s) && s->tx_count < (s->fifo_config & 0xff)) {
        s->int_status |= TX_IRQ;
    }

    if (s->rx_count && s->rx_count >= MAX(threshold, 1)) {
        s->int_status |= RX_IRQ;
    }
    qemu_set_irq(s->irq, (s->global_ctrl & 1) &&
                 (s->int_status & s->int_enable));
}

static void bk7258_uart_pause_tx(BK7258UARTState *s)
{
    if (s->tx_active && s->hz && timer_pending(s->tx_timer)) {
        s->tx_cycles = bk7258_remaining_cycles(
            s->tx_cycles, s->hz, s->tx_deadline,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    timer_del(s->tx_timer);
}

static void bk7258_uart_start_tx(BK7258UARTState *s, int64_t base)
{
    uint64_t delay;

    if (!bk7258_uart_tx_enabled(s) || timer_pending(s->tx_timer)) {
        return;
    }
    if (!s->tx_active) {
        unsigned data_bits = 5 + ((s->config >> 3) & 3);
        unsigned frame_bits = 1 + data_bits + ((s->config >> 5) & 1) +
                              1 + ((s->config >> 7) & 1);
        uint64_t divider = ((s->config >> 8) & 0xffff) + 1;

        if (!s->tx_count) {
            return;
        }
        /* FIFO occupancy excludes this separate, currently shifting byte. */
        s->tx_byte = s->tx_fifo[s->tx_head] & ((1U << data_bits) - 1);
        s->tx_head = (s->tx_head + 1) % sizeof(s->tx_fifo);
        s->tx_count--;
        s->tx_cycles = frame_bits * divider;
        s->tx_active = true;
    }
    delay = DIV_ROUND_UP(s->tx_cycles * NANOSECONDS_PER_SECOND, s->hz);
    if (delay <= INT64_MAX - base) {
        s->tx_deadline = base + delay;
        timer_mod(s->tx_timer, s->tx_deadline);
    }
}

static void bk7258_uart_tx_complete(void *opaque)
{
    BK7258UARTState *s = opaque;
    int64_t previous = s->tx_deadline;

    assert(s->tx_active && bk7258_uart_tx_enabled(s));
    qemu_chr_fe_write_all(&s->chr, &s->tx_byte, 1);
    s->tx_active = false;
    s->tx_cycles = 0;
    if (s->tx_count) {
        /* Catch up a bounded queue when qtest jumps virtual time forward. */
        bk7258_uart_start_tx(s, previous);
    } else {
        s->int_status |= TX_DONE_IRQ;
    }
    bk7258_uart_update(s);
}

static void bk7258_uart_clear_tx(BK7258UARTState *s)
{
    timer_del(s->tx_timer);
    s->tx_active = false;
    s->tx_cycles = 0;
    s->tx_head = s->tx_count = 0;
}

static void bk7258_uart_clear_rx(BK7258UARTState *s)
{
    timer_del(s->rx_idle_timer);
    s->idle_armed = false;
    s->idle_cycles = 0;
    s->rx_head = s->rx_count = 0;
}

static void bk7258_uart_rx_idle(void *opaque)
{
    BK7258UARTState *s = opaque;

    s->idle_armed = false;
    s->idle_cycles = 0;
    if (bk7258_uart_rx_enabled(s) && s->rx_count) {
        s->int_status |= RX_FINISH_IRQ;
        bk7258_uart_update(s);
    }
}

static void bk7258_uart_arm_idle(BK7258UARTState *s)
{
    uint64_t delay;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    timer_del(s->rx_idle_timer);
    if (s->idle_armed && s->hz) {
        delay = DIV_ROUND_UP(s->idle_cycles * NANOSECONDS_PER_SECOND, s->hz);
        if (delay <= INT64_MAX - now) {
            s->idle_deadline = now + delay;
            timer_mod(s->rx_idle_timer, s->idle_deadline);
        }
    }
}

static void bk7258_uart_schedule_rx_idle(BK7258UARTState *s)
{
    uint64_t divider = ((s->config >> 8) & 0xffff) + 1;
    uint64_t idle_bits = 32U << ((s->fifo_config >> 16) & 3);

    if (!(s->global_ctrl & 1) || !(s->config & 2) || !s->rx_count) {
        timer_del(s->rx_idle_timer);
        s->idle_armed = false;
        s->idle_cycles = 0;
        return;
    }

    /*
     * uart_hal_set_baud_rate() programs clk_div = source / baud - 1.
     * uart_struct.h defines RX stop detection as 32, 64, 128 or 256
     * bit-times. Chardev input represents complete received bytes;
     * this timer models the subsequent idle interval, not wire framing.
     */
    s->idle_cycles = idle_bits * divider;
    s->idle_armed = true;
    bk7258_uart_arm_idle(s);
}

static void bk7258_uart_clock(void *opaque, ClockEvent event)
{
    BK7258UARTState *s = opaque;

    if (event == ClockPreUpdate) {
        bk7258_uart_pause_tx(s);
        if (s->idle_armed && s->hz && timer_pending(s->rx_idle_timer)) {
            s->idle_cycles = bk7258_remaining_cycles(
                s->idle_cycles, s->hz, s->idle_deadline,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
    } else {
        s->hz = clock_get_hz(s->clk);
        bk7258_uart_arm_idle(s);
        bk7258_uart_start_tx(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        bk7258_uart_update(s);
        qemu_chr_fe_accept_input(&s->chr);
    }
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
    unsigned data_bits = 5 + ((s->config >> 3) & 3);
    int i;

    if (!bk7258_uart_rx_enabled(s)) {
        return;
    }

    for (i = 0; i < size && s->rx_count < sizeof(s->rx_fifo); i++) {
        /* Store the complete character at its acceptance-time data width. */
        s->rx_fifo[(s->rx_head + s->rx_count) % sizeof(s->rx_fifo)] =
            buf[i] & ((1U << data_bits) - 1);
        s->rx_count++;
    }
    if (i) {
        bk7258_uart_schedule_rx_idle(s);
    }
    bk7258_uart_update(s);
}

static MemTxResult bk7258_uart_read(void *opaque, hwaddr offset,
                                   uint64_t *result, unsigned size,
                                   MemTxAttrs attrs)
{
    BK7258UARTState *s = opaque;
    uint32_t value;

    switch (offset) {
    case 0x08:
        *result = s->global_ctrl;
        break;
    case 0x10:
        *result = s->config;
        break;
    case 0x14:
        *result = s->fifo_config;
        break;
    case 0x18:
        *result = s->tx_count | (s->rx_count << 8) |
                  (s->tx_count ? 0 : (1U << 17)) |
                  (s->tx_count == sizeof(s->tx_fifo) ? (1U << 16) : 0) |
                  (bk7258_uart_tx_enabled(s) &&
                   s->tx_count < sizeof(s->tx_fifo) ? (1U << 20) : 0) |
                  (s->rx_count ? (1U << 21) : (1U << 19)) |
                  (s->rx_count == sizeof(s->rx_fifo) ? (1U << 18) : 0);
        break;
    case 0x1c:
        if (!s->rx_count) {
            *result = 0;
            break;
        }
        value = s->rx_fifo[s->rx_head] << 8;
        s->rx_head = (s->rx_head + 1) % sizeof(s->rx_fifo);
        s->rx_count--;
        if (!s->rx_count) {
            timer_del(s->rx_idle_timer);
            s->idle_armed = false;
            s->idle_cycles = 0;
        }
        bk7258_uart_update(s);
        qemu_chr_fe_accept_input(&s->chr);
        *result = value;
        break;
    case 0x20:
        *result = s->int_enable;
        break;
    case 0x24:
        *result = s->int_status;
        break;
    case 0x28:
        *result = s->flow_config;
        break;
    case 0x2c:
        *result = s->wake_config;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-uart: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bk7258_uart_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    BK7258UARTState *s = opaque;

    switch (offset) {
    case 0x08:
        s->global_ctrl = value;
        if (!(value & 1)) {
            bk7258_uart_clear_tx(s);
            bk7258_uart_clear_rx(s);
            s->int_status = 0;
        }
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case 0x10: {
        uint32_t changed = s->config ^ value;

        /*
         * uart_hal_flush_fifo() toggles each enable off then on. Model this
         * as a directional flush, with byte-atomic TX cancellation; physical
         * partial-wire behavior is outside the stream abstraction. The HAL
         * does not acknowledge status, so preserve existing IRQ latches.
         */
        if (s->config & ~value & 1) {
            bk7258_uart_clear_tx(s);
        }
        if (s->config & ~value & 2) {
            bk7258_uart_clear_rx(s);
        }
        s->config = value;
        bk7258_uart_start_tx(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        if (changed & ~1U) {
            bk7258_uart_schedule_rx_idle(s);
        }
        qemu_chr_fe_accept_input(&s->chr);
        break;
    }
    case 0x14:
        s->fifo_config = value;
        bk7258_uart_schedule_rx_idle(s);
        break;
    case 0x1c:
        if (bk7258_uart_tx_enabled(s)) {
            if (s->tx_count == sizeof(s->tx_fifo)) {
                qemu_log_mask(LOG_GUEST_ERROR, "bk7258-uart: TX FIFO full\n");
                return MEMTX_ERROR;
            }
            s->tx_fifo[(s->tx_head + s->tx_count) % sizeof(s->tx_fifo)] = value;
            s->tx_count++;
            bk7258_uart_start_tx(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
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
        return MEMTX_ERROR;
    }
    bk7258_uart_update(s);
    return MEMTX_OK;
}

static const MemoryRegionOps bk7258_uart_ops = {
    .read_with_attrs = bk7258_uart_read,
    .write_with_attrs = bk7258_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_uart_reset(DeviceState *dev)
{
    BK7258UARTState *s = BK7258_UART(dev);

    bk7258_uart_clear_tx(s);
    bk7258_uart_clear_rx(s);
    s->global_ctrl = 0;
    s->config = 0;
    s->fifo_config = 0;
    s->flow_config = s->wake_config = 0;
    s->int_enable = s->int_status = 0;
    bk7258_uart_update(s);
}

static void bk7258_uart_realize(DeviceState *dev, Error **errp)
{
    BK7258UARTState *s = BK7258_UART(dev);

    s->hz = clock_get_hz(s->clk);
    qemu_chr_fe_set_handlers(&s->chr, bk7258_uart_can_receive,
                            bk7258_uart_receive, NULL, NULL, s, NULL, true);
}

static void bk7258_uart_init(Object *obj)
{
    BK7258UARTState *s = BK7258_UART(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "pclk", bk7258_uart_clock, s,
                                ClockPreUpdate | ClockUpdate);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                               bk7258_uart_tx_complete, s);
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
    timer_free(s->tx_timer);
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
