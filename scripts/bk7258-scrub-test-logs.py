#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drop unrelated inherited environment from Meson logs before publication.

Commands, stdout, stderr, failures, timings and results are retained.
Run only on the log directory selected for publication, after tests complete.
"""
import json
import sys
from pathlib import Path

for root in sys.argv[1:]:
    for path in Path(root).glob("*.txt"):
        text = path.read_text()
        path.write_text("\n".join(
            "Inherited environment: omitted (unrelated host environment)"
            if line.startswith("Inherited environment:") else line
            for line in text.splitlines()) + "\n")
    for path in Path(root).glob("*.json"):
        # Meson JSON logs are newline-delimited records. Unrelated JSON files
        # and unknown shapes are left intact, never guessed at or truncated.
        try:
            records = [json.loads(line) for line in path.read_text().splitlines()]
        except (ValueError, UnicodeError):
            continue
        if records and all(isinstance(r, dict) and "command" in r and
                           "result" in r for r in records):
            for record in records:
                record.pop("env", None)
            path.write_text("".join(json.dumps(r) + "\n" for r in records))
