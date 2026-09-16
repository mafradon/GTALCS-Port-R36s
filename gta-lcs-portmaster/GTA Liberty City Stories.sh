#!/bin/bash
# GTA: Liberty City Stories — PortMaster launcher
# 32-bit (armhf) native port. Game data is supplied by the user (APK + OBBs).
#
# DATA LAYOUT: the OBB is UNPACKED at first run and then deleted.
#
# The engine can mount the .obb as an archive and read from it directly (it is a
# Rockstar 'DAWL' WAD), and this port did exactly that for a while.  But every
# asset read then costs an XOR decrypt pass, which a handheld cannot spare, so
# installer.armhf unpacks it once into gtalcs/gamedata/ and removes the archive
# — gamedata/ is about the same size as the OBB and few cards hold both.
#
# An OBB left in place still works: with no gamedata/ present the engine mounts
# the WAD as before.  Both layouts are accepted; section 5 records which is in
# use.  The APK is consumed for libGTALcs.so and the assets/ folder.

# ──────────────────────────────────────────────────────────────────────────
# 1. Locate and source the PortMaster control library.
# ──────────────────────────────────────────────────────────────────────────
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"

# 32-bit port: ask PortMaster for its armhf runtime on aarch64-only firmware.
export PORT_32BIT="Y"

[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

# ESUDO: never blindly fall back to "sudo" — some images run the frontend as
# root and ship no sudo at all, which would turn every privileged line below
# into "command not found", including the frontend restart (a dead screen).
if [ "$(id -u)" -eq 0 ]; then
    ESUDO=""
elif [ -z "${ESUDO+x}" ]; then
    if command -v sudo >/dev/null 2>&1; then ESUDO="sudo"; else ESUDO=""; fi
elif [ -n "$ESUDO" ] && ! command -v "${ESUDO%% *}" >/dev/null 2>&1; then
    # Test the FIRST WORD only: control.txt may set
    # ESUDO="sudo --preserve-env=..." and testing the whole string looks up one
    # executable with an impossible name and wrongly blanks a good ESUDO.
    ESUDO=""
fi
DEVICE_ARCH="${DEVICE_ARCH:-armhf}"
CFW_NAME="${CFW_NAME:-unknown}"
type get_controls       >/dev/null 2>&1 || get_controls() { :; }
type pm_platform_helper >/dev/null 2>&1 || pm_platform_helper() { :; }
type pm_finish          >/dev/null 2>&1 || pm_finish() { :; }

get_controls

# ──────────────────────────────────────────────────────────────────────────
# 2. Paths — derived from the script's own location, so this works wherever a
#    given CFW mounts the ports folder.
# ──────────────────────────────────────────────────────────────────────────
PORTDIR="$(dirname "$(realpath "$0")")"
GAMEDIR="$PORTDIR/gtalcs"
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR"
cd "$GAMEDIR" || exit 1

CURR_TTY="/dev/tty1"
[ -e "$CURR_TTY" ] || CURR_TTY="/dev/tty0"
CONSOLE_OUT="$CURR_TTY"

LOG="$GAMEDIR/gtalcs.log"
> "$LOG"

# ──────────────────────────────────────────────────────────────────────────
# 2b. Display back-end. Decides the SDL video driver AND whether we are
#     entitled to seize the console and the DRM node at all.
#
#     Default is kmsdrm: the device boots to a bare KMS console and this port
#     is the only DRM client. Under a Wayland compositor the framebuffer is
#     not ours, kmsdrm cannot open the device, and the console dance is wrong.
#
#     Precedence: conf/videodriver.txt > inherited $SDL_VIDEODRIVER >
#                 Wayland autodetect > kmsdrm
# ──────────────────────────────────────────────────────────────────────────
VIDEO_FILE="$CONFDIR/videodriver.txt"
if [ -f "$VIDEO_FILE" ]; then
    SDL_VIDEODRIVER="$(tr -d ' \t\r\n' < "$VIDEO_FILE" | tr '[:upper:]' '[:lower:]')"
    [ -n "$SDL_VIDEODRIVER" ] && \
        echo "launcher: video driver = $SDL_VIDEODRIVER (conf/videodriver.txt)" >> "$LOG"
fi

if [ -z "$SDL_VIDEODRIVER" ]; then
    # $DISPLAY is deliberately NOT consulted: Sway images export DISPLAY=:0.0
    # too, so it discriminates nothing.
    _wl_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    _wl_sock="$(ls "$_wl_dir"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)"
    if [ -z "$_wl_sock" ]; then
        _wl_sock="$(ls /run/user/*/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)"
    fi
    if [ -n "$WAYLAND_DISPLAY" ] || [ -n "$_wl_sock" ]; then
        SDL_VIDEODRIVER="wayland"
    else
        SDL_VIDEODRIVER="kmsdrm"
    fi
    echo "launcher: video driver = $SDL_VIDEODRIVER (autodetected)" >> "$LOG"
fi

if [ "$SDL_VIDEODRIVER" = "kmsdrm" ]; then OWNS_DISPLAY=1; else OWNS_DISPLAY=0; fi

# ──────────────────────────────────────────────────────────────────────────
# 3. Take the screen + input devices, then arm the cleanup trap.
#    The trap goes up HERE, before anything that can exit early, so no failure
#    path can leave the handheld with a blank screen.
# ──────────────────────────────────────────────────────────────────────────
$ESUDO chmod 666 /dev/uinput 2>/dev/null
if [ "$OWNS_DISPLAY" = "1" ]; then
    $ESUDO chmod 666 "$CURR_TTY"         2>/dev/null
    $ESUDO chmod 666 /dev/dri/card0      2>/dev/null
    $ESUDO chmod 666 /dev/dri/renderD128 2>/dev/null
fi

_cleanup() {
    [ -n "$_CLEANED" ] && return
    _CLEANED=1

    # Stop the game FIRST and WAIT for it to actually die. Restoring the
    # console while the game still holds DRM does not stick — the rebind is
    # accepted but the framebuffer console does not come back, which is the
    # state that needs a physical reset.
    if [ -n "$GAME_PID" ] && kill -0 "$GAME_PID" 2>/dev/null; then
        kill -TERM "$GAME_PID" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$GAME_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -0 "$GAME_PID" 2>/dev/null && $ESUDO kill -9 "$GAME_PID" 2>/dev/null
    fi

    $ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null

    if [ "$OWNS_DISPLAY" = "1" ]; then
        echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
        echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1
        printf "\033c"   > "$CURR_TTY"
        printf "\e[?25h" > "$CURR_TTY"

        # pm_finish is NOT defined by control.txt on ArkOS/dArkOS (our stub is
        # a no-op), so restart the frontend explicitly or the user is left
        # looking at a black screen.
        $ESUDO systemctl start emulationstation 2>/dev/null || \
          $ESUDO systemctl restart oga_events   2>/dev/null || true
    fi

    pm_finish
}
trap _cleanup EXIT
trap '_cleanup; exit 130' INT TERM HUP

# We deliberately do NOT stop the frontend: launched from EmulationStation this
# script lives in the frontend's own systemd cgroup, so stopping it kills us
# too. ES backgrounds itself when launching a port, and PortMaster stops the
# frontend on its own path. Releasing the vtconsole is what frees the
# framebuffer for KMS; the frontend is restarted explicitly in _cleanup.
sleep 1

if [ "$OWNS_DISPLAY" = "1" ]; then
    printf "\033c"   > "$CURR_TTY"
    printf "\e[?25l" > "$CURR_TTY"
fi

# ──────────────────────────────────────────────────────────────────────────
# 4. Runtime environment — needed by the first-run setup as well as the game.
# ──────────────────────────────────────────────────────────────────────────
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL_VIDEODRIVER

# Unversioned driver names are the proven values on the kmsdrm devices. They
# are dev-package symlinks, so a runtime-only image may ship only the versioned
# sonames — forcing the bare names there makes SDL fail to create a window.
# Only set them on the path where they are known good.
if [ "$OWNS_DISPLAY" = "1" ]; then
    export SDL_VIDEO_GL_DRIVER=libGLESv2.so
    export SDL_VIDEO_EGL_DRIVER=libEGL.so
fi

# Deliberately "armhf", NOT "$DEVICE_ARCH": DEVICE_ARCH is the arch of the
# DEVICE (aarch64 on most modern CFWs), while this port and its bundled libs
# are always 32-bit.
export LD_LIBRARY_PATH="$GAMEDIR/libs.armhf:/usr/lib/arm-linux-gnueabihf:/usr/lib32:$LD_LIBRARY_PATH"

# ──────────────────────────────────────────────────────────────────────────
# 5. First-run setup, done by installer.armhf.
#
# The installer unpacks libGTALcs.so and assets/ out of the player's own APK
# and UNPACKS THE OBB into gamedata/, then deletes the archives.
#
# Why unpack at all: the engine can read the .obb in place — it is a Rockstar
# 'DAWL' WAD and every byte of it is XOR obfuscated — but then it pays to
# decrypt on every asset read for the life of the install, and a handheld has
# no CPU to spare for that. Unpacking once trades a few minutes at setup for
# the frame budget back.
#
# ORDERING MATTERS. The installer is a KMS client exactly like the game, so it
# must run AFTER the vtconsole is unbound (just above) and AFTER the SDL_*
# exports in section 4: SDL2's KMSDRM backend is GBM/EGL based with no plain
# framebuffer path, so without those it cannot create a window and drops to
# text. The _cleanup trap is already armed, so an early exit here still
# restores the screen.
# ──────────────────────────────────────────────────────────────────────────
if [ "$OWNS_DISPLAY" = "1" ]; then
    echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
    echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1
    # 666 so the installer can RE-BIND the console if it falls back to text
    # mode — otherwise its message prints to a console nobody can see.
    $ESUDO chmod 666 /sys/class/vtconsole/vtcon0/bind 2>/dev/null
    $ESUDO chmod 666 /sys/class/vtconsole/vtcon1/bind 2>/dev/null
fi

say() { echo "$*" | tee -a "$LOG" > "$CONSOLE_OUT"; }
die() { say ""; say "$*"; say ""; say "Returning to the frontend in 20 seconds..."; sleep 20; exit 1; }

if [ -x "$GAMEDIR/installer.armhf" ]; then
    GTALCS_DIR="$GAMEDIR" "$GAMEDIR/installer.armhf" "$GAMEDIR" 2>&1 \
        | tee -a "$LOG" > "$CONSOLE_OUT"
    INST_RC=${PIPESTATUS[0]}
    echo "launcher: installer exit=$INST_RC" >> "$LOG"
    case "$INST_RC" in
        0) ;;
        1) die "SETUP INCOMPLETE

Your game files are missing. Setup needs ALL THREE of these in
  $GAMEDIR

  your GTA: Liberty City Stories .apk
  main.17.com.rockstargames.gtalcs.obb
  patch.15.com.rockstargames.gtalcs.obb

The patch archive is REQUIRED, not optional. See README.md." ;;
        *) die "SETUP FAILED while unpacking the game data.
See gtalcs.log in $GAMEDIR for the reason." ;;
    esac
else
    echo "launcher: no installer.armhf — skipping first-run setup" >> "$LOG"
fi

# After a successful install the layout is gamedata/. An OBB left in place still
# works — the engine mounts it as a WAD — so accept either and record which one
# is in use. Requiring the OBB specifically is what once made a perfectly good
# unpacked install refuse to start.
MAIN_OBB="$(ls "$GAMEDIR"/main.*.com.rockstargames.gtalcs.obb 2>/dev/null | head -n1)"
HAVE_GAMEDATA=0
if [ -d "$GAMEDIR/gamedata" ] && [ -n "$(ls -A "$GAMEDIR/gamedata" 2>/dev/null)" ]; then
    HAVE_GAMEDATA=1
fi

if [ -z "$MAIN_OBB" ] && [ "$HAVE_GAMEDATA" = "0" ]; then
    die "ERROR: no game data found in
$GAMEDIR

GTA: LCS needs EITHER its OBB (about 1.9 GB, which setup unpacks and then
removes) OR an already-unpacked gamedata/ folder. Neither is present."
fi

if [ "$HAVE_GAMEDATA" = "1" ]; then
    echo "launcher: data = unpacked gamedata/" >> "$LOG"
else
    echo "launcher: data = OBB archive $(basename "$MAIN_OBB")" >> "$LOG"
fi

# ──────────────────────────────────────────────────────────────────────────
# 5b. Language. The engine takes a language/country pair; we derive both from
#     conf/language.txt, else the device $LANG, else English.
# ──────────────────────────────────────────────────────────────────────────
LANG_FILE="$CONFDIR/language.txt"
if [ -f "$LANG_FILE" ]; then
    GAME_LANG="$(tr -d ' \t\r\n' < "$LANG_FILE" | tr '[:upper:]' '[:lower:]' | cut -c1-2)"
fi
case "$GAME_LANG" in
    en|fr|de|it|es|ja) ;;
    "") GAME_LANG="" ;;
    *)  echo "launcher: unsupported language '$GAME_LANG' — using English" >> "$LOG"
        GAME_LANG="en" ;;
esac
case "$GAME_LANG" in
    en) export LANG="en_US.UTF-8" ;;
    fr) export LANG="fr_FR.UTF-8" ;;
    de) export LANG="de_DE.UTF-8" ;;
    it) export LANG="it_IT.UTF-8" ;;
    es) export LANG="es_ES.UTF-8" ;;
    ja) export LANG="ja_JP.UTF-8" ;;
esac
[ -n "$GAME_LANG" ] && echo "launcher: language = $GAME_LANG (LANG=$LANG)" >> "$LOG"

# ──────────────────────────────────────────────────────────────────────────
# 5c. conf/debug.env — optional bring-up switches, one KEY=VALUE per line.
#     Lets instrumentation (GTALCS_CANARY=1, GTALCS_TXD_DIAG=1 …) be turned on
#     for a normal frontend launch without rebuilding or editing this script,
#     so a bug that only shows up in real play can be captured by the player.
#     Absent on a normal install, which is the point.
# ──────────────────────────────────────────────────────────────────────────
DEBUG_ENV="$CONFDIR/debug.env"
if [ -f "$DEBUG_ENV" ]; then
    while IFS= read -r _line || [ -n "$_line" ]; do
        case "$_line" in ''|\#*) continue ;; *=*) ;; *) continue ;; esac
        export "$_line"
        echo "launcher: debug.env $_line" >> "$LOG"
    done < "$DEBUG_ENV"
fi

# ──────────────────────────────────────────────────────────────────────────
# 6. Game run. Log only from here; the game owns the framebuffer.
# ──────────────────────────────────────────────────────────────────────────
exec > >(tee -a "$LOG") 2>&1

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export AUDIODEV="${AUDIODEV:-default}"
export ALSOFT_DRIVERS="${ALSOFT_DRIVERS:-alsa}"

# Where the data lives, instead of the compiled-in default. Fixes launches from
# SD2 and CFWs whose ports directory differs.
export GTALCS_DIR="$GAMEDIR"

# conf/msaa.txt — antialiasing samples: 0, 2, 4 or 8 (default 4). The game
# renders into the port's own window, so this applies to everything. Exposed as
# a file because a device whose driver dislikes a multisampled config should be
# fixable without a rebuild; the binary also falls back to 0 on its own if the
# window cannot be created.
# conf/<name>.txt -> environment variable, digits only. Used for the handful of
# switches worth changing on a device without a rebuild.
conf_flag() {
    _f="$CONFDIR/$1"; _var="$2"
    [ -f "$_f" ] || return 0
    _v="$(tr -dc '0-9' < "$_f")"
    [ -n "$_v" ] || return 0
    export "$_var=$_v"
    echo "launcher: $_var = $_v (conf/$1)" >> "$LOG"
}

conf_flag msaa.txt           GTALCS_MSAA            # 0, 2, 4 or 8 (default 4)
conf_flag swap_ab.txt        GTALCS_SWAP_AB         # 1 = exchange A and B
conf_flag swap_shoulders.txt GTALCS_SWAP_SHOULDERS  # 0 = keep L1/L2 as labelled

# Which button taps the menu pointer. Takes an SDL button NAME, not a number, so
# it is read separately from the digits-only flags above. Must not be a button
# held during gameplay: default "b" is the one silkscreened A on these handhelds.
TAP_FILE="$CONFDIR/tap_button.txt"
if [ -f "$TAP_FILE" ]; then
    GTALCS_TAP_BUTTON="$(tr -d ' \t\r\n' < "$TAP_FILE")"
    if [ -n "$GTALCS_TAP_BUTTON" ]; then
        export GTALCS_TAP_BUTTON
        echo "launcher: GTALCS_TAP_BUTTON = $GTALCS_TAP_BUTTON (conf/tap_button.txt)" >> "$LOG"
    fi
fi

# Works around a broken 32-bit vDSO clock_gettime on some Rockchip kernels.
# Applied ONLY to the game process — not exported — so PortMaster's 64-bit
# helpers do not reject a 32-bit preload with an ELFCLASS32 warning.
GAME_PRELOAD="$GAMEDIR/libclock_fix.so"

# NO gptokeyb. LCS reads the gamepad natively through the engine's own JNI
# entry points (setJoyAxis / onJoyButtonDown / onJoyButtonUp), and gptokeyb may
# EVIOCGRAB the pad, which takes those events away from the game. There is
# deliberately no .gptk file to ship.

pm_platform_helper "$GAMEDIR/gtalcs.armhf"

# Background + `wait` so $GAME_PID is known to the trap; `wait` is
# interruptible by signals where a foreground child would not be.
LD_PRELOAD="$GAME_PRELOAD" ./gtalcs.armhf &
GAME_PID=$!
wait "$GAME_PID"
