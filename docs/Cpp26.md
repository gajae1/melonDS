# C++26 migration and accuracy-preserving optimization

## Scope

Project-owned C++ targets now require **C++26 mode** and **CMake 3.30+**.
The core exports `cxx_std_26` to consumers; the former `src/CMakeLists.txt`
C++17 override is removed. The Qt frontend, network utilities and regression
suite inherit the new mode. Vendored projects retain their own requirements
(for example Teakra still selects C++17); C sources remain C11.

C++26 mode does not mean every C++26 language/library feature is implemented.
GCC 14+ and Clang 17+ are candidate toolchains for this mode, not a claim that
every platform combination was tested. The Linux CI explicitly uses GCC 14
and Clang 18, and CMake 3.31.6. The application workflows remain responsible for
platform build coverage. Unsupported toolchains fail rather than falling back.

The first library modernization uses the established `<bit>` operations
`std::countr_zero` / `std::countl_zero` (introduced in C++20), not experimental
reflection, contracts or a new SIMD API. Merely selecting C++26 is not claimed
to improve performance.

## Changes to invalidation bitfields

`NonStupidBitfield.h` iterates memory-invalidation bits used by GPU VRAM
tracking. Clear the lowest set bit with `x &= x - 1`, removing the dependency
on constructing a shifted mask from the counted bit index. Use standard bit
operations instead of compiler-specific builtins.

Two independently reproduced edge cases are fixed:

- Empty `SetRange` / `CheckRange` operations return before a shift by 64 or a
  possible access past the final word (an empty range may start at `Size`).
- Iterator termination checks the absolute bit index, not just the bit index
  within a 64-bit word. Padding bits in a partial final word are not returned.

Storage layout, guest timing, CPU instructions, DMA behavior, JIT emitters and
savestate format are unchanged. No game-compatibility improvement is claimed
from these edge cases without an observed game reproduction.

## Verification

The baseline header `d471f0afdd31c495c1d154a6b2acc0e8e14a5e45` was retrieved
from GitHub and its Git blob hash was checked before local testing.
The baseline passed valid nonempty-range tests, failed the padding test, and
UBSan rejected a 64-bit shift in the empty-range test. The modified header
passed the tests with GCC 14.2 and Clang 17, including ASan/UBSan.

The committed suite adds a C++26-mode compile assertion and the bitfield tests
to the seven existing CTest entries. It also compiles the complete core with
JIT enabled and disabled in CI. A compiled core is not an executed JIT test;
record per-job CI results separately. Local testing used a source fixture,
not a full application checkout. Real GPU/game/hardware tests were not run.

## Measured microbenchmark

Same harness and **same C++26 flags for both implementations**:
`-std=c++2c -O3 -fwrapv`, no `-march=native`, no added AVX requirement, no LTO.
AMD EPYC 9V74 virtual host, one allowed logical CPU, deterministic inputs,
one warmup per variant, nine randomized-order paired samples. Report medians;
checksums matched for every pair. Each sample scans 128 1024-bit fields for
1024 iterations. Inputs are mutated each iteration to prevent loop hoisting;
therefore the case named `empty` starts empty but is not permanently empty.

| Compiler | Initial pattern | Before (ms) | After (ms) | Time reduction |
| --- | --- | ---: | ---: | ---: |
| GCC 14.2 | empty | 0.979 | 0.836 | 14.65% |
| GCC 14.2 | sparse | 2.965 | 2.192 | 26.06% |
| GCC 14.2 | mixed | 71.412 | 43.396 | 39.23% |
| GCC 14.2 | dense | 141.530 | 82.109 | 41.98% |
| Clang 17 | empty | 0.863 | 0.873 | -1.17% |
| Clang 17 | sparse | 2.148 | 1.843 | 14.19% |
| Clang 17 | mixed | 52.214 | 43.839 | 16.04% |
| Clang 17 | dense | 110.353 | 91.910 | 16.71% |

These are **routine times, not emulator FPS**. Results are environment- and
compiler-dependent, and a shared VM is not a dedicated performance lab.
Do not turn these numbers into fixed CI timing thresholds.

To compare on another machine, build `tests/BitfieldBenchmark.cpp` against
before/after source directories with the same compiler/flags, alternate runs
for `empty`, `sparse`, `mixed`, `dense`, and verify equal checksums. The current
version can also be built with:

```sh
cmake -S tests -B build-bench -DCMAKE_BUILD_TYPE=Release -DMELONDS_BUILD_MICROBENCHMARKS=ON
cmake --build build-bench --target BitfieldBenchmark
./build-bench/BitfieldBenchmark mixed
```

## Acceptance policy for subsequent emulation work

A hardware-accuracy change needs a reproducible test ROM/input and an observed
DS/DSi result (record console revision, firmware/boot mode and test hash).
Agreement with another emulator is supporting evidence, not the hardware oracle.
A test passing on the current interpreter alone does not prove accurate timing.

An optimization must preserve guest-visible results: ordering of events,
integer rounding, memory aliases, IO side effects, DMA/IRQ behavior, and state
restoration. Compare interpreter/JIT and relevant renderers, with exact
frame/audio/state comparisons where meaningful; do not blanket-hash structures
containing pointers, padding or wall-clock-dependent data.

Measure matched workloads with median and tail frame time. Keep host throughput
separate from the speed of the emulated hardware. Do not skip guest work,
change timing constants, enable fast-math, or add unchecked assumptions solely
to improve a benchmark. SIMD/assembly stays optional and evidence-driven.

The next hardware-oriented candidates remain FreeBIOS unsigned square root
(including generated BIOS data), DSi direct-boot state, and rendering/capture
regressions. Those are not silently included in this migration.

## References

- https://gcc.gnu.org/projects/cxx-status.html
- https://clang.llvm.org/cxx_status.html
- https://cmake.org/cmake/help/latest/release/3.30.html
- https://libcxx.llvm.org/Status/Cxx26.html
