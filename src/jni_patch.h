#ifndef JNI_PATCH_H
#define JNI_PATCH_H

extern char fake_vm[0x1000];
extern char fake_env[0x1000];

/* Build the fake JavaVM / JNIEnv vtables.  Call before jni_boot_lcs(). */
void jni_init(void);

/* Drive the engine's exported JNI entry points in Android's order, then run
 * the frame loop.  Does not return. */
void jni_boot_lcs(void);

#endif /* JNI_PATCH_H */

/* Read-only dump of CStreaming's exported counters (no hooks, no trampolines).
 * Called from the CPed::SetModelIndex hook at the moment a special slot is used
 * unloaded — that is the one instant where the counters answer anything. */
void streaming_dump(void);
