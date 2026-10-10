#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""From repo root, with MESON_BUILD_ROOT set to the QEMU build directory:
reproduce.py BASELINE_QEMU NEW_QEMU OUTPUT_DIR.

Build only synthetic test guests. Reuse the V4 native-frame parser. Neither
SDK nor product inputs are modified/built. All NOR inputs start erased.
"""
import hashlib
import json
import subprocess
import sys
import unittest
from pathlib import Path

root = Path.cwd()
sys.path[:0] = [str(root / p) for p in
               ('python', 'tests/functional', 'tests/functional/arm')]
from test_bk7258 import BK7258Machine  # pylint: disable=wrong-import-position

old, new = map(lambda p: Path(p).resolve(), sys.argv[1:3])
out = Path(sys.argv[3]).resolve()
out.mkdir(parents=True, exist_ok=True)
record = {'baseline': '7f72f37ccb3d3ea119eed6b0967621a08913a760',
          'old_binary_sha256': hashlib.sha256(old.read_bytes()).hexdigest(),
          'new_binary_sha256': hashlib.sha256(new.read_bytes()).hexdigest(),
          'runs': []}
for name, n, fast in [('page24', 256, 0), ('length33-48', 33, 1),
                       ('short24', 32, 0)]:
    elf = out / (name + '.elf')
    build = ['arm-none-eabi-gcc', '-mcpu=cortex-m33', '-mthumb',
             '-ffreestanding', '-nostdlib', '-Os', '-Wall', '-Wextra', '-Werror',
             '-Wl,--fatal-warnings',
             '-Wl,-T,tests/functional/arm/guest-src/bk7258/diagnostic.ld',
             'tests/functional/arm/guest-src/bk7258/qspi_page.c',
             f'-DPAGE_LENGTH={n}', f'-DPROFILE_48={fast}', '-o', str(elf)]
    subprocess.run(build, check=True)
    for tag, binary in [('old', old), ('new', new)]:
        trace = out / f'{name}-{tag}-trace.log'
        args = [str(binary), '-M', 't5_board', '-display', 'none',
                '-serial', 'stdio', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-icount',
                'shift=0,align=off,sleep=off', '-kernel', str(elf),
                '-device', 'w25q32,bus=qspi0,cs=0',
                '-device', 'w25q32,bus=qspi1,cs=0',
                '-trace', 'enable=m25p80_select',
                '-trace', 'enable=m25p80_transfer',
                '-d', 'guest_errors,unimp', '-D', str(trace)]
        run = subprocess.run(args, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=15)
        (out / f'{name}-{tag}-uart.log').write_bytes(run.stdout)
        record['runs'].append({'name': name, 'model': tag, 'command': args,
                               'build': build, 'exit': run.returncode,
                               'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                               'uart': run.stdout.decode()})
        assert run.returncode == (1 if tag == 'old' else 0), run.stdout
        if tag == 'old':
            assert b'QSPI FAULT PC BFAR CFSR' in run.stdout
        else:
            assert b'QSPI PAGE OK' in run.stdout
            frames = BK7258Machine.qspi_frames(unittest.TestCase(), trace)
            for g, transfers in enumerate(frames.values()):
                expected = [[159, 255, 255, 255]]
                for page in range(2):
                    data = [(j * 37 + page * 17 + g * 83) % 256 for j in range(n)]
                    expected += [[6], [5, 255], [2, 0, 16 + page, 0] + data,
                                 [5, 255], [3, 0, 16 + page, 0] + [255] * n]
                expected += [[3, 0, 16, 0] + [255] * n]
                assert transfers == expected
(out / 'comparison.json').write_text(json.dumps(record, indent=2) + '\n')
print('Same-ELF comparisons and native frames: PASS')
