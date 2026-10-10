#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""From repo root with MESON_BUILD_ROOT: verify-cancel.py TRACE RESULT_JSON."""
import hashlib
import json
import sys
import unittest
from pathlib import Path

root = Path.cwd()
sys.path[:0] = [str(root / p) for p in
               ('python', 'tests/functional', 'tests/functional/arm')]
from test_bk7258 import BK7258Machine  # pylint: disable=wrong-import-position

trace = Path(sys.argv[1])
frames = BK7258Machine.qspi_frames(unittest.TestCase(), trace)
result = {'trace_sha256': hashlib.sha256(trace.read_bytes()).hexdigest(),
          'controllers': []}
for g, transfers in enumerate(frames.values()):
    expected = []
    for mode in range(3):
        n = 256 if g or mode == 0 else 160
        data = [(j * 37 + g * 83 + mode * 17) % 256 for j in range(n)]
        expected += [[6], [2, 0, 16 + mode, 0] + data,
                     [3, 0, 16 + mode, 0] + [255] * 256]
    assert transfers == expected, (g, [len(f) for f in transfers])
    result['controllers'].append({'unit': g,
                                  'frame_lengths': [len(f) for f in transfers],
                                  'byte_streams_and_CS': 'PASS'})
Path(sys.argv[2]).write_text(json.dumps(result, indent=2) + '\n')
print('Native long-frame pause/disable/reset: PASS')
