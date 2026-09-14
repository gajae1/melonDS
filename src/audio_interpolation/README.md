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
This does not change the frozen frequency response or guest audio capture.

The reference step responses were generated with
[r8brain-free-src 7.5, commit 9e73d2dd59fd5b95108fdb4f590083e35758b45f](https://github.com/avaneev/r8brain-free-src/commit/9e73d2dd59fd5b95108fdb4f590083e35758b45f).
The local 57-file upstream snapshot was checked byte-for-byte against that commit's
archive. Its MIT notice is included in `LICENSE.r8brain` and Windows packages.

For decoder period P and mixer interval M (352 or 512 clocks), the reference uses
grid G=min(P,256), cutoff 1/(G*max(1,M/P)), transition band 10, attenuation 96 dB,
minimum phase and gain G. Each phase is normalized to unity DC (deviation is
required to be below 1e-4 before normalization); the integrated step response is
linearly sampled between grid points. These are design parameters, not measured
whole-game attenuation or physical latency guarantees.

P1–256 residuals are fitted in blocks of M clocks with 16 Chebyshev coefficients
(degree 15). P257–M-1 use 256-point blocks expanded once during preparation.
P>=M shares one 19,843-value step response. P1–32 retain a trailing zero-support
block from the original dense fit. The loader validates record sizes and finite
coefficients. Fits used NumPy 2.4.2 least squares; platform BLAS/SVD differences
can change coefficient bits.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| bank-352.coeff | 3,805,348 | 300961fde06ef30a8182b345fd9a422ccb33842f16886ad8692d38fa47353227 |
| bank-512.coeff | 6,202,532 | 1c2914cbdf11a347a591f3518b6129678e0e00dfa4e2f3f4bc213e1983deabef |

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
