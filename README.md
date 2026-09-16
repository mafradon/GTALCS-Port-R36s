# GTA: Liberty City Stories — R36S port

A native 32-bit ARM (armhf) Linux port of the Android release of *Grand Theft
Auto: Liberty City Stories*, for R36S-class handhelds running dArkOS / ArkOS /
ROCKNIX. Developed and tested on an RG353V (Mali-Bifrost G52).

The port loads the original Android `libGTALcs.so` directly inside a small Linux
host binary that supplies just enough of Android to keep the engine happy:
bionic's libc ABI, the NDK surface it imports, and a Java runtime faked well
enough that the engine builds and drives its whole menu system through it.
Graphics go to KMS/DRM + GLES2, audio to OpenAL Soft.

**No game content is in this repository.** You supply your own APK and OBB;
even the engine and the setup screen's artwork are taken out of your own copy at
install time.

---

## Status

**Playable end to end.** The game boots, streams Liberty City, drives, and the
frontend is navigable. Measured on device: 500k+ draw calls per 30 s at ~57 fps,
vsync-locked.

Working:

- World streaming, rendering, and gameplay
- The engine's whole HAL frontend (Social Club, menus, legal screens)
- Native gamepad input — the engine reads the pad itself, so no gptokeyb
- Touch frontend driven by a virtual pointer on the stick / D-pad
- First-run installer that unpacks the APK and OBB behind a splash screen

Graphics quality added over the Android build: 4× MSAA, trilinear mipmapping,
anisotropic filtering, and nearest-neighbour filters upgraded to linear.

Known issues are listed in
[the package README](gta-lcs-portmaster/README.md#known-issues) — briefly: an
intermittent heap abort that did not reproduce in normal play, an invalid-pointer
abort on the way out at teardown, audio not verified by ear, and save/load
untested.

---

## How it works

The interesting parts, in rough order of how much trouble they caused:

| Piece | What it does |
|---|---|
| `src/so_util.c` | ELF loader — maps the Android `.so`, relocates it, and resolves all 408 of its imports against our own symbol table |
| `src/jni_patch.c` | The fake JNI. LCS's engine constructs ~25 `com/rockstargames/hal/*` Java widget classes and drives them through roughly 50 up-calls; this implements them as real fake objects, not stubs |
| `src/egl_patch.c` | Routes every EGL call to the vendor library that actually owns the context. Two EGL implementations end up in the process (glvnd→mesa and SDL's Mali) and calling the wrong one fails in ways that look like driver bugs |
| `src/main.c` | Host: window, frame loop, input, the bionic libc surface the engine imports |
| `src/clock_fix.c`, `src/setjmp_fix.c` | ABI repairs where bionic and glibc disagree on a struct or a `jmp_buf` |
| `src/guard_alloc.c` | Optional canary allocator for bring-up |
| `src/installer.c`, `src/wad_extract.c` | First-run setup: decodes the OBB's Rockstar 'DAWL' archive into loose files so the engine stops paying an XOR decrypt on every asset read |

`libGTALcs.so` is **soft-float ABI** while the host binary is hard-float, so
every hook that passes a float or double by value needs `SOFTFP`. That was this
port's default failure mode throughout.

The full bring-up log — every crash, what it actually turned out to be, and the
measurements that got there — is in
[`docs/REVERSE-ENGINEERING-NOTES.md`](docs/REVERSE-ENGINEERING-NOTES.md). It is
long, and it is the most useful thing here if you are porting something similar.

---

## What you need to supply

From your own legitimate copy of the Android game:

| File | Size | Notes |
|---|---|---|
| `*.apk` | ~51 MB | holds the engine, the `assets/` folder and the setup artwork |
| `main.*.com.rockstargames.gtalcs.obb` | ~1.9 GB | game data |
| `patch.*.com.rockstargames.gtalcs.obb` | ~14 MB | optional |

Drop them into the port's `gtalcs/` folder on the device and launch; setup does
the rest. About 3.9 GB free is needed during setup, ~2 GB once it is done.

---

## Building from source

### 1. Cross-compile toolchain (one-time, on a Linux host)

```bash
sudo dpkg --add-architecture armhf
sudo apt-get update
sudo apt-get install -y \
    gcc-arm-linux-gnueabihf \
    libsdl2-dev:armhf \
    libopenal-dev:armhf \
    libgles2-mesa-dev:armhf \
    libegl1-mesa-dev:armhf \
    zlib1g-dev:armhf
```

The Makefile expects headers and libraries under `armhf-sysroot/root/usr/...`.
Symlink that to your install root, or unpack the `.deb` files into it.

The shipping build additionally links against a Debian 11 "bullseye" glibc 2.31
sysroot so the binaries run on older CFWs — `scripts/build-bullseye-sysroot.sh`
creates it at `bullseye-sysroot/root`.

### 2. Extract the engine from your APK

```bash
unzip -j your.apk lib/armeabi-v7a/libGTALcs.so -d extracted/lib/armeabi-v7a/
```

The Makefile reads it from there for `make check-syms` and `make check-abi`.

### 3. Build

```bash
make              # dev build       -> gtalcs_r36 (+ libclock_fix.so)
make portmaster   # shipping build  -> gtalcs.armhf, old-glibc
make installer    # setup binary    -> installer.armhf
```

Resulting glibc floors: **2.29** for the game binary, **2.7** for the installer.

### 4. Assemble the release package

```bash
scripts/make-release.sh    # -> release/ and gtalcs.zip
```

`release/` is exactly what goes into `/roms/ports` on the device, plus the
metadata files PortMaster wants. Neither it nor the built binaries are tracked
in git — grab `gtalcs.zip` from the
[releases page](https://github.com/mafradon/GTALCS-Port-R36s/releases) if you
just want to play.

---

## Useful make targets

| Target | Effect |
|---|---|
| `make check-syms` | every symbol `libGTALcs.so` needs from us — diff against the resolved table after a build |
| `make check-abi` | confirms the soft-float / hard-float mismatch is still what we think it is |
| `make data` | stages a runnable data directory locally |

`tests/` holds small host-side tests for the pieces that are testable off-device
(the WAD decoder, the `_ctype_` table, the canary allocator).

---

## Hardware / OS target

- **CPU**: ARMv7-A 32-bit, NEON, hard-float
- **GPU**: Mali, GLES 2.0 (developed on Bifrost G52)
- **Display**: 640×480
- **OS**: Linux CFW with SDL2 + KMS/DRM (or Wayland), OpenAL Soft, zlib

---

## Acknowledgements

Built on the groundwork of my earlier
[GTA: Chinatown Wars port](https://github.com/mafradon/GTACTW-Port-R36s), which
shares the ELF loader, the clock shim and the two-build Makefile structure.

Port by **Mafradon**. The game itself is © Rockstar Games — this repository
contains only the port glue and Linux host code, and no game assets are bundled.
Licences for the components shipped in the package are in
`gta-lcs-portmaster/gtalcs/licenses/`.
