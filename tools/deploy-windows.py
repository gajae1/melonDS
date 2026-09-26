#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deploy a new Windows x64 runtime from an explicit MSYS2 UCRT64 installation.

Qt's dry-run plan, CMake's PE import closure and pacman's license inventory
are the only dependency inputs. Existing runtime/manifest paths are refused.
Missing package licenses fail deployment unless explicitly supplied with an
installed-version and SHA-256 bound license supplement. Failed stages remain.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run_tool(command):
    result = subprocess.run([str(arg) for arg in command], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, encoding='utf-8', errors='replace')
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr)
    if result.returncode:
        raise ValueError(f'{Path(command[0]).name} failed ({result.returncode}): {result.stdout}')
    return result.stdout


def canonical_relative(relative):
    parts = relative.split('/')
    if (any(part in ('', '.', '..') or part.endswith((' ', '.')) for part in parts) or
            re.search(r'[\x00-\x1f<>:"\\|?*;]', relative) or
            any(re.fullmatch(r'(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?', p, re.I) for p in parts)):
        raise ValueError(f'Noncanonical Windows destination: {relative!r}')
    return relative


def absolute_path(path):
    # CMake lists cannot represent semicolons in these input paths.
    path = path.absolute()
    canonical_relative('/'.join(path.parts[1:]))
    return path.resolve()


def source_file(path, base):
    if not path.is_absolute():
        raise ValueError(f'Expected absolute source path: {path}')
    absolute_path(path)
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(base) or not resolved.is_file():
        raise ValueError(f'Source must be a regular file inside {base}: {path}')
    return resolved


def add_input(plan, relative, source, expected=None):
    relative = canonical_relative(relative)
    key = relative.casefold()
    actual = digest(source) if isinstance(source, Path) else hashlib.sha256(source).hexdigest()
    if expected is not None and expected != actual:
        raise ValueError(f'License supplement hash mismatch: {relative}')
    expected = actual
    if key in plan and plan[key][2] != expected:
        raise ValueError(f'Conflicting destination contents: {relative}')
    plan.setdefault(key, (relative, source, expected))


def system_filters():
    def windows_path(api):
        buffer = ctypes.create_unicode_buffer(32768)
        length = api(buffer, len(buffer))
        if not 0 < length < len(buffer):
            raise OSError('Cannot locate Windows system directories')
        return Path(buffer.value).resolve().as_posix()

    def insensitive(path):
        return ''.join(f'[{c.lower()}{c.upper()}]' if c.isalpha() else re.escape(c) for c in path)

    system = windows_path(ctypes.windll.kernel32.GetSystemDirectoryW)
    windows = windows_path(ctypes.windll.kernel32.GetWindowsDirectoryW)
    return ['^' + insensitive(system) + '/', '^' + insensitive(windows) + '/[^/]+\\.[dD][lL][lL]$']


def qt_inputs(prefix, executable, runtime):
    raw = run_tool([prefix / 'bin/windeployqt.exe', '--release', '--dry-run', '--json',
                    '--no-translations', '--no-patchqt', '--dir', runtime, executable])
    entries = json.loads(raw)['files']
    if not isinstance(entries, list) or not entries:
        raise ValueError('Qt deployment plan has no files')
    result = []
    for entry in entries:
        source = source_file(Path(entry['source']), prefix)
        target = Path(entry['target'])
        if not target.is_absolute():
            raise ValueError('Qt target must be an absolute destination directory')
        target = absolute_path(target)
        if not target.is_relative_to(runtime):
            raise ValueError(f'Qt target escapes runtime: {target}')
        if source.suffix.lower() != '.dll':
            raise ValueError(f'Unexpected Qt deployment input for this profile: {source}')
        result.append(((target / source.name).relative_to(runtime).as_posix(), source))
    return result


def dependencies(prefix, executable, qt, scratch):
    request = {
        'prefix': prefix.as_posix(), 'executable': executable.as_posix(),
        'objdump': (prefix / 'bin/objdump.exe').as_posix(),
        'libraries': [p.as_posix() for _, p in qt if p.parent == prefix / 'bin'],
        'modules': [p.as_posix() for _, p in qt if p.parent != prefix / 'bin'],
        'system_filters': system_filters(),
    }
    input_file, output_file = scratch / 'request.json', scratch / 'dependencies.txt'
    input_file.write_text(json.dumps(request), encoding='utf-8')
    run_tool([prefix / 'bin/cmake.exe', f'-DINPUT_FILE={input_file.as_posix()}',
              f'-DOUTPUT_FILE={output_file.as_posix()}', '-P', ROOT / 'cmake/RuntimeDependencies.cmake'])
    return [source_file(Path(line), prefix) for line in output_file.read_text(encoding='utf-8').splitlines() if line]


def license_inputs(prefix, binaries, supplement):
    pacman = prefix.parent / 'usr/bin/pacman.exe'
    installed = ['/' + prefix.name + '/' + p.relative_to(prefix).as_posix() for p in sorted(set(binaries))]
    packages = sorted(set(run_tool([pacman, '-Qoq', *installed]).splitlines()))
    if not packages or any(not re.fullmatch(r'mingw-w64-ucrt-x86_64-[a-zA-Z0-9@+_.-]+', p) for p in packages):
        raise ValueError('Binary owners do not match the MSYS2 UCRT64 package profile')
    if set(supplement) - set(packages):
        raise ValueError('License supplement contains packages outside the selected runtime')
    inventories = package_inventories(pacman, packages)
    versions = package_versions(pacman, packages)
    selected, missing = [], []
    marker = '/' + prefix.name + '/share/licenses/'
    for package in packages:
        inventory = inventories[package]
        licenses = [p for p in inventory if p.startswith(marker) and not p.endswith('/')]
        # ICU installs its license beside its versioned data, not in share/licenses.
        if not licenses and package == 'mingw-w64-ucrt-x86_64-icu':
            licenses = [p for p in inventory if re.fullmatch('/' + prefix.name + r'/share/icu/[^/]+/LICENSE', p)]
        # MSYS2 splits these binaries from qt6-multimedia, which owns the texts.
        # Require the installed source-package identity/version to agree before
        # reusing that package's explicit inventory; unrelated licenses cannot fill a gap.
        provider = 'mingw-w64-ucrt-x86_64-qt6-multimedia'
        if not licenses and package == provider + '-ffmpeg' and provider in inventories:
            if package_source(prefix, package, versions[package]) != package_source(prefix, provider, versions[provider]):
                raise ValueError('Qt multimedia license provider source/version mismatch')
            licenses = [p for p in inventories[provider] if p.startswith(marker + 'qt6-multimedia/') and not p.endswith('/')]
        if not licenses and package not in supplement:
            missing.append(package)
        for path in licenses:
            installed_relative = canonical_relative(path[len('/' + prefix.name + '/'):])
            relative = canonical_relative(path[len(marker):]) if path.startswith(marker) else 'icu/LICENSE'
            source = source_file(prefix / installed_relative, prefix)
            selected.append(('third-party-licenses/' + relative, source, None))
        if package in supplement:
            entry = supplement[package]
            if entry['version'] != versions[package]:
                raise ValueError(f'License supplement installed version mismatch: {package}')
            for filename, expected in entry['files'].items():
                source = Path(filename)
                source = source_file(source, source.parent.resolve())
                selected.append((f'third-party-licenses/{package}/{source.name}', source, expected))
    if missing:
        raise ValueError('No installed license files listed by pacman for: ' + ', '.join(missing))
    return selected, packages


def package_inventories(pacman, packages):
    # One -Ql lists the same installed paths as per-package -Qlq queries, with
    # each line prefixed by the owning package name. Pacman only writes errors
    # for unknown packages to stderr, so any other line shape or owner is
    # unexpected output rather than a packaging fact.
    inventories = {package: [] for package in packages}
    for line in run_tool([pacman, '-Ql', *packages]).splitlines():
        name, separator, path = line.partition(' ')
        if not separator or not path or name not in inventories:
            raise ValueError(f'Unexpected pacman -Ql output line: {line!r}')
        inventories[name].append(path)
    return inventories


def package_versions(pacman, packages):
    versions = {}
    for line in run_tool([pacman, '-Q', *packages]).splitlines():
        identity = line.split()
        if len(identity) != 2 or identity[0] in versions:
            raise ValueError('Cannot determine installed package version')
        versions[identity[0]] = identity[1]
    if set(versions) != set(packages):
        raise ValueError('Cannot determine installed package version')
    return versions


def package_source(prefix, package, version):
    # Read only the active installed DB record, including pkgbase, which -Qi
    # does not expose. This is the default MSYS2 package database.
    database = prefix.parent / 'var/lib/pacman/local'
    path = source_file(database / f'{package}-{version}/desc', database)
    fields = dict(block.split('\n', 1) for block in path.read_text(encoding='utf-8').strip().split('\n\n'))
    if not fields.get('%BASE%') or fields.get('%VERSION%') != version or fields.get('%NAME%') != package:
        raise ValueError('Installed package source metadata does not match pacman')
    return fields['%BASE%'], fields['%VERSION%']


def load_supplement(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate license supplement key')
            result[key] = value
        return result
    document = json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)
    if (not isinstance(document, dict) or set(document) != {'schema', 'packages'} or
            type(document['schema']) is not int or document['schema'] != 1 or
            not isinstance(document['packages'], dict)):
        raise ValueError('Expected license supplement schema 1 with packages')
    for entry in document['packages'].values():
        if (not isinstance(entry, dict) or set(entry) != {'version', 'files'} or
                not isinstance(entry['version'], str) or not entry['version'] or
                not isinstance(entry['files'], dict) or not entry['files']):
            raise ValueError('License supplement needs an installed version and explicit files')
        for filename, expected in entry['files'].items():
            if not Path(filename).is_absolute() or not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected):
                raise ValueError('License supplement requires absolute paths and lowercase SHA-256 hashes')
    return document['packages']


def x64_pe(path):
    # Fast accept for ordinary x64 PE images: objdump -f's pei-x86-64 verdict is
    # decided by the MZ/PE signatures, machine 0x8664, an optional header no
    # larger than the x64 target's 0xF0 bytes, and enough file bytes for the
    # declared section table. Anything else still defers to objdump below, so
    # unusual or corrupt inputs keep the same error path and diagnostics.
    try:
        with path.open('rb') as stream:
            head = stream.read(64)
            if len(head) < 64 or head[:2] != b'MZ':
                return False
            offset = int.from_bytes(head[0x3C:0x40], 'little')
            if offset < 0x40:
                return False
            stream.seek(offset)
            header = stream.read(24)
            if len(header) < 24 or header[:4] != b'PE\0\0':
                return False
            if int.from_bytes(header[4:6], 'little') != 0x8664:
                return False
            sections = int.from_bytes(header[6:8], 'little')
            optional = int.from_bytes(header[20:22], 'little')
            if optional > 0xF0:
                return False
            stream.seek(0, os.SEEK_END)
            return stream.tell() >= offset + 24 + optional + sections * 40
    except OSError:
        return False


def copy_inputs(plan, runtime, manifest):
    files = {}
    for relative, source, expected in sorted(plan.values()):
        target = runtime / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.resolve().is_relative_to(runtime):
            raise ValueError(f'Output escapes runtime: {target}')
        with target.open('xb') as output:
            if isinstance(source, Path):
                with source.open('rb') as stream:
                    shutil.copyfileobj(stream, output)
            else:
                output.write(source)
        if digest(target) != expected:
            raise ValueError(f'Copy hash mismatch: {relative}')
        files[relative] = expected
    # This scan only verifies the already selected names; it never selects inputs.
    actual = {p.relative_to(runtime).as_posix() for p in runtime.rglob('*') if p.is_file()}
    if actual != set(files):
        raise ValueError('Final runtime contains missing or unselected files')
    for relative, expected in files.items():
        if digest(source_file(runtime / relative, runtime)) != expected:
            raise ValueError(f'Final runtime hash mismatch: {relative}')
    # Windows rename will not replace an existing destination. Publish only the
    # complete verified manifest, and never remove anything from a failed stage.
    temporary = manifest.with_name(manifest.name + '.tmp')
    with temporary.open('x', encoding='utf-8') as stream:
        json.dump({'schema': 1, 'files': files}, stream, indent=2)
        stream.write('\n')
    temporary.rename(manifest)


def deploy(build, runtime, prefix, manifest, license_supplement=None):
    if os.name != 'nt' or prefix.name.lower() != 'ucrt64':
        raise ValueError('Only the Windows x64 MSYS2 UCRT64 profile is supported')
    # lexists also protects dangling aliases, before resolving any output path.
    for path in (runtime, manifest, manifest.with_name(manifest.name + '.tmp')):
        if os.path.lexists(path):
            raise ValueError(f'Output already exists; nothing was overwritten: {path}')
    build, runtime, prefix, manifest = map(absolute_path, (build, runtime, prefix, manifest))
    if (manifest.is_relative_to(runtime) or runtime.is_relative_to(prefix) or
            manifest.is_relative_to(prefix) or prefix.is_relative_to(runtime) or build.is_relative_to(runtime)):
        raise ValueError('Outputs must be separate from SDK/input files, with the manifest outside runtime')
    if not runtime.parent.is_dir() or not manifest.parent.is_dir():
        raise ValueError('Runtime and manifest parent directories must already exist')
    executable = source_file(build / 'melonDS.exe', build)
    version_file = source_file(build / 'release-version.txt', build)
    version = version_file.read_text(encoding='utf-8').strip()
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]{2,}', version):
        raise ValueError('Expected a numeric dotted release version such as 1.1.44')
    exe_name = f'{version}-melonDS.exe'
    supplement = load_supplement(license_supplement) if license_supplement else {}
    for tool in ('windeployqt.exe', 'cmake.exe', 'objdump.exe'):
        if not (prefix / 'bin' / tool).is_file():
            raise ValueError(f'Missing MSYS2 tool: {tool}')
    runtime.mkdir()  # Exclusive reservation: even an empty pre-existing directory is refused.
    try:
        qt = qt_inputs(prefix, executable, runtime)
        with tempfile.TemporaryDirectory(prefix='melonds-deploy-', dir=runtime.parent) as directory:
            closure = dependencies(prefix, executable, qt, Path(directory))
        plan = {}
        add_input(plan, exe_name, executable)
        for relative, source in qt:
            add_input(plan, relative, source)
        for source in closure:
            add_input(plan, source.name, source)
        binaries = {source for _, source, _ in plan.values()}
        for binary in sorted(binaries):
            if not (x64_pe(binary) or
                    re.search(r'file format pei-x86-64\b', run_tool([prefix / 'bin/objdump.exe', '-f', binary]))):
                raise ValueError(f'Expected an x64 PE binary: {binary}')
        licenses, packages = license_inputs(prefix, binaries - {executable}, supplement)
        for relative, source, expected in licenses:
            add_input(plan, relative, source, expected)
        add_input(plan, 'LICENSE', source_file(ROOT / 'LICENSE', ROOT))
        add_input(plan, 'third-party-licenses/miniaudio/LICENSE',
                  source_file(ROOT / 'src/miniaudio/LICENSE', ROOT))
        add_input(plan, 'third-party-licenses/rubberband/COPYING',
                  source_file(ROOT / 'src/rubberband/COPYING', ROOT))
        add_input(plan, 'third-party-licenses/r8brain/LICENSE',
                  source_file(ROOT / 'src/audio_interpolation/LICENSE.r8brain', ROOT))
        add_input(plan, 'release-version.txt', version_file)
        add_input(plan, 'README.txt', (f'melonDS Windows runtime\r\nRun {exe_name} or run-melonDS.cmd.\r\n'
                  'No games, BIOS, firmware, saves or user settings are included.\r\n'
                  'Third-party licenses are in third-party-licenses.\r\n').encode('ascii'))
        add_input(plan, 'run-melonDS.cmd', f'@echo off\r\nsetlocal\r\ncd /d "%~dp0"\r\n"%~dp0{exe_name}" %*\r\n'.encode('ascii'))
        copy_inputs(plan, runtime, manifest)
        return {'runtime': str(runtime), 'runtime_manifest': str(manifest),
                'files': len(plan), 'license_packages': packages}
    except Exception:
        print(f'Deployment failed; new partial directory retained: {runtime}', file=sys.stderr)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path)
    parser.add_argument('runtime_dir', type=Path)
    parser.add_argument('--msys-prefix', type=Path, required=True)
    parser.add_argument('--runtime-manifest', type=Path, required=True)
    parser.add_argument('--license-supplement', type=Path,
                        help='Optional schema-1 packages map: exact installed version, files {absolute path: SHA256}')
    args = parser.parse_args(argv)
    try:
        report = deploy(args.build_dir, args.runtime_dir, args.msys_prefix, args.runtime_manifest, args.license_supplement)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f'Deployment error: {exc}', file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
