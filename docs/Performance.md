# Performance direction for this fork

## Decision

Keep the existing C++ core, Qt frontend, and native JIT backends. No Rust/Zig
rewrite, handwritten assembly, blanket AVX-512 requirement, or speedup claim is
part of the initial regression fixes. There is no melonDS A/B measurement here
that establishes a benefit from changing the implementation language.

A future isolated Rust or Zig component should justify its boundary through
measurable performance or maintainability/safety benefits. Do not introduce
per-instruction or per-pixel language crossings just to mix languages. Preserve
guest-visible timing, integer behavior, memory mapping, and save compatibility.

## Measurement before specialization

Compare identical scenes and inputs, recording the commit, build flags, CPU,
GPU/driver, renderer, JIT settings, resolution, and speed-limiter state. Measure
both median and tail frame time; separate emulation CPU time, rendering,
readbacks/uploads, audio, and synchronization. Distinguish host throughput from
guest timing accuracy. Use frame/audio/state comparisons appropriate to the
change instead of treating a successful build as a compatibility test.

Investigate common 2D/capture/readback paths before assuming that a 3D backend
or CPU instruction set is the bottleneck. Compare software, classic OpenGL,
and compute modes on the same affected scene. Do not defer GPU readback past a
guest CPU/DMA read that requires the result.

For software rasterization, compare any depth-test dispatch change in isolation;
do not attribute a combined depth-test and alpha-blend benchmark to one patch.
Profile-guided optimization should use representative workloads, with separate
build directories and an unchanged compiler/configuration between collection
and use. Existing release LTO is not a newly introduced optimization.

## SIMD and assembly

Prefer a clear scalar implementation and compiler-generated code first, then
intrinsics for a measured batch-processing bottleneck. Compare scalar/SSE,
AVX2, and relevant AVX-512 variants where available. Verify exact integer results,
unaligned inputs, short spans, and tails. Dispatch outside the pixel loop.

AVX-512 is not one feature bit: check the subsets used by a kernel and OS state
support. Keep a portable fallback; do not compile distributable binaries with
blanket `-march=native` or AVX-512 assumptions. Wider instructions are not proof of
lower total frame time. CPU model and workload matter, including frequency and
memory-bandwidth effects. Handwritten assembly needs an advantage over compiler
output large enough to justify ABI and cross-platform maintenance.

Potential candidates, only after profiling: texture/color conversion, bulk
blending, and framebuffer operations. A serial chain of emulated ARM operations
or a blocking GPU readback cannot be fixed merely by selecting wider vectors.

## References

- Clang optimization and PGO: https://clang.llvm.org/docs/UsersManual.html
- Rust architecture intrinsics and runtime dispatch: https://doc.rust-lang.org/stable/core/arch/
- Zig optimization-mode safety semantics: https://ziglang.org/documentation/0.15.2/
