/* exec_asset.c -- MIT licensed. See LICENSE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exec_asset.h"
#include "exec_log.h"
#include "exec_io.h"

static char root[512];

void exec_asset_init(const char *assets_dir) {
  snprintf(root, sizeof(root), "%s", assets_dir);
  size_t n = strlen(root);
  while (n && (root[n-1] == '/' || root[n-1] == '\\')) root[--n] = 0;
}

AAssetManager *AAssetManager_fromJava(void *env, void *obj) {
  (void)env; (void)obj;
  return (AAssetManager *)root;      /* the prefix is the manager */
}

/* Every call here goes through exec_io's lock.
 *
 * These are not called only from the game thread: pthread_create_fake spawns
 * whatever workers the engine wants, and this port additionally runs an audio
 * decode thread that streams a track off the same card. devkitPro's newlib
 * keeps a process-wide file handle table with no locking of its own, so an
 * unlocked fopen here racing a locked fread there is the same bug as no
 * locking at all. */
AAsset *AAssetManager_open(AAssetManager *mgr, const char *name, int mode) {
  (void)mgr; (void)mode;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", root, name ? name : "");
  FILE *f = fopen_locked(path, "rb");
  if (!f) {
    /* DEBUG, not WARN. A miss is the normal case, not an error:
     * RMSystemAndroid::resolve_asset_name probes six directories for every
     * file it opens, so five misses per hit is the steady state. The first
     * run logged 60 of them before the first real load.
     *
     * Level matters beyond noise. exec_log flushes at WARN and above, so
     * every one of those was an SD-card write, in the middle of the asset
     * load, on the thread the frame loop is waiting for. */
    exec_log(EXEC_LOG_DEBUG, "asset miss: %s", path);
    return NULL;
  }
  return (AAsset *)f;
}

int AAsset_read(AAsset *a, void *buf, size_t count) {
  if (!a) return -1;
  size_t got = fread_locked(buf, 1, count, (FILE *)a);
  return (int)got;
}

int64_t AAsset_getLength(AAsset *a) {
  if (!a) return 0;
  FILE *f = (FILE *)a;
  /* Three calls that must not be interleaved: another thread seeking this
   * same handle between them would leave the file position moved and this
   * function returning a length for a position it did not restore. */
  io_enter();
  long cur = ftell(f);
  fseek(f, 0, SEEK_END);
  long end = ftell(f);
  fseek(f, cur, SEEK_SET);
  io_leave();
  return (int64_t)end;
}

void AAsset_close(AAsset *a) { if (a) fclose_locked((FILE *)a); }

int exec_asset_exists(const char *name) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", root, name ? name : "");
  FILE *f = fopen_locked(path, "rb");
  if (!f) return 0;
  fclose_locked(f);
  return 1;
}

int exec_asset_read_all(const char *name, void **data, size_t *len) {
  *data = NULL; *len = 0;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", root, name ? name : "");
  FILE *f = fopen_locked(path, "rb");
  if (!f) { exec_log(EXEC_LOG_DEBUG, "asset miss: %s", path); return 0; }
  fseek_locked(f, 0, SEEK_END);
  const long n = ftell_locked(f);
  fseek_locked(f, 0, SEEK_SET);
  if (n <= 0) { fclose_locked(f); return 0; }
  void *p = malloc((size_t)n);
  if (!p) { fclose_locked(f); return 0; }
  const size_t got = fread_locked(p, 1, (size_t)n, f);
  fclose_locked(f);
  if (got != (size_t)n) { free(p); return 0; }
  *data = p; *len = (size_t)n;
  return 1;
}
