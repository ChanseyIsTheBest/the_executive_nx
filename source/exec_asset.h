/* exec_asset.h -- AAssetManager over a directory on the SD card.
 *
 * The engine opens assets through AAssetManager_open / AAsset_read /
 * AAsset_getLength / AAsset_close and nothing else -- no AAssetDir, no
 * seek, no getBuffer. Four calls is the whole contract, so the "asset
 * manager" is a directory prefix and AAsset is a FILE *.
 */
#ifndef EXEC_ASSET_H
#define EXEC_ASSET_H
#include <stddef.h>
#include <stdint.h>

typedef struct AAssetManager AAssetManager;
typedef struct AAsset        AAsset;

void  exec_asset_init(const char *assets_dir);

/* Helpers for our own code, not resolver targets. Both go through exec_io's
 * lock: exec_audio.c calls them from the decode thread, beside the engine. */
int   exec_asset_exists(const char *name);
int   exec_asset_read_all(const char *name, void **data, size_t *len);

AAssetManager *AAssetManager_fromJava(void *env, void *obj);
AAsset *AAssetManager_open(AAssetManager *mgr, const char *name, int mode);
int     AAsset_read(AAsset *a, void *buf, size_t count);
int64_t AAsset_getLength(AAsset *a);
void    AAsset_close(AAsset *a);
#endif
