.. SPDX-License-Identifier: GPL-2.0-or-later

BK7258 startup SYS V1 evidence boundary
======================================

This downstream experiment implements SYS+0x24 configuration and a bounded
Flash-command clock consumer. The supported finite profiles have independent
register, progress, source-loss and cancellation tests. It does not implement
SYS+0x0c status, general Flash clock topology, physical serial/program/erase
timing, PLL lock, or complete product startup. Product acceptance remains
PARTIAL. The remote gate and a local unchanged-product comparison have now
been validated below; default startup still stops at unimplemented devices.

Engineering gate
----------------

The native-build workflow runs every change on its explicit branch allowlist
and on manual dispatch. A device-only path allowlist missed transitive inputs:
``include/hw/misc/bk7258_clock.h``, core clock and ptimer headers/implementations,
trace descriptions/generators, Kconfig and Meson inputs. Removing the path
filter covers those dependencies, including future ones, at the cost of also
building documentation-only changes on these branches. It does not broaden
the push branch allowlist beyond the named SYS V1 branch.

The gate keeps warnings-as-errors, BK clock accounting, native qtests and
source-built three-board guests. It additionally builds/runs the existing
shared ``ptimer-test`` and non-BK ``cmsdk-apb-timer-test``. These are shared
component regressions, not additional BK7258 feature claims. A prior run for
master is baseline evidence only; a branch CI result must have the delivered
commit as its actual ``head_sha``.

SYS+0x0c: deliberately unresolved
--------------------------------

The active BK7258 SDK at ``4ca389311a7ef641f10b94d298dc07ee16b0f79c`` defines
``COREn_HALTED`` at bits 0..2, ``CPUn_SW_RESET`` at bits 4..6, and
``CPUn_PWR_DW_STATE`` at bits 8..10 in ``soc/sys_reg.h``. Its generated LL
has individual getters and a whole-register setter. Accessor availability
alone does not prove writable status, write-ignore semantics, reset values,
or the instant at which a hardware transition becomes visible.

The independently rehashed SDK normal bootloader at
``cb080de1655d579c7593ecf504c440997c4c137b`` has SHA256
``105161bb603eedafbffcb5efb8f7c06a0c8503e42ba4da46490c2c21ed813de6``.
At 0x020003b6 it loads [r4,#12], clears 0x200 at 0x020003b8, and stores
back at 0x020003bc; r4's literal at 0x02000414 is 0x44010000. This is the
original binary, not a modified test firmware. The active SDK and disabled
legacy map conflict as detailed in :doc:`bk7258-entry-state-probe`.

Keep these four different quantities separate:

* ``cpu_control[]`` is accepted control-register readback.
* A queued work item holds a requested value, release edge and reset generation.
* The target-CPU callback applies reset/power gating, then emits its apply trace.
* CPU WFI sleep can change QEMU's ``halted`` independently of those controls.

No register status is inferred from any one of these without a field-specific
contract. Reads and writes at +0x0c continue to fault. In particular, the bit9
RMW is neither discarded nor redirected to the legacy map. The existing
release-vector preservation and stale-generation rejection remain unchanged.

SYS+0x24: configuration contract
-----------------------------------------

The same active SDK's ``soc/sys_reg.h`` defines ``CKSEL_FLASH`` at [25:24]
and ``CKDIV_FLASH`` at [27:26]; ``hal/sys_ll.h`` uses independent field RMW.
``hal/sys_hal.c`` exposes their setters/getters. Only these four bits are
implemented here:

* Aligned 32-bit reads return the stored four-bit configuration. Other fields
  of this shared register are outside this subset and read as zero, not as
  established reset values of the corresponding peripherals.
* All sixteen raw field encodings can be stored and read. This accepts an
  encoding as **configuration**, not as a working source/divider mode.
* A write containing any other bit returns ``MEMTX_ERROR`` before modifying
  either supported field. This is a strict model boundary, not a claim about
  silicon's reserved-bit write response. It also applies to RMW and aliases.
* Secure +0x44010024 and nonsecure +0x54010024 share the same storage.
* Whole-machine reset clears the configuration to zero, an explicit
  direct-load convention rather than an established silicon POR value.
* This synchronous latch has no busy/ready/lock flag. It updates a real QEMU
  clock consumed by pending Flash commands for the finite profiles below.
  The controller's existing busy flag describes its pending command, not
  PLL readiness or calibration success.

The native qtest independently enumerates both fields, their RMW preservation,
both aliases, reset and atomic rejection of unsupported bits. Source-built
C guests execute the field operations on each board; a separate negative
execution verifies precise BusFault (CFSR 0x8200, BFAR 0x54010024) and preserved
configuration after the unsupported transaction. Neither test reads host
implementation state. These configuration tests alone do not prove clock
consumption; the separate command tests below cover it.

Bounded Flash-command clock contract
------------------------------------

The pinned SDK at ``4ca389311a7ef641f10b94d298dc07ee16b0f79c`` establishes
complete software-selected profiles through this active path:

* ``cp/middleware/soc/bk7258/bk7258.defconfig`` selects
  ``CONFIG_SOC_BK7236XX`` and ``CONFIG_SOC_BK7236_V5``, not BK7236N/BK7239.
* ``cp/middleware/driver/flash/flash_driver.c`` lines 570--605 first call
  ``flash_hal_set_default_clk`` and then select source 1 with divider 1 for
  GD/TH (80 MHz), or divider 3 for other IDs (48 MHz). The 120-MHz option is
  a separate conditional and is not included in this model subset.
* ``cp/middleware/soc/common/hal/include/flash_hal.h`` line 46 forwards that
  call to ``flash_ll_set_default_clk``; BK7258 ``hal/flash_ll.h`` lines
  253--260 set controller ``CLK_CFG`` to 5.
* ``hal/sys_hal.c`` names source 1 as ``PM_CLKSEL_FLASH_480M`` and the MP
  default as 480/6=80 MHz. The unchanged product BL1's source selection agrees.
  The `BK7258 datasheet
  <https://docs.riselink.ai/spec/BK7258/BK7258%C2%A0Datasheet.pdf>`_,
  DS-BK7258-E12 V2.1 section 4.3, identifies nominal 26-MHz XTAL and
  480-MHz DPLL sources.

This narrows the prior uncertainty: the getter's /1,/2,/4,/8 comment does not
win over the selected driver profile. It does **not** justify a formula for
all divider values or additional ``CLK_CFG`` encodings. Accordingly the model
uses only complete tuples, with no inferred multiplication of two dividers:

.. list-table:: Command progress profiles
   :header-rows: 1

   * - SYS source / divider
     - Controller CLK_CFG
     - Nominal command clock
     - Availability
   * - 0 / 0
     - 0
     - 26 MHz
     - Existing direct-load diagnostic reset convention, not silicon POR
   * - 1 / 1
     - 5
     - 80 MHz
     - Committed ANA5.EN_DPLL (bit5) is set
   * - 1 / 3
     - 5
     - 48 MHz
     - Committed ANA5.EN_DPLL (bit5) is set

Other encodings retain configuration readback but supply no modeled command
progress. In particular, source 2/3, divider 0/2 on DPLL, XTAL with CLK_CFG=5,
and other controller timing codes are **unimplemented profiles**, not a claim
that those silicon modes physically have no clock. A pending command stays
pending; it cannot manufacture successful completion. Changing to a supported
available profile resumes it. Existing busy-register write restrictions still
apply to controller configuration; SYS source changes can pause/resume a
pending command. No unrelated MMIO or unsupported register bit is ignored.

The source enable is sampled after the existing analog-register transfer
commits, never from its queued value. It is a nominal digital availability
input, not PLL lock or measured analog settling. Clearing it removes DPLL
progress. No additional Flash gate is invented. The reset diagnostic XTAL
profile is always available by the existing direct-load convention.

A command has a 26-cycle **functional work budget**, preserving the former
one-microsecond asynchronous boundary at the diagnostic 26-MHz rate. This
budget is a model convention, not the number of wire bits or measured NOR
program/erase cycles. Pending work consumes the old rate before a clock
change, pauses at zero rate, and resumes the remaining whole-cycle budget.
Deadlines round upward to virtual nanoseconds; subcycle phase is not modeled.
At 80/48 MHz a fresh command completes at 325/542 ns respectively. Existing
unsupported-command, FIFO, protection and backing-error behavior is retained.

Controller reset cancels its timer and pending work, resets its CLK_CFG to the
existing diagnostic value, and preserves the SYS selection. Whole-machine
reset additionally clears SYS clock selection and ANA5 and cancels any queued
analog enable. Neither reset permits a canceled program to alter NOR later.
XIP remains the preexisting last-committed-array/cache abstraction; its fetches
are not clocked or stalled by this subset. Serial lanes, bus arbitration,
physical program/erase latency and Flash power domains remain unmodeled.

The qtest observes command busy/data/ID through MMIO, not merely QOM clock
properties. It checks both rates, a mid-command rate change, source loss and
remaining-work recovery, the analog commit boundary, all other SYS encodings,
peripheral cancel and system reset with a queued source enable. The separate
``flash_clock.c`` guest runs from CRC NOR and checks absent-source noncompletion,
real program/read at both profiles, source-loss cancel, unsupported-profile
noncompletion and recovery. A file-level oracle checks that only its intended
32-byte program committed and all canceled locations remained erased.

Minimum remaining evidence
--------------------------

* For +0x0c: applicable silicon revision and field access/reset semantics;
  what halted includes; how reset/power status acknowledges the actual
  transition; and the legacy bit9 write's ownership.
* To extend +0x24 beyond the finite profiles: general source/divider and
  controller ``CLK_CFG`` composition, additional gate/power/reset ownership,
  and serial/XIP timing. These are outside the completed command-clock slice.
* For unchanged product comparison: the original hash-matched direct-app,
  MCUboot and BL1 NOR inputs and their existing entries/status inputs. A
  missing or inaccessible artifact must not be replaced by a new firmware
  build. Default and individually enabled OTP/R7A diagnostic runs remain
  separate evidence categories.

Initial configuration validation (2026-10-09)
----------------------------

On baseline ``1ce94aeafec7f92e1d73c1dac3599d0ba8109ed4``, native compilation
with ``--enable-werror`` and source-built guests establish the following
configuration-subset evidence after this change:

* 3 clock-accounting unit checks, 576 existing shared ptimer checks and the
  existing non-BK CMSDK timer qtest passed.
* All 256 BK native qtests and 259 functional tests passed across t5_board,
  t5ai_core and aidk_ai_toy. Existing negative tests were retained, including
  core-reset-vector and pending-reset generation coverage.
* The exact same positive and negative guest ELF bytes were then executed on
  independently built baseline and changed emulators on all three boards.
  All six baseline executions failed at the first +0x24 read: PC 0x020100f4,
  fault address 0x44010024. Changed positive guests completed successfully;
  changed negative guests faulted at PC 0x02010142, address 0x54010024 and
  checked that the previous supported configuration remained intact.
  These 12 runs record emulator/guest hashes, command lines, UART and traces.

The first local qtest invocation failed because the sandbox disallowed its
UNIX socket, before hardware testing. Its original log was retained; the
identical command passed with local IPC permitted. Initial Python dependency
failures were likewise retained and resolved in an isolated temporary Python
3.12 environment. No hardware implementation or negative test was changed to
work around those environment failures.

Historical product artifacts 11084027687 and 11084869555 were queried
separately and both returned HTTP 404. Their documented hashes were not
found in the checked local input locations. No replacement firmware was built.
Default product and injected OTP/R7A product runs were therefore **not rerun**;
the prior stop addresses remain historical, with the new next stop unknown.
The fork CI run 37724570591 was rechecked as successful at exactly the
baseline SHA, but is not validation of these new commits. New remote branch
publication and CI still require the user's applicable authorization.

These initial results validate configuration only. The subsequent command-clock
implementation has separate evidence below; do not use these earlier counts
to validate the clock consumer.

Command-clock validation (2026-10-09)
------------------------------------

The command-clock implementation passes all 259 native BK qtests and all 262
source-built functional tests across the three machine models, with the same
3 shared clock-accounting, 576 ptimer and non-BK CMSDK timer checks. A final
focused guest rerun checks all nine Flash configuration/clock cases; the final
native rerun also includes reset during a pending analog source-enable write.
Warnings-as-errors and existing negative tests remain enabled.

For a stronger before/after oracle, the identical clock guest NOR bytes were
run on the independently built configuration-only commit ``d7fee00`` and the
new consumer on every board. The old implementation passes register accesses
but fails the guest's absent-source check (exit 3): its fixed-delay command
completes without an available source. The new implementation exits 0 and the
file oracle observes precisely its intended 32-byte program, with canceled
locations unchanged. All six commands, input/output hashes and traces are
retained. This shows a behavioral clock-consumer addition, not merely a clock
variable or another successful register test. No physical timing accuracy is
inferred from this functional experiment.

Supplementary unchanged manufacturer binary
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The original SDK normal bootloader above can be acquired independently of the
unavailable product artifacts. Its reset words are SP=0x28030000 and
PC=0x020001c1. A simulation NOR container encodes the original bytes with the
existing CRC framing at physical offset zero; decoding recovers the identical
SHA-256 payload. Execution uses its vector at 0x02000000, without semihosting,
icount, ROM emulation or any firmware patch. Fresh NOR/status copies are used
for each run. This is a supplementary manufacturer-binary experiment, **not**
a replacement for the missing product NOR inputs.

.. list-table:: Manufacturer binary: baseline and new clock implementation
   :header-rows: 1

   * - Explicit premise
     - All three baseline boards
     - All three changed boards
   * - Default, no OTP/R7A probe
     - PC 0x0200016a / fault 0x4b1002c8
     - Same OTP-controller read fault
   * - Separate OTP diagnostic: control=3, status=0, word242=0; no R7A
     - PC 0x020003b6 / fault 0x4401000c
     - Same disputed SYS status/RMW read fault

The OTP values are declared synthetic entry observations. They do not prove
activation, fuse contents or calibration. The diagnostic is only selected by
an explicit command-line option; the ordinary run supplies neither probe and
OTP/R7A are never combined. These cases cannot be reported as default boot or
OTP hardware support.

Each of these twelve runs stops through QMP, then reads CPU0 registers, its
exception stack, CFSR and BFAR through the read-only GDB interface. The stopped
PC is the original HardFault loop at 0x02000140; the stacked PC identifies the
faulting instruction above. CFSR is 0x8200 and BFAR matches the trace. NOR-array
hashes remain unchanged. QMP exit is host termination, not firmware success.
The initial attempt used HMP, which this minimal build disables; its failure
and trace were retained before switching to GDB. No firmware or implementation
change was used to bypass that diagnostic-environment limitation.

The bounded Flash-command slice now has its contract, implementation and
independent positive/negative evidence. New remote CI and the original product
NOR comparisons remain open. SYS+0x0c and full product capability remain
PARTIAL; neither stopped manufacturer run closes those gaps.

Local unchanged-product comparison (2026-10-09)
------------------------------------------------

The user supplied the local OpenVela project as the firmware source, read-only.
The existing full package from cold-delivery run ``36506715047`` was copied
with its release/build manifests. Its declared source is
``e0dce2516b7300d58ef6a4cd12861f2f39380378``, version ``0.7.0+1``, target
``aidk_ai_toy``, CP ``app``, AP ``openvela_ap``, boot ``mcuboot``.
No product source, configuration, build output or firmware byte was modified;
no product rebuild, signing or physical flashing was performed. Running this
AIDK input on the other two machine models is model regression, not evidence
that their physical boards accept this product package.

This is a new, separately identified product baseline. It does not recover
the missing historical direct-app/MCUboot images listed above:

* Package SHA256:
  ``394fcd8cd1d3a30efd66c56a100b281e5d0c3d4ce7261e41ac919e141e4de5d2``.
* Simulation NOR SHA256:
  ``795e90a4dd7e29bff700ac4f932c1fdc848e5545e784f1c9f6d7f3bd39c13cf4``.
* Original raw CP hash, recovered from CRC-framed payload after its 512-byte
  MCUboot header and checked against the build manifest:
  ``a6974b8f93155a707ba8942b57c0e4a3eb9f17b0c45dcedab69cf5bbce11c07b``.
* Original raw BL1 hash, likewise checked:
  ``4e1da542eb4d43a585f40d569ddbf7b15f9714a838b9444368f12d6eba20378c``.

The 8-MiB NOR container copies every package ``images`` member and the
``full_update`` payload unchanged to its manifest offset, checking sizes,
hashes, partition bounds and non-overlap. Other ranges remain erased,
including unspecified device identity and calibration. This is not a
board-flashable factory image or a signature/boot-chain acceptance result.
The explicit Flash status input is 512 bytes: ``00 00 20`` followed by zeros,
SHA256 ``0447b8e676635ad3befa686ec7e75bf5dbea1f7005a0e2af8f808582eb12fe6e``.
It is a diagnostic backend input, not a captured physical-device status.

The same inputs ran on baseline ``1ce94ae`` and SYS V1 ``3adf85e`` on
``t5_board``, ``t5ai_core`` and ``aidk_ai_toy``. All 24 executions agreed
across the three machines:

.. list-table:: First fault, before and after SYS V1
   :header-rows: 1
   :widths: 31 30 30

   * - Entry / premise
     - Baseline PC / address
     - SYS V1 PC / address
   * - CP 0x02010200, no probe
     - 0x020180fa / 0x440001e8
     - 0x020180fa / 0x440001e8
   * - BL1 0x02000000, no probe
     - 0x02000bfa / 0x4b1002c8
     - 0x02000bfa / 0x4b1002c8
   * - CP, explicit R7A=0x01000020 only
     - 0x0202b9b2 / 0x44010024
     - 0x020daa80 / 0x4980c000
   * - BL1, explicit OTP control=3/status=0/word242=0 only
     - 0x02000b18 / 0x4401000c
     - 0x02000b18 / 0x4401000c

CP fault PCs come from the firmware's UART exception frame, correlated with
QEMU's first data-abort address. BL1 PCs come from its stable exception stack
and instruction trace. In the R7A experiment, the later watchdog reset
invalidates the supplied snapshot; a subsequent fault returns to R7A.
Reading only the last BFAR/PC would therefore hide the first RF fault.
Stopped CPU registers and raw stack are retained separately and must not
automatically be called the first exception frame.

This comparison proves that unchanged product code crosses the previous
SYS+0x24 boundary. It does not independently validate every supported Flash
rate, nor make the R7A injection a real reset model. The independent native
and source-built guest tests remain the command-clock consumer oracle.
RF at 0x4980c000 is the next unimplemented boundary, not a newly discovered
regression; RF implementation is outside SYS V1. Unknown accesses still fault.

Reproducing a prepared-input capture
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``scripts/bk7258-product-probe.py`` accepts explicit NOR/status hashes and a
vector-table entry. It creates fresh writable copies in a new output directory,
preserves source inputs, saves the command, emulator hash, UART, trace, stopped
registers and before/after hashes, and never interprets a timed capture as
successful startup. It needs local QMP/GDB Unix sockets. For example::

  python3 scripts/bk7258-product-probe.py \
    --qemu build-bk7258/qemu-system-arm --board aidk_ai_toy \
    --nor product.nor \
    --nor-sha256 795e90a4dd7e29bff700ac4f932c1fdc848e5545e784f1c9f6d7f3bd39c13cf4 \
    --status input.status \
    --status-sha256 0447b8e676635ad3befa686ec7e75bf5dbea1f7005a0e2af8f808582eb12fe6e \
    --entry 0x02010200 --seconds 5 --output cp-default

Use a different output directory for each capture. A separate, explicit
``--r7a 0x01000020`` run reproduces the CP diagnostic. For BL1 use entry
``0x02000000``, ``--seconds 1`` and ``--instruction-trace``; its isolated
diagnostic additionally supplies ``--otp-control 3 --otp-status 0
--otp-word242 0``. No diagnostic input is enabled by default, and combining
OTP with R7A is rejected before launching QEMU. Partial OTP inputs and hash
mismatches were also tested to reject without creating output or starting QEMU.

An initial one-second CP capture with instruction tracing stopped during BSS
zeroing, with no fault yet; it was not treated as success or a hardware bug.
The measured CP comparisons use five seconds and no per-instruction trace.
Every NOR remained hash-identical after execution. The original local package
and manifests were rehashed after testing and remained unchanged.

The actual ``3adf85e`` branch CI run ``37897442986`` and master CI run
``37898455438`` both succeeded. These replace the former remote-gate gap for
that implementation commit. Default product boot, SYS+0x0c ownership/status,
physical-device behavior and complete product capability remain PARTIAL.
