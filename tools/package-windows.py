#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Archive an already deployed Windows runtime and its committed source.

The runtime directory may contain local game data. It is read only; portable
settings, firmware, ROMs and saves are excluded from the distributable archive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import source_identity


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def private(path):
    return ('portable' in (part.lower() for part in path.parts)
            or path.suffix.lower() in {
                '.bin', '.toml', '.ini', '.sav', '.nds', '.dsi', '.srl', '.gba',
                '.bak', *(f'.ml{i}' for i in range(10))})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path, help='CMake build with release-version.txt')
    parser.add_argument('runtime_dir', type=Path, help='Deployed EXE, DLLs, plugins and licenses')
    parser.add_argument('--output-dir', type=Path, default=Path('build'))
    parser.add_argument('--arch', choices=('x64', 'arm64'), default='x64')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    runtime = args.runtime_dir.resolve()
    output = args.output_dir.resolve()
    version = (build / 'release-version.txt').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d{2,}', version):
        parser.error('Expected a display version such as 1.1.01')
    # Source ZIPs must describe the binary's final, reviewable source revision.
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=root):
        parser.error('Commit source changes before packaging')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    exe_name = f'{version}-melonDS.exe'
    exe = runtime / exe_name
    if not exe.is_file():
        exe = runtime / 'melonDS.exe'
    if digest(exe) != digest(build / 'melonDS.exe'):
        parser.error('Deployed EXE differs from the selected build')
    files = sorted(p for p in runtime.rglob('*') if p.is_file())
    private_before = {p: digest(p) for p in files if private(p.relative_to(runtime))}
    env = os.environ.copy()
    system_root = os.environ['SystemRoot']
    env['PATH'] = os.pathsep.join((str(Path(system_root) / 'System32'), system_root))
    help_result = subprocess.run([str(exe), '--help'], cwd=runtime, env=env,
                                 capture_output=True, text=True, errors='replace', timeout=30)
    if help_result.returncode or 'Usage:' not in help_result.stdout + help_result.stderr:
        raise RuntimeError(f'Deployed EXE cannot start with a clean PATH: {help_result.returncode}')
    try:
        info_result = subprocess.run([str(exe), '--build-info'], cwd=runtime, env=env,
                                     capture_output=True, timeout=30)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError('Deployed EXE --build-info timed out (pre-identity build?)') from exc
    if info_result.returncode:
        raise RuntimeError(f'Deployed EXE --build-info failed: {info_result.returncode}')
    try:
        build_info = source_identity.parse_build_info(info_result.stdout.decode('utf-8'))
        verified_source_id = source_identity.verify_build_info(
            build_info, root, expected_version=version)
    except source_identity.SourceIdentityError as exc:
        raise RuntimeError(f'Deployed EXE source identity rejected: {exc}') from exc
    output.mkdir(parents=True, exist_ok=True)
    runtime_zip = output / f'{version}-melonDS-windows-{args.arch}.zip'
    runtime_temp = runtime_zip.with_suffix('.zip.tmp')
    with zipfile.ZipFile(runtime_temp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in files:
            relative = path.relative_to(runtime)
            if path in private_before or path.suffix.lower() == '.exe' and path != exe:
                continue
            archive.write(path, exe_name if path == exe else relative.as_posix())
    with zipfile.ZipFile(runtime_temp) as archive:
        if archive.testzip() or any(private(Path(name)) for name in archive.namelist()):
            raise RuntimeError('Runtime archive validation failed')
        if hashlib.sha256(archive.read(exe_name)).hexdigest() != digest(exe):
            raise RuntimeError('Archived EXE differs from the build')
        runtime_files = len(archive.namelist())
    source_zip = output / f'{version}-melonDS-source.zip'
    source_temp = source_zip.with_suffix('.zip.tmp')
    subprocess.run(['git', 'archive', '--format=zip', '--output=' + str(source_temp), commit],
                   cwd=root, check=True)
    with zipfile.ZipFile(source_temp) as archive:
        if archive.testzip() or any(private(Path(name)) for name in archive.namelist()):
            raise RuntimeError('Source archive validation failed')
    if any(digest(path) != value for path, value in private_before.items()):
        raise RuntimeError('Local game data changed during packaging')
    runtime_temp.replace(runtime_zip)
    source_temp.replace(source_zip)
    report = {
        'version': version, 'source_commit': commit, 'exe_sha256': digest(exe),
        'package_identity_schema': build_info['schema'],
        'package_identity_source_id': verified_source_id,
        'package_identity_version': build_info['version'],
        'package_identity_normalization': 'Git check-in filters; core.autocrlf=input; committed HEAD attributes',
        'package_identity_docs_only_excludes': source_identity.DOC_EXCLUDED_POLICY,
        'runtime_zip': runtime_zip.name, 'runtime_bytes': runtime_zip.stat().st_size,
        'runtime_sha256': digest(runtime_zip), 'runtime_files': runtime_files,
        'source_zip': source_zip.name, 'source_bytes': source_zip.stat().st_size,
        'source_sha256': digest(source_zip), 'preserved_private_files': len(private_before),
        'private_files_in_zip': 0, 'clean_path_help_exit': help_result.returncode,
    }
    (output / f'{version}-melonDS-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
