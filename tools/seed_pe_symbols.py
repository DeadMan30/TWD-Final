#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Seed symbols/ghidra_functions.csv and symbols/imports.csv from a local PC PE binary.

Reads only PE headers (.pdata RUNTIME_FUNCTION unwind ranges and Import Directory DLL/function
names) from game/WalkingDead4.exe. Writes only addresses, byte sizes, and public API import names;
no game code, strings, or assets are copied.

Once full Ghidra analysis is run (tools/ghidra/ExportFunctions.java), replace
symbols/ghidra_functions.csv with the Ghidra export so leaf functions without unwind info are
also counted.
"""
from __future__ import annotations

import argparse
import csv
import io
import os
import struct
import sys

DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_pe(path: str) -> tuple[list[tuple[int, int]], list[tuple[str, str]]]:
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'MZ':
        raise ValueError(f'{path}: not a PE file')
    pe_off = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe_off:pe_off + 4] != b'PE\x00\x00':
        raise ValueError(f'{path}: invalid PE signature')

    opt_off = pe_off + 24
    magic = struct.unpack_from('<H', data, opt_off)[0]
    if magic != 0x20b:
        raise ValueError(f'{path}: expected PE32+ (x86-64), got 0x{magic:x}')

    num_sec = struct.unpack_from('<H', data, pe_off + 6)[0]
    opt_sz = struct.unpack_from('<H', data, pe_off + 20)[0]
    image_base = struct.unpack_from('<Q', data, opt_off + 24)[0]
    sec_off = opt_off + opt_sz

    secs = []
    for i in range(num_sec):
        name = data[sec_off + i * 40:sec_off + i * 40 + 8].rstrip(b'\x00').decode('ascii', errors='replace')
        vsize, vaddr, rsize, roff = struct.unpack_from('<IIII', data, sec_off + i * 40 + 8)
        secs.append((name, vaddr, vsize, roff, rsize))

    def rva2off(rva: int) -> int | None:
        for _, va, vs, ro, rs in secs:
            if va <= rva < va + max(vs, rs):
                return ro + (rva - va)
        return None

    # Primary functions from .pdata (skip UNW_FLAG_CHAININFO split fragments)
    funcs: dict[int, int] = {}
    for name, _, vs, ro, _ in secs:
        if name != '.pdata':
            continue
        for j in range(vs // 12):
            begin_rva, end_rva, unwind_rva = struct.unpack_from('<III', data, ro + j * 12)
            if begin_rva == 0 or end_rva <= begin_rva:
                continue
            uoff = rva2off(unwind_rva)
            if uoff is not None and ((data[uoff] >> 3) & 4):
                continue
            addr = image_base + begin_rva
            size = end_rva - begin_rva
            if addr not in funcs or size > funcs[addr]:
                funcs[addr] = size

    # Imports from PE Import Directory (DataDirectory[1])
    imp_rva, _ = struct.unpack_from('<II', data, opt_off + 112 + 1 * 8)
    imports: list[tuple[str, str]] = []
    if imp_rva:
        off = rva2off(imp_rva)
        while off is not None:
            ilt, _, _, name_rva, iat = struct.unpack_from('<IIIII', data, off)
            if not name_rva:
                break
            noff = rva2off(name_rva)
            dll = data[noff:noff + 256].split(b'\x00', 1)[0].decode('ascii', errors='replace')
            thunk_rva = ilt or iat
            toff = rva2off(thunk_rva)
            idx = 0
            while toff is not None:
                val = struct.unpack_from('<Q', data, toff + idx * 8)[0]
                if val == 0:
                    break
                if val & (1 << 63):
                    sym = f'ordinal_{val & 0xffff}'
                else:
                    hoff = rva2off(val & 0x7fffffff)
                    sym = data[hoff + 2:hoff + 258].split(b'\x00', 1)[0].decode('ascii', errors='replace')
                imports.append((dll, sym))
                idx += 1
            off += 20

    return sorted(funcs.items()), sorted(imports, key=lambda x: (x[0].lower(), x[1]))


def write_csv(path: str, header: list[str], rows: list[list[object]]) -> None:
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator='\n')
    w.writerow(header)
    for r in rows:
        w.writerow(r)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='', encoding='utf-8') as f:
        f.write(buf.getvalue())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--exe', default=None, help='Path to WalkingDead4.exe (default: <root>/game/WalkingDead4.exe)')
    a = ap.parse_args()

    exe = a.exe or os.path.join(a.root, 'game', 'WalkingDead4.exe')
    if not os.path.isfile(exe):
        print(f'seed_pe_symbols: {exe} not found', file=sys.stderr)
        return 1

    funcs, imports = parse_pe(exe)
    importer = os.path.basename(exe)
    write_csv(os.path.join(a.root, 'symbols', 'ghidra_functions.csv'),
              ['address', 'size'],
              [[f'0x{addr:09x}', size] for addr, size in funcs])
    write_csv(os.path.join(a.root, 'symbols', 'imports.csv'),
              ['importer', 'library', 'symbol', 'kind', 'provider'],
              [[importer, dll, sym, 'function', ''] for dll, sym in imports])
    print(f'seed_pe_symbols: wrote {len(funcs)} functions and {len(imports)} imports from {importer}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
