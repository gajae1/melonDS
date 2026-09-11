#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generated fetch/pin fixtures exercise the maintenance CLI with real git apply.

No upstream network or real overlay writes. Requires Python 3.12+ and Git.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest
import urllib.error
import urllib.request
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('libarchive_overlay', ROOT/'tools/prepare-libarchive-overlay.py')
overlay = importlib.util.module_from_spec(spec)
with patch.object(urllib.request, 'urlopen', side_effect=AssertionError('Import must not fetch')):
    spec.loader.exec_module(overlay)


def blob_hash(data):
    return hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()


def archive_bytes():
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w:xz') as archive:
        data = b'original\n'
        member = tarfile.TarInfo('libarchive-3.8.9/sample.txt')
        member.size = len(data)
        archive.addfile(member, io.BytesIO(data))
    return stream.getvalue()


def source_patch(before, after):
    return f'diff --git a/sample.txt b/sample.txt\n--- a/sample.txt\n+++ b/sample.txt\n@@ -1 +1 @@\n-{before}\n+{after}\n'.encode()


class LibarchiveOverlay(unittest.TestCase):
    def setUp(self):
        if not shutil.which('git'):
            self.fail('Git is required for actual patch verification')
        self.temporary = tempfile.TemporaryDirectory(prefix='melonds-overlay-test-')
        self.work = Path(self.temporary.name).resolve()
        self.root = self.work/'repo'
        self.vendored = self.root/'cmake/overlay-ports/libarchive'
        self.vendored.mkdir(parents=True)
        self.custom = {'fix-buildsystem.patch': b'custom first patch\n', 'fix-deps.patch': b'custom second patch\n',
                       'portfile.cmake': b'custom recipe\n', 'local-note.txt': b'generated customization\n'}
        for name, data in self.custom.items():
            (self.vendored/name).write_bytes(data)
        self.before = self.snapshot()
        self.inputs = {
            'fix-buildsystem.patch': source_patch('original', 'intermediate'),
            'fix-deps.patch': source_patch('intermediate', 'patched'),
            'portfile.cmake': b'vcpkg_from_github(\n    REF v3.8.8\n)\nkeep_all_options()\n',
            'usage': b'generated usage\n',
            'vcpkg-cmake-wrapper.cmake.in': b'generated wrapper\n',
            'vcpkg.json': b'{"name":"libarchive","version":"3.8.8","port-version":2,"features":{"keep":{}}}',
        }
        self.license = b'generated pinned vcpkg license\n'
        self.archive = archive_bytes()
        self.calls = []

    def tearDown(self):
        self.assertTrue(self.work.is_relative_to(Path(tempfile.gettempdir()).resolve()))
        self.assertTrue(self.work.name.startswith('melonds-overlay-test-'))
        self.temporary.cleanup()

    def snapshot(self):
        return {p.relative_to(self.vendored).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in self.vendored.rglob('*') if p.is_file()}

    def invoke(self, output=None, failure=None):
        pins = {name: blob_hash(data) for name, data in self.inputs.items()}
        def fetch(url, timeout):
            self.calls.append(url)
            name = url.rsplit('/', 1)[-1]
            if failure == 'network' and name == 'fix-deps.patch':
                raise urllib.error.URLError('generated second-fetch failure')
            if url == overlay.URL:
                data = self.archive
            elif name == 'LICENSE.txt':
                data = self.license
            else:
                data = self.inputs[name]
            if ((failure == 'blob' and name == 'fix-deps.patch') or
                    (failure == 'license' and name == 'LICENSE.txt') or
                    (failure == 'archive' and url == overlay.URL)):
                data += b'corrupted fixture input'
            return io.BytesIO(data)
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.multiple(overlay, ROOT=self.root, FILES=pins, LICENSE_SHA1=blob_hash(self.license),
                            SHA256=hashlib.sha256(self.archive).hexdigest()), \
                patch.object(urllib.request, 'urlopen', side_effect=fetch), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = overlay.main(['--output', str(output)] if output else [])
        return code, stdout.getvalue(), stderr.getvalue()

    def test_existing_output_preserved_before_fetch(self):
        code, _, error = self.invoke(self.vendored)
        self.assertEqual(code, 1)
        self.assertIn('Output already exists', error)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.snapshot(), self.before)
        empty = self.work/'existing-empty'
        empty.mkdir()
        self.assertEqual(self.invoke(empty)[0], 1)
        self.assertTrue(empty.is_dir())
        self.assertEqual(list(empty.iterdir()), [])
        self.assertEqual(self.calls, [])

    def test_late_fetch_and_hash_failures_publish_nothing(self):
        for failure, message in (('network', 'second-fetch failure'), ('blob', 'fix-deps.patch'),
                                 ('license', 'VCPKG-LICENSE.txt'), ('archive', 'Release archive SHA256')):
            with self.subTest(failure=failure):
                output = self.work/failure
                code, _, error = self.invoke(output, failure)
                self.assertEqual(code, 1)
                self.assertIn(message, error)
                self.assertFalse(output.exists())
                self.assertEqual(self.snapshot(), self.before)

    def test_actual_second_patch_failure_publishes_nothing(self):
        self.inputs['fix-deps.patch'] = source_patch('not-present-after-first-patch', 'patched')
        output = self.work/'bad-patch'
        code, _, error = self.invoke(output)
        self.assertEqual(code, 1)
        self.assertIn('Patch verification failed: fix-deps.patch', error)
        self.assertIn('patch does not apply', error)
        self.assertFalse(output.exists())
        self.assertEqual(self.snapshot(), self.before)

    def test_success_exact_candidate_and_check_preserves_differences(self):
        output = self.work/'candidate'
        code, stdout, error = self.invoke(output)
        self.assertEqual(code, 0, error)
        report = json.loads(stdout)
        self.assertEqual(report['vendored_differences']['changed'],
                         ['fix-buildsystem.patch', 'fix-deps.patch', 'portfile.cmake'])
        self.assertEqual(report['vendored_differences']['extra'], ['local-note.txt'])
        self.assertEqual(self.snapshot(), self.before)
        expected = dict(self.inputs, **{'VCPKG-LICENSE.txt': self.license})
        expected['vcpkg.json'] = (json.dumps({'name': 'libarchive', 'version': '3.8.9',
                                            'features': {'keep': {}}}, indent=2)+'\n').encode()
        sha512 = hashlib.sha512(self.archive).hexdigest()
        expected['portfile.cmake'] = (
            f'# Port recipe from microsoft/vcpkg@{overlay.REV}; see VCPKG-LICENSE.txt.\n'
            f'vcpkg_download_distfile(ARCHIVE\n    URLS "{overlay.URL}"\n'
            f'    FILENAME "libarchive-3.8.9.tar.xz"\n    SHA512 {sha512}\n)\n'
            'vcpkg_extract_source_archive(SOURCE_PATH\n    ARCHIVE "${ARCHIVE}"\n'
            '    PATCHES fix-buildsystem.patch fix-deps.patch\n)\nkeep_all_options()\n').encode()
        actual = {p.name: p.read_bytes() for p in output.iterdir()}
        self.assertEqual(actual, expected)
        code, stdout, error = self.invoke()
        self.assertEqual(code, 1, error)
        self.assertEqual(json.loads(stdout)['vendored_differences'], report['vendored_differences'])
        self.assertEqual(self.snapshot(), self.before)
        # A CRLF checkout of this exact candidate is not a customization.
        clean_root = self.work/'clean-repo'
        clean_overlay = clean_root/'cmake/overlay-ports/libarchive'
        clean_overlay.mkdir(parents=True)
        for name, data in expected.items():
            (clean_overlay/name).write_bytes(data.replace(b'\n', b'\r\n'))
        self.root = clean_root
        code, stdout, error = self.invoke()
        self.assertEqual(code, 0, error)
        self.assertEqual(json.loads(stdout)['vendored_differences'], {'changed': [], 'missing': [], 'extra': []})


if __name__ == '__main__':
    unittest.main()
