.. SPDX-License-Identifier: GPL-2.0-or-later

BK7258 ordinary driver-path audit
================================

This downstream audit compares ordinary initialization, transfer and interrupt
acknowledgement sequences with the bounded models in this repository. It does
not certify a complete SDK build or the physical chip. The source revision is
``Embracecactus/bk_avdk_smp``
``4ca389311a7ef641f10b94d298dc07ee16b0f79c``; the audit was performed on
2026-10-01 against QEMU model revision
``520f2f5432c5a5f55ba96db81c26255a501126b4``. Register fields, active LL/HAL
operations and driver call sites were checked together. Generated names and
comments alone are not a sufficient behavioral specification.

The SDK is a read-only reference, not a build or test dependency. Tests use
source-only bare-metal fixtures and QEMU-native test devices. The three boards
share these chip limitations; their different LED/key wiring does not change
the conclusions. Detailed timing and unsupported-access policies remain in
:doc:`bk7258`.

Implemented and exercised subsets
--------------------------------

The entries below describe existing native qtest and/or real Cortex-M33 guest
coverage. They do not imply that every function in the linked SDK driver can
run unchanged.

.. list-table:: Ordinary sequences and current coverage
   :header-rows: 1
   :widths: 16 42 42

   * - Block
     - SDK sequence checked
     - Model and regression scope
   * - UART
     - Frame/divider/threshold setup, directional FIFO flush and W1C.
       See the UART source links in :doc:`bk7258`.
     - XTAL-derived TX/RX, 5--8-bit accepted RX payload, FIFO/IRQ routing,
       reset and clock loss. Queued RX bytes retain their accepted width.
   * - RTC / CKMN
     - RTC split-read retry and high/low threshold writes; CKMN's documented
       64 ROSC/measured-clock cycles (2 ms at 32 kHz). See RTC/CKMN provenance in
       :doc:`bk7258`.
     - Counter, threshold and IRQ54; timed ratio and IRQ21/W1C. CKMN guest
       uses independent RTC/TIMG deadlines and a missing-route negative case.
       Extreme virtual-time deadline tests are robustness checks, not a
       common firmware obstacle or measured oscillator behavior.
   * - MBOX0
     - `Mailbox source`_: channel allocation, TDATA then TID submission,
       SID then RDATA receive, status acknowledgement.
     - Eight shared slots and three physical channels, ordering, overflow,
       ownership checks and real three-core IRQ exchange.
   * - I2C
     - `I2C driver`_ and `I2C LL`_: address phases, FIFO threshold changes,
       same-address repeated START, final ACK/NAK and combined W0C/STOP.
     - Two native I2C buses, real EEPROM data, segmented IRQ-driven transfers,
       address NAK, missing SYS route, clock pause and reset cancellation.
       The matrix described below exercises complete bounded sequences.
   * - SPI
     - `SPI transmit path`_ and `SPI ISR`_: TX-only and equal-length 8-bit
       full-duplex enable/service/finish/disable, status read then W1C.
     - Two native SSI buses, test-part responses, FIFO/error/finish IRQ,
       MSB/LSB adapter, clock pause and reset. The optional ideal APLL input
       is explicitly separate from physical PLL conformance.
   * - DMA
     - `DMA copy path`_ and `DMA HAL`_: single block, memory request, equal
       widths, incrementing addresses, length-minus-one, enable and completion.
     - RAM-only nonsecure, unprivileged transfers on both units, all eight
       channels, half/finish IRQ, partial bus-error progress and cancellation.
       Full driver initialization has a separate unsupported attribute write.
   * - TIMG
     - `Timer LL`_: period/divider/enable, write-one status clear, snapshot
       request followed by polling and result read.
     - Two three-counter groups, divide-by-1..16, count/snapshot/W1C and real
       IRQ3/13. CDC, source topology and edge cases use documented policies.
   * - PWM counters
     - `PWM setup`_: selected V1PX implementation, ARR=period-1, PSC, compare,
       preload and UG. `PWM ISR`_ reads status then clears the observed bits.
     - First three counter/compare channels per unit, update/compare IRQ,
       preload and reset. Output-mode and pad-waveform initialization is
       outside this subset; full SDK PWM output is not established.
   * - Flash / NOR
     - `Flash read/write`_ and `Flash LL`_: aligned 32-byte command blocks,
       eight FIFO words, busy polling, and erase commands.
     - Explicit GD25WQ64E experiment part, AND programming, protection,
       cancellation, file persistence and CRC XIP/fault execution. This does
       not identify any board's integrated memory die or prove boot/OTA.

I2C sequence matrix
-------------------

``tests/functional/arm/guest-src/bk7258/i2c_sdk.c`` adapts the pinned driver's
PIO state transitions; it does not compile the SDK or replace firmware code.
The SDK's ``i2c_ll_enable_stop()`` body is empty at this revision. STOP is
committed by the ISR's combined status write with SM_INT cleared, not by an
invented extra write attributed to the helper's name.

One matrix guest covers both controllers, 8-bit and 16-bit EEPROM memory
addresses, and lengths 1, 4, 5, 8, 9, 12, 13, 16, 17, 31, 32, 33 and 64.
Each write is read back with repeated START and checked byte-for-byte. Native
``at24c-eeprom`` devices at 0x50/0x51 have 256/512 bytes; 0x52 is absent.
These are explicit test devices, not a claim about board-mounted EEPROMs or
sensors. The normal run completes 104 successful transfers and four expected
address-NAK transfers, exactly 108 attempts and 108 completions.

Two negative variants have distinct required exits. With I2C1's SYS route
missing, I2C0 completes all 54 transfers, the first I2C1 transaction reaches
its peripheral service latch without CPU interrupt delivery, and the guest
exits 1 at its independent SysTick deadline: 55 attempts, 54 completions,
52 successes and two NAKs. With the first I2C1 target replaced by absent
address 0x52, exactly one I2C1 IRQ reports NAK and STOP completes; exit 2
requires 55 attempts, 55 completions, 52 successes and three NAKs. Other
timeouts, faults, counts or markers fail. All variants require zero CFSR/BFAR
and empty unexpected-MMIO logs.

The runner uses standard QEMU instruction-count scheduling to keep the SDK's
START/threshold read-modify-write sequence independent of host preemption.
This is a deterministic test scheduling aid, not an asserted chip execution
rate. This audit found no new model defect in the supported I2C sequence.

Further work supported by existing sources
------------------------------------------

The source evidence supports additional sequence-level validation without
inventing registers, timing or board devices. A useful next increment is the
ordinary Flash buffer algorithm: the SDK fills each 32-byte staging buffer
with 0xff, overlays only the requested bytes, writes eight FIFO words, and
programs the aligned block. It does not read-modify-write the old array.
Existing tests exercise the controller's fixed-size operations and NOR AND
semantics; a guest adapting this full algorithm could cover non-word-aligned
requests, multiple blocks and preservation of previously programmed neighbors.
Payload, untouched neighbors, completion and a rejected/error path must all
be checked. No new flash identity or physical program latency is needed.

The reviewed SPI source also permits longer TX-only/equal-length duplex FIFO
service tests with existing native SSI devices. These are potential coverage
improvements, not evidence of a model failure. DMA maximum length, widths,
address modes, partial faults and rearming already have targeted tests; adding
more length-only entries would not establish its missing peripheral handshake.

Evidence still needed before extending behavior
-----------------------------------------------

.. list-table:: Remaining normal-path gates
   :header-rows: 1
   :widths: 18 42 40

   * - Area
     - Exact gap, despite existing register definitions
     - Consequence and minimum useful evidence
   * - Core / reset / security
     - SYS reset/release polarity is established. Its per-core reset signal's
       relationship to NVIC, SysTick, STAR cache/TCM/security state is not.
       Generic Cortex-M33 reset values do not prove the SoC's reset wiring.
     - AP restart and secure firmware compatibility remain limited. Supply
       vendor core configuration and a reset-domain table, or a controlled
       before/after trace whose access and reset safety is already known.
   * - MBOX0 held reset
     - Register fields and normal ordering are known. Reset priority versus
       ownership-error latching for accesses while reset is asserted is not.
     - Leave the model unchanged pending a precise reset/access contract.
       A possible model consistency concern is not a confirmed silicon bug.
   * - UART / GPIO
     - UART's 120-MHz parent, conflicting parity encoding comments, FIFO mode
       and alternate-function pad ownership under reset/source loss.
     - Byte-console tests remain useful on all boards. Need clock routing and
       pad/framing behavior before claiming physical serial conformance.
   * - DMA initialization
     - `DMA initialization`_ writes ``privileged_attr=0xfff`` when releasing
       reset; ``secure_attr`` is conditional on CONFIG_SPE. The model rejects
       nonzero security/privilege/allocation values. The generated 12-bit
       attribute mask also needs reconciliation with eight active channels.
     - RAM copy tests do not prove unchanged full SDK initialization. Need
       attribute polarity, bus attribution/enforcement and valid channel
       mapping. Accepting and ignoring these writes would hide a real gap.
   * - DMA peripherals / clock
     - Request enums exist; peripheral request assertion/acknowledgement,
       tail-byte strobes, HCLK routing and arbitration are not established.
     - UART/SPI/I2C DMA paths remain unsupported. Need endpoint handshake and
       clock contracts; fixed-ready responses are not a substitute.
   * - SPI receive-only
     - `SPI receive path`_ enables RX without TX. Current model requires TX,
       and enabled RX must have equal length. RX-only dummy bits, CS lifetime
       and completion behavior cannot be inferred from that call alone.
     - The whole ``bk_spi_transmit`` API is not covered by TX tests. Need the
       controller's RX-only transaction definition or an authoritative trace;
       16-bit, 3-wire, slave and DMA modes also remain unsupported.
   * - TIMG / PWM
     - CLK32 netlist and measured CDC/reset edges; PWM counter inventory
       conflicts, output mode, polarity, capture and dead-time semantics.
     - Counter/IRQ tests do not prove PWM waveforms on any board. Need exact
       BK7258 V1PX configuration and output/capture contract before expansion.
   * - RTC / AON
     - Threshold rewrite synchronization is documented, but detailed compare
       edges, digital-only reset retention and R7A reset-cause capture/clear
       are not fully established by accessors or decode code.
     - Deep sleep/wakeup and reset-cause lifecycle are not accepted. Need
       reset-domain and synchronization rules, not just more field names.
   * - Flash / PSRAM / boards
     - Capacity does not identify integrated dies. PSRAM command packing,
       completion/error/reset and source routing remain partly unresolved.
       Flash OTP/bypass/remap and reset-ROM entry are outside the current part.
     - Need exact fitted NOR/PSRAM parts and controller contracts. Memory
       datasheets plus board/module BOMs are more useful than capacity labels.

This is a bounded review of the named paths, not a declaration that all SDK
evidence or all useful chip work has been exhausted. New evidence should
refine an operation's contract before a model is widened. Do not ask users to
blindly read registers: read side effects, security access and reset impact
must be established before proposing a hardware capture procedure.

Pinned source locations
-----------------------

.. _Mailbox source: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/cp/middleware/soc/common/hal/mbox0_hal.c#L343-L464
.. _I2C driver: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/driver/i2c/i2c_driver.c#L335-L462
.. _I2C LL: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/soc/bk7258_ap/hal/i2c_ll.h#L229-L493
.. _SPI transmit path: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/driver/spi/spi_driver.c#L743-L780
.. _SPI receive path: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/driver/spi/spi_driver.c#L783-L818
.. _SPI ISR: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/driver/spi/spi_driver.c#L1128-L1194
.. _DMA copy path: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/driver/general_dma/dma_driver.c#L1132-L1191
.. _DMA HAL: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/soc/common/hal/dma_hal.c#L30-L90
.. _DMA initialization: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/soc/bk7258_ap/hal/dma_ll.h#L67-L76
.. _Timer LL: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/cp/middleware/soc/bk7258/hal/timer_ll.h#L168-L235
.. _PWM setup: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/driver/pwm/v1px/pwm_driver.c#L438-L589
.. _PWM ISR: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/ap/middleware/driver/pwm/v1px/pwm_driver.c#L2101-L2139
.. _Flash read/write: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/cp/middleware/driver/flash/flash_driver.c#L354-L460
.. _Flash LL: https://github.com/Embracecactus/bk_avdk_smp/blob/4ca389311a7ef641f10b94d298dc07ee16b0f79c/cp/middleware/soc/bk7258/hal/flash_ll.h#L207-L282
