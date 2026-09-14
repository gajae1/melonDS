#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real GCC profiles and stale-input rejection in an owned fixture."""
import argparse
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--repo", type=Path, required=True)
parser.add_argument("--work", type=Path, required=True)
parser.add_argument("--cmake", required=True)
parser.add_argument("--cc", required=True)
parser.add_argument("--cxx", required=True)
parser.add_argument("--ninja", required=True)
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
work = Path(tempfile.mkdtemp(prefix="pgo-", dir=args.work)).resolve()


def run(command, expected_error=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90)
    output = result.stdout + result.stderr
    if expected_error:
        if result.returncode == 0 or expected_error not in output:
            raise AssertionError((command, result.returncode, output))
    elif result.returncode:
        raise AssertionError((command, result.returncode, output))
    return output


try:
    repo = work / "source"
    for directory in ("src", "cmake", "tools"):
        (repo / directory).mkdir(parents=True)
    for name in ("tools/pgo.py", "tools/source_identity.py", "cmake/PGO.cmake"):
        shutil.copyfile(args.repo / name, repo / name)
    (repo / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.25)
project(PGOFixture LANGUAGES C CXX)
include(cmake/PGO.cmake)
add_subdirectory(src)
add_executable(runner main.cpp)
target_link_libraries(runner PRIVATE core)
melonds_configure_pgo(core)
''')
    (repo / "src/CMakeLists.txt").write_text('''add_library(core STATIC hot.cpp cold.cpp)
set(CASE_SWITCH 0 CACHE STRING "Fixture compile option")
target_compile_definitions(core PRIVATE CASE_SWITCH=${CASE_SWITCH})
''')
    hot = repo / "src/hot.cpp"
    hot.write_text("int hot(int n) { int sum=0; for(int i=0;i<n;++i) sum += (i&1)?2*i:i; return sum+CASE_SWITCH; }\n")
    (repo / "src/cold.cpp").write_text("extern const int never_referenced[2]={1,2};\n")
    (repo / "main.cpp").write_text('#include <cstdio>\n#include <cstdlib>\nint hot(int); int main(int n,char**v){ if(n!=2)return 2; std::printf("%d\\n",hot(std::atoi(v[1]))); }\n')
    run(["git", "init", "-q", repo])
    run(["git", "-C", repo, "add", "."])
    run(["git", "-C", repo, "-c", "user.name=PGO test", "-c", "user.email=pgo@example.invalid", "commit", "-qm", "fixture"])
    profile = work / "profile"
    train, use = work / "train", work / "use"
    base = [args.cmake, "-S", repo, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=" + args.cc, "-DCMAKE_CXX_COMPILER=" + args.cxx,
            "-DCMAKE_MAKE_PROGRAM=" + args.ninja, "-DMELONDS_PGO_DIR=" + str(profile)]
    run([*base, "-B", train, "-DMELONDS_PGO=GENERATE"])
    run([args.cmake, "--build", train])
    exe = "runner.exe" if sys.platform == "win32" else "runner"
    assert run([train / exe, "10"]).strip() == "70"
    run([sys.executable, repo / "tools/pgo.py", "seal", "--build", train])
    manifest_path = profile / "profile.json"
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    assert manifest["sealed"] and manifest["files"] and len(manifest["files"]) < len(manifest["commands"])
    run([*base, "-B", use, "-DMELONDS_PGO=USE"])
    run([args.cmake, "--build", use])
    assert run([use / exe, "10"]).strip() == "70"
    assert run([use / exe, "7"]).strip() == "30"  # Not used during training.
    # Build-time guards matter even if configure already succeeded.
    old = hot.read_text()
    hot.write_text(old + "// changed source\n")
    run([args.cmake, "--build", use], "PGO source or compiler changed")
    hot.write_text(old)
    run([*base, "-B", use, "-DMELONDS_PGO=USE", "-DCASE_SWITCH=1"])
    run([args.cmake, "--build", use], "PGO compile commands changed")
    run([*base, "-B", use, "-DMELONDS_PGO=USE", "-DCASE_SWITCH=0"])
    datum = profile / "data" / next(iter(manifest["files"]))
    before = datum.read_bytes()
    datum.write_bytes(before + b"changed")
    run([args.cmake, "--build", use], "PGO data changed or is missing")
    datum.write_bytes(before)
    manifest["identity"]["compilers"][0]["sha256"] = "0" * 64
    manifest_path.write_text(json.dumps(manifest))
    run([args.cmake, "--build", use], "PGO source or compiler changed")
    manifest_path.write_bytes(manifest_bytes)
    print("PGO: separate generate/use builds, holdout output and source/flags/compiler/data rejection PASS")
except Exception:
    print("PGO fixture retained at", work, file=sys.stderr)
    raise
else:
    # Only the directory created by this invocation is eligible for cleanup.
    if not work.is_relative_to(args.work.resolve()) or not work.name.startswith("pgo-"):
        raise RuntimeError("Unexpected fixture cleanup path")
    def remove_readonly(function, path, error):
        # Git marks its immutable object files read-only on Windows.
        mode = Path(path).stat().st_mode
        if not isinstance(error, PermissionError) or mode & stat.S_IWRITE:
            raise error
        Path(path).chmod(mode | stat.S_IWRITE)
        function(path)
    # onerror also works with the workflow's minimum Python 3.11.
    shutil.rmtree(work, onerror=lambda function, path, info: remove_readonly(function, path, info[1]))
