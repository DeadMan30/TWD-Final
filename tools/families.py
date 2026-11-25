#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Function families: many small originals with the same instruction bytes, replaced together.

Tables under symbols/:
- symbols/families.csv               one row per family
- symbols/families/<family>.csv      one row per member (address,size,extent,value,status)
- symbols/families/<family>.verification.csv
- symbols/families/<family>.matching.csv
- symbols/families/<family>.reviews.csv
"""
from __future__ import annotations

import csv
import io
import os
import re
from dataclasses import dataclass

FAMILY_HEADER = ['family', 'system', 'shape', 'returns', 'members', 'bytes', 'notes']
MEMBER_HEADER = ['address', 'size', 'extent', 'value', 'status']
FAMILIES_TABLE = os.path.join('symbols', 'families.csv')
MEMBERS_DIR = os.path.join('symbols', 'families')
HOOK_JUMP = 14
FAMILY_NAME = re.compile(r'^[a-z][a-z0-9]*(_[a-z0-9]+)*$')
FIELD = re.compile(r'\{(u8|d8|u32|d32)\}')
FIELD_WIDTH = {'u8': 1, 'd8': 1, 'u32': 4, 'd32': 4}
DISPLACEMENT_LIMIT = {'d8': 0x80, 'd32': 0x80000000}
REL = re.compile(r'\{rel:0x([0-9a-f]{1,16})(?:\+([1-9]))?\}')
REL_WIDTH = 4


class ShapeError(ValueError):
    pass


@dataclass(frozen=True)
class Shape:
    """A family's byte template: fixed bytes, at most one parameter field, and optional relative fields."""
    text: str
    prefix: bytes
    field: str | None
    suffix: bytes
    rels: tuple[tuple[int, int, int], ...] = ()

    @property
    def size(self) -> int:
        return len(self.prefix) + (FIELD_WIDTH[self.field] if self.field else 0) + len(self.suffix)

    def value(self, code: bytes, address: int | None = None) -> int | None:
        if self.rels:
            self.check_relative(code, address)
        elif len(code) != self.size or not code.startswith(self.prefix) or not code.endswith(self.suffix):
            raise ShapeError(f'{code.hex()} is not {self.text}')
        if self.field is None:
            return None
        raw = code[len(self.prefix):len(self.prefix) + FIELD_WIDTH[self.field]]
        value = int.from_bytes(raw, 'little')
        if self.field in DISPLACEMENT_LIMIT and not 0 < value < DISPLACEMENT_LIMIT[self.field]:
            raise ShapeError(f'{code.hex()}: displacement {value:#x} is not a positive signed {{{self.field}}}')
        return value

    def check_relative(self, code: bytes, address: int | None) -> None:
        if address is None:
            raise ShapeError(f'{self.text[:40]}...: a shape with relative fields matches only at an address')
        if len(code) != self.size:
            raise ShapeError(f'{len(code)} bytes, the shape has {self.size}')
        masked = bytearray(code)
        for offset, target, after in self.rels:
            disp = int.from_bytes(code[offset:offset + REL_WIDTH], 'little', signed=True)
            reached = address + offset + REL_WIDTH + after + disp
            if reached != target:
                raise ShapeError(f'0x{address:08x}+{offset:#x} reaches 0x{reached:08x}, not 0x{target:08x}')
            masked[offset:offset + REL_WIDTH] = bytes(REL_WIDTH)
        if self.field:
            at = len(self.prefix)
            masked[at:at + FIELD_WIDTH[self.field]] = bytes(FIELD_WIDTH[self.field])
        if bytes(masked) != self.prefix + bytes(FIELD_WIDTH[self.field] if self.field else 0) + self.suffix:
            raise ShapeError(f'0x{address:08x}: the bytes outside the relative fields differ')

    def matches(self, code: bytes, address: int | None = None) -> bool:
        try:
            self.value(code, address)
        except ShapeError:
            return False
        return True


def parse_relative_shape(text: str, parts: list[str]) -> Shape:
    code = bytearray()
    rels = []
    field, at = None, None
    for p in parts:
        m = REL.fullmatch(p)
        if m:
            rels.append((len(code), int(m.group(1), 16), int(m.group(2) or 0)))
            code += bytes(REL_WIDTH)
        elif FIELD.fullmatch(p):
            if field:
                raise ShapeError(f'{text[:40]}...: at most one parameter field')
            field, at = FIELD.fullmatch(p).group(1), len(code)
            code += bytes(FIELD_WIDTH[field])
        elif re.fullmatch(r'[0-9a-f]{2}', p):
            code += bytes.fromhex(p)
        else:
            raise ShapeError(f'{text[:40]}...: {p!r} is neither a lowercase hex byte nor a field')
    if field is None:
        return Shape(text, bytes(code), None, b'', tuple(rels))
    return Shape(text, bytes(code[:at]), field, bytes(code[at + FIELD_WIDTH[field]:]), tuple(rels))


def parse_shape(text: str) -> Shape:
    parts = text.split()
    if not parts:
        raise ShapeError('empty shape')
    if any(REL.fullmatch(p) for p in parts):
        return parse_relative_shape(text, parts)
    fields = [i for i, p in enumerate(parts) if FIELD.fullmatch(p)]
    if len(fields) > 1:
        raise ShapeError(f'{text}: at most one parameter field')
    for p in parts:
        if not FIELD.fullmatch(p) and not re.fullmatch(r'[0-9a-f]{2}', p):
            raise ShapeError(f'{text}: {p!r} is neither a lowercase hex byte nor a field')
    if not fields:
        return Shape(text, bytes.fromhex(''.join(parts)), None, b'')
    at = fields[0]
    return Shape(text, bytes.fromhex(''.join(parts[:at])), FIELD.fullmatch(parts[at]).group(1),
                 bytes.fromhex(''.join(parts[at + 1:])))


def value_text(value: int | None) -> str:
    return '' if value is None else f'0x{value:x}'


def member_name(family: dict, address: str) -> str:
    return f"{family['system']}_{family['family']}_{address[2:]}"


def replacement(family: dict, value: str) -> str:
    base = f"twd_{family['system']}_{family['family']}"
    return f'{base}_{value}' if value else base


def members_path(root: str, family: str) -> str:
    return os.path.join(root, MEMBERS_DIR, f'{family}.csv')


def verification_path(root: str, family: str) -> str:
    return os.path.join(root, MEMBERS_DIR, f'{family}.verification.csv')


def read_table(path: str, header: list[str]) -> list[dict]:
    if not os.path.isfile(path):
        return []
    with open(path, newline='', encoding='utf-8') as f:
        rows = list(csv.reader(f))
    if not rows or rows[0] != header:
        raise ValueError(f'{path}: header must be {",".join(header)}')
    out = []
    for n, r in enumerate(rows[1:], 2):
        if len(r) != len(header):
            raise ValueError(f'{path}:{n}: expected {len(header)} columns, got {len(r)}')
        out.append(dict(zip(header, r), _line=n))
    return out


def write_table(path: str, header: list[str], rows: list[dict]) -> None:
    out = io.StringIO()
    w = csv.writer(out, lineterminator='\n')
    w.writerow(header)
    for r in rows:
        w.writerow([r[k] for k in header])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', newline='', encoding='utf-8') as f:
        f.write(out.getvalue())
    os.replace(tmp, path)


def families(root: str) -> list[dict]:
    return read_table(os.path.join(root, FAMILIES_TABLE), FAMILY_HEADER)


def members(root: str, family: str) -> list[dict]:
    return read_table(members_path(root, family), MEMBER_HEADER)


def function_rows(root: str) -> list[dict]:
    out = []
    for fam in families(root):
        for m in members(root, fam['family']):
            out.append({'address': m['address'], 'size': m['size'], 'name': member_name(fam, m['address']),
                        'system': fam['system'], 'status': m['status'], 'notes': fam['notes'],
                        'family': fam['family'], 'extent': m['extent'], 'value': m['value']})
    return out
