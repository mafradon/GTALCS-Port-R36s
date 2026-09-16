#ifndef ASSET_MANAGER_H
#define ASSET_MANAGER_H

#include <stddef.h>

struct AAsset;

/* Override the directory the shim reads assets from (defaults to ASSETS_PATH). */
void asset_manager_set_root(const char *path);

void          *AAssetManager_fromJava(void *env, void *assetManager);
struct AAsset *AAssetManager_open(void *mgr, const char *filename, int mode);
int            AAsset_read(struct AAsset *a, void *buf, size_t count);
long           AAsset_seek(struct AAsset *a, long offset, int whence);
long           AAsset_getLength(struct AAsset *a);
long           AAsset_getRemainingLength(struct AAsset *a);
void           AAsset_close(struct AAsset *a);

#endif /* ASSET_MANAGER_H */
