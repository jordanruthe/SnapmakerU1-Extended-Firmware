#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

"""Runs analyze_gui.py on the stock GUI of every extracted firmware
(tmp/extracted-*/), then on copies with one instruction changed in a way that
would make patching unsafe, and checks that each copy is rejected."""

import glob
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
ROOT = os.path.realpath(os.path.join(HERE, '..', '..', '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'scripts'))

import analyze_gui as A  # noqa: E402

COND = {'eq': 0, 'ne': 1}


def versions():
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'tmp', 'extracted-*'))):
        gui = os.path.join(path, 'rootfs', 'usr', 'bin', 'gui')
        if os.path.isfile(gui):
            out.append((os.path.basename(path)[len('extracted-'):], gui))
    return out


def ldr_imm(rt, rn, off):
    return 0xf9400000 | (off // 8) << 10 | rn << 5 | rt


def reg(name):
    return int(name[1:])


class AnalyzeGuiTest(unittest.TestCase):
    stock = {}

    @classmethod
    def setUpClass(cls):
        if not versions():
            raise unittest.SkipTest('no tmp/extracted-*/rootfs/usr/bin/gui')
        for ver, gui in versions():
            g = A.Gui(gui)
            cls.stock[ver] = (g, A.Analysis(g, A.EXPECT).run())

    def mutate(self, ver, changes, pattern):
        g, _ = self.stock[ver]
        data = bytearray(g.data)
        for va, word in changes:
            struct.pack_into('<I', data, g.v2o(va), word)
        with tempfile.NamedTemporaryFile(suffix='-gui') as f:
            f.write(data)
            f.flush()
            with self.assertRaisesRegex(A.AnalysisError, pattern):
                A.Analysis(A.Gui(f.name), A.EXPECT).run()

    def each(self):
        for ver in sorted(self.stock):
            with self.subTest(firmware=ver):
                yield ver, self.stock[ver][1]

    def test_stock_descriptor_and_dry_run(self):
        for ver, a in self.each():
            d = a.descriptor()
            self.assertIn('gui_sha256 %s\n' % a.g.sha256, d)
            self.assertEqual(sum(A.EXPECT[k] for k in A.EXPECT if k != 'nozzle'), len(a.sites()))
            log = A.dry_run_apply(a, A.MAX_VENDORS, 0x5555550000)[-1]
            self.assertEqual(len(a.sites()), len(log))

    def test_table_ref_with_other_offset(self):
        for ver, a in self.each():
            ref = a.table_sites[0][0]
            self.mutate(ver, [(ref.pc, ref.word + (8 << 10))], 'reference to the table|row|stride')

    def test_count_bound_differs(self):
        for ver, a in self.each():
            c = next(iter(a.count_bounds.values()))
            self.mutate(ver, [(c.pc, c.word + (1 << 10))], 'disagree')

    def test_count_loop_latch_inverted(self):
        for ver, a in self.each():
            c = next(iter(a.count_bounds.values()))
            latch = a.g.ins[c.pc + 4]
            self.mutate(ver, [(latch.pc, latch.word & ~0xf | COND['eq'])], 'latch')

    def test_vendor_index_equality_compare(self):
        for ver, a in self.each():
            pc = min(a.maxidx_bounds)
            br = a.g.ins[pc + 4]
            self.assertTrue(br.mn.startswith('b.'), br)
            self.mutate(ver, [(br.pc, br.word & ~0xf | COND['eq'])], 'not as an upper bound')

    def test_direct_button_slot_access(self):
        for ver, a in self.each():
            # the instruction after a count loop, reading slot 1 through the array register
            site = a.button_sites[0][0]
            c = min((c for c in a.count_bounds.values() if c.pc > site.pc), key=lambda c: c.pc)
            xb = reg(site.ops[0][1])
            self.mutate(ver, [(c.pc + 8, ldr_imm(16, xb, 8))], 'unsupported use of the button array')

    def test_load_past_table_end_from_table_ref(self):
        for ver, a in self.each():
            size = a.rows * a.stride
            for ref, _ in a.table_sites:
                nx = a.g.ins[ref.pc + 4]
                xd = ref.ops[0][1]
                if xd not in nx.r and xd not in nx.w and not nx.is_branch() and nx.mn in ('stp', 'str') \
                        and nx.mem()[1] == 'sp':
                    break
            else:
                self.fail('no table ref followed by a stack store')
            self.mutate(ver, [(nx.pc, ldr_imm(16, reg(xd), size + 8))], 'inside the vendor table and beyond')


if __name__ == '__main__':
    unittest.main(verbosity=2)
