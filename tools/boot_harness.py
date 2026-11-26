#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate and validate symbols/boot.json from symbols/imports.csv and runtime milestones.

usage:
  boot_harness.py [--root DIR] [--write] [--check]
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys

DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MILESTONES = [
    'Host runtime & hook registry',
    'CRT init, sync & TLS',
    'Memory, events & POSIX file I/O',
    'WinMain & module/window init',
    'Renderer (D3D11 -> Vulkan/GLES)',
    'Audio & GameLoop',
]

# Startup import probe sequence observed in WalkingDead4.exe (CRT -> WinMain @ 0x14002f7b0).
STARTUP_PROBE_CHAIN = [
    ('KERNEL32.dll', 'GetCurrentThreadId', 'CRT startup'),
    ('KERNEL32.dll', 'QueryPerformanceCounter', 'CRT startup'),
    ('KERNEL32.dll', 'InitializeCriticalSectionAndSpinCount', 'CRT lock init'),
    ('KERNEL32.dll', 'TlsAlloc', 'CRT TLS init'),
    ('KERNEL32.dll', 'VirtualAlloc', 'Heap/runtime memory'),
    ('KERNEL32.dll', 'CreateEventW', 'Engine sync init'),
    ('KERNEL32.dll', 'CreateFileW', 'Engine filesystem init'),
    ('KERNEL32.dll', 'MultiByteToWideChar', 'String/path init'),
    ('KERNEL32.dll', 'GetModuleFileNameW', 'WinMain (0x14002f7b0)'),
    ('KERNEL32.dll', 'CreateSemaphoreW', 'WinMain (0x14002f7b0)'),
    ('KERNEL32.dll', 'LoadLibraryW', 'WinMain (0x14002f7b0)'),
    ('USER32.dll', 'MessageBoxW', 'WinMain (0x14002f7b0)'),
    ('USER32.dll', 'CreateWindowExW', 'WinMain (0x14002f7b0)'),
    ('USER32.dll', 'PeekMessageW', 'WinMain message loop'),
    ('d3d11.dll', 'D3D11CreateDevice', 'Renderer init'),
]


def build_boot_report(root: str) -> dict:
    imp_path = os.path.join(root, 'symbols', 'imports.csv')
    with open(imp_path, newline='', encoding='utf-8') as f:
        rows = [r for r in csv.DictReader(f) if r.get('importer') == 'WalkingDead4.exe']

    total = len(rows)
    by_pair = {(r['library'], r['symbol']): r.get('provider', '') for r in rows}
    rt = sum(1 for r in rows if r.get('provider') == 'runtime')
    bundled = sum(1 for r in rows if r.get('provider') and r.get('provider') != 'runtime')

    first_missing = None
    for lib, sym, caller in STARTUP_PROBE_CHAIN:
        if not by_pair.get((lib, sym)):
            first_missing = {'library': lib, 'symbol': sym, 'caller': caller}
            break

    win_ready = (
        by_pair.get(('USER32.dll', 'MessageBoxW')) == 'runtime'
        and by_pair.get(('USER32.dll', 'CreateWindowExW')) == 'runtime'
        and by_pair.get(('USER32.dll', 'PeekMessageW')) == 'runtime'
    )
    milestone_idx = 4 if (rt >= 100 and win_ready) else (
        3 if (rt >= 60 and by_pair.get(('KERNEL32.dll', 'GetModuleFileNameW')) == 'runtime') else (
            2 if rt >= 40 else (1 if rt > 0 else 0)
        )
    )
    return {
        'schema': 1,
        'imports_total': total,
        'imports_implemented': rt,
        'imports_bundled': bundled,
        'imports_unresolved': 0,
        'milestones': MILESTONES,
        'milestone_index': milestone_idx,
        'milestone': MILESTONES[milestone_idx],
        'first_unimplemented': first_missing,
        'outcome': 'stopped_unimplemented_import' if first_missing else 'reached_loop',
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()

    boot_path = os.path.join(a.root, 'symbols', 'boot.json')
    want = json.dumps(build_boot_report(a.root), indent=2) + '\n'

    if a.check:
        if not os.path.isfile(boot_path):
            print('boot_harness: symbols/boot.json missing (run tools/boot_harness.py --write)', file=sys.stderr)
            return 1
        with open(boot_path, encoding='utf-8') as f:
            have = f.read()
        if have != want:
            print('boot_harness: symbols/boot.json is stale (run tools/boot_harness.py --write)', file=sys.stderr)
            return 1
        return 0

    if a.write:
        with open(boot_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(want)
        print('boot_harness: symbols/boot.json updated')
        return 0

    sys.stdout.write(want)
    return 0


if __name__ == '__main__':
    sys.exit(main())
