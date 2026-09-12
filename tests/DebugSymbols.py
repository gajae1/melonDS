#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native UCRT64 tiny-PE checks; not full Qt/application acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/split-debug-symbols.py'
SDK = None
WORK_DIR = None


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(os.name == 'nt', 'native Windows GNU x64 PE fixture')
class DebugSymbols(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = SDK / 'bin/gcc.exe' if SDK else Path(shutil.which('gcc') or '')
        if not compiler.is_file() or compiler.parent.parent.name.lower() != 'ucrt64':
            raise RuntimeError('Requires an installed MSYS2 UCRT64 compiler')
        cls.sdk = compiler.resolve().parent.parent
        cls.env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                       PATH=str(cls.sdk / 'bin') + os.pathsep + os.environ.get('PATH', ''))
        if WORK_DIR:
            WORK_DIR.mkdir(parents=True, exist_ok=True)
            cls.root = Path(tempfile.mkdtemp(prefix='native-', dir=WORK_DIR)).resolve()
        else:
            cls.temporary = tempfile.TemporaryDirectory(prefix='melonds-symbols-')
            cls.root = Path(cls.temporary.name).resolve()
        cls.source = cls.root / 'known.c'
        cls.source.write_text('''#include <stdio.h>
#include <string.h>
__attribute__((noinline)) int known_function(int value)
{
    return value + 7;
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--build-info") == 0) {
        puts("{\\"schema\\":1,\\"source_id\\":\\"1111111111111111111111111111111111111111111111111111111111111111\\",\\"version\\":\\"9.9.46\\"}");
        return 0;
    }
    printf("fixture=%d\\n", known_function(35));
    return 0;
}
''', encoding='utf-8')
        cls.original = cls.root / '9.9.46-fixture.exe'
        cls.run_command([compiler, '-g', '-O0', cls.source, '-o', cls.original])
        print(f'Native fixture: {cls.root}', flush=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'temporary'):
            cls.temporary.cleanup()

    @classmethod
    def run_command(cls, args, check=True):
        result = subprocess.run([str(a) for a in args], env=cls.env, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=30)
        if check and result.returncode:
            raise AssertionError(f'{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
        return result

    def split(self, source, destination, *extra, sdk=None):
        return self.run_command([sys.executable, TOOL, source, destination,
                                 '--msys-prefix', sdk or self.sdk, *extra], check=False)

    def test_native_split_execution_and_source_recovery(self):
        original = self.original.read_bytes()
        original_time = self.original.stat().st_mtime_ns
        destination = self.root / 'symbols with spaces'
        before = self.run_command([self.original]).stdout
        result = self.split(self.original, destination, '--build-info')
        self.assertEqual(result.returncode, 0, result.stderr)
        executable = destination / self.original.name
        debug = destination / (self.original.name + '.debug')
        self.assertEqual(before, 'fixture=42\n')
        self.assertEqual(self.run_command([executable]).stdout, before)
        self.assertEqual(self.original.read_bytes(), original)
        self.assertEqual(self.original.stat().st_mtime_ns, original_time)
        manifest = json.loads((destination / 'debug-symbols.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['schema'], 1)
        self.assertEqual(manifest['input'], {'name': self.original.name, 'sha256': sha(self.original)})
        self.assertEqual(manifest['executable'], {'name': executable.name, 'sha256': sha(executable)})
        self.assertEqual(manifest['debug'], {'name': debug.name, 'sha256': sha(debug)})
        self.assertEqual(manifest['build_info'], {'schema': 1, 'source_id': '1' * 64, 'version': '9.9.46'})
        self.assertEqual(set(p.name for p in destination.iterdir()),
                         {executable.name, debug.name, 'debug-symbols.json'})
        sections = self.run_command([self.sdk / 'bin/objdump.exe', '-h', executable]).stdout
        self.assertNotIn('.debug_info', sections)
        self.assertIn('.gnu_debuglink', sections)
        symbols = self.run_command([self.sdk / 'bin/nm.exe', '-n', self.original]).stdout
        address = re.search(r'^([0-9a-fA-F]+) T known_function$', symbols, re.M).group(1)
        recovered = self.run_command([self.sdk / 'bin/addr2line.exe', '-f', '-e', debug, '0x' + address]).stdout
        self.assertEqual(recovered.splitlines()[0], 'known_function')
        self.assertRegex(recovered.splitlines()[1].replace('\\', '/'), r'/known\.c:4$')
        link = self.root / 'debuglink.bin'
        self.run_command([self.sdk / 'bin/objcopy.exe', '--dump-section',
                          f'.gnu_debuglink={link}', executable])
        content = link.read_bytes()
        self.assertEqual(content.split(b'\0', 1)[0], debug.name.encode('utf-8'))
        self.assertEqual(struct.unpack('<I', content[-4:])[0], zlib.crc32(debug.read_bytes()))
        self.assertEqual(manifest['debuglink_crc32'], f'{zlib.crc32(debug.read_bytes()):08x}')
        self.assertNotEqual(sha(executable), sha(self.original))
        print(f'Execution equal; known_function 0x{address} -> {recovered.strip()}; '
              f'EXE={sha(executable)} debug={sha(debug)}', flush=True)

    def test_existing_outputs_are_preserved(self):
        original = self.original.read_bytes()
        original_time = self.original.stat().st_mtime_ns
        occupied = self.root / 'occupied'
        occupied.mkdir()
        sentinel = occupied / 'user.txt'
        sentinel.write_bytes(b'generated user file\x00\xff')
        empty = self.root / 'empty'
        empty.mkdir()
        for destination in (occupied, empty, self.original):
            with self.subTest(destination=destination.name):
                result = self.split(self.original, destination)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('already exists', result.stderr)
        self.assertEqual(list(occupied.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b'generated user file\x00\xff')
        self.assertEqual(list(empty.iterdir()), [])
        self.assertEqual(self.original.read_bytes(), original)
        self.assertEqual(self.original.stat().st_mtime_ns, original_time)

    def test_missing_debug_and_unsupported_input(self):
        stripped = self.root / 'no-debug.exe'
        shutil.copyfile(self.original, stripped)
        self.run_command([self.sdk / 'bin/strip.exe', '--strip-debug', stripped])
        non_pe = self.root / 'not-pe.exe'
        non_pe.write_bytes(b'generated unsupported input')
        for source, message in ((stripped, 'no supported DWARF'), (non_pe, 'file format not recognized')):
            with self.subTest(source=source.name):
                before = (sha(source), source.stat().st_mtime_ns)
                destination = self.root / (source.stem + '-output')
                result = self.split(source, destination)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(destination.exists())
                self.assertEqual((sha(source), source.stat().st_mtime_ns), before)

    def test_wrong_tool_and_mid_pipeline_failure(self):
        # Real CLI subprocesses: one non-GNU executable, then a fault-injection
        # strip executable that reports a GNU banner but fails the strip call.
        fake_sdk = self.root / 'injected-tools' / 'ucrt64'
        (fake_sdk / 'bin').mkdir(parents=True)
        for name in ('objcopy.exe', 'objdump.exe'):
            shutil.copyfile(self.sdk / 'bin' / name, fake_sdk / 'bin' / name)
        strip = fake_sdk / 'bin/strip.exe'
        shutil.copyfile(self.original, strip)
        before = (sha(self.original), self.original.stat().st_mtime_ns)
        wrong_output = self.root / 'wrong-tool-output'
        result = self.split(self.original, wrong_output, sdk=fake_sdk)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Expected GNU strip', result.stderr)
        self.assertFalse(wrong_output.exists())

        stub = self.root / 'failing-strip.c'
        stub.write_text('''#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("GNU strip (test fault injection)"); return 0;
    }
    fputs("injected strip failure", stderr); return 23;
}
''', encoding='utf-8')
        self.run_command([self.sdk / 'bin/gcc.exe', stub, '-o', strip])
        failed_output = self.root / 'failed-strip-output'
        result = self.split(self.original, failed_output, sdk=fake_sdk)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('strip.exe exited 23', result.stderr)
        self.assertIn('partial output retained', result.stderr)
        self.assertTrue((failed_output / (self.original.name + '.debug')).is_file())
        self.assertFalse((failed_output / 'debug-symbols.json').exists())
        self.assertEqual((sha(self.original), self.original.stat().st_mtime_ns), before)

    def test_build_info_is_opt_in_and_invalid_metadata_fails(self):
        source = self.root / 'no-metadata.c'
        source.write_text('int main(void) { return 0; }\n', encoding='utf-8')
        executable = self.root / 'no-metadata.exe'
        self.run_command([self.sdk / 'bin/gcc.exe', '-g', source, '-o', executable])
        destination = self.root / 'no-metadata-output'
        result = self.split(executable, destination)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((destination / 'debug-symbols.json').read_text(encoding='utf-8'))
        self.assertIsNone(manifest['build_info'])
        rejected = self.root / 'invalid-metadata-output'
        result = self.split(executable, rejected, '--build-info')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Debug symbols error: build info is not valid JSON', result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertFalse(rejected.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--msys-prefix', type=Path)
    parser.add_argument('--work-dir', type=Path, help='Keep generated evidence beneath this directory')
    args, remaining = parser.parse_known_args()
    SDK, WORK_DIR = args.msys_prefix, args.work_dir
    unittest.main(argv=[sys.argv[0], *remaining])
