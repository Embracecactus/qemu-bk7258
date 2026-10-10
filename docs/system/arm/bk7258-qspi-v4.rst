BK7258 V4: bounded QSPI indirect PIO
==================================

This is a downstream functional experiment, not an upstream contribution or
physical board qualification. The original boot Flash controller, NOR mapping
and CRC-XIP input are unchanged. Product CP/BL1 evidence from V1/V2 is reused;
QSPI PIO success is not product boot, ROM entry, or R7A support.

Sources and applicability
-------------------------

The read-only AP SDK is pinned to
``cb080de1655d579c7593ecf504c440997c4c137b``. Source paths and raw-byte SHA256
values are in ``bk7258-qspi-v4-evidence/sources.json``. No SDK build is needed.

* ``ap/middleware/driver/qspi/qspi_driver.c`` and
  ``ap/middleware/soc/common/hal/qspi_hal.c`` establish the driver/HAL route.
  The BK7258 AP defconfig enables QSPI and the Flash helper. The line-mode
  Kconfig default is quad; our single-line guest deliberately exercises the
  supported LL subset, not that entire configured Flash application.
* ``ap/middleware/soc/bk7258_ap/hal/qspi_ll.h`` and
  ``ap/middleware/soc/bk7258_ap/soc/qspi_struct.h`` establish CMDC writes,
  CMDD reads, increasing-address PIO access, and polling/clear of bit 2.
  The LL's empty interrupt helpers do not establish an IRQ contract.
* ``ap/include/soc/bk7258/{reg_base,qspi_cap}.h`` establish two controllers:
  QSPI0 at 0x46040000 and QSPI1 at 0x46060000. Each has an independent native
  SSI bus (``qspi0``, ``qspi1``), registers, buffer, timer and clock.
  The SoC's existing nonsecure alias convention adds 0x10000000.
* ``sys_hal.c``, ``sys_ll.h`` and ``sys_types.h`` in the BK7258 AP HAL route
  QSPI0 source/divider through SYS+0x24[10:6], QSPI1 through SYS+0x28[10:6],
  and gates through SYS+0x30[20:21]. Selector 1 denotes the 480 MHz source.
* ``ap/middleware/driver/lcd/{lcd_qspi_driver,lcd_spi_driver}.c`` independently
  select source 480 MHz, SYS divider 9 and controller divider 0 for a named
  48 MHz configuration. This verifies a controller clock setting, not LCD
  functionality. V4 supplies only this finite setting, not a general divider
  formula. ``qspi_flash.c`` uses a different 480/4/2 setting, left unsupported.

The Beken (博通集成, not Broadcom BCM) AP BK7258 v3.1.1
`QSPI guide <https://docs.riselink.ai/arminodoc/bk_avdk_smp/ap_doc/bk7258/en/v3.1.1/developer-guide/peripheral/bk_qspi.html>`_
corroborates the C/D directions and programmable frame stages. Its 256-byte
capacity description differs from the fixed SDK's 61-word array. V4 does not
resolve that difference. The companion
`example guide <https://docs.riselink.ai/arminodoc/bk_avdk_smp/ap_doc/bk7258/en/v3.1.1/examples/peripheral/bk_qspi.html>`_
describes divider-zero bypass, consistent with the selected 48 MHz tuple, but
also describes a newer frequency-selection helper absent from the fixed SDK.
Do not silently substitute that newer code for the pinned implementation.
Both pages were readable through search-index extraction; direct web open and
HTML download timed out. No downloaded-HTML hash is claimed.

Register and transaction contract
---------------------------------

Only aligned 32-bit accesses are supported. Unimplemented offsets and modes
return MEMTX_ERROR, producing a precise guest bus fault; there is no unknown
MMIO success fallback.

* CMDC is +0x40..+0x4c; CMDD is +0x50..+0x5c. Each bank contains low command,
  high command, CFG1 and CFG2. The LL's CFG1 values 0xc, 0x30, 0xc0, 0x300
  select one through four single-line bytes, respectively. Bytes are taken
  from successive low-to-high bytes of CMD_HIGH. Thus a normal 24-bit-address
  command is opcode, address[23:16], address[15:8], address[7:0]. CMD_LOW is
  stored, but configurations requiring command bytes 5..8 are rejected.
  No opcode is decoded by the controller.
* CFG2 START[0] captures the supported request. DATA_LEN[11:2] is 0..32 for C,
  1..32 for D. Zero describes a command-only frame, e.g. WREN; the native
  target owns the meaning of its opcode. DATA_LINE[15:14], DUMMY_CLK[22:16]
  and DUMMY_MODE[26:24] must be zero; reserved bits are rejected.
* CONFIG+0x60 supports enable[0] and IO2/3-mode[3] (stored, without pinmux).
  Only mode 0, MSB first, clk_rate 0 and ordinary automatic CS are supported.
  Busy configuration/command/buffer writes are rejected, except disable.
* +0x100..+0x120 is a nine-word, increasing-address PIO/guard window. The
  transferable prefix is 32 bytes; the ninth word enables a tail sentinel.
  This bounded window is not a claim about complete FIFO capacity. Word
  loads/stores do not pop a FIFO. A read updates exactly DATA_LEN bytes,
  preserving padding and guard bytes. Writes serialize exactly DATA_LEN.
* STATUS+0x70 exposes only ``cmd_start_done`` bit 2. STATUS_CLR+0x6c accepts
  bit 2, then zero, as in the LL. Readback of CLR is zero. Other clear bits
  and INT_EN accesses are unsupported; no QSPI IRQ is advertised or wired.
  The other done/busy/status flags have no implemented contract.

Explicit model abstractions: START stays set while the accepted command is
pending, clears on completion/cancellation, and bit 2 latches only after the
last byte and CS release. Clear completion before a new START; starting with
uncleared completion is rejected. These are bounded software-service rules,
not measured silicon busy/START waveforms. One request per controller is
allowed. There is no queue, DMA, XIP, quad, or command-bank chaining.

Clock, CS, cancellation and reset
---------------------------------

Each controller receives a QEMU Clock. The 48 MHz setting is available only
with its SYS gate enabled and the existing committed SYS analog[5].EN_DPLL
bit set. Requesting analog enable alone is not sufficient. As with SYS V1,
this is nominal digital timing, not a PLL-ready/lock simulation. Unknown
source/divider tuples produce no clock; CONFIG's unverified local dividers
are rejected. SYS+0x24 preserves the existing Flash fields and rejects all
remaining unimplemented fields.

One virtual timer expiry transfers one real SSI byte (eight clock cycles).
Clock loss retains remaining cycles, position and selected target; restoring
the clock continues without resending earlier bytes. CS becomes active on
the first byte and stays active through command/address/data. It is released
at completion or cancellation. Initial CS is explicitly inactive.

Disable and QEMU device reset delete the timer and release CS. They cannot
undo bytes already accepted by the external device. QEMU system reset also
clears controller registers/buffer/completion. This is the emulator reset
contract, not evidence for silicon power/retention/core/global reset domains.
The ambiguous local soft-reset/FIFO-reset controls remain unsupported.
Migration is explicitly blocked rather than silently losing active state.

Each bus accepts one low-active CS0 native endpoint. Tests explicitly attach
``w25q32``. No NOR is installed by board default; this is not a BOM assertion.
NOR ID/status/WEL/program/read behavior belongs exclusively to ``m25p80``.
An unpopulated SSI bus is not an I2C address NAK, and the controller does not
invent a missing-device ACK or error. There are no public SSI core changes.

Verification and remaining boundaries
-------------------------------------

``tests/functional/arm/guest-src/bk7258/qspi_pio.c`` is built from source with
warnings treated as errors. It accesses real BK MMIO, polls completion, uses
independent SysTick interrupts for its deadline, and compares target data.
The accompanying functional tests parse native m25p80 select/transfer events
and compare every entire frame, including exact opcode/address/data length.
They also compare fresh writable NOR files against the expected programmed
arrays. Input hashes and final NOR hashes are recorded separately.

The same fixed ELF fails on baseline a472543 with PC 0x020101a6,
BFAR 0x46040060, CFSR 0x00008200, and completes on V4. The baseline has no
QSPI buses, so only the V4 invocation attaches the explicit test targets;
this command-line prerequisite is recorded, not hidden as identical topology.
This is a synthetic CP vector guest, not unchanged product firmware.

Native qtests independently cover both controllers, both MMIO aliases,
1/3/4/5/31/32-byte reads/programs, padding, state separation, WREN/status,
clock-cycle pause/resume, data-phase pause, concurrent independent progress,
source disable/commit, disable after a transmitted prefix, pending reset,
and atomic rejection of unsupported fields. Guest dummy-mode rejection
checks the actual precise bus fault. Existing SYS negative tests remain for
all unsupported bits; newly assigned QSPI0 bits now have positive RMW tests
that preserve Flash fields. Other regression and negative tests remain.

Acceptance requires the exact implementation SHA's CI, not this prose or an
old run. See the delivered manifest/logs and remote CI for the result. Full
QSPI remains PARTIAL: IRQ, full buffer capacity, general divider/source
coverage, local/physical reset domains, pin timing, quad, XIP and DMA are not
implemented. Full product startup remains unestablished. No R7A/OTP probe,
firmware change, SDK build, or physical-device operation is part of V4.
