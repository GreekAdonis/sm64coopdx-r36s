# Building for RK3326 Handhelds (R36S)

This fork targets AArch64 RK3326 handhelds running ArkOS, including the R36S,
R36 Ultra X, and compatible clones. It builds against an older userspace
(glibc 2.30) and the strict GLES2 driver commonly shipped on these devices.

## CI builds

The `.github/workflows/build-r36s.yml` workflow builds the game in an arm64
Debian bullseye container (via QEMU), runs the glibc check below, and packages
the PortMaster-format release zip (`sm64coopdx_r36s.zip`). It runs on every
push to `main` and attaches the zip to tagged releases. You can also trigger it
manually from the Actions tab.

No ROM is required to build. The build uses the game data committed in this
repo (`sound/`, `assets/`, `textures/`, `levels/`); `extract_assets.py` is a
manual tool used by upstream developers to regenerate those assets and is not
part of the build. The ROM is only required at runtime (see Install below).

## Included Compatibility Changes

The required device changes are already included in this fork:

- Debian bullseye/ArkOS linker compatibility (`librt` and `pthread`).
- SDL 2.0.14 compatibility for controller hints and mouse-wheel events.
- Strict GLSL ES 1.00 typing for exposure and posterization shaders.
- A controller-operated on-screen keyboard for text fields.
- A rebuilt ARM64 CoopNet library with an executable-hash override, allowing
  custom AArch64 builds to join public internet lobbies.
- The stock upstream ARM64 `libjuice` library.

The CoopNet override reports the official v1.5.1 Linux compatibility token.
When updating the fork to another game release, verify and update this token.
The patched CoopNet source and build revision are documented in
`lib/coopnet/README.md` and `lib/coopnet/executable-hash-override.patch`.
To rebuild the ARM64 `libcoopnet` archive from source, run
`scripts/rebuild-coopnet.sh` inside an arm64 build environment.

## Build Environment

The easiest reproducible build uses an ARM64 Debian bullseye container under
Docker/QEMU on an x86_64 host:

```sh
docker run --rm --platform linux/arm64 \
  -v "$PWD":/build -w /build debian:bullseye bash -lc '
    apt-get update &&
    apt-get install -y --no-install-recommends \
      build-essential python3 libglew-dev libsdl2-dev libz-dev \
      libcurl4-openssl-dev bsdmainutils &&
    make HANDHELD=1 DISCORD_SDK=0 UPDATER=0 \
      EXTRA_CPP_FLAGS="-std=c++17" -j$(nproc)
  '
```

Do not pass `LDFLAGS` on the command line. Doing so replaces the linker flags
defined by the Makefile.

The output binary is:

```text
build/us_pc/sm64coopdx.arm
```

## Verify the Build

Confirm that the executable is AArch64 and does not require a newer glibc than
the handheld provides:

```sh
file build/us_pc/sm64coopdx.arm
readelf --version-info build/us_pc/sm64coopdx.arm | grep -E 'GLIBC_|GLIBCXX_'
```

The file should be reported as `ELF 64-bit ... ARM aarch64`. Review the
version list for dependencies newer than the device's glibc 2.30.

Test these features before publishing a build:

- Start the game from the ArkOS Ports menu.
- Enter text using the on-screen keyboard.
- List and join a public CoopNet lobby.
- Start or join a LAN game.
- Enable post-processing effects to exercise the GLES2 shader fixes.

## On-Screen Keyboard Controls

- D-pad or analog stick: move between keys.
- A: enter the selected key.
- B: backspace; hold to repeat.
- L or R: shift/capitalized characters.
- OK: close the keyboard and keep the entered value.
- Start: close the keyboard.

## Install on ArkOS

Copy the runtime files to `/roms/ports/sm64coopdx/`. At minimum this includes
the built game data, `mods/`, `dynos/`, `lang/`, `palettes/`, the ROM expected
by the project, and the executable renamed to `sm64coopdx.aarch64`.

Example launcher at `/roms/ports/sm64coopdx.sh`:

```sh
#!/bin/bash
cd /roms/ports/sm64coopdx || exit 1
exec ./sm64coopdx.aarch64 --fullscreen
```

Make the launcher executable:

```sh
chmod +x /roms/ports/sm64coopdx.sh
```

ArkOS can invoke it through its normal Ports wrapper, for example:

```text
sudo perfmax %GOVERNOR% %ROM%; nice -n -19 /usr/local/bin/AltSDL.sh %ROM%; sudo perfnorm
```

No SDL preload is required for this build.

## PortMaster Package

The `port/` directory holds the PortMaster templates (launcher script,
`port.json`, `gameinfo.xml`, cover art and licenses). The CI workflow assembles
the released `sm64coopdx_r36s.zip` from the built binary, the `dynos/ lang/
mods/ palettes/` data folders (as `files.zip`) and these templates. Install by
unzipping into `/roms/ports/`, then provide a vanilla Super Mario 64 (USA)
ROM as `sm64coopdx/baserom.us.z64`.

## Interrupted Build Recovery

An interrupted build can leave
`build/us_pc/sound/sound_data.ctl.inc.c` empty. Make may not regenerate it,
causing an immediate startup crash in audio initialization. Remove the stale
generated files and rebuild:

```sh
rm -f build/us_pc/sound/sound_data.ctl.inc.c \
      build/us_pc/sound/sound_data.o
make HANDHELD=1 DISCORD_SDK=0 UPDATER=0 \
  EXTRA_CPP_FLAGS="-std=c++17" -j$(nproc)
```

If SDL reports that the ALSA device is busy when launching over SSH, test from
the Ports menu instead; EmulationStation can retain the audio device during an
SSH-launched session.