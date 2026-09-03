/* exec_rwlock.c -- rwlock shims over exec_lazy. MIT licensed. See LICENSE.
 *
 * libc_shim.c has its own pthread_rwlock_*_fake, and they carry the same
 * assumption that broke the mutex path: `get_rwlock(void **storage)` reads
 * and writes a pointer through the caller's object. A bionic pthread_rwlock_t
 * is `int32_t __private[14]` -- 56 bytes, four-byte aligned -- so a
 * pointer-width slot there is neither the right size nor reliably aligned.
 *
 * libexecutive_android.so imports pthread_rwlock_rdlock, _wrlock and _unlock, and
 * libc++_shared.so imports the same three, so this path is live.
 *
 * These are here rather than as an edit to libc_shim.c because that file is
 * reused byte-identical from the reference ports; imports_exec.c routes the
 * three symbols to these instead, which is the tree's normal way of making an
 * intentional override.
 */

#include <switch.h>

#include "exec_rwlock.h"
#include "exec_lazy.h"

static RwLock *back(void *rw) {
  return (RwLock *)exec_lazy_get(rw, EXEC_LAZY_RWLOCK);
}

int exec_rwlock_rdlock(void *rw) {
  RwLock *l = back(rw);
  if (!l) return -1;
  rwlockReadLock(l);
  return 0;
}

int exec_rwlock_wrlock(void *rw) {
  RwLock *l = back(rw);
  if (!l) return -1;
  rwlockWriteLock(l);
  return 0;
}

int exec_rwlock_tryrdlock(void *rw) {
  RwLock *l = back(rw);
  if (!l) return -1;
  return rwlockTryReadLock(l) ? 0 : 16 /* EBUSY */;
}

int exec_rwlock_trywrlock(void *rw) {
  RwLock *l = back(rw);
  if (!l) return -1;
  return rwlockTryWriteLock(l) ? 0 : 16 /* EBUSY */;
}

int exec_rwlock_unlock(void *rw) {
  RwLock *l = back(rw);
  if (!l) return -1;
  /* libnx keeps read and write locks in one object but unlocks them through
   * different calls, and the caller does not tell us which it holds. Asking
   * the lock is the only correct way to choose. */
  if (rwlockIsWriteLockHeldByCurrentThread(l)) rwlockWriteUnlock(l);
  else                                         rwlockReadUnlock(l);
  return 0;
}

int exec_rwlock_init(void *rw, const void *attr) {
  (void)attr;
  exec_lazy_release(rw, EXEC_LAZY_RWLOCK);
  return 0;
}

int exec_rwlock_destroy(void *rw) {
  exec_lazy_release(rw, EXEC_LAZY_RWLOCK);
  return 0;
}
