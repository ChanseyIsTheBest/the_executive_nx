/* exec_shim.c -- MIT licensed. See LICENSE. */
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>
#include "exec_shim.h"
#include "so_util.h"
#include "exec_log.h"

FILE *exec_stdout_ptr;

void exec_shim_init(void) { exec_stdout_ptr = stdout; }

/* The engine reads ro.build.version.sdk once, through RMSystemAndroid, and
 * branches on API level for nothing that exists here. Reporting a recent
 * level keeps it out of the legacy paths. */
int exec_system_property_get(const char *name, char *value) {
  const char *v = "";
  if (!name || !value) return 0;
  if (!strcmp(name, "ro.build.version.sdk"))      v = "33";
  else if (!strcmp(name, "ro.product.model"))     v = "Switch";
  else if (!strcmp(name, "ro.product.manufacturer")) v = "Nintendo";
  else if (!strcmp(name, "ro.build.version.release")) v = "13";
  size_t n = strlen(v);
  memcpy(value, v, n + 1);
  return (int)n;
}

/* libc++_shared's unwinder calls getauxval(AT_PLATFORM/AT_HWCAP) during
 * static init. Returning 0 is correct here -- it falls back to the generic
 * path, which is the one that works. */
unsigned long exec_getauxval(unsigned long type) { (void)type; return 0; }

int exec_register_atfork(void (*p)(void), void (*c)(void), void (*x)(void), void *d) {
  (void)p; (void)c; (void)x; (void)d; return 0;   /* no fork here */
}

void exec_cxa_finalize(void *dso) { (void)dso; }

/* The module never calls dlopen on anything real; these exist because the
 * import table has to be complete for a BIND_NOW link. */
void *exec_dlopen(const char *name, int flags) {
  (void)flags;
  exec_log(EXEC_LOG_WARN, "dlopen(%s) -- returning a token", name ? name : "(null)");
  return (void *)1;
}
void *exec_dlsym(void *h, const char *name) {
  (void)h;
  return (void *)so_resolve_external(name);
}
int   exec_dlclose(void *h) { (void)h; return 0; }
char *exec_dlerror(void)    { return NULL; }

/* std::filesystem reaches for both of these on a normal Linux build. Neither
 * path is taken here: every file the modules touch is a plain file on the SD
 * card, and there is no socket layer at all -- libexecutive_android.so imports no
 * socket symbol, checked against its .dynsym. Failing is the honest answer
 * and shows up in the log if it ever happens. */
int exec_ioctl(int fd, unsigned long req, ...) {
  (void)fd; (void)req;
  exec_log(EXEC_LOG_WARN, "ioctl(%lu) -- unsupported", req);
  errno = ENOTTY;
  return -1;
}

long exec_sendfile(int out_fd, int in_fd, long *offset, size_t count) {
  (void)out_fd; (void)in_fd; (void)offset; (void)count;
  errno = EINVAL;
  return -1;
}

/* THE *at() FAMILY
 * ----------------
 * All three are reached only from libc++_shared's std::filesystem, which
 * neither module actually uses -- the engine opens named files through
 * AAssetManager and fopen. They exist because the resolver table has to be
 * complete for a BIND_NOW link.
 *
 * The returns are chosen so that if std::filesystem ever does run, it gets
 * the answer a FAT volume would give rather than an error it has no path for:
 *
 *   fchmodat   there are no POSIX permissions on exFAT. chmod on a FAT mount
 *              is a no-op that succeeds, so this succeeds too. Returning -1
 *              would make filesystem::permissions() throw.
 *   fdopendir  there is no way to turn a newlib fd into a DIR *, and faking
 *              one would hand out a handle that later crashes. Fail honestly.
 *   utimensat  timestamps are writable on FAT but nothing here wants to set
 *              them; succeeding silently is closer to the truth than ENOSYS.
 */
int exec_fchmodat(int dirfd, const char *path, unsigned mode, int flags) {
  (void)dirfd; (void)path; (void)mode; (void)flags;
  return 0;
}

void *exec_fdopendir(int fd) {
  (void)fd;
  exec_log(EXEC_LOG_WARN, "fdopendir -- unsupported");
  errno = ENOTSUP;
  return NULL;
}

int exec_utimensat(int dirfd, const char *path, const void *times, int flags) {
  (void)dirfd; (void)path; (void)times; (void)flags;
  return 0;
}
