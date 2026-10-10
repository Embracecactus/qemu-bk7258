BK7258 V5: SDK Flash clock and single-page PIO
=============================================

V5 extends :doc:`bk7258-qspi-v4` with two finite capabilities: the fixed SDK
Flash clock tuple and a 256-byte indirect PIO window/transaction. This remains
a downstream experiment. It adds no peripheral, IRQ, quad, XIP, DMA, product
initialization, probe injection or physical-device support.

Source contract and adaptations
-------------------------------

The read-only SDK pin remains ``cb080de1655d579c7593ecf504c440997c4c137b``.
``bk7258-qspi-v5-evidence/sources.json`` records checked source byte hashes and
links to the V4 evidence instead of copying previous captures.

* ``ap/middleware/driver/qspi/qspi_flash.c`` defines ``QSPI_FIFO_LEN_MAX`` and
  page size as 256. ``bk_qspi_flash_single_page_program`` calls WREN (including
  its status/WIP poll), writes the PIO data, starts single-line opcode 0x02
  with the address/length, waits for controller completion, then polls NOR
  WIP with opcode 0x05. ``bk_qspi_flash_single_read`` starts opcode 0x03 and
  reads the PIO window after controller completion.
* ``qspi_driver.c`` forwards write/read/command to ``qspi_hal.c``, which
  forwards to the BK7258 AP ``qspi_ll.h``. The LL accesses ceil(length/4)
  increasing word addresses. Its command setup uses C for write, D for read,
  four command/address bytes, DATA_LINE=0, DUMMY_CLK=0, DUMMY_MODE=0, START=1,
  then polls STATUS bit 2 and writes STATUS_CLR bit 2 followed by zero.
* The same Flash initializer selects 480 MHz, SYS divider 4, local divider 2.
  This selection is independent of its later unprotect/quad-enable calls;
  those calls and the top-level quad write path are outside V5.

``qspi_page.c`` is an equivalent MMIO **sequence compatibility test**, not an
original SDK binary or complete SDK initialization. It uses aligned local
buffers and one helper-equivalent call per aligned page. The SDK's
``uint32_t *cmd_data += cmd_data_len`` increments in words using a byte count;
V5 deliberately avoids subsequent loop iterations, does not fix that SDK,
and does not compensate in the controller. Test glue packs the LL's command
bytes directly and zeros inactive command bytes; CFG2 fields use the LL's
RMW/service order. JEDEC ID reads use three fixture ID bytes. Independent
SysTick bounds polling instead of depending on RTOS delay/services.

NOR completion is separate from controller completion. The guest issues
real 0x05 transfers and checks WIP after programming; WREN's returned WEL is
also checked. The current native ``m25p80`` target executes programming
synchronously and retains WEL after page programming. An initial extra guest
assertion requiring WEL auto-clear failed and was removed because neither the
SDK sequence nor this native target provides it. This limitation is not
silicon WIP timing evidence. No target or public SSI implementation changed.
A separate negative test sends a program after WRDI and verifies erased data
remains unchanged: the controller does not insert WREN.

Finite clock extension
----------------------

The Beken AP BK7258 v3.1.1
`QSPI example <https://docs.riselink.ai/arminodoc/bk_avdk_smp/ap_doc/bk7258/en/v3.1.1/examples/peripheral/bk_qspi.html>`_
corroborates the pinned 480/4/2 tuple as 24 MHz: SYS divides by 1+4, then the
local stage by 2*2. Its newer frequency-selection helper is not substituted
for the fixed SDK. The V4 480/9/0 tuple continues to produce 48 MHz.

The device's existing QEMU ``sclk`` input now explicitly represents the
**post-SYS, pre-local frequency**, not final SCK:

.. list-table:: Supported tuples only
   :header-rows: 1

   * - Source / SYS divider / local divider
     - SYS fields [10:6]
     - QEMU input Clock
     - Effective SCK
   * - 480 MHz / 9 / 0
     - 0x640
     - 48 MHz
     - 48 MHz
   * - 480 MHz / 4 / 2
     - 0x500
     - 96 MHz
     - 24 MHz

CONFIG[15:8] accepts local 0 or 2; other local divider encodings are rejected.
The mixed tuples 480/4/0 and 480/9/2 supply no effective SCK. Unknown SYS
source/divider selections supply no input clock. Both outputs still require
committed EN_DPLL and their independent SYS gate. There is no lock/ready bit.
This whitelist does not imply other divider combinations are implemented.

Timers consume eight **effective SCK** cycles per byte, not eight input
cycles followed by an additional guessed division. Per-byte deadlines round
up to integer virtual nanoseconds: 167 ns at 48 MHz, 334 ns at 24 MHz.
Qtests check a real WREN before/at 334 ns and a status frame before/at 668 ns,
then confirm target WEL. They also check gate pause/resume, committed source
loss/recovery and both unsupported mixed tuples. Clock attributes alone do
not satisfy these checks. The existing cycle-budget helper remains unchanged.

Window and transaction extension
---------------------------------

PIO covers aligned words +0x100 through +0x1fc (bytes +0x100..+0x1ff).
The 64 word accesses generated for a 256-byte SDK call include +0x1f4,
+0x1f8 and +0x1fc, although ``qspi_struct.h`` declares ``fifo_data[61]``.
The Beken v3.1.1
`guide <https://docs.riselink.ai/arminodoc/bk_avdk_smp/ap_doc/bk7258/en/v3.1.1/developer-guide/peripheral/bk_qspi.html>`_
also describes 256 bytes. V5 implements that corroborated software-visible
window; the declaration discrepancy and silicon capacity remain unverified.
No guard register is invented beyond +0x1ff. Short reads preserve all bytes
outside DATA_LEN inside this window, including word padding and the tail.

CMDC permits 0..256 data bytes, where zero is a command-only frame. CMDD
permits 1..256. Length 257 and larger requests are rejected at START, without
sending bytes or changing active state. Counts and offsets remain unsigned
integers, not eight-bit values. The 256-byte array matches the MMIO window;
no special casing of guest PC, buffer address, opcode or filename is used.

All command/address/data bytes remain in one CS session. The controller has
no NOR page-size or page-alignment restriction: alignment is the guest's
responsibility for the explicitly attached native ``w25q32`` fixture.
The target owns ID/WEL/WIP/program/read semantics. No backing access is made
by the controller and no default board BOM is inferred.

Completion/clear/START, busy-write rejection and strict unsupported-mode
behavior otherwise retain V4's contract. Disable/reset releases CS and
cancels the timer without rolling back bytes accepted by the target. QEMU
reset is still an emulator lifecycle rule, not a silicon reset-domain claim.

Evidence and acceptance
-----------------------

Three separately fixed ELFs isolate the increments. Against baseline
``7f72f37ccb3d3ea119eed6b0967621a08913a760``:

* 24 MHz / 256 bytes: precise fault PC 0x02010208, BFAR 0x46040060.
* 48 MHz / 33 bytes: precise fault PC 0x0201015e, BFAR 0x4604004c.
* 24 MHz / 32 bytes: precise fault PC 0x02010208, BFAR 0x46040060.

All have CFSR 0x00008200 and exit 1 on V4; the identical respective ELFs exit
0 on V5 with exact native SSI frames. These PCs were captured by the guest,
not predicted from source. Both invocations attach the same two test NORs.

Qtests cover 32/33, 244/245, 255/256/257; the last three words; aliases;
short-read tail preservation; two targets/pages and clock/buffer isolation;
and no implicit WREN. The V4 33-byte negative boundary is explicitly moved
to 257, retaining all other valid negative tests and existing capabilities.

The long-frame test pauses QSPI0 after 160 data bytes while QSPI1 completes.
Resume yields one 260-byte frame (4 command/address + 256 data). Disable and
system reset yield one 164-byte prefix frame on QSPI0, with exactly the first
160 bytes committed and the remaining 96 erased. QSPI1 retains full pages.
The V4 native select/transfer parser is reused to check every byte and CS
boundary; CI independently reruns this capture and checker. No late callback
or fake completion is permitted after cancellation.

The source-built page guest runs on all three machines, programs different
aligned pages on each target, polls controller and target status separately,
compares every byte and rereads the earlier page. Its functional harness also
checks whole NOR-file hashes and entire native frames. These are model
regressions, not physical board acceptance or original SDK-binary execution.

Only exact-SHA CI completes V5 acceptance. Full QSPI and product startup
remain PARTIAL/unestablished. IRQ, dummy, quad, XIP, DMA, physical capacity,
general clocks and physical reset domains remain outside scope. Original
boot Flash/NOR/CRC-XIP inputs and implementation are unchanged. SYS changes
only add the QSPI clock tuple; existing Flash and product-path evidence is
reused without rerunning the 24 historical CP/BL1 captures.
