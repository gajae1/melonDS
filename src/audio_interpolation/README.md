# Minimum-phase interpolation coefficients

These frozen little-endian coefficient banks are compiled into the core. They
require no external data file, filter construction library or Python at runtime.
They affect optional host audio reconstruction, not guest capture or save-state
layout. Existing interpolation indices 0–4 retain their meanings; index 5 selects
this mode.

Channel gain and pan are applied to timestamped input events before reconstruction.
Stereo tails retain those weights when the game stops and reuses a channel, so a
new note cannot amplify or repan the previous note's residual. Both sides share
the same kernel lookup; identical dense L/R histories also share accumulation.
This does not change guest audio capture.

The reference step responses were generated with
[r8brain-free-src 7.5, commit 9e73d2dd59fd5b95108fdb4f590083e35758b45f](https://github.com/avaneev/r8brain-free-src/commit/9e73d2dd59fd5b95108fdb4f590083e35758b45f).
The local 57-file upstream snapshot was checked byte-for-byte against that commit's
archive. Its MIT notice is included in `LICENSE.r8brain` and Windows packages.

For decoder period P and mixer interval M (352 or 512 clocks), the reference uses
grid G=min(P,256), cutoff 1/(G*max(1,M/P)), transition band 8, attenuation 96 dB,
minimum phase and gain G. Each phase is normalized to unity DC (deviation is
required to be below 1e-4 before normalization); the integrated step response is
linearly sampled between grid points. These are design parameters, not measured
whole-game attenuation or physical latency guarantees.

P1–256 residuals are fitted in blocks of M clocks with 16 Chebyshev coefficients
(degree 15). P257–M-1 use 256-point blocks expanded once during preparation.
P>=M shares one 24,613-value step response. P1–32 retain a trailing zero-support
block from the original dense fit. The loader validates record sizes and finite
coefficients. Fits used NumPy 2.4.2 least squares; platform BLAS/SVD differences
can change coefficient bits.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| bank-352.coeff | 4,719,796 | a0b65302427e18271eab0186675c582d274434ae73c1aec4220093fd90fbdea9 |
| bank-512.coeff | 7,705,396 | 42a73e6f5554221ac3fedbd104b875ec52a90d94e60437fdc62e5c1bf2e56320 |

Version 1.1.121 replaces the previous 10-percent transition profile in the same
option. The transition parameter measures the distance from the -3 dB point to
cutoff. At 90 percent of the lower source/mixer Nyquist frequency, the normalized
grid response is about -0.61 dB instead of -3.01 dB; at 95 percent it is about
-13.82 dB instead of -20.41 dB. This preserves more upper-band detail while
retaining the 96 dB stopband design target. Longer support increases processing
cost; it is not a CPU optimization or a physical-latency guarantee.

Independent raw-reference comparisons measured maximum fitted-step knot errors
of 3.45e-7 (previous profile: 1.52e-7). Actual stereo stream checks with randomized
full-scale samples, gain/pan changes and fixed/variable periods measured at most
0.025 PCM units of error before rounding; rounded differences were at most one
unit. These are bounded test results, not universal mixed-channel error bounds.

To independently regenerate candidates, install NumPy (the verified run used
2.4.2), provide GCC or Clang with C++20 support, then run from the repository root:

```sh
python tools/audio-interpolation/generate.py --output /path/to/new-output-directory
```

Use `--compiler /path/to/g++` to select the compiler. By default the tool downloads
the pinned r8brain archive and verifies its SHA-256 and all 57 source files.
`--r8brain-dir /path/to/exact/source-tree` allows offline generation. No historical
fit output or frozen bank is read during reference generation or fitting. The
output directory must be new; the tool never installs candidates in the core.

After generation, the actual product loader compares all dense moment rows and
integer-clock sparse/shared steps with the frozen banks, rejecting deltas above
1e-12. `report.json` records tool/source hashes, environment, bank hashes and
measured differences. Windows GCC 16.2 / NumPy 2.4.2 regenerated both banks
byte-for-byte (all compared deltas zero). This is not a cross-platform bit identity
promise, a new fit-error bound or whole-stream/physical audio acceptance.

The original staged experiments and binary inputs are retained locally. This
independent pipeline does not recover the missing historical metadata writer
revision.
Do not silently regenerate or replace these banks without numerical comparison
against the reference and decoder-event acceptance checks.
