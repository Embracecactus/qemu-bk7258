/*
 * Bounded GD25WQ64E backend for the downstream BK7258 experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * AI-assisted downstream experiment; not an upstream QEMU contribution.
 *
 * Source: GigaDevice DS-00476-GD25WQ64E-Rev1.2, sections 5, 6,
 * 7.1--7.4, 7.13--7.18, 7.25 and 8.2:
 * https://download.gigadevice.com/Datasheet/DS-00476-GD25WQ64E-Rev1.2.pdf
 * C86517 is an explicit emulated part choice, not a board identity claim.
 * The exact SDK cp/middleware/driver/flash/flash_driver.c C86517 entry
 * uses BP at S6:S2 and CMP at S14. Its "unprotect_last_block" value 0x00e
 * is a BP field value protecting the lower 4 MiB, regardless of its label.
 *
 * No drives: erased in-memory array and delivery status, process-local only.
 * drive: exactly 8 MiB, byte-for-byte raw NOR array; no status/footer bytes.
 * status-drive: one 512-byte block sector, NV SR1/SR2/SR3 at offsets 0..2,
 * with the remaining 509 bytes reserved and zero. QEMU BlockBackend exposes
 * sector-aligned capacity, so the three status bytes need their own sector.
 * Without status-drive, status persists across reset but not process exit.
 * Device migration is deliberately blocked until image/cache reconciliation
 * has a supported migration contract. Physical interrupted-write corruption,
 * electrical timings and endurance are not modeled.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/block/bk7258_nor.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"

#define SR1_WIP  0x01
#define SR1_WEL  0x02
#define SR1_SRP0 0x80
#define SR2_SRP1 0x01
#define SR2_QE   0x02
#define SR2_LB   0x38
#define SR2_CMP  0x40

static const uint8_t status_mask[] = { 0xfc, SR2_QE | SR2_CMP, 0x61 };

static int bk7258_nor_ready(BK7258NORState *s)
{
    if (!s->data) {
        return -ENODEV;
    }
    if (s->io_failed) {
        return -EIO;
    }
    return s->busy ? -EBUSY : 0;
}

static bool bk7258_nor_range_valid(uint32_t addr, size_t len)
{
    return addr < BK7258_NOR_SIZE && len > 0 &&
           len <= BK7258_NOR_SIZE - addr;
}

static int bk7258_nor_write_ready(BK7258NORState *s, BlockBackend *blk)
{
    int ret = bk7258_nor_ready(s);

    if (ret) {
        return ret;
    }
    if (!s->wel) {
        return -EACCES;
    }
    if (blk && !blk_supports_write_perm(blk)) {
        return -EROFS;
    }
    return 0;
}

/* Tables 4 and 5: BP4 selects sectors, BP3 selects bottom, CMP inverts. */
static bool bk7258_nor_protected(BK7258NORState *s, uint32_t addr,
                                uint32_t len)
{
    unsigned bp = (s->status[0] >> 2) & 0x1f;
    unsigned fraction = bp & 7;
    uint32_t size, start, end;

    if (fraction == 0) {
        size = 0;
    } else if (fraction == 7) {
        size = BK7258_NOR_SIZE;
    } else if (bp & 0x10) {
        size = 4096U << MIN(fraction - 1, 3);
    } else {
        size = 65536U << fraction;
    }
    start = (bp & 8) ? 0 : BK7258_NOR_SIZE - size;
    end = start + size;
    if (s->status[1] & SR2_CMP) {
        /* Any requested byte outside the original interval is protected. */
        return addr < start || addr + len > end;
    }
    return addr < end && start < addr + len;
}

/*
 * RAM is changed only after a successful write and flush. Host failures may
 * nevertheless partially alter the image. Latch the error rather than claim
 * a successful commit or continue with an incoherent RAM/image pair.
 */
static int bk7258_nor_persist(BK7258NORState *s, BlockBackend *blk,
                             uint32_t addr, const uint8_t *data, uint32_t len)
{
    int ret;

    if (!blk) {
        return 0;
    }
    /* A synchronous block operation can poll the main loop while waiting. */
    s->busy = true;
    ret = blk_pwrite(blk, addr, len, data, 0);
    if (!ret) {
        ret = blk_flush(blk);
    }
    s->busy = false;
    if (ret < 0) {
        s->io_failed = true;
        error_report("bk7258-nor: persistent %s commit failed: %s; "
                     "image state is uncertain, further array access blocked",
                     blk == s->blk ? "array" : "status", strerror(-ret));
    }
    return ret;
}

int bk7258_nor_read(BK7258NORState *s, uint32_t addr, void *dst, size_t len)
{
    int ret = bk7258_nor_ready(s);

    if (ret) {
        return ret;
    }
    if (!dst || !bk7258_nor_range_valid(addr, len)) {
        return -EINVAL;
    }
    memcpy(dst, s->data + addr, len);
    return 0;
}

int bk7258_nor_write_enable(BK7258NORState *s)
{
    int ret = bk7258_nor_ready(s);

    if (!ret) {
        s->wel = true;
    }
    return ret;
}

int bk7258_nor_write_disable(BK7258NORState *s)
{
    int ret = bk7258_nor_ready(s);

    if (!ret) {
        s->wel = false;
    }
    return ret;
}

int bk7258_nor_read_status(BK7258NORState *s, unsigned index, uint8_t *value)
{
    if (!s->data) {
        return -ENODEV;
    }
    if (!value || index >= ARRAY_SIZE(s->status)) {
        return -EINVAL;
    }
    *value = s->status[index];
    if (index == 0) {
        *value |= (s->wel ? SR1_WEL : 0) | (s->busy ? SR1_WIP : 0);
    }
    return 0;
}

int bk7258_nor_check_status(BK7258NORState *s, unsigned index, uint8_t value)
{
    int ret = bk7258_nor_write_ready(s, s->status_blk);

    if (ret) {
        return ret;
    }
    if (index >= ARRAY_SIZE(s->status)) {
        return -EINVAL;
    }
    if (index == 1 && (value & (SR2_SRP1 | SR2_LB))) {
        return -ENOTSUP;
    }
    if (index == 2 && (value & ~status_mask[2])) {
        return -EINVAL;
    }
    /* QE=1 repurposes WP# as IO2; it no longer locks the status register. */
    if ((s->status[0] & SR1_SRP0) && !(s->status[1] & SR2_QE) &&
        !s->wp_level) {
        return -EACCES;
    }
    return 0;
}

int bk7258_nor_write_status(BK7258NORState *s, unsigned index, uint8_t value)
{
    uint8_t next[BK7258_NOR_STATUS_SIZE] = { 0 };
    int ret = bk7258_nor_check_status(s, index, value);

    if (ret) {
        return ret;
    }
    memcpy(next, s->status, sizeof(s->status));
    next[index] = value & status_mask[index];
    s->wel = false;
    ret = bk7258_nor_persist(s, s->status_blk, 0, next, sizeof(next));
    if (!ret) {
        memcpy(s->status, next, sizeof(s->status));
    }
    return ret;
}

int bk7258_nor_check_program(BK7258NORState *s, uint32_t addr, size_t len)
{
    int ret = bk7258_nor_write_ready(s, s->blk);

    if (ret) {
        return ret;
    }
    if (addr >= BK7258_NOR_SIZE || !len || len > BK7258_NOR_PAGE_SIZE) {
        return -EINVAL;
    }
    if (bk7258_nor_protected(s, addr & ~(BK7258_NOR_PAGE_SIZE - 1),
                             BK7258_NOR_PAGE_SIZE)) {
        return -EACCES;
    }
    return 0;
}

int bk7258_nor_program(BK7258NORState *s, uint32_t addr,
                       const uint8_t *data, size_t len)
{
    uint8_t next[BK7258_NOR_PAGE_SIZE];
    uint32_t base = addr & ~(BK7258_NOR_PAGE_SIZE - 1);
    uint32_t offset = addr & (BK7258_NOR_PAGE_SIZE - 1);
    int ret = bk7258_nor_check_program(s, addr, len);
    size_t i;

    if (ret) {
        return ret;
    }
    if (!data) {
        return -EINVAL;
    }
    memcpy(next, s->data + base, sizeof(next));
    for (i = 0; i < len; i++) {
        next[(offset + i) & (BK7258_NOR_PAGE_SIZE - 1)] &= data[i];
    }
    s->wel = false;
    ret = bk7258_nor_persist(s, s->blk, base, next, sizeof(next));
    if (!ret) {
        memcpy(s->data + base, next, sizeof(next));
    }
    return ret;
}

int bk7258_nor_check_erase(BK7258NORState *s, uint32_t addr, uint32_t size)
{
    int ret = bk7258_nor_write_ready(s, s->blk);

    if (ret) {
        return ret;
    }
    switch (size) {
    case 4096:
    case 32768:
    case 65536:
        break;
    case BK7258_NOR_SIZE:
        if (addr != 0) {
            return -EINVAL;
        }
        break;
    default:
        return -EINVAL;
    }
    if (addr >= BK7258_NOR_SIZE) {
        return -EINVAL;
    }
    addr &= ~(size - 1);
    if (bk7258_nor_protected(s, addr, size)) {
        return -EACCES;
    }
    return 0;
}

int bk7258_nor_erase(BK7258NORState *s, uint32_t addr, uint32_t size)
{
    g_autofree uint8_t *next = NULL;
    int ret = bk7258_nor_check_erase(s, addr, size);

    if (ret) {
        return ret;
    }
    addr &= ~(size - 1);
    next = g_malloc(size);
    memset(next, 0xff, size);
    s->wel = false;
    ret = bk7258_nor_persist(s, s->blk, addr, next, size);
    if (!ret) {
        memcpy(s->data + addr, next, size);
    }
    return ret;
}

bool bk7258_nor_is_busy(const BK7258NORState *s)
{
    return s->busy;
}

void bk7258_nor_set_busy(BK7258NORState *s, bool busy)
{
    s->busy = busy;
}

bool bk7258_nor_has_io_error(const BK7258NORState *s)
{
    return s->io_failed;
}

void bk7258_nor_set_wp(BK7258NORState *s, bool high)
{
    s->wp_level = high;
}

void bk7258_nor_reset(BK7258NORState *s)
{
    s->wel = false;
    s->busy = false;
    /* Array, NV status, external WP# level and host I/O failure survive. */
}

const uint8_t *bk7258_nor_storage(const BK7258NORState *s)
{
    return s->io_failed ? NULL : s->data;
}

static void bk7258_nor_wp(void *opaque, int n, int level)
{
    bk7258_nor_set_wp(opaque, !!level);
}

static void bk7258_nor_device_reset(DeviceState *dev)
{
    bk7258_nor_reset(BK7258_NOR(dev));
}

static bool bk7258_nor_load(BlockBackend *blk, const char *name,
                            uint8_t *data, uint32_t size, Error **errp)
{
    uint64_t perm = BLK_PERM_CONSISTENT_READ;
    int64_t length;
    int ret;

    if (blk_supports_write_perm(blk)) {
        perm |= BLK_PERM_WRITE;
    }
    /* Cached contents require exclusive write and resize ownership. */
    ret = blk_set_perm(blk, perm, BLK_PERM_CONSISTENT_READ, errp);
    if (ret < 0) {
        return false;
    }
    length = blk_getlength(blk);
    if (length < 0) {
        error_setg_errno(errp, -length, "bk7258-nor: cannot size %s", name);
        return false;
    }
    if (length != size) {
        error_setg(errp, "bk7258-nor: %s must be exactly %u bytes "
                   "(got %" PRId64 ")", name, size, length);
        return false;
    }
    ret = blk_pread(blk, 0, size, data, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "bk7258-nor: cannot read %s", name);
        return false;
    }
    return true;
}

static void bk7258_nor_realize(DeviceState *dev, Error **errp)
{
    BK7258NORState *s = BK7258_NOR(dev);
    g_autofree uint8_t *data = g_malloc(BK7258_NOR_SIZE);
    uint8_t status[BK7258_NOR_STATUS_SIZE] = { 0, 0, 0x20 };
    unsigned i;

    if (s->blk) {
        if (!bk7258_nor_load(s->blk, "drive", data, BK7258_NOR_SIZE, errp)) {
            return;
        }
    } else {
        memset(data, 0xff, BK7258_NOR_SIZE);
    }
    if (s->status_blk &&
        !bk7258_nor_load(s->status_blk, "status-drive", status,
                         sizeof(status), errp)) {
        return;
    }
    for (i = 0; i < ARRAY_SIZE(s->status); i++) {
        if (status[i] & ~status_mask[i]) {
            error_setg(errp, "bk7258-nor: status-drive SR%u contains "
                       "unsupported or volatile bits: 0x%02x", i + 1,
                       status[i] & ~status_mask[i]);
            return;
        }
    }
    for (i = ARRAY_SIZE(s->status); i < sizeof(status); i++) {
        if (status[i] != 0) {
            error_setg(errp, "bk7258-nor: status-drive reserved padding "
                       "must be zero (byte %u is 0x%02x)", i, status[i]);
            return;
        }
    }
    s->data = g_steal_pointer(&data);
    memcpy(s->status, status, sizeof(s->status));
    s->io_failed = false;
    bk7258_nor_reset(s);
}

static void bk7258_nor_init(Object *obj)
{
    BK7258NORState *s = BK7258_NOR(obj);

    s->wp_level = true;
    qdev_init_gpio_in_named(DEVICE(obj), bk7258_nor_wp, "WP#", 1);
}

static void bk7258_nor_finalize(Object *obj)
{
    g_free(BK7258_NOR(obj)->data);
}

static void bk7258_nor_unrealize(DeviceState *dev)
{
    BK7258NORState *s = BK7258_NOR(dev);

    g_clear_pointer(&s->data, g_free);
    bk7258_nor_reset(s);
}

static const Property bk7258_nor_properties[] = {
    DEFINE_PROP_DRIVE("drive", BK7258NORState, blk),
    DEFINE_PROP_DRIVE("status-drive", BK7258NORState, status_blk),
};

static const VMStateDescription bk7258_nor_vmstate = {
    .name = TYPE_BK7258_NOR,
    .unmigratable = true,
};

static void bk7258_nor_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_nor_realize;
    dc->unrealize = bk7258_nor_unrealize;
    dc->vmsd = &bk7258_nor_vmstate;
    dc->desc = "Downstream GD25WQ64E NOR backend (C86517, 8 MiB)";
    device_class_set_props(dc, bk7258_nor_properties);
    device_class_set_legacy_reset(dc, bk7258_nor_device_reset);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo bk7258_nor_info = {
    .name = TYPE_BK7258_NOR,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BK7258NORState),
    .instance_init = bk7258_nor_init,
    .instance_finalize = bk7258_nor_finalize,
    .class_init = bk7258_nor_class_init,
};

static void bk7258_nor_register_types(void)
{
    type_register_static(&bk7258_nor_info);
}

type_init(bk7258_nor_register_types)
