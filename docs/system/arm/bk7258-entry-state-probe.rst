BK7258 diagnostic entry-state experiment
========================================

This is an isolated, opt-in investigation tool. It injects declared synthetic
entry observations so an unchanged firmware image can reveal its next
independent dependency. It is **not** an OTP peripheral implementation,
activation test, R7A capture model, silicon reset model or cold-boot result.
The ordinary machine maps neither snapshot. It supplies no fuse contents,
calibration values, reset reasons, busy completion or PLL lock by default.
The OTP and R7A experiments cannot be enabled together.

Evidence and the reason for this boundary
----------------------------------------

At SDK ``cb080de1655d579c7593ecf504c440997c4c137b``,
`otp_struct.h
<https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/middleware/soc/bk7258/soc/otp_struct.h#L228-L255>`_
defines OTP+0x2c4 bit0 as PTM busy and +0x2c8 bit0 as macro sleep/active.
Bit1 controls the internal PUFtrng clock; its role is not evidence that it is
an OTP-array read prerequisite. RD_TO_SEL describes a 10/5/2-us/immediate
active-mode wait selection, without defining how that relates to PTM busy.

`otp_ll_init
<https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/middleware/soc/bk7258/hal/otp_ll.h#L54-L69>`_
enables the SYS gate and power, waits, and polls busy. The separate
`otp_ll_active
<https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/middleware/soc/bk7258/hal/otp_ll.h#L434-L450>`_
sets both activation bits then polls. These different software entry
assumptions do not establish a complete hardware activation/reset contract.

The unchanged package's `BL1 early initialization
<https://github.com/Embracecactus/contest2026_135_yongwangzhiqian/blob/ffe8356bf8b0e4eeaa504b6bcfaaf529bc842b43/chips/bk7258/bootloader/boot_runtime.c#L234-L263>`_
reads busy **once**, then conditionally reads OTP word242 and writes 7 to
memory-check+8 if its low nibble is 7. Assigning an invented activation delay
to busy could silently skip this branch. The SDK's
`shared startup
<https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/startup_bk7236.c#L362-L389>`_
identifies this field as provisioned by CP-TEST. A synthetic zero is not
knowledge of a physical board's calibration.

The SDK `reset-reason reader
<https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/middleware/driver/reset_reason/reset_reason.c#L230-L256>`_
reads R7A, extracts CP/AP fields and then changes R0's tags independently.
It does not establish when hardware captures R0 into R7A or clears R7A.

Explicit probe rules
--------------------

The child type is ``bk7258-entry-probe`` at ``/machine/soc/entry-probe``.
All input properties are immutable after realization. Presence is tracked
separately from the parsed numeric value, keeping all 32-bit data values,
including 0xffffffff, available while rejecting malformed or out-of-range
explicit inputs. No input is inferred from a board name or firmware filename.

OTP requires all three properties, with no partial defaults::

  -global bk7258-entry-probe.otp-control=3 \
  -global bk7258-entry-probe.otp-status=0 \
  -global bk7258-entry-probe.otp-word242=0

Control must be exactly 3. Status must be 0 or 1, and word242 must be a
32-bit word. The three accepted word-sized reads are 0x4b1002c8,
0x4b1002c4 and 0x4b1007c8. Word242 is readable only with status=0.
Status=1 is an **immutable busy-state fault injection**, not an activation
that will complete. It must not be counted as normal startup acceptance.

The first machine reset starts one pending epoch and retains existing SYS
initial values. The first state with SYS+0x30 bit15 set and SYS+0x40 bit3
clear exposes the snapshot. This is deferred diagnostic provisioning, not
physical activation. It does not depend on the first MMIO read. Subsequent
gate or power loss permanently invalidates that epoch; restoring them does
not rearm it. Every later device/system reset invalidates it as well.

An identical control write is accepted without changing anything, solely
as an idempotent probe rule. All other writes and unsupported reads fail
atomically with MEMTX_ERROR. They neither update the supplied state nor
simulate activation, programming, calibration, repair or an error flag.
These faults are deliberate unsupported-operation policy, not claimed
silicon BusFault behavior. Partial and unaligned accesses are rejected.
Only the stated secure addresses are supplied; no alias is inferred.

R7A is a separate experiment::

  -global bk7258-entry-probe.r7a=0x01000020

It supplies one immutable 32-bit entry observation at 0x440001e8 through
the initial epoch. R0 writes and keyed commits do not modify it. Later
reset invalidates it and does not copy R0, supply POWERON or recreate the
snapshot. Its read stability and write rejection are probe rules, not a
claim about physical read/clear behavior.

Declared-input execution results
--------------------------------

These observations use the existing MCUboot package built from source
``ffe8356bf8b0e4eeaa504b6bcfaaf529bc842b43``. The unchanged simulation NOR
array SHA-256 is
``13efc53dce4596d75c7e3a09a0e3391a25cb00b6ff09632d3734725f70922a72``.
No firmware bytes, OpenVela configuration or SDK source were changed.
The host selects BL1 vector 0x02000000 or CP vector 0x02010200; hardware ROM
is bypassed. An existing BL1 entry is not a completed bootloader chain.

.. list-table:: First fault under each declared input
   :header-rows: 1
   :widths: 39 17 17 27

   * - Entry / injected premise
     - Faulting PC
     - Fault address
     - Observed branch
   * - BL1, no snapshot
     - 0x02000bfa
     - 0x4b1002c8
     - Original OTP control stop
   * - BL1, control=3, status=0, word242=0
     - 0x02000b18
     - 0x4401000c
     - Reads supplied word; skips memory-check write
   * - BL1, control=3, status=0, word242=7
     - 0x02000c24
     - 0x44890008
     - Reads supplied word; attempts memory-check write of 7
   * - BL1, control=3, status=1, word242=7
     - 0x02000b18
     - 0x4401000c
     - Never reads word242; immutable busy fault case
   * - CP, no snapshot
     - 0x020180fa
     - 0x440001e8
     - Original R7A stop
   * - CP, R7A=0
     - 0x0202b9ca
     - 0x44010024
     - Reads zero; later Flash selector is unsupported
   * - CP, R7A=0x01000020
     - 0x0202b9ca
     - 0x44010024
     - Reads supplied tags; same later Flash selector stop

Trace events ``bk7258_entry_probe_*`` record supplied reads and epoch changes.
BL1 PCs were captured using ``-accel tcg,one-insn-per-tb=on`` with
``-d in_asm,cpu,int,guest_errors,unimp``. Each injected BL1 case was also
rerun with ordinary TCG translation and reached the same fault address and
exact read sequence. Neither run uses icount or semihosting. CP PCs come
from the unchanged firmware's precise HardFault UART report. Host execution
bounds and emulator process exit status are not firmware success criteria.

The next BL1 stop is its secondary-core shutdown status poll at SYS+0x0c.
This is a candidate for separate source-based modeling, not yet implemented
by this tool. A later BL1 RMW clears bit9 there, while the active SDK names
that bit CPU1 powerdown state and places Flash selection at SYS+0x08.
The same RMW occurs in both SDK-provided
`A/B <https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/components/bk_libs/bk7258/bootloader/ab_bootloader/bootloader.bin>`_
and `normal <https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/components/bk_libs/bk7258/bootloader/normal_bootloader/bootloader.bin>`_
bootloader binaries, at 0x0200035e--0x02000364 and
0x020003b6--0x020003bc respectively. Their SHA-256 values are
``3b27958ef78cbb7e56b57695585008465c759a7671cfd776334fec49d3164047`` and
``105161bb603eedafbffcb5efb8f7c06a0c8503e42ba4da46490c2c21ed813de6``.
It is therefore not a demonstrated reconstruction-only firmware typo.

The common `disabled reference map
<https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/middleware/soc/common/hal/include/sys_hal.h#L535-L593>`_
places status at +0x08 and storage/Flash at +0x0c, unlike the
`active BK7258 map
<https://github.com/Embracecactus/bk_avdk_smp/blob/cb080de1655d579c7593ecf504c440997c4c137b/cp/middleware/soc/bk7258/soc/sys_struct.h#L39-L69>`_.
This suggests inherited mapping or revision differences, not resolved silicon
semantics. Do not silently ignore the write or reinterpret +0x0c as Flash.
Nor should CPU status simply echo requested controls before their asynchronous
application, or equate QEMU's WFI-induced halted state with this register's
halted indication without evidence.
The memory-check controller and the physical OTP/R7A transitions remain
unimplemented. None of these injected runs is full firmware acceptance.
