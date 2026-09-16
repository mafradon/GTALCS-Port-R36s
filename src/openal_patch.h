#ifndef OPENAL_PATCH_H
#define OPENAL_PATCH_H

#include "so_util.h"

/* Probes the host OpenAL device; call once before the game initialises audio. */
void patch_openal(void);

extern const so_default_dynlib openal_dynlib[];
extern const int               openal_dynlib_count;

#endif
