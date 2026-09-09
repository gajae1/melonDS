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
See [the release record](../plans/releases/1.1.07.md) for current scope and measurement limitations.

`FrontendTouch` delivers generated Qt touch, mouse, tablet, focus and application
events to current production handlers with the real screen layout. Its offscreen
widget checks current drag coordinates, cancellation, inactive releases and the
need for a new press after focus loss. `TouchPublication` uses the current input
publication/read functions to check coordinate pairs while another thread moves
and releases the contact. These checks do not cover physical devices or every
rotation, DPI and multi-window configuration.

`FrontendJoystick` uses real SDL virtual devices, open/close and input sampling.
It covers controller/joystick transitions, capabilities, rumble latch, detach,
reconnect, open failures and ordinary key/hat/axis/hotkey merging. Sensor support
and open failures are injected at their SDL boundaries; motion samples and
physical hotplug are not exercised. If physical devices remain visible after
disabling their drivers for the test process, it skips without opening them.

`FrontendMicrophone` supplies generated sample blocks to the current callback,
resampler, queue reader and open path. Guard pages check endpoint and tiny input
reads; hand-calculated waveforms preserve interpolation phase and ring correction.
A held producer mutex checks the consumer handoff, and reopening an already open
device checks queued state preservation. No microphone is opened; physical
reconnection, listening, measured latency and TSan remain separate validation.

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
save rejection after failed recovery, and reset/boot recovery. The current import
wrapper and dispatcher also check file preparation before reset, input/allocation
failure, and audio exclusion until save application finishes. It does not start a
physical audio device or the full window event loop.

`SaveManagerIO` covers atomic writes/retry, the real worker's path-change locking,
unpublished updates during relocation, and buffer grow/shrink/memory-copy bounds. The path
test holds the worker after a real commit and checks that relocation waits for
the mutex, then verifies both files. It also checks unpublished pending requests,
synchronous flush of the latest data, recovery copies that preserve the original
pending state, same-path rejection, and failed copy commits. This is a bounded
locking regression, not a race detector or proof of crash/power-loss recovery.

`CartReplacement` compiles the current frontend load/console definitions with the
real core, cart parsers and SaveManager. Generated DS/GBA ROMs and temporary save
files check failed preparation without changing the active or queued cart, save
manager or asset paths, normal replacement, inactive GBA save ownership, pending
write failure, same-game reopen, and SRAM length/read failures. It does not prove
real-game progress, successful DSi mode transitions or all allocation failures.
Its partial-import case routes the real cartridge save callback through the real
SaveManager and checks that both SRAM and the persisted file retain the tail.

`FrontendFileIO` uses current ROM/RTC definitions with real Qt files, zstd,
libarchive and core RTC. It checks length narrowing, complete reads, allocation
failure, relative/Unicode names, rejected output preservation and atomic RTC
writes. `ArchiveIO` compiles the production archive code against generated
ZIP/7z/tar inputs and injects selected header/data/allocator failures. Missing
members, resource release, CRC/end-of-entry errors, valid short chunks and output
preservation are covered. Tagged UTF-8 ZIP names are checked in the C locale;
untagged legacy filenames retain libarchive's existing interpretation.
Maximum-size decompression and every archive format are
not. The existing shared `rtc.bin` name and state layout remain unchanged.

`FrontendClose` runs the current close handlers with real Qt windows, close
events, message boxes and file dialogs on the offscreen platform. Temporary files
and scripted producer/save contracts cover retry, cancel, recovery copies and
child-instance cancellation before any window is destroyed. Actual worker I/O is
covered by SaveManagerIO and CartReplacement; full frontend/device shutdown acceptance remains
separate. Generated method includes explicitly precede moc processing.


`LANPacket` injects real ENet packets through event delivery and checks the public
MP receive paths. It covers v1 sender/peer association, exact payload size,
empty AID-0 replies, the existing receive crops and readiness across channels.
Only event delivery/outbound transport is replaced; no socket is opened.

`PCapInput` compiles the production backend with dynamic-library, pcap and adapter
enumeration boundaries replaced. Guarded capture buffers, missing symbols,
non-Ethernet link types, I/O errors and open/move/destruction lifetimes are checked.
It does not load a capture driver or transmit on a physical adapter.

`SlirpDNS` extracts the current production frame-finalization, DNS handler and
send method at build time. Protected headers/names, IPv4 options and fragments,
UDP/DNS bounds and normal A responses are checked with generated frames and a
substitute resolver. Host DNS lookups and live slirp networking are not used.

`GdbProtocol` compiles the real stub, framing and commands in one translation unit.
Guest memory callbacks record access widths and bytes. Injected socket boundaries
cover exact/overflow buffers, malformed m/M/X, binary escapes, checksums,
coalesced/repeated packets, partial/zero/would-block sends and connection reset.
Its `loopback` case uses real local TCP for split requests, checksum recovery,
NoAck negotiation, reconnect and detach. Only the listener bind is restricted to
loopback by the fixture. Actual ARM debugger clients, both emulated CPU ports and
Linux/BSD runtime acceptance remain separate.
