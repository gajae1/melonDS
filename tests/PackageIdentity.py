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
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / 'tools'))
import source_identity


MINI_MAIN = r'''#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "package_identity.h"

int main(int argc, char** argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--help"))
    {
        std::puts("Usage: generated identity fixture [--help|--build-info]");
        return 0;
    }
    // Deterministic concurrent-writer control, after the packager's initial
    // manifest validation but before it reads the files into the ZIP.
    if (argc == 2 && !std::strcmp(argv[1], "--build-info"))
        if (const char* path = std::getenv("PACKAGE_TEST_CHANGE_FILE"))
        {
            auto* file = std::fopen(path, "wb");
            if (!file) return 3;
            std::fputs("changed during packaging", file);
            std::fclose(file);
        }
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


def packaging_scenario(work, repo, build, exe, env):
    runtime = work / 'runtime'
    runtime.mkdir()
    shutil.copy2(exe, build / 'melonDS.exe')
    shutil.copy2(exe, runtime / 'melonDS.exe')
    (build / 'release-version.txt').write_text('9.9.90\n', encoding='utf-8')
    # Explicit trusted deploy inputs, never inferred from a runtime scan.
    deployed = {
        'Qt6Core.dll': b'generated dependency placeholder',
        'plugins/platforms/qwindows.dll': b'generated platform plugin',
        'plugins/imageformats/qjpeg.dll': b'generated image plugin',
        'licenses/qt/LICENSE.LGPL3': b'generated license placeholder',
        'licenses/sdl/COPYING.txt': b'generated license placeholder',
    }
    extra = {
        'user-notes.txt': b'synthetic user note',
        'user-profile.json': b'{"synthetic":true}',
        'screenshots/user-photo.jpg': b'synthetic jpg-named data',
        'plugins/platforms/user-note.txt': b'synthetic non-plugin',
        'licenses/qt/user-note.json': b'synthetic non-license',
        'portable/note.txt': b'synthetic portable data',
        'game.nds': b'synthetic ROM-named data',
        'settings.toml': b'synthetic settings-named data',
        'bios.bin': b'synthetic BIOS-named data',
        'other.exe': b'synthetic unselected EXE',
    }
    for rel, data in (deployed | extra).items():
        path = runtime / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    def sha(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()
    trusted = {'melonDS.exe': sha(exe), **{
        rel: hashlib.sha256(data).hexdigest() for rel, data in deployed.items()}}
    manifest = work / 'runtime-manifest.json'
    manifest.write_text(json.dumps({'schema': 1, 'files': trusted}), encoding='utf-8')
    command = [sys.executable, str(repo / 'tools/package-windows.py'), str(build), str(runtime)]
    def package(output, manifest_path=manifest, environment=env):
        return run([*command, '--runtime-manifest', str(manifest_path), '--output-dir', str(output)],
                   repo, env=environment, check=False)
    before = {p.relative_to(runtime): sha(p) for p in runtime.rglob('*') if p.is_file()}
    omitted = run([*command, '--output-dir', str(work / 'omitted-manifest')], repo, env=env, check=False)
    normal = package(work / 'package')
    print(f'Packaging CLI: omitted-manifest exit={omitted.returncode}; valid-manifest exit={normal.returncode}', flush=True)
    check(normal.returncode == 0, 'valid runtime manifest must package successfully: ' + normal.stderr.decode('utf-8', 'replace'))
    check(omitted.returncode != 0 and b'--runtime-manifest' in omitted.stderr,
          'runtime manifest must be required')
    with zipfile.ZipFile(work / 'package/9.9.90-melonDS-windows-x64.zip') as archive:
        expected = {'9.9.90-melonDS.exe': sha(exe), **{rel: trusted[rel] for rel in deployed}}
        check(set(archive.namelist()) == set(expected), 'ZIP must contain exactly trusted deploy inputs')
        check(all(hashlib.sha256(archive.read(rel)).hexdigest() == value for rel, value in expected.items()),
              'ZIP byte hashes must match trusted inputs')
    report = json.loads(normal.stdout)
    check(report['excluded_runtime_files'] == len(extra) and report['excluded_known_private_files'] == 4,
          'report must distinguish all exclusions from known private-file rules')
    check(report['known_private_files_in_zip'] == 0 and report['runtime_manifest_sha256'] == sha(manifest),
          'receipt must bind the validated selection')
    check(b'10' in normal.stderr and b'excluded' in normal.stderr.lower() and
          all(rel.encode() not in normal.stderr for rel in extra), 'exclusion warning uses counts, not private filenames')
    check(before == {p.relative_to(runtime): sha(p) for p in runtime.rglob('*') if p.is_file()},
          'normal packaging preserves every generated runtime file')

    if os.name == 'nt':
        mixed_files = dict(trusted)
        mixed_files['Qt6Core.DLL'] = mixed_files.pop('Qt6Core.dll')
        mixed_manifest = work / 'mixed-case-manifest.json'
        mixed_manifest.write_text(json.dumps({'schema': 1, 'files': mixed_files}), encoding='utf-8')
        mixed = package(work / 'mixed-case-package', mixed_manifest)
        check(mixed.returncode == 0, 'Windows manifest may use a different case for the same file')
        with zipfile.ZipFile(work / 'mixed-case-package/9.9.90-melonDS-windows-x64.zip') as archive:
            mixed_expected = dict(expected)
            mixed_expected['Qt6Core.DLL'] = mixed_expected.pop('Qt6Core.dll')
            check(set(archive.namelist()) == set(mixed_expected) and
                  all(hashlib.sha256(archive.read(rel)).hexdigest() == value
                      for rel, value in mixed_expected.items()),
                  'mixed-case ZIP must preserve manifest names and bytes')
        mixed_report = json.loads(mixed.stdout)
        print(f"Packaging CLI: mixed-case excluded={mixed_report['excluded_runtime_files']}; expected={len(extra)}", flush=True)
        check(mixed_report['excluded_runtime_files'] == len(extra) and
              mixed_report['excluded_known_private_files'] == 4 and b'excluded 10 runtime files' in mixed.stderr,
              'Windows case aliases already in the ZIP must not count as excluded files')

    rejected_output = work / 'rejected-package'
    def reject_manifest(document, why):
        bad = work / 'invalid-manifest.json'
        bad.write_text(document if isinstance(document, str) else json.dumps(document), encoding='utf-8')
        result = package(rejected_output, bad)
        check(result.returncode != 0 and b'manifest' in result.stderr.lower(), why)
        check(not rejected_output.exists(), 'invalid input must fail before creating output')
    # Schema, aliases, and unsafe Windows/relative path spellings are one input
    # validation group. No separate test infrastructure for each separator.
    valid_json = json.dumps({'schema': 1, 'files': trusted})
    for document in [[], {'schema': True, 'files': trusted}, {'schema': 2, 'files': trusted},
                     {'schema': 1, 'files': []},
                     valid_json.replace('"schema": 1', '"schema": 1, "schema": 1'),
                     valid_json.replace('"files": {', '"files": {"Qt6Core.dll": "' + trusted['Qt6Core.dll'] + '",'),
                     {'schema': 1, 'files': {**trusted, 'qt6core.DLL': trusted['Qt6Core.dll']}},
                     {'schema': 1, 'files': {'Qt6Core.dll': trusted['Qt6Core.dll']}}]:
        reject_manifest(document, 'invalid schema/duplicate/alias/missing main EXE must be refused')
    for rel in ('../outside.dll', '/outside.dll', 'C:/outside.dll', 'C:outside.dll',
                'plugins\\q.dll', 'Qt6Core.dll:stream', 'plugins/./q.dll', 'q.dll.',
                'portable/note.txt', 'bios.bin', 'other.exe'):
        expected_hash = hashlib.sha256(extra[rel]).hexdigest() if rel in extra else 'a'*64
        reject_manifest({'schema': 1, 'files': {**trusted, rel: expected_hash}}, 'unsafe manifest path must be refused')
    for value in ('A'*64, 'not-a-sha256', '0'*64):
        reject_manifest({'schema': 1, 'files': {**trusted, 'Qt6Core.dll': value}}, 'invalid or mismatched file hash')
    dll = runtime / 'Qt6Core.dll'
    dll.unlink()
    reject_manifest({'schema': 1, 'files': trusted}, 'missing deployed dependency')
    dll.write_bytes(b'changed before packaging')
    reject_manifest({'schema': 1, 'files': trusted}, 'changed deployed dependency')
    dll.write_bytes(deployed['Qt6Core.dll'])
    outside = work / 'outside.dll'
    outside.write_bytes(b'generated outside-runtime file')
    link = runtime / 'outside.dll'
    try:
        link.symlink_to(outside)
    except (OSError, NotImplementedError) as exc:
        print(f'Packaging containment symlink fixture unavailable: {exc}', flush=True)
    else:
        reject_manifest({'schema': 1, 'files': {**trusted, 'outside.dll': sha(outside)}},
                        'manifest file resolving outside runtime')
        link.unlink()

    result = package(runtime / 'unsafe-output')
    check(result.returncode != 0 and not (runtime / 'unsafe-output').exists(), 'output cannot be inside runtime')
    collision = work / 'manifest-output'
    collision.mkdir()
    colliding_manifest = collision / '9.9.90-melonDS-manifest.json'
    colliding_manifest.write_bytes(manifest.read_bytes())
    result = package(collision, colliding_manifest)
    check(result.returncode != 0 and colliding_manifest.read_bytes() == manifest.read_bytes(),
          'output cannot replace input manifest')
    alias = work / 'alias-output'
    alias.mkdir()
    os.link(runtime / 'user-notes.txt', alias / '9.9.90-melonDS-windows-x64.zip.tmp')
    result = package(alias)
    check(result.returncode != 0 and (runtime / 'user-notes.txt').read_bytes() == extra['user-notes.txt'],
          'output hardlink cannot overwrite runtime data')

    writer_output = work / 'writer-package'
    result = package(writer_output, environment=dict(env, PACKAGE_TEST_CHANGE_FILE=str(dll)))
    check(result.returncode != 0 and b'archive' in result.stderr.lower(), 'ZIP must reject bytes changed after initial validation')
    check(not list(writer_output.glob('*.zip')), 'concurrent-writer failure must not publish archives')
    dll.write_bytes(deployed['Qt6Core.dll'])
    print('Packaging CLI: exact selection, exclusions, invalid inputs, output collisions and writer checks PASS', flush=True)
    return lambda: package(work / 'stale-package')


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

    # Match the production repository: importing the CLI's Python modules may
    # create bytecode, which is runtime cache rather than new source content.
    (repo / '.gitignore').write_text('__pycache__/\n', encoding='utf-8', newline='\n')
    (repo / '.gitattributes').write_text('* text=auto\nlegacy.txt -text\n', encoding='utf-8', newline='\n')
    (repo / 'legacy.txt').write_bytes(b'historical source A\r\n')
    (repo / 'source.txt').write_text('identity source A\n', encoding='utf-8', newline='\n')
    (repo / 'my file.txt').write_text('spaced path\n', encoding='utf-8', newline='\n')
    (repo / '데이터.txt').write_text('데이터\n', encoding='utf-8', newline='\n')
    (repo / 'main.cpp').write_text(MINI_MAIN, encoding='utf-8', newline='\n')
    (repo / 'CMakeLists.txt').write_text(
        mini_cmake(str(ROOT / 'cmake' / 'PackageIdentity.cmake')),
        encoding='utf-8', newline='\n')
    (repo / 'tools').mkdir()
    for name in ('package-windows.py', 'source_identity.py'):
        shutil.copyfile(ROOT / 'tools' / name, repo / 'tools' / name)
    git('add', '-A')
    git('commit', '--quiet', '-m', 'source A')
    # Older repositories can contain CRLF blobs committed before a text policy.
    # Changing attributes does not retroactively normalize those stored blobs.
    (repo / '.gitattributes').write_text('* text=auto\n', encoding='utf-8', newline='\n')
    git('add', '--', '.gitattributes')
    git('commit', '--quiet', '-m', 'enable text normalization without rewriting history')
    check(b'\r\n' in git('show', 'HEAD:legacy.txt').stdout, 'legacy fixture must retain committed CRLF')

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
    stale_package = packaging_scenario(work, repo, build, exe, git_env)

    # Commit source B without rebuilding: the stale binary must be rejected.
    (repo / 'source.txt').write_text('identity source B\n', encoding='utf-8', newline='\n')
    git('add', '-A')
    git('commit', '--quiet', '-m', 'source B')
    rejected = stale_package()
    check(rejected.returncode != 0 and b'source identity rejected' in rejected.stderr,
          'packaging CLI must reject stale EXE after source commit')
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

    # A real edit to an old CRLF blob still changes the identity and verifies
    # only after committing; historical EOL handling must not hide data changes.
    (repo / 'legacy.txt').write_bytes(b'historical source B\r\n')
    build_project()
    legacy_info = exe_info()
    expect_rejected(legacy_info, 'edited historical CRLF blob before commit')
    git('add', '--', 'legacy.txt')
    git('commit', '--quiet', '-m', 'edit historical CRLF blob')
    check(source_identity.verify_build_info(legacy_info, repo, expected_version='9.9.90')
          == legacy_info['source_id'], 'edited historical CRLF blob must verify after commit')
    # A HEAD identity must use HEAD attributes even while working attributes differ.
    committed = source_identity.committed_source_identity(repo)
    (repo / '.gitattributes').write_text('* -text\n', encoding='utf-8', newline='\n')
    check(source_identity.committed_source_identity(repo) == committed,
          'working attributes must not redefine the committed identity')
    (repo / '.gitattributes').write_text('* text=auto\n', encoding='utf-8', newline='\n')

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
        check(work.resolve().is_relative_to(Path(tempfile.gettempdir()).resolve()) and
              work.name.startswith('melonds-identity-'), 'cleanup must stay inside the generated temp root')
        shutil.rmtree(work, ignore_errors=True)
    print('PackageIdentity: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
