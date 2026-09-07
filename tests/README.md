# Focused core regression tests

Run from a complete melonDS source checkout. These tests require a C++17 compiler,
CMake 3.16 or newer, and Python 3; Ninja is optional. They do not require Qt, SDL,
BIOS images, ROMs, or a GPU context.

```sh
cmake -S tests -B build-regression -DCMAKE_BUILD_TYPE=Debug
cmake --build build-regression
ctest --test-dir build-regression --output-on-failure
```

For a separate Clang AddressSanitizer/UndefinedBehaviorSanitizer build on Linux:

```sh
cmake -S tests -B build-regression-sanitized \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-regression-sanitized
ctest --test-dir build-regression-sanitized --output-on-failure
```

## Coverage and limits

- `SavestateSections` compiles the actual `src/Savestate.cpp`; only frontend
  logging is replaced. Five cases cover valid/reordered/header-only sections,
  invalid matching lengths, truncated headers, zero-length skips, and oversized
  skips. The zero-length case has a timeout to catch non-advancing iteration.
- `CaptureReadback` compiles the current `GLRenderer::SyncVRAMCapture` definition
  extracted from `src/GPU_OpenGL.cpp` with a recording OpenGL boundary and a small
  VRAM fixture. All 16 start/size combinations check written bytes and dirty
  flags, including the 128x128 special case. It does not test shader output,
  real OpenGL drivers, frame timing, or complete GPU state.
- `JitSettings` extracts the current `ARMJIT::SetMaxBlockSize` definition and the
  actual `JITArgs` layout. Eight flag combinations check argument forwarding to
  a recording `SetJITArgs` boundary, not native code generation, cache resets,
  or JIT execution.

The extraction script deliberately follows the current source formatting and
fails on a missing/non-unique definition or unbalanced braces. It is not a
C++ parser and does not keep a second, potentially stale implementation. Keep
these tests narrow; use integrated core/game tests for broader changes.

Passing this suite is not evidence of complete emulator compatibility or a
measured performance improvement. No savestate format version is changed by
these fixes. Section validation is not a complete malformed-state audit.
