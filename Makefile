# Makefile -- GTA: Liberty City Stories R36S port
#
# Target: ARMv7 HF (32-bit ARM host binary) loading the Android armeabi-v7a
#         libGTALcs.so.  Cross-compiled from x86-64.
#
# NOTE: libGTALcs.so is SOFT-FLOAT ABI and our binary is HARD-FLOAT.  Every
#       hook crossing that boundary with a float/double BY VALUE needs the
#       SOFTFP attribute.  See docs/REVERSE-ENGINEERING-NOTES.md.
#
# Quick start:
#   make             # dev build      -> gtalcs_r36
#   make portmaster  # shipping build -> gtalcs.armhf (old-glibc, PortMaster)
#   make check-syms  # list every unresolved symbol in libGTALcs.so

CROSS   := arm-linux-gnueabihf-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

# Shared with the Chinatown Wars port — same device class, same libraries.
SYSROOT  := armhf-sysroot/root
BULLSEYE := bullseye-sysroot/root

GAME_SO := extracted/lib/armeabi-v7a/libGTALcs.so

CFLAGS  := -march=armv7-a -mfpu=neon -mfloat-abi=hard \
           -O1 -g -fno-omit-frame-pointer \
           -DDEBUG \
           -Wall -Wno-unused-function \
           -I src \
           -I $(SYSROOT)/usr/include \
           -I $(SYSROOT)/usr/include/arm-linux-gnueabihf

LDFLAGS := -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
           -lSDL2 -lEGL -lGLESv2 -lopenal -lz -ldl -lpthread -lm \
           -Wl,-rpath-link,$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
           -Wl,--allow-shlib-undefined

SRCS    := src/main.c \
           src/clock_fix.c \
           src/setjmp_fix.c \
           src/guard_alloc.c \
           src/so_util.c \
           src/jni_patch.c \
           src/asset_manager.c \
           src/egl_patch.c \
           src/openal_patch.c \
           src/opengl_patch.c

OBJS    := $(SRCS:.c=.o)
TARGET  := gtalcs_r36
PRELOAD := libclock_fix.so

# ── PortMaster (old-glibc) build ───────────────────────────────────────────
# Links against a Debian 11 "bullseye" glibc-2.31 sysroot so the binary runs on
# CFWs as old as glibc 2.31.  Only glibc is swapped; the graphics/audio dev libs
# stay link-only stubs from the newer sysroot (the device supplies the real ones
# at runtime, and their symbols carry no GLIBC_2.34+ version tags).
#   * --sysroot + bullseye lib dirs FIRST -> -lc/-lm/-lpthread bind to 2.31
#   * -idirafter for the noble includes   -> bullseye's pre-C23 <stdlib.h> wins
#     (no __isoc23_strtol@GLIBC_2.38); noble only fills SDL2/GLES/AL headers
GCC_INC := $(shell $(CC) -print-file-name=include)

PM_CFLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=hard \
             -O1 -g -fno-omit-frame-pointer \
             -DDEBUG \
             -Wall -Wno-unused-function \
             --sysroot=$(BULLSEYE) \
             -nostdinc \
             -I src \
             -isystem $(GCC_INC) \
             -isystem $(BULLSEYE)/usr/include/arm-linux-gnueabihf \
             -isystem $(BULLSEYE)/usr/include \
             -idirafter $(SYSROOT)/usr/include \
             -idirafter $(SYSROOT)/usr/include/arm-linux-gnueabihf

PM_LDFLAGS := --sysroot=$(BULLSEYE) \
             -L$(BULLSEYE)/usr/lib/arm-linux-gnueabihf \
             -L$(BULLSEYE)/lib/arm-linux-gnueabihf \
             -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -lSDL2 -lEGL -lGLESv2 -lopenal -lz -ldl -lpthread -lm \
             -Wl,-rpath-link,$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -Wl,--allow-shlib-undefined

PM_OBJS   := $(SRCS:.c=.pm.o)
PM_TARGET := gtalcs.armhf

# ── first-run installer ────────────────────────────────────────────────────
# Unpacks the player's APK and OBB into the game directory behind a full-screen
# SDL2 splash.  Built with the same old-glibc sysroot as the shipping binary,
# and deliberately linked against SDL2 + zlib ONLY: no GLES, no OpenAL, no
# SDL2_image/ttf.  Every extra dependency raises the port's glibc floor and the
# set of CFWs it will start on.
INST_SRCS   := src/installer.c src/wad_extract.c src/png_min.c
INST_OBJS   := $(INST_SRCS:.c=.inst.o)
INST_TARGET := installer.armhf

INST_LDFLAGS := --sysroot=$(BULLSEYE) \
             -L$(BULLSEYE)/usr/lib/arm-linux-gnueabihf \
             -L$(BULLSEYE)/lib/arm-linux-gnueabihf \
             -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -lSDL2 -lz -lm \
             -Wl,-rpath-link,$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -Wl,--allow-shlib-undefined

.PHONY: all clean portmaster installer check-syms check-abi data

all: $(TARGET) $(PRELOAD)

installer: $(INST_TARGET)
	@echo "=== $(INST_TARGET): glibc version requirements ==="
	@$(CROSS)objdump -T $(INST_TARGET) | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tr '\n' ' '; echo
	@ls -la $(INST_TARGET)

$(INST_TARGET): $(INST_OBJS)
	$(CC) $(PM_CFLAGS) -o $@ $^ $(INST_LDFLAGS)

%.inst.o: %.c
	$(CC) $(PM_CFLAGS) -c -o $@ $<

portmaster: $(PM_TARGET) $(PRELOAD)
	@echo "=== $(PM_TARGET): glibc version requirements ==="
	@$(CROSS)objdump -T $(PM_TARGET) | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tr '\n' ' '; echo

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(PM_TARGET): $(PM_OBJS)
	$(CC) $(PM_CFLAGS) -o $@ $^ $(PM_LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/%.pm.o: src/%.c
	$(CC) $(PM_CFLAGS) -c -o $@ $<

# LD_PRELOAD shim working around the clock_gettime vDSO SIGILL on some devices.
$(PRELOAD): src/clock_preload.c src/clock_preload.map
	$(CC) -O1 -fPIC -shared -nostdlib \
	  -march=armv7-a -mfpu=neon -mfloat-abi=hard \
	  -Wl,-soname,$(PRELOAD) \
	  -Wl,--version-script=src/clock_preload.map \
	  -o $@ $<

clean:
	rm -f $(OBJS) $(PM_OBJS) $(INST_OBJS) $(TARGET) $(PM_TARGET) $(INST_TARGET) $(PRELOAD)

# Every symbol libGTALcs.so needs from us.  Diff this against the resolved
# table after a build — anything missing crashes at the first call, not at load.
check-syms:
	@$(CROSS)nm -D $(GAME_SO) | awk '/ U /{ print $$2 }' | sed 's/@.*//' | sort -u

# Confirm the float-ABI mismatch is still what we think it is.
check-abi:
	@echo -n "libGTALcs.so: "; $(CROSS)readelf -h $(GAME_SO) | sed -n 's/.*Flags:.*EABI, //p'
	@test -f $(TARGET) && { echo -n "$(TARGET):   "; $(CROSS)readelf -h $(TARGET) | sed -n 's/.*Flags:.*EABI, //p'; } || true

# Stage a runnable data directory locally (mirrors /roms/ports/gtalcs).
data:
	mkdir -p data/assets data/save
	cp $(GAME_SO) data/
	cp -r extracted/assets/. data/assets/
	@echo "Now copy the two .obb files from Original-Files/ into data/"
