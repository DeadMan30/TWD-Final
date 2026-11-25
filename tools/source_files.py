#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Attribute functions in symbols/ghidra_functions.csv to libraries and source files.

usage: source_files.py --exe EXE [--root DIR] [--out FILE] [--stats]
       source_files.py --check [--root DIR]

Attribution steps (no game data or machine paths are ever written):
1. Direct source-path evidence: scan RIP-relative `lea` instructions in each function for
   NUL-terminated `.c`/`.cc`/`.cpp`/`.cxx` paths matching `game/source_libraries.csv`.
2. Contiguity and home-cluster filtering: drop inlined outliers and keep each file's primary
   contiguous cluster (`HOME_GAP`).
3. Range propagation: functions sitting between two direct anchors of the same file (or library)
   receive `evidence=range`.
4. Library extents: remaining functions inside `game/library_extents.csv` spans receive
   `evidence=extent` for that library.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import io
import os
import posixpath
import re
import struct
import sys

DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUNCTIONS = os.path.join('symbols', 'ghidra_functions.csv')
RULES = os.path.join('game', 'source_libraries.csv')
EXTENTS = os.path.join('game', 'library_extents.csv')
TABLE = os.path.join('symbols', 'source_files.csv')
HEADER = ['address', 'last', 'functions', 'bytes', 'library', 'file', 'evidence', 'confidence']

MAX_PATH = 512
HOME_GAP = 0x20000
SOURCE_EXT = re.compile(r'\.(c|cc|cpp|cxx)$', re.I)
LEA_RIP = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')
PRINTABLE = re.compile(rb'[\x20-\x7e]+')


def read_functions(path: str) -> list[tuple[int, int]]:
    with open(path, newline='', encoding='utf-8') as f:
        return [(int(r['address'], 16), int(r['size'])) for r in csv.DictReader(f)]


def read_rules(path: str) -> list[tuple[re.Pattern, str, str]]:
    if not os.path.isfile(path):
        return []
    with open(path, newline='', encoding='utf-8') as f:
        return [(re.compile(r['pattern']), r['library'], r['prefix']) for r in csv.DictReader(f)]


def read_extents(path: str) -> list[tuple[int, int, str]]:
    if not os.path.isfile(path):
        return []
    with open(path, newline='', encoding='utf-8') as f:
        rows = [(int(r['first'], 16), int(r['last'], 16), r['library']) for r in csv.DictReader(f)]
    for first, last, library in rows:
        if last < first or not library:
            raise ValueError(f'{EXTENTS}: bad extent 0x{first:09x}..0x{last:09x} {library!r}')
    return rows


def classify(path: str, rules: list[tuple[re.Pattern, str, str]]) -> tuple[str, str] | None:
    norm = posixpath.normpath(path.replace('\\', '/'))
    if not SOURCE_EXT.search(norm):
        return None
    for pattern, library, prefix in rules:
        m = pattern.search(norm)
        if m:
            return library, prefix + m.group('file')
    return None


class PEImage:
    """Minimal read-only PE32+ view mapping virtual addresses to file offsets."""

    def __init__(self, path: str):
        with open(path, 'rb') as f:
            self.data = f.read()
        if self.data[:2] != b'MZ':
            raise SystemExit(f'{path}: not a PE executable')
        pe_off = struct.unpack_from('<I', self.data, 0x3c)[0]
        if self.data[pe_off:pe_off + 4] != b'PE\x00\x00':
            raise SystemExit(f'{path}: invalid PE header')
        opt_off = pe_off + 24
        if struct.unpack_from('<H', self.data, opt_off)[0] != 0x20b:
            raise SystemExit(f'{path}: expected PE32+ (x86-64)')
        num_sec = struct.unpack_from('<H', self.data, pe_off + 6)[0]
        opt_sz = struct.unpack_from('<H', self.data, pe_off + 20)[0]
        self.image_base = struct.unpack_from('<Q', self.data, opt_off + 24)[0]
        sec_off = opt_off + opt_sz
        self.sections: list[tuple[int, int, int]] = []  # (va_start, file_off, mapped_size)
        for i in range(num_sec):
            vsize, vaddr, rsize, roff = struct.unpack_from('<IIII', self.data, sec_off + i * 40 + 8)
            size = min(vsize, rsize)
            if size:
                self.sections.append((self.image_base + vaddr, roff, size))

    def offset(self, address: int) -> int | None:
        for start, off, size in self.sections:
            if start <= address < start + size:
                return off + (address - start)
        return None

    def c_string(self, address: int) -> str | None:
        off = self.offset(address)
        if off is None:
            return None
        end = self.data.find(b'\0', off, off + MAX_PATH)
        if end <= off:
            return None
        m = PRINTABLE.fullmatch(self.data, off, end)
        return m.group().decode('ascii') if m else None


def direct_candidates(image: PEImage, functions: list[tuple[int, int]],
                      rules: list[tuple[re.Pattern, str, str]]) -> tuple[dict[int, set], collections.Counter]:
    found: dict[int, set] = {}
    unknown: collections.Counter = collections.Counter()
    cache: dict[int, tuple | str | None] = {}
    for address, size in functions:
        off = image.offset(address)
        if off is None:
            continue
        body = image.data[off:off + size]
        names = set()
        for m in LEA_RIP.finditer(body):
            disp_at = m.start() + 3
            if disp_at + 4 > len(body):
                continue
            disp, = struct.unpack_from('<i', body, disp_at)
            target = address + disp_at + 4 + disp
            if target not in cache:
                text = image.c_string(target)
                tag = classify(text, rules) if text and ('/' in text or '\\' in text) else None
                cache[target] = tag if tag or not (text and SOURCE_EXT.search(text)) else text
            tag = cache[target]
            if isinstance(tag, tuple):
                names.add(tag)
            elif isinstance(tag, str):
                unknown[tag] += 1
        if names:
            found[address] = names
    return found, unknown


def choose_files(candidates: dict[int, set]) -> dict[int, tuple[str, str]]:
    uses = collections.Counter(tag for tags in candidates.values() for tag in tags)
    return {a: min(tags, key=lambda t: (uses[t], t)) for a, tags in candidates.items()}


def drop_inlined(tags: dict[int, tuple[str, str]]) -> tuple[dict[int, tuple[str, str]], list[int]]:
    kept = dict(tags)
    dropped: list[int] = []
    while True:
        order = sorted(kept)
        runs = []
        for i, a in enumerate(order):
            if runs and runs[-1][2] == kept[a]:
                runs[-1][1] = i
            else:
                runs.append([i, i, kept[a]])
        sandwiched = []
        for n in range(1, len(runs) - 1):
            left, mid, right = runs[n - 1], runs[n], runs[n + 1]
            length = mid[1] - mid[0] + 1
            if left[2] == right[2] and length <= max(left[1] - left[0] + 1, right[1] - right[0] + 1):
                sandwiched.append((length, n))
        taken: set[int] = set()
        remove = []
        for _, n in sorted(sandwiched):
            if n - 1 in taken or n + 1 in taken:
                continue
            taken.add(n)
            remove.extend(order[runs[n][0]:runs[n][1] + 1])
        if not remove:
            return kept, sorted(dropped)
        for a in set(remove):
            del kept[a]
            dropped.append(a)


def keep_home(tags: dict[int, tuple[str, str]]) -> tuple[dict[int, tuple[str, str]], list[int]]:
    clusters: dict[tuple[str, str], list[list[int]]] = collections.defaultdict(list)
    for a in sorted(tags):
        runs = clusters[tags[a]]
        if runs and a - runs[-1][-1] <= HOME_GAP:
            runs[-1].append(a)
        else:
            runs.append([a])
    kept, dropped = {}, []
    for _, runs in clusters.items():
        home = max(runs, key=len)
        for run in runs:
            for a in run:
                if run is home:
                    kept[a] = tags[a]
                else:
                    dropped.append(a)
    return kept, sorted(dropped)


def anchors(candidates: dict[int, set]) -> tuple[dict[int, tuple[str, str]], list[int]]:
    tags = choose_files(candidates)
    dropped: list[int] = []
    while True:
        tags, inlined = drop_inlined(tags)
        tags, far = keep_home(tags)
        if not inlined and not far:
            return tags, sorted(dropped)
        dropped += inlined + far


def propagate(functions: list[tuple[int, int]], direct: dict[int, tuple[str, str]]) -> list[tuple[str, str, str] | None]:
    out: list[tuple[str, str, str] | None] = [None] * len(functions)
    idx = [i for i, (a, _) in enumerate(functions) if a in direct]
    for i in idx:
        out[i] = (*direct[functions[i][0]], 'direct')
    for i, j in zip(idx, idx[1:]):
        if j == i + 1:
            continue
        lib_i, file_i = direct[functions[i][0]]
        lib_j, file_j = direct[functions[j][0]]
        if lib_i != lib_j:
            continue
        fill = (lib_i, file_i if file_i == file_j else '', 'range')
        for k in range(i + 1, j):
            out[k] = fill
    return out


def apply_extents(functions: list[tuple[int, int]], tags: list, extents: list[tuple[int, int, str]]) -> list:
    out = list(tags)
    for first, last, library in extents:
        for k, (address, _) in enumerate(functions):
            if first <= address <= last and (out[k] is None or out[k][2] != 'direct'):
                out[k] = (library, '', 'extent')
    return out


def confidence(evidence: str, file: str) -> str:
    if evidence == 'direct':
        return 'high'
    if evidence == 'extent':
        return 'medium'
    return 'medium' if file else 'low'


def to_rows(functions: list[tuple[int, int]], tags: list) -> list[dict]:
    rows: list[dict] = []
    previous = None
    for (address, size), tag in zip(functions, tags):
        if tag is None:
            previous = None
            continue
        lib, file, ev = tag
        if previous == tag:
            r = rows[-1]
            r['last'] = f'0x{address:09x}'
            r['functions'] += 1
            r['bytes'] += size
        else:
            rows.append({
                'address': f'0x{address:09x}',
                'last': f'0x{address:09x}',
                'functions': 1,
                'bytes': size,
                'library': lib,
                'file': file,
                'evidence': ev,
                'confidence': confidence(ev, file),
            })
            previous = tag
    return rows


def render_csv(rows: list[dict]) -> str:
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=HEADER, lineterminator='\n')
    w.writeheader()
    w.writerows(rows)
    return buf.getvalue()


def check_table(root: str) -> list[str]:
    path = os.path.join(root, TABLE)
    if not os.path.isfile(path):
        return [f'{TABLE} is missing']
    errs: list[str] = []
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.reader(f)
        rows = list(reader)
    if not rows or rows[0] != HEADER:
        return [f'{TABLE}: header must be {",".join(HEADER)}']
    prev_last = -1
    for n, r in enumerate(rows[1:], 2):
        if len(r) != len(HEADER):
            errs.append(f'{TABLE}:{n}: expected {len(HEADER)} columns, got {len(r)}')
            continue
        row = dict(zip(HEADER, r))
        try:
            a = int(row['address'], 16)
            last = int(row['last'], 16)
        except ValueError:
            errs.append(f'{TABLE}:{n}: invalid hex address')
            continue
        if last < a:
            errs.append(f'{TABLE}:{n}: last < address')
        if a <= prev_last:
            errs.append(f'{TABLE}:{n}: addresses must be strictly ascending and non-overlapping')
        prev_last = last
        if not row['library']:
            errs.append(f'{TABLE}:{n}: empty library')
    return errs


def print_stats(functions: list[tuple[int, int]], rows: list[dict]) -> None:
    tot_f = len(functions)
    tot_b = sum(s for _, s in functions)
    by_lib: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0])
    for r in rows:
        by_lib[r['library']][0] += int(r['functions'])
        by_lib[r['library']][1] += int(r['bytes'])
    cov_f = sum(v[0] for v in by_lib.values())
    cov_b = sum(v[1] for v in by_lib.values())
    print(f'source_files: {len(rows)} runs, {cov_f}/{tot_f} functions ({100*cov_f/tot_f:.2f}%), '
          f'{cov_b}/{tot_b} bytes ({100*cov_b/tot_b:.2f}%)')
    for lib, (fn, b) in sorted(by_lib.items(), key=lambda kv: -kv[1][1]):
        print(f'  {lib:12s} {fn:6d} fns ({100*fn/tot_f:5.2f}%)  {b:9d} B ({100*b/tot_b:5.2f}%)')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--exe', help='Path to WalkingDead4.exe')
    ap.add_argument('--out', help='Output CSV path (default: symbols/source_files.csv)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--stats', action='store_true')
    a = ap.parse_args()

    if a.check:
        errs = check_table(a.root)
        for e in errs:
            print(f'source_files: {e}', file=sys.stderr)
        return 1 if errs else 0

    exe = a.exe or os.path.join(a.root, 'game', 'WalkingDead4.exe')
    if not os.path.isfile(exe):
        print(f'source_files: {exe} not found', file=sys.stderr)
        return 1

    functions = read_functions(os.path.join(a.root, FUNCTIONS))
    rules = read_rules(os.path.join(a.root, RULES))
    extents = read_extents(os.path.join(a.root, EXTENTS))
    image = PEImage(exe)
    candidates, unknown = direct_candidates(image, functions, rules)
    direct, dropped = anchors(candidates)
    tags = apply_extents(functions, propagate(functions, direct), extents)
    rows = to_rows(functions, tags)

    out_path = a.out or os.path.join(a.root, TABLE)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(render_csv(rows))

    if a.stats:
        print_stats(functions, rows)
        if dropped:
            print(f'  dropped {len(dropped)} cross-file inlined/outlier direct tags')
        if unknown:
            print(f'  unclassified source paths ({len(unknown)}): {list(unknown.most_common(5))}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
