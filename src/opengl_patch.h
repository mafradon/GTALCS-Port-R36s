#ifndef OPENGL_PATCH_H
#define OPENGL_PATCH_H

#define SOFTFP __attribute__((pcs("aapcs")))

void patch_opengl(void);
void glVertexAttribPointerHook(unsigned int, int, unsigned int, unsigned char, int, const void *);
void glDrawArraysHook(unsigned int, int, int);
void glDrawElementsHook(unsigned int, int, unsigned int, const void *);
void glLinkProgramHook(unsigned int);
void glUseProgramHook(unsigned int);
void glTexImage2DHook(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
void glTexSubImage2DHook(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
void glCompressedTexImage2DHook(unsigned int, int, unsigned int, int, int, int, int, const void *);
void glShaderSourceHook(unsigned int, int, const char **, const int *);
void glBindAttribLocationHook(unsigned int, unsigned int, const char *);
void glUniform4fvHook(int, int, const float *);
void glBlendFuncHook(unsigned int, unsigned int);
void glEnableHook(unsigned int);
void glDisableHook(unsigned int);
void glDepthMaskHook(unsigned char);
void glBindFramebufferHook(unsigned int, unsigned int);
void glFramebufferTexture2DHook(unsigned int, unsigned int, unsigned int, unsigned int, int);
void glCompileShaderHook(unsigned int);
void glUniformMatrix4fvHook(int, int, unsigned char, const float *);
void glUniform3fvHook(int, int, const float *);
SOFTFP void glUniform4fHook(int, float, float, float, float);
SOFTFP void glClearColorHook(float, float, float, float);
int  CShaderProgram__CompileShaderWithFlags(void *, unsigned int, int);

/* Softfp ABI thunks — bridge Android soft-float calls to hard-float libMali.so */
SOFTFP void glUniform1f_abi(int, float);
SOFTFP void glUniform2f_abi(int, float, float);
SOFTFP void glUniform3f_abi(int, float, float, float);
SOFTFP void glTexParameterf_abi(unsigned int, unsigned int, float);
SOFTFP void glDepthRangef_abi(float, float);
SOFTFP void glClearDepthf_abi(float);
SOFTFP void glLineWidth_abi(float);
SOFTFP void glPolygonOffset_abi(float, float);
SOFTFP void glSampleCoverage_abi(float, unsigned char);
SOFTFP void glVertexAttrib1f_abi(unsigned int, float);
SOFTFP void glVertexAttrib2f_abi(unsigned int, float, float);
SOFTFP void glVertexAttrib3f_abi(unsigned int, float, float, float);
SOFTFP void glVertexAttrib4f_abi(unsigned int, float, float, float, float);

#endif
