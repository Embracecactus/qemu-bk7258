.. SPDX-License-Identifier: GPL-2.0-or-later

BK7258 entry de-injection V2: evidence boundary
==============================================

Result: **hardware subset PARTIAL; default CP de-injection PARTIAL;
complete boot PARTIAL**. No AON/R7A or SYS+0x0c hardware behavior was added.
The evidence below is insufficient to choose the initial R7A value or its
capture/reset transitions without inventing behavior. Unknown accesses remain
strict. SYS V1, its independent tests, and both multicore reset fixes are
unchanged. This is an explicit CP vector-entry investigation, not ROM startup.

The starting master is ``1f2d4876b5cc65455ed6b15d129c9632f94e565b``;
CI ``37903264521`` succeeded for that exact SHA. The 24 product captures from
SYS V1 are reused, not rerun as new acceptance. Their original evidence archive
and checksums are in ``bk7258-entry-v2-evidence/`` next to this document.

Sources and applicability
-------------------------

``sources.json`` provides immutable revisions, paths, URLs and SHA256 values.
The primary sources checked were:

* **SMP SDK** ``cb080de1655d579c7593ecf504c440997c4c137b``, the actual local
  vendor checkout and the product manifest's pin. The AIDK CP profile's bundle
  identity is ``d62004bf57db326149324184cf3c818914d77f46c66ad5ef8d053b67569c68d0``,
  matching the unchanged product build manifest. BK7258's defconfig selects
  BK7236XX and BK7258; a silicon stepping is not established by that selection.
* **Official IDK** ``650e754e12fe1e43c37ce2316a973668b033fd48``, branch
  ``release/v2.0.1``. Its reset-reason ABI differs from the SMP SDK. It is a
  comparison source, not permission to substitute its software bit allocation.
* **Vendor datasheet** DS-BK7258-E12 V2.1, 2025-05-23, section 4.4/page 61,
  downloaded from the manufacturer's hardware-reference page. It distinguishes
  chip-wide POR/BOR/AWDT reset, configurable DWDT reset scope, shutdown wake
  reset and digital-block reset on deep-sleep wake. It does not specify R7A
  reset values, capture timing or read side effects. The complete official SDK
  tree query found no BK7258 register workbook or dedicated reset manual.
* **Original binary** SDK normal bootloader, SHA256
  ``105161bb603eedafbffcb5efb8f7c06a0c8503e42ba4da46490c2c21ed813de6``;
  and the existing product CP, raw hash
  ``a6974b8f93155a707ba8942b57c0e4a3eb9f17b0c45dcedab69cf5bbce11c07b``.
  Disassembly is analysis only; neither binary is patched or rebuilt.
* **Historical hardware captures**, 2026-08-23, read-only. The captured test
  contains failures even though its UART capture utility reports success.
  Excerpts, original file hashes and capture metadata are recorded separately;
  they do not establish a silicon stepping or a controlled reset experiment.

No SDK/OpenVela files were modified or built. No physical reset, peripheral
write, fuse operation, firmware signing or flashing was performed.

R0, R7B and R7A are different evidence questions
------------------------------------------------

At the SMP SDK pin, ``aon_pmu_struct.h`` lines 26--38 assigns CP software tags
to R0[11:4] and AP tags to R0[30:24]. ``reset_reason.c`` lines 187--256 writes
those tags and reads R7A before preparing the next POWERON tags. The HAL's
``aon_pmu_hal_set_r0`` writes R0 followed by R25 keys 0x424b55aa/0xbdb4aa55.
The official IDK comment in ``reset_reason.c`` explicitly describes those keys
as passing R0 to **R7B**. SMP ``sys_pm_hal.c`` lines 1496--1512 restores R0
from R7B during low-power initialization. Neither establishes R7A's capture
edge or proves that R7A follows current R0 or R7B continuously.

The official IDK uses [30:24] as its single reset-reason field. The SMP change
``6efd7c603e7b8277970e0fe808e5fca3c639ff62`` separates CP/AP software tags.
The original product instruction at 0x020180fa reads 0x440001e8, and the next
instruction extracts [11:4]. That verifies the product's actual software ABI;
it does not identify the hardware latch operation that produced those bits.

Both SDK versions describe the R7A structure as a 32-bit reserved field.
The LL has a word getter at AON+0x1e8, but no access/reset contract. A missing
setter is not evidence of write-ignore semantics, and a volatile getter is
not evidence that reads are side-effect free. Reading once and caching the
result in software cannot distinguish clear-on-read from stable hardware.

A historical UART sample reports R0=0, R2=0x26 and R7A=0x20; another reports
R7A=0. The unequal R0/R7A sample is consistent with distinct retained state,
not a basis for a current-R0 mirror. Without ordered accesses, intervening
writes and reset provenance, it cannot establish capture or clear timing.
Nor can either observed value become a fixed model default.

.. list-table:: Candidate contract decisions
   :header-rows: 1
   :widths: 24 38 38

   * - Behavior
     - Confirmed evidence
     - Abstraction / unsupported boundary
   * - R7A address and width
     - LL word load at +0x1e8; product LDR at 0x440001e8
     - Address is confirmed; returned state is unresolved, so no new read
       implementation is enabled. Byte/halfword/unaligned semantics unknown.
   * - Software-tag relationship
     - CP [11:4], AP [30:24]; keyed R0-to-R7B operation
     - No inferred R0/R7B-to-R7A transition. Existing R0/R7B model unchanged.
   * - Capture / hold / clear
     - Startup consumes one observation; historical values vary
     - Trigger, ordering, retention duration, initial value and clear event
       missing. No constant POWERON value or automatic clearing is invented.
   * - POR / system / watchdog / core reset
     - Datasheet distinguishes reset scopes; R2 has watchdog domain controls
     - No R7A-specific reset matrix or stepping applicability. These resets
       cannot be collapsed into a shared latch update or shared reset value.
   * - Read / write side effects
     - Getter and consumers demonstrate reads only
     - Read-clear, repeat-read stability, legal writes and ignored bits unknown.
   * - Address aliases
     - SDK reg_base.h defines a generic secure/nonsecure 0x10000000 offset
     - This alone does not prove R7A-specific access/security permissions;
       no new alias behavior is claimed or implemented.

SYS+0x0c remains an independent blocked candidate
------------------------------------------------

The pinned LL names halted bits [2:0], software-reset bits [6:4], and power-down
state bits [10:8]. Its generated whole-word setter does not define writable
fields, reset values or acknowledgement timing. The original normal bootloader
loads SYS+0x0c at PC 0x020003b6, clears bit9 and writes it back at 0x020003bc.
This conflicts with treating that bit as an unconditionally read-only current
power-state observation. Neither ignoring the write nor merging legacy and
current register maps is justified. There is no proven mapping from vendor
halted to WFI or from queued CPU control to applied state. All four quantities
remain distinct; the existing strict +0x0c boundary is retained.

Minimal additional evidence
---------------------------

For the actual BK7258 stepping, obtain a vendor register specification or an
already-authorized, reproducible capture establishing:

* R7A's initial readable value and valid-bit mask at the chosen CP entry;
* whether it captures R0, committed R7B, or another source, and the exact
  triggering edge relative to commit, reset assertion and reset release;
* hold/clear behavior and read side effects, plus legal access sizes/writes;
* a domain matrix for POR/BOR, AWDT, configured DWDT, system reset and individual
  core reset, including any R2 mask effect and secure/nonsecure permissions.

A useful capture must record chip ID/stepping, reset source and configuration,
ordered R0/R7B/R7A values before and after a known tag transition, and repeated
reads. The current authorization does not permit creating that hardware
experiment; these are requirements for supplied evidence, not actions run here.
For SYS+0x0c the corresponding minimum is the applicable register map, bit9
write ownership, reset values, and when each reported status becomes effective.

Capture improvement and validation
----------------------------------

The only executable change is to ``scripts/bk7258-product-probe.py``. It now
retains raw QMP events, including WATCHDOG/RESET with their timestamps/reasons,
and trace line references for faults, vector loads and invalidated snapshots.
It does not call every vector load a system reset, merge unordered streams
into a fabricated timeline, or treat the final stopped frame as the first fault.

The original 24 runs are reused. Three additional AIDK captures validate this
recording change: a five-second default CP run, a separate five-second R7A
probe, and an eight-second R7A run because the shorter window did not yet
observe the post-reset fault. The five-second default still first faults at
PC 0x020180fa / address 0x440001e8. The isolated diagnostic still first faults
at PC 0x020daa80 / address 0x4980c000; after watchdog reset the snapshot becomes
invalid and the later fault returns to R7A. The supplied R7A value is still
explicit and is never enabled in the default run. No OTP probe was enabled.

All runs start with NOR hash
``795e90a4dd7e29bff700ac4f932c1fdc848e5545e784f1c9f6d7f3bd39c13cf4``
and independent writable copies. Source files and NOR-array bytes remain
unchanged. Diagnostic Flash status writes are recorded separately; they are
not suppressed to force equal hashes. A small host check also verifies that
an interleaved QMP RESET event is retained and that QMP errors still propagate.
These are capture-tool checks, not new hardware qtests or guest acceptance.

Evidence delivery
-----------------

``bk7258-entry-v2-evidence/SHA256SUMS`` enumerates the checked-in review inputs:
version/hash inventory, bounded historical excerpts, binary disassembly,
unchanged SYS V1 baseline archive, and new capture logs/results. Source firmware,
raw device backups, vendor PDF copies and full SDK sources are not published.
The workflow uploads this directory, this report and the capture script as a
separate named evidence artifact, in addition to the existing regression logs.
Its run must correspond to the delivered commit; a successful CI remains an
engineering gate and does not promote either V2 hardware status to PASS.
