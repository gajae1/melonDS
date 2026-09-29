<p align="center"><img src="https://raw.githubusercontent.com/melonDS-emu/melonDS/master/res/icon/melon_128x128.png"></p>
<h2 align="center"><b>melonDS</b></h2>
<p align="center">
<a href="http://melonds.kuribo64.net/" alt="melonDS website"><img src="https://img.shields.io/badge/website-melonds.kuribo64.net-%2331352e.svg"></a>
<a href="plans/Current_Status.md" alt="Fork release: 1.1.225"><img src="https://img.shields.io/badge/fork_release-1.1.225-%235c913b.svg"></a>
<a href="https://www.gnu.org/licenses/gpl-3.0" alt="License: GPLv3"><img src="https://img.shields.io/badge/License-GPL%20v3-%23ff554d.svg"></a>
<a href="https://kiwiirc.com/client/irc.badnik.net/?nick=IRC-Source_?#melonds" alt="IRC channel: #melonds"><img src="https://img.shields.io/badge/IRC%20chat-%23melonds-%23dd2e44.svg"></a>
<a href="https://discord.gg/pAMAtExcqV" alt="Discord"><img src="https://img.shields.io/badge/Discord-Kuribo64-7289da?logo=discord&logoColor=white"></a>
<br>
<a href="https://github.com/gajae1/melonDS/actions/workflows/build-windows.yml?query=event%3Apush"><img src="https://github.com/gajae1/melonDS/actions/workflows/build-windows.yml/badge.svg" /></a>
<a href="https://github.com/gajae1/melonDS/actions/workflows/build-ubuntu.yml?query=event%3Apush"><img src="https://github.com/gajae1/melonDS/actions/workflows/build-ubuntu.yml/badge.svg" /></a>
<a href="https://github.com/gajae1/melonDS/actions/workflows/build-macos.yml?query=event%3Apush"><img src="https://github.com/gajae1/melonDS/actions/workflows/build-macos.yml/badge.svg" /></a>
<a href="https://github.com/gajae1/melonDS/actions/workflows/build-bsd.yml?query=event%3Apush"><img src="https://github.com/gajae1/melonDS/actions/workflows/build-bsd.yml/badge.svg" /></a>
</p>
DS emulator, sorta

The goal is to do things right and fast, akin to blargSNES (but hopefully better). But also to, you know, have a fun challenge :)
<hr>

## Fork status

The default Windows build uses SDL3; a separate SDL2 package is retained for compatibility. This fork includes direct Vulkan presentation, Sinc audio interpolation and JIT/core optimizations. See [current implementation and validation limits](plans/Current_Status.md), [reference-project adoption](plans/Reference_Adoption.md), and the [1.1.225 release record](plans/releases/1.1.225.md). 1.1.225 extends the Vulkan raster/blend fusion to small textured Modulate batches (1 to 8 polygons, one texture variant, 1x to 4x), skipping the indirect raster dispatch for eligible batches. In 96 synthetic cases through the real Vulkan compute pipeline on one AMD Radeon GPU, all pixels matched the original Vulkan path and the OpenGL reference; GPU3D time was lower in 91 cases beyond each case's own A/A variation (median 6.78% lower), while 5 narrow single-polygon 4x cases were 0.03 to 0.26% slower within that variation, so no gain or loss is established for them. CPU record and total times were too noisy to claim a reliable gain. No full-game FPS, latency or Vulkan-over-OpenGL/Metal advantage has been established. Previously, 1.1.224 ([release record](plans/releases/1.1.224.md)) fuses raster math into the ordered depth/blend shader for small untextured OpenGL Compute batches (1 to 8 polygons, 1x to 4x), skipping one indirect raster dispatch, its barrier and the intermediate color/depth/attribute accesses for eligible batches; Vulkan received the same fusion in 1.1.223. Through the real OpenGL renderer on one Radeon integrated GPU, all 32 synthetic cases had identical pixels and GPU elapsed time was lower in 30 of them (16.93 to 58.85% lower for broad overlapping polygons). Two narrow single-polygon 1x cases were 0.42% and 0.65% slower, within the 2.56%/2.55% A/A variation, so no gain is established there. No full-game FPS, audio/input-latency or startup improvement has been established. GPU 2D remains opt-in; a universal OpenGL or Vulkan advantage over other renderers has not been established.

## How to use

Firmware boot (not direct boot) requires a BIOS/firmware dump from an original DS or DS Lite.
DS firmwares dumped from a DSi or 3DS aren't bootable and only contain configuration data, thus they are only suitable when booting games directly.

### Possible firmware sizes

 * 128KB: DSi/3DS DS-mode firmware (reduced size due to lacking bootcode)
 * 256KB: regular DS firmware
 * 512KB: iQue DS firmware

DS BIOS dumps from a DSi or 3DS can be used with no compatibility issues. DSi BIOS dumps (in DSi mode) are not compatible. Or maybe they are. I don't know.

As for the rest, the interface should be pretty straightforward. If you have a question, don't hesitate to ask, though!

## How to build
See [BUILD.md](./BUILD.md) for build instructions.

## TODO LIST

See the fork's [development plan](plans/README.md) for tracked work, release phases and validation status.

The list below is the original upstream TODO list. Fork status notes are in brackets; see [the task catalog](plans/Task_Catalog.md) for evidence.

 * better DSi emulation [fork: partial - DSi clock/NDMA timers, reset state, I2C, BTDMP FIFO and title-replace rollback fixed; DSP delay, AHBM/DMA and camera formats are recorded but held for hardware comparison]
 * better OpenGL rendering [fork: partial - palette range upload, texture-state skip, capture downscale limited to read rows, strict-driver compute fix, refresh-limited fast-forward presentation; renderer accuracy differences (GR-08) not yet resolved]
 * netplay [upstream: LAN/Netplay present; fork: connection lifetime and UI hardening; real-game host/client validation not started]
 * the impossible quest of pixel-perfect 3D graphics [not started]
 * support for rendering screens to separate windows [upstream: "Open new window" present]
 * emulating some fancy addons [upstream: RAM expansion, Rumble Pak, Boktai solar sensor, Motion Pak, Guitar Grip present]
 * other non-core shit (debugger, graphics viewers, etc) [GDB stub from upstream, fork hardened parsing/reconnect/port checks; graphics viewers not started]

### TODO LIST FOR LATER (low priority)

 * big-endian compatibility (Wii, etc)
 * LCD refresh time (used by some games for blending effects) [not started]
 * any feature you can eventually ask for that isn't outright stupid

## Credits

 * Martin for GBAtek, a good piece of documentation
 * Cydrak for the extra 3D GPU research
 * limittox for the icon
 * All of you comrades who have been testing melonDS, reporting issues, suggesting shit, etc

## Licenses

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

melonDS is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

### External
* Images used in the Input Config Dialog - see `src/frontend/qt_sdl/InputConfig/resources/LICENSE.md`
