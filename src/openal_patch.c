/* openal_patch.c -- OpenAL resolution for the GTA:LCS loader
 *
 * DIFFERENT FROM THE CHINATOWN WARS PORT — do not copy that design here.
 *
 * CTW had OpenAL Soft statically compiled into libCTW.so with only the Android
 * AudioTrack backend, so its openal_patch.c had to reach *inside* the game
 * binary and overwrite each al/alc symbol with hook_addr().
 *
 * LCS instead links a real libopenal.so and *imports* 23 al/alc symbols, so
 * they arrive as ordinary undefined symbols.  Resolving them is just a table —
 * no patching of loaded code at all.  We point them at the host's OpenAL Soft,
 * which has proper ALSA/PipeWire backends.
 *
 * THE ONE HAZARD: libGTALcs.so is soft-float, the host OpenAL is hard-float.
 * Any entry point taking a float BY VALUE needs a SOFTFP bridge or the callee
 * reads a stale VFP register instead of the argument.  That is four functions
 * here — alListenerf, alListener3f, alSourcef, alSource3f.  Note alListenerfv
 * deliberately has NO bridge: it takes a const float*, which travels in a GP
 * register already.  See docs/REVERSE-ENGINEERING-NOTES.md.
 */

#include <stdio.h>
#include <AL/al.h>
#include <AL/alc.h>

#include "so_util.h"
#include "openal_patch.h"

#define SOFTFP __attribute__((pcs("aapcs")))

/* ── Soft-float bridges (the only four that need one) ────────────────────── */

static SOFTFP void alListenerf_abi(ALenum param, ALfloat value) {
    alListenerf(param, value);
}
static SOFTFP void alListener3f_abi(ALenum param, ALfloat x, ALfloat y, ALfloat z) {
    alListener3f(param, x, y, z);
}
static SOFTFP void alSourcef_abi(ALuint src, ALenum param, ALfloat value) {
    alSourcef(src, param, value);
}
static SOFTFP void alSource3f_abi(ALuint src, ALenum param,
                                  ALfloat x, ALfloat y, ALfloat z) {
    alSource3f(src, param, x, y, z);
}

/* ── Table consumed by main.c's symbol resolver ──────────────────────────── */

const so_default_dynlib openal_dynlib[] = {
    /* Context / device management */
    { "alcOpenDevice",           (uintptr_t)&alcOpenDevice           },
    { "alcCreateContext",        (uintptr_t)&alcCreateContext        },
    { "alcMakeContextCurrent",   (uintptr_t)&alcMakeContextCurrent   },
    { "alcProcessContext",       (uintptr_t)&alcProcessContext       },
    { "alcSuspendContext",       (uintptr_t)&alcSuspendContext       },

    /* Buffers */
    { "alGenBuffers",            (uintptr_t)&alGenBuffers            },
    { "alDeleteBuffers",         (uintptr_t)&alDeleteBuffers         },
    { "alBufferData",            (uintptr_t)&alBufferData            },

    /* Sources */
    { "alGenSources",            (uintptr_t)&alGenSources            },
    { "alGetSourcei",            (uintptr_t)&alGetSourcei            },
    { "alSourcei",               (uintptr_t)&alSourcei               },
    { "alSourcePlay",            (uintptr_t)&alSourcePlay            },
    { "alSourcePause",           (uintptr_t)&alSourcePause           },
    { "alSourceStop",            (uintptr_t)&alSourceStop            },

    /* Streaming — the radio and ambient beds ride on these */
    { "alSourceQueueBuffers",    (uintptr_t)&alSourceQueueBuffers    },
    { "alSourceUnqueueBuffers",  (uintptr_t)&alSourceUnqueueBuffers  },

    /* Listener */
    { "alDistanceModel",         (uintptr_t)&alDistanceModel         },
    { "alListenerfv",            (uintptr_t)&alListenerfv            },  /* ptr arg: no bridge */
    { "alGetError",              (uintptr_t)&alGetError              },

    /* Soft-float bridged */
    { "alListenerf",             (uintptr_t)&alListenerf_abi         },
    { "alListener3f",            (uintptr_t)&alListener3f_abi        },
    { "alSourcef",               (uintptr_t)&alSourcef_abi           },
    { "alSource3f",              (uintptr_t)&alSource3f_abi          },
};

const int openal_dynlib_count = sizeof(openal_dynlib) / sizeof(openal_dynlib[0]);

/* Open the default device early so a failure is reported here, next to the
 * audio code, rather than as silence with no explanation later.  The game does
 * its own alcOpenDevice; this only proves the host stack works. */
void patch_openal(void) {
    ALCdevice *probe = alcOpenDevice(NULL);
    if (!probe) {
        fprintf(stderr, "[openal] WARNING: alcOpenDevice(NULL) failed — "
                        "expect silence. Check ALSOFT_CONF / ALSA config.\n");
        return;
    }
    fprintf(stderr, "[openal] default device OK: %s\n",
            alcGetString(probe, ALC_DEVICE_SPECIFIER));
    alcCloseDevice(probe);
}
