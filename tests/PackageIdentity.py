#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Integrated scenario test for the melonDS package source identity.

Builds a tiny real CMake/C++ binary in a temporary owned Git repository and
drives the production cmake/PackageIdentity.cmake and
tools/source_identity.py through the packaging lifecycle: clean build,
stale-binary rejection after a source commit, docs-only commits keeping the
identity, untracked and deleted files tracked without reconfiguring, dirty
edits rejected until committed, CRLF-equivalent content hashing equal, and
the OFF default embedding an empty identity.

Run directly, no test framework required:
    python tests/PackageIdentity.py
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / 'tools'))
import source_identity


MINI_MAIN = r'''#include <cstdio>

#include "package_identity.h"

int main()
{
    std::printf("{\"schema\":%d,\"source_id\":\"%s\",\"version\":\"9.9.90\"}\n",
                MELONDS_PACKAGE_IDENTITY_SCHEMA, MELONDS_PACKAGE_IDENTITY_SOURCE_ID);
    return 0;
}
'''


def mini_cmake(package_identity_cmake):
    package_identity_cmake = Path(package_identity_cmake).as_posix()
    return f'''cmake_minimum_required(VERSION 3.13)
project(identity_mini CXX)

include("{package_identity_cmake}")

add_executable(identity_mini main.cpp)
melonds_add_package_identity(identity_mini)
'''


def run(argv, cwd, env=None, check=True):
    result = subprocess.run(argv, cwd=str(cwd), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and result.returncode:
        raise AssertionError(
            f'command failed ({result.returncode}): {argv}\n'
            f'stdout: {result.stdout.decode("utf-8", "replace")}\n'
            f'stderr: {result.stderr.decode("utf-8", "replace")}')
    return result


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def pick_generator():
    if shutil.which('ninja'):
        return ['-G', 'Ninja']
    if shutil.which('mingw32-make'):
        return ['-G', 'MinGW Makefiles']
    return ['-G', 'Unix Makefiles']


def scenario(work):
    repo = work / 'repo'
    build = work / 'build'
    repo.mkdir()

    # Hermetic Git configuration: no system/user config, no global excludes.
    excludes = work / 'git-excludes'
    excludes.write_bytes(b'')
    global_config = work / 'git-config'
    global_config.write_bytes(b'')
    git_env = dict(os.environ,
                   GIT_CONFIG_NOSYSTEM='1',
                   GIT_CONFIG_GLOBAL=str(global_config))
    run(['git', 'init', '--quiet'], repo, env=git_env)
    for key, value in (
        ('user.name', 'Identity Test'),
        ('user.email', 'identity@test.invalid'),
        ('commit.gpgsign', 'false'),
        ('core.excludesFile', str(excludes)),
    ):
        run(['git', 'config', key, value], repo, env=git_env)

    def git(*args):
        return run(['git', *args], repo, env=git_env)

    (repo / '.gitattributes').write_text('* text=auto\n', encoding='utf-8', newline='\n')
    (repo / 'source.txt').write_text('identity source A\n', encoding='utf-8', newline='\n')
    (repo / 'my file.txt').write_text('spaced path\n', encoding='utf-8', newline='\n')
    (repo / '데이터.txt').write_text('데이터\n', encoding='utf-8', newline='\n')
    (repo / 'main.cpp').write_text(MINI_MAIN, encoding='utf-8', newline='\n')
    (repo / 'CMakeLists.txt').write_text(
        mini_cmake(str(ROOT / 'cmake' / 'PackageIdentity.cmake')),
        encoding='utf-8', newline='\n')
    git('add', '-A')
    git('commit', '--quiet', '-m', 'source A')

    def configure(identity='ON'):
        run(['cmake', '-S', str(repo), '-B', str(build), *pick_generator(),
             f'-DMELONDS_PACKAGE_IDENTITY={identity}',
             f'-DPython3_EXECUTABLE={sys.executable}'], work)

    def build_project():
        run(['cmake', '--build', str(build), '--parallel', '2'], work)

    exe = build / ('identity_mini.exe' if os.name == 'nt' else 'identity_mini')

    def exe_info():
        return source_identity.parse_build_info(run([str(exe)], work).stdout.decode('utf-8'))

    def expect_rejected(info, why):
        try:
            source_identity.verify_build_info(info, repo, expected_version='9.9.90')
        except source_identity.SourceIdentityError:
            return
        raise AssertionError(f'verification should have been rejected: {why}')

    # Clean source A: embedded, working and committed identities all agree.
    configure()
    build_project()
    info_a = exe_info()
    id_a = info_a['source_id']
    check(re.fullmatch('[0-9a-f]{64}', id_a), f'expected a 64-hex identity, got {id_a!r}')
    check(id_a == source_identity.working_source_identity(repo),
          'EXE identity differs from the working identity (source A)')
    check(id_a == source_identity.verify_build_info(info_a, repo, expected_version='9.9.90'),
          'clean tree: working identity should equal the committed identity')

    # Commit source B without rebuilding: the stale binary must be rejected.
    (repo / 'source.txt').write_text('identity source B\n', encoding='utf-8', newline='\n')
    git('add', '-A')
    git('commit', '--quiet', '-m', 'source B')
    expect_rejected(exe_info(), 'stale EXE after a source commit')
    build_project()
    info_b = exe_info()
    check(info_b['source_id'] == source_identity.verify_build_info(
        info_b, repo, expected_version='9.9.90'), 'rebuilt EXE should verify for source B')
    check(info_b['source_id'] != id_a, 'a source change must change the identity')

    # Documentation-only commits keep the identity and do not relink.
    (repo / 'README.md').write_text('docs only\n', encoding='utf-8', newline='\n')
    (repo / 'plans').mkdir()
    (repo / 'plans' / 'notes.md').write_text('plan notes\n', encoding='utf-8', newline='\n')
    git('add', '-A')
    git('commit', '--quiet', '-m', 'docs only')
    check(source_identity.verify_build_info(exe_info(), repo, expected_version='9.9.90')
          == info_b['source_id'], 'docs-only commit must keep the committed identity')
    exe_bytes = exe.read_bytes()
    build_project()
    check(exe.read_bytes() == exe_bytes,
          'docs-only commit must not rewrite the binary')

    # Nonignored untracked files join the identity without reconfiguring.
    (repo / 'untracked_source.txt').write_text('late arrival\n', encoding='utf-8', newline='\n')
    build_project()
    info_u = exe_info()
    check(info_u['source_id'] == source_identity.working_source_identity(repo),
          'untracked file must be included without reconfiguring')
    check(info_u['source_id'] != info_b['source_id'],
          'an untracked source file must change the identity')
    git('add', '-A')
    git('commit', '--quiet', '-m', 'late arrival')
    check(source_identity.verify_build_info(exe_info(), repo, expected_version='9.9.90')
          == info_u['source_id'],
          'committing the already-hashed untracked file must keep the identity')

    # Tracked deletions leave the identity without reconfiguring.
    git('rm', '--quiet', '--', 'my file.txt')
    build_project()
    info_d = exe_info()
    check(info_d['source_id'] == source_identity.working_source_identity(repo),
          'deleted tracked file must leave the working identity')
    check(info_d['source_id'] != info_u['source_id'],
          'deleting a tracked file must change the identity')
    git('commit', '--quiet', '-m', 'remove spaced file')

    # Dirty content may be built, but never verifies as the committed source.
    (repo / '데이터.txt').write_text('데이터 B\r\n', encoding='utf-8', newline='')
    build_project()
    info_dirty = exe_info()
    check(info_dirty['source_id'] == source_identity.working_source_identity(repo),
          'dirty edit must produce its own working identity')
    expect_rejected(info_dirty, 'dirty tree against the committed identity')
    git('add', '-A')
    git('commit', '--quiet', '-m', 'dirty edit committed')
    check(source_identity.verify_build_info(exe_info(), repo, expected_version='9.9.90')
          == info_dirty['source_id'],
          'committing the exact dirty content must verify without rebuilding')

    # Git's EOL check-in normalization: CRLF working copy == committed LF.
    (repo / 'source.txt').write_bytes(b'identity source B\r\n')
    check(source_identity.working_source_identity(repo)
          == source_identity.committed_source_identity(repo),
          'CRLF-equivalent working copy must hash to the committed identity')
    build_project()
    check(exe_info()['source_id'] == source_identity.committed_source_identity(repo),
          'CRLF rewrite must keep the binary identity stable')

    # Default OFF: empty identity, no Python/Git requirement at build time,
    # and the empty identity never passes packaging verification.
    configure(identity='OFF')
    build_project()
    info_off = exe_info()
    check(info_off['source_id'] == '', 'OFF build must embed an empty identity')
    expect_rejected(info_off, 'identity-less OFF build')


def main():
    if not shutil.which('cmake'):
        raise AssertionError('cmake not found on PATH')
    work = Path(tempfile.mkdtemp(prefix='melonds-identity-'))
    try:
        scenario(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('PackageIdentity: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
