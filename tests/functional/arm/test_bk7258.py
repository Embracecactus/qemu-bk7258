#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# AI-assisted downstream experiment, not an upstream contribution.

"""Execute trusted, source-built three-core fixtures without an OS checkout."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from qemu_test import QemuSystemTest, wait_for_console_pattern


class BK7258Machine(QemuSystemTest):
    HARDWARE_MARKERS = (
        "BK7258 SRAM ALIASES OK",
        "BK7258 UART CLOCK GATE OK",
        "BK7258 WATCHDOG NMI OK",
        "BK7258 WATCHDOG CLOCK LOSS OK",
        "BK7258 SYSTICK IRQ OK",
        "BK7258 EXTERNAL 32K SYSTICK OK",
        "BK7258 CPU1 CPU2 RELEASE AND PRIVATE TCM OK",
        "BK7258 HALT RESUME AND RESET OK",
        "BK7258 MAILBOX THREE CORE IRQ AND PROTECTION OK",
    )

    def build_fixture(
        self, board, filename, linker="diagnostic.ld", **defines
    ):
        self.require_accelerator("tcg")
        self.set_machine(board)
        compiler = os.environ.get("BK7258_TEST_CC") or shutil.which(
            "arm-none-eabi-gcc"
        )
        if compiler is None:
            self.skipTest("arm-none-eabi-gcc or BK7258_TEST_CC is required")
        self.fixture_compiler = compiler
        source = Path(__file__).parent / "guest-src" / "bk7258"
        elf = Path(self.scratch_file(Path(filename).stem + ".elf"))
        version = subprocess.run(
            [compiler, "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        subprocess.run(
            [
                compiler,
                "-mcpu=cortex-m33",
                "-mthumb",
                "-ffreestanding",
                "-nostdlib",
                "-Os",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-Wl,-T,{source / linker}",
                str(source / filename),
                *(f"-D{name}={value}" for name, value in defines.items()),
                "-o",
                str(elf),
            ],
            check=True,
            capture_output=True,
            timeout=30,
        )
        inputs = [source / filename, source / linker, elf]
        if filename.endswith(".c"):
            inputs.append(source / "uart_console.h")
        hashes = {
            str(path.name): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in inputs
        }
        Path(self.log_file("fixture-inputs.json")).write_text(
            json.dumps(
                {
                    "board": board,
                    "defines": defines,
                    "compiler_version": version.stdout.splitlines()[0],
                    "sha256": hashes,
                    "qemu_sha256": hashlib.sha256(
                        Path(self.qemu_bin).read_bytes()
                    ).hexdigest(),
                    "scope": "bare-metal chip/CPU/IRQ only; no OS boot",
                },
                indent=2,
            )
            + "\n"
        )

        return elf

    def launch_fixture(
        self, elf, console_index=0, accelerator="tcg", qmp=False
    ):
        mmio = Path(self.log_file("mmio.log"))
        # Short semihosting guests can exit before the monitor connects.
        # Interactive fixtures opt in and wait for a final host acknowledgement.
        self.vm.set_qmp_monitor(qmp)
        self.vm.set_console(console_index=console_index)
        self.vm.add_args(
            "-accel",
            accelerator,
            "-kernel",
            str(elf),
            "-semihosting-config",
            "enable=on,target=native",
            "-d",
            "guest_errors,unimp",
            "-D",
            str(mmio),
        )
        self.vm.launch()
        self.vm.console_socket.settimeout(10)
        return mmio

    def run_aidk_gpio(self, missing_route=False):
        elf = self.build_fixture(
            "aidk_ai_toy", "aidk_gpio.c", MISSING_ROUTE=int(missing_route)
        )
        mmio = self.launch_fixture(
            elf, accelerator="tcg,thread=single", qmp=True
        )
        failure = "BK7258 AIDK GPIO PROBE FAILED"
        contacts = (
            ("key1-pressed", 13, True, False),
            ("key2-pressed", 12, False, True),
            ("user-key-pressed", 8, True, True),
        )
        if missing_route:
            contacts = contacts[:1]
        for index, (contact, pin, red, green) in enumerate(contacts):
            wait_for_console_pattern(
                self, f"BK7258 AIDK GPIO READY {pin:08x}", failure
            )
            self.assertFalse(self.vm.cmd(
                "qom-get", path="/machine", property=contact
            ))
            self.vm.cmd(
                "qom-set", path="/machine", property=contact, value=True
            )
            self.assertTrue(self.vm.cmd(
                "qom-get", path="/machine", property=contact
            ))
            if missing_route:
                output = wait_for_console_pattern(
                    self, " SYS ROUTE DEADLINE", failure
                )
                sample = re.search(
                    rb"BK7258 AIDK GPIO BLOCKED ([0-9a-f]{8}) "
                    rb"([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})",
                    output,
                )
                self.assertIsNotNone(sample)
                values = tuple(int(value, 16) for value in sample.groups())
                # The peripheral latched P13, but no IRQ arrived by the
                # independent SysTick deadline while its SYS route was absent.
                self.assertEqual(values[:3], (pin, 1 << pin, 0))
                self.assertGreaterEqual(values[3], 25)
                red = green = False
            else:
                wait_for_console_pattern(
                    self,
                    f"BK7258 AIDK GPIO EVENT {pin:08x} {1 << pin:08x} "
                    f"00000047 {index + 1:08x}",
                    failure,
                )
            self.assertEqual(self.vm.cmd(
                "qom-get", path="/machine", property="user-led-on"
            ), red)
            self.assertEqual(self.vm.cmd(
                "qom-get", path="/machine", property="led2-on"
            ), green)
            self.vm.cmd(
                "qom-set", path="/machine", property=contact, value=False
            )
            self.assertFalse(self.vm.cmd(
                "qom-get", path="/machine", property=contact
            ))
            self.vm.console_socket.sendall(b"R")
            wait_for_console_pattern(
                self, f"BK7258 AIDK GPIO RELEASED {pin:08x}", failure
            )
        wait_for_console_pattern(self, "BK7258 AIDK GPIO EXIT READY", failure)
        for led in ("user-led-on", "led2-on"):
            self.assertFalse(self.vm.cmd(
                "qom-get", path="/machine", property=led
            ))
        # All monitor requests finish before semihosting may exit QEMU.
        self.vm.console_socket.sendall(b"X")
        if missing_route:
            wait_for_console_pattern(
                self, "BK7258 AIDK GPIO EXPECTED SYS ROUTE FAILURE", failure
            )
        marker = failure if missing_route else "BK7258 AIDK GPIO PROBE OK"
        wait_for_console_pattern(self, marker)
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(missing_route))
        self.assertEqual(mmio.read_bytes(), b"")

    def test_aidk_ai_toy_gpio(self):
        self.run_aidk_gpio()

    def test_aidk_ai_toy_gpio_missing_sys_route(self):
        self.run_aidk_gpio(missing_route=True)

    def run_gpio_reset(self, board):
        elf = self.build_fixture(board, "gpio_reset.c")
        mmio = self.launch_fixture(elf, accelerator="tcg,thread=single")
        output = wait_for_console_pattern(self, " END")
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0, output.decode())
        self.assertEqual(
            re.findall(rb"BK7258 GPIO RESET [^\r\n]+", output),
            [
                b"BK7258 GPIO RESET HIGH EDGE ARMED",
                b"BK7258 GPIO RESET RETAINED RISE OK",
                b"BK7258 GPIO RESET REAL IRQ55 OK",
                b"BK7258 GPIO RESET RETAINED FALL OK",
                b"BK7258 GPIO RESET RETAINED HIGH LEVEL OK",
                b"BK7258 GPIO RESET RETAINED LOW LEVEL OK",
                b"BK7258 GPIO RESET UNLOCKED DEFAULT OK",
                b"BK7258 GPIO RESET RESULT 00000000 END",
            ],
        )
        self.assertEqual(mmio.read_bytes(), b"")

    def test_t5_board_gpio_retained_reset(self):
        self.run_gpio_reset("t5_board")

    def test_t5ai_core_gpio_retained_reset(self):
        self.run_gpio_reset("t5ai_core")

    def test_aidk_ai_toy_gpio_retained_reset(self):
        self.run_gpio_reset("aidk_ai_toy")

    def run_core_clock(self, board, enabled=True, omit_dpll=False):
        elf = self.build_fixture(
            board, "core_clock.c", OMIT_DPLL=int(omit_dpll)
        )
        if enabled:
            self.vm.add_args(
                "-global", "bk7258-soc.experimental-core-clocks=on"
            )
        # Instruction counting makes RTC timestamps repeatable; the guest
        # tests SysTick exception timing, never TCG instruction throughput.
        self.vm.add_args("-icount", "shift=0,align=off,sleep=off")
        mmio = self.launch_fixture(elf, accelerator="tcg,thread=single")
        positive = enabled and not omit_dpll
        marker = (
            "BK7258 THREE CORE CLOCK SYSTICK IRQ OK" if positive else
            "BK7258 CORE CLOCK XTAL DEADLINE FAILED" if omit_dpll else
            "BK7258 CORE CLOCK RTC TIMING FAILED"
        )
        output = wait_for_console_pattern(
            self, marker, "BK7258 CORE CLOCK PROBE FAILED"
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0 if positive else 1)
        samples = re.findall(
            rb"BK7258 CORE CLOCK SAMPLE ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) ([0-9a-f]{8})", output
        )
        if positive:
            self.assertEqual(
                [(int(test, 16), int(core, 16))
                 for test, core, _ in samples],
                [(test, core) for test in range(8) for core in range(3)],
            )
        else:
            self.assertNotIn(b"BK7258 THREE CORE CLOCK SYSTICK IRQ OK", output)
            # These are executed guest negative controls, not launcher errors.
            if omit_dpll:
                self.assertEqual(samples, [])
            else:
                self.assertEqual(len(samples), 1)
                self.assertEqual(samples[0][:2], (b"00000000", b"00000000"))
                self.assertGreater(int(samples[0][2], 16), 300 * 32)

        # Clock programming has deliberate, immediately visible intermediate
        # tuples. No unrelated unimplemented access or guest error is allowed.
        permitted = {
            "bk7258-sys: experimental core clock tuple "
            f"mode=0x{mode:02x} speeds=0x{speeds:x} is unsupported"
            for mode, speeds in ((0, 1), (0, 3), (0, 6), (0x20, 7), (0x73, 7))
        }
        diagnostics = mmio.read_text().splitlines()
        for diagnostic in diagnostics:
            self.assertIn(diagnostic, permitted)
        if positive:
            self.assertEqual(
                diagnostics.count(
                    "bk7258-sys: experimental core clock tuple "
                    "mode=0x73 speeds=0x7 is unsupported"
                ),
                3,
            )
        elif not enabled:
            self.assertEqual(diagnostics, [])

    def test_t5_board_core_clock(self):
        self.run_core_clock("t5_board")

    def test_t5_board_core_clock_disabled(self):
        self.run_core_clock("t5_board", enabled=False)

    def test_t5_board_core_clock_missing_dpll(self):
        self.run_core_clock("t5_board", omit_dpll=True)

    def test_t5ai_core_core_clock(self):
        self.run_core_clock("t5ai_core")

    def test_t5ai_core_core_clock_disabled(self):
        self.run_core_clock("t5ai_core", enabled=False)

    def test_t5ai_core_core_clock_missing_dpll(self):
        self.run_core_clock("t5ai_core", omit_dpll=True)

    def test_aidk_ai_toy_core_clock(self):
        self.run_core_clock("aidk_ai_toy")

    def test_aidk_ai_toy_core_clock_disabled(self):
        self.run_core_clock("aidk_ai_toy", enabled=False)

    def test_aidk_ai_toy_core_clock_missing_dpll(self):
        self.run_core_clock("aidk_ai_toy", omit_dpll=True)

    def run_fixture(self, board, positive):
        elf = self.build_fixture(board, "diagnostic.c")
        mmio = self.launch_fixture(elf)
        before = wait_for_console_pattern(
            self, "BK7258 WAIT RX", "BK7258 FAULT"
        )
        for marker in self.HARDWARE_MARKERS:
            self.assertIn(marker.encode(), before)
        self.vm.console_socket.sendall(b"Z" if positive else b"X")
        if positive:
            after = wait_for_console_pattern(
                self, "BK7258 DIAGNOSTIC PASS", "BK7258 FAULT"
            )
            for marker in self.HARDWARE_MARKERS + (
                "BK7258 UART RX IRQ OK",
                "BK7258 SYSTEM RESET REQUEST",
                "BK7258 SYSTEM RESET OK",
            ):
                self.assertIn(marker.encode(), after)
        else:
            after = wait_for_console_pattern(self, "BK7258 FAULT")
            self.assertNotIn(b"BK7258 DIAGNOSTIC PASS", after)
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0 if positive else 1)
        self.assertEqual(mmio.read_bytes(), b"")

    def run_sys_fault(self, board, write, flash_config=False):
        fields = {"WRITE_PROBE": int(write)}
        if flash_config:
            fields.update(PROBE_ADDRESS="0x44010044u", PRESERVED_VALUE="0x80u")
        elf = self.build_fixture(board, "sys_fault.c", **fields)
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 SYS ACCESS FAULT OK",
            "BK7258 SYS ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        operation = "write offset 0xa0" if write else "read offset 0x0"
        expected = ("bk7258-sys: unsupported SYS2Flash configuration\n"
                    if flash_config else
                    f"bk7258-sys: {operation} is not implemented\n")
        self.assertEqual(mmio.read_text(), expected)

    def test_t5_board_sys_flash_unsupported_control(self):
        self.run_sys_fault("t5_board", True, flash_config=True)

    def test_t5ai_core_sys_flash_unsupported_control(self):
        self.run_sys_fault("t5ai_core", True, flash_config=True)

    def test_aidk_ai_toy_sys_flash_unsupported_control(self):
        self.run_sys_fault("aidk_ai_toy", True, flash_config=True)

    def run_uart_tx(self, board, index, missing_route=False, full_fifo=False):
        elf = self.build_fixture(
            board,
            "uart_tx.c",
            INDEX=index,
            INDEX_TEXT=f'"{index}"',
            MISSING_ROUTE=int(missing_route),
            FULL_FIFO=int(full_fifo),
        )
        tx_file = Path(self.log_file("uart-tx.bin"))
        if index:
            # launch_fixture adds serial0's console; these become ports 1/2.
            self.vm.set_console()
            for port in range(1, index + 1):
                self.vm.add_args(
                    "-serial", f"file:{tx_file}" if port == index else "null"
                )
        mmio = self.launch_fixture(elf)
        if missing_route:
            wait_for_console_pattern(
                self,
                "BK7258 MISSING UART ROUTE DETECTED",
                "BK7258 UART TX PROBE FAILED",
            )
        wait_for_console_pattern(
            self,
            (
                "BK7258 UART TX PROBE FAILED"
                if missing_route
                else (
                    "BK7258 UART FIFO PRECISE FAULT OK"
                    if full_fifo
                    else "BK7258 UART TX FIFO AND IRQ OK"
                )
            ),
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(missing_route))
        self.assertEqual(
            mmio.read_text(),
            "bk7258-uart: TX FIFO full\n" if full_fifo else "",
        )
        payload = b"" if full_fifo else f"U{index}:A\n".encode()
        if not full_fifo and not missing_route:
            payload += f"s{index}:B\n".encode()
        if index:
            self.assertEqual(tx_file.read_bytes(), payload)
        elif payload:
            # The console logger has timestamps; test both complete lines.
            console = Path(self.console_log_name).read_text()
            for line in payload.decode().splitlines():
                self.assertIn(line, console)

    def test_t5_board_uart0_tx(self):
        self.run_uart_tx("t5_board", 0)

    def run_uart_rx_width(self, board, index):
        elf = self.build_fixture(board, "uart_rx_width.c", INDEX=index)
        mmio = self.launch_fixture(elf, console_index=index)
        for bits in range(5, 9):
            # The guest configures RX before emitting the final newline.
            wait_for_console_pattern(
                self,
                f"BK7258 UART RX WIDTH {bits}\n",
                "BK7258 UART RX WIDTH FAILED",
            )
            self.vm.console_socket.sendall(bytes([0xff, 0x80, 0xe5, 0x55]))
        wait_for_console_pattern(
            self,
            "BK7258 UART RX WIDTH AND SNAPSHOT OK",
            "BK7258 UART RX WIDTH FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        self.assertEqual(mmio.read_bytes(), b"")

    def test_t5_board_uart0_rx_width(self):
        self.run_uart_rx_width("t5_board", 0)

    def test_t5_board_uart1_rx_width(self):
        self.run_uart_rx_width("t5_board", 1)

    def test_t5_board_uart2_rx_width(self):
        self.run_uart_rx_width("t5_board", 2)

    def test_t5ai_core_uart0_rx_width(self):
        self.run_uart_rx_width("t5ai_core", 0)

    def test_t5ai_core_uart1_rx_width(self):
        self.run_uart_rx_width("t5ai_core", 1)

    def test_t5ai_core_uart2_rx_width(self):
        self.run_uart_rx_width("t5ai_core", 2)

    def test_aidk_ai_toy_uart0_rx_width(self):
        self.run_uart_rx_width("aidk_ai_toy", 0)

    def test_aidk_ai_toy_uart1_rx_width(self):
        self.run_uart_rx_width("aidk_ai_toy", 1)

    def test_aidk_ai_toy_uart2_rx_width(self):
        self.run_uart_rx_width("aidk_ai_toy", 2)

    def test_t5_board_uart0_tx_missing_route(self):
        self.run_uart_tx("t5_board", 0, missing_route=True)

    def test_t5_board_uart0_tx_full_fifo(self):
        self.run_uart_tx("t5_board", 0, full_fifo=True)

    def test_t5_board_uart1_tx(self):
        self.run_uart_tx("t5_board", 1)

    def test_t5_board_uart1_tx_missing_route(self):
        self.run_uart_tx("t5_board", 1, missing_route=True)

    def test_t5_board_uart1_tx_full_fifo(self):
        self.run_uart_tx("t5_board", 1, full_fifo=True)

    def test_t5_board_uart2_tx(self):
        self.run_uart_tx("t5_board", 2)

    def test_t5_board_uart2_tx_missing_route(self):
        self.run_uart_tx("t5_board", 2, missing_route=True)

    def test_t5_board_uart2_tx_full_fifo(self):
        self.run_uart_tx("t5_board", 2, full_fifo=True)

    def test_t5ai_core_uart0_tx(self):
        self.run_uart_tx("t5ai_core", 0)

    def test_t5ai_core_uart0_tx_missing_route(self):
        self.run_uart_tx("t5ai_core", 0, missing_route=True)

    def test_t5ai_core_uart0_tx_full_fifo(self):
        self.run_uart_tx("t5ai_core", 0, full_fifo=True)

    def test_t5ai_core_uart1_tx(self):
        self.run_uart_tx("t5ai_core", 1)

    def test_t5ai_core_uart1_tx_missing_route(self):
        self.run_uart_tx("t5ai_core", 1, missing_route=True)

    def test_t5ai_core_uart1_tx_full_fifo(self):
        self.run_uart_tx("t5ai_core", 1, full_fifo=True)

    def test_t5ai_core_uart2_tx(self):
        self.run_uart_tx("t5ai_core", 2)

    def test_t5ai_core_uart2_tx_missing_route(self):
        self.run_uart_tx("t5ai_core", 2, missing_route=True)

    def test_t5ai_core_uart2_tx_full_fifo(self):
        self.run_uart_tx("t5ai_core", 2, full_fifo=True)

    def test_aidk_ai_toy_uart0_tx(self):
        self.run_uart_tx("aidk_ai_toy", 0)

    def test_aidk_ai_toy_uart0_tx_missing_route(self):
        self.run_uart_tx("aidk_ai_toy", 0, missing_route=True)

    def test_aidk_ai_toy_uart0_tx_full_fifo(self):
        self.run_uart_tx("aidk_ai_toy", 0, full_fifo=True)

    def test_aidk_ai_toy_uart1_tx(self):
        self.run_uart_tx("aidk_ai_toy", 1)

    def test_aidk_ai_toy_uart1_tx_missing_route(self):
        self.run_uart_tx("aidk_ai_toy", 1, missing_route=True)

    def test_aidk_ai_toy_uart1_tx_full_fifo(self):
        self.run_uart_tx("aidk_ai_toy", 1, full_fifo=True)

    def test_aidk_ai_toy_uart2_tx(self):
        self.run_uart_tx("aidk_ai_toy", 2)

    def test_aidk_ai_toy_uart2_tx_missing_route(self):
        self.run_uart_tx("aidk_ai_toy", 2, missing_route=True)

    def test_aidk_ai_toy_uart2_tx_full_fifo(self):
        self.run_uart_tx("aidk_ai_toy", 2, full_fifo=True)

    def run_uart_fault(self, board, index, write):
        base = (0x44820000, 0x45830000, 0x45840000)[index]
        elf = self.build_fixture(
            board,
            "sys_fault.c",
            WRITE_PROBE=int(write),
            PROBE_ADDRESS=hex(base + (0x10000018 if write else 0)),
            PROBE_NAME='"UART"',
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 UART ACCESS FAULT OK",
            "BK7258 UART ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        operation = "write offset 0x18" if write else "read offset 0x0"
        self.assertEqual(
            mmio.read_text(), f"bk7258-uart: {operation} is not implemented\n"
        )

    def test_t5_board_uart0_invalid_read(self):
        self.run_uart_fault("t5_board", 0, False)

    def test_t5_board_uart0_invalid_write(self):
        self.run_uart_fault("t5_board", 0, True)

    def test_t5_board_uart1_invalid_read(self):
        self.run_uart_fault("t5_board", 1, False)

    def test_t5_board_uart1_invalid_write(self):
        self.run_uart_fault("t5_board", 1, True)

    def test_t5_board_uart2_invalid_read(self):
        self.run_uart_fault("t5_board", 2, False)

    def test_t5_board_uart2_invalid_write(self):
        self.run_uart_fault("t5_board", 2, True)

    def test_t5ai_core_uart0_invalid_read(self):
        self.run_uart_fault("t5ai_core", 0, False)

    def test_t5ai_core_uart0_invalid_write(self):
        self.run_uart_fault("t5ai_core", 0, True)

    def test_t5ai_core_uart1_invalid_read(self):
        self.run_uart_fault("t5ai_core", 1, False)

    def test_t5ai_core_uart1_invalid_write(self):
        self.run_uart_fault("t5ai_core", 1, True)

    def test_t5ai_core_uart2_invalid_read(self):
        self.run_uart_fault("t5ai_core", 2, False)

    def test_t5ai_core_uart2_invalid_write(self):
        self.run_uart_fault("t5ai_core", 2, True)

    def test_aidk_ai_toy_uart0_invalid_read(self):
        self.run_uart_fault("aidk_ai_toy", 0, False)

    def test_aidk_ai_toy_uart0_invalid_write(self):
        self.run_uart_fault("aidk_ai_toy", 0, True)

    def test_aidk_ai_toy_uart1_invalid_read(self):
        self.run_uart_fault("aidk_ai_toy", 1, False)

    def test_aidk_ai_toy_uart1_invalid_write(self):
        self.run_uart_fault("aidk_ai_toy", 1, True)

    def test_aidk_ai_toy_uart2_invalid_read(self):
        self.run_uart_fault("aidk_ai_toy", 2, False)

    def test_aidk_ai_toy_uart2_invalid_write(self):
        self.run_uart_fault("aidk_ai_toy", 2, True)

    def run_i2c_empty_fault(self, board, index):
        elf = self.build_fixture(
            board,
            "sys_fault.c",
            WRITE_PROBE=0,
            PROBE_ADDRESS=hex(0x45850018 + index * 0x10000),
            PROBE_NAME='"I2C"',
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 I2C ACCESS FAULT OK",
            "BK7258 I2C ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        self.assertEqual(
            mmio.read_text(), "bk7258-i2c: unsupported read at 0x18\n"
        )

    def test_t5_board_i2c0_empty_read_fault(self):
        self.run_i2c_empty_fault("t5_board", 0)

    def test_t5_board_i2c1_empty_read_fault(self):
        self.run_i2c_empty_fault("t5_board", 1)

    def test_t5ai_core_i2c0_empty_read_fault(self):
        self.run_i2c_empty_fault("t5ai_core", 0)

    def test_t5ai_core_i2c1_empty_read_fault(self):
        self.run_i2c_empty_fault("t5ai_core", 1)

    def test_aidk_ai_toy_i2c0_empty_read_fault(self):
        self.run_i2c_empty_fault("aidk_ai_toy", 0)

    def test_aidk_ai_toy_i2c1_empty_read_fault(self):
        self.run_i2c_empty_fault("aidk_ai_toy", 1)

    def run_i2c(self, board, missing_route, noop=False):
        elf = self.build_fixture(
            board, "i2c.c", MISSING_ROUTE=int(missing_route),
            NOOP_PROBE=int(noop),
        )
        if noop:
            self.vm.add_args("-icount", "shift=0,align=off,sleep=off")
        for bus in ("i2c0", "i2c1"):
            self.vm.add_args(
                "-device", f"at24c-eeprom,bus={bus},address=0x50,rom-size=256"
            )
        mmio = self.launch_fixture(elf)
        if noop:
            wait_for_console_pattern(
                self,
                "BK7258 I2C UNCHANGED CLOCK PROGRESS OK",
                "BK7258 I2C PROBE FAILED",
            )
        if missing_route:
            wait_for_console_pattern(
                self,
                "BK7258 MISSING I2C1 ROUTE DETECTED",
                "BK7258 I2C PROBE FAILED",
            )
        wait_for_console_pattern(
            self,
            (
                "BK7258 I2C PROBE FAILED"
                if missing_route
                else "BK7258 I2C BUS IRQ RESTART NAK OK"
            ),
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(missing_route))
        self.assertEqual(mmio.read_bytes(), b"")

    def test_t5_board_i2c(self):
        self.run_i2c("t5_board", False)

    def test_t5_board_i2c_unchanged_clock(self):
        self.run_i2c("t5_board", False, noop=True)

    def test_t5ai_core_i2c_unchanged_clock(self):
        self.run_i2c("t5ai_core", False, noop=True)

    def test_aidk_ai_toy_i2c_unchanged_clock(self):
        self.run_i2c("aidk_ai_toy", False, noop=True)

    def test_t5_board_i2c_missing_route(self):
        self.run_i2c("t5_board", True)

    def test_t5ai_core_i2c(self):
        self.run_i2c("t5ai_core", False)

    def test_t5ai_core_i2c_missing_route(self):
        self.run_i2c("t5ai_core", True)

    def test_aidk_ai_toy_i2c(self):
        self.run_i2c("aidk_ai_toy", False)

    def test_aidk_ai_toy_i2c_missing_route(self):
        self.run_i2c("aidk_ai_toy", True)

    def run_spi_empty_fault(self, board, index):
        elf = self.build_fixture(
            board,
            "sys_fault.c",
            WRITE_PROBE=0,
            PROBE_ADDRESS=hex(0x4487001C + index * 0x1010000),
            PROBE_NAME='"SPI"',
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 SPI ACCESS FAULT OK",
            "BK7258 SPI ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        self.assertEqual(
            mmio.read_text(), "bk7258-spi: unsupported read at 0x1c\n"
        )

    def test_t5_board_spi0_empty_read_fault(self):
        self.run_spi_empty_fault("t5_board", 0)

    def test_t5_board_spi1_empty_read_fault(self):
        self.run_spi_empty_fault("t5_board", 1)

    def test_t5ai_core_spi0_empty_read_fault(self):
        self.run_spi_empty_fault("t5ai_core", 0)

    def test_t5ai_core_spi1_empty_read_fault(self):
        self.run_spi_empty_fault("t5ai_core", 1)

    def test_aidk_ai_toy_spi0_empty_read_fault(self):
        self.run_spi_empty_fault("aidk_ai_toy", 0)

    def test_aidk_ai_toy_spi1_empty_read_fault(self):
        self.run_spi_empty_fault("aidk_ai_toy", 1)

    def run_pwm_mixed_preload(self, board, unit, keep_arr):
        elf = self.build_fixture(
            board, "pwm_preload_fault.c", UNIT=unit, KEEP_ARR=int(keep_arr)
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 PWM MIXED PRELOAD FAULT ATOMIC OK",
            "BK7258 PWM MIXED PRELOAD FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        value = "24" if keep_arr else "104"
        self.assertEqual(
            mmio.read_text(),
            f"bk7258-pwm: unsupported write 0x{value} at 0x10\n",
        )

    def test_t5_board_pwm0_mixed_preload(self):
        self.run_pwm_mixed_preload("t5_board", 0, False)

    def test_t5_board_pwm1_mixed_preload(self):
        self.run_pwm_mixed_preload("t5_board", 1, True)

    def test_t5ai_core_pwm0_mixed_preload(self):
        self.run_pwm_mixed_preload("t5ai_core", 0, False)

    def test_t5ai_core_pwm1_mixed_preload(self):
        self.run_pwm_mixed_preload("t5ai_core", 1, True)

    def test_aidk_ai_toy_pwm0_mixed_preload(self):
        self.run_pwm_mixed_preload("aidk_ai_toy", 0, False)

    def test_aidk_ai_toy_pwm1_mixed_preload(self):
        self.run_pwm_mixed_preload("aidk_ai_toy", 1, True)

    def run_wdt_fault(self, board, write):
        elf = self.build_fixture(
            board,
            "sys_fault.c",
            WRITE_PROBE=int(write),
            PROBE_ADDRESS=hex(0x5480000C if write else 0x44800000),
            PROBE_NAME='"WDT"',
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 WDT ACCESS FAULT OK",
            "BK7258 WDT ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        expected = (
            "bk7258-wdt: write offset 0xc is not implemented\n"
            if write
            else "bk7258-wdt: read offset 0x0 is not implemented\n"
        )
        self.assertEqual(mmio.read_text(), expected)

    def test_t5_board_wdt_unsupported_read(self):
        self.run_wdt_fault("t5_board", False)

    def test_t5_board_wdt_unsupported_write(self):
        self.run_wdt_fault("t5_board", True)

    def test_t5ai_core_wdt_unsupported_read(self):
        self.run_wdt_fault("t5ai_core", False)

    def test_t5ai_core_wdt_unsupported_write(self):
        self.run_wdt_fault("t5ai_core", True)

    def test_aidk_ai_toy_wdt_unsupported_read(self):
        self.run_wdt_fault("aidk_ai_toy", False)

    def test_aidk_ai_toy_wdt_unsupported_write(self):
        self.run_wdt_fault("aidk_ai_toy", True)

    def run_pwm_output_fault(self, board, unit):
        elf = self.build_fixture(
            board,
            "sys_fault.c",
            WRITE_PROBE=1,
            PROBE_ADDRESS=hex(0x558A0028 + unit * 0x50000),
            PROBE_NAME='"PWM"',
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 PWM ACCESS FAULT OK",
            "BK7258 PWM ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        self.assertEqual(
            mmio.read_text(),
            "bk7258-pwm: unsupported write 0x12345678 at 0x28\n",
        )

    def test_t5_board_pwm0_unsupported_output(self):
        self.run_pwm_output_fault("t5_board", 0)

    def test_t5_board_pwm1_unsupported_output(self):
        self.run_pwm_output_fault("t5_board", 1)

    def test_t5ai_core_pwm0_unsupported_output(self):
        self.run_pwm_output_fault("t5ai_core", 0)

    def test_t5ai_core_pwm1_unsupported_output(self):
        self.run_pwm_output_fault("t5ai_core", 1)

    def test_aidk_ai_toy_pwm0_unsupported_output(self):
        self.run_pwm_output_fault("aidk_ai_toy", 0)

    def test_aidk_ai_toy_pwm1_unsupported_output(self):
        self.run_pwm_output_fault("aidk_ai_toy", 1)

    def run_pwm_counter(self, board, missing_route=False):
        elf = self.build_fixture(
            board, "pwm.c", MISSING_ROUTE=int(missing_route)
        )
        mmio = self.launch_fixture(elf)
        if missing_route:
            wait_for_console_pattern(
                self,
                "BK7258 MISSING PWM1 ROUTE DETECTED",
                "BK7258 PWM COUNTER PROBE FAILED",
            )
        wait_for_console_pattern(
            self,
            (
                "BK7258 PWM COUNTER PROBE FAILED"
                if missing_route
                else "BK7258 PWM COUNTER COMPARE IRQ OK"
            ),
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(missing_route))
        self.assertEqual(mmio.read_bytes(), b"")

    def test_t5_board_pwm_counter(self):
        self.run_pwm_counter("t5_board")

    def test_t5_board_pwm_missing_route(self):
        self.run_pwm_counter("t5_board", True)

    def test_t5ai_core_pwm_counter(self):
        self.run_pwm_counter("t5ai_core")

    def test_t5ai_core_pwm_missing_route(self):
        self.run_pwm_counter("t5ai_core", True)

    def test_aidk_ai_toy_pwm_counter(self):
        self.run_pwm_counter("aidk_ai_toy")

    def test_aidk_ai_toy_pwm_missing_route(self):
        self.run_pwm_counter("aidk_ai_toy", True)

    def run_dma(self, board, missing_route=False):
        elf = self.build_fixture(
            board, "dma.c", MISSING_ROUTE=int(missing_route)
        )
        mmio = self.launch_fixture(elf)
        if missing_route:
            wait_for_console_pattern(
                self,
                "BK7258 MISSING DMA1 ROUTE DETECTED",
                "BK7258 DMA PROBE FAILED",
            )
        wait_for_console_pattern(
            self,
            (
                "BK7258 DMA PROBE FAILED"
                if missing_route
                else "BK7258 DMA RAM IRQ ERROR ISOLATION OK"
            ),
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(missing_route))
        expected = (
            ""
            if missing_route
            else "".join(
                f"bk7258-dma: unit {unit} channel 0 bus error\n" * 4
                for unit in range(2)
            )
        )
        self.assertEqual(mmio.read_text(), expected)

    def run_dma_security_fault(self, board, unit):
        elf = self.build_fixture(
            board,
            "sys_fault.c",
            WRITE_PROBE=1,
            PROBE_ADDRESS=hex(0x55020010 + unit * 0x10000),
            PROBE_NAME='"DMA"',
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 DMA ACCESS FAULT OK",
            "BK7258 DMA ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        self.assertEqual(
            mmio.read_text(),
            "bk7258-dma: unsupported write 0x12345678 at 0x10\n",
        )

    def test_t5_board_dma(self):
        self.run_dma("t5_board")

    def test_t5_board_dma_missing_route(self):
        self.run_dma("t5_board", True)

    def test_t5_board_dma0_unsupported_security(self):
        self.run_dma_security_fault("t5_board", 0)

    def test_t5_board_dma1_unsupported_security(self):
        self.run_dma_security_fault("t5_board", 1)

    def test_t5ai_core_dma(self):
        self.run_dma("t5ai_core")

    def test_t5ai_core_dma_missing_route(self):
        self.run_dma("t5ai_core", True)

    def test_t5ai_core_dma0_unsupported_security(self):
        self.run_dma_security_fault("t5ai_core", 0)

    def test_t5ai_core_dma1_unsupported_security(self):
        self.run_dma_security_fault("t5ai_core", 1)

    def test_aidk_ai_toy_dma(self):
        self.run_dma("aidk_ai_toy")

    def test_aidk_ai_toy_dma_missing_route(self):
        self.run_dma("aidk_ai_toy", True)

    def test_aidk_ai_toy_dma0_unsupported_security(self):
        self.run_dma_security_fault("aidk_ai_toy", 0)

    def test_aidk_ai_toy_dma1_unsupported_security(self):
        self.run_dma_security_fault("aidk_ai_toy", 1)

    def run_spi_apll(self, board, enabled=True, probe_mode=0):
        elf = self.build_fixture(
            board, "spi_apll.c", PROBE_MODE=probe_mode
        )
        if enabled:
            self.vm.add_args(
                "-global", "bk7258-soc.experimental-spi-apll=on"
            )
        for bus in ("spi0", "spi1"):
            self.vm.add_args("-device", f"w25q32,bus={bus},cs=0")
        # TIMG0 observes virtual time from independent XTAL, never CPU speed.
        self.vm.add_args("-icount", "shift=0,align=off,sleep=off")
        mmio = self.launch_fixture(elf, accelerator="tcg,thread=single")
        positive = enabled and probe_mode == 0
        output = wait_for_console_pattern(
            self,
            "BK7258 SPI APLL SSI ID STATUS TIMING IRQ OK" if positive else
            "BK7258 SPI APLL XTAL DEADLINE FAILED",
            "BK7258 SPI APLL PROBE FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0 if positive else 1)
        samples = re.findall(
            rb"BK7258 SPI APLL SAMPLE ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) ([0-9a-f]{8})", output
        )
        deadlines = re.findall(
            rb"BK7258 SPI APLL DEADLINE ([0-9a-f]{8}) ([0-9a-f]{8})",
            output,
        )
        if positive:
            self.assertEqual(deadlines, [])
            self.assertEqual(
                [(int(profile, 16), int(index, 16))
                 for profile, index, _ in samples],
                [(profile, index)
                 for profile in range(2) for index in range(2)],
            )
            for profile, _, elapsed in samples:
                # 32 bytes * 8 bits * raw divider 255 * 2 source clocks,
                # measured in 26-MHz XTAL ticks; zero inter-byte interval.
                nominal = (34531, 37585)[int(profile, 16)]
                self.assertLessEqual(abs(int(elapsed, 16) - nominal), 64)
            self.assertIn(
                b"BK7258 SPI APLL INDEPENDENT GATE RESUME IRQ OK", output
            )
            self.assertIn(b"BK7258 SPI APLL TRIGGER RESUME IRQ OK", output)
        else:
            # Both controllers execute and fail in the guest, with an
            # independent 10-ms deadline and no invented receive response.
            self.assertEqual(samples, [])
            self.assertEqual(
                [int(index, 16) for index, _ in deadlines], [0, 1]
            )
            for _, elapsed in deadlines:
                self.assertGreaterEqual(int(elapsed, 16), 260000)
                # TIMG1 wakes the guest at 100-us intervals (2600 ticks).
                self.assertLess(int(elapsed, 16), 262664)
            self.assertNotIn(b"SSI ID STATUS TIMING IRQ OK", output)
        self.assertEqual(
            mmio.read_text(),
            "bk7258-sys: SPI APLL source is not implemented\n"
            if not enabled else
            "bk7258-sys: ideal SPI APLL coefficient 0x8973ca6e unsupported\n"
            * 2 if probe_mode == 3 else "",
        )

    def test_t5_board_spi_apll(self):
        self.run_spi_apll("t5_board")

    def test_t5_board_spi_apll_disabled(self):
        self.run_spi_apll("t5_board", enabled=False)

    def test_t5_board_spi_apll_missing_trigger(self):
        self.run_spi_apll("t5_board", probe_mode=1)

    def test_t5_board_spi_apll_incomplete_trigger(self):
        self.run_spi_apll("t5_board", probe_mode=2)

    def test_t5_board_spi_apll_unknown_profile(self):
        self.run_spi_apll("t5_board", probe_mode=3)

    def test_t5ai_core_spi_apll(self):
        self.run_spi_apll("t5ai_core")

    def test_t5ai_core_spi_apll_disabled(self):
        self.run_spi_apll("t5ai_core", enabled=False)

    def test_t5ai_core_spi_apll_missing_trigger(self):
        self.run_spi_apll("t5ai_core", probe_mode=1)

    def test_t5ai_core_spi_apll_incomplete_trigger(self):
        self.run_spi_apll("t5ai_core", probe_mode=2)

    def test_t5ai_core_spi_apll_unknown_profile(self):
        self.run_spi_apll("t5ai_core", probe_mode=3)

    def test_aidk_ai_toy_spi_apll(self):
        self.run_spi_apll("aidk_ai_toy")

    def test_aidk_ai_toy_spi_apll_disabled(self):
        self.run_spi_apll("aidk_ai_toy", enabled=False)

    def test_aidk_ai_toy_spi_apll_missing_trigger(self):
        self.run_spi_apll("aidk_ai_toy", probe_mode=1)

    def test_aidk_ai_toy_spi_apll_incomplete_trigger(self):
        self.run_spi_apll("aidk_ai_toy", probe_mode=2)

    def test_aidk_ai_toy_spi_apll_unknown_profile(self):
        self.run_spi_apll("aidk_ai_toy", probe_mode=3)

    def run_spi(
        self, board, missing_route=False, missing_endpoint=False, lsb=False,
        reject_index=None,
    ):
        elf = self.build_fixture(
            board,
            "spi.c",
            MISSING_ROUTE=int(missing_route),
            MISSING_ENDPOINT=int(missing_endpoint),
            LSB_FIRST=int(lsb),
            REJECT_CFG=int(reject_index is not None),
            REJECT_INDEX=reject_index or 0,
        )
        if not missing_endpoint:
            for bus in ("spi0", "spi1"):
                self.vm.add_args("-device", f"w25q32,bus={bus},cs=0")
        mmio = self.launch_fixture(elf)
        if missing_route or missing_endpoint:
            wait_for_console_pattern(
                self,
                (
                    "BK7258 MISSING SPI1 ROUTE DETECTED"
                    if missing_route
                    else "BK7258 ABSENT SPI ENDPOINT DETECTED"
                ),
                "BK7258 SPI PROBE FAILED",
            )
        expected = (
            "BK7258 SPI COMPLETED CFG PRECISE FAULT OK"
            if reject_index is not None
            else (
                "BK7258 SPI PROBE FAILED"
                if missing_route or missing_endpoint
                else (
                    "BK7258 SPI LSB SSI ID IRQ OK"
                    if lsb
                    else "BK7258 SPI SSI ID IRQ OK"
                )
            )
        )
        failure = (
            None if missing_route or missing_endpoint
            else "BK7258 SPI PROBE FAILED"
        )
        wait_for_console_pattern(self, expected, failure)
        self.vm.wait(timeout=5)
        self.assertEqual(
            self.vm.exitcode(), int(missing_route or missing_endpoint)
        )
        self.assertEqual(
            mmio.read_text(),
            "bk7258-spi: unsupported write 0x1 at 0x14\n"
            if reject_index is not None else "",
        )

    def test_t5_board_spi0_completed_config_fault(self):
        self.run_spi("t5_board", reject_index=0)

    def test_t5_board_spi1_completed_config_fault(self):
        self.run_spi("t5_board", reject_index=1)

    def test_t5ai_core_spi0_completed_config_fault(self):
        self.run_spi("t5ai_core", reject_index=0)

    def test_t5ai_core_spi1_completed_config_fault(self):
        self.run_spi("t5ai_core", reject_index=1)

    def test_aidk_ai_toy_spi0_completed_config_fault(self):
        self.run_spi("aidk_ai_toy", reject_index=0)

    def test_aidk_ai_toy_spi1_completed_config_fault(self):
        self.run_spi("aidk_ai_toy", reject_index=1)

    def test_t5_board_spi(self):
        self.run_spi("t5_board")

    def test_t5_board_spi_missing_route(self):
        self.run_spi("t5_board", missing_route=True)

    def test_t5_board_spi_missing_endpoint(self):
        self.run_spi("t5_board", missing_endpoint=True)

    def test_t5ai_core_spi(self):
        self.run_spi("t5ai_core")

    def test_t5ai_core_spi_missing_route(self):
        self.run_spi("t5ai_core", missing_route=True)

    def test_t5ai_core_spi_missing_endpoint(self):
        self.run_spi("t5ai_core", missing_endpoint=True)

    def test_aidk_ai_toy_spi(self):
        self.run_spi("aidk_ai_toy")

    def test_aidk_ai_toy_spi_missing_route(self):
        self.run_spi("aidk_ai_toy", missing_route=True)

    def test_aidk_ai_toy_spi_missing_endpoint(self):
        self.run_spi("aidk_ai_toy", missing_endpoint=True)

    def test_t5_board_spi_lsb(self):
        self.run_spi("t5_board", lsb=True)

    def test_t5_board_spi_lsb_missing_route(self):
        self.run_spi("t5_board", lsb=True, missing_route=True)

    def test_t5ai_core_spi_lsb(self):
        self.run_spi("t5ai_core", lsb=True)

    def test_t5ai_core_spi_lsb_missing_route(self):
        self.run_spi("t5ai_core", lsb=True, missing_route=True)

    def test_aidk_ai_toy_spi_lsb(self):
        self.run_spi("aidk_ai_toy", lsb=True)

    def test_aidk_ai_toy_spi_lsb_missing_route(self):
        self.run_spi("aidk_ai_toy", lsb=True, missing_route=True)

    def run_timer(self, board, missing_route=False, fault=False):
        elf = self.build_fixture(
            board,
            "timer.c",
            MISSING_ROUTE=int(missing_route),
            FAULT_PROBE=int(fault),
        )
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            (
                "BK7258 TIMER PROBE FAILED"
                if missing_route
                else "BK7258 TIMER GROUP PROBE OK"
            ),
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(missing_route))
        self.assertEqual(
            mmio.read_text(),
            "bk7258-timer: unsupported write 0xd at 0x20\n" if fault else "",
        )

    def test_t5_board_timer(self):
        self.run_timer("t5_board")

    def test_t5_board_timer_missing_route(self):
        self.run_timer("t5_board", missing_route=True)

    def test_t5_board_timer_invalid_channel(self):
        self.run_timer("t5_board", fault=True)

    def test_t5ai_core_timer(self):
        self.run_timer("t5ai_core")

    def test_t5ai_core_timer_missing_route(self):
        self.run_timer("t5ai_core", missing_route=True)

    def test_t5ai_core_timer_invalid_channel(self):
        self.run_timer("t5ai_core", fault=True)

    def test_aidk_ai_toy_timer(self):
        self.run_timer("aidk_ai_toy")

    def test_aidk_ai_toy_timer_missing_route(self):
        self.run_timer("aidk_ai_toy", missing_route=True)

    def test_aidk_ai_toy_timer_invalid_channel(self):
        self.run_timer("aidk_ai_toy", fault=True)

    def run_saradc(self, board, mode=0):
        elf = self.build_fixture(board, "saradc.c", TEST_MODE=mode)
        codes = (0, 4095, 291, 2048, 2730, 85)
        self.vm.add_args(
            "-global", "bk7258-soc.experimental-saradc=on",
            "-icount", "shift=5,align=off,sleep=off",
        )
        for channel, code in enumerate(codes, 1):
            if mode != 2 or channel != 6:
                self.vm.add_args(
                    "-global", f"bk7258-saradc.input{channel}={code}"
                )
        mmio = self.launch_fixture(elf, accelerator="tcg,thread=single")
        output = wait_for_console_pattern(self, " DONE")
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), mode, output.decode())
        samples = [tuple(int(value, 16) for value in row) for row in
                   re.findall(rb"BK7258 SARADC SAMPLE ([0-9a-f]{8}) "
                              rb"([0-9a-f]{8}) END", output)]
        expected = (list(enumerate(codes, 1)) * 2 + [(3, 291)] * 3 +
                    [(4, 2048)]) if mode == 0 else (
                        [(3, 291)] if mode == 1 else
                        [(1, 0)] if mode == 3 else [])
        self.assertEqual(samples, expected)
        result = f"BK7258 SARADC RESULT {mode:08x} {len(expected):08x} DONE"
        self.assertIn(result.encode(), output)
        self.assertIn(
            b"BK7258 SARADC PRECISE FAULT STATE OK" if mode else
            b"BK7258 SARADC INPUT REARM CLOCK CANCEL OK", output,
        )
        errors = {
            0: "",
            1: "bk7258-saradc: unsupported write 0x7ea5 at 0x10\n",
            2: "bk7258-saradc: unsupported write 0x7eb5 at 0x10\n",
            3: "bk7258-saradc: unsupported write 0x1 at 0x8\n",
        }
        self.assertEqual(mmio.read_text(), errors[mode])

    def test_t5_board_saradc(self):
        self.run_saradc("t5_board", mode=0)

    def test_t5_board_saradc_busy_write(self):
        self.run_saradc("t5_board", mode=1)

    def test_t5_board_saradc_missing_input(self):
        self.run_saradc("t5_board", mode=2)

    def test_t5_board_saradc_unsupported_reset(self):
        self.run_saradc("t5_board", mode=3)

    def test_t5ai_core_saradc(self):
        self.run_saradc("t5ai_core", mode=0)

    def test_t5ai_core_saradc_busy_write(self):
        self.run_saradc("t5ai_core", mode=1)

    def test_t5ai_core_saradc_missing_input(self):
        self.run_saradc("t5ai_core", mode=2)

    def test_t5ai_core_saradc_unsupported_reset(self):
        self.run_saradc("t5ai_core", mode=3)

    def test_aidk_ai_toy_saradc(self):
        self.run_saradc("aidk_ai_toy", mode=0)

    def test_aidk_ai_toy_saradc_busy_write(self):
        self.run_saradc("aidk_ai_toy", mode=1)

    def test_aidk_ai_toy_saradc_missing_input(self):
        self.run_saradc("aidk_ai_toy", mode=2)

    def test_aidk_ai_toy_saradc_unsupported_reset(self):
        self.run_saradc("aidk_ai_toy", mode=3)

    def run_ckmn(self, board, missing_route=False):
        elf = self.build_fixture(
            board, "ckmn.c", MISSING_ROUTE=int(missing_route)
        )
        # Genuine M33 execution: RTC and TIMG measure the 2 ms window while
        # the native CKMN handler checks IRQ21/IPSR37 and W1C deassertion.
        self.vm.add_args("-icount", "shift=0,align=off,sleep=off")
        mmio = self.launch_fixture(elf, accelerator="tcg,thread=single")
        output = wait_for_console_pattern(self, " DONE")
        self.vm.wait(timeout=5)
        self.assertEqual(
            self.vm.exitcode(), int(missing_route), output.decode()
        )
        sample = re.search(
            rb"BK7258 CKMN SAMPLE ([0-9a-f]{8}) ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) END", output
        )
        self.assertIsNotNone(sample, output.decode())
        rtc, timer, exception, count, result, status = (
            int(value, 16) for value in sample.groups()
        )
        self.assertEqual(result, 52000)
        if missing_route:
            self.assertIn(b"BK7258 CKMN EXPECTED SYS ROUTE FAILURE", output)
            self.assertEqual((exception, count, status), (0, 0, 1))
            self.assertGreaterEqual(rtc, 288)
            self.assertGreaterEqual(timer, 234000)
        else:
            self.assertIn(b"BK7258 CKMN 2MS IRQ21 W1C OK", output)
            self.assertEqual((exception, count, status), (37, 1, 0))
            self.assertGreaterEqual(rtc, 63)
            self.assertLessEqual(rtc, 65)
            self.assertGreaterEqual(timer, 51990)
            self.assertLessEqual(timer, 52260)
        self.assertEqual(mmio.read_bytes(), b"")

    def test_t5_board_ckmn(self):
        self.run_ckmn("t5_board")

    def test_t5_board_ckmn_missing_route(self):
        self.run_ckmn("t5_board", missing_route=True)

    def test_t5ai_core_ckmn(self):
        self.run_ckmn("t5ai_core")

    def test_t5ai_core_ckmn_missing_route(self):
        self.run_ckmn("t5ai_core", missing_route=True)

    def test_aidk_ai_toy_ckmn(self):
        self.run_ckmn("aidk_ai_toy")

    def test_aidk_ai_toy_ckmn_missing_route(self):
        self.run_ckmn("aidk_ai_toy", missing_route=True)

    def run_i2c_sdk(self, board, missing_route=False, bad_address=False):
        elf = self.build_fixture(
            board, "i2c_sdk.c", MISSING_ROUTE=int(missing_route),
            BAD_ADDRESS=int(bad_address)
        )
        for bus in range(2):
            for address, size in ((0x50, 256), (0x51, 512)):
                self.vm.add_args(
                    "-device", f"at24c-eeprom,bus=i2c{bus},"
                    f"address={address},rom-size={size}"
                )
        # Instruction-count scheduling keeps the SDK's START/threshold RMW
        # sequence independent of host preemption. This is not a CPU speed.
        self.vm.add_args("-icount", "shift=0,align=off,sleep=off")
        mmio = self.launch_fixture(elf, accelerator="tcg,thread=single")
        output = wait_for_console_pattern(self, " DONE")
        self.vm.wait(timeout=5)
        code = 1 if missing_route else 2 if bad_address else 0
        self.assertEqual(self.vm.exitcode(), code, output.decode())
        counts = re.search(
            rb"BK7258 I2C SDK COUNTS ([0-9a-f]{8}) ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) END", output
        )
        result = re.search(
            rb"BK7258 I2C SDK RESULT ([0-9a-f]{8}) ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) "
            rb"([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) DONE", output
        )
        self.assertIsNotNone(counts, output.decode())
        self.assertIsNotNone(result, output.decode())
        values = tuple(int(v, 16) for v in counts.groups())
        state = tuple(int(v, 16) for v in result.groups())
        self.assertEqual(state[0], code)
        self.assertEqual(state[-2:], (0, 0))  # No hidden BusFault.
        self.assertGreater(values[4], 0)
        if missing_route:
            self.assertEqual(values[:4], (55, 54, 52, 2))
            self.assertEqual(values[5], 0)
            self.assertEqual(state[1:6], (1, 0, 1, 0, 1))
            self.assertIn(b"BK7258 I2C SDK EXPECTED ROUTE FAILURE", output)
        elif bad_address:
            self.assertEqual(values[:4], (55, 55, 52, 3))
            self.assertEqual(values[5], 1)
            self.assertEqual(state[1:6], (1, 0, 1, 0, 1))
            self.assertIn(b"BK7258 I2C SDK EXPECTED ADDRESS FAILURE", output)
        else:
            self.assertEqual(values[:4], (108, 108, 104, 4))
            self.assertGreater(values[5], 0)
            self.assertIn(b"BK7258 I2C SDK 108 TRANSACTIONS OK", output)
        self.assertEqual(mmio.read_bytes(), b"")

    def test_t5_board_i2c_sdk_matrix(self):
        self.run_i2c_sdk("t5_board")

    def test_t5_board_i2c_sdk_missing_route(self):
        self.run_i2c_sdk("t5_board", missing_route=True)

    def test_t5_board_i2c_sdk_bad_address(self):
        self.run_i2c_sdk("t5_board", bad_address=True)

    def test_t5ai_core_i2c_sdk_matrix(self):
        self.run_i2c_sdk("t5ai_core")

    def test_t5ai_core_i2c_sdk_missing_route(self):
        self.run_i2c_sdk("t5ai_core", missing_route=True)

    def test_t5ai_core_i2c_sdk_bad_address(self):
        self.run_i2c_sdk("t5ai_core", bad_address=True)

    def test_aidk_ai_toy_i2c_sdk_matrix(self):
        self.run_i2c_sdk("aidk_ai_toy")

    def test_aidk_ai_toy_i2c_sdk_missing_route(self):
        self.run_i2c_sdk("aidk_ai_toy", missing_route=True)

    def test_aidk_ai_toy_i2c_sdk_bad_address(self):
        self.run_i2c_sdk("aidk_ai_toy", bad_address=True)

    def run_flash_sdk(self, board, bad_fifo=False):
        elf = self.build_fixture(board, "flash_sdk.c", BAD_FIFO=int(bad_fifo))
        raw = Path(self.scratch_file("flash_sdk.bin"))
        objcopy = Path(self.fixture_compiler).with_name("arm-none-eabi-objcopy")
        subprocess.run(
            [str(objcopy), "-O", "binary", str(elf), str(raw)],
            check=True, capture_output=True, timeout=10
        )
        payload = raw.read_bytes()
        payload += b"\xff" * (-len(payload) % 32)
        encoded = bytearray()
        for offset in range(0, len(payload), 32):
            frame = payload[offset:offset + 32]
            encoded.extend(frame)
            encoded.extend(self.crc16(frame).to_bytes(2, "big"))
        image = bytearray(b"\xff" * (8 * 1024 * 1024))
        self.assertLess(0x11000 + len(encoded), 0x200000)
        image[0x11000:0x11000 + len(encoded)] = encoded
        nor = Path(self.scratch_file("nor.bin"))
        nor.write_bytes(image)
        metadata_file = Path(self.log_file("fixture-inputs.json"))
        metadata = json.loads(metadata_file.read_text())
        metadata["expected_exit"] = int(bad_fifo)
        metadata["sha256"].update({
            "raw_payload": hashlib.sha256(raw.read_bytes()).hexdigest(),
            "initial_nor": hashlib.sha256(image).hexdigest(),
        })
        metadata_file.write_text(json.dumps(metadata, indent=2) + "\n")
        mmio = Path(self.log_file("mmio.log"))
        self.vm.set_qmp_monitor(False)
        self.vm.set_console()
        # Bound virtual progress by instruction count rather than host speed.
        # This is test scheduling, not a model of flash or CPU performance.
        self.vm.add_args(
            "-accel", "tcg,thread=single",
            "-icount", "shift=5,align=off,sleep=off",
            "-semihosting-config", "enable=on,target=native",
            "-drive", f"if=pflash,unit=0,format=raw,file={nor}",
            "-d", "guest_errors,unimp", "-D", str(mmio)
        )
        # No -kernel or host loader: vectors and instructions use CRC NOR XIP.
        self.vm.launch()
        self.vm.console_socket.settimeout(10)
        output = wait_for_console_pattern(self, " DONE")
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(bad_fifo), output.decode())
        self.assertIn(
            b"BK7258 FLASH SDK COUNTS 00000036 0000039e 00000143 "
            b"00000220 00000037 END", output
        )
        expected_log = "".join(
            f"bk7258-flash: operation {op} at 0x{addr:x} failed: "
            "Permission denied\n"
            for op, addr in ((12, 0x2001e0), (12, 0x200200),
                             (12, 0x200220), (13, 0x200000))
        )
        if bad_fifo:
            self.assertIn(b"BK7258 FLASH SDK EXPECTED FIFO FAULT", output)
            self.assertIn(
                b"BK7258 FLASH SDK RESULT 00000001 00008200 44030010 DONE",
                output
            )
            expected_log += (
                "bk7258-flash: page program requires eight TX words\n"
            )
        else:
            self.assertIn(b"BK7258 FLASH SDK 54 CASES OK", output)
            self.assertIn(
                b"BK7258 FLASH SDK RESULT 00000000 00000000 00000000 DONE",
                output
            )
        self.assertEqual(mmio.read_text(), expected_log)
        # Independent file-level oracle for the last matrix case. Everything
        # outside this one test sector, including executable XIP, is immutable.
        expected = bytearray(b"\xff" * 4096)
        seed = bytearray(((i * 29 + 53 * 7) ^ 0xd3) & 0xff for i in range(128))
        for i in range(65):
            seed[31 + i] &= ((i * 17 + 53 * 11) ^ 0x6b) & 0xff
        expected[0x1e0:0x260] = seed
        committed = nor.read_bytes()
        self.assertEqual(len(committed), len(image))
        for span in (slice(None, 0x200000), slice(0x201000, None)):
            self.assertEqual(
                hashlib.sha256(committed[span]).digest(),
                hashlib.sha256(image[span]).digest()
            )
        self.assertEqual(committed[0x200000:0x201000], expected)
        metadata["sha256"]["committed_nor"] = hashlib.sha256(
            committed
        ).hexdigest()
        metadata_file.write_text(json.dumps(metadata, indent=2) + "\n")

    def test_t5_board_flash_sdk_matrix(self):
        self.run_flash_sdk("t5_board")

    def test_t5_board_flash_sdk_bad_fifo(self):
        self.run_flash_sdk("t5_board", bad_fifo=True)

    def test_t5ai_core_flash_sdk_matrix(self):
        self.run_flash_sdk("t5ai_core")

    def test_t5ai_core_flash_sdk_bad_fifo(self):
        self.run_flash_sdk("t5ai_core", bad_fifo=True)

    def test_aidk_ai_toy_flash_sdk_matrix(self):
        self.run_flash_sdk("aidk_ai_toy")

    def test_aidk_ai_toy_flash_sdk_bad_fifo(self):
        self.run_flash_sdk("aidk_ai_toy", bad_fifo=True)

    def run_spi_sdk(self, board):
        elf = self.build_fixture(board, "spi_sdk.c")
        stores = []
        for bus in range(2):
            backing = Path(self.scratch_file(f"spi{bus}.bin"))
            backing.write_bytes(b"\xff" * (4 * 1024 * 1024))
            stores.append(backing)
            self.vm.add_args(
                "-drive", f"if=none,id=spi{bus}flash,format=raw,file={backing}",
                "-device", f"w25q32,bus=spi{bus},cs=0,drive=spi{bus}flash"
            )
        # Deterministic service scheduling, not physical instruction timing.
        self.vm.add_args(
            "-icount", "shift=0,align=off,sleep=off",
            "-trace", "enable=m25p80_select",
            "-trace", "enable=m25p80_transfer"
        )
        mmio = self.launch_fixture(elf, accelerator="tcg,thread=single")
        output = wait_for_console_pattern(self, " DONE")
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0, output.decode())
        self.assertIn(
            b"BK7258 SPI SDK RESULT 00000000 00000008 00000003 "
            b"00000003 00000002 00000000 00000000 DONE", output
        )
        # Native endpoint callbacks independently observe what left the FIFO.
        # A reset cannot undo a prefix already accepted by the NOR endpoint.
        active, frames = {}, {}
        for line in mmio.read_text().splitlines():
            event = re.search(
                r"m25p80_(select|transfer) \[(0x[0-9a-f]+)\] (.*)", line
            )
            self.assertIsNotNone(event, line)  # Reject other MMIO/errors.
            kind, device, detail = event.groups()
            if kind == "select":
                if detail == "select":
                    self.assertFalse(active.get(device), line)
                    active[device] = []
                else:
                    self.assertEqual(detail, "deselect", line)
                    if active.get(device):
                        frames.setdefault(device, []).append(active[device])
                    active[device] = None
            else:
                self.assertIsNotNone(active.get(device), line)
                tx = re.search(r" tx 0x([0-9a-f]+)$", detail)
                self.assertIsNotNone(tx, line)
                active[device].append(int(tx.group(1), 16))
        self.assertTrue(all(not pending for pending in active.values()))
        self.assertEqual(len(frames), 2)
        metadata_file = Path(self.log_file("fixture-inputs.json"))
        metadata = json.loads(metadata_file.read_text())
        metadata["ssi_frame_lengths"] = []
        for bus, transfers in enumerate(frames.values()):
            pattern = bytes(((i * 37 + bus * 13) ^ 0xa5) & 0xff
                            for i in range(4, 196))
            self.assertEqual(
                transfers,
                [[6], list(b"\x02\x01\x00\x20" + pattern[:128]),
                 [6], list(b"\x02\x02\x00\x20" + pattern[:64])]
            )
            expected = bytearray(b"\xff" * (4 * 1024 * 1024))
            expected[0x10020:0x100a0] = pattern[:128]
            expected[0x20020:0x20060] = pattern[:64]
            committed = stores[bus].read_bytes()
            self.assertEqual(len(committed), len(expected))
            self.assertEqual(hashlib.sha256(committed).digest(),
                             hashlib.sha256(expected).digest())
            metadata["sha256"][stores[bus].name] = hashlib.sha256(
                committed
            ).hexdigest()
            metadata["ssi_frame_lengths"].append([len(t) for t in transfers])
        metadata_file.write_text(json.dumps(metadata, indent=2) + "\n")

    def test_t5_board_spi_sdk_long_tx(self):
        self.run_spi_sdk("t5_board")

    def test_t5ai_core_spi_sdk_long_tx(self):
        self.run_spi_sdk("t5ai_core")

    def test_aidk_ai_toy_spi_sdk_long_tx(self):
        self.run_spi_sdk("aidk_ai_toy")

    def run_rtc(self, board, missing_route):
        elf = self.build_fixture(
            board, "rtc.c", MISSING_ROUTE=int(missing_route)
        )
        mmio = self.launch_fixture(elf)
        expected = (
            "BK7258 RTC PROBE FAILED"
            if missing_route
            else ("BK7258 RTC IRQ CLOCK STOP OK")
        )
        wait_for_console_pattern(self, expected)
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), int(missing_route))
        self.assertEqual(mmio.read_bytes(), b"")

    def test_t5_board_rtc(self):
        self.run_rtc("t5_board", False)

    def test_t5_board_rtc_missing_route(self):
        self.run_rtc("t5_board", True)

    def test_t5ai_core_rtc(self):
        self.run_rtc("t5ai_core", False)

    def test_t5ai_core_rtc_missing_route(self):
        self.run_rtc("t5ai_core", True)

    def test_aidk_ai_toy_rtc(self):
        self.run_rtc("aidk_ai_toy", False)

    def test_aidk_ai_toy_rtc_missing_route(self):
        self.run_rtc("aidk_ai_toy", True)

    @staticmethod
    def crc16(data):
        # Independent polynomial long division, rather than the C model loop.
        dividend = (int.from_bytes(data, "big") << 16) ^ (
            0xFFFF << (8 * len(data))
        )
        while dividend.bit_length() > 16:
            dividend ^= 0x18005 << (dividend.bit_length() - 17)
        return dividend

    def run_physical_xip(self, board, outcome):
        flags = {"BAD_RESULT": 1} if outcome == 1 else {}
        elf = self.build_fixture(
            board, "xip_cross_page.S", "xip_cross_page.ld", **flags
        )
        raw = Path(self.scratch_file("xip.bin"))
        objcopy = Path(self.fixture_compiler).with_name(
            "arm-none-eabi-objcopy"
        )
        subprocess.run(
            [str(objcopy), "-O", "binary", str(elf), str(raw)],
            check=True,
            capture_output=True,
            timeout=10,
        )
        self.assertEqual(self.crc16(bytes(32)), 0x8029)
        self.assertEqual(self.crc16(b"123456789"), 0xAEE7)
        payload = raw.read_bytes()
        payload += b"\xff" * (-len(payload) % 32)
        encoded = bytearray()
        for offset in range(0, len(payload), 32):
            frame = payload[offset : offset + 32]
            encoded.extend(frame)
            encoded.extend(self.crc16(frame).to_bytes(2, "big"))
        image = bytearray(b"\xff" * (8 * 1024 * 1024))
        # Logical 0x10000 maps to physical 0x11000 with 32+2 framing.
        image[0x11000 : 0x11000 + len(encoded)] = encoded
        if outcome == 2:
            # Corrupt the frame containing the instruction's second halfword.
            image[(0x11000 // 32) * 34] ^= 1
        nor = Path(self.scratch_file("nor.bin"))
        nor.write_bytes(image)
        inputs = Path(self.log_file("fixture-inputs.json"))
        metadata = json.loads(inputs.read_text())
        metadata["expected_exit"] = outcome
        metadata["sha256"].update(
            {
                path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in (raw, nor)
            }
        )
        inputs.write_text(json.dumps(metadata, indent=2) + "\n")
        trace = Path(self.log_file("instructions.log"))
        self.vm.set_qmp_monitor(False)
        self.vm.add_args(
            "-accel",
            "tcg",
            "-serial",
            "null",
            "-semihosting-config",
            "enable=on,target=native",
            "-drive",
            f"if=pflash,unit=0,format=raw,file={nor}",
            "-d",
            "in_asm,guest_errors,unimp",
            "-D",
            str(trace),
        )
        # No -kernel: all vectors and instruction bytes use physical NOR XIP.
        self.vm.launch()
        self.vm.wait(timeout=10)
        self.assertEqual(self.vm.exitcode(), outcome)
        observed = trace.read_text()
        self.assertIn("0x02010040" if outcome == 2 else "0x02010ffe", observed)
        self.assertNotIn("not implemented", observed)
        if outcome == 2:
            self.assertIn("XIP CRC mismatch", observed)
        else:
            self.assertNotIn("XIP CRC mismatch", observed)

    def test_t5_board_physical_xip(self):
        self.run_physical_xip("t5_board", 0)

    def test_t5_board_physical_xip_bad_result(self):
        self.run_physical_xip("t5_board", 1)

    def test_t5_board_physical_xip_bad_crc(self):
        self.run_physical_xip("t5_board", 2)

    def test_t5ai_core_physical_xip(self):
        self.run_physical_xip("t5ai_core", 0)

    def test_t5ai_core_physical_xip_bad_result(self):
        self.run_physical_xip("t5ai_core", 1)

    def test_t5ai_core_physical_xip_bad_crc(self):
        self.run_physical_xip("t5ai_core", 2)

    def test_aidk_ai_toy_physical_xip(self):
        self.run_physical_xip("aidk_ai_toy", 0)

    def test_aidk_ai_toy_physical_xip_bad_result(self):
        self.run_physical_xip("aidk_ai_toy", 1)

    def test_aidk_ai_toy_physical_xip_bad_crc(self):
        self.run_physical_xip("aidk_ai_toy", 2)

    def test_t5_board_sys_read_fault(self):
        self.run_sys_fault("t5_board", False)

    def test_t5_board_sys_write_fault(self):
        self.run_sys_fault("t5_board", True)

    def test_t5ai_core_sys_read_fault(self):
        self.run_sys_fault("t5ai_core", False)

    def test_t5ai_core_sys_write_fault(self):
        self.run_sys_fault("t5ai_core", True)

    def test_aidk_ai_toy_sys_read_fault(self):
        self.run_sys_fault("aidk_ai_toy", False)

    def test_aidk_ai_toy_sys_write_fault(self):
        self.run_sys_fault("aidk_ai_toy", True)

    def test_t5_board(self):
        self.run_fixture("t5_board", True)

    def test_t5_board_bad_input(self):
        self.run_fixture("t5_board", False)

    def test_t5ai_core(self):
        self.run_fixture("t5ai_core", True)

    def test_t5ai_core_bad_input(self):
        self.run_fixture("t5ai_core", False)

    def test_aidk_ai_toy(self):
        self.run_fixture("aidk_ai_toy", True)

    def test_aidk_ai_toy_bad_input(self):
        self.run_fixture("aidk_ai_toy", False)


if __name__ == "__main__":
    QemuSystemTest.main()
