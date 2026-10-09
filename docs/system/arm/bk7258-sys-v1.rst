.. SPDX-License-Identifier: GPL-2.0-or-later

BK7258 startup SYS V1 evidence boundary
======================================

This downstream experiment adds a **configuration-register subset**, not a
Flash clock implementation. Startup SYS functionality remains PARTIAL until a
sourced status or clock-consumer contract is implemented and independently
verified. Passing configuration tests must not be reported as Flash timing,
source availability, PLL lock, or complete product acceptance.

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

SYS+0x24: supported configuration contract
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
* This synchronous latch queues no work and has no busy/ready/lock flag.
  Flash operation completion and cancellation retain their prior contract.

The native qtest independently enumerates both fields, their RMW preservation,
both aliases, reset and atomic rejection of unsupported bits. Source-built
C guests execute the field operations on each board; a separate negative
execution verifies precise BusFault (CFSR 0x8200, BFAR 0x54010024) and preserved
configuration after the unsupported transaction. Neither test reads host
implementation state. They do not prove clock delivery to Flash.

Why no Flash clock claim
------------------------

SDK ``sys_hal.c`` labels the divider getter /1,/2,/4,/8, but its initialization
uses a Flash 480/6=80 MHz comment. ``flash_driver.c`` has /4,/6,/10 comments in
SoC-conditional branches; those must be checked against the executing image's
configuration rather than applied indiscriminately. ``flash_ll.h`` separately
sets the controller's ``CLK_CFG`` to 5. The source getter's APLL terminology
also differs from the active boot path's DPLL terminology. A four-bit latch
cannot resolve these differences.

The present controller schedules an asynchronous one-microsecond functional
transaction; it has no SYS Flash-clock input. That latency is not derived
from these new fields. XIP also remains the documented committed-array/cache
abstraction. Do not infer performance, source-loss stalling, gating or PLL
availability from the new readback. No OTP, R7A, analog-ready or lock value is
added to get past a firmware stop.

Minimum remaining evidence
--------------------------

* For +0x0c: applicable silicon revision and field access/reset semantics;
  what halted includes; how reset/power status acknowledges the actual
  transition; and the legacy bit9 write's ownership.
* For a functional +0x24 subset: applicable source/divider combinations,
  controller ``CLK_CFG`` composition, source enable/loss behavior and reset
  ownership. Then wire a real consumer and test rate changes, loss/recovery
  and reset cancellation independently of configuration readback.
* For unchanged product comparison: the original hash-matched direct-app,
  MCUboot and BL1 NOR inputs and their existing entries/status inputs. A
  missing or inaccessible artifact must not be replaced by a new firmware
  build. Default and individually enabled OTP/R7A diagnostic runs remain
  separate evidence categories.

Local validation (2026-10-09)
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

Configuration readback has a contract, implementation and independent
positive/negative evidence. Actual startup status/clock functionality and
complete product capability remain PARTIAL. The preceding minimum-evidence
list is the next gate, not a reason to claim a completed clock connection.
