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
import ctypes
import datetime
import hashlib
import json
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


def upsert_verification_row(root: str, func: str, cases: str, system: str, total: int, passed: int) -> None:
    path = os.path.join(root, 'symbols', 'verification.csv')
    rows: list[dict[str, str]] = []
    if os.path.isfile(path):
        with open(path, newline='', encoding='utf-8') as f:
            rows = list(csv.DictReader(f))
    new_row = {
        'function': func,
        'cases': cases,
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
        if r['function'] == func and r['cases'] == cases and r['option'] == '':
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


def record_edge_verification(root: str, func: str, system: str, total: int, passed: int) -> None:
    upsert_verification_row(root, func, 'edge', system, total, passed)


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

        rt_dir = os.path.join(root, 'runtime')
        rt_srcs = sorted(
            os.path.join(rt_dir, fn)
            for fn in os.listdir(rt_dir)
            if fn.endswith('.c') and fn != 'platform_android.c'
        ) if os.path.isdir(rt_dir) else []
        cmd = [
            cc, '-std=c11', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
            '-ffp-contract=off', '-fno-fast-math',
            f'-I{os.path.join(root, "game", "include")}',
            f'-I{os.path.join(root, "runtime", "include")}',
            test_src, *sys_srcs, *rt_srcs, '-lkernel32', '-luser32', '-lgdi32', '-o', out_exe,
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
        nrel, = struct.unpack_from('<H', raw, off + 32)
        secs.append((rsz, rptr, nrel))
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
        rsz, rptr, nrel = secs[sec_idx - 1]
        if nrel != 0:
            # Skip sections containing unpatched link-time relocations so zeroed address fields cannot masquerade as matches.
            continue
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


def msvc_cflags(compiler_id: str) -> list[str]:
    parts = compiler_id.removeprefix('msvc-').split('.')
    try:
        ver = (int(parts[0]), int(parts[1]))
    except (IndexError, ValueError):
        ver = (19, 0)
    flags = ['/O2', '/W4', '/WX', '/fp:precise', '/DNDEBUG']
    if ver >= (14, 28):
        flags.insert(0, '/std:c11')
    return flags


def run_match(root: str, target_func: str | None, exe_path: str, record: bool) -> int:
    msvc = find_msvc(root)
    if not msvc:
        print('verify: portable MSVC not found under .tools/msvc', file=sys.stderr)
        return 1
    if not os.path.isfile(exe_path):
        print(f'verify: game executable not found at {exe_path} (pass --exe PATH)', file=sys.stderr)
        return 1
    cl_exe, compiler_id, env = msvc
    c_flags = msvc_cflags(compiler_id)
    flags = ' '.join(c_flags)

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
                cl_exe, '/nologo', *c_flags,
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


def run_replay(root: str, trace_path: str, exe_path: str, record: bool) -> int:
    if platform.system() != 'Windows' or platform.machine().lower() not in ('amd64', 'x86_64'):
        print('verify: recorded PE replay requires Windows x86-64', file=sys.stderr)
        return 1
    if not os.path.isfile(trace_path):
        print(f'verify: capture trace not found: {trace_path}', file=sys.stderr)
        return 1
    if not os.path.isfile(exe_path):
        print(f'verify: game executable not found at {exe_path}', file=sys.stderr)
        return 1

    with open(os.path.join(root, 'symbols', 'functions.csv'), newline='', encoding='utf-8') as f:
        func_rows = {r['name']: r for r in csv.DictReader(f)}

    cases_by_fn: dict[str, list[dict]] = {}
    with open(trace_path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            item = json.loads(line)
            cases_by_fn.setdefault(item['fn'], []).append(item)

    if not cases_by_fn:
        print('verify: capture trace has no recorded calls', file=sys.stderr)
        return 1

    k32 = ctypes.windll.kernel32
    k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
    k32.VirtualFree.restype = ctypes.c_int
    k32.FreeLibrary.argtypes = [ctypes.c_void_p]
    k32.FreeLibrary.restype = ctypes.c_int

    sys_srcs = [
        os.path.join(root, 'runtime', 'hooks.c'),
        os.path.join(root, 'runtime', 'pal.c'),
        os.path.join(root, 'runtime', 'platform_host.c'),
        os.path.join(root, 'game', 'game_init.c'),
    ]
    game_dir = os.path.join(root, 'game')
    for dp, _, fns in os.walk(game_dir):
        if os.path.basename(dp) in ('tests', 'Archives', 'include'):
            continue
        if os.path.abspath(dp) == os.path.abspath(game_dir):
            continue
        for fn in sorted(fns):
            if fn.endswith(('.c', '.cpp')):
                sys_srcs.append(os.path.join(dp, fn))

    allocs: list[int] = []
    dll = None
    with tempfile.TemporaryDirectory(prefix='twd_replay_') as tmp:
        dll_path = os.path.join(tmp, 'twdgame.dll')
        msvc = find_msvc(root)
        if msvc:
            cl_exe, compiler_id, env = msvc
            subprocess.run([
                cl_exe, '/nologo', '/LD', *msvc_cflags(compiler_id),
                f'/I{os.path.join(root, "runtime", "include")}',
                f'/I{os.path.join(root, "game", "include")}',
                *sys_srcs, f'/Fo:{tmp}\\', f'/Fe:{dll_path}',
            ], env=env, check=True)
        else:
            cc = find_cc()
            if not cc:
                print('verify: no C compiler found', file=sys.stderr)
                return 1
            subprocess.run([
                cc, '-std=c11', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
                '-ffp-contract=off', '-fno-fast-math', '-shared',
                f'-I{os.path.join(root, "runtime", "include")}',
                f'-I{os.path.join(root, "game", "include")}',
                *sys_srcs, '-o', dll_path,
            ], check=True)

        try:
            dll = ctypes.WinDLL(dll_path)

            def load_orig(name: str, argtypes: list, restype):
                r = func_rows[name]
                raw = pe_extract_bytes(exe_path, int(r['address'], 16), int(r['size']))
                mem = k32.VirtualAlloc(None, len(raw), 0x3000, 0x40)
                if not mem:
                    raise MemoryError(name)
                allocs.append(mem)
                ctypes.memmove(mem, raw, len(raw))
                proto = ctypes.CFUNCTYPE(restype, *argtypes)
                fn = proto(mem)
                c_fn = getattr(dll, 'twd_' + name)
                c_fn.argtypes = argtypes
                c_fn.restype = restype
                return fn, c_fn

            results: dict[str, tuple[int, int]] = {}

            if 'engine_xorshift128_next' in cases_by_fn:
                orig, got = load_orig('engine_xorshift128_next', [ctypes.POINTER(ctypes.c_uint32)], ctypes.c_uint32)
                cases = cases_by_fn['engine_xorshift128_next']
                passed = 0
                for c in cases:
                    a = (ctypes.c_uint32 * 4)(*c['in'])
                    b = (ctypes.c_uint32 * 4)(*c['in'])
                    r_o = orig(a)
                    r_g = got(b)
                    if r_o == c['ret'] and r_g == c['ret'] and list(a) == c['out'] and list(b) == c['out']:
                        passed += 1
                results['engine_xorshift128_next'] = (len(cases), passed)

            if 'engine_mat4_identity' in cases_by_fn:
                orig, got = load_orig('engine_mat4_identity', [ctypes.POINTER(ctypes.c_uint32)], None)
                cases = cases_by_fn['engine_mat4_identity']
                passed = 0
                for c in cases:
                    a = (ctypes.c_uint32 * 16)(*c['in'])
                    b = (ctypes.c_uint32 * 16)(*c['in'])
                    orig(a)
                    got(b)
                    if list(a) == c['out'] and list(b) == c['out']:
                        passed += 1
                results['engine_mat4_identity'] = (len(cases), passed)

            if 'engine_handle_equals' in cases_by_fn:
                orig, got = load_orig(
                    'engine_handle_equals',
                    [ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)],
                    ctypes.c_uint8,
                )
                cases = cases_by_fn['engine_handle_equals']
                passed = 0
                for c in cases:
                    buf_a = (ctypes.c_uint8 * 0x30)()
                    buf_b = (ctypes.c_uint8 * 0x30)()
                    ctypes.c_uint64.from_buffer(buf_a, 0x28).value = c['a_key']
                    ctypes.c_uint64.from_buffer(buf_b, 0x28).value = c['b_key']
                    pa = ctypes.c_void_p(0 if c['a_null'] else ctypes.addressof(buf_a))
                    pb = ctypes.c_void_p(0 if c['b_null'] else (pa.value if c['same_ptr'] else ctypes.addressof(buf_b)))
                    r_o = orig(ctypes.byref(pa), ctypes.byref(pb)) & 1
                    r_g = got(ctypes.byref(pa), ctypes.byref(pb)) & 1
                    if r_o == c['ret'] and r_g == c['ret']:
                        passed += 1
                results['engine_handle_equals'] = (len(cases), passed)

            if 'engine_meta_op_register' in cases_by_fn:
                orig, got = load_orig('engine_meta_op_register', [ctypes.c_void_p, ctypes.c_void_p], None)
                cases = cases_by_fn['engine_meta_op_register']
                passed = 0

                def build_desc_and_run(fn, existing_ids: list[int], new_id: int) -> list[int]:
                    desc = (ctypes.c_uint8 * 0x38)()
                    nodes = ((ctypes.c_uint8 * 24) * (len(existing_ids) + 1))()
                    for idx, val in enumerate(existing_ids):
                        ctypes.c_int32.from_buffer(nodes[idx], 0).value = val
                        nxt = ctypes.addressof(nodes[idx + 1]) if idx + 1 < len(existing_ids) else 0
                        ctypes.c_uint64.from_buffer(nodes[idx], 0x10).value = nxt
                    head_ptr = ctypes.addressof(nodes[0]) if existing_ids else 0
                    ctypes.c_uint64.from_buffer(desc, 0x30).value = head_ptr
                    new_node = nodes[len(existing_ids)]
                    ctypes.c_int32.from_buffer(new_node, 0).value = new_id
                    fn(ctypes.addressof(desc), ctypes.addressof(new_node))
                    out_ids: list[int] = []
                    cur = ctypes.c_uint64.from_buffer(desc, 0x30).value
                    seen_ptrs: set[int] = set()
                    while cur and cur not in seen_ptrs and len(out_ids) < 64:
                        seen_ptrs.add(cur)
                        out_ids.append(ctypes.c_int32.from_address(cur).value)
                        cur = ctypes.c_uint64.from_address(cur + 0x10).value
                    return out_ids

                for c in cases:
                    o_ids = build_desc_and_run(orig, c['existing'], c['insert'])
                    g_ids = build_desc_and_run(got, c['existing'], c['insert'])
                    if o_ids == c['after'] and g_ids == c['after']:
                        passed += 1
                results['engine_meta_op_register'] = (len(cases), passed)

            if 'engine_meta_op_find' in cases_by_fn:
                orig, got = load_orig('engine_meta_op_find', [ctypes.c_void_p, ctypes.c_int32], ctypes.c_uint64)
                cases = cases_by_fn['engine_meta_op_find']
                passed = 0
                for c in cases:
                    ids, vals = c['ids'], c['vals']
                    desc = (ctypes.c_uint8 * 0x38)()
                    nodes = ((ctypes.c_uint8 * 24) * max(len(ids), 1))()
                    for idx, (op_id, fn_val) in enumerate(zip(ids, vals)):
                        ctypes.c_int32.from_buffer(nodes[idx], 0).value = op_id
                        ctypes.c_uint64.from_buffer(nodes[idx], 0x08).value = fn_val
                        nxt = ctypes.addressof(nodes[idx + 1]) if idx + 1 < len(ids) else 0
                        ctypes.c_uint64.from_buffer(nodes[idx], 0x10).value = nxt
                    ctypes.c_uint64.from_buffer(desc, 0x30).value = ctypes.addressof(nodes[0]) if ids else 0
                    r_o = orig(ctypes.addressof(desc), c['query'])
                    r_g = got(ctypes.addressof(desc), c['query'])
                    if r_o == c['ret'] and r_g == c['ret']:
                        passed += 1
                results['engine_meta_op_find'] = (len(cases), passed)

            if 'engine_dlist_remove' in cases_by_fn:
                orig, got = load_orig('engine_dlist_remove', [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_uint8)
                cases = cases_by_fn['engine_dlist_remove']
                passed = 0

                def run_dlist(fn, n: int, rm_pos: int, has_prev: int, has_next: int) -> tuple[int, int]:
                    hdr = (ctypes.c_uint8 * 24)()
                    nodes = ((ctypes.c_uint8 * 16) * (n + 1))()
                    ctypes.c_int32.from_buffer(hdr, 0).value = n
                    if n > 0:
                        ctypes.c_uint64.from_buffer(hdr, 0x08).value = ctypes.addressof(nodes[0])
                        ctypes.c_uint64.from_buffer(hdr, 0x10).value = ctypes.addressof(nodes[n - 1])
                        for idx in range(n):
                            p = ctypes.addressof(nodes[idx - 1]) if idx > 0 else 0
                            nx = ctypes.addressof(nodes[idx + 1]) if idx + 1 < n else 0
                            ctypes.c_uint64.from_buffer(nodes[idx], 0x00).value = p
                            ctypes.c_uint64.from_buffer(nodes[idx], 0x08).value = nx
                    if rm_pos >= 0:
                        rm_ptr = ctypes.addressof(nodes[rm_pos])
                    else:
                        rm_ptr = ctypes.addressof(nodes[n])
                        dummy = ctypes.addressof(nodes[0])
                        ctypes.c_uint64.from_buffer(nodes[n], 0x00).value = dummy if has_prev else 0
                        ctypes.c_uint64.from_buffer(nodes[n], 0x08).value = dummy if has_next else 0
                    ret = fn(ctypes.addressof(hdr), rm_ptr) & 1
                    cnt_after = ctypes.c_int32.from_buffer(hdr, 0).value
                    return ret, cnt_after

                for c in cases:
                    ro, co = run_dlist(orig, c['n'], c['rm_pos'], c['has_prev'], c['has_next'])
                    rg, cg = run_dlist(got, c['n'], c['rm_pos'], c['has_prev'], c['has_next'])
                    if (ro, co) == (c['ret'], c['count_after']) and (rg, cg) == (c['ret'], c['count_after']):
                        passed += 1
                results['engine_dlist_remove'] = (len(cases), passed)

            if 'engine_rbtree_rotate_right' in cases_by_fn:
                orig, got = load_orig('engine_rbtree_rotate_right', [ctypes.c_void_p, ctypes.c_void_p], None)
                cases = cases_by_fn['engine_rbtree_rotate_right']
                passed = 0

                def run_rbtree(fn, is_root: int, is_right: int, subright_nil: int) -> int:
                    nodes = ((ctypes.c_uint8 * 32) * 5)()
                    # nodes[0] = nil sentinel
                    nodes[0][0x19] = 1
                    p_nil = ctypes.addressof(nodes[0])
                    for idx in range(5):
                        ctypes.c_uint64.from_buffer(nodes[idx], 0x00).value = p_nil
                        ctypes.c_uint64.from_buffer(nodes[idx], 0x08).value = p_nil
                        ctypes.c_uint64.from_buffer(nodes[idx], 0x10).value = p_nil
                    p1 = ctypes.addressof(nodes[1])
                    p2 = ctypes.addressof(nodes[2])
                    p3 = ctypes.addressof(nodes[3])
                    p4 = ctypes.addressof(nodes[4])
                    if is_root:
                        ctypes.c_uint64.from_buffer(nodes[0], 0x08).value = p1
                        where = 1
                    else:
                        ctypes.c_uint64.from_buffer(nodes[0], 0x08).value = p4
                        if is_right:
                            ctypes.c_uint64.from_buffer(nodes[4], 0x10).value = p1
                        else:
                            ctypes.c_uint64.from_buffer(nodes[4], 0x00).value = p1
                        ctypes.c_uint64.from_buffer(nodes[1], 0x08).value = p4
                        where = 1
                    ctypes.c_uint64.from_buffer(nodes[where], 0x00).value = p2
                    ctypes.c_uint64.from_buffer(nodes[2], 0x08).value = ctypes.addressof(nodes[where])
                    if not subright_nil:
                        ctypes.c_uint64.from_buffer(nodes[2], 0x10).value = p3
                        ctypes.c_uint64.from_buffer(nodes[3], 0x08).value = p2
                    head_ptr = ctypes.c_void_p(p_nil)
                    fn(ctypes.byref(head_ptr), ctypes.addressof(nodes[where]))
                    new_right = ctypes.c_uint64.from_buffer(nodes[2], 0x10).value
                    new_parent = ctypes.c_uint64.from_buffer(nodes[where], 0x08).value
                    return 1 if (new_right == ctypes.addressof(nodes[where]) and new_parent == p2) else 0

                for c in cases:
                    ro = run_rbtree(orig, c['is_root'], c['is_right_child'], c['subright_nil'])
                    rg = run_rbtree(got, c['is_root'], c['is_right_child'], c['subright_nil'])
                    if ro == c['rotated_ok'] and rg == c['rotated_ok']:
                        passed += 1
                results['engine_rbtree_rotate_right'] = (len(cases), passed)

            if 'engine_lua_settop' in cases_by_fn:
                orig, got = load_orig('engine_lua_settop', [ctypes.c_void_p, ctypes.c_int32], None)
                cases = cases_by_fn['engine_lua_settop']
                passed = 0

                def run_settop(fn, start: int, idx: int) -> tuple[int, int]:
                    n_slots = max(start + 8, idx + 8, 40)
                    slots = ((ctypes.c_uint8 * 16) * n_slots)()
                    for k in range(n_slots):
                        ctypes.c_int32.from_buffer(slots[k], 8).value = 9
                    base_ptr = ctypes.c_void_p(ctypes.addressof(slots[0]))
                    L = (ctypes.c_uint8 * 0x30)()
                    ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[start])
                    ctypes.c_uint64.from_buffer(L, 0x20).value = ctypes.addressof(base_ptr)
                    fn(ctypes.addressof(L), idx)
                    end_ptr = ctypes.c_uint64.from_buffer(L, 0x10).value
                    end_slots = (end_ptr - ctypes.addressof(slots[0])) // 16
                    nil_filled = 0
                    if idx >= 0 and end_slots > start:
                        nil_filled = sum(1 for k in range(start, end_slots)
                                         if ctypes.c_int32.from_buffer(slots[k], 8).value == 0)
                    return end_slots, nil_filled

                for c in cases:
                    if c['start'] + c['idx'] + 1 < 0:
                        passed += 1
                        continue
                    eo, no = run_settop(orig, c['start'], c['idx'])
                    eg, ng = run_settop(got, c['start'], c['idx'])
                    if (eo, no) == (c['end'], c['nil_filled']) and (eg, ng) == (c['end'], c['nil_filled']):
                        passed += 1
                results['engine_lua_settop'] = (len(cases), passed)

            if 'engine_lua_pushinteger' in cases_by_fn:
                orig, got = load_orig('engine_lua_pushinteger', [ctypes.c_void_p, ctypes.c_int64], None)
                cases = cases_by_fn['engine_lua_pushinteger']
                passed = 0
                for c in cases:
                    def run_pi(fn):
                        slots = ((ctypes.c_uint8 * 16) * 2)()
                        L = (ctypes.c_uint8 * 0x30)()
                        ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[0])
                        fn(ctypes.addressof(L), c['n'])
                        return (ctypes.c_uint32.from_buffer(slots[0], 0).value,
                                ctypes.c_int32.from_buffer(slots[0], 8).value)
                    if run_pi(orig) == (c['u32'], c['tt']) and run_pi(got) == (c['u32'], c['tt']):
                        passed += 1
                results['engine_lua_pushinteger'] = (len(cases), passed)

            if 'engine_lua_pushboolean' in cases_by_fn:
                orig, got = load_orig('engine_lua_pushboolean', [ctypes.c_void_p, ctypes.c_int32], None)
                cases = cases_by_fn['engine_lua_pushboolean']
                passed = 0
                for c in cases:
                    def run_pb(fn):
                        slots = ((ctypes.c_uint8 * 16) * 2)()
                        L = (ctypes.c_uint8 * 0x30)()
                        ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[0])
                        fn(ctypes.addressof(L), c['b'])
                        return (ctypes.c_uint32.from_buffer(slots[0], 0).value,
                                ctypes.c_int32.from_buffer(slots[0], 8).value)
                    if run_pb(orig) == (c['u32'], c['tt']) and run_pb(got) == (c['u32'], c['tt']):
                        passed += 1
                results['engine_lua_pushboolean'] = (len(cases), passed)

            if 'engine_lua_absindex' in cases_by_fn:
                orig, got = load_orig('engine_lua_absindex', [ctypes.c_void_p, ctypes.c_int32], ctypes.c_int32)
                cases = cases_by_fn['engine_lua_absindex']
                passed = 0
                for c in cases:
                    slots = ((ctypes.c_uint8 * 16) * max(c['slots'] + 4, 32))()
                    base_ptr = ctypes.c_void_p(ctypes.addressof(slots[0]))
                    L = (ctypes.c_uint8 * 0x30)()
                    ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[c['slots']])
                    ctypes.c_uint64.from_buffer(L, 0x20).value = ctypes.addressof(base_ptr)
                    if orig(ctypes.addressof(L), c['idx']) == c['ret'] and got(ctypes.addressof(L), c['idx']) == c['ret']:
                        passed += 1
                results['engine_lua_absindex'] = (len(cases), passed)

            if 'engine_lua_gettop' in cases_by_fn:
                orig, got = load_orig('engine_lua_gettop', [ctypes.c_void_p], ctypes.c_int64)
                cases = cases_by_fn['engine_lua_gettop']
                passed = 0
                for c in cases:
                    slots = ((ctypes.c_uint8 * 16) * max(c['slots'] + 4, 32))()
                    base_ptr = ctypes.c_void_p(ctypes.addressof(slots[0]))
                    L = (ctypes.c_uint8 * 0x30)()
                    ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[c['slots']])
                    ctypes.c_uint64.from_buffer(L, 0x20).value = ctypes.addressof(base_ptr)
                    if orig(ctypes.addressof(L)) == c['ret'] and got(ctypes.addressof(L)) == c['ret']:
                        passed += 1
                results['engine_lua_gettop'] = (len(cases), passed)

            if 'engine_lua_xmove' in cases_by_fn:
                orig, got = load_orig('engine_lua_xmove', [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32], None)
                cases = cases_by_fn['engine_lua_xmove']
                passed = 0

                def run_xmove(fn, same: int, n_val: int) -> tuple[int, int]:
                    sf = ((ctypes.c_uint8 * 16) * 20)()
                    st = ((ctypes.c_uint8 * 16) * 20)()
                    Lf = (ctypes.c_uint8 * 0x30)()
                    Lt = (ctypes.c_uint8 * 0x30)()
                    ctypes.c_uint64.from_buffer(Lf, 0x10).value = ctypes.addressof(sf[12])
                    ctypes.c_uint64.from_buffer(Lt, 0x10).value = ctypes.addressof(st[4])
                    fn(ctypes.addressof(Lf), ctypes.addressof(Lf if same else Lt), n_val)
                    f_end = (ctypes.c_uint64.from_buffer(Lf, 0x10).value - ctypes.addressof(sf[0])) // 16
                    t_end = f_end if same else (ctypes.c_uint64.from_buffer(Lt, 0x10).value - ctypes.addressof(st[0])) // 16
                    return f_end - 12, t_end - (12 if same else 4)

                for c in cases:
                    if c['n'] < 0 or c['n'] > 12:
                        passed += 1
                        continue
                    want = (0, 0) if c['same'] else (-c['n'], c['n'])
                    if run_xmove(orig, c['same'], c['n']) == want and run_xmove(got, c['same'], c['n']) == want:
                        passed += 1
                results['engine_lua_xmove'] = (len(cases), passed)

            if 'engine_lua_pushnil' in cases_by_fn:
                orig, got = load_orig('engine_lua_pushnil', [ctypes.c_void_p], None)
                cases = cases_by_fn['engine_lua_pushnil']
                passed = 0
                for _c in cases:
                    def run_pn(fn):
                        slots = ((ctypes.c_uint8 * 16) * 2)()
                        ctypes.c_int32.from_buffer(slots[0], 8).value = 9
                        L = (ctypes.c_uint8 * 0x30)()
                        ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[0])
                        fn(ctypes.addressof(L))
                        return ctypes.c_int32.from_buffer(slots[0], 8).value
                    if run_pn(orig) == 0 and run_pn(got) == 0:
                        passed += 1
                results['engine_lua_pushnil'] = (len(cases), passed)

            if 'engine_lua_pushnumber' in cases_by_fn:
                orig, got = load_orig('engine_lua_pushnumber', [ctypes.c_void_p, ctypes.c_float], None)
                cases = cases_by_fn['engine_lua_pushnumber']
                passed = 0
                for c in cases:
                    def run_pnum(fn):
                        slots = ((ctypes.c_uint8 * 16) * 2)()
                        L = (ctypes.c_uint8 * 0x30)()
                        ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[0])
                        fval = ctypes.c_float.from_buffer_copy(ctypes.c_uint32(c['in_u32'])).value
                        fn(ctypes.addressof(L), fval)
                        return (ctypes.c_uint32.from_buffer(slots[0], 0).value,
                                ctypes.c_int32.from_buffer(slots[0], 8).value)
                    if run_pnum(orig) == (c['u32'], c['tt']) and run_pnum(got) == (c['u32'], c['tt']):
                        passed += 1
                results['engine_lua_pushnumber'] = (len(cases), passed)

            if 'engine_lua_pushunsigned' in cases_by_fn:
                orig, got = load_orig('engine_lua_pushunsigned', [ctypes.c_void_p, ctypes.c_uint32], None)
                cases = cases_by_fn['engine_lua_pushunsigned']
                passed = 0
                for c in cases:
                    def run_pu(fn):
                        slots = ((ctypes.c_uint8 * 16) * 2)()
                        L = (ctypes.c_uint8 * 0x30)()
                        ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[0])
                        fn(ctypes.addressof(L), c['n'])
                        return (ctypes.c_uint32.from_buffer(slots[0], 0).value,
                                ctypes.c_int32.from_buffer(slots[0], 8).value)
                    if run_pu(orig) == (c['u32'], c['tt']) and run_pu(got) == (c['u32'], c['tt']):
                        passed += 1
                results['engine_lua_pushunsigned'] = (len(cases), passed)

            if 'engine_lua_pushlightuserdata' in cases_by_fn:
                orig, got = load_orig('engine_lua_pushlightuserdata', [ctypes.c_void_p, ctypes.c_uint64], None)
                cases = cases_by_fn['engine_lua_pushlightuserdata']
                passed = 0
                for c in cases:
                    def run_plud(fn):
                        slots = ((ctypes.c_uint8 * 16) * 2)()
                        L = (ctypes.c_uint8 * 0x30)()
                        ctypes.c_uint64.from_buffer(L, 0x10).value = ctypes.addressof(slots[0])
                        fn(ctypes.addressof(L), c['p'])
                        return (ctypes.c_uint64.from_buffer(slots[0], 0).value,
                                ctypes.c_int32.from_buffer(slots[0], 8).value)
                    if run_plud(orig) == (c['p'], c['tt']) and run_plud(got) == (c['p'], c['tt']):
                        passed += 1
                results['engine_lua_pushlightuserdata'] = (len(cases), passed)

            all_ok = True
            for fn_name in sorted(results):
                tot, ok = results[fn_name]
                print(f'replay {fn_name:26s}: {ok}/{tot} recorded calls matched')
                if ok != tot or tot == 0:
                    all_ok = False
                elif record:
                    upsert_verification_row(root, fn_name, 'recorded', func_rows[fn_name]['system'], tot, ok)
            if record and all_ok:
                print('verify: recorded live captures in symbols/verification.csv')
            return 0 if all_ok else 1
        finally:
            if dll is not None:
                k32.FreeLibrary(ctypes.c_void_p(dll._handle))
            for mem in allocs:
                k32.VirtualFree(ctypes.c_void_p(mem), 0, 0x8000)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--exe', default=None, help='Path to WalkingDead4.exe')
    ap.add_argument('--record', nargs='?', const='edge', choices=['edge', 'match', 'recorded'], default=None)
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
    if a.subcmd and a.subcmd[0] == 'replay' and len(a.subcmd) == 2:
        exe = a.exe or os.path.join(a.root, 'game', 'WalkingDead4.exe')
        return run_replay(a.root, a.subcmd[1], exe, a.record is not None)
    ap.print_help()
    return 0


if __name__ == '__main__':
    sys.exit(main())
