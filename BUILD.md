# Building melonDS

## Current local modernization batch

The new C sources require C23; project C++ and the maintained Teakra target use
C++26. See [release scope and tested configurations](plans/releases/1.1.07.md),
[dependency and toolchain plans](plans/Release_Plan.md#v36), and
[local validation commands](plans/Validation.md). This fork has not been fully built on all
platforms. `MELONDS_CURRENT_TOOLCHAIN=ON` checks the audited current compiler
versions; it does not install or select a compiler for you.

## Toolchain requirements for this fork

Use **CMake 3.30 or newer** and a compiler supporting **C++26 mode** (GCC 14+
or Clang 17+). The build does not silently fall back to C++17/20/23. Language
mode support is not a promise that all C++26 features are available; see
[the development plan](plans/README.md) for scope and verification boundaries.
Vendored libraries keep their own language requirements.

For Ubuntu 24.04, install `g++-14 python3-venv`, install CMake 3.31.6 in a
virtual environment, activate it, and select `-DCMAKE_C_COMPILER=gcc-14
-DCMAKE_CXX_COMPILER=g++-14` when configuring. For example:

```sh
python3 -m venv .venv-build
. .venv-build/bin/activate
python -m pip install cmake==3.31.6
cmake -B build -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
```

The dependency lists below do not upgrade an old system compiler. Older
Ubuntu/BSD/Nix environments may need a newer toolchain separately. Linux CI
now builds on Ubuntu 24.04; do not assume its binaries retain Ubuntu 22.04
runtime compatibility. Use a new build directory when switching compilers.

* [Linux](#linux)
* [Windows](#windows)
* [macOS](#macos)

## Linux
1. Install dependencies:
   * Ubuntu:
     * All versions: `sudo apt install cmake extra-cmake-modules libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev`
     * 24.04: `sudo apt install qt6-{base,base-private,multimedia,svg}-dev`
     * 22.04: `sudo apt install qt6-base-dev qt6-base-private-dev qt6-multimedia-dev libqt6svg6-dev`
     * Older versions: `sudo apt install qtbase5-dev qtbase5-private-dev qtmultimedia5-dev libqt5svg5-dev`  
       Also add `-DUSE_QT6=OFF` to the first CMake command below.
   * Fedora: `sudo dnf install gcc-c++ cmake extra-cmake-modules SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-{qtbase,qtbase-private,qtmultimedia,qtsvg}-devel wayland-devel`
   * Arch Linux: `sudo pacman -S base-devel cmake extra-cmake-modules git libpcap sdl2 qt6-{base,multimedia,svg} libarchive enet zstd faad2`
2. Download the melonDS repository and prepare:
   ```bash
   git clone https://github.com/gajae1/melonDS
   cd melonDS
   ```
3. Compile (select the newer compiler above where necessary):
   ```bash
   cmake -B build
   cmake --build build -j$(nproc --all)
   ```

## Windows
1. Install [MSYS2](https://www.msys2.org/)
2. Open the MSYS2 terminal from the Start menu:
   * For x64 systems (most common), use **MSYS2 UCRT64**
   * For ARM64 systems, use **MSYS2 CLANGARM64**
3. Update the packages using `pacman -Syu` and reopen the same terminal if it asks you to
4. Install git and clone the repository
   ```bash
   pacman -S git
   git clone https://github.com/gajae1/melonDS
   cd melonDS
   ```
5. Install dependencies:  
   Replace `<prefix>` below with `mingw-w64-ucrt-x86_64` on x64 systems, or `mingw-w64-clang-aarch64` on ARM64 systems.
   ```bash
   pacman -S <prefix>-{toolchain,cmake,SDL2,libarchive,enet,zstd,faad2}
   ```
6. Install Qt and configure the build directory
   * Dynamic builds (with DLLs)
     1. Install Qt: `pacman -S <prefix>-{qt6-base,qt6-svg,qt6-multimedia,qt6-tools}`
     2. Set up the build directory with `cmake -B build`
   * Static builds (without DLLs, standalone executable)
     1. Install Qt: `pacman -S <prefix>-qt5-static`  
        (Note: As of writing, the `qt6-static` package does not work.)
     2. Set up the build directory with `cmake -B build -DBUILD_STATIC=ON -DUSE_QT6=OFF -DCMAKE_PREFIX_PATH=$MSYSTEM_PREFIX/qt5-static`
7. Compile: `cmake --build build`

If everything went well, melonDS should now be in the `build` folder. For dynamic builds, you may need to run melonDS from the MSYS2 terminal in order for it to find the required DLLs.

## macOS
1. Install the [Homebrew Package Manager](https://brew.sh)
2. Install dependencies: `brew install git pkg-config cmake sdl2 qt@6 libarchive enet zstd faad2`
3. Download the melonDS repository and prepare:
   ```zsh
   git clone https://github.com/gajae1/melonDS
   cd melonDS
   ```
4. Compile:
   ```zsh
   cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6);$(brew --prefix libarchive)"
   cmake --build build -j$(sysctl -n hw.logicalcpu)
   ```
If everything went well, melonDS.app should now be in the `build` directory.

### Self-contained app bundle
If you want an app bundle that can be distributed to other computers without needing to install dependencies through Homebrew, you can additionally run `
../tools/mac-libs.rb .` after the build is completed, or add `-DMACOS_BUNDLE_LIBS=ON` to the first CMake command.

## Nix (macOS/Linux)

melonDS provides a Nix flake with support for both macOS and Linux. The [Nix package manager](https://nixos.org) needs to be installed to use it.

* To run melonDS, just type `nix run github:gajae1/melonDS`.
* To get a shell for development, clone the melonDS repository and type `nix develop` in its directory.

## Separate Windows debug symbols (optional)

In a configured MSYS2 UCRT64 environment, use a separate build directory:

```sh
cmake -S . -B build/symbols -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMELONDS_PACKAGE_IDENTITY=ON
cmake --build build/symbols --target melonDS
python tools/split-debug-symbols.py build/symbols/melonDS.exe build/symbol-output --msys-prefix C:/msys64/ucrt64 --build-info
```

Use native Windows Python 3.11+ and the SDK that built the EXE. The output directory
must be new, with an existing parent. The source EXE is preserved. The output pairs
a stripped EXE with its `.debug` file through GNU debuglink and records both hashes
in `debug-symbols.json`. The optional `--build-info` executes both EXEs and checks
matching embedded melonDS metadata; without it, no input EXE is executed.
A normal stripped Release EXE cannot recover missing debug information.
Keep debug files separately: they may contain build/source paths and are not
included by the normal runtime deployer. This tool currently supports Windows x64
MSYS2 UCRT64 GNU binutils; it does not create PDB or dSYM files.

## Current dependency profile

`release-current-deps` selects Clang, the audited current-toolchain version
floors, pinned vcpkg dependencies, and an independently installed **Qt 6.11.2**.
Set `CMAKE_PREFIX_PATH` to that Qt installation. This profile does not download
Qt or LLVM implicitly. Generic builds continue to accept the pinned registry
Qt version instead. Windows uses its native Clang preset and can pass the same
`-DMELONDS_USE_EXTERNAL_QT=ON -DCMAKE_PREFIX_PATH=... -DBUILD_STATIC=OFF` options
with a matching MSVC SDK/runtime environment.

The current Qt6 recommended macOS deployment floor is **13.0**; do not expect
new artifacts to retain the old macOS 10.15/11 minimum. See
[the release record](plans/releases/1.1.07.md) for verified and unverified configurations.

## Packaging a Windows release

The optional `MELONDS_PACKAGE_IDENTITY=ON` setting requires Git 2.43+ and Python 3.11+
and embeds a source content ID in `melonDS.exe`. Enable it before building
an executable for `tools/package-windows.py`:

```sh
cmake -S . -B build/windows-dev -DMELONDS_PACKAGE_IDENTITY=ON
cmake --build build/windows-dev
```

For the Windows x64 MSYS2 UCRT64 profile, deploy from the installed SDK with
native Windows Python 3.11+, CMake, Qt's `windeployqt`, binutils and pacman:

```sh
python tools/deploy-windows.py build/windows-dev build/runtime --msys-prefix C:/msys64/ucrt64 --runtime-manifest build/runtime-manifest.json
```

Use the actual SDK prefix that supplied the build dependencies. Qt's dry-run
file plan selects the plugins; CMake resolves their transitive DLL imports, and
pacman's installed file lists select licenses for the owning packages. Missing
or conflicting dependencies and missing package license files fail deployment.
This profile does not deploy Qt translations, matching the previous runtime.
It does not cover MSVC, static builds or other architectures.

If an installed package omits license files, supply reviewed local notices with
`--license-supplement PATH`. The JSON format is
`{"schema":1,"packages":{"<package>":{"version":"<installed-version>","files":{"C:/notices/LICENSE":"<sha256>"}}}}`.
Use exact `pacman -Q` versions and absolute paths with lowercase SHA-256 hashes.
These files are copied under `third-party-licenses/<package>/`; mismatched
versions/hashes, unselected packages and remaining missing notices are rejected.
The tool does not download notices or determine whether supplied texts are legally
complete. ICU's installed license and Qt Multimedia's matching split-package
notices are selected from their package metadata directly.

Both output paths must be new, with existing parent directories, and the manifest
must be outside the runtime. A failed deployment retains its new partial directory
for inspection without publishing a success manifest. Existing output is never
overwritten. The runtime contains `<version>-melonDS.exe` and `run-melonDS.cmd`.
From the build directory in an MSYS2 UCRT64 shell, `tools/msys-dist.sh` (using its
path in the source checkout) creates `dist` and `dist-manifest.json`. It uses
UCRT64 Python by default; `PYTHON` can name another native Windows Python.
Arguments supplied to the wrapper are forwarded to the deployment CLI.

After validation, commit the matching source. The packager requires a clean working tree
and verifies the actual EXE's `--build-info` against the committed source
before writing archives. An older or identity-disabled EXE is rejected.
Root README/BUILD/CONTRIBUTING Markdown and Markdown under `plans/` are the
only content-policy exclusions, so a documentation-only commit can reuse
the same binary. Both sides use Git's check-in filters with
`core.autocrlf=input`; committed blobs use the attributes from HEAD. This
also handles historical CRLF blobs that were never renormalized in the index.

Packaging also requires a UTF-8 runtime manifest with exact relative POSIX paths
and lowercase SHA-256 hashes. For example (replace the sample hash):

```json
{"schema":1,"files":{"melonDS.exe":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}}
```

Generate this list from trusted deployment inputs and the explicit copy list,
including every required DLL, plugin and license file. Do not generate it by
scanning an existing runtime directory or allowing whole directories/extensions.
Unlisted runtime files are excluded with a count-only warning. Missing or changed
listed files fail packaging; the ZIP's exact contents and hashes are checked too.
Keep output outside the runtime and do not alias the input manifest.

```sh
python tools/package-windows.py build/windows-dev build/runtime --runtime-manifest build/runtime-manifest.json --output-dir build/packages
```

Ordinary builds default to identity OFF and do not gain a Git/Python
requirement from this option. The source ID does not attest to the compiler,
external libraries, reproducible builds, or signatures.

To verify the maintained libarchive overlay, run
`python tools/prepare-libarchive-overlay.py` with Python 3.12+ and Git.
This explicit maintenance command downloads the pinned registry files and release,
verifies their hashes, applies the inherited patches in a temporary directory,
and reports differences from the checked-in overlay without changing it.
Matching content returns zero; differences or failed verification return nonzero.
Use `--output NEW_DIRECTORY` to create a verified candidate for review; its parent
must exist and the output must not exist. Review local changes before replacing
the maintained overlay. Normal configuration does not run this command.

The vendored Teakra base is the 83 selected files from upstream commit
[`01db7cdd00aabcce559a8dddce8798dabb71949b`](https://github.com/wwylele/teakra/commit/01db7cdd00aabcce559a8dddce8798dabb71949b).
Vendored libslirp is based on
[`v4.8.0`, `ce314e39458223c2c42245fe536fbe1bcd94e9b1`](https://gitlab.freedesktop.org/slirp/libslirp/-/commit/ce314e39458223c2c42245fe536fbe1bcd94e9b1).
Both include melonDS integration and subsequent local patches; these references
identify their source bases, not unmodified upstream trees.
The libslirp fuzz corpus aliases are materialized as regular packet files and
directories so Windows checkouts and source ZIPs contain the same seeds.
When updating upstream corpus data, update these copies together with their
original targets; the original symlink relationships are recorded by the pinned
upstream tree. This does not restore executable modes or validate an OSS-Fuzz run.

Development builds with tests can additionally execute generated A64 multiply
blocks by setting `-DMELONDS_UNICORN_PYTHON=/path/to/python`. That interpreter must
have Unicorn installed (2.1.4 was validated). The check compares guest registers,
flags and emitted cycle accounting with ARM instruction rules and the interpreter;
it does not replace native ARM64 ABI, executable-memory or hardware timing tests.
