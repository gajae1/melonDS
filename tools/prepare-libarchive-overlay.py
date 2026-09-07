#!/usr/bin/env python3
"""Reproduce the 3.8.9 overlay from a hash-verified vcpkg port and release.

This is an explicit maintenance operation, never run during normal configure.
All other port options and dependency features are retained from the registry.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REV = '04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4'
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

out = ROOT/'cmake'/'overlay-ports'/'libarchive'
out.mkdir(parents=True,exist_ok=True)
for name, expected in FILES.items():
    data = get(f'https://raw.githubusercontent.com/microsoft/vcpkg/{REV}/ports/libarchive/{name}')
    actual = hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()
    if actual != expected:
        raise RuntimeError(f'Upstream port hash mismatch: {name}')
    (out/name).write_bytes(data)
(out/'VCPKG-LICENSE.txt').write_bytes(get(f'https://raw.githubusercontent.com/microsoft/vcpkg/{REV}/LICENSE.txt'))
archive = get(URL)
if hashlib.sha256(archive).hexdigest() != SHA256:
    raise RuntimeError('Release archive SHA256 mismatch')
sha512 = hashlib.sha512(archive).hexdigest()
port = (out/'portfile.cmake').read_text()
end = port.index('\n)')+2
port = f'''# Port recipe from microsoft/vcpkg@{REV}; see VCPKG-LICENSE.txt.
vcpkg_download_distfile(ARCHIVE
    URLS "{URL}"
    FILENAME "libarchive-3.8.9.tar.xz"
    SHA512 {sha512}
)
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${{ARCHIVE}}"
    PATCHES fix-buildsystem.patch fix-deps.patch
)'''+port[end:]
(out/'portfile.cmake').write_text(port)
manifest = json.loads((out/'vcpkg.json').read_text())
manifest['version']='3.8.9'
manifest.pop('port-version',None)
(out/'vcpkg.json').write_text(json.dumps(manifest,indent=2)+'\n')
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    path = root/'release.tar.xz';path.write_bytes(archive)
    with tarfile.open(path) as tar:
        tar.extractall(root,filter='data')
    source = root/'libarchive-3.8.9'
    for name in ('fix-buildsystem.patch','fix-deps.patch'):
        subprocess.run(['git','apply','--check',str(out/name)],cwd=source,check=True)
        subprocess.run(['git','apply',str(out/name)],cwd=source,check=True)
print('libarchive 3.8.9 release and inherited patches verified:',sha512)
