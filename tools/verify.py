#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Differential verification harness for TWD-Final replacements.

usage:
  verify.py --self-test                            compile and run the x86-64 differential test & mutant check
  verify.py hash <system>                          print the SHA-256 source hash for game/<system>/
  verify.py run <function> [--exe PATH] [--record edge]
                                                   run differential verification against original PE machine code
"""
from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile

DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERIF_HEADER = ['function', 'cases', 'option', 'total', 'passed', 'failed', 'quarantined', 'source', 'date']
MATCH_HEADER = [
    'address', 'name', 'compiler', 'flags', 'match_percent', 'objdiff_percent',
    'byte_exact', 'targets_checked', 'source', 'source_sha256', 'date',
]
RESULT_RE = re.compile(
    r'^VERIF_RESULT\s+total=(\d+)\s+passed=(\d+)\s+failed=(\d+)\s+mutant_killed=(\d+)\s+mode=(\S+)$',
    re.MULTILINE,
)

SELF_TEST_C = r"""
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <string.h>
#include <sys/mman.h>
#endif

static uint32_t c_replacement(uint32_t a, uint32_t b) {
    return a * b + 7u;
}

static uint32_t c_mutant_wrong(uint32_t a, uint32_t b) {
    return a * b + 8u;
}

int main(void) {
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _WIN32
    /* Win64 ABI (rcx=a, rdx=b): imul ecx, edx; lea eax, [rcx+7]; ret */
    static const uint8_t code[] = {0x0f, 0xaf, 0xca, 0x8d, 0x41, 0x07, 0xc3};
    void *mem = VirtualAlloc(NULL, sizeof(code), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return 2;
    CopyMemory(mem, code, sizeof(code));
#else
    /* SysV AMD64 ABI (rdi=a, rsi=b): imul edi, esi; lea eax, [rdi+7]; ret */
    static const uint8_t code[] = {0x0f, 0xaf, 0xfe, 0x8d, 0x47, 0x07, 0xc3};
    void *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return 2;
    memcpy(mem, code, sizeof(code));
#endif
    typedef uint32_t (*fn_t)(uint32_t, uint32_t);
    fn_t orig = (fn_t)mem;

    static const uint32_t cases[][2] = {
        {0, 0}, {1, 1}, {7, 6}, {0xffffu, 0xffffu}, {0x12345678u, 3u}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t a = cases[i][0], b = cases[i][1];
        uint32_t want = orig(a, b);
        uint32_t got = c_replacement(a, b);
        if (want != got) {
            fprintf(stderr, "verify self-test mismatch on case %zu: want=%u got=%u\n", i, want, got);
            return 1;
        }
        if (c_mutant_wrong(a, b) == want) {
            fprintf(stderr, "verify self-test mutant survived on case %zu\n", i);
            return 1;
        }
    }
#ifdef _WIN32
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, 4096);
#endif
    printf("verify self-test: 5/5 cases matched, mutant killed (x86_64)\n");
    return 0;
#else
    /* Non-x86_64 fallback: still verify replacement != mutant logic. */
    if (c_replacement(3, 4) != 19u || c_mutant_wrong(3, 4) == 19u) return 1;
    printf("verify self-test: logic & mutant check ok (%s)\n", "non-x86_64");
    return 0;
#endif
}
"""


def find_cc() -> str | None:
    for cand in (os.environ.get('CC'), 'gcc', 'clang', 'cc'):
        if cand and shutil.which(cand):
            return cand
    return None


def system_source_hash(root: str, system: str) -> str:
    sys_dir = os.path.join(root, 'game', system)
    h = hashlib.sha256()
    if not os.path.isdir(sys_dir):
        return h.hexdigest()[:16]
    files = []
    for dp, _, fns in os.walk(sys_dir):
        if os.path.basename(dp) == 'tests':
            continue
        for fn in sorted(fns):
            if fn.endswith(('.c', '.cc', '.cpp', '.h', '.hpp')):
                files.append(os.path.join(dp, fn))
    for p in sorted(files):
        rel = os.path.relpath(p, root).replace('\\', '/')
        h.update(rel.encode('utf-8') + b'\0')
        with open(p, 'rb') as f:
            h.update(f.read())
    return h.hexdigest()[:16]


def pe_extract_bytes(exe_path: str, va: int, size: int) -> bytes:
    with open(exe_path, 'rb') as f:
        data = f.read(4096)
        if len(data) < 0x40 or data[:2] != b'MZ':
            raise ValueError(f'{exe_path}: not a PE executable (missing MZ)')
        e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
        f.seek(e_lfanew)
        pe_hdr = f.read(24 + 240)
        if len(pe_hdr) < 24 + 32 or pe_hdr[:4] != b'PE\x00\x00':
            raise ValueError(f'{exe_path}: invalid PE header at 0x{e_lfanew:x}')
        machine, num_sections, _, _, _, opt_size, _ = struct.unpack_from('<HHIIIHH', pe_hdr, 4)
        if machine != 0x8664:
            raise ValueError(f'{exe_path}: expected x86-64 PE (0x8664), got 0x{machine:04x}')
        opt_magic = struct.unpack_from('<H', pe_hdr, 24)[0]
        if opt_magic != 0x20b:
            raise ValueError(f'{exe_path}: expected PE32+ (0x20b), got 0x{opt_magic:04x}')
        image_base = struct.unpack_from('<Q', pe_hdr, 24 + 24)[0]
        rva = va - image_base
        if rva < 0:
            raise ValueError(f'VA 0x{va:x} is below image base 0x{image_base:x}')

        sec_off = e_lfanew + 24 + opt_size
        f.seek(sec_off)
        sec_table = f.read(num_sections * 40)
        for i in range(num_sections):
            base = i * 40
            vsize, vaddr, raw_size, raw_ptr = struct.unpack_from('<IIII', sec_table, base + 8)
            span = max(vsize, raw_size)
            if vaddr <= rva and rva + size <= vaddr + min(raw_size, span):
                file_off = raw_ptr + (rva - vaddr)
                f.seek(file_off)
                chunk = f.read(size)
                if len(chunk) != size:
                    raise ValueError(f'short read at 0x{file_off:x}: got {len(chunk)}/{size}')
                return chunk
        raise ValueError(f'VA 0x{va:x} (+{size}) not mapped inside any raw PE section')


def lookup_function(root: str, name: str) -> dict[str, str]:
    path = os.path.join(root, 'symbols', 'functions.csv')
    if not os.path.isfile(path):
        raise FileNotFoundError(path)
    with open(path, newline='', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            if row['name'] == name:
                return row
    raise KeyError(f"function '{name}' not found in symbols/functions.csv")


def collect_system_sources(root: str, system: str) -> list[str]:
    sys_dir = os.path.join(root, 'game', system)
    srcs = []
    for dp, _, fns in os.walk(sys_dir):
        if os.path.basename(dp) == 'tests':
            continue
        for fn in sorted(fns):
            if fn.endswith(('.c', '.cc', '.cpp')):
                srcs.append(os.path.join(dp, fn))
    return sorted(srcs)


def record_edge_verification(root: str, func: str, system: str, total: int, passed: int) -> None:
    path = os.path.join(root, 'symbols', 'verification.csv')
    rows: list[dict[str, str]] = []
    if os.path.isfile(path):
        with open(path, newline='', encoding='utf-8') as f:
            rows = list(csv.DictReader(f))
    new_row = {
        'function': func,
        'cases': 'edge',
        'option': '',
        'total': str(total),
        'passed': str(passed),
        'failed': '0',
        'quarantined': '0',
        'source': system_source_hash(root, system),
        'date': datetime.date.today().isoformat(),
    }
    replaced = False
    for i, r in enumerate(rows):
        if r['function'] == func and r['cases'] == 'edge' and r['option'] == '':
            rows[i] = new_row
            replaced = True
            break
    if not replaced:
        rows.append(new_row)
    rows.sort(key=lambda r: (r['function'], r['cases'], r['option']))
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=VERIF_HEADER, lineterminator='\n')
        w.writeheader()
        w.writerows(rows)


def run_self_test() -> int:
    cc = find_cc()
    if not cc:
        print('verify: no C compiler (gcc/clang) found on PATH', file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix='twd_verify_') as tmp:
        src = os.path.join(tmp, 'self_test.c')
        exe = os.path.join(tmp, 'self_test.exe' if platform.system() == 'Windows' else 'self_test')
        with open(src, 'w', encoding='utf-8', newline='\n') as f:
            f.write(SELF_TEST_C)
        subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Werror', src, '-o', exe], check=True)
        res = subprocess.run([exe], check=True, capture_output=True, text=True, encoding='utf-8')
        print(res.stdout.strip())
    return 0


def run_function(root: str, func: str, exe_path: str, record: str | None) -> int:
    if platform.system() != 'Windows' or platform.machine().lower() not in ('amd64', 'x86_64'):
        print('verify: live PE x86-64 differential testing requires Windows x86-64', file=sys.stderr)
        return 1
    cc = find_cc()
    if not cc:
        print('verify: no C compiler (gcc/clang) found on PATH', file=sys.stderr)
        return 1
    if not os.path.isfile(exe_path):
        print(f'verify: game executable not found at {exe_path} (pass --exe PATH)', file=sys.stderr)
        return 1

    row = lookup_function(root, func)
    addr = int(row['address'], 16)
    size = int(row['size'])
    system = row['system']

    test_src = os.path.join(root, 'game', system, 'tests', f'test_{func}.c')
    if not os.path.isfile(test_src):
        print(f'verify: test driver missing: {test_src}', file=sys.stderr)
        return 1
    sys_srcs = collect_system_sources(root, system)
    if not sys_srcs:
        print(f'verify: no implementation sources under game/{system}/', file=sys.stderr)
        return 1

    code_bytes = pe_extract_bytes(exe_path, addr, size)
    code_sha = hashlib.sha256(code_bytes).hexdigest()[:16]
    print(f'verify: extracted {size} bytes @ 0x{addr:x} (sha256={code_sha}) from {os.path.basename(exe_path)}')

    with tempfile.TemporaryDirectory(prefix=f'twd_{func}_') as tmp:
        bin_path = os.path.join(tmp, f'{func}.bin')
        out_exe = os.path.join(tmp, f'test_{func}.exe')
        with open(bin_path, 'wb') as f:
            f.write(code_bytes)

        cmd = [
            cc, '-std=c11', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
            '-ffp-contract=off', '-fno-fast-math',
            f'-I{os.path.join(root, "game", "include")}',
            f'-I{os.path.join(root, "runtime", "include")}',
            test_src, *sys_srcs, '-o', out_exe,
        ]
        subprocess.run(cmd, check=True)
        res = subprocess.run([out_exe, bin_path], capture_output=True, text=True, encoding='utf-8')
        if res.stdout:
            print(res.stdout.strip())
        if res.stderr:
            print(res.stderr.strip(), file=sys.stderr)
        if res.returncode != 0:
            return res.returncode

        m = RESULT_RE.search(res.stdout or '')
        if not m:
            print('verify: test output missing VERIF_RESULT summary line', file=sys.stderr)
            return 1
        total, passed, failed, mutant_killed = (int(m.group(i)) for i in range(1, 5))
        mode = m.group(5)
        if mode != 'x86_64_live_exe' or total <= 0 or failed != 0 or passed != total or mutant_killed != 1:
            print('verify: differential verification requirements not met', file=sys.stderr)
            return 1

        if record == 'edge':
            record_edge_verification(root, func, system, total, passed)
            print(f'verify: recorded edge verification in symbols/verification.csv ({passed}/{total})')
    return 0


def find_msvc(root: str) -> tuple[str, str, dict[str, str]] | None:
    msvc_root = os.path.join(root, '.tools', 'msvc')
    vc_base = os.path.join(msvc_root, 'VC', 'Tools', 'MSVC')
    sdk_inc_base = os.path.join(msvc_root, 'Windows Kits', '10', 'Include')
    sdk_lib_base = os.path.join(msvc_root, 'Windows Kits', '10', 'Lib')
    sdk_bin_base = os.path.join(msvc_root, 'Windows Kits', '10', 'bin')
    if not os.path.isdir(vc_base) or not os.path.isdir(sdk_inc_base):
        return None
    vc_vers = sorted(os.listdir(vc_base))
    sdk_vers = sorted(os.listdir(sdk_inc_base))
    if not vc_vers or not sdk_vers:
        return None
    vc_ver = vc_vers[-1]
    sdk_ver = sdk_vers[-1]
    vc_dir = os.path.join(vc_base, vc_ver)
    cl_exe = os.path.join(vc_dir, 'bin', 'Hostx64', 'x64', 'cl.exe')
    if not os.path.isfile(cl_exe):
        return None
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([
        os.path.join(vc_dir, 'bin', 'Hostx64', 'x64'),
        os.path.join(sdk_bin_base, sdk_ver, 'x64'),
        os.path.join(sdk_bin_base, sdk_ver, 'x64', 'ucrt'),
        env.get('PATH', ''),
    ])
    env['INCLUDE'] = os.pathsep.join([
        os.path.join(vc_dir, 'include'),
        os.path.join(sdk_inc_base, sdk_ver, 'ucrt'),
        os.path.join(sdk_inc_base, sdk_ver, 'shared'),
        os.path.join(sdk_inc_base, sdk_ver, 'um'),
    ])
    env['LIB'] = os.pathsep.join([
        os.path.join(vc_dir, 'lib', 'x64'),
        os.path.join(sdk_lib_base, sdk_ver, 'ucrt', 'x64'),
        os.path.join(sdk_lib_base, sdk_ver, 'um', 'x64'),
    ])
    return cl_exe, f'msvc-{vc_ver}', env


def parse_coff_text_symbols(obj_path: str) -> dict[str, bytes]:
    with open(obj_path, 'rb') as f:
        raw = f.read()
    _, nsec, _, sym_off, nsym, opt_sz, _ = struct.unpack_from('<HHIIIHH', raw, 0)
    secs = []
    for i in range(nsec):
        off = 20 + opt_sz + i * 40
        _, _, rsz, rptr = struct.unpack_from('<IIII', raw, off + 8)
        secs.append((rsz, rptr))
    str_off = sym_off + nsym * 18

    def sym_name(rec: bytes) -> str:
        raw_n = rec[:8]
        if raw_n[:4] == b'\x00\x00\x00\x00':
            idx = struct.unpack_from('<I', raw_n, 4)[0]
            end = raw.find(b'\x00', str_off + idx)
            return raw[str_off + idx:end].decode('ascii', errors='replace')
        return raw_n.rstrip(b'\x00').decode('ascii', errors='replace')

    by_sec: dict[int, list[tuple[int, str]]] = {}
    i = 0
    while i < nsym:
        rec = raw[sym_off + i * 18:sym_off + (i + 1) * 18]
        name = sym_name(rec)
        val, sec_idx, _, _, naux = struct.unpack_from('<IhHBB', rec, 8)
        if sec_idx > 0 and name.startswith('twd_'):
            by_sec.setdefault(sec_idx, []).append((val, name))
        i += 1 + naux

    out: dict[str, bytes] = {}
    for sec_idx, items in by_sec.items():
        items.sort()
        rsz, rptr = secs[sec_idx - 1]
        for idx, (val, name) in enumerate(items):
            end = items[idx + 1][0] if idx + 1 < len(items) else rsz
            out[name] = raw[rptr + val:rptr + end]
    return out


def find_definition_file(root: str, system: str, sym_name: str) -> str:
    pat = re.compile(rf'\b{re.escape(sym_name)}\s*\(')
    for p in collect_system_sources(root, system):
        with open(p, encoding='utf-8', errors='replace') as f:
            if pat.search(f.read()):
                return p
    return ''


def record_matching_rows(root: str, updates: list[dict[str, str]]) -> None:
    path = os.path.join(root, 'symbols', 'matching.csv')
    rows: list[dict[str, str]] = []
    if os.path.isfile(path):
        with open(path, newline='', encoding='utf-8') as f:
            rows = list(csv.DictReader(f))
    by_addr = {r['address']: r for r in rows}
    for u in updates:
        by_addr[u['address']] = u
    merged = sorted(by_addr.values(), key=lambda r: int(r['address'], 16))
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=MATCH_HEADER, lineterminator='\n')
        w.writeheader()
        w.writerows(merged)


def run_match(root: str, target_func: str | None, exe_path: str, record: bool) -> int:
    msvc = find_msvc(root)
    if not msvc:
        print('verify: portable MSVC not found under .tools/msvc', file=sys.stderr)
        return 1
    if not os.path.isfile(exe_path):
        print(f'verify: game executable not found at {exe_path} (pass --exe PATH)', file=sys.stderr)
        return 1
    cl_exe, compiler_id, env = msvc
    flags = '/std:c11 /O2 /W4 /WX /fp:precise /DNDEBUG'

    with open(os.path.join(root, 'symbols', 'functions.csv'), newline='', encoding='utf-8') as f:
        funcs = [r for r in csv.DictReader(f) if target_func is None or r['name'] == target_func]
    if not funcs:
        print(f'verify: no matching functions in symbols/functions.csv', file=sys.stderr)
        return 1

    compiled: dict[str, bytes] = {}
    with tempfile.TemporaryDirectory(prefix='twd_msvc_match_') as tmp:
        systems = sorted({r['system'] for r in funcs})
        for system in systems:
            srcs = collect_system_sources(root, system)
            if not srcs:
                continue
            cmd = [
                cl_exe, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/fp:precise', '/DNDEBUG',
                f'/I{os.path.join(root, "runtime", "include")}',
                f'/I{os.path.join(root, "game", "include")}',
                f'/Fo:{tmp}\\', '/c', *srcs,
            ]
            subprocess.run(cmd, env=env, check=True)
            for src in srcs:
                stem = os.path.splitext(os.path.basename(src))[0]
                obj = os.path.join(tmp, stem + '.obj')
                if os.path.isfile(obj):
                    compiled.update(parse_coff_text_symbols(obj))

    updates: list[dict[str, str]] = []
    today = datetime.date.today().isoformat()
    for r in funcs:
        addr = int(r['address'], 16)
        sz = int(r['size'])
        name = r['name']
        orig = pe_extract_bytes(exe_path, addr, sz)
        got = compiled.get('twd_' + name, b'')
        exact = (orig == got)
        if exact:
            pct = '100.00'
        else:
            common = min(len(orig), len(got))
            same = sum(1 for i in range(common) if orig[i] == got[i])
            pct = f'{100.0 * same / max(len(orig), len(got), 1):.2f}'
        print(f'{name:28s} orig={len(orig):3d}B  cl={len(got):3d}B  exact={exact} ({pct}%)')
        if exact and record:
            src_abs = find_definition_file(root, r['system'], 'twd_' + name)
            rel = os.path.relpath(src_abs, root).replace('\\', '/') if src_abs else ''
            src_sha = ''
            if src_abs and os.path.isfile(src_abs):
                with open(src_abs, 'rb') as sf:
                    src_sha = hashlib.sha256(sf.read()).hexdigest()[:16]
            updates.append({
                'address': r['address'],
                'name': name,
                'compiler': compiler_id,
                'flags': flags,
                'match_percent': pct,
                'objdiff_percent': '',
                'byte_exact': 'yes',
                'targets_checked': 'x86_64',
                'source': rel,
                'source_sha256': src_sha,
                'date': today,
            })

    if record and updates:
        record_matching_rows(root, updates)
        print(f'verify: recorded {len(updates)} byte-exact match(es) in symbols/matching.csv')
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--exe', default=None, help='Path to WalkingDead4.exe')
    ap.add_argument('--record', nargs='?', const='edge', choices=['edge', 'match'], default=None)
    ap.add_argument('subcmd', nargs='*')
    a = ap.parse_args()

    if a.self_test:
        return run_self_test()
    if a.subcmd and a.subcmd[0] == 'hash' and len(a.subcmd) == 2:
        print(system_source_hash(a.root, a.subcmd[1]))
        return 0
    if a.subcmd and a.subcmd[0] == 'run' and len(a.subcmd) == 2:
        exe = a.exe or os.path.join(a.root, 'game', 'WalkingDead4.exe')
        return run_function(a.root, a.subcmd[1], exe, a.record)
    if a.subcmd and a.subcmd[0] == 'match' and len(a.subcmd) in (1, 2):
        exe = a.exe or os.path.join(a.root, 'game', 'WalkingDead4.exe')
        target = a.subcmd[1] if len(a.subcmd) == 2 else None
        return run_match(a.root, target, exe, a.record is not None)
    ap.print_help()
    return 0


if __name__ == '__main__':
    sys.exit(main())
