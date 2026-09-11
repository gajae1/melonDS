#!/usr/bin/env python3
"""Check the vendored overlay against verified libarchive 3.8.9 inputs.

This is an explicit maintenance operation, never run during normal configure.
Default: verify pins/patches and report vendored differences without writing it.
Use --output NEW_DIRECTORY to publish a verified candidate for manual review.
Existing outputs are always refused; updating the vendored overlay is manual.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REV = '04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4'
VERSION = '3.8.9'
# LICENSE.txt blob from the same pinned vcpkg commit's root Git tree.
LICENSE_SHA1 = '4d23e0e39b2531c41499e7b1c5ec0efffd15c6b6'
FILES = {
    'fix-buildsystem.patch':'9588acceebb9cb2faa66b1b8769e5350dc79eaa2',
    'fix-deps.patch':'54376115163cabcb75a9d7be63f7a7d3306ef8ce',
    'portfile.cmake':'501f3a80ed3343d325044c462d8ec0bdbb2e9812',
    'usage':'213f642d2d940780e57de357781a66294a3783ce',
    'vcpkg-cmake-wrapper.cmake.in':'e5c965e984ed706aecea16912e73ba27a94ad0f6',
    'vcpkg.json':'ea3dc96d81eec35ba9ca36d5b7fa78a56c5dd7df',
}
URL = 'https://github.com/libarchive/libarchive/releases/download/v3.8.9/libarchive-3.8.9.tar.xz'
SHA256 = '888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde'

def get(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()

def verified_blob(url, expected, name):
    data = get(url)
    actual = hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()
    if actual != expected:
        raise RuntimeError(f'Upstream port hash mismatch: {name}')
    return data


def candidate_inputs():
    files = {name: verified_blob(
        f'https://raw.githubusercontent.com/microsoft/vcpkg/{REV}/ports/libarchive/{name}', expected, name)
        for name, expected in FILES.items()}
    files['VCPKG-LICENSE.txt'] = verified_blob(
        f'https://raw.githubusercontent.com/microsoft/vcpkg/{REV}/LICENSE.txt', LICENSE_SHA1, 'VCPKG-LICENSE.txt')
    archive = get(URL)
    if hashlib.sha256(archive).hexdigest() != SHA256:
        raise RuntimeError('Release archive SHA256 mismatch')
    sha512 = hashlib.sha512(archive).hexdigest()
    port = files['portfile.cmake'].decode('utf-8')
    if not port.startswith('vcpkg_from_github('):
        raise RuntimeError('Unexpected pinned port source declaration')
    end = port.index('\n)')+2
    port = f'''# Port recipe from microsoft/vcpkg@{REV}; see VCPKG-LICENSE.txt.
vcpkg_download_distfile(ARCHIVE
    URLS "{URL}"
    FILENAME "libarchive-{VERSION}.tar.xz"
    SHA512 {sha512}
)
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${{ARCHIVE}}"
    PATCHES fix-buildsystem.patch fix-deps.patch
)'''+port[end:]
    files['portfile.cmake'] = port.encode('utf-8')
    manifest = json.loads(files['vcpkg.json'])
    manifest['version'] = VERSION
    manifest.pop('port-version', None)
    files['vcpkg.json'] = (json.dumps(manifest, indent=2)+'\n').encode('utf-8')
    return files, archive, sha512


def verify_patches(stage, candidate, archive):
    path = stage/'release.tar.xz'
    path.write_bytes(archive)
    unpacked = stage/'source'
    unpacked.mkdir()
    with tarfile.open(path) as tar:
        tar.extractall(unpacked, filter='data')
    source = unpacked/f'libarchive-{VERSION}'
    if not source.is_dir():
        raise RuntimeError('Release archive is missing its expected source directory')
    # A staging directory may live below this repository. Prevent git apply
    # from discovering the parent checkout and skipping paths outside its prefix.
    env = {key: value for key, value in os.environ.items()
           if key not in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR')}
    env['GIT_CEILING_DIRECTORIES'] = str(unpacked)
    for name in ('fix-buildsystem.patch', 'fix-deps.patch'):
        for flags in (['--check'], []):
            result = subprocess.run(['git', 'apply', *flags, str(candidate/name)], cwd=source,
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if result.returncode:
                raise RuntimeError(f'Patch verification failed: {name}\n' + result.stderr.decode('utf-8', 'replace'))


def differences(vendored, files):
    # These seven inputs are text files; checkout CRLF is not a vendored change.
    missing, changed = [], []
    for name, data in sorted(files.items()):
        path = vendored/name
        if not path.is_file():
            missing.append(name)
        elif path.read_bytes().replace(b'\r\n', b'\n') != data.replace(b'\r\n', b'\n'):
            changed.append(name)
    extra = sorted(p.relative_to(vendored).as_posix() for p in vendored.rglob('*')
                   if p.is_file() and p.relative_to(vendored).as_posix() not in files)
    return {'changed': changed, 'missing': missing, 'extra': extra}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Publish to a new directory; never replace an existing overlay')
    args = parser.parse_args(argv)
    output = args.output.absolute() if args.output is not None else None
    try:
        if output is not None:
            if os.path.lexists(output):
                raise RuntimeError(f'Output already exists; preserved: {output}')
            output = output.resolve()
            if not output.parent.is_dir():
                raise RuntimeError('Output parent directory must already exist')
        # All fetches, hashes and real patch application precede publication.
        with tempfile.TemporaryDirectory(prefix='melonds-libarchive-', dir=output.parent if output else None) as directory:
            stage = Path(directory).resolve()
            files, archive, sha512 = candidate_inputs()
            candidate = stage/'overlay'
            candidate.mkdir()
            for name, data in files.items():
                (candidate/name).write_bytes(data)
            verify_patches(stage, candidate, archive)
            diff = differences(ROOT/'cmake/overlay-ports/libarchive', files)
            if output is not None:
                if os.path.lexists(output):
                    raise RuntimeError(f'Output appeared during verification; preserved: {output}')
                candidate.rename(output)
        print(json.dumps({'version': VERSION, 'registry_commit': REV, 'release_sha256': SHA256,
                          'release_sha512': sha512, 'output': str(output) if output else None,
                          'vendored_differences': diff}, indent=2))
        return 0 if output is not None or not any(diff.values()) else 1
    except (OSError, RuntimeError, ValueError, tarfile.TarError) as exc:
        print(f'Overlay verification failed; no candidate published: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
