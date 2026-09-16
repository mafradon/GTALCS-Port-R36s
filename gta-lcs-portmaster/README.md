# GTA: Liberty City Stories — PortMaster port

A native 32-bit ARM (armhf) port of the Android release. The original
`libGTALcs.so` engine is loaded by a custom Android-on-Linux loader that
supplies bionic, NDK and Java-runtime behaviour on top of KMS/DRM, GLES2 and
OpenAL.

**No game content is redistributed.** You supply your own APK and OBB. Even the
engine and the setup screen's artwork are taken out of your own APK at install
time.

## What you need

Copy these into the port's `gtalcs/` folder:

| File | Size | Notes |
|---|---|---|
| your `*.apk` | ~51 MB | unpacked at first run, then deleted |
| `main.*.com.rockstargames.gtalcs.obb` | ~1.9 GB | unpacked at first run, then deleted |
| `patch.*.com.rockstargames.gtalcs.obb` | ~14 MB | optional; applied over the main data |

### Free space

* **during setup: about 3.9 GB** — the archives and the unpacked data both exist
  until extraction finishes
* **after setup: about 2 GB**

### Why it unpacks

The `.obb` is a Rockstar 'DAWL' archive in which every byte is obfuscated. The
engine can read it in place, but then it pays to decrypt on *every* asset read
for the life of the install, and that costs frames on a handheld. Setup unpacks
it once into `gtalcs/gamedata/` and deletes the archive.

If you would rather keep the archive, put the `.obb` in place without letting
setup run to completion and the engine will mount it as before — both layouts
work. `gtalcs.log` records which one is in use.

## First run

A setup screen appears, asks for your language, checks free space, and then
unpacks the APK and the OBB with a progress bar. It takes a few minutes. If
something is missing it says so on screen and returns to the frontend.

## Controls

The gamepad is read natively by the engine, so no key mapping is used and
gptokeyb is deliberately not started.

| Button | In a car | On foot |
|---|---|---|
| **B** (bottom) | accelerate | sprint |
| **A** (right) | attack | attack |
| **X** (top) | enter / exit vehicle | enter vehicle |
| **Y** (left) | brake / reverse | jump |
| **L1 / R1** | brake / throttle | — |
| **L2 / R2** | what the game calls L1 / R1 | |

**L1/R1 and L2/R2 are exchanged** relative to the labels, so the shoulder
buttons work the brake and throttle. Set `conf/swap_shoulders.txt` to `0` for
the layout as labelled.

The menus are the Android touch frontend: the **left stick or D-pad** moves a
pointer and **A** taps. That tap button is deliberately not the accelerator, so
holding the throttle does not also hold a touch down on the frontend.

On these handhelds the button silkscreened **A** is the one SDL calls `b`, which
is why the default below reads `b`. If your pad is labelled the other way round,
`conf/tap_button.txt` takes any SDL button name (`a`, `b`, `back`,
`rightstick`, …).

## Settings

`gtalcs/conf/`:

| File | Effect |
|---|---|
| `language.txt` | `en`, `fr`, `de`, `it`, `es` or `ja` — written by the setup screen |
| `videodriver.txt` | overrides the SDL video driver (default: kmsdrm, or wayland if a compositor is detected) |
| `msaa.txt` | antialiasing samples: `0`, `2`, `4` or `8` (default `4`) |
| `swap_ab.txt` | `1` exchanges A and B as the game sees them (default `0`) |
| `tap_button.txt` | SDL button name that taps the menu pointer (default `b` — the button labelled **A**) |
| `swap_shoulders.txt` | `0` keeps L1/L2 and R1/R2 as labelled (default: exchanged) |

## Graphics

Beyond what the Android build does, the port applies:

* **4x MSAA** — the game renders into the port's own window, so this covers
  everything; Mali resolves it in tile memory, so it is cheap here
* **trilinear mipmapping** — mipmaps are generated on every texture upload
* **anisotropic filtering** at whatever maximum the driver reports
* **nearest-neighbour filters upgraded to linear**, and prevented from being
  downgraded again by the engine

Set `conf/msaa.txt` to `0` if antialiasing causes trouble on your device.

## Known issues

* An intermittent heap abort can freeze the game — the picture stays on the last
  frame and the process stops responding. Not yet fixed; it did not reproduce in
  normal play during testing.
* Audio has not been verified by ear.
* Save/load is untested.

## Troubleshooting

`gtalcs/gtalcs.log` is written fresh on every launch and is the first thing to
read. It records the video driver, which data layout was used, the installer's
exit code, and any crash.

## Credits

Port by **Mafradon**. Licences for the bundled components are in
`gtalcs/licenses/`.
