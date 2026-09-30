/*
 * Bounded GD25WQ64E backend for the downstream BK7258 experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * AI-assisted downstream experiment; not an upstream QEMU contribution.
 */
#ifndef HW_BLOCK_BK7258_NOR_H
#define HW_BLOCK_BK7258_NOR_H

#include "hw/core/qdev.h"

#define TYPE_BK7258_NOR "bk7258-nor"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258NORState, BK7258_NOR)

#define BK7258_NOR_SIZE        (8U * 1024U * 1024U)
#define BK7258_NOR_PAGE_SIZE   256U
#define BK7258_NOR_JEDEC_ID    0xc86517U
#define BK7258_NOR_STATUS_SIZE 512U

/*
 * Optional "drive": exactly BK7258_NOR_SIZE bytes of raw array data.
 * Optional "status-drive": one BK7258_NOR_STATUS_SIZE-byte block sector,
 * with NV SR1/SR2/SR3 at offsets 0/1/2 and zero reserved padding at 3..511.
 * The complete status sector is validated on load and persisted on writes.
 * Default delivery status image starts 00 00 20, followed by 509 zero bytes.
 * These geometries refer to the capacities exposed by BlockBackend.
 */

struct BK7258NORState {
    DeviceState parent_obj;
    BlockBackend *blk;
    BlockBackend *status_blk;
    uint8_t *data;
    uint8_t status[3];
    bool wel;
    bool busy;
    bool wp_level;
    bool io_failed;
};

/*
 * All helpers run under the BQL. There is no SPI parser or internal timer.
 * The controller owns command decoding, quad-mode eligibility and timing:
 * assert busy during an operation, clear it immediately before committing
 * with a mutation helper, and propagate that helper's result to the guest.
 * An accepted mutation consumes WEL, including a failed persistent commit.
 * Reset the controller's pending operation before resetting this device.
 *
 * Return 0 on success or a negative errno: -EBUSY (WIP), -EACCES (WEL or
 * protection), -EROFS (read-only backing), -EINVAL (bounds/shape/index),
 * -ENOTSUP (unsupported status mode), -ENODEV (not realized), or a backend
 * I/O error. A host I/O failure is latched: no subsequent array access or
 * mutation can succeed, even after reset. Reads of status remain possible.
 *
 * NV status writes use one byte, index 0/1/2 = SPI 01h/31h/11h. A controller
 * 16-bit status operation must explicitly perform two separate commands,
 * with its own WREN for each; it is not a two-byte SPI 31h operation.
 * SRP1 special-order modes and security-register OTP locks are unsupported.
 * Volatile-SR-write enable (50h), OTP/security access, suspend/resume, SFDP,
 * unique ID, deep power-down and raw serial framing are outside this API;
 * the caller must reject such commands, never report fabricated success.
 */
int bk7258_nor_read(BK7258NORState *s, uint32_t addr, void *dst, size_t len);
/* Concrete memory-part metadata, not SoC identity or host-image geometry. */
uint32_t bk7258_nor_capacity(const BK7258NORState *s);
int bk7258_nor_read_id(BK7258NORState *s, uint32_t *id);
int bk7258_nor_write_enable(BK7258NORState *s);
int bk7258_nor_write_disable(BK7258NORState *s);
int bk7258_nor_read_status(BK7258NORState *s, unsigned index, uint8_t *value);
int bk7258_nor_write_status(BK7258NORState *s, unsigned index, uint8_t value);

/*
 * Program 1..256 bytes, ANDing with the array. Crossing a page boundary
 * wraps within that 256-byte page, as on the part. Larger streams are
 * explicitly rejected instead of silently truncating them.
 */
int bk7258_nor_program(BK7258NORState *s, uint32_t addr,
                       const uint8_t *data, size_t len);

/*
 * size is 4096, 32768, 65536 or BK7258_NOR_SIZE. Sector/block addresses
 * select the containing erase unit; chip erase requires addr == 0.
 */
int bk7258_nor_erase(BK7258NORState *s, uint32_t addr, uint32_t size);

/* Non-mutating preflight; the same validation is repeated at commit. */
int bk7258_nor_check_status(BK7258NORState *s, unsigned index, uint8_t value);
int bk7258_nor_check_program(BK7258NORState *s, uint32_t addr, size_t len);
int bk7258_nor_check_erase(BK7258NORState *s, uint32_t addr, uint32_t size);

bool bk7258_nor_is_busy(const BK7258NORState *s);
void bk7258_nor_set_busy(BK7258NORState *s, bool busy);
bool bk7258_nor_has_io_error(const BK7258NORState *s);
void bk7258_nor_set_wp(BK7258NORState *s, bool high);
void bk7258_nor_reset(BK7258NORState *s);

/*
 * Read-only controller/XIP-cache maintenance view, exactly BK7258_NOR_SIZE
 * bytes. NULL before realization or after an I/O failure. Bypasses WIP;
 * never expose this as a guest read without the controller's access checks.
 */
const uint8_t *bk7258_nor_storage(const BK7258NORState *s);

#endif
