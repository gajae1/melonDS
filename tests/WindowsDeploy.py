#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows-only deployment contracts: native PE/CMake, simulated Qt/pacman lists.

Requires an installed UCRT64 gcc/cmake/objdump on PATH; builds only generated
tiny binaries. The real SDK/Qt/application deployment is a separate acceptance.
"""
import contextlib
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('deploy_windows', ROOT / 'tools/deploy-windows.py')
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(os.name == 'nt', 'native Windows UCRT64 fixture')
class WindowsDeploy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = str(cls.sdk / 'bin/gcc.exe') if hasattr(cls, 'sdk') else shutil.which('gcc')
        if not compiler or Path(compiler).parent.parent.name.lower() != 'ucrt64':
            raise RuntimeError('WindowsDeploy requires the MSYS2 UCRT64 toolchain on PATH')
        cls.sdk = Path(compiler).resolve().parent.parent
        cls.temporary = tempfile.TemporaryDirectory(prefix='melonds-deploy-test-')
        cls.root = Path(cls.temporary.name).resolve()
        source = cls.root / 'source'
        source.mkdir()
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.21)
project(deploy_fixture C)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_library(leaf SHARED leaf.c)
add_library(bridge SHARED bridge.c)
add_library(plugin MODULE plugin.c)
set_target_properties(leaf bridge plugin PROPERTIES PREFIX "")
target_link_libraries(bridge PRIVATE leaf)
target_link_libraries(plugin PRIVATE leaf)
add_executable(melonDS main.c)
target_link_libraries(melonDS PRIVATE bridge)
''', encoding='utf-8')
        contents = {
            'leaf.c': '__declspec(dllexport) int leaf(void) { return 1; }',
            'bridge.c': '__declspec(dllimport) int leaf(void); __declspec(dllexport) int bridge(void) { return leaf(); }',
            'plugin.c': '__declspec(dllimport) int leaf(void); __declspec(dllexport) int plugin(void) { return leaf(); }',
            'main.c': '__declspec(dllimport) int bridge(void); int main(void) { return bridge() == 1 ? 0 : 1; }',
        }
        for name, content in contents.items():
            (source / name).write_text(content, encoding='utf-8')
        cls.native = cls.root / 'native'
        for command in ([str(cls.sdk / 'bin/cmake.exe'), '-S', str(source), '-B', str(cls.native),
                         '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_C_COMPILER={compiler}'],
                        [str(cls.sdk / 'bin/cmake.exe'), '--build', str(cls.native), '--parallel', '2']):
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise AssertionError(result.stdout.decode('utf-8', 'replace'))

    @classmethod
    def tearDownClass(cls):
        # Cleanup is limited to the generated test directory under the temp root.
        assert cls.root.is_relative_to(Path(tempfile.gettempdir()).resolve())
        assert cls.root.name.startswith('melonds-deploy-test-')
        cls.temporary.cleanup()

    def setUp(self):
        self.work = self.root / self._testMethodName
        self.work.mkdir()
        self.prefix = self.work / 'msys64/ucrt64'
        (self.prefix / 'bin').mkdir(parents=True)
        (self.prefix.parent / 'usr/bin').mkdir(parents=True)
        shutil.copy2(self.sdk / 'bin/objdump.exe', self.prefix / 'bin/objdump.exe')
        (self.prefix / 'bin/cmake.exe').write_bytes(b'Use the installed CMake resource directory')
        (self.prefix / 'bin/windeployqt.exe').write_bytes(b'Qt tool call is simulated')
        self.build = self.work / 'build'
        self.build.mkdir()
        shutil.copy2(self.native / 'bin/melonDS.exe', self.build / 'melonDS.exe')
        (self.build / 'release-version.txt').write_text('9.9.90\n', encoding='utf-8')
        for name in ('bridge.dll', 'leaf.dll'):
            shutil.copy2(self.native / 'bin' / name, self.prefix / 'bin' / name)
        self.plugin = self.prefix / 'share/qt6/plugins/platforms/plugin.dll'
        self.plugin.parent.mkdir(parents=True)
        shutil.copy2(self.native / 'bin/plugin.dll', self.plugin)
        self.license = self.prefix / 'share/licenses/fixture/COPYING'
        self.license.parent.mkdir(parents=True)
        self.license.write_bytes(b'explicit generated license')
        self.unlisted = [self.prefix / 'bin/personal.txt', self.license.parent / 'personal.json',
                         self.prefix / 'bin/unreferenced.dll']
        for path in self.unlisted:
            path.write_bytes(b'generated file absent from trusted lists')
        self.runtime = self.work / 'runtime'
        self.manifest = self.work / 'manifest.json'
        self.qt = [{'source': str(self.prefix / 'bin/bridge.dll'), 'target': str(self.runtime)},
                   {'source': str(self.plugin), 'target': str(self.runtime / 'platforms')}]
        self.inventory = '/ucrt64/share/licenses/fixture/\n/ucrt64/share/licenses/fixture/COPYING\n/ucrt64/include/ignored.h\n'
        self.package = 'mingw-w64-ucrt-x86_64-fixture'
        self.package_version = '1.2.3-1'
        self.extra_owner = None
        self.notice_map = None
        self.owned_queries = []

    def invoke(self):
        original_run = deploy.run_tool
        def tools(command):
            name = Path(command[0]).name
            if name == 'windeployqt.exe':
                self.assertTrue({'--dry-run', '--json', '--no-translations', '--no-patchqt'}.issubset(command))
                return json.dumps({'files': self.qt})
            if name == 'pacman.exe':
                if command[1] == '-Qoq':
                    self.owned_queries.extend(command[2:])
                    return '\n'.join([self.package] + ([self.extra_owner] if self.extra_owner else [])) + '\n'
                if command[1] == '-Q':
                    self.assertEqual(command[2], self.package)
                    return self.package + ' ' + self.package_version + '\n'
                if self.extra_owner and command[1:] == ['-Qlq', self.extra_owner]:
                    return '/ucrt64/include/ignored.h\n'
                self.assertEqual(command[1:], ['-Qlq', self.package])
                return self.inventory
            if name == 'cmake.exe':
                command = [self.sdk / 'bin/cmake.exe', *command[1:]]
            return original_run(command)  # Real CMake and objdump, including failure cases.
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(deploy, 'run_tool', side_effect=tools), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            args = [str(self.build), str(self.runtime), '--msys-prefix', str(self.prefix),
                    '--runtime-manifest', str(self.manifest)]
            if self.notice_map:
                args.extend(['--license-supplement', str(self.notice_map)])
            code = deploy.main(args)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_existing_outputs_preserved(self):
        self.runtime.mkdir()
        user_file = self.runtime / 'user-photo.jpg'
        user_file.write_bytes(b'generated user file')
        code, _, error = self.invoke()
        self.assertEqual(code, 1)
        self.assertIn('already exists', error)
        self.assertEqual(user_file.read_bytes(), b'generated user file')
        self.assertFalse(self.manifest.exists())
        self.runtime = self.work / 'another-runtime'
        self.manifest.write_bytes(b'original manifest bytes')
        self.assertEqual(self.invoke()[0], 1)
        self.assertEqual(self.manifest.read_bytes(), b'original manifest bytes')
        self.assertFalse(self.runtime.exists())
        self.manifest = self.work / 'invalid-version-manifest.json'
        version = self.build / 'release-version.txt'
        version.write_bytes(b'9.9.90&not-a-version\n')
        code, _, error = self.invoke()
        self.assertEqual(code, 1)
        self.assertIn('numeric dotted release version', error)
        self.assertFalse(self.runtime.exists())
        self.assertFalse(self.manifest.exists())
        self.assertEqual(version.read_bytes(), b'9.9.90&not-a-version\n')

    def test_explicit_mapping_closure_licenses_and_hashes(self):
        before = {p: sha(p) for p in [self.build / 'melonDS.exe', self.prefix / 'bin/bridge.dll',
                                     self.prefix / 'bin/leaf.dll', self.plugin, self.license, *self.unlisted]}
        code, output, error = self.invoke()
        self.assertEqual(code, 0, error)
        manifest = json.loads(self.manifest.read_text(encoding='utf-8'))
        self.assertEqual(manifest['schema'], 1)
        expected = {'9.9.90-melonDS.exe', 'bridge.dll', 'leaf.dll', 'platforms/plugin.dll', 'LICENSE',
                    'release-version.txt', 'README.txt', 'run-melonDS.cmd', 'third-party-licenses/fixture/COPYING'}
        self.assertEqual(set(manifest['files']), expected)
        self.assertEqual({p.relative_to(self.runtime).as_posix() for p in self.runtime.rglob('*') if p.is_file()}, expected)
        self.assertTrue(all(sha(self.runtime / name) == value for name, value in manifest['files'].items()))
        self.assertEqual(before, {p: sha(p) for p in before})
        self.assertEqual(set(self.owned_queries), {'/ucrt64/bin/bridge.dll', '/ucrt64/bin/leaf.dll',
                                                  '/ucrt64/share/qt6/plugins/platforms/plugin.dll'})
        self.assertEqual(json.loads(output)['license_packages'], [self.package])
        self.assertIn('9.9.90-melonDS.exe', (self.runtime / 'README.txt').read_text(encoding='utf-8'))
        self.assertIn('"%~dp09.9.90-melonDS.exe"', (self.runtime / 'run-melonDS.cmd').read_text(encoding='utf-8'))
        env = dict(os.environ, PATH=os.environ['SystemRoot'] + '/System32')
        result = subprocess.run([str(Path(os.environ['SystemRoot']) / 'System32/cmd.exe'), '/d', '/c', 'run-melonDS.cmd'],
                                cwd=self.runtime, env=env)
        self.assertEqual(result.returncode, 0, 'launcher must run the versioned native EXE with its transitive DLLs')

    def test_missing_and_conflicting_imports(self):
        leaf = self.prefix / 'bin/leaf.dll'
        saved = leaf.read_bytes()
        leaf.unlink()
        code, _, error = self.invoke()
        self.assertEqual(code, 1)
        self.assertIn('unresolved=[leaf.dll]', error)
        self.assertFalse(self.manifest.exists())
        leaf.write_bytes(saved)
        (self.plugin.parent / 'leaf.dll').write_bytes(saved + b'different generated image')
        self.runtime = self.work / 'conflict-runtime'
        for entry in self.qt:
            entry['target'] = entry['target'].replace('/runtime', '/conflict-runtime').replace('\\runtime', '\\conflict-runtime')
        code, _, error = self.invoke()
        self.assertEqual(code, 1)
        self.assertIn('conflicting=[leaf.dll]', error)
        self.assertFalse(self.manifest.exists())

    def test_untrusted_sources_destinations_and_collisions(self):
        original = [dict(entry) for entry in self.qt]
        for kind in ('outside-source', 'outside-target', 'destination-conflict'):
            with self.subTest(kind=kind):
                self.runtime = self.work / kind
                self.qt = [{'source': e['source'], 'target': str(self.runtime / ('platforms' if i else ''))}
                           for i, e in enumerate(original)]
                if kind == 'outside-source':
                    self.qt[0]['source'] = str(self.build / 'melonDS.exe')
                elif kind == 'outside-target':
                    self.qt[0]['target'] = str(self.work)
                else:
                    other = self.prefix / 'other/bridge.dll'
                    other.parent.mkdir()
                    other.write_bytes((self.prefix / 'bin/bridge.dll').read_bytes() + b'different')
                    self.qt.append({'source': str(other), 'target': str(self.runtime)})
                code, _, error = self.invoke()
                self.assertEqual(code, 1, error)
                self.assertIn({'outside-source': 'inside', 'outside-target': 'escapes runtime',
                               'destination-conflict': 'Conflicting destination contents'}[kind], error)
                self.assertFalse(self.manifest.exists())

    def test_missing_license_and_final_stage_tampering(self):
        inventory = self.inventory
        self.inventory = '/ucrt64/include/ignored.h\n'
        code, _, error = self.invoke()
        self.assertEqual(code, 1)
        self.assertIn('No installed license files', error)
        self.assertIn(self.package, error)
        self.assertFalse(self.manifest.exists())
        self.inventory = inventory
        original_copy = shutil.copyfileobj
        for kind in ('extra', 'changed'):
            with self.subTest(kind=kind):
                self.runtime = self.work / kind
                self.qt[0]['target'] = str(self.runtime)
                self.qt[1]['target'] = str(self.runtime / 'platforms')
                def tamper(source, output):
                    original_copy(source, output)
                    if kind == 'extra':
                        (self.runtime / 'unselected.txt').write_bytes(b'generated concurrent file')
                    else:
                        output.write(b'generated changed bytes')
                with patch.object(deploy.shutil, 'copyfileobj', side_effect=tamper):
                    code, _, error = self.invoke()
                self.assertEqual(code, 1)
                self.assertIn('unselected files' if kind == 'extra' else 'hash mismatch', error)
                self.assertFalse(self.manifest.exists())
                self.assertTrue(self.runtime.is_dir(), 'failed new stages are preserved for diagnosis')


    def test_installed_icu_and_qt_split_license_metadata(self):
        icu = 'mingw-w64-ucrt-x86_64-icu'
        provider = 'mingw-w64-ucrt-x86_64-qt6-multimedia'
        backend = provider + '-ffmpeg'
        icu_license = self.prefix / 'share/icu/78.3/LICENSE'
        qt_license = self.prefix / 'share/licenses/qt6-multimedia/LGPL-3.0-only.txt'
        for path in (icu_license, qt_license):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'generated package-specific notice')
        versions = {provider: '6.11.2-1', backend: '6.11.2-1'}
        def record(package, base):
            path = self.prefix.parent / f'var/lib/pacman/local/{package}-{versions[package]}/desc'
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f'%NAME%\n{package}\n\n%VERSION%\n{versions[package]}\n\n%BASE%\n{base}\n', encoding='utf-8')
        record(provider, 'mingw-w64-qt6-multimedia')
        record(backend, 'mingw-w64-qt6-multimedia')
        inventories = {icu: '/ucrt64/share/icu/78.3/LICENSE\n', provider:
                       '/ucrt64/share/licenses/qt6-multimedia/LGPL-3.0-only.txt\n', backend: '/ucrt64/bin/ignored.dll\n'}
        def pacman(command):
            if command[1] == '-Qoq':
                return '\n'.join((icu, provider, backend))
            if command[1] == '-Qlq':
                return inventories[command[2]]
            self.assertEqual(command[1], '-Q')
            return command[2] + ' ' + versions[command[2]] + '\n'
        with patch.object(deploy, 'run_tool', side_effect=pacman):
            inputs, packages = deploy.license_inputs(self.prefix, [self.prefix / 'bin/bridge.dll'], {})
            self.assertEqual(set(packages), {icu, provider, backend})
            self.assertEqual({(relative, path) for relative, path, _ in inputs}, {
                ('third-party-licenses/icu/LICENSE', icu_license),
                ('third-party-licenses/qt6-multimedia/LGPL-3.0-only.txt', qt_license)})
            record(backend, 'different-source-package')
            with self.assertRaisesRegex(ValueError, 'provider source/version mismatch'):
                deploy.license_inputs(self.prefix, [self.prefix / 'bin/bridge.dll'], {})
            versions[backend] = '6.11.1-1'
            record(backend, 'mingw-w64-qt6-multimedia')
            with self.assertRaisesRegex(ValueError, 'provider source/version mismatch'):
                deploy.license_inputs(self.prefix, [self.prefix / 'bin/bridge.dll'], {})

    def test_explicit_version_and_hash_bound_license_supplement(self):
        self.inventory = '/ucrt64/include/ignored.h\n'
        notice = self.work / 'source/NOTICE.txt'
        notice.parent.mkdir()
        notice.write_bytes(b'generated exact-version source notice')
        self.notice_map = self.work / 'license-supplement.json'
        input_path = str(notice)
        document = {'schema': 1, 'packages': {self.package: {
            'version': self.package_version, 'files': {input_path: sha(notice)}}}}
        self.notice_map.write_text(json.dumps(document), encoding='utf-8')
        code, _, error = self.invoke()
        self.assertEqual(code, 0, error)
        manifest = json.loads(self.manifest.read_text(encoding='utf-8'))
        relative = f'third-party-licenses/{self.package}/{notice.name}'
        self.assertEqual(manifest['files'][relative], sha(notice))
        self.assertEqual((self.runtime / relative).read_bytes(), notice.read_bytes())
        for kind in ('wrong-version', 'wrong-hash', 'empty-files', 'unselected-package', 'remaining-missing', 'relative-path'):
            with self.subTest(kind=kind):
                self.runtime = self.work / kind
                self.qt[0]['target'] = str(self.runtime)
                self.qt[1]['target'] = str(self.runtime / 'platforms')
                self.manifest = self.work / f'{kind}.json'
                invalid = json.loads(json.dumps(document))
                entry = invalid['packages'][self.package]
                if kind == 'wrong-version':
                    entry['version'] = '1.2.2-1'
                elif kind == 'wrong-hash':
                    entry['files'][input_path] = '0' * 64
                elif kind == 'empty-files':
                    entry['files'] = {}
                elif kind == 'unselected-package':
                    invalid['packages'][self.package + '-unselected'] = invalid['packages'].pop(self.package)
                elif kind == 'remaining-missing':
                    self.extra_owner = self.package + '-unprovided'
                else:
                    self.extra_owner = None
                    entry['files'] = {'../outside-NOTICE.txt': sha(notice)}
                self.notice_map.write_text(json.dumps(invalid), encoding='utf-8')
                code, _, error = self.invoke()
                self.assertEqual(code, 1, error)
                self.assertIn({'wrong-version': 'installed version mismatch', 'wrong-hash': 'supplement hash mismatch',
                               'empty-files': 'explicit files', 'unselected-package': 'outside the selected runtime',
                               'remaining-missing': 'No installed license files', 'relative-path': 'absolute paths'}[kind], error)
                self.assertFalse(self.manifest.exists())
                self.assertEqual(sha(notice), document['packages'][self.package]['files'][input_path])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--msys-prefix', type=Path)
    args, remaining = parser.parse_known_args()
    if args.msys_prefix:
        WindowsDeploy.sdk = args.msys_prefix.resolve()
        os.environ['PATH'] = os.pathsep.join((str(WindowsDeploy.sdk / 'bin'),
                                             str(WindowsDeploy.sdk.parent / 'usr/bin'), os.environ['PATH']))
    unittest.main(argv=[sys.argv[0], *remaining])
