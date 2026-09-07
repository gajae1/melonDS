#!/usr/bin/env python3
"""CI-only current-toolchain/full-frontend build; no game/accuracy claims.

Run from a clean checkout in a disposable runner with native SDKs installed.
CMake 4.4.3 and aqtinstall 3.3.0 must be installed in this Python environment.
Linux/Windows download verified LLVM archives; macOS uses Homebrew LLVM.
"""
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
TEMP = Path(os.environ['RUNNER_TEMP']) / 'melonds-current'
TEMP.mkdir(parents=True, exist_ok=True)
VCPKG = '04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4'
LLVM = '23.1.0'
QT = '6.11.2'
SYSTEM = platform.system()

def run(*args, cwd=ROOT, capture=False):
    print('+', ' '.join(map(str,args)), flush=True)
    return subprocess.run(list(map(str,args)), cwd=cwd, check=True,
                          text=True, stdout=subprocess.PIPE if capture else None)

def download(url, dest, digest):
    print('Download:', url, flush=True)
    with urllib.request.urlopen(url, timeout=120) as response, dest.open('wb') as output:
        shutil.copyfileobj(response, output)
    with dest.open('rb') as source:
        actual = hashlib.file_digest(source, 'sha256').hexdigest()
    if actual != digest:
        raise RuntimeError(f'SHA256 mismatch for {dest.name}: {actual}')

def prepend(path):
    os.environ['PATH'] = str(path) + os.pathsep + os.environ['PATH']

if SYSTEM == 'Linux':
    name = f'LLVM-{LLVM}-Linux-X64.tar.zst'
    digest = '1a3dd8e99df9fe6f5b3b9c62a4a81e0c57da9b95b1889a6dbf6c304c264e756f'
    archive = TEMP / name
    download(f'https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM}/{name}', archive, digest)
    llvm = TEMP / 'llvm'; llvm.mkdir(exist_ok=True)
    run('tar', '--zstd', '-xf', archive, '--strip-components=1', '-C', llvm)
    archive.unlink()
    prepend(llvm / 'bin')
    host, arch, triplet = 'linux', 'linux_gcc_64', 'x64-linux-release'
elif SYSTEM == 'Windows':
    installer = TEMP / 'llvm.msi'
    download(f'https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM}/LLVM-{LLVM}-win64.msi', installer,
             '95602d03944a7db534899adc5203988b63921ad0db71fe15748a3fe3d340416f')
    llvm = TEMP / 'llvm'
    result = subprocess.run(['msiexec.exe','/i',str(installer),'/qn','/norestart',f'INSTALLDIR={llvm}'], check=False)
    if result.returncode not in (0,3010):
        raise RuntimeError(f'LLVM installer failed: {result.returncode}')
    # Official MSI versions may use their default installation directory.
    if not (llvm/'bin'/'clang.exe').is_file():
        llvm = Path(os.environ['ProgramFiles'])/'LLVM'
    prepend(llvm/'bin')
    host, arch, triplet = 'windows', 'win64_msvc2022_64', 'x64-windows'
elif SYSTEM == 'Darwin':
    llvm = Path(run('brew','--prefix','llvm',capture=True).stdout.strip())
    prepend(llvm/'bin')
    host, arch = 'mac', 'clang_64'
    triplet = 'arm64-osx-13-release' if platform.machine() == 'arm64' else 'x64-osx-13-release'
else:
    raise RuntimeError(f'Unconfigured runner OS: {SYSTEM}')

cc, cxx = shutil.which('clang'), shutil.which('clang++')
if not cc or not cxx:
    raise RuntimeError('LLVM binaries were not installed')
version = run(cxx,'--version',capture=True).stdout
print(version, flush=True)
if f'clang version {LLVM}' not in version:
    raise RuntimeError('This verification batch requires the pinned LLVM version')
run('cmake','--version')
os.environ['CC'], os.environ['CXX'] = cc, cxx
os.environ['VCPKG_DISABLE_METRICS'] = '1'

# Pin the registry checkout rather than tracking an unbounded moving branch.
vcpkg = ROOT/'vcpkg'
if not (vcpkg/'.git').exists():
    vcpkg.mkdir(exist_ok=True)
    run('git','init',vcpkg)
    run('git','remote','add','origin','https://github.com/microsoft/vcpkg.git',cwd=vcpkg)
run('git','fetch','--depth=1','origin',VCPKG,cwd=vcpkg)
run('git','checkout','--detach',VCPKG,cwd=vcpkg)
if SYSTEM == 'Windows':
    run('cmd','/c',vcpkg/'bootstrap-vcpkg.bat','-disableMetrics')
else:
    run('sh',vcpkg/'bootstrap-vcpkg.sh','-disableMetrics')

# Qt binaries avoid recompiling the same dependency for every source edit.
run(sys.executable,'-m','aqt','install-qt',host,'desktop',QT,arch,
    '-m','qtmultimedia','-O',TEMP/'Qt')
candidates = list((TEMP/'Qt'/QT).glob('*/lib/cmake/Qt6/Qt6Config.cmake'))
if len(candidates) != 1:
    raise RuntimeError(f'Expected one Qt installation, got {candidates}')
qt_prefix = candidates[0].parents[3]
common = ['-G','Ninja','-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_C_COMPILER={cc}',
          f'-DCMAKE_CXX_COMPILER={cxx}',f'-DCMAKE_ASM_COMPILER={cc}',
          '-DMELONDS_CURRENT_TOOLCHAIN=ON','-DCMAKE_CXX_SCAN_FOR_MODULES=OFF']
if SYSTEM != 'Darwin':
    common += ['-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld']
if SYSTEM == 'Windows':
    common += ['-DCMAKE_RC_COMPILER=llvm-rc.exe']

dep_args = ['-DUSE_VCPKG=ON',f'-DVCPKG_ROOT={vcpkg}',
            f'-DVCPKG_TARGET_TRIPLET={triplet}',f'-DVCPKG_HOST_TRIPLET={triplet}',
            '-DBUILD_STATIC=OFF','-DMELONDS_USE_EXTERNAL_QT=ON',
            f'-DCMAKE_PREFIX_PATH={qt_prefix}']
# Windows core headers require dirent from the resolved dependency set.
core_deps = dep_args if SYSTEM == 'Windows' else []
# Real headless core execution and a separate ASan/UBSan boundary suite.
run('cmake','-S',ROOT,'-B','build-current-core',*common,*core_deps,
    '-DBUILD_QT_SDL=OFF','-DENABLE_OGLRENDERER=OFF','-DMELONDS_BUILD_TESTS=ON','-DENABLE_AVX512=ON')
run('cmake','--build','build-current-core','--parallel','3')
run('ctest','--test-dir','build-current-core','--output-on-failure')
if SYSTEM == 'Linux':
    run('cmake','-S','tests','-B','build-current-sanitized',*common,
        '-DCMAKE_BUILD_TYPE=Debug',
        '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer',
        '-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -fsanitize=address,undefined','-DENABLE_AVX512=ON')
    run('cmake','--build','build-current-sanitized','--parallel','3')
    run('ctest','--test-dir','build-current-sanitized','--output-on-failure')

run('cmake','-S',ROOT,'-B','build-current-app',*common,*dep_args,
    '-DENABLE_WAYLAND=OFF','-DMELONDS_BUILD_TESTS=OFF','-DENABLE_AVX512=OFF')
run('cmake','--build','build-current-app','--parallel','3')
# Record the resolved dependency graph, never infer versions merely from a pin.
status = ROOT/'build-current-app'/'vcpkg_installed'/'vcpkg'/'status'
report = ROOT/'current-build-versions.txt'
report.write_text(version+'\nQt '+QT+'\nvcpkg '+VCPKG+'\n'+(status.read_text() if status.exists() else 'No vcpkg status file\n'))
