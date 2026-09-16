/* asset_manager.c -- AAssetManager shim for the GTA:LCS loader
 *
 * libGTALcs.so reads the APK's assets/ folder through Android's NDK asset API:
 * fonts, images/, json/socialClubAssets.json, xml/audio_data.xml.  On Android
 * those live compressed inside the APK; here the installer has already
 * extracted them to ASSETS_PATH, so every asset is just a file on disk and the
 * whole API collapses to a thin wrapper over stdio.
 *
 * The manager handle the game receives is our own opaque pointer — it only ever
 * comes back to us, so it needs no Android-compatible layout.  Note the game
 * obtains it via AAssetManager_fromJava(env, jobject), whose jobject comes from
 * our fake JNI layer; both sides are ours, so the value is arbitrary.
 *
 * None of these entry points take or return a float, so no SOFTFP bridging is
 * needed here — unusual for this port, and worth stating so the next reader
 * does not go looking for a missing bridge.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

#include "config.h"
#include "asset_manager.h"

/* The one manager instance.  Address is the handle; contents are unused. */
static int g_asset_manager_token;

struct AAsset {
    FILE  *fp;
    long   length;      /* total size, cached at open */
};

/* Half of PATH_MAX so root + "/" + filename provably fits the join buffer. */
static char g_assets_root[PATH_MAX / 2] = ASSETS_PATH;

void asset_manager_set_root(const char *path) {
    if (!path || !*path) return;
    snprintf(g_assets_root, sizeof(g_assets_root), "%s", path);
}

void *AAssetManager_fromJava(void *env, void *assetManager) {
    (void)env; (void)assetManager;
    return &g_asset_manager_token;
}

/* mode is AASSET_MODE_{UNKNOWN,RANDOM,STREAMING,BUFFER} — all are advisory
 * hints about access pattern, and stdio handles every one of them. */
struct AAsset *AAssetManager_open(void *mgr, const char *filename, int mode) {
    (void)mgr; (void)mode;
    if (!filename) return NULL;

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", g_assets_root, filename)
            >= (int)sizeof(path)) {
        fprintf(stderr, "[asset] path too long: %s\n", filename);
        return NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[asset] MISSING: %s\n", path);
        return NULL;
    }

    struct AAsset *a = calloc(1, sizeof(*a));
    if (!a) { fclose(fp); return NULL; }

    a->fp = fp;
    fseek(fp, 0, SEEK_END);
    a->length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    return a;
}

int AAsset_read(struct AAsset *a, void *buf, size_t count) {
    if (!a || !a->fp) return -1;
    return (int)fread(buf, 1, count, a->fp);
}

/* Returns the new absolute offset, or -1.  Matches the NDK contract. */
long AAsset_seek(struct AAsset *a, long offset, int whence) {
    if (!a || !a->fp) return -1;
    if (fseek(a->fp, offset, whence) != 0) return -1;
    return ftell(a->fp);
}

long AAsset_getLength(struct AAsset *a) {
    return a ? a->length : 0;
}

long AAsset_getRemainingLength(struct AAsset *a) {
    if (!a || !a->fp) return 0;
    long pos = ftell(a->fp);
    return pos < 0 ? 0 : a->length - pos;
}

void AAsset_close(struct AAsset *a) {
    if (!a) return;
    if (a->fp) fclose(a->fp);
    free(a);
}
