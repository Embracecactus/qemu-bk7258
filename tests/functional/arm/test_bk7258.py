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
        hashes = {
            str(path.name): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (
                source / filename,
                source / linker,
                elf,
            )
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

    def launch_fixture(self, elf):
        mmio = Path(self.log_file("mmio.log"))
        self.vm.set_console()
        self.vm.add_args(
            "-S",
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
        self.assertFalse(self.vm.cmd("query-status")["running"])
        self.vm.cmd("cont")
        return mmio

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

    def run_sys_fault(self, board, write):
        elf = self.build_fixture(board, "sys_fault.c", WRITE_PROBE=int(write))
        mmio = self.launch_fixture(elf)
        wait_for_console_pattern(
            self,
            "BK7258 SYS ACCESS FAULT OK",
            "BK7258 SYS ACCESS FAULT FAILED",
        )
        self.vm.wait(timeout=5)
        self.assertEqual(self.vm.exitcode(), 0)
        operation = "write offset 0xa0" if write else "read offset 0x0"
        self.assertEqual(
            mmio.read_text(),
            f"bk7258-sys: {operation} is not implemented\n",
        )

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
        self.vm.add_args(
            "-S",
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
        # Very short guests can exit before QMP's initial negotiation finishes.
        self.assertFalse(self.vm.cmd("query-status")["running"])
        self.vm.cmd("cont")
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
