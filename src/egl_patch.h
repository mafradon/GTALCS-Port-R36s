#ifndef EGL_PATCH_H
#define EGL_PATCH_H

#include <EGL/egl.h>
#include "so_util.h"

/* Call once, right after SDL_GL_CreateContext + SDL_GL_MakeCurrent, while the
 * context is current on the calling thread. */
void egl_patch_capture(void);

EGLDisplay egl_patch_display(void);
EGLContext egl_patch_main_context(void);

/* Log the context/surfaces bound on the CALLING thread (bring-up). */
void egl_patch_report_current(const char *tag);

extern const so_default_dynlib egl_dynlib[];
extern const int               egl_dynlib_count;

#endif /* EGL_PATCH_H */
