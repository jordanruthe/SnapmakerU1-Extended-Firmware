#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

import glob
import importlib.util
import json
import logging
import os
import subprocess
import sys
import tempfile
import types
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
ROOT = os.path.realpath(os.path.join(HERE, '..', '..', '..', '..'))
PATCH = os.path.join(HERE, '..', 'patches', 'home', 'lava', 'klipper', '01-filament-library.patch')
STOCK_REL = os.path.join('rootfs', 'home', 'lava', 'klipper', 'klippy', 'extras', 'filament_parameters.py')

LIBRARY = {'version': 1, 'filaments': [
    {'id': 'a', 'name': 'eSun PLA+ Black', 'vendor': 'eSun', 'type': 'PLA+', 'sub_type': '', 'base_type': 'PLA',
     'load_temp': 225, 'unload_temp': 215, 'print_temp': 220},
    {'id': 'b', 'name': 'Generic HIPS', 'vendor': 'Generic', 'type': 'HIPS', 'base_type': 'ABS', 'print_temp': 235},
    {'id': 'c', 'name': 'My Polymaker override', 'vendor': 'Polymaker', 'type': 'PLA', 'sub_type': 'PolyTerra',
     'load_temp': 200},
    {'id': 'd', 'name': 'bad', 'type': ''},
]}


def versions():
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'tmp', 'extracted-*'))):
        stock = os.path.join(path, STOCK_REL)
        if os.path.isfile(stock):
            out.append((os.path.basename(path)[len('extracted-'):], stock))
    return out


class Gcmd:
    def __init__(self, **params):
        self.params, self.out = params, []

    def get(self, name, default=None):
        return self.params.get(name, default)

    def get_float(self, name, default=None):
        return float(self.params[name]) if name in self.params else default

    def respond_info(self, msg):
        self.out.append(msg)


class Printer:
    def __init__(self, cfgdir):
        self.cfgdir, self.commands = cfgdir, {}

    def get_reactor(self): return None
    def get_snapmaker_config_dir(self): return self.cfgdir
    def get_config_dir(self): return self.cfgdir
    def load_snapmaker_config_file(self, path, default, create_if_not_exist=True): return dict(default)
    def register_event_handler(self, *a): pass

    def lookup_object(self, name):
        return types.SimpleNamespace(register_command=lambda cmd, fn, *a, **k: self.commands.__setitem__(cmd, fn))


class Base:
    """Tests run once per extracted firmware version (see load_tests)."""
    version = stock = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = os.path.join(cls.tmp.name, 'klipper')
        os.makedirs(os.path.join(src, 'klippy', 'extras'))
        dst = os.path.join(src, 'klippy', 'extras', 'filament_parameters.py')
        with open(cls.stock, 'rb') as f, open(dst, 'wb') as o:
            o.write(f.read())
        subprocess.run(['patch', '-F', '0', '--no-backup-if-mismatch', '-d', src, '-p1', '-i', os.path.realpath(PATCH)],
                       check=True, stdout=subprocess.PIPE)
        spec = importlib.util.spec_from_file_location('fp_' + cls.version.replace('.', '_'), dst)
        cls.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.mod)
        spec = importlib.util.spec_from_file_location('fp_stock_' + cls.version.replace('.', '_'), cls.stock)
        cls.stock_mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.stock_mod)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.cfg = tempfile.TemporaryDirectory()
        self.addCleanup(self.cfg.cleanup)
        os.makedirs(os.path.join(self.cfg.name, 'extended'))
        self.path = os.path.join(self.cfg.name, 'extended', 'filament_library.json')
        self.fp = self.make(self.mod)
        self.stock_fp = self.make(self.stock_mod, stock=True)

    def make(self, mod, stock=False):
        printer = Printer(self.cfg.name)
        fp = mod.FilamentParameters(types.SimpleNamespace(get_printer=lambda: printer))
        fp.test_printer = printer
        return fp

    def write(self, data, raw=None, mtime=None):
        with open(self.path, 'w') as f:
            f.write(raw if raw is not None else json.dumps(data))
        if mtime is not None:
            os.utime(self.path, (mtime, mtime))

    def get(self, vendor, typ, sub='', nozzle=0.4, fp=None):
        return (fp or self.fp).get_filament_parameters(vendor, typ, sub, nozzle_diameter=nozzle)

    def temps(self, params):
        return tuple(params[k] for k in ('load_temp', 'unload_temp', 'print_temp'))

    def assertStock(self, vendor, typ, sub='', nozzle=0.4):
        self.assertEqual(self.get(vendor, typ, sub, nozzle), self.get(vendor, typ, sub, nozzle, fp=self.stock_fp))


class LibraryTests(Base):
    def test_base_type_fills_unknown_type(self):
        self.write(LIBRARY)
        p = self.get('eSun', 'PLA+')
        self.assertEqual(self.temps(p), (225, 215, 220))
        self.assertAlmostEqual(p['fast_v'], 4.989, places=2)
        # any subtype matches an entry with blank subtype
        self.assertEqual(self.temps(self.get('eSun', 'PLA+', 'Basic')), (225, 215, 220))
        # case-insensitive
        self.assertEqual(self.temps(self.get('ESUN', 'pla+')), (225, 215, 220))

    def test_other_vendor_unaffected_by_vendor_entry(self):
        self.write(LIBRARY)
        self.assertEqual(self.temps(self.get('Other', 'PLA+')), (250, 250, self.get('Other', 'PLA+')['print_temp']))
        self.assertStock('Other', 'PLA+')

    def test_generic_entry_with_base_type(self):
        self.write(LIBRARY)
        p = self.get('Generic', 'HIPS')
        stock_abs = self.get('Generic', 'ABS', fp=self.stock_fp)
        self.assertEqual(p['load_temp'], stock_abs['load_temp'])
        self.assertEqual(p['print_temp'], 235)
        self.assertEqual(p['load_temp'], 280)
        # blank vendor on the lookup side also hits the generic entry
        self.assertEqual(self.get('Anything', 'HIPS')['print_temp'], 235)

    def test_stock_vendor_partial_override(self):
        self.write(LIBRARY)
        p, s = self.get('Polymaker', 'PLA', 'PolyTerra'), self.get('Polymaker', 'PLA', 'PolyTerra', fp=self.stock_fp)
        self.assertEqual(p['load_temp'], 200)
        self.assertEqual(p['unload_temp'], 250)
        self.assertEqual(p['print_temp'], 215)
        self.assertEqual(p['unload_temp'], s['unload_temp'])
        self.assertEqual(p['print_temp'], s['print_temp'])

    def test_untouched_cases_equal_stock(self):
        self.write(LIBRARY)
        self.assertStock('Snapmaker', 'PLA', 'Basic')
        self.assertStock('Generic', 'FOO')
        self.assertStock('Polymaker', 'PLA', 'PolyLite')

    def test_empty_type_ignored(self):
        self.write(LIBRARY)
        self.assertEqual(len(self.fp._library_entries()), 3)

    def test_precedence(self):
        self.write({'filaments': [
            {'type': 'PETG', 'vendor': 'Generic', 'print_temp': 201},
            {'type': 'PETG', 'vendor': 'Generic', 'sub_type': 'Rapid', 'print_temp': 202},
            {'type': 'PETG', 'vendor': 'Acme', 'print_temp': 203},
            {'type': 'PETG', 'vendor': 'Acme', 'sub_type': 'Rapid', 'print_temp': 204},
        ]})
        pt = lambda *a: self.get(*a)['print_temp']
        self.assertEqual(pt('Acme', 'PETG', 'Rapid'), 204)       # exact
        self.assertEqual(pt('Acme', 'PETG', 'Other'), 203)       # vendor + any subtype
        self.assertEqual(pt('Zed', 'PETG', 'Rapid'), 202)        # generic + subtype
        self.assertEqual(pt('Zed', 'PETG', 'Other'), 201)        # generic + any

    def test_missing_file_is_stock(self):
        self.assertFalse(os.path.exists(self.path))
        for args in (('eSun', 'PLA+'), ('Polymaker', 'PLA', 'PolyTerra'), ('Generic', 'HIPS'), ('Snapmaker', 'PLA', 'Basic')):
            self.assertStock(*args)
        self.assertEqual(self.fp._library_entries(), [])

    def test_malformed_json_is_stock(self):
        for raw in ('{not json', '', '[1,2]', '{"filaments": 5}', '{"filaments": [{"type": "PLA", "print_temp": "abc"}]}'):
            self.write(None, raw=raw)
            self.fp._library_mtime = None
            for args in (('eSun', 'PLA+'), ('Polymaker', 'PLA', 'PolyTerra'), ('Generic', 'PLA')):
                self.assertStock(*args)

    def test_invalid_entry_skips_only_that_entry(self):
        self.write({'filaments': [
            {'vendor': 'Bad', 'type': 'PLA', 'print_temp': 'abc'},
            {'vendor': 'eSun', 'type': 'PLA+', 'base_type': 'PLA', 'print_temp': 220},
        ]})
        self.assertEqual(self.get('eSun', 'PLA+')['print_temp'], 220)
        self.assertStock('Bad', 'PLA')

    def test_mtime_reload(self):
        self.write(LIBRARY, mtime=1000000)
        self.assertEqual(self.get('eSun', 'PLA+')['print_temp'], 220)
        changed = json.loads(json.dumps(LIBRARY))
        changed['filaments'][0]['print_temp'] = 231
        self.write(changed, mtime=1000005)
        self.assertEqual(self.get('eSun', 'PLA+')['print_temp'], 231)
        # unchanged mtime: cached, even if content differs
        changed['filaments'][0]['print_temp'] = 240
        self.write(changed, mtime=1000005)
        self.assertEqual(self.get('eSun', 'PLA+')['print_temp'], 231)
        # file removed: back to stock
        os.unlink(self.path)
        self.assertStock('eSun', 'PLA+')

    def test_is_soft_override(self):
        self.write({'filaments': [{'type': 'PLA', 'vendor': 'Acme', 'is_soft': True},
                                  {'type': 'TPU', 'vendor': 'Acme', 'is_soft': False}]})
        self.assertTrue(self.get('Acme', 'PLA')['is_soft'])
        self.assertFalse(self.get('Acme', 'TPU')['is_soft'])
        self.assertFalse(self.get('Other', 'PLA')['is_soft'])

    def test_other_nozzles(self):
        self.write(LIBRARY)
        for nozzle in (0.2, 0.6, 0.8):
            self.assertEqual(self.temps(self.get('eSun', 'PLA+', nozzle=nozzle)), (225, 215, 220), nozzle)
            self.assertEqual(self.get('Generic', 'HIPS', nozzle=nozzle)['print_temp'], 235, nozzle)
            self.assertEqual(self.get('Polymaker', 'PLA', 'PolyTerra', nozzle=nozzle)['load_temp'], 200, nozzle)

    def test_query_command(self):
        self.write(LIBRARY)
        handler = self.fp.test_printer.commands['FILAMENT_LIBRARY_QUERY']
        g = Gcmd(VENDOR='eSun', TYPE='PLA+')
        handler(g)
        self.assertEqual(len(g.out), 1)
        self.assertIn("eSun PLA+ : library entry 'eSun PLA+ Black'", g.out[0])
        self.assertIn('load_temp=225 unload_temp=215 print_temp=220 is_soft=False flow_k=', g.out[0])
        g = Gcmd(VENDOR='Other', TYPE='PLA+', NOZZLE='0.6')
        handler(g)
        self.assertIn('library entry none', g.out[0])
        g = Gcmd(TYPE='HIPS', SUBTYPE='x')
        handler(g)
        self.assertIn("Generic HIPS x: library entry 'Generic HIPS'", g.out[0])
        self.assertIn('print_temp=235', g.out[0])


def load_tests(loader, tests, pattern):
    suite = unittest.TestSuite()
    found = versions()
    if not found:
        print('SKIP: no tmp/extracted-*/ rootfs with filament_parameters.py found')
    for version, stock in found:
        cls = type('LibraryTests_' + version.replace('.', '_'), (LibraryTests, unittest.TestCase),
                   {'version': version, 'stock': stock})
        suite.addTests(loader.loadTestsFromTestCase(cls))
    return suite


if __name__ == '__main__':
    logging.disable(logging.CRITICAL)  # stock code logs expected errors
    unittest.main(verbosity=2)
