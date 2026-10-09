#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture unchanged BK7258 NOR inputs; never asserts boot success."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_input(path, expected):
    data = path.read_bytes()
    if digest(data) != expected:
        raise ValueError(f"SHA256 mismatch: {path}")
    return data


def connect(path, process):
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(5)
    deadline = time.monotonic() + 5
    while True:
        try:
            sock.connect(str(path))
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            if process.poll() is not None or time.monotonic() >= deadline:
                sock.close()
                raise
            time.sleep(0.01)


def qmp(stream, command):
    stream.write(json.dumps({'execute': command}).encode() + b'\n')
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError('QMP closed before reply')
        reply = json.loads(line)
        if 'error' in reply:
            raise RuntimeError(reply['error'])
        if 'return' in reply:
            return reply['return']


def packet(sock, command):
    data = command.encode()
    sock.sendall(b'$' + data + b'#' + f'{sum(data) % 256:02x}'.encode())
    while True:
        byte = sock.recv(1)
        if not byte:
            raise RuntimeError('GDB closed before reply')
        if byte == b'$':
            break
    data = b''
    while True:
        byte = sock.recv(1)
        if not byte:
            raise RuntimeError('GDB closed inside reply')
        if byte == b'#':
            break
        data += byte
    checksum = b''
    while len(checksum) < 2:
        byte = sock.recv(2 - len(checksum))
        if not byte:
            raise RuntimeError('GDB closed inside checksum')
        checksum += byte
    if int(checksum, 16) != sum(data) % 256:
        raise RuntimeError('GDB checksum mismatch')
    sock.sendall(b'+')
    return data.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--nor', type=Path, required=True)
    parser.add_argument('--nor-sha256', required=True)
    parser.add_argument('--status', type=Path, required=True)
    parser.add_argument('--status-sha256', required=True)
    parser.add_argument('--board', required=True,
                        choices=('t5_board', 't5ai_core', 'aidk_ai_toy'))
    parser.add_argument('--entry', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--instruction-trace', action='store_true')
    parser.add_argument('--r7a', type=lambda x: int(x, 0))
    for name in ('control', 'status', 'word242'):
        parser.add_argument('--otp-' + name, type=lambda x: int(x, 0))
    args = parser.parse_args()
    otp = (args.otp_control, args.otp_status, args.otp_word242)
    has_otp = any(value is not None for value in otp)
    if has_otp and (args.r7a is not None or None in otp):
        parser.error('OTP needs all three inputs and cannot combine with R7A')
    if has_otp and (otp[0] != 3 or otp[1] not in (0, 1)):
        parser.error('OTP probe requires control=3 and status=0 or 1')
    if any(v is not None and not 0 <= v <= 0xffffffff
           for v in (*otp, args.r7a)):
        parser.error('probe inputs must be unsigned 32-bit words')
    if not 0 < args.seconds <= 30:
        parser.error('capture window must be in (0, 30] seconds')
    nor = checked_input(args.nor, args.nor_sha256)
    status = checked_input(args.status, args.status_sha256)
    if len(nor) != 8 * 1024 * 1024 or len(status) != 512:
        parser.error('this probe requires 8-MiB NOR and 512-byte status inputs')
    if not 0x02000000 <= args.entry < 0x02000000 + len(nor) // 34 * 32 - 8:
        parser.error('entry must be in the CRC-framed XIP window')
    if args.entry % 128:
        parser.error('entry must be a 128-byte aligned vector table')
    offset = (args.entry - 0x02000000) // 32 * 34
    vectors = struct.unpack_from('<2I', nor, offset)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'nor.bin').write_bytes(nor)
    (output / 'status.bin').write_bytes(status)
    executable = args.qemu.resolve()
    record = {'qemu': str(executable),
              'qemu_sha256': digest(executable.read_bytes()),
              'board': args.board, 'entry': hex(args.entry),
              'initial_sp': hex(vectors[0]), 'reset_pc': hex(vectors[1]),
              'nor_sha256_before': digest(nor),
              'status_sha256_before': digest(status),
              'capture_seconds': args.seconds,
              'injection': {}, 'boot_success': 'not asserted'}
    properties = []
    if args.r7a is not None:
        record['injection']['r7a'] = args.r7a
    if has_otp:
        record['injection'].update(zip(('otp-control', 'otp-status',
                                       'otp-word242'), otp))
    for key, value in record['injection'].items():
        properties += ['-global', f'bk7258-entry-probe.{key}={value:#x}']
    with tempfile.TemporaryDirectory(prefix='bk7258-probe-') as sockets:
        qmp_path, gdb_path = Path(sockets) / 'qmp', Path(sockets) / 'gdb'
        command = [str(executable), '-M', args.board, '-S',
                   '-qmp', f'unix:{qmp_path},server=on,wait=off',
                   '-gdb', f'unix:{gdb_path},server=on,wait=off',
                   '-global', f'bk7258-soc.boot-vector={args.entry:#x}',
                   '-display', 'none', '-monitor', 'none',
                   '-serial', f'file:{output}/uart.log',
                   '-drive',
                   f'if=pflash,unit=0,format=raw,file={output}/nor.bin',
                   '-drive',
                   f'if=pflash,unit=1,format=raw,file={output}/status.bin',
                   '-d', ('in_asm,' if args.instruction_trace else '') +
                   'int,guest_errors,unimp', '-D', str(output / 'trace.log'),
                   '-trace', 'enable=clock_set'] + properties
        record['argv'] = command
        (output / 'invocation.json').write_text(
            json.dumps(record, indent=2) + '\n')
        with (output / 'stderr.log').open('wb') as stderr:
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                       stderr=stderr)
            try:
                with connect(qmp_path, process) as control:
                    with control.makefile('rwb', buffering=0) as stream:
                        json.loads(stream.readline())
                        qmp(stream, 'qmp_capabilities')
                        qmp(stream, 'cont')
                        time.sleep(args.seconds)
                        qmp(stream, 'stop')
                        with connect(gdb_path, process) as debug:
                            packet(debug, '?')
                            if packet(debug, 'Hg1') != 'OK':
                                raise RuntimeError('cannot select CPU0')
                            registers = packet(debug, 'g')
                            regs = struct.unpack('<16I', bytes.fromhex(
                                registers[:128]))
                            record['stopped_cpu0_registers'] = [
                                hex(v) for v in regs]
                            record['stopped_stack_raw'] = packet(
                                debug, f'm{regs[13]:x},20')
                            record['stopped_cfsr_raw'] = packet(
                                debug, 'me000ed28,4')
                            record['stopped_bfar_raw'] = packet(
                                debug, 'me000ed38,4')
                        qmp(stream, 'quit')
                        process.wait(timeout=5)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    trace = (output / 'trace.log').read_text(errors='replace')
    uart = (output / 'uart.log').read_text(errors='replace')
    faults = re.findall(r'fault address (0x[0-9a-f]+)', trace)
    pcs = re.findall(r'HF .*? P=([0-9a-fA-F]{8})', uart)
    record['fault_addresses_in_order'] = faults
    record['first_fault_address'] = faults[0] if faults else None
    record['uart_fault_pcs_in_order'] = ['0x' + pc for pc in pcs]
    record['first_uart_fault_pc'] = '0x' + pcs[0] if pcs else None
    record['capture_note'] = ('Stopped registers/stack may follow a watchdog '
                              'reset; they are not the first exception frame. '
                              'No fault within the window is not boot success.')
    record['nor_sha256_after'] = digest((output / 'nor.bin').read_bytes())
    record['status_sha256_after'] = digest((output / 'status.bin').read_bytes())
    checked_input(args.nor, args.nor_sha256)
    checked_input(args.status, args.status_sha256)
    (output / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({key: record[key] for key in
                      ('board', 'entry', 'injection', 'first_fault_address',
                       'first_uart_fault_pc', 'boot_success')}))


if __name__ == '__main__':
    main()
