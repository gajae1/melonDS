# Building melonDS

## Current local modernization batch

The new C sources require C23; project C++ and the maintained Teakra target use
C++26. See [release scope and tested configurations](plans/releases/1.1.05.md),
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
[the release record](plans/releases/1.1.05.md) for verified and unverified configurations.
