# Third-party licences

| Component | Where it is used | Licence |
|---|---|---|
| SDL2 | window, input, GL context (device-supplied, not bundled) | `LICENSE.SDL2.txt` (zlib) |
| OpenAL Soft | audio output (device-supplied, not bundled) | `LICENSE.OpenAL-Soft.txt` (LGPL 2.1) |
| zlib | bundled as `libs.armhf/libz.so.1`; also linked into the installer | `LICENSE.zlib.txt` |
| Terminus Font | the setup screen's bitmap font, baked into `installer.armhf` | `LICENSE.Terminus-Font.txt` (SIL OFL 1.1) |

gptokeyb is **not** used by this port — the engine reads the gamepad natively —
so it is neither shipped nor started.

Nothing from Grand Theft Auto: Liberty City Stories is redistributed here. The
engine (`libGTALcs.so`), all game data, and even the setup screen's background
artwork come from the player's own APK and OBB at install time.
