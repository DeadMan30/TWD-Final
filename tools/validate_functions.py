#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate symbols/functions.csv, symbols/ghidra_functions.csv, game/hooks.csv, and evidence tables.

usage: validate_functions.py [--root DIR]
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import families as fam  # noqa: E402

DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDR = re.compile(r'^0x[0-9a-f]{9,}$')
IMAGE_BASE = 0x140000000
NAME = re.compile(r'^[a-z][a-z0-9]*(_[a-z0-9]+)+$')
STATUSES = ('original', 'replaced', 'edge-verified', 'verified')
HOOKED = ('replaced', 'edge-verified', 'verified')
FUNC_HEADER = ['address', 'size', 'name', 'system', 'status', 'notes']
EXPORT_HEADER = ['address', 'size']
HOOK_HEADER = ['address', 'replacement', 'system', 'returns']
REVIEW_HEADER = ['address', 'name', 'date', 'reviewer', 'verdict', 'notes']
VERIF_HEADER = ['function', 'cases', 'option', 'total', 'passed', 'failed', 'quarantined', 'source', 'date']
MATCH_HEADER = ['address', 'name', 'compiler', 'flags', 'match_percent', 'objdiff_percent', 'byte_exact',
                'targets_checked', 'source', 'source_sha256', 'date']
RETURN_CLASSES = {
    'void': {'void'},
    'i8': {'bool', 'int8_t', 'uint8_t', 'char'},
    'i32': {'int', 'unsigned', 'int32_t', 'uint32_t'},
    'i64': {'int64_t', 'uint64_t', 'size_t', 'intptr_t', 'uintptr_t', 'pointer'},
    'f32': {'float'},
    'f64': {'double'},
}
MAX_NOTE = 200
DECL_RE = re.compile(r'(?:extern\s+"C"\s+)?([\w ]+?)\s*(\*?)\s*\b(twd_\w+)\s*\(')


def load(path: str, header: list[str], errs: list[str]) -> list[dict]:
    rel = os.path.basename(os.path.dirname(path)) + '/' + os.path.basename(path)
    if not os.path.isfile(path):
        errs.append(f'{rel}: missing')
        return []
    with open(path, newline='', encoding='utf-8') as f:
        rd = csv.reader(f)
        head = next(rd, None)
        if head != header:
            errs.append(f'{rel}: header must be {",".join(header)}')
            return []
        rows = []
        for n, r in enumerate(rd, 2):
            if len(r) != len(header):
                errs.append(f'{rel}:{n}: expected {len(header)} columns, got {len(r)}')
                continue
            d = dict(zip(header, r))
            d['_line'] = n
            rows.append(d)
    return rows


def check_sorted_unique(rows: list[dict], rel: str, errs: list[str]) -> None:
    prev = None
    for r in rows:
        if not ADDR.match(r['address']):
            errs.append(f"{rel}:{r['_line']}: address '{r['address']}' must be 0x + 9+ lowercase hex digits")
            continue
        val = int(r['address'], 16)
        if val < IMAGE_BASE:
            errs.append(f"{rel}:{r['_line']}: address {r['address']} is below image base 0x{IMAGE_BASE:09x}")
            continue
        if prev is not None and val <= prev:
            errs.append(f"{rel}:{r['_line']}: addresses must be unique and ascending")
        prev = val


def replacement_definitions(root: str, system: str) -> dict[str, str]:
    out: dict[str, str] = {}
    sys_dir = os.path.join(root, 'game', system)
    if not os.path.isdir(sys_dir):
        return out
    for dp, _, fns in os.walk(sys_dir):
        if os.path.basename(dp) == 'tests':
            continue
        for fn in fns:
            if fn.endswith(('.c', '.cc', '.cpp')):
                with open(os.path.join(dp, fn), encoding='utf-8', errors='replace') as f:
                    for m in DECL_RE.finditer(f.read()):
                        ret = 'pointer' if m.group(2) else ' '.join(m.group(1).split()).replace('const ', '')
                        out[m.group(3)] = ret
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    root = ap.parse_args().root
    errs: list[str] = []

    export = load(os.path.join(root, 'symbols', 'ghidra_functions.csv'), EXPORT_HEADER, errs)
    check_sorted_unique(export, 'symbols/ghidra_functions.csv', errs)
    exp_size: dict[str, int] = {}
    for r in export:
        if not r['size'].isdigit() or int(r['size']) < 1:
            errs.append(f"symbols/ghidra_functions.csv:{r['_line']}: size must be a positive integer")
            continue
        if not ADDR.match(r['address']):
            continue
        exp_size[r['address']] = int(r['size'])

    game_dir = os.path.join(root, 'game')
    systems = sorted(
        d for d in os.listdir(game_dir)
        if os.path.isdir(os.path.join(game_dir, d)) and d not in ('Archives', 'include')
    ) if os.path.isdir(game_dir) else []

    funcs = load(os.path.join(root, 'symbols', 'functions.csv'), FUNC_HEADER, errs)
    check_sorted_unique(funcs, 'symbols/functions.csv', errs)
    names: set[str] = set()
    by_addr: dict[str, dict] = {}
    defs_cache: dict[str, dict[str, str]] = {}

    for r in funcs:
        where = f"symbols/functions.csv:{r['_line']}"
        if not r['size'].isdigit() or int(r['size']) < 1:
            errs.append(f'{where}: size must be a positive integer')
        if r['system'] not in systems:
            errs.append(f"{where}: system '{r['system']}' has no game/{r['system']}/ directory")
        if not NAME.match(r['name']):
            errs.append(f"{where}: name '{r['name']}' must be snake_case with a system prefix")
        elif not r['name'].startswith(r['system'] + '_'):
            errs.append(f"{where}: name '{r['name']}' must start with '{r['system']}_'")
        if r['name'] in names:
            errs.append(f"{where}: duplicate name '{r['name']}'")
        names.add(r['name'])
        if r['status'] not in STATUSES:
            errs.append(f"{where}: status '{r['status']}' must be one of {'|'.join(STATUSES)}")
        if len(r['notes']) > MAX_NOTE:
            errs.append(f'{where}: notes longer than {MAX_NOTE} characters')
        if export:
            if r['address'] not in exp_size:
                errs.append(f"{where}: {r['address']} is not in symbols/ghidra_functions.csv")
            elif r['size'].isdigit() and int(r['size']) != exp_size[r['address']]:
                errs.append(f"{where}: size {r['size']} differs from export ({exp_size[r['address']]})")
        by_addr[r['address']] = r

    hooks = load(os.path.join(root, 'game', 'hooks.csv'), HOOK_HEADER, errs)
    check_sorted_unique(hooks, 'game/hooks.csv', errs)
    hooked: set[str] = set()
    for h in hooks:
        where = f"game/hooks.csv:{h['_line']}"
        f = by_addr.get(h['address'])
        if f is None:
            errs.append(f'{where}: no row in symbols/functions.csv for {h["address"]}')
            continue
        hooked.add(h['address'])
        if f['status'] not in HOOKED:
            errs.append(f"{where}: {f['name']} is '{f['status']}', only replaced/verified functions are hooked")
        if h['system'] != f['system']:
            errs.append(f"{where}: system '{h['system']}' differs from functions.csv ('{f['system']}')")
        if h['replacement'] != 'twd_' + f['name']:
            errs.append(f"{where}: replacement must be 'twd_{f['name']}'")
        elif h['returns'] not in RETURN_CLASSES:
            errs.append(f"{where}: returns '{h['returns']}' must be one of {', '.join(sorted(RETURN_CLASSES))}")
        else:
            sys_defs = defs_cache.setdefault(h['system'], replacement_definitions(root, h['system']))
            declared = sys_defs.get(h['replacement'])
            if declared is not None and declared not in RETURN_CLASSES[h['returns']]:
                errs.append(f"{where}: returns '{h['returns']}' but {h['replacement']} returns '{declared}'")

    for r in funcs:
        if r['status'] in HOOKED and r['address'] not in hooked:
            errs.append(f"symbols/functions.csv:{r['_line']}: {r['name']} is {r['status']} without game/hooks.csv entry")

    reviews = load(os.path.join(root, 'symbols', 'reviews.csv'), REVIEW_HEADER, errs)
    verifs = load(os.path.join(root, 'symbols', 'verification.csv'), VERIF_HEADER, errs)
    load(os.path.join(root, 'symbols', 'matching.csv'), MATCH_HEADER, errs)

    approved = {r['address'] for r in reviews if r['verdict'] == 'approved'}
    passing_edge = {
        r['function'] for r in verifs
        if r['cases'] == 'edge' and r['option'] == '' and r['total'].isdigit()
        and int(r['total']) > 0 and r['failed'] == '0' and r['passed'] == r['total']
    }
    for r in funcs:
        if r['status'] in ('edge-verified', 'verified') and r['name'] not in passing_edge:
            errs.append(f"symbols/functions.csv:{r['_line']}: {r['name']} is {r['status']} without passing edge verification")
        if r['status'] == 'verified' and r['address'] not in approved:
            errs.append(f"symbols/functions.csv:{r['_line']}: {r['name']} is verified without an approved review")

    fam.families(root)

    for e in errs:
        print(f'validate: {e}', file=sys.stderr)
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
