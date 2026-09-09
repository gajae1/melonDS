# Focused core regression tests

Run from a complete melonDS source checkout. These tests require a compiler supporting C++26 mode,
CMake 3.30 or newer, and Python 3; Ninja is optional. They do not require Qt, SDL,
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

The original ten entries cover the areas below. No real DS/DSi, ROM, GPU driver or filesystem
mutation is exercised. The optional microbenchmark measures only bitfield
iteration, not emulator FPS; see [measurement boundaries](../plans/Validation.md).

- `LanguageStandard` verifies the requested C++26 mode and standard bit operations.
- `Bitfield` checks valid ranges, zero-length boundaries, partial-word padding,
  and sparse/dense iteration against a bitwise reference.
- `UTF8Paths` checks byte-preserving filesystem-path conversion for ASCII,
  Korean, Japanese, non-BMP, long and empty strings, plus embedded-NUL byte
  conversion. It does not open or modify files.

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

## Expanded modernization and real-core tests

The standalone suite now has 17 entries (the original ten plus seven C23,
SIMD, software capture, header-completeness and network tests).
A full-core build enables real Teakra instruction/event/state tests and
hand-authored ARM frame/save/restore tests:

```sh
cmake -S . -B build-core-tests -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF -DMELONDS_BUILD_TESTS=ON
cmake --build build-core-tests --parallel 3
ctest --test-dir build-core-tests --output-on-failure
```

This produces 21 entries on a supported JIT-enabled target, or 19 with
`ENABLE_ASM=OFF`. The headless platform explicitly omits filesystem, network,
AAC and host input/output; unexpected file use aborts. It does not replace
real driver/game/physical-hardware regression tests. The full-core tests are
not automatically sanitized by the standalone sanitizer configuration.
See [the release record](../plans/releases/1.1.04.md) for current scope and measurement limitations.

The Qt build also provides `firmware-profile-direct-boot`. It runs the real
frontend profile override and MAC parser with temporary configuration, then
checks the real firmware/core for counter wrap, an invalid backup, direct boot,
and Chinese/Korean SPI reads. It uses generated firmware, not private dumps.
The opt-in `gpu-compute-frame-capture` test additionally renders a synthetic
triangle at 1x/2x on the host GPU, checking coordinate options, reuse of compiled
shaders, and updates when the guest frame is unchanged. These are regression
checks, not physical-console or full-game accuracy measurements.

`SavestateLoad` uses the current frontend load/undo definitions with the real
core and generated ARM programs. It checks late device failure, preserved CPU/RAM
and the next frame after rollback, one-shot undo and retry, fatal recovery,
file read bounds, and a load allocation exception. The JIT variants run warmed
code and compare the full serialized state after the recovered frame. Its host
file boundary uses temporary Qt files; no user data or physical devices are used.
`StateLoadMessages` exercises the real Qt message dispatcher with scripted core
results: error propagation, audio callback exclusion, queued resume/frame-step/
save rejection after failed recovery, and reset/boot recovery. It does not start
a physical audio device or the full window event loop.

`SaveManagerIO` covers atomic writes/retry, the real worker's path-change locking,
reload with a partial update, and buffer grow/shrink/memory-copy bounds. The path
test holds the worker after a real commit and checks that relocation waits for
the mutex, then verifies both files. This is a bounded locking regression, not a
race detector or proof of crash/power-loss recovery.
