# GTA: Liberty City Stories — R36S port: reverse-engineering notes

Source material (user-supplied, not redistributable):
- `Original-Files/com.rockstargames.gtalcs.apk` (51 MB, built 2016-07-15)
- `Original-Files/main.17.com.rockstargames.gtalcs.obb` (1.9 GB)
- `Original-Files/patch.15.com.rockstargames.gtalcs.obb` (14 MB)

## Target binary

`lib/armeabi-v7a/libGTALcs.so` — 7.9 MB, ELF 32-bit ARM EABI5, stripped, `DT_SYMBOLIC` set.
`DT_SYMBOLIC` matters: intra-library references bind locally, so our symbol table only
has to cover the 409 undefined imports.

Other APK libs (all bionic-linked, **do not reuse**): `libopenal.so`, `libmpg123.so`,
`libmpg123-jni.so`, `libImmEmulatorJ.so`.

`DT_NEEDED`: liblog, libandroid, libEGL, libGLESv2, libOpenSLES, libopenal,
libImmEmulatorJ, libstdc++, libm, libc, libdl.

## Undefined-symbol budget (409 total) — see `undefined_symbols.txt`

| Group | Count | How we satisfy it |
|---|---|---|
| GLES2 (`gl*`) | 141 | forward to host `libGLESv2.so.2` |
| EGL (`egl*`) | 12 | forward to host `libEGL.so.1` |
| OpenAL (`al*`/`alc*`) | 23 | host OpenAL-Soft (PortMaster control folder) |
| pthread + sem | 30 | **ABI-translating shims** (see below) |
| AAssetManager | 7 | our shim over extracted APK `assets/` |
| ImmVibe (haptics) | 7 | no-op stubs |
| `__android_log_*` | 4 | route to stderr / logfile |
| libc / libm / `__aeabi_*` / `_Unwind_*` | rest | mostly direct glibc passthrough, some wrapped |

Notable: **OpenSLES is in `DT_NEEDED` but no `sl*` symbol is imported** — audio is OpenAL
only. No `dlopen` (only `dlsym`). No C++ mangled imports at all (`_Z*` count = 0), so the
STL is statically linked inside the .so — nothing to satisfy there.

## Graphics: hybrid GLSurfaceView pattern

Imported EGL entry points include `eglCreateContext`, `eglMakeCurrent`,
`eglGetCurrentContext`, `eglCreatePbufferSurface` — but **not** `eglCreateWindowSurface`.

That is the Android GLSurfaceView shape: Java creates the on-screen context + window
surface, and the native side only creates a *shared* context on a pbuffer for background
resource streaming. So:

- **We** create the window and main GL context (SDL2, kmsdrm, EGL backend).
- The game calls `eglGetCurrentContext()` to grab ours and passes it as the share context.
- Our EGL forwarding must hand back the *same* `EGLDisplay`/`EGLConfig` SDL used, or the
  shared-context creation fails. Using SDL's EGL backend (not GLX) is required, not optional.

## Assets: two separate paths

1. `AAssetManager_open`/`_read`/`_seek`/`_close`/`_getLength`/`_getRemainingLength`/
   `AAssetManager_fromJava` → APK `assets/` (fonts, `images/`, `json/socialClubAssets.json`,
   `xml/audio_data.xml`). Small; we extract the directory and back the shim with real files.
2. Plain `fopen`/`fread`/`fseek` on paths built from `setGameFilesDir` → the OBBs.
   The OBBs are a **custom Rockstar container, not a zip** (both begin `eb 27 f8 2a`).
   We don't need to parse them — the game's own reader does. We only supply the directory.

## JNI surface

`GTAJNIlib` static natives (from `classes.dex`):

```
setAssetManager(AssetManager)      setPrivateFilesDir(String)
setGameFilesDir(String)            setFileInstalled(String)
setDeviceInfo(int, String, String, String)
setOSVersion(int)                  setDisplaySize(int, int)
setIsTVDevice(bool)                setHasVibrator(bool)
viewOnInit()  viewOnDrawFrame(float)  viewOnPause()  viewOnResume()
setNoJoysticks(int)  onJoyButtonDown(int,int)  onJoyButtonUp(int,int)  setJoyAxis(int,int,float)
onTouchStart/Move/End(int, float, float)   setAccelerometer(float,float,float)
onBackButtonPressed()  isOnMainMenuScreen()  getGameBuildType()
callSocialClubUpdate()  callSocialClubSignOut()  TestJNI()
```

Also `ActivityWrapper_main` and a large `com.rockstargames.hal.*` UI callback surface
(andButton / andScrollView / andTable / andTextInput / andHttp / andWebView …) — the
Android-side HAL widgets used for menus and Social Club.

### Call order to emulate (recovered from dex bytecode)

Startup (`GTAActivityBase.onCreate` → `GTAActivity.a()`):
```
setAssetManager(am)
setIsTVDevice(false)          // UiModeManager type 4 == TV; we pass false
setHasVibrator(hasVibrator)
setPrivateFilesDir(dir + "/")
setGameFilesDir(dir)
```
GL thread (`A` = the GLSurfaceView.Renderer):
```
onSurfaceCreated:  viewOnInit(); setOSVersion(Build.VERSION.SDK_INT)
onSurfaceChanged:  setOSVersion(SDK_INT)
                   setDeviceInfo(int, String, Build.MANUFACTURER, Build.HARDWARE)
                   setDisplaySize(max(w,h), min(w,h))     // landscape: long edge first
onDrawFrame:       viewOnDrawFrame(dt)
                   setNoJoysticks(getNumberControllers())
```

**`setDisplaySize` takes `max(w,h), min(w,h)`** — always long edge first.
`setNoJoysticks` is pushed every frame, not once.

### Input — the good news

LCS has **native gamepad support** (`setJoyAxis`, `onJoyButtonDown/Up`, `setNoJoysticks`).
Unlike the Chinatown Wars port, we should not need gptokeyb touch emulation; SDL_GameController
events feed straight in. Button-ID encoding still needs confirming against
`GTAGLview.getNumberControllers` and the key handler.

## The landmine: bionic vs glibc pthread ABI

32-bit bionic `pthread_mutex_t` is **4 bytes**; glibc ARM32's is **24**. `pthread_cond_t`
is 4 vs 48. The game calls `pthread_mutex_init` on memory it allocated itself — often
embedded inline in its own structs. Passing those addresses to glibc's implementation
overruns adjacent fields and corrupts memory in ways that surface as random crashes far
from the cause.

Mitigation, built in from the start (not retrofitted): the bionic-side object holds only a
handle; a side table maps handles to real glibc objects. Applies to `pthread_mutex_t`,
`pthread_cond_t`, and `sem_t`.

Related: never let a bionic-side `FILE*` cross into glibc — shim the whole
`fopen`/`fread`/`fwrite`/`fseek`/`ftell`/`fclose`/`fflush` family consistently.
Also `__errno` (bionic) vs `__errno_location` (glibc), and `struct stat` layout differences.

## Other behavioural notes

- Stub `andHttp` / Social Club to return **failure**, never to block — a stub that never
  completes becomes a hang on a network wait instead of a clean skip.
- `MovieActivity` plays the intro video; safe to skip.
- `lucidobb/*` is the OBB downloader — irrelevant, the user supplies the OBBs.

## Reference: the Chinatown Wars port

Shipping layout to clone (`gtacw2-0.zip`): `port.json`, `gameinfo.xml`,
`GTA Chinatown Wars.sh`, `gtactw/{gtactw.armhf, libs.armhf/, licenses/, installer.armhf,
gtactw.gptk, libclock_fix.so}`, `README.md`, `screenshot.png`.
Its binary links `libSDL2-2.0.so.0, libEGL.so.1, libGLESv2.so.2, libz.so.1, libm, libc`.
Distribution model: loader only — the user supplies APK/OBB and the installer extracts.

---

# Second pass: the HAL surface (2026-09-07)

## The real engine entry is `ActivityWrapper.runMain(int, int)`

`GTAJNIlib`'s `viewOnInit`/`viewOnDrawFrame` are the *render* path. The engine
boot path is separate, and recovered from `ActivityWrapper.runMain(I I)`:

```
getInstance()
setupLocale()
setUserAgent(String)
setVersionNumber(getVersion())
setCurrentScreenSize(int, int)
main()                       <- Java_com_rockstargames_hal_ActivityWrapper_main
```

`main()` is the native entry proper. Whether it blocks decides the host's
threading model: on Android `runMain` is a UI-thread path while
`viewOnDrawFrame` runs on the GLSurfaceView render thread. Whichever host
thread makes the GL context current must be the one that calls
`viewOnDrawFrame` forever after — `egl_patch_capture()` assumes exactly that.
**Still to confirm: does `ActivityWrapper_main` return, or does it run the game
loop itself?**

Also required init, not in the GTAJNIlib list: `setLanguage(String, String)`,
`setVersionNumber`, `setUserAgent`, `onStartApp`, `onResumeApp(String)`.

## FindClass surface — the biggest remaining scope item

Class-path strings in the `.so`'s rodata (`docs/findclass_strings.txt`, 35 total)
are exactly what the fake JNI layer must satisfy. **25 of them are
`com/rockstargames/hal/*`:**

```
ActivityWrapper  andAudio     andButton      andColourPicker  andDrawingView
andDropDownList  andFile      andHttp        andImage         andImageView
andLabel         andLinkAccounts             andNotification  andScreen
andScrollView    andSecureData               andSpinner       andTable
andTextInput     andThread    andVideo       andView          andViewManager
andWebView       OtherAppLauncher
```
plus `com/rockstargames/gtalcs/{CommonAPI,GTAActivity}`,
`com/rockstargames/socialclub/SocialClubActivity`, and framework classes
`android/app/Activity`, `android/view/ViewRoot`, `android/net/DhcpInfo`,
`android/net/wifi/WifiManager`, `java/lang/Thread`, `java/util/UUID`.

These are Java UI objects that **native code constructs and drives via
up-calls** — the engine's entire frontend/menu layer. The Chinatown Wars port
never needed this: its `jni_patch.c` stubs `FindClass` to `ret0` and answers a
handful of `GetMethodID`s. For LCS the fake-JNI layer has to be a real object
system, and that is the single largest piece of remaining work.

**`andAudio` is reached from native**, which resolves the radio/music question:
streaming genuinely is a Java-side path we have to implement, not something the
engine does natively. (The engine does import
`alSourceQueueBuffers`/`alSourceUnqueueBuffers`, so some native streaming exists
too — the split between the two still needs mapping.)

## One piece of luck: `com/wardrumstudios/utils/WarGamepad`

Same Wardrum Studios middleware as Chinatown Wars. Note the CTW port's
`jni_patch.c` does *not* reference it, so there is nothing to copy — but the
gamepad conventions should be familiar territory.

## `dlsym` without `dlopen`

The `.so` imports `dlsym` but never `dlopen`, so it is `dlsym(RTLD_DEFAULT, ...)`.
Our stub must search our own resolver table and **log every miss** — returning
NULL silently turns into a call through a null pointer much later.

---

# Bring-up log (2026-09-07)

## Test device is an RG353V, not the R36S

The test device now reports hostname `rg353v`: aarch64 kernel 5.10.226, **glibc 2.41
(Debian 13)**, Mali-Bifrost **G52** (`libmali-bifrost-g52-g13p0-gbm.so`), 1971 MB RAM,
pad `retrogame_joypad`. The older notes recorded that device as the Mali-400 R36S — that is
no longer what answers there.

armhf multiarch is complete (`/lib/arm-linux-gnueabihf/` has SDL2 2.32, libEGL,
libGLESv2, libopenal 1.24). `/usr/lib32` is empty; `PORT_32BIT=Y` in PortMaster's
control.txt points there, so it contributes nothing on this device — the armhf libs come
from the normal multiarch path. `/roms` has only ~3.8 GB free, and the main OBB is 1.9 GB.

## THE BIG ONE: 64-bit time_t smashes the game's stack

Symptom: `SIGSEGV, PC=00000000`, with `LR` pointing into unrelated clock code, during
`so_initialize` — initialiser 126/181, a global `lgClock` constructor.

Cause: bionic's 32-bit `struct timespec` is **8 bytes** (32-bit `time_t` + `tv_nsec`).
`Platform::GetSystemTicks` allocates exactly that:

```
sub sp, sp, #8
mov r0, #1                  ; CLOCK_MONOTONIC
bl  clock_gettime@plt
ldr r0, [fp, #-8]           ; tv_nsec
ldr r2, [fp, #-12]          ; tv_sec
mov r3, #0x3b9aca00         ; 1e9
```

glibc 2.41 on ARM32 uses a **64-bit `time_t`**, so its `struct timespec` is 16 bytes.
Its `clock_gettime` overruns the game's 8-byte buffer by 8 bytes, directly onto the
saved `fp` and `lr`. The function then returns via `pop {fp, pc}` into the wreckage —
hence `PC=0` and a stale `LR` that points at whatever ran last. **Nothing in the symptom
points at the cause.**

Fix: `bionic_clock_gettime`/`bionic_gettimeofday`/`bionic_nanosleep`/`bionic_time` in
`clock_fix.c` issue the legacy 32-bit syscalls directly (`clock_gettime` 263,
`gettimeofday` 78, `nanosleep` 162) and write the 8-byte layout. The symbol table points
the game's imports at these. y2038-limited on purpose: it is the ABI the game was built
against.

**Generalise this.** Any struct crossing the boundary whose layout depends on `time_t`
or on bionic-vs-glibc differences is suspect — `stat`/`fstat` were already hooked for the
same class of reason. Check the game's stack allocation against the host struct size.

## Where bring-up stands

All 181 initialisers now run. The JNI bootstrap reaches
`Java_com_rockstargames_hal_ActivityWrapper_main`, which executes real engine code and
gets as far as Social Club / HAL screen setup before asserting:

```
JNI: unmapped GetMethodID('createView', '(I)Lcom/rockstargames/hal/andView;')
JNI: unmapped GetMethodID('IsNetworkReachable', '()Z')
WARNING: halHttpInit not implemented.
JNI: unmapped GetMethodID('getFile', '(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[B')
NULL string returned from Java side getFile()
Entered ../../../SocialClub/code/hal/android/andScreen.cpp:66 createScreen
JNI: unmapped GetMethodID('createScreen'…), ('createLoadingScreen'…)
Assert false failed on line 149 in ../../../SocialClub/code/hal/android/and.cpp
-> SIGSEGV at libGTALcs.so+0x5380c0
```

### Scope correction, stated honestly
The earlier rodata signature scan suggested the up-call surface was only CommonAPI +
WarGamepad, and the HAL classes were referenced but not driven. **Bring-up disproves
that.** The engine really does call `createView`/`createScreen`/`createLoadingScreen`
and the Social Club HAL during boot. The truth is between the two estimates: more than
the ~30 methods the signature scan implied, less than a general fake-object system.

Next: implement the `andScreen`/`andView` construction path and make the Social Club
HTTP/getFile paths fail cleanly rather than assert. `halHttpInit` and `IsNetworkReachable`
should report no network so the engine takes its offline branch.

---

# The real up-call surface (recovered 2026-09-07, second scan)

The first rodata scan used too narrow a regex and found only the CommonAPI/WarGamepad
cluster. Scanning for *any* JNI signature string and pairing it with the nearest
preceding identifier recovers **50 up-calls** — the whole surface, available up front
instead of one `unmapped GetMethodID` log line at a time.

The reassuring part: it is **bounded and mostly mechanical**. There is no need for a
general fake-object system; these group into six clusters.

| Cluster | Methods |
|---|---|
| HAL widgets | `createView` `createScreen` `createLoadingScreen` `addViewToScreen` `addSubview` `setBounds(FFFFFF)` `setTransform(FFFFFF)` `setNextFocusView` `setDebugString` `setAutoResize` `setTouchEventsEnabled` |
| Shapes | `createShape(Z)I` `addPoint(IFF)` `setPoints(I[F)` `setFillColour(IIIII)` `setStrokeThickness(IF)` `SetShapeOverlay(IIII)` |
| Images | `toImage` `unpackImage(…11 ints)` `loadImageFromBytes` `getByteData()[B` `setImage` `setBackgroundImages` |
| Text | `setText` `setTextColour(IIII)` `setTextSize(F)` `getTextWidth()I` `getTextHeight()I` `getCachedW/H()I` `setTypeFace(ZZ)` `setWatermarkText` |
| Key-value store | `GetBool/GetInt/GetFloat/GetString(String)` `SetBool/SetInt/SetFloat(String,…)` |
| HTTP (Social Club) | `GET(ILjava/lang/String;Ljava/lang/String;)V` `POST(…[B)` `HEAD(ILjava/lang/String;)` `IsNetworkReachable()Z` |
| Audio (andAudio) | `PlayAudioFile(Ljava/lang/String;FLjava/lang/String;ZI)I` `IsPlaying(I)Z` `ReleaseHandle(I)V` |
| Misc | `getFile` `readUserFile` `getActivity` `getSystemService` `getDhcpInfo` `randomUUID` `sleep(J)` `getTotalMemoryBytes()J` |

Full list with signatures: `docs/findclass_strings.txt` plus the scan in the session log.

## Two lessons that keep repeating — apply them to anything new

1. **Zero and NULL both mean "failure" to this engine.**
   - `GetMethodID` returning 0 → `getClassAndMethod()` calls `halDebug::abort()`, which is
     a deliberate `mov r3,#0 / str r3,[r3]`. That is the intentional-segfault idiom, so the
     "crash" is really an assert; do not go hunting for a memory bug.
   - An object-returning up-call returning NULL → the engine treats the Java side as
     broken and throws (`terminate called without an active exception`).

   So unmapped methods get a unique **non-zero** stub id, and object-returning calls get a
   unique **non-NULL** opaque handle. The engine only hands these back to us, so no real
   object modelling is needed until a screen genuinely depends on behaviour.

2. **Both the varargs and the `va_list` call forms are used.** Wiring only the `…MethodV`
   slots leaves the plain `CallLongMethod` etc. pointing at `jni_unimpl`. AAPCS spreads
   varargs over r3 + stack, so the varargs forms marshal into a flat array and delegate to
   the V implementation.

## `getFile` convention — confirmed from a live call

```
[getFile] args: 'json' | 'socialClubAssets' | 'json'
```
i.e. **(directory, basename, extension)** relative to the APK `assets/` root →
`assets/json/socialClubAssets.json`. Everything the Social Club HAL loads comes through
this one call, and a NULL return leaves its resource maps empty, which surfaces much later
and confusingly as `Image index out of bounds (index 24 out of 0)`.

---

# Threading model — settled on device (2026-09-07)

**`Java_com_rockstargames_hal_ActivityWrapper_main` does not return.** Called inline it
blocks driving the HAL/Social Club UI, and phase 3 (`viewOnInit`) was never reached.

So the host mirrors Android's split:

| Thread | Role |
|---|---|
| **main** (created the SDL window, holds the GL context, `egl_patch_capture()` ran here) | `viewOnInit`, `viewOnDrawFrame`, input, `SDL_GL_SwapWindow` |
| **`lcs-ui`** (spawned) | `ActivityWrapper_main` — never returns |

The UI thread is the one that gets moved, never the render loop: the EGL context belongs
to the thread that made it current, and `egl_patch` captured it on main.

With that split, phase 3 is reached for the first time — but every `glGetString` there
returns NULL (`GL_RENDERER` 0x1F01, `GL_EXTENSIONS` 0x1F03).

The obvious suspicion was that the UI thread had called `eglMakeCurrent` and taken the
context, since an EGL context can only be current on one thread at a time. **Tested and
disproved:** phase 3 now re-asserts `SDL_GL_MakeCurrent` before proceeding, that call
*succeeds*, and `GL_RENDERER` is still `(NULL)`. Nothing stole the context — the offscreen
driver never produced a usable one. The re-assert is kept anyway (it is correct and
cheap), but it is not the fix for anything.

## The offscreen harness has reached its limit

All bring-up so far used `SDL_VIDEODRIVER=offscreen`, deliberately, so tests never touched
the frontend. That harness is now the constraint rather than the port:

- SDL's armhf build here has **no `dummy` video driver**; `x11` and `wayland` are absent;
  `kmsdrm` fails with *"Can't load EGL/GL library on window creation"* while ES holds DRM.
- `offscreen` logs `libEGL warning: egl: failed to create dri2 screen` twice and yields a
  context whose `glGetString` returns NULL — i.e. degenerate, not merely headless.

**Next step must be a real KMS/DRM run**, which requires stopping EmulationStation and
restarting it afterwards — never leave the device with a dead screen.

## Defensive shims added (and why they are correct, not papering over)

`strstr`/`strchr`/`strcmp` are mapped to NULL-tolerant wrappers, and `glGetString` to one
that substitutes `""` for NULL. The engine does the standard
`strstr(glGetString(GL_EXTENSIONS), "GL_OES_…")` probe with no null check. Bionic's strstr
is equally undefined for NULL, so the game never had a guarantee — it simply never hit the
case on Android. Tolerating it costs one branch, turns a libc-internal crash into a clean
"extension not found", and made the real cause visible in one run.

---

# First real display run (2026-09-07, KMS/DRM, EmulationStation stopped)

`SDL_VIDEODRIVER=kmsdrm` + `SDL_AUDIODRIVER=alsa`, framebuffer console unbound.

**`GL_RENDERER = Mali-G52`** — a real GPU context, where the offscreen harness gave NULL.
That confirms the offscreen path was the limitation, not the port.

## Two fixes this run, and one hypothesis that was wrong

**Wrong hypothesis (recorded so nobody retries it):** the NULL `JNIEnv` at `viewOnInit`
looked like `AttachCurrentThread`, which was wired to `ret0` — it "succeeds" without ever
writing `*env`. That *is* a genuine bug and is now fixed, but it was **not** this crash:
the binary md5 matched and the fault did not move.

**Actual cause:** the engine keeps a global `gJavaVM` (BSS, exported) that `JNI_OnLoad`
fills in, and `AndroidUtil_JNIInit` derives every `JNIEnv` from it. LCS boots through named
entry points rather than `RegisterNatives`, so calling `JNI_OnLoad` seemed unnecessary and
was dropped — leaving `gJavaVM` NULL. The engine's own
`_JNIEnv::CallStaticObjectMethod` wrapper then faulted on a null vtable the first time
anything called up into Java (`GetDeviceLanguage`, during `viewOnInit`).

`jni_boot_lcs()` now calls `JNI_OnLoad(fake_vm, NULL)` first — it returns `0x10006`
(JNI_VERSION_1_6) and sets `gJavaVM` — with a direct write to the symbol as a fallback.
**Android's loader always calls JNI_OnLoad; so must we, regardless of how the game boots.**

## Where boot now stops: no game data

```
CText::ReadChunkHeader -> CFileMgr::Read -> RslfRead
  43b118:  ldr ip, [r3]      ; r3 = file handle = NULL
```

`RslfRead` is the engine's file-read primitive and the handle is NULL because the file was
never opened: **the OBBs are not on the device.** Only the binary, `libGTALcs.so` and the
APK `assets/` were deployed. So this is a provisioning gap, not a code defect — the port
now boots cleanly through JNI, Social Club/HAL, GL context creation and into game-data
loading.

**Next:** copy `main.17.…obb` (1.9 GB) and `patch.15.…obb` (14 MB) to
`/roms/ports/gtalcs/`. Note `/roms` had only ~3.8 GB free, so it fits but not comfortably.

## Console handling — a real gotcha

`/sys/class/vtconsole/vtcon0` is `(S) dummy device`, `vtcon1` is `(M) frame buffer device`.
Original state on this device is **vtcon0=0, vtcon1=1**. Unbinding for KMS and restoring
afterwards *in the same script* did not stick — the state came back inverted
(vtcon0=1, vtcon1=0), leaving the framebuffer console unbound. Restoring from a **separate,
later** SSH session works reliably. Always verify the bind state after a display run;
never leave the device with the framebuffer console unbound.

---

# Game data wired up (2026-09-07, OBBs on device)

Both OBBs copied to `/roms/ports/gtalcs/` via rsync (2.0 GB, byte-exact; `/roms` down to
1.9 GB free).

## `setGameFilesDir` is NOT a directory — it is the archive path

Despite the name, the engine `fopen()`s the string verbatim. Passing the directory
`/roms/ports/gtalcs` logged `[fopen] ok: /roms/ports/gtalcs` — it opened the *directory*
(which succeeds on Linux), got a handle it could not read, and every asset then fell
through to relative paths that do not exist on disk:

```
[fopen] MISS: TEXT/ENGLISH.GXT (rb)
[fopen] MISS: texture_meta_data_alpha_blend.csv (rb)
[fopen] MISS: master_texture_mapping_process.csv (rb)
```

Pointing `setGameFilesDir` and `setFileInstalled` at
`<data>/main.17.com.rockstargames.gtalcs.obb` fixes it completely: the archive opens and
**every one of those misses disappears** — they are entries inside the OBB, resolved by
the engine's own reader. `main.c` also `chdir()`s to the data directory, since a few
lookups do fall back to relative paths.

This is the same lesson as `getFile`: hooking `fopen`/`open` to log misses turns an
opaque NULL-file-handle crash deep inside `RslfRead` into the literal path the engine
wanted. Do that first, always.

## Current blocker: the EGL capture never succeeds

`viewOnInit` still crashes with `PC=0`, and line 4 of every run explains why:

```
[egl] WARNING: no current EGL context after SDL init (display=(nil) ctx=(nil))
```

`egl_patch_capture()` calls `eglGetCurrentDisplay()`/`eglGetCurrentContext()` immediately
after `SDL_GL_CreateContext` + `SDL_GL_MakeCurrent`, and both return nil — even though a
context demonstrably exists (`glGetString(GL_RENDERER)` returns `Mali-G52` later, and
`/dev/mali0` is mapped). So `g_display`/`g_main_ctx`/`g_config` are all unset, and when the
engine asks for its shared loader context it is handed `EGL_NO_DISPLAY`, gets
`EGL_NO_CONTEXT` back, and calls through the resulting null pointer.

**Planned fix:** stop relying on EGL's current-state query. Take the context from
`SDL_GL_GetCurrentContext()` — on the EGL backend SDL's `SDL_GLContext` *is* the
`EGLContext` — and obtain the display via `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, then
recover the config with `eglQueryContext(EGL_CONFIG_ID)` as now. Re-capture at phase 3 as
well, where the context is known good.

## Boot state as of this run

`JNI_OnLoad` → `gJavaVM` set → phase 1/2 → UI thread → Social Club HAL screens →
`getFile` assets → OBB archive opened → GL context (`Mali-G52`) → **phase 3 / `viewOnInit`
→ crash on the un-captured EGL display.**

---

# EGL capture, and a lesson taken from the CTW port (2026-09-07)

## Capture had to stop trusting eglGetCurrentContext()

On this device both `eglGetCurrentDisplay()` and `eglGetCurrentContext()` return nil
immediately after SDL has successfully created and made a context current — while
`glGetString(GL_RENDERER)` reports `Mali-G52` from the *same thread*. The context plainly
exists; EGL's current-state query just does not report it to us.

`egl_patch_capture()` now takes the context from **`SDL_GL_GetCurrentContext()`** (on the
EGL backend, an `SDL_GLContext` *is* the `EGLContext`) and the display from
`eglGetCurrentDisplay()` falling back to `eglGetDisplay(EGL_DEFAULT_DISPLAY)` +
`eglInitialize`. It is also re-run at phase 3, where the context is known good.

Result: `[egl] context taken from SDL (0x…); captured display=0x… ctx=0x…` — display and
context are now real. **Still outstanding: `config_id=0 have_config=0`** —
`eglQueryContext(EGL_CONFIG_ID)` fails and the attribute-search fallback finds nothing, so
the shared-context path still has no config. The `viewOnInit` crash is unchanged, so the
EGL capture was necessary but not sufficient.

## eglGetProcAddress must return NULL — from gtacw-port

Per the standing rule to check the Chinatown Wars port first, `gtacw-port/src/main.c` (the
EGL block, ~line 887) stubs `eglGetDisplay` / `eglGetProcAddress` / `eglQueryString` to
`ret0`, with the reason recorded inline:

> stub all three so the game uses its static GOT imports, which go through our
> softfp→hardfp thunks. Real eglGetProcAddress would return hard-float pointers the game
> calls with soft-float convention.

Our `egl_patch.c` was **forwarding** `eglGetProcAddress` to the driver — precisely that
hazard. It now returns NULL and logs the name, so the engine falls back to its GOT
imports, which the symbol table resolves through the ABI-correct thunks.

**Where LCS differs and the CTW answer must NOT be copied wholesale:** CTW also stubbed
`eglGetDisplay`, because its engine let Java do all EGL setup (`InitEGLAndGLES2`). LCS
creates its own shared context natively, so `eglGetDisplay`/`eglCreateContext` must stay
real. Only the proc-address lookup is stubbed.

## Also worth carrying over when the render thread is sorted

`gtacw-port/src/jni_patch.c` (~line 408) releases the GL context from the main thread with
`SDL_GL_MakeCurrent(g_window, NULL)` before handing control to the engine, because
`eglMakeCurrent` fails with `EGL_BAD_ACCESS` if the context is still current on another
thread. LCS keeps the context on the render thread by design, so this is not a
straight copy — but if the engine ever needs *our* context on *its* thread, that is the
fix and the failure will be `EGL_BAD_ACCESS`.

---

# PortMaster package (2026-09-07)

`gta-lcs-portmaster/` — 468 KB, mirroring the CTW release layout:

```
GTA Liberty City Stories.sh   port.json   gameinfo.xml   README.md
gtalcs/{gtalcs.armhf, libclock_fix.so, libs.armhf/libz.so.1, licenses/, conf/}
```

The launcher is adapted from the CTW one and keeps its hard-won logic: the ESUDO
first-word test, `OWNS_DISPLAY` kmsdrm-vs-wayland detection, the early-armed cleanup trap,
no frontend stop (it would kill the launcher's own cgroup), `PORT_32BIT=Y`, and the
armhf-not-`$DEVICE_ARCH` library path.

**The one deliberate behavioural difference from CTW:** CTW unpacked its OBB and deleted
it to reclaim space. LCS must not — the engine opens the `.obb` as an archive and reads
from it at runtime, so first-run setup unpacks only the APK (for `libGTALcs.so` and
`assets/`) and leaves both OBBs in place. The README states the ~2 GB permanent
requirement explicitly.

Cleanup ordering matters and is encoded in `_cleanup`: kill the game and *wait for it to
exit* before rebinding the vtconsole. Rebinding while the game still holds DRM is accepted
but does not take effect — observed repeatedly during bring-up.

---

# Session 2 bring-up log (2026-09-08): past viewOnInit into the frame loop

## The viewOnInit PC=0 crash was two ABI bugs, both now fixed

**1. bionic jmp_buf vs glibc setjmp (src/setjmp_fix.c).** The stack at the crash
contained the libpng string "iCCP: known incorrect sRGB profile" and the fault was a
`pop {fp,pc}` over a zeroed return slot inside png_chunk_unknown_handling's caller.
libpng/zlib are static in libGTALcs.so and embed jmp_bufs sized for BIONIC; the
setjmp/longjmp calls import to glibc, whose jmp_buf layout writes past them and
clobbers the png_struct. Fix: self-consistent register-only setjmp/longjmp in asm
(r4-r11,sp,lr + d8-d15, 26 words — fits bionic hard-float), imports pointed at them.
Lesson logged to memory [[project-setjmp-abi]]: same family as the time_t smash —
compare the game's allocation against the host struct size.

**2. Two EGL libraries in one process (src/egl_patch.c rewrite).** Device split:
libGLESv2.so.2 -> libMali.so (GL is direct-to-vendor) but libEGL.so.1 -> glvnd with
ONLY the mesa vendor JSON. SDL is driven by /etc/profile.d/SDL_VIDEO.sh
(SDL_VIDEO_EGL_DRIVER=libEGL.so -> libMali.so), so SDL's context belongs to Mali
while our link-time libEGL was glvnd/mesa — every nil eglGetCurrentContext,
have_config=0 and "no display claims the context" was glvnd answering about an
object it does not own. Fix: dlopen(RTLD_NOLOAD) the vendor list at capture time
(finds SDL's own mapping; on pure-glvnd devices fall back to RTLD_DEFAULT, which is
then correct by construction) and route EVERY real EGL call through dlsym'd pointers
from that library. Result: config_id=2 have_config=1, eglCreateContext shares,
viewOnInit passes. [[project-egl-two-library]]

Launch over ssh MUST export SDL_VIDEO_EGL_DRIVER=libEGL.so (login profile does it;
plain ssh shells do not — "Can't load EGL/GL library" was this, not DRM).

## Heap-corruption hunt (still open)

With boot flowing, the game aborts in malloc_consolidate() ~30 s in. Probes:
- MALLOC_CHECK_=3 -> crash later, same class. MALLOC_PERTURB_=165 -> UAF fault in
  the engine's getClassCached (0xA5 table) -> first suspect: OUR
  ReleaseByteArrayElements free()'d arrays the engine re-Release's or reads after
  Release (Android GCs; we don't). FIXED with a bounded live-set (jni_patch.c) —
  run length went 30 s -> 40 s+ (20269 frames) but eventually still aborts.
- Removed the png_error/png_longjmp suppression hooks once setjmp was fixed: they
  had been papering over bug 1 and became actively wrong (swallowed error left
  libpng continuing with a bad state -> AlignPointer wild write). Keep only the
  png_warning log.
- Built src/guard_alloc.c: Electric-Fence allocator for the game's four allocator
  imports (malloc/free/realloc/calloc), enabled with GTALCS_GUARD=1, pass-through
  otherwise. Caught a SIGBUS in std::string assignment — WHICH WAS THE GUARD'S OWN
  BUG: tail placement made block starts 8-byte-misaligned when size%8!=0 (malloc
  contract violated; game ldm'd a misaligned string). Fixed: floor to 8. Also
  added alloc/free caller attribution (live table stores malloc caller; freed-ring
  of the last 2040 frees; guard_annotate() called from the crash handler).

Status: guard build DEPLOYED to device (md5 fc1bbe04...), with attribution + a
draw-call counter (g_gl_draw_calls) in the screenshot handler UNBUILT — the build
was interrupted. Next: build, run with GTALCS_GUARD=1 and read the annotated fault
(block owner + alloc_by + freed_by name the corruptor directly).

## Black screen (parallel lead)

Framebuffer readback (glReadPixels before swap, frame*.ppm + min/max/mean) is
uniformly black at 2 s..30 s — but the readback could be lying OR the engine may
draw on another context/thread. Draws-per-interval counter added to decide which.
CTW precedent checked: CTW's Mali-400 black-screen causes (GL_FIXED reinterpret,
GL_BGRA_EXT) — our glVertexAttribPointerHook passes type through untouched (no
GL_FIXED bug here); GL_BGRA_EXT is NOT handled in LCS's opengl_patch.c yet — if the
counter shows the engine draws and nothing appears, that's candidate #1 (Mali may
still reject 0x80E1 on this driver; port CTW's bgra_to_rgba wrapper if GL_INVALID_VALUE
shows in texture uploads).

## Misc facts learned

- Engine fopen()s relative 'touch_fe/%s.png', 'Textures_Shared/...' (%s/%s.png
  format strings in .rodata) — 169 misses, absent from APK assets/. On Android the
  APK installer unpacks these to the private files dir; we don't. CTW precedent:
  their installer unzipped the OBB (zip) — LCS's OBB is the RSLF container, so
  either unpack the TouchFE PNGs with the game's own code path (it may do it
  itself given a writable cwd — check where) or extract them once at install.
- Save dir: engine opens <private>/save/GTA3LCSsf9.b (miss is fine, fresh save).
- Engine calls PS2 PLACEHOLDER memory-card ctors — real game logic, good sign.
- UI thread crash (AlignPointer wild write, thread=UI) appeared with png hooks and
  disappeared when the hooks were removed — consistent with the "swallowed png
  error left bad state" analysis.
- Device: RG353V. Launch harness that works:
    cd /roms/ports/gtalcs
    echo "$PW" | sudo -S sh -c 'echo 0 > /sys/class/vtconsole/vtcon1/bind'
    SDL_VIDEODRIVER=kmsdrm SDL_AUDIODRIVER=alsa SDL_VIDEO_EGL_DRIVER=libEGL.so \
      [GTALCS_GUARD=1] timeout N ./gtalcs_r36 > log 2>&1
    # afterwards, separate ssh: echo 1 > vtcon1/bind; echo 0 > vtcon0/bind (original)
  scp fails with ETXTBSY while the game runs — kill first (pgrep, not pkill -f).

---

# Session 3 (2026-09-08): the game renders, and the frontend is navigable

## The black screen was a missing SDL_GL_SwapWindow — nothing more

`libGTALcs.so`'s 408 undefined symbols do **not** include `eglSwapBuffers`. On Android
that is not an omission: `GLSurfaceView` swaps for the renderer once `onDrawFrame()`
returns. **We are the GLSurfaceView**, and the frame loop never swapped, so no frame ever
reached the panel. One line in `jni_patch.c` after `viewOnDrawFrame`:

```c
SDL_GL_SwapWindow(g_window);
```

Confirmation is in the frame counter, not just the picture: the loop had been spinning at
~560 fps (22654 frames in 40 s) because nothing throttled it. With the swap it settles at
a vsync-locked ~59 fps, and `g_gl_draw_calls` shows the engine issuing ~1780 draws per
5 s interval.

**Check `nm -D` for `eglSwapBuffers` before assuming the engine presents its own frames.**
An Android renderer that draws but never swaps is the normal case, not the exception.

## Every "screenshot" before this was fiction — glReadPixels was failing

`glReadPixels(..., GL_RGB, GL_UNSIGNED_BYTE, ...)` returns **GL_INVALID_OPERATION (0x0502)**
on this Mali driver and writes nothing. ES2 guarantees exactly one format for the default
framebuffer, `GL_RGBA/GL_UNSIGNED_BYTE`; `GL_RGB` is an implementation-defined extra.

The buffer therefore kept whatever `malloc` returned, which is why the capture history
read as a plausible story and was entirely noise:

| what the log said | what it actually was |
|---|---|
| `min=0 max=0 mean=0.0` — "uniform black" | fresh zero pages from `malloc` |
| `min=0 max=255 mean=37.0` — "content!" | dirty heap: vertical RGB static |

Two rules that would have caught it in minutes: **check `glGetError()` after the call**,
and **poison the buffer first** (`memset(px, 0x7f, …)`) so an untouched buffer is obvious.
Both are in the code now, and the interval is `GTALCS_SHOT_MS`.

## Touch input: the frontend is the Android *touch* frontend

`GTAJNIlib.onTouchStart/onTouchMove/onTouchEnd` are the entry points. Disassembling
`onTouchStart` (+0x4f49e8) gives both the shape and the units:

```
mov  r1, r2          ; pointer id
vmov s12, r3         ; x  -> arrives in a CORE register: soft-float
vldr s13, [fp, #4]   ; y  -> first stack slot
...
vmul.f32 s14, s12, s14   ; x * screen_width
vmul.f32 s15, s13, s15   ; y * screen_height
b    AND_TouchEvent(2, id, px, py)
```

So the signature is `(jint id, jfloat x, jfloat y)` with **normalised 0..1** coordinates.
The soft-float bridge is free here: declare the prototype with `uint32_t` for the floats
and pass their bit patterns — an all-integer prototype puts them in exactly the registers
and stack slot the callee reads.

Wired in `poll_input`: the left stick / d-pad drive a virtual cursor (`CURSOR_SPEED`
screen-widths/second) and **A** presses it. For headless bring-up there are two env vars:

* `GTALCS_AUTOTAP=<ms>`  — tap the centre every `<ms>` ms
* `GTALCS_TAPS="t:x,y …"` — scripted taps at `t` ms, normalised coords read straight off
  a screenshot as `(px/w, py/h)`

## Boot path, confirmed visually

`Title ("Tap To Continue")` → tap → `Legal/EULA text` → tap → `Main menu (Start Game /
Settings / Rockstar)` → tap Start Game at (0.477, 0.760) → world load begins:
`Models/GENERIC.TXD`, `Models/Generic/WHEELS.TXD`, hundreds of PNG texture decodes.

## The `Textures_Shared/*` misses are a red herring — do not build an OBB unpacker

169 `[fopen] MISS` lines, but they are **the same ~11 basenames tried under all four of
`fonts/`, `hud/`, `menu/`, `touch_fe/`** — a search-path loop, not 169 required files.
One entry is literally `Textures_Shared/<dir>/.png`, an empty basename from a table walk.
`hud_radardiscback.png` under `fonts/` is not a real asset.

Decisive evidence they do not matter: **the frontend renders correctly** — logo, fonts,
EULA body text, menu tiles — while every one of those misses is logged. A plain-`fopen`
MISS only means the loose-file override is absent; the OBB reader serves the asset.

## my_setjmp had to be NAKED ASM — the C wrapper was silently broken

Session 2 added `src/setjmp_fix.c` for the bionic-vs-glibc `jmp_buf` size problem, and it
did fix `viewOnInit`. But `my_setjmp` was a **C wrapper** calling `_setjmp_asm(buf)`, and
that is wrong in a way that only shows up when a `longjmp` actually fires:

`_setjmp_asm` stores **its own caller's** `sp` and `lr` — the *wrapper's* frame. A later
`longjmp` therefore returns into the middle of `my_setjmp`, and `my_setjmp`'s epilogue
pops `{r4, r5, r6, r7, r8, fp, pc}` off a stack frame that died thousands of calls ago,
restoring whatever libpng has since left on that stack.

It surfaced during world load, and the symptom pointed at the wrong place entirely:

```
[png] png_warning: iTXt: CRC error
[longjmp] #1 buf=0xebd3bb60 val=1 from png_safe_error+0x5c
=== CRASH sig=11  PC = prepareForUploadPNG+0xc0  r4=00003b40  fault=00003b58
```

`prepareForUploadPNG+0xc0` is `ldr r1,[r4,#24]` on the **success** path out of
`png_image_begin_read_from_memory` (the failure branch returns 4 instructions earlier).
So the crash is in code that never saw an error — because the frame it returned into was
reconstructed from garbage.

Fix: `my_setjmp` is now naked asm that writes the buffer *before* anything else, so `sp`
and `lr` are the real caller's. Logging moved into a `setjmp_log(buf)` called after the
buffer is committed — safe, because by then the extra frame is transient. `my_longjmp`
may stay a C wrapper: it never returns and `_longjmp_asm` reinstates `sp`/`lr` from the
buffer, so its own frame is irrelevant.

**Rule: a setjmp shim can never be a C function that calls a helper.** The buffer must be
written in the frame of the original caller.

## guard_alloc.c: the instrument was corrupting the heap it was hunting

`core_malloc` returned `ptr = (tail - n) & ~7` to satisfy malloc's alignment contract, but
`core_free` recovers the block from `p + n`. With the pointer rounded down, that sum falls
up to 7 bytes short of the page-aligned tail, so `mprotect` rejected the misaligned base
(block never re-protected) and the recorded hole was off by the same amount — **a later
allocation could be handed memory overlapping a live one.**

Fix: round the *size* up instead. `ptr = tail - align_up(n,8)` is 8-aligned because `tail`
is page-aligned, and storing the padded size keeps `p + stored_n == tail` exactly. Cost:
the overflow guard trips up to 7 bytes late. Also `GTALCS_GUARD=0` now means off.

With that fixed the guard runs, and its first real report is a **false positive worth
knowing about**:

```
PC = ReadTextureMetaData+0x438   ->  ldrb r3, [r9, #1]   r9 = 0xd3d08fff
block=0xd3d08fe0 size=32  (ends exactly at the guard page)
```

The CSV parser peeks one byte past the end of its buffer. Every real allocator has that
byte mapped; only a page-guard allocator faults. Not the corruption bug — it is a read.

## Still open

* `malloc_consolidate(): unaligned fastbin chunk` — a real heap corruption, but it
  detonates **at exit** (`exit` → `_IO_file_write` → `__libc_free`), not during play. It
  did not block any of this session's progress. Deprioritised deliberately.
* Whether the world actually finishes loading after the setjmp fix.
* Save files (`save/GTA3LCSsf9.b`, `save/gta_lcs.set`) — misses are fine for a fresh save,
  but writing has not been exercised.
* PortMaster build + package refresh with everything from sessions 2 and 3.

---

# Session 3, part 2: into Liberty City

## The world-load blocker was pthread TLS stubbed to `ret0`

After the swap and setjmp fixes, "Start New Game" reached the streaming code and
then died **nondeterministically** — a different site on every run:

| run | site |
|---|---|
| world2 | `CStreaming::LoadCdDirectory+0x1a0` — `strb r5,[r4]` with `r4 = strchr(name,'.') == NULL` |
| cd2 | `SerialiseEncryptDecryptBuffer+0x34` — 8 KB decrypt running off the top of the stack |
| stat1 | `libGTALcs.so+0x436f9c`, `fault=8`, `r0=0` |
| stat2 | `memset+48` from `cMemoryManager::Allocate+0x24` — **malloc returned NULL** |
| alloc1 | `malloc_consolidate(): unaligned fastbin chunk` |

Two runs of the *same* binary crashed in different places, so it was corruption
or a race, not a data-interpretation bug. Ruling things out mattered:

* not memory pressure — 1.5 GB available, no swap in use;
* not address-space exhaustion — 386 MB mapped in a 3 GB user space;
* not an absurd allocation — an allocator warning hook (`GTALCS_ALLOC_WARN`,
  logs any request ≥ 64 MB or any NULL return) never fired.

The cause was in the symbol table, not in any hook body:

```c
{ "pthread_getspecific", (uintptr_t)ret0 },
{ "pthread_key_create",  (uintptr_t)ret0 },
{ "pthread_key_delete",  (uintptr_t)ret0 },
{ "pthread_setspecific", (uintptr_t)ret0 },
```

libGTALcs.so **imports and uses all four**. So every `pthread_setspecific`
silently discarded its value and every `pthread_getspecific` returned NULL — any
per-thread context the engine stashed came back as "not set" on every lookup,
and the streaming thread does exactly that. `pthread_key_t` is a 4-byte int in
both bionic and glibc and the signatures match, so the fix is to bind them
straight through to the real functions.

**Result: the game loads into Liberty City and stays there.** Draw calls per
interval went from ~6.6k (menu) to **240k–610k**, the framebuffer changes every
capture, and a 7-minute run showed no crash.

`ret0` is the right stub for something the engine only needs to *not fail*. It
is the wrong stub for anything with an out-parameter or a value the engine reads
back — the same lesson as the FalsoJNI "zero and NULL mean failure" rule, one
layer down. Audit for it by listing every `ret0` binding against what the
function is contractually required to write.

## Also fixed in the same pass

**`pthread_mutexattr_init`/`settype` were `ret0` too**, so the game's 4-byte
bionic attr was never written — and `pthread_mutex_init_fake` handed that
uninitialised word to glibc's `pthread_mutex_init` *as a glibc attr*. The two
encodings are unrelated (bionic: type in bits 0-3, shared at 0x10; glibc: kind
sharing the word with ROBUST 0x40000000, PSHARED 0x80000000 and the priority
bits). Now implemented honestly on the bionic side and translated at the
boundary — a recursive mutex asked for is a recursive mutex created.

**`stat`/`fstat` filled in exactly one field.** Both hooks did

```c
*(int *)((char *)statbuf + 0x50) = (int)st.st_mtime;   /* and nothing else */
```

0x50 *is* bionic's `st_mtime`, so it read as a considered translation — but
every other field, including **`st_size` at offset 48**, was left holding
whatever was on the caller's stack. bionic's 8-byte `st_size` at 48 overlaps
glibc's `st_blksize`+`st_blocks` at 48/52, so it could never have been right.
Replaced with a full `struct bionic_stat` (104 bytes, static-asserted) filled
from `stat64`/`fstat64`. As it happens the engine never calls `fstat` on this
path — the fix is correctness, not the cure.

## Do NOT wire up asset_manager.c (tried, reverted)

`src/asset_manager.c` implements all seven `AAsset*` entry points over the
extracted `assets/` tree, while the symbol table binds them to `ret0` — which
looks exactly like a bug waiting to be fixed. Binding them **broke a working
build**, crashing during boot before the first frame:

1. the engine passes **absolute** paths to `AAssetManager_open` (the first call
   is the OBB itself), and the shim prefixes the assets root, producing
   `.../assets//roms/ports/gtalcs/main.17….obb`;
2. more fundamentally, a non-NULL `AAssetManager_fromJava` changes which loader
   the engine picks. With NULL it falls through to `fopen` and its own OBB
   reader — the path that works and is complete.

The `assets/` tree is still reachable: it is served through the fake-JNI
`getFile()` up-call (`[getFile] .../assets/json/socialClubAssets.json (4295
bytes)` in the boot log). The comment in `main.c` now says all of this so the
next reader does not repeat the experiment.

## Bring-up switches now in the build

| env var | effect |
|---|---|
| `GTALCS_SHOT_MS=<ms>` | framebuffer dump every `<ms>` (**off by default** — each is ~900 KB) |
| `GTALCS_TAPS="t:x,y …"` | scripted taps at `t` ms, normalised coords |
| `GTALCS_AUTOTAP=<ms>` | tap screen centre every `<ms>` |
| `GTALCS_AUTOWALK=<ms>` | hold the left stick forward from `<ms>` — proves the in-game control path without hands on the device |
| `GTALCS_GUARD=1` | page-guard allocator (`GTALCS_GUARD=0` now means off) |
| `GTALCS_ALLOC_WARN=<MB>` | log any allocation ≥ `<MB>` or any NULL return (default 64) |

## Verified on device (session 3, end state)

* **Renders and plays.** Title → EULA → main menu → Start New Game → Liberty City.
  ~57 fps vsync-locked, 500k+ draw calls per 30 s interval once the world is streaming.
* **Input reaches the engine.** `GTALCS_AUTOWALK=70000` holds the left stick forward from
  70 s; the frame at 67 s has Toni standing beside a mailbox, the frame at 79 s is pressed
  against the side of a taxi. He walked across the street. This also proves vehicle and
  world models load — which retires the `Models/GENERIC.TXD ` red herring below.
* **Assets are fine.** The OBB is a `WAD` container XOR-encrypted with a 2-byte key
  `{0xaf, 0x66}` alternating on **absolute offset parity** (decrypt the first four bytes
  and the tag falls out). Scanning it decrypted finds `GENERIC.TXD` and `WHEELS.TXD`
  present. Note the scan under-reports — `TEXT/ENGLISH.GXT` returns 0 hits yet
  demonstrably loads — so a 0 there proves nothing; a hit proves presence.

## The one real remaining bug

After roughly five minutes in-game, a background thread faults at

```
GetFirstElement(RslElementGroup*) +0x24
  -> RslElementGroupForAllElements+0x8   ldr r4, [r5, #8]!   with r5 = NULL
```

i.e. the engine asks for the first element of a model group that is NULL. It is almost
certainly **downstream of the heap corruption that has been present since session 2** —
`malloc_consolidate(): unaligned fastbin chunk` — rather than a missing asset:

* the same site appeared in an earlier run (`stat1`), so it is a recurring landing spot,
  not a one-off;
* vehicles and pedestrians demonstrably load and render for minutes beforehand;
* the corruption abort and this NULL deref alternate between runs of the same binary.

**Next step, and it is a specific one:** `GTALCS_GUARD=1` now works (its own alignment
bug is fixed) but died at `ReadTextureMetaData+0x438` on a benign one-byte over-read
before world load. `GTALCS_GUARD_SLACK=<bytes>` (default 16) now pads the mapped region
after each block so small over-reads land in mapped memory while anything past the slack
still faults. That should let the guard survive into the world-load path for the first
time and name the corruptor directly via its `alloc_by`/`freed_by` attribution.

## The page-guard allocator does not scale to world load — use a canary instead

Two more guard bugs found and fixed while trying to point it at the corruption:

1. **`GTALCS_GUARD_SLACK` sized the block wrong.** The slack has to be inside `body`;
   computing `body = ALIGN_UP(n, page)` and then placing the pointer at
   `tail - (n + slack)` puts the user data *before* the block start, in the previous
   block's guard page. `core_malloc` handed back `PROT_NONE` memory and our own
   `ga_calloc` faulted memsetting it at offset 0. Fixed:
   `body = ALIGN_UP(n + ga_slack(), page)`.
2. **`HT_SIZE` was 2^16.** GTA's world load holds more than 65536 live allocations, and
   once the table fills every further block is untracked — which also makes `core_free`
   report **spurious DOUBLE FREEs** for blocks it never recorded. Raised to 2^20.

But the design itself is the wrong tool at this scale: a page guard costs a minimum of
two pages (8 KB) of address space per allocation, so ~200k live blocks would need
~1.6 GB inside a 3 GB user space. It is excellent for a targeted, low-volume path and
unusable for world streaming.

**Recommended next instrument: a redzone/canary allocator.** Put a magic pattern in a
16–32 byte header and footer around each block, validate both on `free`, and report the
allocating and freeing call sites on mismatch. It costs a few percent of memory instead
of 8 KB per block, and it catches the overflow **writes** that corrupt glibc's chunk
metadata — which is precisely the failure signature here
(`malloc_consolidate(): unaligned fastbin chunk`).

## The remaining crash, fully localised: a script-spawned ped with no model

`MALLOC_PERTURB_=165` is the result that redirects this investigation. The faulting
register is `r0 = 0x00000000`, **not** `0xa5a5a5a5` — so the NULL group pointer was not
read out of freed memory. **This is not a use-after-free, and probably not the heap
corruption at all.** It is a genuinely NULL model group that the engine does not check.

Reconstructing the call chain from the crash handler's stack dump (every
`libGTALcs.so+0x…` word, resolved against the dynsym table) gives the whole story:

```
CTheScripts::Process()                          +0x2b0
  CRunningScript::Process()                     +0x148
    CRunningScript::ProcessCommands100To199()   +0x24cc   <- a CREATE_CHAR-class opcode
      CCivilianPed::CCivilianPed(ePedType, unsigned modelIndex)  +0x34
        CPed::SetModelIndex(unsigned)           +0x1c
          RslAnimBlendElementGroupInit(RslElementGroup*)  +0x14
            IsElementGroupSkinned(RslElementGroup*)       +0xc
              GetFirstElement(RslElementGroup*)           +0x24
                RslElementGroupForAllElements+0x8   ldr r4,[r5,#8]!   r5 = NULL
```

So: **a mission script spawns a civilian ped whose model is not resident.**
`CPed::SetModelIndex` assumes the clump for that model index is loaded and does not
null-check — on Android the streamer always had it in memory by then.

This also explains the timing. It is not "five minutes of uptime"; it is however long it
takes the running script to reach a `CREATE_CHAR` for a model that never streamed in.
It is nothing to do with rendering: peds, vehicles and world geometry demonstrably load
and draw for minutes beforehand.

**Where to look next** (in order):

1. Instrument `CStreaming`'s model-request path — log the model index and name for every
   request and every completion, then find which index is requested but never completes.
   The name of that one model is the whole answer.
2. Check the streamer's asynchronous read path against our file layer. Everything comes
   through one `fopen` of the OBB plus the engine's own `WadArchive` reader; a short read
   or a mis-seek on a background thread would leave exactly this state — the streamer
   believing a slot is filled while the clump pointer stays NULL.
3. Only then consider whether the still-unexplained `malloc_consolidate()` abort is
   related. The perturb result argues it is a separate problem.

## What is verified, and what is not (end of session 3)

**Verified on the RG353V:**
* Both builds — `gtalcs_r36` (dev, noble sysroot) and `gtalcs.armhf` (shipping,
  bullseye/glibc-2.31) — boot, render, navigate the frontend and load into Liberty City.
* ~57 fps vsync-locked; 500k–1.2M draw calls per capture interval once streaming.
* Gamepad reaches the engine: the `GTALCS_AUTOWALK` frame pair shows the character
  walking from a mailbox across the street into a taxi.
* OpenAL-Soft opens the default device cleanly.

**Not verified — do not claim these:**
* **Audio has never been heard.** The device opens; nothing confirms sound comes out.
* **Save/load never exercised.** `save/GTA3LCSsf9.b` and `save/gta_lcs.set` miss on a
  fresh profile, which is expected, but writing has not been tested.
* **The launcher's first-run installer path was never run.** Every test this session
  invoked the binary directly inside an already-populated `/roms/ports/gtalcs`, not
  through `GTA Liberty City Stories.sh`. The APK-unpack/APK-delete branch is unexercised.
* The `malloc_consolidate(): unaligned fastbin chunk` abort is still unexplained — and
  the perturb result above shows it is a **separate** problem from the ped-model crash,
  not the same one.

Note also that `GTALCS_GUARD_SLACK` defaults to 16, so `GTALCS_GUARD=1` no longer means
byte-exact bounds checking. Set `GTALCS_GUARD_SLACK=0` for strict mode.

## PortMaster install and launcher, verified on device (session 3, addendum)

Up to this point every test had invoked the binary directly over ssh with a manual
`vtcon1` unbind, leaving EmulationStation running the whole time — so the launcher had
never executed, and the port did not appear in the frontend at all. `/roms/ports/` held
`GTA Chinatown Wars.sh`, `GTA 3.sh`, `GTA San Andreas.sh` and `GTA Vice City.sh`, but no
`GTA Liberty City Stories.sh`.

**Installed layout** (matches `port.json`'s `items: ["GTA Liberty City Stories.sh",
"gtalcs"]`, same as the CTW port):

```
/roms/ports/GTA Liberty City Stories.sh
/roms/ports/gtalcs/{gtalcs.armhf, libclock_fix.so, libGTALcs.so,
                    assets/, save/, conf/, libs.armhf/libz.so.1, licenses/,
                    main.17.….obb, patch.15.….obb}
```

Checked before installing: the first-run branch only fires when `libGTALcs.so` **or**
`assets/json/socialClubAssets.json` is missing. Both were already present, so the
`rm -f "$APK"` line cannot run and the 1.9 GB OBB is never touched.

**Launcher run** (ES stopped first, the way PortMaster does it — the script deliberately
does not stop the frontend itself, because launched from ES it lives in the frontend's
own cgroup):

```
launcher: video driver = kmsdrm (autodetected)      -> OWNS_DISPLAY=1
[jni] GL_RENDERER = Mali-G52
[jni] entering frame loop
LAUNCHER EXIT=124                                    (our timeout, as intended)
```

**Cleanup trap, measured before any intervention** — this is the part that matters,
because a launcher that starts the game but does not restore the console leaves a
handheld needing a physical reset:

```
BEFORE:      ES=inactive  vtcon0=0 vtcon1=1
AFTER-TRAP:  ES=active    vtcon0=0 vtcon1=1     <- launcher's own _cleanup
AFTER-TRAP:  game=EXITED
```

The trap killed the game, restored both vtconsoles to exactly their pre-launch state and
restarted EmulationStation unaided; the standby safety net never engaged.

**Still unverified:** the first-run **APK unpack** branch. Every test ran against an
already-extracted `libGTALcs.so` + `assets/`, so the `unzip` path, its error messages and
the APK deletion have never executed. That needs one run with a fresh `gtalcs/` folder
containing only an `.apk` and the OBBs.

---

# Session 4 (2026-09-08): input, from device feedback

First real hands-on report: sound works, the intro movie plays after Start New Game, but
**skipping the intro stops the game**, **buttons are mapped wrong**, and **touch does not
work**. Two of those three were straightforward once the CTW port was consulted, per the
standing rule.

## Button numbering comes from the APK, not the .so

The engine's button index is NOT SDL's enum order. `onJoyButtonUp` clamps with
`cmp r3,#15` and writes `joypadButtons[index]`, so it wants 0..15 in the engine's own
numbering — and the authoritative table is Java, in `classes.dex`:
`GTAGLview.getJoypadButtonFromKeyCode(int)`, a sparse-switch from Android keycodes.

Decoded with androguard (`androguard.core.dex.DEX`, walking `get_classes()` →
`get_methods()` → `get_code().get_bc().get_instructions()`, then pairing the
sparse-switch payload's keys with its targets):

| Android keycode | index | | keycode | index |
|---|---|---|---|---|
| BUTTON_A 96  | 0 | | DPAD_UP 19    | 8  |
| BUTTON_B 97  | 1 | | DPAD_DOWN 20  | 9  |
| BUTTON_X 99  | 2 | | DPAD_LEFT 21  | 10 |
| BUTTON_Y 100 | 3 | | DPAD_RIGHT 22 | 11 |
| START 108    | 4 | | THUMBL 106    | 12 |
| (MODE 110    | 4)| | THUMBR 107    | 13 |
| BUTTON_L1 102| 6 | | BACK 4        | 14 |
| BUTTON_R1 103| 7 | | MENU 82       | 15 |

Index 5 is unused. Triggers are axes, never buttons.

Why it felt scrambled rather than simply wrong: SDL's A/B/X/Y are also 0-3, so the face
buttons worked and everything else did not. SDL `BACK`(4) hit the engine's START,
SDL `START`(6) hit L1, and the whole d-pad landed three indices low —
SDL `DPAD_UP`(11) arrived as DPAD_RIGHT.

## Axes were already right

From the same dex, `Lcom/rockstargames/gtalcs/v;->run()`:

```
setJoyAxis(0, 0, AXIS_X 0)      setJoyAxis(0, 2, AXIS_Z 11)
setJoyAxis(0, 1, AXIS_Y 1)      setJoyAxis(0, 3, AXIS_RZ 14)
setJoyAxis(0, 4, AXIS_LTRIGGER 17 else AXIS_BRAKE 23)
setJoyAxis(0, 5, AXIS_RTRIGGER 18 else AXIS_GAS 22)
```

i.e. 0..5 = LX, LY, RX, RY, L2, R2 — exactly what we already send. The same method also
converts HAT_X/HAT_Y into d-pad *button* events (10/11 and 8/9), confirming the table.

Native side: `setJoyAxis` stores into an array of **doubles** (`add r2,r2,r3,lsl #3`,
`vcvt.f64.f32`), indexed by axis only — the port argument is ignored.

## Touch: there is a real touchscreen, and we were not reading it

The RG353V has a "Hynitron cst3xx Touchscreen" on `/dev/input/event1`, and the entire LCS
frontend is the Android *touch* frontend. The port had no touchscreen reader at all —
only the pad-driven virtual cursor — so the panel did nothing.

Ported from `gtacw-port/src/main.c`'s evdev MT type-B reader, with two changes: the device
is found by capability (`EVIOCGBIT(EV_ABS)` testing `ABS_MT_POSITION_X`) rather than
hardcoded to event1, and the axis ranges come from `EVIOCGABS` instead of being assumed.
On this panel they happen to be `x=0..639 y=0..479`, a 1:1 match to the screen.
`GTALCS_TOUCH_DEV` overrides the path. `ark` is already in the `input` group, so no chmod
is needed.

**Both paths now call `AND_TouchEvent(action, pointer, x_px, y_px)` directly**, the way
CTW does — the JNI wrappers only scale normalised floats and tail-call it anyway, so this
removes a soft-float boundary for free.

**The action codes are NOT CTW's.** Read them off the tail calls:

| wrapper | AND_TouchEvent action |
|---|---|
| `onTouchStart` | **2** (down) |
| `onTouchEnd`   | **1** (up) |
| `onTouchMove`  | **3** (move) |

CTW used 0=down/1=up/2=move. Copying its constants would have sent the wrong action for
every single event — the exact failure mode the "check CTW first" rule is supposed to
prevent, if applied without checking the target.

## New bring-up switch

`GTALCS_BUTTONS="t:index t:index …"` presses an engine button at `t` ms and releases it
120 ms later, so a headless run can reproduce things that need a button — skipping the
opening cutscene above all — with no hands on the device.

## The GetFirstElement(NULL) crash was an understated device memory report

`config.h` hardcoded `DEVICE_MEMORY_MB 512` on a **2 GB** device, and that value goes
straight into `setDeviceInfo` → `SetDevicePerfIndex(manufacturer, model, hardware,
memMB)`, which the engine uses to size its streaming budget (among other tiering).

Told it had 512 MB, the streamer ran a budget far below what the device could hold and
evicted constantly. The evidence is in the draw-call counter, and it is unambiguous
because the sign flips:

| `DEVICE_MEMORY_MB` | draws per interval, in-game |
|---|---|
| 512  | 333k → 163k → 131k → **37k** — decaying, the world unloading and never returning |
| 2048 | 335k → **649k → 689k** — rising, the world streaming in |

With 512 the run died in 40-80 s after skipping the intro; with 2048 it ran the whole
130 s window with **no crash**.

That closes the chain that took three sessions to walk back:

```
memory under-reported -> streaming budget too small -> models evicted as fast as
they load -> script CREATE_CHAR gets a model whose clump was evicted ->
CEntity::SetModelIndex leaves CPed::m_pClump (offset 0x60) NULL ->
RslAnimBlendElementGroupInit -> IsElementGroupSkinned -> GetFirstElement(NULL)
```

It also explains why it looked like heap corruption and why `MALLOC_PERTURB_` showed a
clean `r0 = 0` rather than `0xa5a5a5a5`: nothing was corrupt. The pointer was legitimately
NULL because the model really had been evicted.

**Fixed properly:** the value is now read from `/proc/meminfo` `MemTotal` at boot rather
than hardcoded, so it is right on any device, with `DEVICE_MEMORY_MB` as the fallback.

### Dead ends ruled out on the way (do not re-walk these)

* **CD directory truncation** — instrumented `strchr(name,'.')` at the `LoadCdDirectory`
  call site as a free per-entry counter: **4971 entries** parsed, names sane
  ("1Lift.dff" … "cleaver.txd"), and `CModelInfo::msNumModelInfos = 4900`. The directory
  is complete.
* **A streaming deadlock.** Instrumenting `sem_wait` to report waits over 2 s did find a
  12-second stall — but resolving the caller gave `StreamThread::ThreadMain+0x30`, which
  is the worker's **idle** wait for the next queued request, not a lost wakeup. The
  detector now ignores that call site. (An idle streamer while the world decays was
  itself a clue: requests were not being made, because the budget said there was no room.)
* **The `Platform::Semaphore` shim.** `Semaphore::Down()` is `ldr r0,[r0]` then `sem_wait`;
  the ctor `lgMemMalloc(4)`s a bionic-sized slot and our `sem_init` shim stores a real
  glibc `sem_t*` in it, so the indirection resolves correctly. Not a bug.
* **`SetDevicePerfIndex`'s other inputs** — the manufacturer/model/hardware strings only
  feed `strstr`/`isInFamily` device-family checks and a CPU-frequency comparison for a
  1/2/3 visual tier. Only the memory argument mattered.

### CORRECTION: the memory fix did NOT fix the crash

The section above claims the `DEVICE_MEMORY_MB` change resolved the
`GetFirstElement(NULL)` crash. **That is wrong** — device testing from
EmulationStation reproduced the identical crash with the fix in place:

```
[jni] setDeviceInfo memory = 1971 MB (from /proc/meminfo)
=== CRASH sig=11  PC = libGTALcs.so+0x436f9c  r0=00000000
     LR = libGTALcs.so+0x48955c    (GetFirstElement+0x24)
```

Same site, same NULL, same call chain down from `CTheScripts::Process`.

**How the wrong conclusion was reached, because the mistake is instructive:**
the "verification" runs used `GTALCS_BUTTONS` to press a button during the
cutscene, on the assumption that it skipped it. It evidently did not — those
runs simply watched the cutscene play out and then ran in open gameplay, which
is a *different* code path from the one that crashes. The rising draw counts
(335k → 649k → 689k) were real but measured something else entirely; they are
not evidence about the crash.

The lesson is the one already recorded in [[feedback-look-at-the-image]], in a
new costume: **a test that does not demonstrably reproduce the failure cannot
verify a fix for it.** Before claiming a fix, confirm the *unfixed* build fails
under the same harness. That check was never run here.

The memory correction is still right on its own terms — 512 MB was simply wrong
on a 2 GB device, and reading `/proc/meminfo` is the correct thing to do — but
it is not the cause of this crash and must not be described as such.

### Current state of the investigation

A trampoline hook now exists (`hook_arm_trampoline` in `so_util.c`) that patches
a function while keeping the original callable, and it is applied to
`CPed::SetModelIndex` to log the model index and force the model resident via
the engine's own exported `CStreaming::IsObjectInCdImage` / `RequestModel` /
`LoadAllRequestedModels`. It installs correctly:

```
[patch] hooked CPed::SetModelIndex @ ec495bd4 (orig via trampoline 0xf0c71000)
```

but has not yet produced a reading: both attempts ended before the first
`CREATE_CHAR` (one ran out of window during the ~2 minute cutscene; the other
stalled in the frame loop after 2 s for reasons not yet established). **The hook
is in `gtalcs_r36` only — the shipping `gtalcs.armhf` does not carry it**, so
nothing experimental is in the user's hands.

Next: get one run that actually reaches the first ped spawn with the hook
active, and read the `[ped] SetModelIndex(N) inCdImage=…` line. That names the
model and says whether the engine even believes it exists.

---

# Session 4, part 2: the real reproduction, and the OBB question

## The user's own crash log is saved

`docs/logs/2026-09-08-user-crash-after-cutscene.log` — a real
launch-from-EmulationStation run that crashed, with `gtalcs.armhf` `5e704b5f`
(touch + A/B + `/proc/meminfo`, no experimental hook). Keep it: my scripted
harness reproduces this path unreliably and theirs does not.

`docs/libGTALcs-dynsyms.txt` is the demangled dynamic symbol table, used to
resolve every crash address in these notes.

## Where the crash actually sits

It is NOT minutes into gameplay — it is at the very start of level load:

```
[jni] entering frame loop
[fopen] ok: /roms/ports/gtalcs/save/gta_lcs.set
[KDS] Has_tapped_yo
[cd] entry 1 … entry 4000            <- CD directory being read = level load starting
[sem] recovered after 8 s (StreamThread::ThreadMain+0x30, the IDLE wait)
[sem] recovered after 2 s
=== CRASH  PC = libGTALcs.so+0x436f9c  r0=0   (GetFirstElement+0x24)
```

Crash at log line 492 of 3587 (the rest is the handler's stack and maps dump).
Note the stream thread being *idle* for 8 s immediately before — during level
load, when it should be saturated.

## "Could it be that we never extracted the assets from the OBB?" — mostly no, but sharpen it

Asked by the user, and worth answering with evidence rather than reflex.

**Against:** the engine opens the OBB and reads out of it successfully — the CD
directory alone is 4971 entries with real names, and the world renders with
textures, vehicles and peds. Bulk content demonstrably comes from the archive,
unextracted, and that works.

**But the whole-run file inventory is short enough to be worth staring at:**

```
opened OK:  main.17.….obb          (the archive)
            save/gta_lcs.set
MISSing:    Models/GENERIC.TXD        x11   <- note the TRAILING SPACE
            Models/Generic/WHEELS.TXD x11   <- ditto
            Data/WEAPON_MULTI.DAT     x1
            Textures_ETC/race_arrow/racearrowenvmap.pvr x1
            Textures_Shared/{touch_fe,menu,hud,fonts}/… x178
```

The `Textures_Shared` ones are the known red herring (same basenames retried
under four directories). **The first four are not** — they are engine data, not
frontend art, and the engine retries the two `.TXD`s eleven times each. Earlier
sessions dismissed all of these together; that was too broad a brush.

Also of note: **`patch.15.….obb` is never opened at all** (0 references in the
whole run). There is no separate JNI entry point for it — the only file-related
natives are `setGameFilesDir`, `setFileInstalled`, `setPrivateFilesDir` — so it
may be genuinely optional, but it has never been ruled in or out.

## OBB container format, as far as it is decoded

* Tag at offset 0: `DAWL` — `0x4c574144` read as a LE u32, i.e. a **WAD**
  archive, matching `WadArchive::ReadDataEncrypted` in the binary.
* Obfuscation: XOR with the 2-byte key **`{0xaf, 0x66}`**, indexed by
  **absolute file offset parity** — confirmed by `SerialiseEncryptDecryptBuffer`
  (`add r4,r3,r5; and r4,r4,#1; ldrb r4,[r6,r4]; eor`) and by the header
  decoding to sensible values.
* Header: 4096 bytes — tag, version `1`, a second `1`, then all zeros.
* At 0x1000 and 0x2000 there are arrays of u32 pairs whose first member
  increments by 7-13 per entry and whose second is constant within a block
  (`0x0f` at 0x1000, `0x2e6` at 0x2000). Plausibly (name/offset, group) tables.
* **The regions those point at do NOT decode to text under the same XOR**, so
  either the payload is stored plain (the function name `ReadDataEncrypted`
  implies a plain sibling) or the tables mean something else. **The TOC is not
  yet decoded** — do not assume it is.

A name scan of the decrypted file does find `GENERIC.TXD` (x2) and `WHEELS.TXD`
(x1), but the same scan returns 0 for `TEXT/ENGLISH.GXT`, which demonstrably
loads — so the scan under-reports and a hit proves presence while a miss proves
nothing.

## The discriminator, and the instrument for it

Rather than reverse the container speculatively, one log line settles the
question. `CPed::SetModelIndex` is now hooked (via `hook_arm_trampoline`) to
print, for every ped creation:

```
[ped] SetModelIndex(<idx>) inCdImage=<0|1> preload=<0|1>
```

`inCdImage` comes from the engine's own exported
`CStreaming::IsObjectInCdImage(int)`:

* **`inCdImage=1`** → the model IS in the archive. Then this is a
  streaming/eviction problem and extracting files from the OBB would change
  nothing.
* **`inCdImage=0`** → the engine does not believe the model exists at all —
  which is exactly what a missing loose file looks like, and would mean the
  user's extraction instinct is right.

The hook defaults to **log-only**; `GTALCS_PRELOAD_PED=1` opts into the repair
attempt (`RequestModel` + `LoadAllRequestedModels` before the original runs).

## Binary inventory at save time

| file | md5 | contents |
|---|---|---|
| `gtalcs.armhf` (shipping, in package + on device) | `5ab1e6b0` | touch, A/B, /proc/meminfo, ped hook (log-only) |
| `gtalcs_r36` (dev) | `76c57dbb` | same |
| previously on device / what the user's crash log came from | `5e704b5f` | same minus the ped hook |

`scripts/devtest.sh <seconds> [--launcher] [ENV=VAL …]` is the only safe way to
run on device — it stops EmulationStation, records and restores the vtconsole
state, and restarts the frontend unconditionally. Env values must not contain
spaces (they are word-split); the tap and button parsers accept **commas** as
separators, so use `3000:0.5,0.5,8000:0.5,0.5,…`.

## ANSWER: the assets are NOT missing — model 109 is in the archive but not resident

The hook fired and settled it. Peds created successfully, in order, then the crash:

```
[ped] SetModelIndex(0)   inCdImage=1
[ped] SetModelIndex(7)   inCdImage=1   (x4)
[ped] SetModelIndex(12)  inCdImage=1   (x3)
[ped] SetModelIndex(29)  inCdImage=1
[ped] SetModelIndex(17)  inCdImage=1   (x3)
[ped] SetModelIndex(34)  inCdImage=1   (x2)
[ped] SetModelIndex(31)  inCdImage=1
[ped] SetModelIndex(109) inCdImage=1   <-- last line before the crash
=== CRASH  GetFirstElement(NULL)
```

**`inCdImage=1` for the failing model.** `CStreaming::IsObjectInCdImage(109)` — the
engine's own answer — says model 109 exists in the archive. So:

* extracting files from the OBB would change nothing; the archive is being read fine;
* the four non-frontend `fopen` MISSes above are not the cause either;
* this is a **streaming residency** problem: the script creates a ped whose model the
  streamer has not loaded (or has evicted), and `CPed::SetModelIndex` does not check.

Seventeen peds are created successfully first, so the mechanism works in general — model
109 specifically is not resident at the moment it is needed.

**Next:** `GTALCS_PRELOAD_PED=1` makes the hook call the engine's own
`CStreaming::RequestModel(idx,0)` + `LoadAllRequestedModels(1)` before running the
original, i.e. forces the model resident exactly as the script path should have. If that
clears the crash, it is both the diagnosis and a legitimate fix — the engine loading its
own model synchronously, not error suppression.

## Preload repair: built, NOT yet verified

`GTALCS_PRELOAD_PED=1` was tried once and the run hit the intermittent early stall
(frame loop enters, first framebuffer dump at 2 s, then nothing — no taps fire, no
crash dump). **Result inconclusive; do not record it as either working or failing.**

That stall has now appeared in roughly a third of harness runs and is itself unexplained.
It is distinct from the `GetFirstElement` crash: no signal, no handler output, the UI
thread keeps loading menu textures for a while afterwards. Worth chasing separately —
until it is understood, every negative harness result is suspect, and a run must be
confirmed to have *reached gameplay* (a `[shot]` line with 300k+ draws, or `[ped]` lines)
before its outcome means anything.

## Exact state at save time

Source tree `/home/mafradon/gtalcs-port`:

| file | md5 | contents |
|---|---|---|
| `gtalcs_r36` (dev, **on device**) | `76c57dbb` | touch, A/B, /proc/meminfo, CPed hook (log-only default) |
| `gtalcs.armhf` (built, **NOT deployed**) | `5ab1e6b0` | same |
| `gta-lcs-portmaster/gtalcs/gtalcs.armhf` | `5e704b5f` | **no CPed hook** — matches what is on the device |
| device `/roms/ports/gtalcs/gtalcs.armhf` | `5e704b5f` | what the launcher runs, i.e. what the user plays |

So the user's play path is unchanged and carries nothing experimental. The package copy
and the device copy agree. `gtalcs.armhf` in the build root is one step ahead of both and
has not been shipped anywhere.

Device left clean: `ES=active vtcon0=0 vtcon1=1`, nothing running.

## Where to pick this up

1. **Fix the harness stall first** — otherwise results cannot be trusted. Check whether
   the render thread is blocked (the UI thread demonstrably keeps going) and whether it
   correlates with the `sem_wait` instrumentation or the trampoline hook being installed.
2. **Then verify `GTALCS_PRELOAD_PED=1`** on a run confirmed to reach gameplay. If it
   clears the crash, promote it from an env opt-in to the default and ship it.
3. If the preload does not help, the next question is *why* model 109 is not resident:
   whether `RequestModel` was ever called for it, and whether the streamer completed or
   evicted it. `CStreaming::ms_numModelsRequested` and `ms_currentPedLoading` are already
   exported and read by `streaming_dump()` (`GTALCS_DUMP_MS`).

## 2026-09-08 session 5 — the data layer, and what the model-109 crash really is

### Loose files ARE a supported layout (earlier notes said otherwise — they were wrong)

Read out of `libGTALcs.so`, the whole read path is:

```
base::BcfOpen(name, "r", f)                       @0x231744
  -> LogicalFS_OpenBundleFile(name, f)            @0x4f7370   walks a mount list
  -> Platform::FileOpenOSFilePath(path, READ, f)  @0x4f9604   mount has no WadArchive
  -> native open of the RELATIVE path                         GetAssetManager() == NULL
```

* `_LogicalFS_Init` @0x4f75b4 **always** calls `LogicalFS_AddBundleRoot(Platform::GetBundleRoot())`,
  and `GetBundleRoot()` @0x4f92e8 returns the **empty string** — so there is always a
  directory mount and its paths are relative to the cwd.
* WADs are just extra mount points. `setGameFilesDir` @0x4f3b38 only does
  `strcpy(g_WADFilePath + g_totalWadFiles*512, s); g_totalWadFiles++`. There are
  **two** slots and both mount at `""`, so main+patch overlay natively — applying
  the patch OBB never required extraction.
* With `g_totalWadFiles == 0` no mount is attempted at all, and every logical path
  falls through to a plain relative open. `FSWadFile_VerifyMainWadCRC` has **zero
  callers**, so nothing gates on a WAD being present.
* `GetAssetManager()` @0x4f3a1c tail-calls `AAssetManager_fromJava`, which we bind to
  `ret0` — so it returns NULL and `FileOpenOSFilePath` never takes the
  `AAssetManager_open` branch. That is why the loose path reaches our `fopen` shim.

Implemented: `jni_patch.c` probes for the OBB and only calls `setGameFilesDir` when it
exists (`GTALCS_WAD=<path>` overrides); otherwise the unpacked tree under
`GAMEDATA_PATH` serves everything. **Verified on device** — `[cd] entry 4000: "cleaver.txd"`,
i.e. `CStreaming::LoadCdDirectory` parsed the CD directory out of loose files.
`Models/gta3.dir` holds **4971** 32-byte records, which is the number to expect.

`gamedata_redirect` in `main.c` is now case-insensitive with a cached per-directory
index. A WAD lookup hashes the *lowercased* path so the archive is case-insensitive;
ext4 is not, and the engine asks for `MODELS/GENERIC.TXD` (sometimes with a trailing
space) while the unpacked tree has `Models/generic.txd`.

### The crash: slot 109 is MI_SPECIAL01, and it is `vinc_01`

`CStreaming::RequestSpecialChar` @0x47cd40 is literally `add r0,r0,#109` then a tail
branch to `RequestSpecialModel` — so **MI_SPECIAL01 == 109**. Slot 109 is a *named*
cutscene/mission character slot, not a fixed ped. `IsObjectInCdImage(109)==1` therefore
proves nothing on its own: it only says the slot holds some cd position.

Instrumented (`[special]` / `[ped]` hooks in `main.c`, reading `CStreaming::mspInst`
— 20-byte records, `m_loadState` at +12, layout taken straight off
`HasSpecialCharLoaded`'s disassembly):

```
[special] RequestSpecialModel(109, "vinc_01", 0x6)
[special] before: model=109 loaded=0 loadState=0 info=... 00 00 ff ff 00 00 00 00
[special] after:  model=109 loaded=0 loadState=2 info=... 02 06 ff ff fd 84 00 00
[ped] SetModelIndex(109) inCdImage=1 loadAllGuard=0
[ped]  info: model=109 loaded=0 loadState=2                       <- still INQUEUE
=== CRASH sig=11  PC=+0x436f9c RslElementGroupForAllElements+0x8  fault=00000008
                  LR=+0x48955c GetFirstElement+0x24
```

* The request is **correct**: cd position `0x84fd` is exactly where `vinc_01.dff`
  (37 sectors) sits in `Models/gta3.dir`.
* `loadState` is **2 (INQUEUE)**, never 1 (LOADED). Ordinary peds in the same run
  reach 1 normally.
* `LoadAllRequestedModels`' re-entrancy guard (an unnamed static at **+0xae1bf8**,
  `ldrb r4,[r3,#8]` at the top of @0x47e720) reads **0** — the loader is not being
  skipped. It runs and still does not service this request.
* `GTALCS_PRELOAD_PED=1` (forcing `RequestModel(109)` + `LoadAllRequestedModels(0)`
  from the hook) leaves `loadState` at 2. **The preload does not work** — that idea
  from the previous session is now tested and dead.

So: the request is queued with a valid archive position, the loader runs, and this
request is never serviced. Next split is which half fails — the request-list walk
(`GetNextFileOnCd`) or the read/convert (`ConvertBufferToObject`).

**`GTALCS_LOADER_HOOKS=1` is opt-in and currently DESTABILISING — cause not identified.**
Trampolining those two moves the crash earlier: first as a bad `free()` on the first
`ConvertBufferToObject` (identical registers across two runs), and after fixing the log
predicate, as a SIGABRT shortly after `GetNextFileOnCd -> 123`.

What has been ruled out:

* **Not the trampoline.** Both functions were checked in the full disassembly: the only
  branches to their entry points are external `bl` call sites, and every `pop` matches
  the entry `push`. `hook_arm` writes exactly the 8 bytes `hook_arm_trampoline` copies.
* **Not (only) log volume.** The first version's predicate was `>= 109`, which matches
  almost every model id, so the 120-line cap never applied — but the crash arrived after
  just two logged lines, so a flood was not the trigger. The bound is 109..129 now
  (there are 21 special slots); model 6103 was mislabelled `(SPECIAL)` because of it.

Still open, and the next thing to try: hook the two functions **one at a time** to find
which is guilty, and log without calling back into engine code (`log_streaminfo` calls
`HasSpecialCharLoaded`, i.e. re-enters `CStreaming` from inside a `CStreaming` call).

### Correction + the priority-flag lead (same session)

Two claims made earlier in this session were **wrong**, both corrected from the
`GTALCS_LOADER_HOOKS=1` run's log:

* "No special/cutscene model ever loads" — false. Slot **123 (`cscoach`) loads fine**:
  `[cdnext] GetNextFileOnCd(lastPosn=0, prio=1) -> 123` then `[convert] after:
  model=123 loaded=1 loadState=1`. The special path works.
* `ConvertBufferToObject` is not implicated by that run: its post-call log line *did*
  print, so the hooked call returned normally. The SIGABRT came afterwards. Which of
  the two loader hooks destabilises the run is still unknown.

**The lead.** Sort the requests by their flags:

| slot | name | flags | result |
|------|------|-------|--------|
| 120–125 | `cstoni_a`, `cssuitcase`, `cstaxi`, `cscoach`, `csphone`, `csphonebooth` | **0xe** | load |
| 109 | `vinc_01` | **0x6** | stuck INQUEUE |
| 0 | `plr3` | 0x6 | already resident |

0xe carries the **priority** bit (0x8); 0x6 does not. And `GetNextFileOnCd` was called
`prio=1` for 5 of 63 calls — every successful special pick among them, and it never
returned 109.

In this engine family `GetNextFileOnCd` self-cancels the filter with
`if (priority && ms_numPriorityRequests == 0) priority = false;`. So a **leaked
`ms_numPriorityRequests`** — incremented on request, never decremented on completion —
makes the streamer serve priority requests only, forever, starving exactly the
non-priority ones. That fits every observation: valid `cdPosn`, linked into a list,
`loadState` stuck at 2, loader running, guard clear, and `LoadAllRequestedModels(0)`
having no effect.

`ms_numPriorityRequests` is exported (0x00ae1c7c) and already read by
`streaming_dump()` in `jni_patch.c`. It is now called from the `CPed::SetModelIndex`
hook whenever a special slot is used while unloaded — read-only, no trampolines, no
hot-path hooks. Non-zero at that instant confirms the leak; zero kills the theory.
**Built, NOT yet run on device.**

### RETRACTED: "the request is not on the list"

That heading was wrong and is left here only so the mistake is not repeated. The first
list walker had `m_next` at +0 when it is at **+4**, so it followed `m_prev` backwards
from the tail, hit a sentinel after one node, and reported a one-element list. Walking
forwards gives:

```
[reqlist] head->next=0x519d998 tail=0x51bb68c
  [0] model 79   loadState=2 flags=0x16
  [1] model 5024 loadState=2 flags=0x16
  [2] model 152  loadState=2 flags=0x16
  [3] model 5987 loadState=2 flags=0x6
  [4] model 109  loadState=2 flags=0x6     <- vinc_01, correctly queued
  [5] model 182  loadState=2 flags=0x4
```

Six entries, all INQUEUE, terminating cleanly at the tail sentinel — and
`ms_numModelsRequested` reads exactly **6**. The counter and the list agree. (The
`[queued]` census finds only 4 of them because it is bounded by
`CModelInfo::msNumModelInfos` = 4900 and two entries are above that — a scan-bound
artifact, not an engine inconsistency.)

**So the request queue is healthy and model 109 is properly on it.**

### What is actually established about the vinc_01 crash

Everything about the request is correct:

* on the request list, in position 4 of 6, `loadState = 2` (INQUEUE)
* `GetCdPosnAndSize` returns `cdPosn=0x84fd cdSize=37 ok=1` — exactly `vinc_01.dff`
* `ms_channelError = -1` (no error), `ms_disableStreaming = 0`, guard clear

Killed by measurement, do not revisit: **priority starvation** (`ms_numPriorityRequests
== 0`; `GetNextFileOnCd` is called `prio=0` on 58 of 63 calls), **zero cd size**, **the
request being misfiled**, and **the soft-float ABI** (every function on this path is
integer/pointer only, and a float fault would corrupt `cdPosn`/`cdSize`, which are
intact).

**The one open anomaly, and the next thing to chase:** with the list non-empty,
`LoadAllRequestedModels(0)` called from the `CPed::SetModelIndex` hook returned
**without calling `GetNextFileOnCd` once** — while the same run made 130 such calls
elsewhere. The loop's entry test is
`[*mspInst+0x1e33c] != *mspInst+0x1e324`, which the dump above satisfies, so it should
have been entered. Either something between `FlushChannels` @0x47e764 and the call site
@0x47e7c4 bails out, or the drain is being entered and exiting per-request. Disassemble
0x47e768–0x47e7c4 (`CdStreamGetLastPosn`, `GetCdImageOffset`) and instrument there.

**CStreamingInfo, final:** 20 bytes, array at `*mspInst + 4`:
`+0 m_prev, +4 m_next, +8 m_loadState, +9 m_flags, +10 u16, +12 m_cdPosn, +16 m_cdSize`.
Node address = `*mspInst + idx*20 + 4`.

**Hook attribution:** `GTALCS_LOADER_HOOKS=cdnext` (GetNextFileOnCd alone) is stable and
reaches the normal crash; `convert` (ConvertBufferToObject) is what destabilises the run.
The env takes `cdnext` / `convert` / `1`.

## 2026-09-08 session 5 (cont.) — the vinc_01 chain, solved down to one link

### The bug that was hiding all the others: a `void` hook on an `int` function

`CStreaming::ConvertBufferToObject` **returns int**, and `LoadAllRequestedModels` tests it:

```
47e8ec:  bl   ConvertBufferToObject
47e8f0:  subs r8, r0, #0
47e8f4:  beq  47e964          ; 0 == conversion failed
47e990:  strb r8, [r3, #8]    ; clear the re-entrancy guard
47e994:  b    47e750          ; RETURN — abandoning every remaining request
```

Our hook declared it `void`, so `r0` held whatever the C function happened to leave
there and the engine read success/failure **at random**. That is the whole explanation
for the "hooking ConvertBufferToObject destabilises the run" mystery — the bad `free()`,
the SIGABRTs, all of it. With the signature corrected, both loader hooks are stable.

Lesson: before trampolining an engine function, check whether the **caller uses the
return value**. `objdump` the call site, not just the callee.

### One failed conversion starves the entire queue

That `beq` is the mechanism behind the whole blocker:

1. `vinc_01` (slot 109) is requested correctly — on the request list, `loadState=2`,
   `cdPosn=0x84fd cdSize=37`, exactly its `Models/gta3.dir` entry.
2. Every drain aborts before reaching it, because `ConvertBufferToObject(182)` returns 0.
   Model **182 is `toyz.dff`** (cdPosn 0x7f11, size 38 — matches the directory).
   It failed **219 times** in one run; nothing else ever failed.
3. So nothing behind it in the queue ever loads, `vinc_01` stays INQUEUE forever, the
   script creates the ped anyway, and `CPed::SetModelIndex` hands a NULL clump to
   `GetFirstElement`.

### Why toyz fails: its texture dictionary has no archive entry

A model's TXD sits at **model index + 4872** in the streaming index space. Validated
against the directory, not assumed — slot 5029 = `POLICE.TXD` (model 157 = `police.DFF`),
5040 = `RUMPO.TXD` (168 = `rumpo.dff`), 5069 = `TRAIN.TXD` (197 = `train.dff`), and five
more, all exact. Every model that converts OK has its TXD loaded.

Model 182 needs slot **5054 = `TOYZ.TXD`**, and at the moment of failure that slot reads:

```
loadState=1  cdPosn=0x0  cdSize=0  posnAndSize=0  inCdImage=0
```

Marked LOADED while having **no archive entry at all**. The engine does that to itself:
when `GetCdPosnAndSize` returns 0 the loop marks the slot LOADED and skips it (@0x47e864).

And the data is not missing. `TOYZ.TXD` is in `Models/gta3.dir` at 0xd9e7, size 17, and
its bytes in `gta3.img` start `16000000…` — chunk type 0x16, a valid texture dictionary.
`LoadCdDirectory` even **parses the entry** (`GTALCS_CD_TRACE=toyz` shows
`[cd] TRACE entry 4888: "TOYZ.TXD"` alongside `entry 3596: "toyz.dff"`).

**So the one remaining link is: the parsed `TOYZ.TXD` entry never reaches TXD slot 5054.**
Not a case problem — every working vehicle also has an UPPERCASE `.TXD`
(`ESPERANT.TXD`, `KURUMA.TXD`, `RUMPO.TXD` …), and our `strcasecmp`/`strncasecmp`
bindings are the real libc ones.

Next: find the name→slot step in `LoadCdDirectory` (around the `strchr` call site at
+0x478350) and see what it does with entry 4888 — most likely a TXD-store lookup/add
that returns -1. A census of unregistered slots in 4900..6399 would say whether this is
one entry or hundreds; `log_txd_census()` exists for that but has not produced a reading
yet.

### Dead end worth recording

`GTALCS_SKIP_BAD_CONVERT=1` makes the hook report success so the drain continues past an
unconvertible model. It does stop the abort — but the model is then LOADED with no clump,
and the game crashes in `CVehicleModelInfo::CreateInstance()+0x44` when it spawns a toyz.
Confirms 182 is a vehicle and confirms the chain, but it is not a fix. The skip is sticky
(a `g_poisoned` set), because without that the engine re-requests the model — 17327
conversions in one 90 s run.

### Boot-time heap corruption (SEPARATE bug, appeared late in the session)

After roughly ten runs that reached the `vinc_01` crash normally, every subsequent run
started aborting much earlier — during memory-card init, well before
`CStreaming::LoadCdDirectory` (no `[cd] entry` lines at all):

```
****PS2 PLACEHOLDER : CMemoryCard::CMemoryCard()****
malloc_consolidate(): unaligned fastbin chunk detected
```

**Not caused by the instrumentation.** Attribution was tested, not assumed:
`GTALCS_PED_HOOKS=0` installs *no* engine trampolines at all and the abort reproduces
identically. It also reproduces with the loader hooks off, with the TXD census removed,
with `save/gta_lcs.set` present and absent, and with plenty of disk (1.7 G) and RAM free.
The device's `gamedata` is complete — 10135 files, exactly matching the local extraction.

**`MALLOC_PERTURB_=165 MALLOC_CHECK_=3` produces a readable fault** — but note this is a
*different* crash, not necessarily the same bug:

```
=== CRASH sig=11  fault=a5a5a5b5  r2=a5a5a5a5 r3=a5a5a5a5
  PC: libGTALcs.so + 0x534c9c   getClassCached(char const*)+0x40
  LR: libGTALcs.so + 0x5358e4   getClassAndMethod(...)+0x2c
```

`getClassCached` walks a `std::map` keyed by the name **pointer** (`ldr r2,[r3,#16]`,
children at +8/+12), and the faulting value is the *node pointer itself* holding the 0xA5
freed-memory pattern — a use-after-free of the JNI class-cache map's nodes.

**Do not assume these are one bug.** The unperturbed signature is
`malloc_consolidate(): unaligned fastbin chunk`, which is a *write* past a chunk boundary
detected later at consolidation time; the corrupting write happened earlier and
elsewhere. The 0xA5 read may be a downstream victim of that corruption, or a separate
benign-in-practice UAF that `MALLOC_CHECK_=3` promoted into a fault. Two observations in
the same area; the causal link is unproven.

Ruled out as the cause while looking: our `pthread_create_fake` correctly ignores the
bionic `attr` rather than handing it to glibc, and `gd_index`'s realloc/strdup loop has
no off-by-one (`cap` is only advanced after a successful realloc).

One concrete latent bug found while looking and **now fixed**: `FindClass` in
`jni_patch.c` stored the **caller's** `name` pointer in its `seen[]` table rather than a
copy, so every later `strcmp(seen[i], name)` depended on the engine keeping those strings
alive and unmodified forever. It now `strdup`s. This is correct on its own merits but is
**not** claimed as the fix for the abort — expect it to change nothing.

The variable that actually differs from this session's earlier working runs is
**loose-file mode itself**: every asset read now goes through `gamedata_redirect`, which
allocates (`opendir`/`readdir`/`strdup`/`realloc`) on both the main and streaming threads.
The discriminating test is a run with `GTALCS_WAD=<archive>` against a real OBB — which
needs ~1.9 G free on `/roms` (1.7 G available), so it needs the user's decision on space
first.

**Rate at session end: 7 of 7 runs aborted before streaming** (6 × sig 6, 1 × sig 11),
none reaching a single `[cd] entry` line. Earlier the same evening roughly ten runs in a
row reached `vinc_01` normally, so something drifted rather than being configuration.

The `FindClass` strdup fix was deployed and tested: **2 more runs, no change** — as
predicted. It is a real fix for a real lifetime bug, and it is not this bug.

Next session should start here — a boot that aborts before streaming makes every
`vinc_01` experiment unrunnable. `MALLOC_PERTURB_=165 MALLOC_CHECK_=3` is the tool, and
`GTALCS_WAD=<archive>` against a real OBB is the one experiment that would separate
loose-file mode from everything else.

## 2026-09-09 session 6 — the "won't start from the frontend" bug was the LAUNCHER

**Symptom reported:** launching from EmulationStation never reached the menu.
**Cause:** not a crash at all. `GTA Liberty City Stories.sh` hard-required the OBB:

```
MAIN_OBB="$(ls "$GAMEDIR"/main.*.com.rockstargames.gtalcs.obb ...)"
if [ -z "$MAIN_OBB" ]; then die "ERROR: no main.*...obb found in $GAMEDIR" ; fi
```

The device has no OBB any more — it holds the **unpacked `gamedata/`** tree instead
(2.0 GB, 10135 files). `jni_patch.c` has supported both layouts since the LogicalFS
work: it probes for the OBB and, finding none, registers no WAD and lets
`gamedata_redirect` serve loose files. The launcher never learned about the second
layout, so it killed a perfectly runnable install and printed a 20-second message.
`gtalcs.log` said so in plain text; nothing else had to be debugged.

Fixed: the check now accepts **either** a main OBB **or** a non-empty `gamedata/`,
and logs which one it picked (`launcher: data = unpacked gamedata/`). Verified on
device via `scripts/devtest.sh 75 --launcher` and by the user launching from ES.

**Lesson: when the frontend path fails and the dev path works, diff the launcher's
preconditions before touching the engine.** The two entry points had drifted apart —
`devtest.sh` runs `./gtalcs_r36` directly and never evaluates any of those gates.

### State after the fix

* The **boot-time heap abort** (`malloc_consolidate(): unaligned fastbin chunk`
  in `CMemoryCard::CMemoryCard`) that ended session 5 at 7/7 runs **does not
  reproduce**. It is not the live bug; leave it recorded but do not chase it.
* The **live crash is the known post-cutscene one**, unchanged and confirmed twice
  today (launcher run + the user's own ES run):
  `PC = +0x436f9c RslElementGroupForAllElements+0x8`,
  `LR = +0x48955c GetFirstElement+0x24`, `fault=8` — the NULL clump from the
  `vinc_01` / `toyz.dff` chain.
* **The OBB-vs-loose-files question is settled by experiment, not by reasoning:**
  the user ran it both ways and the behaviour is identical. Session 5's "untested,
  needs a decision on disk space" paragraph is stale — loose-file mode is NOT the
  cause of anything.

### Session 5's "+4872" rule is wrong, but its measurement of slot 5054 still stands

`LoadCdDirectory` @0x4783a4 shows the real mapping for a `.TXD` directory entry:

```
4783a4:  mov  r0, r6                       ; entry name, extension already NUL'd
4783a8:  bl   CTexListStore::FindTexListSlot
4783ac:  cmn  r0, #1
4783b4:  beq  47851c                       ; -1 -> AddTexListSlot, then rejoin
4783b8:  add  r3, r0, #4864                ; 0x1300
4783bc:  add  r3, r3, #36                  ; +36   => STREAM_OFFSET_TXD = 4900
4783dc:  bl   CStreamingInfo::GetCdPosnAndSize
4783e0:  bne  4782f4                       ; already positioned -> skip this entry
478404:  bl   CStreamingInfo::SetCdPosnAndSize
```

So the stream slot of a TXD is **`FindTexListSlot(name) + 4900`**, and the index is a
`CTexListStore` slot — NOT a model index, and the offset is 4900 (== `msNumModelInfos`),
not 4872. Sibling paths confirm the shape: COL is `FindColSlot + 6100`, IFP is
`RegisterAnimBlock + 6115`.

**But do not conclude session 5 read the wrong address.** For vehicles declared in
DEFAULT.IDE in id order, the texlist slot runs a constant behind the model id, and
`182 - 28 = 154`, `154 + 4900 = 5054` — the very slot session 5 measured. Two wrong-looking
routes to the same address. `cdPosn=0 cdSize=0 inCdImage=0 loadState=1` at 5054 remains a
live observation of what may well be the right slot. What is now unknown is only whether
the model and the directory agree on *which* slot, and that is measurable directly.

**Session 5's stated "remaining link" is dead as a mechanism.** It read: "the parsed
TOYZ.TXD entry never reaches slot 5054." It cannot simply fail to reach a slot — when
`FindTexListSlot` returns -1 the code calls `AddTexListSlot` and continues down the same
path. Every `.TXD` entry in the directory ends up positioned in *some* slot. The question
is which one, and whether it is the one the model points at.

`ConvertBufferToObject` reads the model's own txd index — a **signed halfword at
`CModelInfo+0x22`** — and indexes `CTexListStore` (28-byte entries), failing when that
slot's dictionary pointer is NULL:

```
47d718:  ldrsh r0, [r7, #34]        ; CModelInfo+0x22 = m_txdIndex
47d728:  ldr   lr, [r1, #4]         ; CTexListStore
47d734:  rsbge lr, r0, r0, lsl #3   ; idx*7 ...
47d740:  addge lr, r3, lr, lsl #2   ; ... *4  => 28-byte entries
47d744:  ldr   lr, [lr]
47d748:  cmp   lr, #0
47d74c:  beq   47d9f4               ; NULL dictionary -> the failure return
```

**`AddTexListSlot` has two callers** — `CBaseModelInfo::SetTexList` (IDE parse) and
`LoadCdDirectory` — so the store is populated from both sides. Whichever runs first
creates the slot and the other finds it. The failure mode that would break toyz
specifically is **two slots for one logical txd**, created by two callers that disagree
on case or normalisation. That is one measurement away, not a theory to argue about.

Static facts to hold against the next measurement:

* `Data/DEFAULT.IDE:247` — `182, toyz, toyz, car, PONY, TOYZ, van, ignore, ...`
  i.e. model name `toyz`, **txd name `toyz` (lowercase)**. The archive entry is
  **`TOYZ.TXD` (uppercase)**. The same lowercase-IDE / uppercase-archive split holds for
  police(157), rumpo(168), train(197) — vehicles that DO load — so case alone is not the
  discriminator, but the store's *lookup semantics* were never tested and must be.
* `Models/gta3.dir` has 4971 entries, 1082 of them `.TXD`, **no duplicate names**;
  `toyz.dff` is entry 3595 (0x7f11, 38) and `TOYZ.TXD` is entry 4887 (0xd9e7, 17).
* `LoadCdDirectory`'s DFF branch explicitly skips model ids **109** and **120**
  (@0x47844c/0x478458), routing them to `CDirectory::AddItem` instead of a stream slot.
  109 is `MI_SPECIAL01` — the `vinc_01` slot. Special models are positioned by name at
  request time, not by the directory scan. Not yet chased; noted so it is not rediscovered.

**Everything above is read-only and callable.** The engine exports
`CTexListStore::FindTexListSlot`, `AddTexListSlot`, `GetTexListName`,
`CModelInfo::GetModelInfo(char const*, int*)` — so the next run should *ask the engine*
rather than compute offsets by hand. That is the whole reason this bug has survived two
sessions of arithmetic.

## 2026-09-09 — SOLVED: the `vinc_01` crash was our `_ctype_` table, off by one

Two sessions of streaming forensics, and the bug was three lines away from the loader:
a wrong pointer convention for the **bionic `_ctype_` table** that `libGTALcs.so`
imports (`nm -D` shows `U _ctype_` — a bionic symbol; glibc has no such thing, so we
supply it).

### The mechanism, end to end

`main.c` defined a correct 257-byte table with `[0]` reserved for EOF and character `c`
at index `c+1`, then handed the engine a pointer to `android_ctype_table + 1`
"so that `ptr[c]` works". But bionic's ctype macros — which are **inlined into
libGTALcs.so** — index as `ptr[c + 1]`. `CTexListStore::FindTexListSlot` @0x48b828:

```
48b828:  add   r2, ip, r3        ; ip = _ctype_, r3 = char
48b82c:  sub   r4, r3, #32
48b830:  ldrb  r2, [r2, #1]      ; flags = ip[c + 1]
48b834:  tst   r2, #2            ; _L : is it lowercase?
48b83c:  uxtbne r3, r4           ; ... then fold to uppercase
```

So every ctype lookup in the engine read the flags of character **c+1**. For the
lowercase test that is harmless for `a`..`y` (their successors are also lowercase) and
wrong for exactly one letter: **`z`**, whose successor `{` is punctuation. `z` never
folded, so the store's case-insensitive compare failed on any name containing a `z`.

`DEFAULT.IDE:247` declares vehicle 182 as `182, toyz, toyz, ...` (lowercase), while the
archive holds `TOYZ.TXD` (uppercase):

1. IDE parse → `CBaseModelInfo::SetTexList("toyz")` → `AddTexListSlot` → **slot 154**,
   and `m_txdIndex = 154` is written to `CModelInfo+0x22`.
2. `LoadCdDirectory` → `FindTexListSlot("TOYZ")` → compare fails on the `z` → `-1` →
   `AddTexListSlot("TOYZ")` → **slot 1084**, and the archive position
   (`cdPosn=0xd9e7 cdSize=17`) is written to *that* slot's streaming info.
3. Model 182 points at slot 154, which has no archive position, so its dictionary
   pointer stays NULL and `ConvertBufferToObject(182)` returns 0 (@0x47d744/0x47d74c).
4. A 0 return makes `LoadAllRequestedModels` abandon **every remaining request**
   (@0x47e8f0), so `vinc_01` (109) never leaves INQUEUE.
5. The script creates the ped anyway → `CPed::SetModelIndex` → NULL clump →
   `GetFirstElement` → `RslElementGroupForAllElements+0x8`, fault=8.

`toyz` is the only vehicle txd name in the game containing a `z`. That is the whole
reason exactly one model out of thousands failed, 219 times per run.

### Measured, both ways (the A/B)

`GTALCS_CTYPE_OLD=1` restores the off-by-one deliberately, so the failure reproduces on
demand. `txd_boot_check()` fires once from the frame loop as soon as the CD directory is
parsed — no gameplay needed to see the split.

```
--- GTALCS_CTYPE_OLD=1 (the bug) ---
[txd] model 182: info=0xecca0340 m_txdIndex=154 msNumModelInfos=4900
[txd]   m_txdIndex ->  slot=154 name="toyz" stream=5054 cdPosn=0x0    cdSize=0
[txd] FindTexListSlot("toyz")=154  FindTexListSlot("TOYZ")=1084   *** TWO SLOTS, ONE TXD ***
[txd]   uppercase ->   slot=1084 name="TOYZ" stream=5984 cdPosn=0xd9e7 cdSize=17
exit=139 (SIGSEGV)

--- fixed (default) ---
[txd] model 182: info=0xece50340 m_txdIndex=154 msNumModelInfos=4900
[txd]   m_txdIndex ->  slot=154 name="toyz" stream=5054 cdPosn=0xd9e7 cdSize=17
[txd] FindTexListSlot("toyz")=154  FindTexListSlot("TOYZ")=154
[special] RequestSpecialModel(109, "vinc_01", 0x6) ... loadState=2
[ped] SetModelIndex(109) ... loaded=1 loadState=1 cdPosn=0x84fd cdSize=37   <- LOADS
zero "[convert] ... FAILED" lines in the whole run
```

Census both ways: 1082 `.TXD` entries, 0 not in the store, 0 without an archive
position — the duplicate is a single extra slot, not a systemic registration failure.

Session 5's reading of **stream slot 5054 was the right address after all** (texlist 154
+ 4900 == model 182 + 4872, by coincidence). What it could not see was the *second* slot
holding the data.

### Still open, and NOT the same bug

With the fix in, the run gets further than it ever has and then aborts:

```
free(): invalid size
=== CRASH sig=6 si_code=-6 ===   PC in libc raise/abort
```

This is a heap fault **after** gameplay begins, not the `GetFirstElement` SIGSEGV and not
obviously session 5's `malloc_consolidate(): unaligned fastbin chunk` at memory-card init.
Treat it as the next bug, on fresh evidence.

### A SECOND suspected fault in the same table — verify before changing

`android_ctype_table`'s comment claims bionic flags `_C=0x08 _S=0x20`. The BSD/bionic
header those macros come from has them the other way round: `_S=0x08` (space),
`_C=0x20` (control). If that is right then in our table control characters carry the
*space* bit and `' '` carries the *control* bit — i.e. `isspace(' ')` is false and
`isspace('\n')` is true, for every ctype query the engine makes. It has not been changed,
because it is not implicated in this crash and a blind edit to a table that now demonstrably
works would be reckless. Verify against a bionic `ctype.h` (or a call site in the .so that
tests 0x08/0x20) and A/B it separately.

### The lesson

**A hand-written libc table needs its *indexing convention* verified against a call site,
not just its contents.** The table was correct. The pointer was one byte off, and the
symptom surfaced 500 KB away in a streaming queue as a model that would not load.
`objdump` the consumer.

### Attribution + a correction to this session's own claim

Re-run with the diagnostics gated OFF (`GTALCS_TXD_DIAG` unset, no `[txd]` lines in the
log) to make sure the new abort was not our own instrumentation:

* run 1: `free(): invalid size`, after reaching gameplay and loading 109
* run 2: `malloc_consolidate(): unaligned fastbin chunk detected`, before reaching 109
* run 3: same as run 2

**0 `[convert] ... FAILED` lines in every run** — the toyz/ctype fix holds regardless of
how far the run gets.

So the abort is **not** the diagnostics, and the dominant signature is session 5's
`malloc_consolidate(): unaligned fastbin chunk`, not the `free(): invalid size` seen once.
That also **retracts the claim made earlier in this session** that the session-5 boot
abort "does not reproduce" — it does. It is intermittent, and the runs it was judged on
happened to be ones that got past it. Two different glibc messages from three runs is the
ordinary signature of a single heap corruptor writing past a chunk.

**This is now the blocker**, and it is a pre-existing bug: session 5 recorded the identical
message before the ctype change existed. Session 5's ruled-out list still applies (not the
trampolines, not `pthread_create_fake`, not `gd_index`, not loose-file mode — the user has
since run OBB and loose files with identical behaviour). `MALLOC_PERTURB_=165
MALLOC_CHECK_=3` remains the tool, and its `getClassCached()` 0xA5 fault the best lead.

### Correction: "heap corruption is now the blocker" was wrong — the runs were idle

The three aborts above were **unattended** runs: nothing drove the game, so it sat on the
intro/attract screens. A driven run (`GTALCS_AUTOTAP=1500`, 120 s, 69 taps) behaves
completely differently:

* 0 `[convert] FAILED`
* **178 `[ped] info:` model loads**, and slot 109 (`vinc_01`) reaches
  `loaded=1 loadState=1 cdPosn=0x84fd cdSize=37` **twice**
* no heap message at all during play
* the only abort is on the way OUT: `JNI: QuitApp — exiting` followed by
  `munmap_chunk(): invalid pointer` — the auto-tap eventually pressed a quit item, so
  this is a **shutdown-path** bug in our teardown, not a gameplay crash

So the honest statement is: the `vinc_01`/toyz crash is fixed and the game plays past it;
what remains is (a) an abort in the exit path and (b) an intermittent heap abort seen when
the game is left idle on the front screens. **Do not repeat the mistake that produced this
paragraph — an unattended run that never enters gameplay is not evidence about gameplay.**
See [[feedback-reproduce-before-verifying]]; the same rule applies to judging a fix as to
judging a bug.

## 2026-09-09 (cont.) — exit path, ctype flags, and a canary allocator

### 1. FIXED: `munmap_chunk(): invalid pointer` when quitting

`CallVoidMethodV`'s `QUIT_APP` case ended in `exit(0)`, which runs the static destructors
`libGTALcs.so` registered through `__cxa_atexit` — handing bionic-allocated pointers to
glibc's `free()`. Now `fflush(NULL); SDL_Quit(); _exit(0);`.

`SDL_Quit()` is kept (it hands back the DRM/KMS mode) and a log line was added *after* it
purely to attribute the abort: it prints, so `SDL_Quit` was never the problem and the
destructors were. Verified — a driven run reached the quit item and exited **0** with no
crash block and no heap message, and the user confirmed quitting through the menu.

### 2. FIXED: `_S`/`_C` swapped in `android_ctype_table`

The table's flag comment claimed `_C=0x08 _S=0x20`; BSD/bionic define `_S=0x08`,
`_C=0x20`. Settled from the binary rather than from memory: disassembling the whole `.so`
and tabulating every ctype-idiom flag test gives exactly two flags in use —

```
tst #2  (0x02)   31 sites   case folding (tolower/toupper)
tst #8  (0x08)    2 sites   both inside std::__convert_to_v — i.e. isspace
```

`_L=0x02` identifies the header as the BSD-style one, which fixes `_S=0x08`/`_C=0x20`, and
the `#8` sites corroborate it. So `isspace(' ')` was false and control characters read as
space. Corrected (control `0x08`→`0x20`, space `0xa0`→`0x88`, DEL likewise) and verified by
compiling the table into a host test that checks every classification and the full a–z /
A–Z fold. **Blast radius is exactly those two STL sites** — nothing else reads that flag.

### 3. NEW TOOL: canary/redzone mode in `guard_alloc.c` (`GTALCS_CANARY=1`)

Built as the notes' own guidance requires — page-guarding costs 8 KB of address space per
block and cannot instrument a streaming game.

```
[ hdr sizeof(can_hdr) ][ user n bytes ][ pad to 8 ][ ftr 8B ]
```

Header magic is keyed to the block's own address (a foreign pointer cannot collide), the
allocation call site and an ordinal are recorded, and both canaries are validated on free.
`GTALCS_CANARY_SWEEP=<n>` walks the whole live list every n allocations, catching a smash
near the write rather than whenever the victim is freed. The live list is intrusive, so
there is no fixed-size table to overflow — the third of the three ways the page-guard
allocator broke.

It reports **interior-pointer frees** by name (walking the live list on the error path to
find the owning block, its size and its allocation site), which is the shape that produces
`munmap_chunk(): invalid pointer`. Unknown pointers are reported and **deliberately not
forwarded to glibc** — an instrument must never trigger the fault it exists to diagnose;
leaking a few host blocks for a debug run is the cheap side of that trade.

**It was self-tested on the host before ever running on device, and that caught two real
bugs in itself:**

1. `CAN_HDR_SIZE` hardcoded to 32, which is only correct on the 32-bit target — the footer
   landed outside the block on a 64-bit host. Now `sizeof(can_hdr)`.
2. The double-free marker was written into freed memory, where glibc's tcache stores its
   next/key words — so it was never readable. Double frees are now identified by the
   live-list classifier instead of by reading freed memory.

`ga_init` runs the self-test at startup and **refuses to enable canary mode if it fails**,
rather than producing confident wrong reports.

### 4. `conf/debug.env` in the launcher

One `KEY=VALUE` per line, exported before the game runs. Lets instrumentation be switched
on for a normal EmulationStation launch without rebuilding, so a bug that only appears in
real play can be captured by the player. Absent on a normal install.

### Still open: the heap abort

2/2 unattended auto-tap runs aborted, both in the PNG path:

```
[fopen] MISS: Textures_Shared/touch_fe/menu_rockstar.png (rb)
[png] png_warning: iCCP: known incorrect sRGB profile
munmap_chunk(): invalid pointer
```

The process does not exit — it hangs (last game log line 1351 of 5340, the rest is crash
dump), so on the panel it looks like a freeze on the last frame. The one canary run so far
was clean (127k live blocks, 0 smashed), which argues the corruption is a **bad free, not
an overflow** — consistent with `munmap_chunk`. libpng is the standing suspect given where
it lands, and this port already has history there ([[project-setjmp-abi]]).

**The user reports normal play is fine**, so the auto-tap may be reaching a menu a player
does not. Next step is a player-driven run with `conf/debug.env` carrying `GTALCS_CANARY=1`.

## 2026-09-09 (cont.) — first-run installer, and the OBB unpacked up front

The port now unpacks the OBB at install time instead of letting the engine read
it as an archive. The engine is perfectly able to mount the WAD — that is how
this port ran for days — but every asset read then costs a decrypt pass, and a
handheld has no CPU to spare for it. Unpacking trades a few minutes once for the
frame budget back, and it is what the porter's own device had been running from
all along.

### The archive format, now implemented in C (`src/wad_extract.c`)

Cross-checked against `scripts/obb_extract.py`, which produced the tree this port
has been running from:

```
@0      u32 'DAWL', u32 version, i32 field ; pad to 2048: pos += 2048 - (pos & 0x7FF)
@0x800  u32 numDirs, dirs[]{ i32 parent, u32 name_off }            (8 B)
        u32 numFat,  fat[] { u32 hash_key, u32 name_off, u32 dir_idx,
                             u32 data_crc, u64 offset, u64 size,
                             u64 size_again, u64 zero }            (48 B)
        u32 names_size, then the name pool if names_size > 0
```

Everything — header, tables, payload — is XOR obfuscated **by absolute file
offset**: even offsets ^ 0xAF, odd ^ 0x66. Chunked reads must track the true
offset, not the buffer index.

**Corrections to the Python script's own docstring**, found while porting it:
its comment labels the FAT's u64s `offset, end, size, 0`, but the code reads
`x[5]` as the size and `end - offset` is nonsense. The real layout is
`offset, size, size, 0`. Believing the comment produced short reads and CRC
mismatches until the field order was checked against the code.

**`data_crc` is NOT a payload checksum.** crc32, ~crc32 and adler32 of both the
encrypted and decrypted bytes were all tested against it; none match. Do not use
it to verify a file. Integrity here rests on the declared size reading in full.

### Names: no pool, so the port ships the dictionary

Both shipped OBBs have `names_size == 0` — there is no name pool — which is why
the Python extractor falls back to a tiling scan and then a CRC dictionary
attack. Rather than re-implement heuristics on-device, the port ships the
resolved path list (`obb_dictionary.txt`, 429 KB) and matches
`hash_key == ~crc32(lowercase(path))` in C. Measured coverage: **10133/10133 in
main.17 and 13/13 in patch.15** — every entry, no fallbacks needed.

### Verified against the Python extractor before it ever ran on hardware

A host harness dumps `crc32(decrypted payload), size, name` for every entry from
the C implementation and from the Python one, and diffs them:

```
C:      entries=10133 named=10133   ->  10133 lines
Python: entries=10133 named=10133   ->  10133 lines
diff: 0 lines
```

Byte-identical for all 10133 files. The file-writing path (paths, mkdir -p,
overwrite) was then exercised for real against the 13-file patch archive: all 13
match expected CRC and size on disk, and a second run over the same directory
overwrites cleanly.

### The patch archive

`patch.15` holds 13 entries: 11 that also exist in `main.17` and 2 new ETC
radar textures. **All 11 shared files are byte-identical to main.17's copies**,
so with this particular pair the patch is nearly a no-op. It is still applied
second (it must overwrite, and other version pairs will differ).

### `installer.armhf`

Modelled directly on the CTW port's installer — SDL2 + zlib only, baked bitmap
font, background decoded from the player's own APK by `png_min.c`, text-mode
fallback, exit codes 0/1/2. Differences worth knowing:

* **Artwork**: `res/drawable-mdpi-v4/android_download_screen_bg.png`, 1024x768,
  i.e. exactly the 4:3 of the 640x480 canvas — so it is drawn full-bleed with a
  translucent strip over the bottom quarter for the UI, rather than CTW's
  letterbox bands.
* **Two independent halves**: engine+assets (from the APK) and gamedata (from
  the OBB) are checked by separate sentinels, so a player who deleted gamedata/
  is not asked for the APK again.
* **Free space** comes from `wad_payload_bytes()`, not the archive's file size —
  the two are not the same number.
* The archives are deleted only **after** the tree is verified.
* 139 KB, glibc floor **GLIBC_2.7**.

Launcher: section 5 now runs the installer and nothing else. It must stay after
the vtconsole unbind and after section 4's `SDL_*` exports — SDL2's KMSDRM
backend is GBM/EGL based and cannot create a window without them, a lesson the
CTW port paid for on-device.

## 2026-09-09 (cont.) — graphics parity with the CTW port

The CTW port's quality list was: 4x MSAA, trilinear mipmaps, anisotropic filtering, and
nearest→linear filter upgrades. Three of the four were already here (`opengl_patch.c`:
`apply_tex_quality`, `init_aniso`, `glTexParameteriHook`). **MSAA was the missing one** and
is now added in `main.c`.

**Why window MSAA covers the whole game, despite the pbuffer.** LCS imports
`eglCreatePbufferSurface` but never `eglCreateWindowSurface`: the native side's only EGL
surface of its own is a pbuffer for streaming resources on a background thread, and the
on-screen rendering goes to OUR window's default framebuffer — we are the GLSurfaceView.
So a multisampled window config antialiases everything the player sees. (This is the same
fact that made the missing `SDL_GL_SwapWindow` the entire black screen —
[[project-android-renderer-swap]].)

Implementation notes:
* `GTALCS_MSAA=<0|2|4|8>`, default 4; the launcher exposes it as `conf/msaa.txt` so a
  device whose driver dislikes a multisampled config can be fixed without a rebuild.
* An unsupported sample count makes `SDL_CreateWindow` fail outright, so creation is
  retried at 0 rather than being allowed to kill the run.
* The granted `SDL_GL_MULTISAMPLEBUFFERS`/`SAMPLES` are read back and logged — a driver
  can hand back fewer samples, and "MSAA is on" should be a measurement, not an assumption.

**Untested on hardware** (device was off): if the Mali driver refuses a multisampled
config for the game's *pbuffer* share, the fallback above does not cover it — the symptom
would be `[egl] eglCreatePbufferSurface FAILED` in the log, and `conf/msaa.txt` = `0` is
the fix.

### Control mapping is OURS, not PortMaster's

Worth recording since it comes up: `engine_button()` in `jni_patch.c` maps SDL's
`SDL_CONTROLLER_BUTTON_*` to the engine's 0..15 indices, decoded from
`GTAGLview.getJoypadButtonFromKeyCode` in the APK's **classes.dex** — see
[[project-lcs-input-mapping]]. PortMaster contributes only the controller *database*
(`get_controls` / `SDL_GAMECONTROLLERCONFIG`), which is what lets SDL call the right
physical button "A". gptokeyb is deliberately not used and its branch was removed from the
launcher: the engine reads the pad natively, and gptokeyb may `EVIOCGRAB` it away.

### Porter's control layout (2026-09-09)

Two changes, both in `jni_patch.c`, both reversible without a rebuild:

* **A/B positional by default.** `swap_ab()` used to default to exchanged (matching the
  Nintendo-style silkscreen against Android's BUTTON_A = index 0); it now defaults to
  SDL's positional naming. `GTALCS_SWAP_AB=1` / `conf/swap_ab.txt` restores the exchange.
  This affects only what the ENGINE receives — the menu pointer's tap is bound to the
  physical SDL "A" separately, so menus are unaffected either way.
* **L1/L2 and R1/R2 exchanged**, on by default. This one is NOT a table edit, because the
  two sit on opposite sides of the engine's input API: L1/R1 are buttons 6/7 while L2/R2
  are axes 4/5 (LTRIGGER/BRAKE, RTRIGGER/GAS). So it is done at the source — the button
  bitmask takes its L1/R1 bits from the trigger axes, and axes 4/5 take their value from
  the shoulder buttons. The engine's own dex-derived table is left alone, which keeps a
  player preference from being encoded as if it were an engine fact.
  `GTALCS_SWAP_SHOULDERS=0` / `conf/swap_shoulders.txt` turns it off.

`poll_input` now logs once per run: `swap_ab`, `swap_shoulders`, and the SDL bind types of
both trigger axes — because on a pad that does not report L2/R2 as axes the swap leaves the
engine's L1/R1 dead, and that should be visible in the log rather than guessed at.

### The A/B "swap" was a button collision, not a mapping error

A `[btn]` trace (SDL name -> engine index, first 40 edges) settled in one line what
reasoning could not: **this pad reports the physical A button as SDL `b`**. So:

```
[btn] SDL b (1) DOWN -> engine 0     <- physical A, accelerate
[btn] SDL a (0) DOWN -> engine 1     <- physical B, attack
```

The virtual menu pointer's tap was hard-wired to `SDL_CONTROLLER_BUTTON_A` — i.e. the
physical **B** button. Mapping A/B positionally therefore put accelerate and tap on the
same physical button, and holding it to drive held a touch DOWN on the frontend for the
whole time. Reported as "I lose accelerate"; it was never a mapping problem.

`tap_button()` now defaults to `SDL_CONTROLLER_BUTTON_B` (the button labelled A on these
handhelds) and is overridable via `GTALCS_TAP_BUTTON` / `conf/tap_button.txt`, which takes
an SDL button *name*. `rightstick` or `back` are collision-free choices if a momentary tap
on the attack button ever disturbs gameplay.

**Rule for next time:** when a remap appears to lose a function, check whether that
physical button is also bound elsewhere in our own input layer before touching the
engine's mapping table.

## State at the end of 2026-09-09

**The port is playable and released.** Installer unpacks the OBB, the game boots, streams,
drives and quits cleanly. `scripts/make-release.sh` builds `release/` + `gtalcs.zip`.

Fixed this session, in order:
1. **The EmulationStation launch failure** — not a crash: the launcher's data gate demanded
   an OBB that no longer existed. Read the launcher's own log first.
2. **The post-cutscene `GetFirstElement(NULL)` crash** — our `_ctype_` table handed to the
   engine off by one, so only the letter `z` failed to case-fold, so `toyz` got two
   texlist slots. Two sessions of streaming forensics for a one-line pointer bug.
3. **`munmap_chunk()` on quit** — `exit(0)` ran the .so's `__cxa_atexit` destructors;
   now `_exit(0)`.
4. **`_S`/`_C` swapped** in the same ctype table, settled by tabulating every ctype flag
   test in the binary rather than by reasoning.
5. **Controls** — A/B positional, L1/R1 ↔ L2/R2, and the menu tap decoupled from the
   accelerator (a button collision, not a mapping error).
6. **4x MSAA**, matching the CTW port's quality set.
7. **The installer**, and with it a C implementation of the DAWL WAD format.

Also: zero compiler warnings, `tests/` holds the host harnesses that verified the
extractor, the allocator and the ctype table.

### Where to start next time

* **The intermittent heap abort** is the one real bug left. `malloc_consolidate():
  unaligned fastbin chunk` / `free(): invalid size` / `munmap_chunk(): invalid pointer` —
  three glibc messages, almost certainly one corruptor. It freezes on the last frame
  (the process does not exit). It did NOT reproduce in the porter's normal play, only in
  unattended auto-tap runs, and it lands right after the PNG loading path. The canary
  allocator is built for exactly this: `GTALCS_CANARY=1 GTALCS_CANARY_SWEEP=20000`, via
  `conf/debug.env` so it can be captured during real play. It reports interior-pointer
  frees by name, which is the shape `munmap_chunk` implies.
* **Screenshot** — `port.json` carries `image: {}` rather than a dangling reference.
  `frame*.ppm` captures are still on the device; convert one to 640x480 JPEG and restore
  the key before submitting to PortMaster.
* **Untested:** audio by ear, save/load, and the installer's SDL path on any device other
  than the porter's (it demonstrably ran there — `conf/language.txt` exists).
