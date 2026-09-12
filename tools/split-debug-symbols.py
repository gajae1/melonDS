#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in separate symbols for unstripped Windows GNU x64 PE executables.

Requires Python 3.11+ and an installed MSYS2 UCRT64 binutils prefix. Creates a
new output directory only; never strips the input. A failed new directory is
retained without a success manifest. No runtime DLLs are copied.
"""
from __future__ import annotations

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
import zlib

from source_identity import SourceIdentityError, parse_build_info


def sha(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args, env) -> str:
    result = subprocess.run([str(arg) for arg in args], env=env, capture_output=True,
                            text=True, encoding='utf-8', errors='replace', timeout=60)
    if result.returncode:
        raise ValueError(f'{Path(args[0]).name} exited {result.returncode}: '
                         f'{result.stderr.strip() or result.stdout.strip()}')
    return result.stdout


def sections(objdump: Path, executable: Path, env) -> dict[str, int]:
    output = run([objdump, '-h', executable], env)
    if not re.search(r'file format pei-x86-64\b', output):
        raise ValueError('Unsupported input: expected a Windows x64 PE executable with GNU DWARF')
    return {name: int(size, 16) for name, size in
            re.findall(r'^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s', output, re.M)}


def require_debug(section_sizes):
    if not all(section_sizes.get(name, 0) for name in ('.debug_info', '.debug_line')):
        raise ValueError('Input has no supported DWARF .debug_info/.debug_line; use an unstripped -g build')


def split(executable: Path, output: Path, prefix: Path, build_info: bool = False) -> dict:
    if os.name != 'nt':
        raise ValueError('Supported profile is native Windows GNU x64 PE (MSYS2 UCRT64) only')
    # Check the supplied name before resolving it, including dangling aliases.
    if os.path.lexists(output):
        raise ValueError(f'Output already exists; choose a new directory: {output}')
    output = output.absolute()
    output = output.parent.resolve(strict=True) / output.name
    executable = executable.resolve(strict=True)
    prefix = prefix.resolve(strict=True)
    if not executable.is_file() or executable.suffix.lower() != '.exe':
        raise ValueError('Input must be a linked .exe file')
    if prefix.name.lower() != 'ucrt64':
        raise ValueError('Unsupported tool profile: supply an MSYS2 UCRT64 prefix')
    env = dict(os.environ, LC_ALL='C',
               PATH=os.pathsep.join((str(executable.parent), str(prefix / 'bin'), os.environ.get('PATH', ''))))
    tools = {name: prefix / 'bin' / f'{name}.exe' for name in ('objcopy', 'strip', 'objdump')}
    versions = {}
    for name, tool in tools.items():
        version = run([tool, '--version'], env).splitlines()
        if not version or not version[0].startswith(f'GNU {name} '):
            raise ValueError(f'Expected GNU {name}: {tool}')
        versions[name] = version[0]
    original_hash = sha(executable)
    require_debug(sections(tools['objdump'], executable, env))
    info = parse_build_info(run([executable, '--build-info'], env)) if build_info else None

    output.mkdir()  # Exclusive reservation, including an existing empty directory.
    try:
        runnable = output / executable.name
        debug = output / (executable.name + '.debug')
        shutil.copyfile(executable, runnable)
        if sha(runnable) != original_hash:
            raise ValueError('Input changed while copying')
        run([tools['objcopy'], '--only-keep-debug', runnable, debug], env)
        require_debug(sections(tools['objdump'], debug, env))
        run([tools['strip'], '--strip-debug', runnable], env)
        # Any previous debuglink belonged to the input's old companion.
        run([tools['objcopy'], '--remove-section=.gnu_debuglink',
             f'--add-gnu-debuglink={debug}', runnable], env)
        remaining = sections(tools['objdump'], runnable, env)
        if any(name.startswith(('.debug_', '.zdebug_')) for name in remaining):
            raise ValueError('Debug sections remain in the output executable')

        link_path = output / 'debuglink-check.bin'
        run([tools['objcopy'], '--dump-section', f'.gnu_debuglink={link_path}', runnable], env)
        link = link_path.read_bytes()
        link_path.unlink()
        crc = 0
        with debug.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                crc = zlib.crc32(chunk, crc)
        filename = debug.name.encode('utf-8') + b'\0'
        expected_link = filename + b'\0' * (-len(filename) % 4) + struct.pack('<I', crc)
        if link != expected_link:
            raise ValueError('Output GNU debuglink filename/checksum does not match the debug file')
        if build_info and parse_build_info(run([runnable, '--build-info'], env)) != info:
            raise ValueError('Build info changed after separating symbols')
        if sha(executable) != original_hash:
            raise ValueError('Input changed during symbol separation')

        manifest = {
            'schema': 1,
            'profile': 'windows-gnu-x86_64-dwarf',
            'input': {'name': executable.name, 'sha256': original_hash},
            'executable': {'name': runnable.name, 'sha256': sha(runnable)},
            'debug': {'name': debug.name, 'sha256': sha(debug)},
            'debuglink_crc32': f'{crc:08x}',
            # Reported metadata, not a claim that the current checkout matches.
            'build_info': info,
            'tools': versions,
        }
        temporary = output / 'debug-symbols.json.tmp'
        with temporary.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(manifest, stream, indent=2)
            stream.write('\n')
        temporary.rename(output / 'debug-symbols.json')
        return manifest
    except Exception:
        print(f'Symbol separation failed; new partial output retained: {output}', file=sys.stderr)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input_exe', type=Path)
    parser.add_argument('output_dir', type=Path, help='Must not exist; its parent must exist')
    parser.add_argument('--msys-prefix', type=Path, required=True)
    parser.add_argument('--build-info', action='store_true',
                        help='Execute both EXEs with --build-info and require matching melonDS JSON; '
                             'otherwise do not execute the input and record build_info: null')
    args = parser.parse_args(argv)
    try:
        manifest = split(args.input_exe, args.output_dir, args.msys_prefix, args.build_info)
    except (OSError, ValueError, SourceIdentityError, subprocess.TimeoutExpired) as exc:
        print(f'Debug symbols error: {exc}', file=sys.stderr)
        return 1
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
