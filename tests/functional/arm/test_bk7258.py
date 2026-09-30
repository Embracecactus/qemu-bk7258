#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# AI-assisted downstream experiment, not an upstream contribution.

"""Execute trusted, source-built three-core fixtures without an OS checkout."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from qemu_test import QemuSystemTest, wait_for_console_pattern


class BK7258Machine(QemuSystemTest):
    HARDWARE_MARKERS = (
        "BK7258 SRAM ALIASES OK",
        "BK7258 WATCHDOG NMI OK",
        "BK7258 WATCHDOG CLOCK LOSS OK",
        "BK7258 SYSTICK IRQ OK",
        "BK7258 EXTERNAL 32K SYSTICK OK",
        "BK7258 CPU1 CPU2 RELEASE AND PRIVATE TCM OK",
        "BK7258 HALT RESUME AND RESET OK",
        "BK7258 MAILBOX THREE CORE IRQ AND PROTECTION OK",
    )

    def run_fixture(self, board, positive):
        self.require_accelerator("tcg")
        self.set_machine(board)
        compiler = os.environ.get("BK7258_TEST_CC") or shutil.which(
            "arm-none-eabi-gcc"
        )
        if compiler is None:
            self.skipTest("arm-none-eabi-gcc or BK7258_TEST_CC is required")
        source = Path(__file__).parent / "guest-src" / "bk7258"
        elf = Path(self.scratch_file("diagnostic.elf"))
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
                f"-Wl,-T,{source / 'diagnostic.ld'}",
                str(source / "diagnostic.c"),
                "-o",
                str(elf),
            ],
            check=True,
            capture_output=True,
            timeout=30,
        )
        hashes = {
            str(path.name): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (
                source / "diagnostic.c",
                source / "diagnostic.ld",
                elf,
            )
        }
        Path(self.log_file("fixture-inputs.json")).write_text(
            json.dumps(
                {
                    "board": board,
                    "positive": positive,
                    "compiler_version": version.stdout.splitlines()[0],
                    "sha256": hashes,
                    "qemu_sha256": hashlib.sha256(
                        Path(self.qemu_bin).read_bytes()
                    ).hexdigest(),
                    "scope": "bare-metal CPU/IRQ only; no OS boot",
                },
                indent=2,
            )
            + "\n"
        )

        mmio = Path(self.log_file("mmio.log"))
        self.vm.set_console()
        self.vm.add_args(
            "-accel",
            "tcg",
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
