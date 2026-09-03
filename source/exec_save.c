/* exec_save.c -- MIT licensed. See LICENSE.
 *
 * CloudSaveBridge on Android goes through Play Games Snapshots and refuses to
 * do anything while unauthenticated. There is no Play Games here, so the same
 * calls are backed by a file next to the .nro. The engine cannot tell the
 * difference: it only ever sees the three read outcomes and the write result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "exec_save.h"
#include "exec_log.h"
#include "exec_io.h"

static char dir[512];

void exec_save_init(const char *d) {
  snprintf(dir, sizeof(dir), "%s", d);
  mkdir(dir, 0777);
}

/* Keys come from the engine and are short identifiers, but they are not
 * ours, so anything that could climb out of the directory is rejected
 * rather than sanitised. */
static int safe_key(const char *k) {
  if (!k || !*k || strlen(k) > 96) return 0;
  for (const char *p = k; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
      return 0;
  if (strstr(k, "..")) return 0;
  return 1;
}

static void keypath(char *out, size_t n, const char *key) {
  snprintf(out, n, "%s/%s.cloud", dir, key);
}

int exec_save_read(const char *key, void **buf, int *len) {
  *buf = NULL; *len = 0;
  if (!safe_key(key)) return -1;
  char path[768]; keypath(path, sizeof(path), key);
  FILE *f = fopen_locked(path, "rb");
  if (!f) return 0;                       /* absent -> nativeReadMissing */
  io_enter();
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  io_leave();
  if (n < 0) { fclose_locked(f); return -1; }
  void *p = malloc((size_t)n + 1);
  if (!p) { fclose_locked(f); return -1; }
  size_t got = fread_locked(p, 1, (size_t)n, f);
  fclose_locked(f);
  if (got != (size_t)n) { free(p); return -1; }
  *buf = p; *len = (int)n;
  return 1;
}

int exec_save_write(const char *key, const void *buf, int len) {
  if (!safe_key(key) || len < 0) return 0;
  char path[768], tmp[800];
  keypath(path, sizeof(path), key);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen_locked(tmp, "wb");
  if (!f) { exec_log(EXEC_LOG_ERROR, "cloud write open failed: %s", tmp); return 0; }
  int ok = (len == 0) || (fwrite_locked(buf, 1, (size_t)len, f) == (size_t)len);
  if (fclose_locked(f) != 0) ok = 0;
  if (!ok) { remove(tmp); return 0; }
  remove(path);
  if (rename(tmp, path) != 0) { remove(tmp); return 0; }
  return 1;
}
