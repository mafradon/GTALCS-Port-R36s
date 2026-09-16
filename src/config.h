#ifndef CONFIG_H
#define CONFIG_H

/* ── Display ─────────────────────────────────────────────────────────────── */
#define SCREEN_W    640
#define SCREEN_H    480

/* ── Paths on the device ─────────────────────────────────────────────────── */
#define DATA_PATH   "/roms/ports/gtalcs"
#define SO_PATH     DATA_PATH "/libGTALcs.so"

/* Assets extracted from the APK's assets/ folder live here.  Backs our
 * AAssetManager shim — the game opens fonts/images/json/xml through it. */
#define ASSETS_PATH DATA_PATH "/assets"

/* setGameFilesDir() is handed this directory; the game builds OBB paths
 * beneath it.  setPrivateFilesDir() gets SAVE_PATH (needs a trailing "/",
 * matching what GTAActivity.a() passes on Android). */
#define GAME_FILES_PATH DATA_PATH
#define SAVE_PATH       DATA_PATH "/save/"

/* The OBBs are a custom Rockstar container, not a zip — the game's own
 * reader parses them.  We only have to place them where it looks. */
#define OBB_MAIN_RELPATH  "/main.17.com.rockstargames.gtalcs.obb"
#define OBB_PATCH_RELPATH "/patch.15.com.rockstargames.gtalcs.obb"

/* Loose game files unpacked from the OBBs (scripts/obb_extract.py).  The
 * engine tries loose files before archive entries, so any relative open that
 * misses in the game dir is retried here (read-only opens only — never
 * redirects writes). */
#define GAMEDATA_PATH DATA_PATH "/gamedata"

/* ── Values fed to the JNI bootstrap ─────────────────────────────────────── */
/* setDeviceInfo(int, const char*, MANUFACTURER, HARDWARE) and setOSVersion(int).
 * We report a mid-range Android device: SDK 19 (KitKat) is what the 2016 build
 * targets, and a plausible manufacturer/hardware pair keeps any device-specific
 * quirk tables on their default path. */
#define DEVICE_MEMORY_MB  2048   /* device has 2 GB; 512 understated it */
#define DEVICE_OS_VERSION 19
#define DEVICE_MANUFACTURER "unknown"
#define DEVICE_HARDWARE     "rk3326"

/* ── Input ───────────────────────────────────────────────────────────────── */
#define STICK_DEADZONE 0.25f

/* Virtual-pointer travel across the touch frontend, screen widths/second. */
#define CURSOR_SPEED 0.6f

/* #define DEBUG */

#endif /* CONFIG_H */
