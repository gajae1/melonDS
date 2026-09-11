#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production release version policy and optional Windows PE resources.

Run directly with Python; --output-dir preserves the generated fixtures and JSON
receipt in a new directory instead of using an automatically removed temp dir.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
CASES = [
    ('0.0.0', '0.0.00', [0, 0, 0, 0]),
    ('1.1.9', '1.1.09', [1, 1, 9, 0]),
    ('1.1.09', '1.1.09', [1, 1, 9, 0]),
    ('1.1.10', '1.1.10', [1, 1, 10, 0]),
    ('1.1.99', '1.1.99', [1, 1, 99, 0]),
    ('1.1.100', '1.1.100', [1, 1, 100, 0]),
    ('1.1.0191', '1.1.0191', [1, 1, 191, 0]),
    ('1.1.65535', '1.1.65535', [1, 1, 65535, 0]),
    ('1.1.65536', None, None),
]
VERSION_READER = r'''param([string]$ExePath)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$v = (Get-Item -LiteralPath $ExePath).VersionInfo
@{
    FileVersion = $v.FileVersion
    FileVersionRaw = $v.FileVersionRaw.ToString()
    ProductVersion = $v.ProductVersion
    ProductVersionRaw = $v.ProductVersionRaw.ToString()
} | ConvertTo-Json -Compress
'''


def file_info(path):
    data = path.read_bytes()
    return {'path': path.as_posix(), 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(argv, cwd, case, name, deadline):
    remaining = deadline - time.monotonic()
    check(remaining > 0, 'PackageVersion exceeded its 110-second execution budget')
    record = {'argv': [str(arg) for arg in argv], 'cwd': cwd.as_posix()}
    case['commands'][name] = record
    started = time.monotonic()
    try:
        result = subprocess.run(record['argv'], cwd=cwd, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=min(60, remaining))
    except (OSError, subprocess.TimeoutExpired) as error:
        record.update(exit_code=None, error=str(error))
        raise
    finally:
        record['elapsed_seconds'] = time.monotonic() - started
    record.update(exit_code=result.returncode,
                  stdout=result.stdout.decode('utf-8', 'replace'),
                  stderr=result.stderr.decode('utf-8', 'replace'))
    for stream in ('stdout', 'stderr'):
        log = cwd / (name + '.' + stream + '.log')
        log.write_bytes(getattr(result, stream))
        record[stream + '_log'] = file_info(log)
    return record


def scenario(work):
    module = ROOT / 'cmake/ReleaseVersion.cmake'
    template = ROOT / 'res/melon.rc.in'
    icon = ROOT / 'res/melon.ico'
    receipt = {'schema': 1, 'invocation': [sys.executable, *sys.argv],
               'inputs': [file_info(p) for p in (Path(__file__), module, template, icon)],
               'cases': [], 'pe_cases_verified': 0, 'passed': False}
    deadline = time.monotonic() + 110
    try:
        cmake = shutil.which('cmake')
        check(cmake, 'cmake not found on PATH')
        ninja = shutil.which('ninja')
        generator = ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + Path(ninja).as_posix()] if ninja else []
        windres = shutil.which('windres') if os.name == 'nt' else None
        gcc = powershell = None
        if windres:
            sibling_gcc = Path(windres).with_name('gcc.exe')
            gcc = str(sibling_gcc) if sibling_gcc.is_file() else shutil.which('gcc')
            powershell = shutil.which('pwsh') or shutil.which('powershell')
            check(gcc and powershell, 'windres found, but GCC and PowerShell are required for PE verification')
            receipt['pe_scope'] = 'windres resource compile, tiny EXE link, PowerShell VersionInfo; no EXE execution'
        else:
            receipt['pe_scope'] = ('SKIPPED: windres unavailable on Windows; CMake/resource text only, no PE verification'
                                   if os.name == 'nt' else 'Not Windows: CMake/resource text only, no PE verification')
        receipt['tools'] = {name: file_info(Path(path)) for name, path in
                            [('cmake', cmake), ('ninja', ninja), ('windres', windres),
                             ('gcc', gcc), ('powershell', powershell)] if path}
        reader = work / 'read-version.ps1'
        reader.write_text(VERSION_READER, encoding='utf-8')

        for version, display, numeric in CASES:
            fixture = work / version
            src, build = fixture / 'src', fixture / 'build'
            (src / 'res').mkdir(parents=True)
            build.mkdir()
            shutil.copyfile(icon, src / 'res/melon.ico')
            (src / 'main.c').write_text('int main(void) { return 0; }\n', encoding='ascii')
            (src / 'values.json.in').write_text(
                '{"display":"@MELONDS_VERSION_DISPLAY@","rc":"@MELON_RC_VERSION@"}\n', encoding='utf-8')
            (src / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(melonDS VERSION {version} LANGUAGES NONE)
include("{module.as_posix()}")
set(MINGW 0)
configure_file("{template.as_posix()}" "${{CMAKE_BINARY_DIR}}/res/melon.rc")
configure_file("${{CMAKE_SOURCE_DIR}}/values.json.in" "${{CMAKE_BINARY_DIR}}/values.json" @ONLY)
''', encoding='utf-8')
            case = {'version': version, 'expected_display': display, 'expected_numeric': numeric,
                    'commands': {}, 'passed': False}
            receipt['cases'].append(case)
            configured = run([cmake, '-S', src, '-B', build, *generator], fixture, case, 'configure', deadline)
            if display is None:
                diagnostic = ' '.join(configured['stderr'].split())
                check(configured['exit_code'] != 0 and 'WORD range (0..65535)' in diagnostic
                      and 'PATCH' in diagnostic and '65536' in diagnostic,
                      '1.1.65536 must fail configure with an explicit Windows WORD range error')
                case['passed'] = True
                print(f'{version}: PASS (expected WORD range rejection)', flush=True)
                continue

            check(configured['exit_code'] == 0, f'{version}: configure failed: {configured["stderr"]}')
            observed = json.loads((build / 'values.json').read_text(encoding='utf-8'))
            case['cmake_values'] = observed
            rc_version = ','.join(str(part) for part in numeric)
            check(observed == {'display': display, 'rc': rc_version}, f'{version}: unexpected CMake values {observed}')
            resource = build / 'res/melon.rc'
            resource_text = resource.read_text(encoding='utf-8')
            check(f'FILEVERSION {rc_version}\n' in resource_text and
                  f'PRODUCTVERSION {rc_version}\n' in resource_text and
                  f'VALUE "FileVersion", "{display}"' in resource_text,
                  f'{version}: generated RC differs from expected values')
            if windres:
                obj, exe = build / 'resource.o', build / 'version-probe.exe'
                compiled = run([windres, '-i', resource, '-O', 'coff', '-o', obj], fixture, case, 'windres', deadline)
                check(compiled['exit_code'] == 0 and 'digit exceeds base' not in compiled['stderr'],
                      f'{version}: resource compilation failed or used an invalid numeric base: {compiled["stderr"]}')
                linked = run([gcc, src / 'main.c', obj, '-o', exe], fixture, case, 'link', deadline)
                check(linked['exit_code'] == 0, f'{version}: link failed: {linked["stderr"]}')
                inspected = run([powershell, '-NoProfile', '-NonInteractive', '-File', reader, '-ExePath', exe],
                                fixture, case, 'version-info', deadline)
                check(inspected['exit_code'] == 0, f'{version}: VersionInfo failed: {inspected["stderr"]}')
                pe = json.loads(inspected['stdout'].lstrip('\ufeff'))
                case['version_info'] = pe
                raw_version = '.'.join(str(part) for part in numeric)
                check(pe == {'FileVersion': display, 'FileVersionRaw': raw_version,
                             'ProductVersion': display, 'ProductVersionRaw': raw_version},
                      f'{version}: unexpected PE version {pe}')
                receipt['pe_cases_verified'] += 1
            case['passed'] = True
            print(f'{version}: PASS ({display}; {rc_version})', flush=True)
        receipt['passed'] = True
        print(f'PackageVersion: PASS ({len(CASES)} boundaries; {receipt["pe_cases_verified"]} PE checks). '
              + receipt['pe_scope'], flush=True)
    except Exception as error:
        receipt['error'] = str(error)
        raise
    finally:
        receipt['artifacts'] = [file_info(path) for path in sorted(work.rglob('*')) if path.is_file()]
        (work / 'result.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, help='new directory to retain fixtures and JSON receipt')
    args = parser.parse_args()
    if args.output_dir:
        work = args.output_dir.resolve()
        work.mkdir(parents=True, exist_ok=False)
        scenario(work)
    else:
        with tempfile.TemporaryDirectory(prefix='melonds-package-version-') as temporary:
            scenario(Path(temporary).resolve())
    return 0


if __name__ == '__main__':
    sys.exit(main())
