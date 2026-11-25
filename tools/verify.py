#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Differential verification harness for TWD-Final replacements.

usage:
  verify.py --self-test               compile and run the x86-64 differential test & mutant check
  verify.py hash <system>             print the SHA-256 source hash for game/<system>/
"""
from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tempfile

DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SELF_TEST_C = r"""
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('subcmd', nargs='*')
    a = ap.parse_args()

    if a.self_test:
        return run_self_test()
    if a.subcmd and a.subcmd[0] == 'hash' and len(a.subcmd) == 2:
        print(system_source_hash(a.root, a.subcmd[1]))
        return 0
    ap.print_help()
    return 0


if __name__ == '__main__':
    sys.exit(main())
