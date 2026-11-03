#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

"""Locate the filament vendor table of the stock touchscreen GUI.

The stock /usr/bin/gui (stripped aarch64 PIE) keeps its filament vendors in
a static `const char *T[rows][stride / 8]` table: T[v][0] is the vendor name
and T[v][1..] are NULL-terminated "TYPE", "TYPE#Sub" or "Sub*TYPE" entries.
The edit view creates one button per row and keeps the button pointers in a
`rows`-slot array inside a .bss state struct.

This script finds every instruction the libgui-vendors loader must rewrite to
grow the table at runtime and writes them as a descriptor:

    table   - `add Xd, Xn, #lo12(T)`        -> trampoline loads the new table
    buttons - `add Xd, Xn, #off`  (= &btn)  -> trampoline loads the new array
    bound_count  - `cmp Xi, #rows`          -> imm becomes the new row count
    bound_maxidx - `cmp Wv, #rows - 1`      -> imm becomes new row count - 1

Every expectation is checked strictly; anything unexpected exits non-zero
with a message, because a missed site crashes the GUI at runtime.
"""

import argparse
import hashlib
import re
import struct
import sys

try:
    import capstone
except ImportError:
    sys.exit("analyze_gui: python3-capstone is required (apt install python3-capstone)")

# Operand details and register access lists differ between capstone major
# versions (4 rejects every binary, 6 renamed the AArch64 API), and the
# analysis was verified with 5.x only.
if capstone.cs_version()[0] != 5:
    sys.exit('analyze_gui: capstone 5.x is required, found %d.%d' % capstone.cs_version()[:2])
CS_ARCH = capstone.CS_ARCH_ARM64
from capstone import arm64 as _cs  # noqa: E402
OP_REG, OP_IMM, OP_MEM = _cs.ARM64_OP_REG, _cs.ARM64_OP_IMM, _cs.ARM64_OP_MEM

# Expected number of sites per kind for the known GUI layout. A different
# count means the GUI changed and the result must be reviewed by hand; use
# --expect KIND=N only after doing that review.
EXPECT = {
    'table': 6,
    'nozzle': 2,
    'buttons': 2,
    'bound_count': 2,
    'bound_maxidx': 3,
}

# Upper bound on vendor rows the loader may create: the vendor index is an
# int8 and the loader caps rows at 64.
MAX_VENDORS = 32

FIRST_VENDOR = b'Snapmaker'
ENTRY_RE = re.compile(rb'^[A-Za-z0-9][A-Za-z0-9 +\-.#*]*$')
VENDOR_RE = re.compile(rb'^[A-Za-z0-9][A-Za-z0-9 +\-.]*$')

R_AARCH64_RELATIVE = 1027
DT_RELR, DT_RELRSZ = 36, 35

UNCOND = ('b', 'ret', 'br', 'braa', 'brab', 'retaa', 'retab')
CONDBR = ('cbz', 'cbnz', 'tbz', 'tbnz')
CALLER_SAVED = tuple('x%d' % i for i in range(19)) + ('x30',)
ARGS = tuple('x%d' % i for i in range(8))
COMPARES = ('cmp', 'cmn', 'tst', 'ccmp', 'ccmn')


class AnalysisError(Exception):
    pass


def fail(msg):
    raise AnalysisError(msg)


def norm(name):
    if name in ('sp', 'wsp'):
        return 'sp'
    if name in ('xzr', 'wzr'):
        return 'zr'
    if name[0] in 'wx' and name[1:].isdigit():
        return 'x' + name[1:]
    return name


class Insn:
    __slots__ = ('pc', 'mn', 'text', 'ops', 'wb', 'post', 'w', 'r', 'word')

    def __init__(self, pc, mn, text, ops, wb, post, w, r, word):
        self.pc, self.mn, self.text, self.ops = pc, mn, text, ops
        self.wb, self.post, self.w, self.r, self.word = wb, post, w, r, word

    def __str__(self):
        return '%#x: %08x  %s %s' % (self.pc, self.word, self.mn, self.text)

    def mem(self):
        for o in self.ops:
            if o[0] == 'm':
                return o
        return None

    def target(self):
        """Branch target (last immediate operand)."""
        return self.ops[-1][1]

    def is_branch(self):
        return self.mn == 'b' or self.mn.startswith('b.') or self.mn in CONDBR


def decode_one(md, code, pc):
    out = []
    for i in md.disasm(code, pc):
        ops = []
        for o in i.operands:
            if o.type == OP_REG:
                ops.append(('r', norm(i.reg_name(o.reg))))
            elif o.type == OP_IMM:
                ops.append(('i', o.imm, o.shift.type, o.shift.value))
            elif o.type == OP_MEM:
                ops.append(('m', norm(i.reg_name(o.mem.base)),
                            norm(i.reg_name(o.mem.index)) if o.mem.index else None,
                            o.mem.disp, o.shift.value))
            else:
                ops.append(('?',))
        rd, wr = i.regs_access()
        wr = tuple(norm(i.reg_name(x)) for x in wr)
        if i.mnemonic in COMPARES:  # capstone lists the compared register as written
            wr = tuple(x for x in wr if x == 'nzcv')
        out.append(Insn(i.address, i.mnemonic, i.op_str, tuple(ops), bool(i.writeback),
                        bool(getattr(i, 'post_index', False)),
                        wr,
                        tuple(norm(i.reg_name(x)) for x in rd),
                        struct.unpack('<I', i.bytes)[0]))
    return out


class Gui:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = f.read()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        self._caps = {}
        self._parse_elf()
        self._decode_text()
        self._index()

    # ---- ELF ------------------------------------------------------------

    def _parse_elf(self):
        d = self.data
        if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1 or struct.unpack_from('<H', d, 18)[0] != 183:
            fail('%s: not a little-endian ELF64 aarch64 file' % self.path)
        phoff, shoff = struct.unpack_from('<QQ', d, 0x20)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x36)
        self.loads = []
        dynamic = None
        for k in range(phnum):
            p_type, p_flags, off, va, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + k * phentsize)
            if p_type == 1:
                self.loads.append((va, filesz, memsz, off, p_flags))
            elif p_type == 2:
                dynamic = (off, filesz)
        secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + k * shentsize) for k in range(shnum)]
        stroff = secs[shstrndx][4]
        self.secs = {}
        for s in secs:
            name = d[stroff + s[0]:d.index(b'\0', stroff + s[0])].decode()
            self.secs[name] = dict(type=s[1], addr=s[3], off=s[4], size=s[5])
        if '.text' not in self.secs:
            fail('no .text section')
        # R_AARCH64_RELATIVE: slot vaddr -> target vaddr
        self.rel = {}
        for name, s in self.secs.items():
            if s['type'] == 4:  # SHT_RELA
                for o in range(s['off'], s['off'] + s['size'], 24):
                    off, info, add = struct.unpack_from('<QQq', d, o)
                    if info & 0xffffffff == R_AARCH64_RELATIVE:
                        self.rel[off] = add
        if dynamic:
            dyn = {}
            for o in range(dynamic[0], dynamic[0] + dynamic[1], 16):
                tag, val = struct.unpack_from('<qQ', d, o)
                dyn[tag] = val
            if DT_RELR in dyn:  # implicit addends read from the slots
                o = self.v2o(dyn[DT_RELR])
                where = 0
                for k in range(dyn[DT_RELRSZ] // 8):
                    e = struct.unpack_from('<Q', d, o + 8 * k)[0]
                    if not e & 1:
                        self.rel[e] = self.u64(e)
                        where = e + 8
                    else:
                        for bit in range(63):
                            if e >> (bit + 1) & 1:
                                a = where + 8 * bit
                                self.rel[a] = self.u64(a)
                        where += 63 * 8

    def v2o(self, va):
        for lva, filesz, _, off, _ in self.loads:
            if lva <= va < lva + filesz:
                return off + va - lva
        return None

    def read(self, va, n):
        o = self.v2o(va)
        if o is None:
            return None
        return self.data[o:o + n]

    def u64(self, va):
        b = self.read(va, 8)
        return None if b is None or len(b) < 8 else struct.unpack('<Q', b)[0]

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def cstr(self, va, limit=128):
        b = self.read(va, limit)
        if b is None or b'\0' not in b:
            return None
        return b[:b.index(b'\0')]

    def ptr(self, va):
        """Relocated pointer stored at va, or None."""
        return self.rel.get(va)

    # ---- code -------------------------------------------------------------

    def _decode_text(self):
        t = self.secs['.text']
        self.text_lo, self.text_hi = t['addr'], t['addr'] + t['size']
        md = capstone.Cs(CS_ARCH, capstone.CS_MODE_ARM)
        md.detail = True
        self.ins = {}
        pc = self.text_lo
        code = self.data[t['off']:t['off'] + t['size']]
        while pc < self.text_hi:
            for i in decode_one(md, code[pc - self.text_lo:], pc):
                self.ins[i.pc] = i
                pc = i.pc + 4
            if pc < self.text_hi and pc not in self.ins:
                pc += 4  # undecodable word (data in text)

    def _index(self):
        self.preds = {}
        self.entries = set()
        for pc, i in self.ins.items():
            if i.mn == 'bl':
                self.entries.add(i.target())
            elif i.is_branch():
                self.preds.setdefault(i.target(), []).append(pc)
            if i.mn not in UNCOND:
                self.preds.setdefault(pc + 4, []).append(pc)
        self.entry_list = sorted(self.entries)

    def func_range(self, pc):
        """Approximate [start, end) of the function containing pc (bl targets as starts)."""
        import bisect
        k = bisect.bisect_right(self.entry_list, pc)
        lo = self.entry_list[k - 1] if k else self.text_lo
        hi = self.entry_list[k] if k < len(self.entry_list) else self.text_hi
        return lo, hi

    def reaching_defs(self, pc, reg, limit=4000):
        """Instructions that may define `reg` as seen just before `pc`.

        Returns a set of (kind, pc): ('def', pc) for an instruction writing
        reg, ('call', pc) for a bl/blr clobbering a caller-saved reg,
        ('entry', pc) when a function entry or an instruction without
        known predecessors is reached first."""
        out = set()
        seen = set()
        work = [(p, ) for p in self.preds.get(pc, [])]
        if not work or pc in self.entries:
            return {('entry', pc)}
        steps = 0
        while work:
            p, = work.pop()
            if p in seen:
                continue
            seen.add(p)
            steps += 1
            if steps > limit:
                fail('reaching_defs(%#x, %s): search limit exceeded' % (pc, reg))
            i = self.ins.get(p)
            if i is None:
                out.add(('entry', p))
                continue
            if reg in i.w:
                out.add(('def', p))
                continue
            if i.mn in ('bl', 'blr') and reg in CALLER_SAVED:
                out.add(('call', p))
                continue
            if p in self.entries:
                out.add(('entry', p))
                continue
            pp = self.preds.get(p, [])
            if not pp:
                out.add(('entry', p))
            for q in pp:
                work.append((q, ))
        return out

    def const_value(self, pc, reg, depth=0):
        """Set of constant values reg can hold just before pc (None = non-constant)."""
        if depth > 8:
            return {None}
        vals = set()
        for kind, p in self.reaching_defs(pc, reg):
            if kind != 'def':
                vals.add(None)
                continue
            i = self.ins[p]
            if i.mn == 'adrp':
                vals.add(i.ops[1][1])
            elif i.mn in ('add', 'sub') and len(i.ops) == 3 and i.ops[2][0] == 'i' and i.ops[1][0] == 'r':
                imm = i.ops[2][1] << (i.ops[2][3] if i.ops[2][2] else 0)
                for v in self.const_value(p, i.ops[1][1], depth + 1):
                    vals.add(None if v is None else v + (imm if i.mn == 'add' else -imm))
            elif i.mn == 'mov' and len(i.ops) == 2 and i.ops[1][0] == 'r' and i.ops[1][1] != 'zr':
                vals |= self.const_value(p, i.ops[1][1], depth + 1)
            else:
                vals.add(None)
        return vals

    def loop_of(self, pc):
        """Innermost backward-branch loop [head, latch] containing pc."""
        best = None
        for p in range(pc, min(pc + 0x400, self.text_hi), 4):
            i = self.ins.get(p)
            if i and i.is_branch() and i.target() <= pc:
                if best is None or i.target() > best[0]:
                    best = (i.target(), p)
        return best

    def loop_cap(self, pc, reg):
        """(lo, hi) bounding the values `reg` takes at pc inside a counted
        loop, or None. The loop must have a single entry and a single back
        edge `cmp Wk, #n; b.cc head`, k must start from constants and step by
        a constant on every iteration, and reg may only be advanced by
        non-negative constants in the body."""
        key = (pc, reg)
        if key not in self._caps:
            self._caps[key] = self._loop_cap(pc, reg)
        return self._caps[key]

    def _loop_cap(self, pc, reg):
        loop = self.loop_of(pc)
        if loop is None:
            return None
        head, latch = loop
        body = range(head, latch + 4, 4)
        lb, c = self.ins[latch], self.ins.get(latch - 4)
        if not lb.mn.startswith('b.') or c is None or not is_cmp_imm(c):
            return None
        for p in body:
            for q in self.preds.get(p, []):
                if (q not in body and p != head) or (p == head and q in body and q != latch):
                    return None
        k, limit, cond = c.ops[0][1], cmp_imm(c.word), lb.mn[2:]
        if (k in CALLER_SAVED or reg in CALLER_SAVED) and any(self.ins[p].mn in ('bl', 'blr') for p in body):
            return None  # a call may change them
        kw = [p for p in body if k in self.ins[p].w]
        if len(kw) != 1:
            return None
        inc = self.ins[kw[0]]
        if inc.mn != 'add' or len(inc.ops) != 3 or inc.ops[1] != ('r', k) or inc.ops[2][0] != 'i' or inc.ops[2][3]:
            return None
        step = inc.ops[2][1]
        for p in range(head, kw[0], 4):  # nothing may skip the increment
            i = self.ins[p]
            if i.is_branch() and kw[0] < i.target() <= latch:
                return None
        delta = 0
        for p in body:
            i = self.ins[p]
            if reg not in i.w:
                continue
            m = i.mem()
            if i.mn == 'add' and len(i.ops) == 3 and i.ops[1] == ('r', reg) and i.ops[2][0] == 'i' and not i.ops[2][3]:
                delta += i.ops[2][1]
            elif m and m[1] == reg and i.wb and reg not in [o[1] for o in i.ops[:-1] if o[0] == 'r']:
                d = i.ops[-1][1] if i.post else m[3]
                if d < 0:
                    return None
                delta += d
            else:
                return None
        inits, starts = set(), set()
        for q in self.preds[head]:
            if q in body:
                continue
            for r, out in ((k, inits), (reg, starts)):
                if r in self.ins[q].w:
                    vals = self.value_def(q, r)
                else:
                    vals = self.const_value(q, r) if r == reg else self.mov_imm_value(q, r)
                if None in vals:
                    return None
                out |= vals
        if not inits or not starts or step <= 0:
            return None
        trips = 0
        for k0 in inits:
            if cond == 'ne':
                if k0 >= limit or (limit - k0) % step:
                    return None  # would wrap around
                n = (limit - k0) // step
            elif cond in ('lt', 'lo', 'cc'):
                n = max(1, -(-(limit - k0) // step))
            elif cond in ('le', 'ls'):
                n = max(1, (limit - k0) // step + 1)
            else:
                return None
            trips = max(trips, n + 1)  # +1: k may be compared before or after the step
        return min(starts), max(starts) + trips * delta

    def is_callee_save(self, i, reg):
        """`str/stp reg, [sp, #off]` of a callee-saved register whose stack
        slot the function only ever reloads into the same register."""
        m = i.mem()
        if i.mn not in ('str', 'stp') or not m or m[1] != 'sp' or m[2] or i.wb or \
                reg not in tuple('x%d' % n for n in range(19, 30)):
            return False
        regs = [o[1] for o in i.ops if o[0] == 'r']
        if regs.count(reg) != 1 or any(not o.startswith('x') for o in regs):
            return False
        slot = m[3] + 8 * regs.index(reg)
        lo, hi = self.func_range(i.pc)
        for pc in range(lo, hi, 4):
            j = self.ins.get(pc)
            jm = j.mem() if j else None
            if not jm or jm[1] != 'sp' or jm[2] or j.mn not in ('ldr', 'ldp', 'str', 'stp'):
                continue
            off = 0 if j.post else jm[3]
            for k, o in enumerate(x for x in j.ops if x[0] == 'r'):
                if off + 8 * k == slot and o[1] != reg:
                    return False
        return True

    def mov_imm_value(self, pc, reg):
        """Constants reg can hold just before pc, from `mov reg, #imm` only."""
        vals = set()
        for kind, p in self.reaching_defs(pc, reg):
            vals |= self.value_def(p, reg) if kind == 'def' else {None}
        return vals

    def value_def(self, p, reg):
        i = self.ins[p]
        if i.mn in ('mov', 'movz') and len(i.ops) == 2 and i.ops[1][0] == 'i' and not i.ops[1][3]:
            return {i.ops[1][1]}
        if reg in i.w:
            return self.const_value(p + 4, reg) if (p + 4) in self.ins and self.preds.get(p + 4) == [p] else {None}
        return {None}

    def def_chain(self, pc, reg, depth=0):
        """Follow simple register moves/extensions back to their origins.

        Returns a set of origin instructions (or ('entry'|'call', pc))."""
        if depth > 8:
            return {('deep', pc)}
        out = set()
        for kind, p in self.reaching_defs(pc, reg):
            if kind != 'def':
                out.add((kind, p))
                continue
            i = self.ins[p]
            passthru = (i.mn in ('mov', 'sxtb', 'uxtb', 'sxtw', 'sxth', 'uxth') and len(i.ops) == 2
                        and i.ops[1][0] == 'r' and i.ops[1][1] != 'zr') or \
                       (i.mn == 'and' and len(i.ops) == 3 and i.ops[2][0] == 'i' and i.ops[2][1] in (0xff, 0xffff))
            if passthru:
                out |= self.def_chain(p, i.ops[1][1], depth + 1)
            else:
                out.add(('def', p))
        return out


# ---- forward value tracking ----------------------------------------------

class Wide:
    """A register that may hold one of several region values at a join
    point: a small set of values, or once that grows past WIDE_MAX (a pointer
    advanced by a loop) a range whose lo/hi is None where it kept growing."""
    __slots__ = ('vals', 'lo', 'hi')

    def __init__(self, vals=None, lo=None, hi=None):
        self.vals = frozenset(vals) if vals is not None else None
        if self.vals is not None:
            lo, hi = min(self.vals), max(self.vals)
        self.lo, self.hi = lo, hi

    def __eq__(self, other):
        return isinstance(other, Wide) and (self.vals, self.lo, self.hi) == (other.vals, other.lo, other.hi)

    def __hash__(self):
        return hash((self.vals, self.lo, self.hi))

    def __repr__(self):
        if self.vals is not None:
            return '{%s}' % ', '.join(map(hex, sorted(self.vals)))
        return '[%s..%s]' % ('-inf' if self.lo is None else hex(self.lo), 'inf' if self.hi is None else hex(self.hi))

    def shift(self, d):
        if self.vals is not None:
            return Wide({v + d for v in self.vals})
        return Wide(lo=None if self.lo is None else self.lo + d, hi=None if self.hi is None else self.hi + d)

    def clamp(self, lo, hi):
        """Values within [lo, hi], e.g. a loop's provable range."""
        if self.vals is not None:
            vals = {v for v in self.vals if lo <= v <= hi}
            return Wide(vals) if vals else self
        return Wide(lo=lo if self.lo is None else max(lo, self.lo), hi=hi if self.hi is None else min(hi, self.hi))

    def overlaps(self, lo, hi):
        if self.vals is not None:
            return any(lo <= v < hi for v in self.vals)
        return (self.lo is None or self.lo < hi) and (self.hi is None or self.hi >= lo)

    def start_in(self, lo, hi):
        """A widened walk that starts inside [lo, hi): it was derived from an
        in-range value, whose computation is reported on its own."""
        if self.vals is not None:
            return lo <= self.lo < hi
        return (self.lo is not None and lo <= self.lo < hi) or \
               (self.lo is None and self.hi is not None and lo <= self.hi < hi)


WIDE_MAX = 16


def join_value(old, new):
    if old == new:
        return old
    o = old if isinstance(old, Wide) else Wide({old})
    n = new if isinstance(new, Wide) else Wide({new})
    if o.vals is not None and n.vals is not None and len(o.vals | n.vals) <= WIDE_MAX:
        return Wide(o.vals | n.vals)
    lo = o.lo if o.lo is not None and n.lo is not None and n.lo >= o.lo else None
    hi = o.hi if o.hi is not None and n.hi is not None and n.hi <= o.hi else None
    return Wide(lo=lo, hi=hi)


def join_state(old, new):
    """Registers absent from a state hold no region value; the join keeps
    every region value either side may hold."""
    out = dict(old)
    for r, v in new.items():
        out[r] = join_value(out[r], v) if r in out else v
    return out


def track(g, start_pc, init, interesting, max_steps=200000):
    """Propagate region-pointer register values forward from start_pc.

    Follows branches (both ways for conditional ones), stops at ret/br and
    tail calls, and keeps callee-saved registers across calls. States are joined per pc and
    values that change around a loop are widened (Wide), so this always
    terminates; hitting max_steps anyway is an error, never a silent cut-off.
    Returns events:
      ('add', pc, src_value, value, rd)
      ('mem', pc, ea, insn)            immediate-offset access
      ('idx', pc, base, insn)          register-offset access
      ('use', pc, {reg: value}, insn)  any other read of a tracked register
      ('call', pc, {reg: value}, insn) tracked value in an argument register
      ('wide', pc, {reg: Wide}, insn)  any read of a widened register
    """
    ev = []
    work = [(start_pc, dict(init))]
    state_at = {}
    steps = 0

    def keep(st, reg, v):
        if isinstance(v, Wide) and v.overlaps(*interesting) or not isinstance(v, Wide) and \
                interesting[0] <= v < interesting[1]:
            st[reg] = v
        else:
            st.pop(reg, None)

    while work:
        pc, st = work.pop()
        while st:
            steps += 1
            if steps > max_steps:
                fail('track(%#x): step limit exceeded' % start_pc)
            old = state_at.get(pc)
            if old is not None:
                st = join_state(old, st)
                for r, v in st.items():
                    if isinstance(v, Wide):
                        cap = g.loop_cap(pc, r)
                        if cap is not None:
                            st[r] = v.clamp(*cap)
                if st == old:
                    break
            state_at[pc] = st
            st = dict(st)
            i = g.ins.get(pc)
            if i is None:
                break
            wide = {x: st[x] for x in i.r if isinstance(st.get(x), Wide)}
            used = {x: st[x] for x in i.r if x in st and x not in wide}
            if wide and not (i.mn == 'mov' and len(i.ops) == 2 and i.ops[1][0] == 'r'):
                ev.append(('wide', pc, wide, i))
            if i.mn in ('add', 'sub') and len(i.ops) == 3 and i.ops[2][0] == 'i' and i.ops[1][0] == 'r' \
                    and i.ops[1][1] in st:
                imm = i.ops[2][1] << (i.ops[2][3] if i.ops[2][2] else 0)
                src = st[i.ops[1][1]]
                d = imm if i.mn == 'add' else -imm
                if isinstance(src, Wide):
                    keep(st, i.ops[0][1], src.shift(d))
                else:
                    ev.append(('add', pc, src, src + d, i.ops[0][1]))
                    keep(st, i.ops[0][1], src + d)
                pc += 4
                continue
            if i.mn == 'mov' and len(i.ops) == 2 and i.ops[1][0] == 'r' and i.ops[1][1] in st:
                st[i.ops[0][1]] = st[i.ops[1][1]]
                pc += 4
                continue
            m = i.mem()
            if m is not None and m[1] in st:
                base = st[m[1]]
                if not isinstance(base, Wide):
                    if m[2]:
                        ev.append(('idx', pc, base, i))
                    else:
                        ev.append(('mem', pc, base if i.post else base + m[3], i))
                other = {k: v for k, v in used.items() if k != m[1]}
                if other:
                    ev.append(('use', pc, other, i))
                for x in i.w:
                    st.pop(x, None)
                if i.wb:
                    d = i.ops[-1][1] if i.post else m[3]
                    keep(st, m[1], base.shift(d) if isinstance(base, Wide) else base + d)
                pc += 4
                continue
            tail = i.mn == 'b' and i.target() in g.entries and not g.func_range(pc)[0] <= i.target() <= pc
            if i.mn in ('bl', 'blr') or tail:
                args = {x: st[x] for x in ARGS if x in st and not isinstance(st[x], Wide)}
                if args:
                    ev.append(('call', pc, args, i))
                wargs = {x: st[x] for x in ARGS if isinstance(st.get(x), Wide)}
                if wargs:
                    ev.append(('wide', pc, wargs, i))
                if tail:  # the callee only saves/restores callee-saved registers
                    break
                for x in CALLER_SAVED:
                    st.pop(x, None)
                pc += 4
                continue
            if used:
                ev.append(('use', pc, used, i))
            if i.mn == 'b':
                pc = i.target()
                continue
            if i.mn.startswith('b.') or i.mn in CONDBR:
                work.append((i.target(), dict(st)))
                pc += 4
                continue
            if i.mn in UNCOND:
                break
            for x in i.w:
                st.pop(x, None)
            pc += 4
    return ev


def scan_region_refs(g, lo, hi, window=0x10000):
    """All ways code computes an address in [lo, hi) without going through
    an already-in-range value: returns list of (pc, value, insn, how)."""
    out = []
    page_lo = (lo & ~0xfff) - window
    page_hi = (hi + 0xfff) & ~0xfff
    inter = (page_lo, page_hi + 0x1000)
    seen = set()
    for pc, i in g.ins.items():
        if i.mn == 'adr' and lo <= i.ops[1][1] < hi:
            out.append((pc, i.ops[1][1], i, 'adr'))
        elif i.mn.startswith('ldr') and len(i.ops) == 2 and i.ops[1][0] == 'i' and lo <= i.ops[1][1] < hi:
            out.append((pc, i.ops[1][1], i, 'literal'))
        elif i.mn == 'adrp' and page_lo <= i.ops[1][1] < page_hi:
            for e in track(g, pc + 4, {i.ops[0][1]: i.ops[1][1]}, inter):
                if e[0] == 'add' and not lo <= e[2] < hi and lo <= e[3] < hi:
                    k = (e[1], e[3], 'add')
                elif e[0] == 'mem' and lo <= e[2] < hi and not lo <= e[2] - (0 if e[3].post else e[3].mem()[3]) < hi:
                    k = (e[1], e[2], 'mem')
                elif e[0] == 'wide' and any(w.overlaps(lo, hi) and not w.start_in(lo, hi) for w in e[2].values()):
                    fail('%s: loop-carried pointer %s may reach %#x..%#x'
                         % (e[3], ', '.join('%s=%r' % kv for kv in e[2].items()), lo, hi))
                else:
                    continue
                if k not in seen:
                    seen.add(k)
                    out.append((k[0], k[1], g.ins[k[0]], k[2]))
    return sorted(out, key=lambda x: x[0])


# ---- analysis -------------------------------------------------------------

def is_add_imm64(i):
    """`add Xd, Xn, #imm12` (64-bit, unshifted, no sp)."""
    w = i.word
    return (w & 0xffc00000) == 0x91000000 and (w & 0x1f) != 31 and (w >> 5 & 0x1f) != 31


def is_cmp_imm(i):
    """`cmp Rn, #imm12` (subs zr, unshifted), 32 or 64 bit."""
    return (i.word & 0x7fc0001f) == 0x7100001f


def cmp_imm(word):
    return word >> 10 & 0xfff


class Analysis:
    def __init__(self, g, expect):
        self.g = g
        self.expect = expect
        self.notes = []

    def run(self):
        g = self.g
        self.find_table()
        self.find_table_refs()
        self.derive_stride()
        self.find_buttons_and_count_bounds()
        self.derive_rows()
        self.classify_table_refs()
        self.check_region('table', self.T, self.T + self.rows * self.stride, [s for s, _ in self.table_sites])
        self.check_region('button array', self.B, self.B + self.rows * 8, [s for s, _ in self.button_sites])
        self.find_vendor_index()
        self.find_maxidx_bounds()
        self.derive_max_entries()
        self.check_counts()
        return self

    # table ------------------------------------------------------------------

    def find_table(self):
        g = self.g
        cands = []
        for slot, tgt in g.rel.items():
            if g.cstr(tgt) != FIRST_VENDOR:
                continue
            nxt = g.ptr(slot + 8)
            s = g.cstr(nxt) if nxt is not None else None
            if s and ENTRY_RE.match(s) and s != FIRST_VENDOR:
                cands.append(slot)
        if len(cands) != 1:
            fail('expected exactly one %r vendor row followed by an entry string, found %d: %s'
                 % (FIRST_VENDOR.decode(), len(cands), ', '.join('%#x' % c for c in cands)))
        self.T = cands[0]
        sec = [n for n, s in g.secs.items() if s['addr'] <= self.T < s['addr'] + s['size'] and s['addr']]
        if sec != ['.data.rel.ro']:
            fail('vendor table %#x is in %s, expected .data.rel.ro' % (self.T, sec))

    def find_table_refs(self):
        """Every `add Xd, Xn, #lo12(T)` whose Xn provably holds page(T)."""
        g = self.g
        page, lo12 = self.T & ~0xfff, self.T & 0xfff
        self.table_refs = []
        for pc, i in sorted(g.ins.items()):
            if i.mn != 'add' or len(i.ops) != 3 or i.ops[2][0] != 'i' or i.ops[2][1] != lo12 or i.ops[2][3]:
                continue
            vals = g.const_value(pc, i.ops[1][1])
            if page not in vals:
                continue
            if vals != {page}:
                fail('%s: base register may hold %s, expected only %#x'
                     % (i, ', '.join('?' if v is None else hex(v) for v in vals), page))
            if not is_add_imm64(i):
                fail('%s: table reference is not `add Xd, Xn, #imm` (64-bit)' % i)
            self.table_refs.append(i)
        if not self.table_refs:
            fail('no references to the vendor table %#x' % self.T)

    def ref_events(self, i):
        """Forward events of the value produced by ref site i."""
        T = self.T
        return track(self.g, i.pc + 4, {i.ops[0][1]: T}, (T - 0x1000, T + 0x2000))

    def derive_stride(self):
        g = self.g
        T = self.T
        cands = {}
        for ref in self.table_refs:
            for e in self.ref_events(ref):
                if e[0] == 'mem' and e[3].post and e[3].wb and e[2] == T:
                    cands.setdefault(e[3].ops[-1][1], set()).add(e[1])
            # row offset constants: `mov Xk, #c` feeding mul/madd in this function
            lo, hi = g.func_range(ref.pc)
            for pc in range(lo, hi, 4):
                i = g.ins.get(pc)
                if i is None or i.mn not in ('mul', 'madd') or abs(pc - ref.pc) > 0x40:
                    continue
                for o in i.ops[1:3]:
                    for kind, p in g.reaching_defs(pc, o[1]):
                        d = g.ins.get(p) if kind == 'def' else None
                        if d and d.mn == 'mov' and d.ops[1][0] == 'i' and d.ops[1][1] >= 0x40 and d.ops[1][1] % 8 == 0:
                            cands.setdefault(d.ops[1][1], set()).add(pc)
        if len(cands) != 1:
            fail('cannot derive the row stride: candidates %s'
                 % {hex(k): sorted(hex(x) for x in v) for k, v in cands.items()})
        self.stride = next(iter(cands))
        self.stride_evidence = sorted(next(iter(cands.values())))

    def loop_of(self, pc):
        return self.g.loop_of(pc)

    def loop_bound(self, loop, ireg):
        """`cmp ireg, #imm; b.ne|lo|lt head` latching a loop that counts ireg
        up by one from zero, so the loop runs exactly #imm times."""
        g = self.g
        head, end = loop
        latch = g.ins[end]
        c = g.ins.get(end - 4)
        if latch.mn not in ('b.ne', 'b.lo', 'b.lt', 'b.cc') or latch.target() != head or c is None or \
                not is_cmp_imm(c) or c.ops[0][1] != ireg:
            fail('loop %#x..%#x: expected `cmp %s, #imm; b.ne|lo|lt %#x` latch, found %s / %s'
                 % (head, end, ireg, head, c, latch))
        writes = [g.ins[p] for p in range(head, end, 4) if ireg in g.ins[p].w]
        if len(writes) != 1 or writes[0].mn != 'add' or writes[0].ops[1] != ('r', ireg) or \
                writes[0].ops[2][:2] != ('i', 1) or writes[0].ops[2][3]:
            fail('loop %#x..%#x: %s is not counted up by one (%s)' % (head, end, ireg, '; '.join(map(str, writes))))
        for p in range(head, writes[0].pc, 4):
            i = g.ins[p]
            if i.is_branch() and writes[0].pc < i.target() <= end:
                fail('%s: skips the increment of %s in loop %#x..%#x' % (i, ireg, head, end))
        for q in g.preds[head]:
            if head <= q <= end:
                continue
            if (g.value_def(q, ireg) if ireg in g.ins[q].w else g.mov_imm_value(q, ireg)) != {0}:
                fail('loop %#x..%#x: %s does not start at 0 (entry %#x)' % (head, end, ireg, q))
        return c

    def find_buttons_and_count_bounds(self):
        """The row-walk loop (post-index load by stride) stores each button
        pointer to `[Xb, Xi, lsl #3]`; Xb is the button array."""
        g = self.g
        walks = []
        for ref in self.table_refs:
            for e in self.ref_events(ref):
                if e[0] == 'mem' and e[3].post and e[3].ops[-1][1] == self.stride:
                    walks.append(e[3])
        walks = sorted({w.pc: w for w in walks}.values(), key=lambda w: w.pc)
        if len(walks) != 1:
            fail('expected one row-walk load (post-index #%#x), found: %s'
                 % (self.stride, '; '.join(map(str, walks))))
        loop = self.loop_of(walks[0].pc)
        if loop is None:
            fail('%s: row-walk load is not inside a loop' % walks[0])
        stores = []
        for pc in range(loop[0], loop[1], 4):
            i = g.ins[pc]
            m = i.mem()
            if i.mn == 'str' and m and m[2] and m[4] == 3:
                vals = g.const_value(pc, m[1])
                if None not in vals and len(vals) == 1:
                    stores.append((i, next(iter(vals))))
        if len(stores) != 1:
            fail('row-walk loop %#x..%#x: expected one `str Xt, [Xb, Xi, lsl #3]` to a constant array, found %d'
                 % (loop[0], loop[1], len(stores)))
        st, self.B = stores[0]
        self.walk_loop = loop
        # sources of the button array pointer: `add Xd, Xn, #imm` == B
        self.button_sites = []
        self.count_bounds = {}
        # slot 0 only: the row count is not known yet, check_region() later
        # rejects anything else inside the array
        for pc, v, i, how in scan_region_refs(self.g, self.B, self.B + 8):
            if how != 'add':
                fail('%s: unsupported %s access to the vendor button array %#x' % (i, how, self.B))
            if not is_add_imm64(i):
                fail('%s: button array reference is not `add Xd, Xn, #imm`' % i)
            vals = g.const_value(i.pc, i.ops[1][1])
            if None in vals or len(vals) != 1:
                fail('%s: base register is not a single constant (%s)' % (i, vals))
            self.button_sites.append((i, None))
        for i, _ in self.button_sites:
            idx = [e for e in track(g, i.pc + 4, {i.ops[0][1]: self.B},
                                    (self.B - 0x1000, self.B + 0x1000))]
            # the register now holds the new array: the only allowed uses are
            # scaled, loop-bounded slot accesses and callee-saved spills
            for e in idx:
                if e[0] == 'use' and all(g.is_callee_save(e[3], r) for r in e[2]):
                    continue
                if e[0] != 'idx' or e[2] != self.B:
                    fail('%s: unsupported use of the button array pointer from %s (%s)' % (e[-1], i, e[0]))
                m = e[3].mem()
                if m[4] != 3:
                    fail('%s: button array access is not scaled by 8' % e[3])
                loop = self.loop_of(e[1])
                if loop is None:
                    fail('%s: button array access outside a loop' % e[3])
                c = self.loop_bound(loop, m[2])
                self.count_bounds[c.pc] = c

    def derive_rows(self):
        counts = {cmp_imm(c.word) for c in self.count_bounds.values()}
        if len(counts) != 1:
            fail('row count loops disagree: %s' % ', '.join(map(str, self.count_bounds.values())))
        self.rows = counts.pop()
        g = self.g
        self.vendors = []
        self.entries = []
        for v in range(self.rows):
            row = self.T + v * self.stride
            name = g.cstr(g.ptr(row)) if g.ptr(row) is not None else None
            if not name or not VENDOR_RE.match(name):
                fail('row %d at %#x: slot 0 is not a vendor name (%r)' % (v, row, name))
            ents = []
            for k in range(1, self.stride // 8):
                p = g.ptr(row + 8 * k)
                if p is None:
                    if g.u64(row + 8 * k) != 0:
                        fail('row %d slot %d: neither NULL nor a relocated pointer' % (v, k))
                    break
                s = g.cstr(p)
                if not s or not ENTRY_RE.match(s):
                    fail('row %d slot %d: %r is not a filament entry' % (v, k, s))
                ents.append(s.decode())
            self.vendors.append(name.decode())
            self.entries.append(ents)
        if self.vendors[0] != FIRST_VENDOR.decode():
            fail('first row is %r' % self.vendors[0])

    def classify_table_refs(self):
        """Refs whose value is only used past the table end (`add #off` or a
        load at offset >= table size) address the next (nozzle) table: those
        must keep the stock address. Refs used inside the table are patched.
        Anything below the table, or a ref used both ways, is an error."""
        size = self.rows * self.stride
        T = self.T
        end = T + size
        self.table_sites, self.nozzle_sites = [], []
        for ref in self.table_refs:
            inside, beyond = [], []

            def place(e, v):
                if v < T:
                    fail('%s: table ref %s reaches %#x, below the vendor table' % (e[-1], ref, v))
                (beyond if v >= end else inside).append(e)

            for e in self.ref_events(ref):
                if e[0] == 'add':
                    if e[2] == T:
                        (beyond if e[3] >= end else inside).append(e)
                elif e[0] in ('mem', 'idx'):
                    place(e, e[2])
                elif e[0] in ('use', 'call'):
                    for v in e[2].values():
                        place(e, v)
                elif e[0] == 'wide':
                    for w in e[2].values():
                        if w.lo is None or w.lo < T:
                            fail('%s: loop-carried table pointer %r from %s' % (e[3], w, ref))
                        (beyond if w.lo >= end else inside).append(e)
            if inside and beyond:
                fail('%s: value used both inside the vendor table and beyond it' % ref)
            if beyond:
                offs = {e[3] - T for e in beyond if e[0] == 'add'} | \
                       {e[2] - T for e in beyond if e[0] in ('mem', 'idx')}
                self.nozzle_sites.append((ref, sorted(offs)))
            elif inside:
                self.table_sites.append((ref, None))
            else:
                fail('%s: cannot tell how the table address is used' % ref)

    def check_region(self, what, lo, hi, sites):
        """No code may compute an address in [lo, hi) except the known sites."""
        known = {i.pc for i in sites} | {i.pc for i, _ in getattr(self, 'nozzle_sites', [])}
        for pc, v, i, how in scan_region_refs(self.g, lo, hi):
            if pc in known and v == lo:
                continue
            fail('%s: unexpected %s reference to the %s (%#x, offset %#x)' % (i, how, what, v, v - lo))
        for slot, tgt in self.g.rel.items():
            if lo <= tgt < hi:
                fail('data pointer at %#x points into the %s (%#x)' % (slot, what, tgt))

    # vendor index ------------------------------------------------------------

    def find_vendor_index(self):
        """The int8 holding the selected vendor: loads feeding `mul ..., #stride`."""
        g = self.g
        V = set()
        for ref, _ in self.table_sites:
            lo, hi = g.func_range(ref.pc)
            for pc in range(max(lo, ref.pc - 0x40), min(hi, ref.pc + 0x40), 4):
                i = g.ins.get(pc)
                if i is None or i.mn not in ('mul', 'madd'):
                    continue
                for o in i.ops[1:3]:
                    for kind, p in g.def_chain(pc, o[1]):
                        d = g.ins.get(p) if kind == 'def' else None
                        if d and d.mn in ('ldrb', 'ldrsb') and d.mem() and not d.mem()[2]:
                            for b in g.const_value(p, d.mem()[1]):
                                if b is not None:
                                    V.add(b + d.mem()[3])
        if len(V) != 1:
            fail('cannot identify the vendor index variable: %s' % sorted(map(hex, V)))
        self.V = V.pop()

    def v_load(self, pc):
        i = self.g.ins.get(pc)
        if i is None or i.mn not in ('ldrb', 'ldrsb') or not i.mem() or i.mem()[2]:
            return False
        return {b + i.mem()[3] for b in self.g.const_value(pc, i.mem()[1]) if b is not None} == {self.V}

    def v_stores(self, lo, hi):
        """Registers stored with `strb Wr, [V]` in [lo, hi)."""
        out = set()
        for pc in range(lo, hi, 4):
            i = self.g.ins.get(pc)
            if i and i.mn == 'strb' and i.mem() and not i.mem()[2] and \
                    {b + i.mem()[3] for b in self.g.const_value(pc, i.mem()[1]) if b is not None} == {self.V}:
                out.add((pc, i.ops[0][1]))
        return out

    def find_maxidx_bounds(self):
        """Every `cmp R, #rows-1|#rows` in the functions using the table, the
        button array or V must be explained."""
        g = self.g
        funcs = set()
        for i, _ in self.table_sites + self.button_sites + [(x, 0) for x, _ in self.nozzle_sites]:
            funcs.add(g.func_range(i.pc))
        for pc, v, i, how in scan_region_refs(g, self.V, self.V + 1):
            funcs.add(g.func_range(pc))
        self.funcs = sorted(funcs)
        self.maxidx_bounds = {}
        self.unrelated = []
        for lo, hi in self.funcs:
            stores = self.v_stores(lo, hi)
            for pc in range(lo, hi, 4):
                i = g.ins.get(pc)
                if i is None or i.mn not in ('cmp', 'cmn', 'ccmp', 'ccmn', 'subs'):
                    continue
                imms = [o[1] for o in i.ops if o[0] == 'i']
                if not imms or imms[0] not in (self.rows - 1, self.rows):
                    continue
                if not is_cmp_imm(i):
                    fail('%s: compare with the row count in an unsupported form' % i)
                if pc in self.count_bounds:
                    continue
                reg = i.ops[0][1]
                origins = g.def_chain(pc, reg)
                from_v = all(k == 'def' and self.v_load(p) for k, p in origins)
                from_arg = all(k == 'entry' for k, p in origins) and any(r == reg for _, r in stores)
                from_call = all(k == 'call' for k, p in origins) and reg == 'x0'
                if (from_v or from_arg) and cmp_imm(i.word) == self.rows - 1:
                    nx = g.ins.get(pc + 4)
                    cond = nx.mn[2:] if nx and nx.mn.startswith('b.') else \
                        nx.text.split(',')[-1].strip() if nx and nx.mn in ('csel', 'csinc', 'cset', 'cinc') else None
                    if cond not in ('gt', 'hi', 'le', 'ls'):
                        fail('%s: vendor index compared with %d but not as an upper bound (next: %s)'
                             % (i, self.rows - 1, nx))
                    self.maxidx_bounds[pc] = i
                elif from_call:
                    self.unrelated.append(i)
                else:
                    fail('%s: unexplained compare with the row count (operand from %s)'
                         % (i, ', '.join('%s@%#x' % o for o in sorted(origins))))
        if not self.maxidx_bounds:
            fail('no vendor index bound checks found')

    def derive_max_entries(self):
        """The type-button loop walks a row with `ldr Xe, [Xrow, Xk, lsl #3]`
        and stops at `cmp Xk, #stride/8 - 1` (slot 0 is the vendor name)."""
        g = self.g
        limit = self.stride // 8 - 1
        found = {}
        for lo, hi in sorted({g.func_range(i.pc) for i, _ in self.table_sites}):
            for pc in range(lo, hi, 4):
                i = g.ins.get(pc)
                if i is None or not is_cmp_imm(i) or cmp_imm(i.word) != limit:
                    continue
                nx = g.ins.get(pc + 4)
                if not nx or not nx.mn.startswith('b.'):
                    continue
                for q in range(pc - 0x40, pc + 0x40, 4):
                    j = g.ins.get(q)
                    m = j.mem() if j else None
                    if j and j.mn == 'ldr' and m and m[2] == i.ops[0][1] and m[4] == 3:
                        found[pc] = i
        if not found:
            fail('no loop bounded by %d entries per row (stride %#x)' % (limit, self.stride))
        self.max_entries = limit
        self.entry_bounds = sorted(found)

    def check_counts(self):
        got = {
            'table': len(self.table_sites),
            'nozzle': len(self.nozzle_sites),
            'buttons': len(self.button_sites),
            'bound_count': len(self.count_bounds),
            'bound_maxidx': len(self.maxidx_bounds),
        }
        bad = ['%s: found %d, expected %d' % (k, got[k], self.expect[k]) for k in got if got[k] != self.expect[k]]
        if bad:
            fail('site counts differ from the known layout (review, then pass --expect): ' + '; '.join(bad))
        for c in list(self.count_bounds.values()):
            if cmp_imm(c.word) != self.rows:
                fail('%s: count bound is not %d' % (c, self.rows))
        if self.rows > MAX_VENDORS:
            fail('stock table already has %d rows' % self.rows)

    # output ------------------------------------------------------------------

    def sites(self):
        out = []
        for i, _ in self.table_sites:
            out.append(('table', i.pc, i.word, i.word & 0x1f))
        for i, _ in self.button_sites:
            out.append(('buttons', i.pc, i.word, i.word & 0x1f))
        for pc, i in self.count_bounds.items():
            out.append(('bound_count', pc, i.word, None))
        for pc, i in self.maxidx_bounds.items():
            out.append(('bound_maxidx', pc, i.word, None))
        return sorted(out, key=lambda s: (['table', 'buttons', 'bound_count', 'bound_maxidx'].index(s[0]), s[1]))

    def descriptor(self):
        L = []
        L.append('# libgui-vendors patch descriptor, generated by analyze_gui.py; do not edit')
        L.append('# vendors: %s' % ', '.join('%s (%d)' % (v, len(e)) for v, e in zip(self.vendors, self.entries)))
        L.append('# vendor index int8 at %#x; row stride evidence %s'
                 % (self.V, ' '.join('%#x' % p for p in self.stride_evidence)))
        for i, offs in self.nozzle_sites:
            L.append('# keep %#x %08x (base + %s addresses the table after it; not patched)'
                     % (i.pc, i.word, '/'.join('%#x' % o for o in offs)))
        for i in self.unrelated:
            L.append('# unrelated %#x %08x %s %s' % (i.pc, i.word, i.mn, i.text))
        L.append('# entry bound %s' % ' '.join('%#x' % p for p in self.entry_bounds))
        L.append('gui_sha256 %s' % self.g.sha256)
        L.append('table %#x rows %d stride %d max_entries %d' % (self.T, self.rows, self.stride, self.max_entries))
        L.append('buttons %#x slots %d' % (self.B, self.rows))
        L.append('max_vendors %d' % MAX_VENDORS)
        for kind, pc, word, xd in self.sites():
            if xd is None:
                L.append('site %s %#x %08x' % (kind, pc, word))
            else:
                L.append('site %s %#x %08x x%d' % (kind, pc, word, xd))
        return '\n'.join(L) + '\n'


# ---- static patch check ---------------------------------------------------

def enc_b(src, dst):
    off = dst - src
    if off % 4 or not -(1 << 27) <= off < (1 << 27):
        fail('branch %#x -> %#x out of range' % (src, dst))
    return 0x14000000 | ((off >> 2) & 0x3ffffff)


def enc_mov64(rd, value):
    """movz + 3x movk loading a 64-bit constant into Xd."""
    out = [0xd2800000 | (value & 0xffff) << 5 | rd]
    for hw in range(1, 4):
        out.append(0xf2800000 | hw << 21 | ((value >> (16 * hw)) & 0xffff) << 5 | rd)
    return out


def dry_run_apply(a, rows, load_base):
    """Apply the descriptor to an in-memory copy, then re-decode every patched
    instruction and trampoline and check what it does."""
    g = a.g
    md = capstone.Cs(CS_ARCH, capstone.CS_MODE_ARM)
    md.detail = True
    patched = bytearray(g.data)
    end = max(va + memsz for va, _, memsz, _, _ in g.loads)
    seg = ((end + 0xffff) & ~0xffff) + load_base  # synthetic segment after the image
    new_table = seg
    new_buttons = seg + ((rows * a.stride + 0xf) & ~0xf)
    tramp_base = (new_buttons + rows * 8 + 0xf) & ~0xf
    tramp = bytearray()
    log = []

    def dis(code, pc):
        return decode_one(md, bytes(code), pc)

    for kind, pc, word, xd in a.sites():
        o = g.v2o(pc)
        if struct.unpack_from('<I', patched, o)[0] != word:
            fail('dry-run: %#x does not hold %08x' % (pc, word))
        site = pc + load_base
        if kind in ('table', 'buttons'):
            target = new_table if kind == 'table' else new_buttons
            t = tramp_base + len(tramp)
            words = enc_mov64(xd, target) + [enc_b(t + 16, site + 4)]
            tramp += b''.join(struct.pack('<I', w) for w in words)
            struct.pack_into('<I', patched, o, enc_b(site, t))
            # verify: site branches to the trampoline, which loads target into Xd and returns
            s = dis(patched[o:o + 4], site)
            if len(s) != 1 or s[0].mn != 'b' or s[0].target() != t:
                fail('dry-run: %#x does not decode as b %#x' % (site, t))
            ti = dis(tramp[t - tramp_base:t - tramp_base + 20], t)
            val = 0
            for k, x in enumerate(ti[:4]):
                if x.mn not in ('mov', 'movz', 'movk') or x.ops[0][1] != 'x%d' % xd:
                    fail('dry-run: trampoline %#x insn %d is %s' % (t, k, x))
                imm = x.ops[1][1]
                if x.mn == 'movk':
                    val = (val & ~(0xffff << x.ops[1][3])) | imm << x.ops[1][3]
                else:
                    val = imm
            if val != target:
                fail('dry-run: trampoline %#x loads %#x, expected %#x' % (t, val, target))
            if ti[4].mn != 'b' or ti[4].target() != site + 4:
                fail('dry-run: trampoline %#x does not return to %#x' % (t, site + 4))
            log.append('%s %#x: %s  ->  %s' % (kind, pc, g.ins[pc].mn + ' ' + g.ins[pc].text,
                                                 ' ; '.join('%s %s' % (x.mn, x.text) for x in s + ti)))
        else:
            want = rows if kind == 'bound_count' else rows - 1
            if want > 0xfff:
                fail('dry-run: %d does not fit a cmp immediate' % want)
            new = (word & ~(0xfff << 10)) | want << 10
            struct.pack_into('<I', patched, o, new)
            s = dis(patched[o:o + 4], site)
            old = g.ins[pc]
            if len(s) != 1 or s[0].mn != 'cmp' or s[0].ops[0][1] != old.ops[0][1] or s[0].ops[1][1] != want \
                    or s[0].text.split(',')[0] != old.text.split(',')[0]:
                fail('dry-run: %#x decodes as %s' % (site, s))
            log.append('%s %#x: %s %s  ->  %s %s' % (kind, pc, old.mn, old.text, s[0].mn, s[0].text))
    # nothing but the sites changed
    words = set()
    for c in range(0, len(g.data), 4096):
        if g.data[c:c + 4096] != patched[c:c + 4096]:
            words |= {k & ~3 for k in range(c, min(c + 4096, len(g.data))) if g.data[k] != patched[k]}
    sites = {g.v2o(pc) for _, pc, _, _ in a.sites()}
    if not words <= sites:
        fail('dry-run: unexpected bytes changed at %s' % sorted(map(hex, words - sites)))
    return seg, new_table, new_buttons, tramp_base, len(tramp), log


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('gui', help='path to the stock /usr/bin/gui')
    ap.add_argument('-o', '--output', help='write the descriptor here (default: stdout)')
    ap.add_argument('--expect', action='append', default=[], metavar='KIND=N',
                    help='override an expected site count (%s)' % ', '.join('%s=%d' % kv for kv in EXPECT.items()))
    ap.add_argument('--dry-run-apply', action='store_true',
                    help='also patch a copy in memory and re-disassemble every patched site')
    ap.add_argument('--dry-run-rows', type=int, default=8, help='row count for --dry-run-apply (default 8)')
    ap.add_argument('--dry-run-base', type=lambda s: int(s, 0), default=0x5555550000,
                    help='load base for --dry-run-apply (default 0x5555550000)')
    args = ap.parse_args(argv)

    expect = dict(EXPECT)
    for kv in args.expect:
        k, _, n = kv.partition('=')
        if k not in expect or not n.isdigit():
            ap.error('bad --expect %r' % kv)
        expect[k] = int(n)

    try:
        a = Analysis(Gui(args.gui), expect).run()
        desc = a.descriptor()
        if args.dry_run_apply:
            rows = args.dry_run_rows
            if not a.rows <= rows <= MAX_VENDORS:
                fail('--dry-run-rows must be within %d..%d' % (a.rows, MAX_VENDORS))
            seg, nt, nb, tb, tl, log = dry_run_apply(a, rows, args.dry_run_base)
            print('dry-run: base %#x, segment %#x: table %#x, buttons %#x, %d trampoline bytes at %#x'
                  % (args.dry_run_base, seg, nt, nb, tl, tb), file=sys.stderr)
            for line in log:
                print('dry-run: ' + line, file=sys.stderr)
            print('dry-run: OK, %d sites patched for %d rows' % (len(log), rows), file=sys.stderr)
    except AnalysisError as e:
        print('analyze_gui: %s: %s' % (args.gui, e), file=sys.stderr)
        return 1

    if args.output:
        with open(args.output, 'w') as f:
            f.write(desc)
    else:
        sys.stdout.write(desc)
    return 0


if __name__ == '__main__':
    sys.exit(main())
