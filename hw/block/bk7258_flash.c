/*
 * BK7258 flash controller and CRC-framed XIP view.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Downstream functional experiment, not an upstream contribution.
 * The SDK exposes 32-byte operations, not raw SPI bytes. Controller-managed
 * WREN and two separate status-byte writes for op7 are explicit inferences
 * from that software-visible contract; serial waveform timing is not modeled.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/block/bk7258_flash.h"
#include "system/system.h"
#include "system/runstate.h"

#define CRC_ENABLE (1U << 26)
#define ALL_FF_OK (1U << 27)
#define DEFAULT_CONFIG (CRC_ENABLE | ALL_FF_OK)
#define TRANSACTION_NS 1000

static bool bk7258_flash_mutating(unsigned operation)
{
    return operation == 4 || operation == 7 ||
           (operation >= 12 && operation <= 16);
}

static int bk7258_flash_status_index(uint32_t commands, bool write)
{
    unsigned shift = write ? 0 : 8;
    unsigned select = write ? 16 : 17;
    unsigned command = commands & (1U << select) ?
                       (commands >> shift) & 0xff : (write ? 1 : 5);

    static const uint8_t opcode[3][2] = {
        { 0x05, 0x01 }, { 0x35, 0x31 }, { 0x15, 0x11 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(opcode); i++) {
        if (command == opcode[i][write]) {
            return i;
        }
    }
    return -ENOTSUP;
}

static int bk7258_flash_execute(BK7258FlashState *s)
{
    uint32_t address = s->pending_address;
    uint16_t status = s->pending_config >> 10;
    int index, ret;

    switch (s->pending_operation) {
    case 1:
        return bk7258_nor_write_enable(s->nor);
    case 2:
        return bk7258_nor_write_disable(s->nor);
    case 3:
        index = bk7258_flash_status_index(s->pending_commands, false);
        return index < 0 ? index :
               bk7258_nor_read_status(s->nor, index, &s->status);
    case 4:
        index = bk7258_flash_status_index(s->pending_commands, true);
        return index < 0 ? index :
               bk7258_nor_write_status(s->nor, index, status);
    case 5:
        ret = bk7258_nor_read(s->nor, address, s->rx, sizeof(s->rx));
        if (!ret) {
            s->rx_words = 8;
            s->rx_index = 0;
        }
        return ret;
    case 6:
        return bk7258_nor_read_status(s->nor, 1, &s->status);
    case 7:
        /* Reject unsupported fields before changing either status byte. */
        ret = bk7258_nor_check_status(s->nor, 0, status);
        if (!ret) {
            ret = bk7258_nor_check_status(s->nor, 1, status >> 8);
        }
        if (!ret) {
            ret = bk7258_nor_write_status(s->nor, 0, status);
        }
        if (!ret) {
            ret = bk7258_nor_write_enable(s->nor);
        }
        return ret ? ret :
               bk7258_nor_write_status(s->nor, 1, status >> 8);
    case 12:
        return bk7258_nor_program(s->nor, address, s->pending_data, 32);
    case 13:
        return bk7258_nor_erase(s->nor, address, 4096);
    case 14:
        return bk7258_nor_erase(s->nor, address, 32768);
    case 15:
        return bk7258_nor_erase(s->nor, address, 65536);
    case 16:
        return bk7258_nor_erase(s->nor, address, bk7258_nor_capacity(s->nor));
    case 20:
        return bk7258_nor_read_id(s->nor, &s->id);
    case 22:
        s->continuous = false;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static void bk7258_flash_complete(void *opaque)
{
    BK7258FlashState *s = opaque;
    int ret;

    if (!s->busy) {
        return;
    }
    bk7258_nor_set_busy(s->nor, false);
    ret = bk7258_flash_execute(s);
    s->busy = false;
    if (ret) {
        qemu_log_mask(ret == -ENOTSUP ? LOG_UNIMP : LOG_GUEST_ERROR,
                      "bk7258-flash: operation %u at 0x%x failed: %s\n",
                      s->pending_operation, s->pending_address, strerror(-ret));
        if (ret != -EACCES) {
            /*
             * No guest-visible error bit is documented for these cases.
             * Stop instead of presenting a successful host-only failure or
             * silently ignoring malformed/unsupported physical operations.
             * Protected NOR writes are the documented no-operation case.
             */
            qemu_system_vmstop_request_prepare();
            qemu_system_vmstop_request(
                bk7258_nor_has_io_error(s->nor) || ret == -EROFS ?
                RUN_STATE_IO_ERROR : RUN_STATE_INTERNAL_ERROR);
        }
    }
}

static MemTxResult bk7258_flash_start(BK7258FlashState *s)
{
    unsigned operation = (s->command >> 24) & 0x1f;
    int ret;

    if (s->busy || !(s->global & 1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bk7258-flash: command while busy or held in reset\n");
        return MEMTX_ERROR;
    }
    if (!((operation >= 1 && operation <= 7) ||
          (operation >= 12 && operation <= 16) || operation == 20 ||
          operation == 22)) {
        qemu_log_mask(LOG_UNIMP, "bk7258-flash: operation %u unsupported\n",
                      operation);
        return MEMTX_ERROR;
    }
    if (operation == 12 && s->tx_words != 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bk7258-flash: page program requires eight TX words\n");
        return MEMTX_ERROR;
    }
    s->pending_operation = operation;
    s->pending_address = s->command & 0xffffff;
    s->pending_config = s->config;
    s->pending_commands = s->command_config;
    if (operation == 5) {
        s->rx_words = s->rx_index = 0;
    }
    if (operation == 12) {
        memcpy(s->pending_data, s->tx, sizeof(s->tx));
        s->tx_words = 0;
    }
    if (bk7258_flash_mutating(operation)) {
        /*
         * SDK program/erase paths omit WREN; controller ownership is inferred.
         */
        ret = bk7258_nor_write_enable(s->nor);
        if (ret) {
            return MEMTX_ERROR;
        }
        bk7258_nor_set_busy(s->nor, true);
    }
    s->busy = true;
    /* Functional transfer latency only, not flash program/erase performance. */
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TRANSACTION_NS);
    return MEMTX_OK;
}

static MemTxResult bk7258_flash_read(void *opaque, hwaddr addr, uint64_t *value,
                                    unsigned size, MemTxAttrs attrs)
{
    BK7258FlashState *s = opaque;

    switch (addr) {
    case 8:
        *value = s->global;
        break;
    case 0x10:
        *value = s->wp | (s->busy ? 1U << 31 : 0);
        break;
    case 0x18:
        if (s->rx_index >= s->rx_words) {
            qemu_log_mask(LOG_GUEST_ERROR, "bk7258-flash: RX FIFO underflow\n");
            return MEMTX_ERROR;
        }
        *value = ldl_le_p(s->rx + 4 * s->rx_index++);
        break;
    case 0x1c:
        *value = s->command_config;
        break;
    case 0x20:
        *value = s->id;
        break;
    case 0x24:
        *value = s->state_config | s->status | (s->crc_errors << 8) |
                 ((s->tx_words & 7) << 16) | ((s->rx_index & 7) << 19);
        break;
    case 0x28:
        *value = s->config;
        break;
    case 0x54:
        *value = s->command;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-flash: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static void bk7258_flash_reset(DeviceState *dev)
{
    BK7258FlashState *s = BK7258_FLASH(dev);

    timer_del(s->timer);
    s->busy = s->continuous = false;
    s->global = s->wp = s->command_config = s->state_config = s->command = 0;
    s->id = s->status = s->crc_errors = 0;
    s->tx_words = s->rx_words = s->rx_index = 0;
    /* Direct XIP entry contract; ROM setup and silicon POR are not modeled. */
    s->config = DEFAULT_CONFIG;
    bk7258_nor_reset(s->nor);
    bk7258_nor_set_wp(s->nor, false);
}

static MemTxResult bk7258_flash_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    BK7258FlashState *s = opaque;

    if (addr == 8) {
        if (!(value & 1)) {
            bk7258_flash_reset(DEVICE(s));
        }
        s->global = value & 3;
        return MEMTX_OK;
    }
    if (s->busy) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bk7258-flash: register write during active transaction\n");
        return MEMTX_ERROR;
    }
    switch (addr) {
    case 0x10:
        s->wp = value & (1U << 30);
        bk7258_nor_set_wp(s->nor, s->wp != 0);
        return value & (1U << 29) ? bk7258_flash_start(s) : MEMTX_OK;
    case 0x14:
        if (s->tx_words == 8) {
            qemu_log_mask(LOG_GUEST_ERROR, "bk7258-flash: TX FIFO overflow\n");
            return MEMTX_ERROR;
        }
        stl_le_p(s->tx + 4 * s->tx_words++, value);
        break;
    case 0x1c:
        s->command_config = value & 0x3ffff;
        break;
    case 0x24:
        if (value & 0xc0000000) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-flash: page-write/OTP mode is unsupported\n");
            return MEMTX_ERROR;
        }
        s->state_config = value & 0x3fc00000;
        break;
    case 0x28:
        if (((value >> 4) & 0x1f) > 2 || (value & (1U << 9)) ||
            !(value & CRC_ENABLE)) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-flash: unsupported line/CPU-write/CRC mode\n");
            return MEMTX_ERROR;
        }
        s->config = value & 0x1ffffdff;
        break;
    case 0x54:
        s->command = value & 0x1fffffff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-flash: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static uint16_t bk7258_flash_crc(const uint8_t *data)
{
    uint16_t crc = 0xffff;

    for (unsigned i = 0; i < 32; i++) {
        crc ^= data[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ (crc & 0x8000 ? 0x8005 : 0);
        }
    }
    return crc;
}

static MemTxResult bk7258_flash_xip_read(void *opaque, hwaddr addr,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    BK7258FlashState *s = opaque;
    const uint8_t *storage = bk7258_nor_storage(s->nor);
    uint64_t last_block = UINT64_MAX;
    uint64_t xip_size = memory_region_size(&s->xip);
    uint8_t status;

    *value = 0;
    if (!storage || addr >= xip_size || size > xip_size - addr) {
        return MEMTX_ERROR;
    }
    if (((s->config >> 4) & 0x1f) == 2) {
        bk7258_nor_read_status(s->nor, 1, &status);
        if (!(status & 2)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bk7258-flash: quad XIP without QE\n");
            return MEMTX_ERROR;
        }
        s->continuous = ((s->state_config >> 26) & 3) == 2;
    }
    /*
     * XIP reads the last committed array, including while a transaction is
     * pending. This functional read-cache abstraction has no physical bus
     * arbitration/timing claim. Commit changes the single backing array.
     */
    for (unsigned i = 0; i < size; i++) {
        uint32_t logical = addr + i;
        uint32_t block = logical / 32;
        const uint8_t *frame = storage + block * 34;

        if (block != last_block) {
            bool erased = true;
            for (unsigned j = 0; j < 34; j++) {
                erased &= frame[j] == 0xff;
            }
            if (!((s->config & ALL_FF_OK) && erased) &&
                bk7258_flash_crc(frame) != lduw_be_p(frame + 32)) {
                s->crc_errors = MIN(s->crc_errors + 1, 255);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bk7258-flash: XIP CRC mismatch at frame 0x%x\n",
                              block * 34);
                return MEMTX_ERROR;
            }
            last_block = block;
        }
        *value |= (uint64_t)frame[logical % 32] << (8 * i);
    }
    return MEMTX_OK;
}

static MemTxResult bk7258_flash_xip_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bk7258-flash: direct XIP writes unsupported\n");
    return MEMTX_ERROR;
}

static const MemoryRegionOps bk7258_flash_ops = {
    .read_with_attrs = bk7258_flash_read,
    .write_with_attrs = bk7258_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps bk7258_xip_ops = {
    .read_with_attrs = bk7258_flash_xip_read,
    .write_with_attrs = bk7258_flash_xip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void bk7258_flash_realize(DeviceState *dev, Error **errp)
{
    BK7258FlashState *s = BK7258_FLASH(dev);

    if (!s->nor || !qdev_is_realized(DEVICE(s->nor))) {
        error_setg(errp, "BK7258 flash controller requires "
                   "a realized NOR link");
        return;
    }
    memory_region_set_size(&s->xip, (bk7258_nor_capacity(s->nor) / 34) * 32);
}

static void bk7258_flash_init(Object *obj)
{
    BK7258FlashState *s = BK7258_FLASH(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bk7258_flash_complete, s);
    memory_region_init_io(&s->regs, obj, &bk7258_flash_ops, s,
                          "bk7258-flash", 0x1000);
    memory_region_init_io(&s->xip, obj, &bk7258_xip_ops, s,
                          "bk7258-flash-xip", 0);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->xip);
}

static void bk7258_flash_finalize(Object *obj)
{
    timer_free(BK7258_FLASH(obj)->timer);
}

static const Property bk7258_flash_properties[] = {
    DEFINE_PROP_LINK("nor", BK7258FlashState, nor,
                     TYPE_BK7258_NOR, BK7258NORState *),
};

static void bk7258_flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_flash_realize;
    device_class_set_props(dc, bk7258_flash_properties);
    device_class_set_legacy_reset(dc, bk7258_flash_reset);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_flash_info = {
    .name = TYPE_BK7258_FLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258FlashState),
    .instance_init = bk7258_flash_init,
    .instance_finalize = bk7258_flash_finalize,
    .class_init = bk7258_flash_class_init,
};

static void bk7258_flash_register_types(void)
{
    type_register_static(&bk7258_flash_info);
}

type_init(bk7258_flash_register_types)
