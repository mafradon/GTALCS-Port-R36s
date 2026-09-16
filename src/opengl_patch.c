/* opengl_patch.c -- OpenGL ES 2 fixups for Mali-400 / R36S */

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "so_util.h"
#include "opengl_patch.h"

extern so_module gtalcs_mod;

static GLuint cur_prog;

/* Black-screen diagnostic: is the engine issuing draw calls into OUR context
 * at all?  The frame loop's screenshot handler reports draws-per-interval:
 * zero alongside black readback means the engine renders elsewhere (pbuffer,
 * other thread); non-zero means it draws here and the readback/present path
 * is the suspect. */
volatile unsigned long g_gl_draw_calls;

/* Camera view matrix — captured from first p=18 MV upload per frame.
 * Used by fix_sprite_translation to convert p=12 world-space translations
 * to view space. */
static GLfloat g_view_mat[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static int     g_view_mat_valid = 0;

/* ── Pass-through hooks (main.c dynlib table entries) ─────────────────── */

void glBindAttribLocationHook(GLuint prog, GLuint index, const char *name) {
    glBindAttribLocation(prog, index, name);
}

void glVertexAttribPointerHook(GLuint index, GLint size, GLenum type,
                                GLboolean norm, GLsizei stride, const void *ptr) {
    glVertexAttribPointer(index, size, type, norm, stride, ptr);
}

void glEnableHook(GLenum cap)          { glEnable(cap); }
void glDisableHook(GLenum cap)         { glDisable(cap); }
void glDepthMaskHook(GLboolean flag)   { glDepthMask(flag); }

void glBindFramebufferHook(GLenum target, GLuint fbo) {
    glBindFramebuffer(target, fbo);
}

void glFramebufferTexture2DHook(GLenum target, GLenum attachment,
                                 GLenum textarget, GLuint texture, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO INCOMPLETE: 0x%04x tex=%u\n", status, texture);
        fflush(stderr);
    }
}

void glBlendFuncHook(GLenum sfactor, GLenum dfactor) {
    glBlendFunc(sfactor, dfactor);
}

void glUniform3fvHook(GLint loc, GLsizei count, const GLfloat *v) {
    glUniform3fv(loc, count, v);
}

void glUniform4fvHook(GLint loc, GLsizei count, const GLfloat *v) {
    glUniform4fv(loc, count, v);
}

void glDrawElementsHook(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    g_gl_draw_calls++;
    glDrawElements(mode, count, type, idx);
}

void glEnableVertexAttribArrayHook(GLuint idx)  { glEnableVertexAttribArray(idx); }
void glDisableVertexAttribArrayHook(GLuint idx) { glDisableVertexAttribArray(idx); }

/* ── glDrawArraysHook: frame boundary detection for view matrix reset ─── */

void glDrawArraysHook(GLenum mode, GLint first, GLsizei count) {
    static GLuint prev_prog = 0;
    /* Heuristic: when prog transitions from any high program back to p=3,
     * a new frame has started — reset view matrix capture so p=18's first
     * upload this frame is used. */
    if (cur_prog == 3 && prev_prog > 3)
        g_view_mat_valid = 0;
    prev_prog = cur_prog;
    g_gl_draw_calls++;
    glDrawArrays(mode, first, count);
}

/* ── glLinkProgramHook: link error reporting ──────────────────────────── */

void glLinkProgramHook(GLuint prog) {
    glLinkProgram(prog);
    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char buf[512] = {0};
        glGetProgramInfoLog(prog, sizeof(buf)-1, NULL, buf);
        fprintf(stderr, "glLinkProgram FAILED prog=%u: %s\n", prog, buf);
        fflush(stderr);
    }
}

void glUseProgramHook(GLuint prog) {
    glUseProgram(prog);
    cur_prog = prog;
}

/* ── p=12 world-space matrix fix ──────────────────────────────────────── *
 * p=12 (FLAG_3D|TEXTURE, non-lit world objects) receives a model matrix   *
 * whose translation is in world short-integer space rather than view       *
 * space. Multiply the translation column by the captured view matrix to    *
 * bring it into view space before handing it to the GPU.                  */

static void fix_sprite_translation(GLfloat *mv) {
    GLfloat tx = mv[12], ty = mv[13], tz = mv[14];
    mv[12] = g_view_mat[0]*tx + g_view_mat[4]*ty + g_view_mat[8]*tz  + g_view_mat[12];
    mv[13] = g_view_mat[1]*tx + g_view_mat[5]*ty + g_view_mat[9]*tz  + g_view_mat[13];
    mv[14] = g_view_mat[2]*tx + g_view_mat[6]*ty + g_view_mat[10]*tz + g_view_mat[14];
}

void glUniformMatrix4fvHook(GLint location, GLsizei count,
                             GLboolean transpose, const GLfloat *value) {
    /* p=18 (lit world geometry): capture the first per-frame V matrix.
     * M00 < 0.1 identifies the view matrix (scale ~1/64); later per-object
     * uploads have M00≈1 and must be left untouched. */
    if (cur_prog == 18 && count >= 1 && !transpose) {
        if (value[0] < 0.1f && !g_view_mat_valid) {
            memcpy(g_view_mat, value, 16 * sizeof(GLfloat));
            g_view_mat_valid = 1;
        }
        glUniformMatrix4fv(location, count, transpose, value);
        return;
    }

    /* p=12: world-space model matrix at loc=2 (M00>0.5 → not yet view-space).
     * Apply the captured view transform to its translation column. */
    if (cur_prog == 12 && location == 2 && count >= 1 && !transpose
            && g_view_mat_valid && value[0] > 0.5f) {
        GLfloat mv[16];
        memcpy(mv, value, 16 * sizeof(GLfloat));
        fix_sprite_translation(mv);
        glUniformMatrix4fv(location, count, transpose, mv);
        return;
    }

    glUniformMatrix4fv(location, count, transpose, value);
}

/* ── GL_BGRA_EXT → GL_RGBA fix ────────────────────────────────────────── *
 * Mali-400 does not support GL_BGRA_EXT as an upload format. Swizzle the  *
 * pixel data to GL_RGBA before passing it to the driver.                   */

/* ── Texture quality: anisotropic filtering + trilinear mipmaps ─────────── *
 * After each texture upload we generate mipmaps and apply the best         *
 * filtering the driver supports.  glTexParameteriHook prevents the game    *
 * from later downgrading those filters back to nearest/linear.             */

#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF

static GLfloat g_max_aniso   = 0.0f;
static int     g_aniso_ready = 0;

/* Call once after the GL context is live. */
static void init_aniso(void) {
    if (g_aniso_ready) return;
    g_aniso_ready = 1;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_max_aniso);
}

/* Track which texture IDs have a full mipmap pyramid so we can safely set
 * GL_LINEAR_MIPMAP_LINEAR without blanking textures that have none. */
#define MAX_TEX_ID 16384
static uint8_t g_has_mipmap[MAX_TEX_ID];

static void apply_tex_quality(void) {
    init_aniso();
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    if (id > 0 && id < MAX_TEX_ID) g_has_mipmap[id] = 1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (g_max_aniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_max_aniso);
}

void glTexParameteriHook(GLenum target, GLenum pname, GLint param) {
    if (target == GL_TEXTURE_2D) {
        if (pname == GL_TEXTURE_MAG_FILTER && param == GL_NEAREST) {
            param = GL_LINEAR;
        } else if (pname == GL_TEXTURE_MIN_FILTER) {
            /* Upgrade nearest/linear to trilinear only if this texture has a
             * mipmap pyramid we generated; otherwise just ensure linear. */
            GLint id = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
            int has = (id > 0 && id < MAX_TEX_ID && g_has_mipmap[id]);
            if (param == GL_NEAREST)
                param = has ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
            else if (param == GL_LINEAR && has)
                param = GL_LINEAR_MIPMAP_LINEAR;
        }
    }
    glTexParameteri(target, pname, param);
}

/* ── GL_BGRA_EXT → GL_RGBA fix ────────────────────────────────────────── */

#define GL_BGRA_EXT 0x80E1

static void *bgra_to_rgba(const void *src, GLsizei width, GLsizei height) {
    size_t n = (size_t)width * (size_t)height * 4;
    uint8_t *buf = malloc(n);
    if (!buf) return NULL;
    const uint8_t *s = src;
    uint8_t *d = buf;
    for (size_t i = 0; i < n; i += 4) {
        d[i+0] = s[i+2];
        d[i+1] = s[i+1];
        d[i+2] = s[i+0];
        d[i+3] = s[i+3];
    }
    return buf;
}

void glTexImage2DHook(GLenum target, GLint level, GLint internalformat,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void *pixels) {
    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE && pixels) {
        void *buf = bgra_to_rgba(pixels, width, height);
        if (buf) {
            glTexImage2D(target, level, GL_RGBA, width, height, border,
                         GL_RGBA, type, buf);
            free(buf);
            if (level == 0) { glGenerateMipmap(target); apply_tex_quality(); }
            return;
        }
    }
    glTexImage2D(target, level, internalformat, width, height, border,
                 format, type, pixels);
    if (level == 0) { glGenerateMipmap(target); apply_tex_quality(); }
}

void glTexSubImage2DHook(GLenum target, GLint level,
                          GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const void *pixels) {
    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE && pixels) {
        void *buf = bgra_to_rgba(pixels, width, height);
        if (buf) {
            glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                            GL_RGBA, type, buf);
            free(buf);
            return;
        }
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                    format, type, pixels);
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum internalformat,
                                 GLsizei width, GLsizei height, GLint border,
                                 GLsizei imageSize, const void *data) {
    while (glGetError() != GL_NO_ERROR) {}
    glCompressedTexImage2D(target, level, internalformat, width, height,
                           border, imageSize, data);
    GLenum err = glGetError();
    if (err) {
        fprintf(stderr, "CompTexImage2D fmt=0x%04x -> GL error 0x%04x\n",
                internalformat, err);
        fflush(stderr);
    }
    /* Try to generate mipmaps for compressed textures — driver may or may not
     * support this; if it fails we silently skip and only apply anisotropy. */
    if (level == 0) {
        while (glGetError() != GL_NO_ERROR) {}
        glGenerateMipmap(target);
        if (glGetError() == GL_NO_ERROR)
            apply_tex_quality();   /* mipmap succeeded: full trilinear + AF */
        else {
            init_aniso();
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            if (g_max_aniso > 1.0f)
                glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_max_aniso);
        }
    }
}

/* ── Shader hooks ─────────────────────────────────────────────────────── */

void glShaderSourceHook(GLuint shader, GLsizei count,
                         const char **string, const GLint *length) {
    glShaderSource(shader, count, string, length);
}

void glCompileShaderHook(GLuint shader) {
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buf[1024] = {0};
        glGetShaderInfoLog(shader, sizeof(buf)-1, NULL, buf);
        fprintf(stderr, "glCompileShader FAILED shader=%u: %s\n", shader, buf);
        fflush(stderr);
    }
}

/* ── Softfp ABI thunks ────────────────────────────────────────────────── *
 * libGTALcs.so (Android armeabi-v7a) passes float args in integer registers  *
 * (soft-float calling convention). libMali.so (system) expects them in    *
 * VFP registers (hard-float). These thunks bridge the gap.                */
#define SOFTFP __attribute__((pcs("aapcs")))

SOFTFP void glUniform1f_abi(GLint l, GLfloat v0)                      { glUniform1f(l, v0); }
SOFTFP void glUniform2f_abi(GLint l, GLfloat v0, GLfloat v1)          { glUniform2f(l, v0, v1); }
SOFTFP void glUniform3f_abi(GLint l, GLfloat v0, GLfloat v1, GLfloat v2) { glUniform3f(l, v0, v1, v2); }
SOFTFP void glTexParameterf_abi(GLenum tgt, GLenum pname, GLfloat p)  { glTexParameterf(tgt, pname, p); }
SOFTFP void glDepthRangef_abi(GLfloat n, GLfloat f)                   { glDepthRangef(n, f); }
SOFTFP void glClearDepthf_abi(GLfloat d)                              { glClearDepthf(d); }
SOFTFP void glLineWidth_abi(GLfloat w)                                { glLineWidth(w); }
SOFTFP void glPolygonOffset_abi(GLfloat factor, GLfloat units)        { glPolygonOffset(factor, units); }
SOFTFP void glSampleCoverage_abi(GLclampf value, GLboolean invert)    { glSampleCoverage(value, invert); }
SOFTFP void glVertexAttrib1f_abi(GLuint idx, GLfloat x)               { glVertexAttrib1f(idx, x); }
SOFTFP void glVertexAttrib2f_abi(GLuint idx, GLfloat x, GLfloat y)    { glVertexAttrib2f(idx, x, y); }
SOFTFP void glVertexAttrib3f_abi(GLuint idx, GLfloat x, GLfloat y, GLfloat z)
    { glVertexAttrib3f(idx, x, y, z); }
SOFTFP void glVertexAttrib4f_abi(GLuint idx, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
    { glVertexAttrib4f(idx, x, y, z, w); }

SOFTFP void glUniform4fHook(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
    { glUniform4f(loc, x, y, z, w); }

SOFTFP void glClearColorHook(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
    { glClearColor(r, g, b, a); }

/* ── Patch entry ──────────────────────────────────────────────────────── */

void patch_opengl(void) {
    hook_addr(so_symbol(&gtalcs_mod, "glVertexAttribPointer"),
              (uintptr_t)glVertexAttribPointerHook);
    hook_addr(so_symbol(&gtalcs_mod, "glDrawArrays"),
              (uintptr_t)glDrawArraysHook);
    hook_addr(so_symbol(&gtalcs_mod, "glLinkProgram"),
              (uintptr_t)glLinkProgramHook);
    hook_addr(so_symbol(&gtalcs_mod, "glUseProgram"),
              (uintptr_t)glUseProgramHook);
    hook_addr(so_symbol(&gtalcs_mod, "glTexParameteri"),
              (uintptr_t)glTexParameteriHook);
}
