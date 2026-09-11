#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Archive an already deployed Windows runtime and its committed source.

The runtime directory may contain local game data. Only the exact paths and
hashes in a trusted deployment manifest are eligible for the runtime archive.
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


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON key in runtime manifest')
        result[key] = value
    return result


def runtime_file(runtime, relative):
    path = (runtime / relative).resolve(strict=True)
    if not path.is_relative_to(runtime) or not path.is_file():
        raise ValueError('Runtime manifest file must resolve to a regular file inside runtime')
    return path


def load_runtime_manifest(path, runtime, exe_name):
    raw = path.read_bytes()
    manifest = json.loads(raw.decode('utf-8'), object_pairs_hook=unique_object)
    if (not isinstance(manifest, dict) or set(manifest) != {'schema', 'files'} or
            type(manifest['schema']) is not int or manifest['schema'] != 1 or
            not isinstance(manifest['files'], dict)):
        raise ValueError('Expected runtime manifest schema 1 with a files object')
    files = manifest['files']
    aliases = set()
    for relative, expected in files.items():
        parts = relative.split('/')
        if (any(part in ('', '.', '..') or part.endswith((' ', '.')) for part in parts) or
                re.search(r'[\x00-\x1f<>:"\\|?*]', relative) or
                any(re.fullmatch(r'(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?', part, re.I) for part in parts)):
            raise ValueError('Runtime manifest requires canonical relative POSIX paths valid on Windows')
        if relative.casefold() in aliases:
            raise ValueError('Case-insensitive path alias in runtime manifest')
        aliases.add(relative.casefold())
        if private(Path(relative)) or (Path(relative).suffix.lower() == '.exe' and relative != exe_name):
            raise ValueError('Runtime manifest cannot include private paths or another EXE')
        if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-f]{64}', expected):
            raise ValueError('Runtime manifest requires lowercase SHA-256 hashes')
    if exe_name not in files:
        raise ValueError('Runtime manifest must include the selected main EXE')
    for relative, expected in files.items():
        if digest(runtime_file(runtime, relative)) != expected:
            raise ValueError('Runtime manifest file hash mismatch')
    return files, hashlib.sha256(raw).hexdigest()


def validate_outputs(paths, runtime, manifest, runtime_files):
    protected = [manifest, *runtime_files]
    for path in paths:
        resolved = path.resolve()
        if resolved.is_relative_to(runtime) or resolved == manifest.resolve():
            raise ValueError('Package output cannot overwrite runtime or the input manifest')
        if path.exists() and any(p.exists() and path.samefile(p) for p in protected):
            raise ValueError('Package output aliases runtime or the input manifest')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path, help='CMake build with release-version.txt')
    parser.add_argument('runtime_dir', type=Path, help='Deployed EXE, DLLs, plugins and licenses')
    parser.add_argument('--runtime-manifest', required=True, type=Path,
                        help='Trusted deployment path/SHA-256 list (UTF-8 JSON schema 1)')
    parser.add_argument('--output-dir', type=Path, default=Path('build'))
    parser.add_argument('--arch', choices=('x64', 'arm64'), default='x64')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    runtime = args.runtime_dir.resolve()
    output = args.output_dir.resolve()
    manifest_path = args.runtime_manifest.resolve()
    version = (build / 'release-version.txt').read_text(encoding='utf-8').strip()
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
    runtime_zip = output / f'{version}-melonDS-windows-{args.arch}.zip'
    runtime_temp = runtime_zip.with_suffix('.zip.tmp')
    source_zip = output / f'{version}-melonDS-source.zip'
    source_temp = source_zip.with_suffix('.zip.tmp')
    report_path = output / f'{version}-melonDS-manifest.json'
    # Inspect names/metadata only; unlisted user files are never opened to hash
    # their contents. The scan is for exclusion counts and output alias checks.
    runtime_files = sorted(p for p in runtime.rglob('*') if p.is_file())
    try:
        validate_outputs([runtime_zip, runtime_temp, source_zip, source_temp, report_path],
                         runtime, manifest_path, runtime_files)
        files, manifest_hash = load_runtime_manifest(manifest_path, runtime, exe.name)
    except (OSError, ValueError) as exc:
        parser.error(f'Runtime manifest/output validation failed: {exc}')
    if digest(exe) != digest(build / 'melonDS.exe'):
        parser.error('Deployed EXE differs from the selected build')
    selected_paths = {os.path.normcase(relative) for relative in files}
    excluded = [p.relative_to(runtime) for p in runtime_files
                if os.path.normcase(p.relative_to(runtime).as_posix()) not in selected_paths]
    known_private = sum(private(p) for p in excluded)
    if excluded:
        print(f'Warning: excluded {len(excluded)} runtime files not listed in the runtime manifest '
              f'({known_private} match known private-file rules).', file=sys.stderr)
    env = os.environ.copy()
    if os.name == 'nt':
        system_root = os.environ['SystemRoot']
        env['PATH'] = os.pathsep.join((str(Path(system_root) / 'System32'), system_root))
    else:
        env['PATH'] = os.defpath
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
    expected_archive = {exe_name if name == exe.name else name: value for name, value in files.items()}
    with zipfile.ZipFile(runtime_temp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for relative in sorted(files):
            archive.write(runtime_file(runtime, relative), exe_name if relative == exe.name else relative)
    with zipfile.ZipFile(runtime_temp) as archive:
        names = archive.namelist()
        if len(names) != len(expected_archive) or set(names) != set(expected_archive) or archive.testzip():
            raise RuntimeError('Runtime archive does not match the manifest selection')
        for name, expected in expected_archive.items():
            with archive.open(name) as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != expected:
                    raise RuntimeError('Runtime archive file hash differs from the manifest')
        archived_private = sum(private(Path(name)) for name in names)
    subprocess.run(['git', 'archive', '--format=zip', '--output=' + str(source_temp), commit],
                   cwd=root, check=True)
    with zipfile.ZipFile(source_temp) as archive:
        if archive.testzip() or any(private(Path(name)) for name in archive.namelist()):
            raise RuntimeError('Source archive validation failed')
    runtime_temp.replace(runtime_zip)
    source_temp.replace(source_zip)
    report = {
        'version': version, 'source_commit': commit, 'exe_sha256': files[exe.name],
        'package_identity_schema': build_info['schema'],
        'package_identity_source_id': verified_source_id,
        'package_identity_version': build_info['version'],
        'package_identity_normalization': 'Git check-in filters; core.autocrlf=input; committed HEAD attributes',
        'package_identity_docs_only_excludes': source_identity.DOC_EXCLUDED_POLICY,
        'runtime_zip': runtime_zip.name, 'runtime_bytes': runtime_zip.stat().st_size,
        'runtime_sha256': digest(runtime_zip), 'runtime_files': len(expected_archive),
        'runtime_manifest_sha256': manifest_hash,
        'excluded_runtime_files': len(excluded), 'excluded_known_private_files': known_private,
        'known_private_files_in_zip': archived_private,
        'source_zip': source_zip.name, 'source_bytes': source_zip.stat().st_size,
        'source_sha256': digest(source_zip), 'clean_path_help_exit': help_result.returncode,
    }
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
