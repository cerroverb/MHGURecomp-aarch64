# MHGU Recomp for 64-bit ARM Linux

This is a fork of [morerokk/MHGURecomp](https://github.com/morerokk/MHGURecomp), the native PC port of Monster Hunter
Generations Ultimate, that builds and releases it for **64-bit ARM Linux (aarch64)**: handhelds such as the AYN Odin 3,
AYN Thor or Retroid Pocket 6 running Armada Linux or Rocknix, ARM laptops and boards.

It is the same game, the same features and the same settings as upstream. Only what ARM needs is added, and every
upstream release is merged in, built and released here automatically within the hour.

No game data is included. You need your own dump of MHGU 1.4.0.

## Download

[Releases](https://github.com/cerroverb/MHGURecomp/releases): `LinuxArm64-MHGURecomp.<version>.tar.gz`. The version is
upstream's plus one more number: `0.13.1.1` is upstream 0.13.1, `0.13.1.2` a fix made here on top of it.

## Install

1. Extract the archive anywhere.
2. Put the game's `exefs` and `romfs` folders (MHGU 1.4.0) in the `data` folder.
3. Start `mhgurecomp-updater`. It checks this fork's releases, installs a newer one if there is one (keeping your
   `data`, `mods`, `cheats`, settings and saves) and starts the game. `mhgurecomp` starts the game without the check.

Everything else (settings, mods, cheats, controls) works as described in
[upstream's README](https://github.com/morerokk/MHGURecomp#readme).

## Requirements

- Linux on 64-bit ARM with glibc 2.38 or newer
- A Vulkan 1.2 driver (Mesa's Turnip, PanVK, Honeykrisp and others). GPUs without BC texture support get those
  textures decoded on the CPU.
- `libcurl.so.4` (curl) for the updater

Tested on an AYN Odin 3 Ultra running Armada Linux.

The ARM code itself is upstream's, shared with its Android build. This fork adds only what Linux on ARM needs:

- the Makefile picks ARM compiler flags when the compiler targets aarch64 (`-march=armv8-a`, `-fsigned-char`);
- the 64 GB guest memory window and the old glibc math symbols for aarch64;
- release archives named `LinuxArm64-`, and an updater that takes only those and asks this fork's releases.

The branch with these changes is [`aarch64-linux-v2`](https://github.com/cerroverb/MHGURecomp/tree/aarch64-linux-v2);
its own README has the full upstream documentation, including how to build (section "64-bit ARM (aarch64)").

ARM-specific problems: open an issue here. Everything else is upstream's: please report it
[there](https://github.com/morerokk/MHGURecomp/issues).

## Credits and license

All credit for MHGU Recomp goes to morerokk and its contributors. MIT license, as upstream (`LICENSE`). LLM assistance
was used to port and maintain the ARM build.
