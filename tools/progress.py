#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Progress numbers, derived only from files under symbols/.

Denominator: symbols/ghidra_functions.csv (address,size).
Numerator: rows of symbols/functions.csv and members of the function families
(symbols/families.csv, symbols/families/; tools/families.py) by status.
Each level includes the next:
replaced (hooked and builds) > edge-verified > verified.

usage: progress.py [--root DIR] [--out FILE] [--update-readme] [--check] [--status-line]
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import families as fam  # noqa: E402

DEFAULT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMPLEMENTED = ('replaced', 'edge-verified', 'verified')
EDGE = ('edge-verified', 'verified')
BLOCKS = ('progress', 'target')


def sha256_file(path: str) -> str | None:
    if not os.path.isfile(path):
        return None
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def read_csv(path: str) -> list[dict]:
    if not os.path.isfile(path):
        return []
    with open(path, newline='', encoding='utf-8') as f:
        return list(csv.DictReader(f))


def pct(part: int, whole: int) -> float | None:
    return round(100.0 * part / whole, 2) if whole else None


def compute(root: str) -> dict:
    export = read_csv(os.path.join(root, 'symbols', 'ghidra_functions.csv'))
    funcs = read_csv(os.path.join(root, 'symbols', 'functions.csv'))
    members = fam.function_rows(root)
    funcs = funcs + members
    total_f = len(export)
    total_b = sum(int(r['size']) for r in export)

    def tally(rows: list[dict]) -> dict:
        return {'functions': len(rows), 'bytes': sum(int(r['size']) for r in rows)}

    impl = [r for r in funcs if r['status'] in IMPLEMENTED]
    edge = [r for r in funcs if r['status'] in EDGE]
    ver = [r for r in funcs if r['status'] == 'verified']
    systems: dict[str, dict] = {}
    for r in funcs:
        s = systems.setdefault(r['system'], {
            'tracked': 0, 'replaced': 0, 'edge_verified': 0, 'verified': 0, 'replaced_bytes': 0,
        })
        s['tracked'] += 1
        if r['status'] in IMPLEMENTED:
            s['replaced'] += 1
            s['replaced_bytes'] += int(r['size'])
        if r['status'] in EDGE:
            s['edge_verified'] += 1
        if r['status'] == 'verified':
            s['verified'] += 1
    return {
        'schema': 1,
        'slices': slices(export, funcs),
        'inputs': {
            'ghidra_functions_sha256': sha256_file(os.path.join(root, 'symbols', 'ghidra_functions.csv')),
            'functions_sha256': sha256_file(os.path.join(root, 'symbols', 'functions.csv')),
            'families_sha256': sha256_file(os.path.join(root, 'symbols', 'families.csv')),
        },
        'total': {'functions': total_f, 'bytes': total_b},
        'tracked': tally(funcs),
        'replaced': {**tally(impl), 'functions_pct': pct(len(impl), total_f),
                     'bytes_pct': pct(tally(impl)['bytes'], total_b)},
        'edge_verified': {**tally(edge), 'functions_pct': pct(len(edge), total_f),
                          'bytes_pct': pct(tally(edge)['bytes'], total_b)},
        'verified': {**tally(ver), 'functions_pct': pct(len(ver), total_f),
                     'bytes_pct': pct(tally(ver)['bytes'], total_b)},
        'systems': {k: systems[k] for k in sorted(systems)},
        'families': family_summary(root, members, total_f, total_b),
        'runtime': runtime(root),
        'imports_summary': imports_summary(root),
    }


def family_summary(root: str, members: list[dict], total_f: int, total_b: int) -> dict:
    impl = [m for m in members if m['status'] in IMPLEMENTED]
    edge = [m for m in members if m['status'] in EDGE]
    per = {}
    for f in fam.families(root):
        rows = [m for m in members if m['family'] == f['family']]
        per[f['family']] = {
            'shape': f['shape'], 'system': f['system'], 'members': len(rows),
            'bytes': sum(int(m['size']) for m in rows),
            'edge_verified': sum(m['status'] in EDGE for m in rows),
            'verified': sum(m['status'] == 'verified' for m in rows),
        }
    replaced_bytes = sum(int(m['size']) for m in impl)
    return {
        'count': len(per),
        'replaced': {
            'functions': len(impl), 'bytes': replaced_bytes,
            'functions_pct': pct(len(impl), total_f),
            'bytes_pct': pct(replaced_bytes, total_b),
        },
        'edge_verified': {'functions': len(edge), 'bytes': sum(int(m['size']) for m in edge)},
        'families': per,
    }


def imports_summary(root: str) -> dict | None:
    rows = read_csv(os.path.join(root, 'symbols', 'imports.csv'))
    if not rows:
        return None
    exe_rows = [r for r in rows if r.get('importer') == 'WalkingDead4.exe'] or rows
    rt = sum(1 for r in exe_rows if r.get('provider') == 'runtime')
    mod = sum(1 for r in exe_rows if r.get('provider') and r.get('provider') != 'runtime')
    total = len(exe_rows)
    return {
        'total': total,
        'runtime': rt,
        'module': mod,
        'covered': rt + mod,
        'missing': total - rt - mod,
        'pct': pct(rt + mod, total),
    }


def runtime(root: str) -> dict | None:
    path = os.path.join(root, 'symbols', 'boot.json')
    if not os.path.isfile(path):
        return None
    with open(path, encoding='utf-8') as f:
        b = json.load(f)
    bundled = b.get('imports_bundled', 0)
    covered = bundled + b['imports_implemented']
    unresolved = b.get('imports_unresolved', 0)
    return {
        'imports_total': b['imports_total'], 'imports_implemented': b['imports_implemented'],
        'imports_bundled': bundled, 'imports_covered': covered,
        'imports_unresolved': unresolved,
        'imports_unbound': b['imports_total'] - covered - unresolved,
        'imports_remaining': b['imports_total'] - covered,
        'imports_pct': pct(covered, b['imports_total']),
        'milestone': b['milestone'], 'milestone_index': b['milestone_index'],
        'milestones': len(b['milestones']) - 1, 'first_unimplemented': b.get('first_unimplemented'),
        'stopped_by_hook': b.get('stopped_by_hook'), 'runtime_stop': b.get('runtime_stop'),
        'fault': b.get('fault'), 'outcome': b['outcome'],
    }


SLICES = 64


def slices(export: list[dict], funcs: list[dict]) -> list[dict]:
    if not export:
        return []
    starts = [int(r['address'], 16) for r in export]
    lo = min(starts)
    hi = max(int(r['address'], 16) + int(r['size']) for r in export)
    step = max(1, -(-(hi - lo) // SLICES))
    out = [{'start': f'0x{lo + i * step:09x}', 'end': f'0x{min(hi, lo + (i + 1) * step):09x}',
            'functions': 0, 'bytes': 0, 'replaced_bytes': 0, 'verified_bytes': 0} for i in range(SLICES)]
    for r in export:
        idx = min(SLICES - 1, (int(r['address'], 16) - lo) // step)
        sl = out[idx]
        sl['functions'] += 1
        sl['bytes'] += int(r['size'])
    for r in funcs:
        a = int(r['address'], 16)
        if not lo <= a < hi:
            continue
        idx = min(SLICES - 1, (a - lo) // step)
        sl = out[idx]
        if r['status'] in IMPLEMENTED:
            sl['replaced_bytes'] += int(r['size'])
        if r['status'] == 'verified':
            sl['verified_bytes'] += int(r['size'])
    return out


def fmt_pct(p: float | None) -> str:
    return 'n/a' if p is None else f'{p:.2f}%'


def render_progress(d: dict) -> str:
    if not d['total']['functions']:
        return ('No function table committed yet (`symbols/ghidra_functions.csv`), '
                'so there is no denominator.\n')
    t, r, e, v = d['total'], d['replaced'], d['edge_verified'], d['verified']
    return '\n'.join([
        '| | Functions | Bytes |',
        '|---|---:|---:|',
        f"| Target (`WalkingDead4.exe`) | {t['functions']} | {t['bytes']} |",
        f"| Replaced | {r['functions']} ({fmt_pct(r['functions_pct'])}) | "
        f"{r['bytes']} ({fmt_pct(r['bytes_pct'])}) |",
        *family_row(d['families']),
        f"| Edge-verified | {e['functions']} ({fmt_pct(e['functions_pct'])}) | "
        f"{e['bytes']} ({fmt_pct(e['bytes_pct'])}) |",
        f"| Verified | {v['functions']} ({fmt_pct(v['functions_pct'])}) | "
        f"{v['bytes']} ({fmt_pct(v['bytes_pct'])}) |",
        '',
        'Each row includes the next: verified functions are also edge-verified and replaced.',
        *family_note(d['families']),
        '',
        *render_runtime(d['runtime'], d.get('imports_summary')),
        'Generated by `tools/progress.py --update-readme` from `symbols/`. Do not edit.',
        '',
    ])


def family_row(f: dict) -> list[str]:
    if not f['count']:
        return []
    r = f['replaced']
    families = 'function family' if f['count'] == 1 else 'function families'
    return [f"| of which in {f['count']} {families} | {r['functions']} ({fmt_pct(r['functions_pct'])}) | "
            f"{r['bytes']} ({fmt_pct(r['bytes_pct'])}) |"]


def family_note(f: dict) -> list[str]:
    if not f['count']:
        return []
    return ['Function families are many tiny identical functions that share one '
            'replacement each: they move the function count much more than the byte count.']


def render_runtime(rt: dict | None, imp: dict | None) -> list[str]:
    if not rt:
        if imp:
            return [
                '| Runtime (Android target) | |',
                '|---|---|',
                f"| PE imports mapped | {imp['covered']} of {imp['total']} "
                f"({fmt_pct(imp['pct'])}): {imp['module']} bundled, {imp['runtime']} by the runtime |",
                '| Boot harness | Not wired yet (`symbols/boot.json` pending) |',
                '',
            ]
        return ['Runtime: no boot run recorded yet.', '']
    return [
        '| Runtime (Android target) | |',
        '|---|---|',
        f"| System imports implemented | {rt['imports_covered']} of {rt['imports_total']} "
        f"({fmt_pct(rt['imports_pct'])}): {rt['imports_bundled']} by bundled libraries, "
        f"{rt['imports_implemented']} by the runtime |",
        f"| Other imports | {rt['imports_unresolved']} registered fail-loud diagnostics, "
        f"{rt['imports_unbound']} unbound |",
        f"| Furthest boot milestone | {rt['milestone']} ({rt['milestone_index']} of {rt['milestones']}) |",
        f"| Boot stops at | {boot_stop(rt)} |",
        '',
    ]


def boot_stop(rt: dict) -> str:
    stop = rt['first_unimplemented']
    if stop:
        caller = stop.get('caller')
        by = f", called by {caller}" if caller and caller != 'WalkingDead4.exe' else ''
        return f"`{stop['symbol'] or stop['nid']}` ({stop['library']}{by})"
    hook = rt.get('stopped_by_hook')
    if hook:
        return f"`{hook['name']}` (a stop hook in {hook['module']}, called from `{hook['returns_to']}`)"
    stop = rt.get('runtime_stop')
    if stop:
        return f"`{stop['function']}`, which stops by name ({stop['reason']})"
    if rt['outcome'] == 'timeout':
        return "the run's time limit, still running (no call stopped it)"
    if rt.get('fault'):
        return f"{rt['outcome']} at `{rt['fault']['at']}`"
    return rt['outcome']


def status_sentence(d: dict, link: bool) -> str:
    v, r, t = d['verified'], d['replaced'], d['total']
    text = (f"The game doesn't run on TWD-Final yet: {v['functions']} of {t['functions']} functions are "
            f"verified ({fmt_pct(v['functions_pct'])}) and {r['functions']} replaced ({fmt_pct(r['functions_pct'])}).")
    rt = d.get('runtime')
    if rt:
        runtime_str = '[our native runtime](runtime/)' if link else 'our native runtime'
        text += (f" On {runtime_str} the executable's boot gets as far as {boot_stop(rt)}, with "
                 f"{rt['imports_covered']} of {rt['imports_total']} imports implemented.")
    elif d.get('imports_summary'):
        imp = d['imports_summary']
        text += f" {imp['covered']} of {imp['total']} PE imports are mapped so far."
    return text


def render_status(d: dict) -> str:
    return status_sentence(d, True) + '\n'


def render_target(root: str) -> str:
    path = os.path.join(root, 'target.sha256')
    if not os.path.isfile(path):
        return 'No `target.sha256` yet.\n'
    with open(path, encoding='utf-8') as f:
        lines = [ln.rstrip('\n') for ln in f if ln.strip()]
    return '```\n' + '\n'.join(lines) + '\n```\n'


def block_re(name: str) -> re.Pattern:
    return re.compile(rf'(<!-- {name}:start -->\n)(.*?)(<!-- {name}:end -->)', re.S)


def apply_blocks(text: str, bodies: dict[str, str]) -> str:
    for name, body in bodies.items():
        pat = block_re(name)
        if not pat.search(text):
            raise SystemExit(f'progress: no {name} block')
        text = pat.sub(lambda m: m.group(1) + body + m.group(3), text)
    return text


GENERATED_MARKDOWN = (
    ('STATUS.md', ('progress',)),
    ('CONTRIBUTING.md', ('status',)),
    ('docs/OVERVIEW.md', ('target',)),
)
REQUIRED_BLOCKS = {'STATUS.md': ('progress',), 'docs/OVERVIEW.md': ('target',)}


def block_body(text: str | None, name: str) -> str | None:
    m = block_re(name).search(text) if text is not None else None
    return m.group(2) if m else None


def read_text(path: str) -> str | None:
    if not os.path.isfile(path):
        return None
    with open(path, encoding='utf-8') as f:
        return f.read()


def expected_outputs(root: str, data: dict) -> dict[str, dict[str, str] | str]:
    bodies = {'status': render_status(data), 'progress': render_progress(data), 'target': render_target(root)}
    out: dict[str, dict[str, str] | str] = {}
    for name, blocks in GENERATED_MARKDOWN:
        cur = read_text(os.path.join(root, name))
        if cur is None:
            continue
        for block in REQUIRED_BLOCKS.get(name, ()):
            if block_body(cur, block) is None:
                raise SystemExit(f'progress: {name} has no {block} block')
        present = {b: bodies[b] for b in blocks if block_body(cur, b) is not None}
        if present:
            out[name] = present
    return out


def git_show(repo: str, ref: str, path: str) -> str | None:
    r = subprocess.run(['git', '-C', repo, 'show', f'{ref}:{path}'],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.stdout if r.returncode == 0 else None


def git_repo_for(root: str) -> str:
    r = subprocess.run(['git', '-C', root, 'rev-parse', '--show-toplevel'],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    return root if r.returncode == 0 else os.getcwd()


def default_refs(repo: str) -> list[str]:
    def rev(*args: str) -> str | None:
        r = subprocess.run(['git', '-C', repo, *args],
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None
    main = rev('rev-parse', '-q', '--verify', 'refs/remotes/origin/main^{commit}')
    if not main:
        head = rev('rev-parse', '-q', '--verify', 'HEAD^{commit}')
        return ['HEAD'] if head else []
    base = rev('merge-base', 'HEAD', main)
    return [main] + ([base] if base else [])


def stale_outputs(root: str, data: dict, untouched_refs: list[str] | None = None) -> list[str]:
    repo = git_repo_for(root) if untouched_refs else root
    stale = []
    for name, want in expected_outputs(root, data).items():
        cur = read_text(os.path.join(root, name))
        if isinstance(want, str):
            parts = {None: (want, cur)}
        else:
            parts = {block: (body, block_body(cur, block)) for block, body in want.items()}
        for block, (body, have) in parts.items():
            if have == body:
                continue
            if untouched_refs:
                bases = (git_show(repo, ref, name) for ref in untouched_refs)
                if any((base if block is None else block_body(base, block)) == have
                       for base in bases if base is not None):
                    continue
            stale.append(name if block is None else f'{name} ({block} block)')
    return stale


def write_outputs(root: str, data: dict) -> None:
    for name, want in expected_outputs(root, data).items():
        path = os.path.join(root, name)
        cur = read_text(path)
        new = want if isinstance(want, str) else apply_blocks(cur, want)
        if new == cur:
            continue
        os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(new)
        print(f'progress: {name} updated')


def check_blocks(root: str) -> list[str]:
    errs = []
    for name, blocks in REQUIRED_BLOCKS.items():
        cur = read_text(os.path.join(root, name))
        errs += [f'{name} has no {b} block' for b in blocks if cur is not None and block_body(cur, b) is None]
    return errs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--out')
    ap.add_argument('--update-readme', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--check-touched', nargs='*', metavar='REF')
    ap.add_argument('--check-blocks', action='store_true')
    ap.add_argument('--status-line', action='store_true')
    a = ap.parse_args()

    if a.check_blocks:
        errs = check_blocks(a.root)
        for e in errs:
            print(f'progress: {e}', file=sys.stderr)
        return 1 if errs else 0
    data = compute(a.root)
    if a.status_line:
        print(status_sentence(data, False))
        return 0
    js = json.dumps(data, indent=2, sort_keys=True) + '\n'
    if a.out:
        with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(js)
    if a.update_readme:
        write_outputs(a.root, data)
        return 0
    if a.check_touched is not None:
        refs = a.check_touched or default_refs(git_repo_for(a.root))
        if not refs:
            return 0
        stale = stale_outputs(a.root, data, refs)
        if stale:
            print(f"progress: generated text changed by hand or left stale in {', '.join(stale)}: "
                  "run python3 tools/progress.py --update-readme", file=sys.stderr)
            return 1
        return 0
    if a.check:
        stale = stale_outputs(a.root, data)
        if stale:
            print(f"progress: generated block stale in {', '.join(stale)}; "
                  "run python3 tools/progress.py --update-readme", file=sys.stderr)
            return 1
        return 0
    if not a.out:
        sys.stdout.write(js)
    return 0


if __name__ == '__main__':
    sys.exit(main())
