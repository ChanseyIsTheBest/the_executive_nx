/* exec_lazy.h -- lazy native backing for bionic pthread objects.
 *
 * MIT licensed. See LICENSE.
 *
 * THE PROBLEM THIS EXISTS TO FIX
 * -----------------------------
 * The reused shims back a caller's pthread object by storing a heap pointer
 * *inside* it and reading that pointer back with an acquire load:
 *
 *     uintptr_t cur = __atomic_load_n((uintptr_t *)uid, __ATOMIC_ACQUIRE);
 *
 * which compiles to `ldar x19, [x0]`. That works when the caller's object is
 * a pointer-sized, 8-byte-aligned opaque handle, which is what the Unity
 * titles those shims came from actually used.
 *
 * A real bionic pthread object is not that. On LP64 bionic they are:
 *
 *     pthread_mutex_t   { int32_t __private[10];  }   40 bytes, _Alignof 4
 *     pthread_cond_t    { int32_t __private[12];  }   48 bytes, _Alignof 4
 *     pthread_rwlock_t  { int32_t __private[14];  }   56 bytes, _Alignof 4
 *
 * Four-byte aligned. AArch64 `LDAR` requires natural alignment regardless of
 * SCTLR_EL1.A, so a 64-bit acquire load of a 4-aligned address is an
 * alignment fault, not a slow path.
 *
 * This port shipped a build that did exactly that and took a Data Abort at
 * `ensure_mutex+0xc`, `ldar x19, [x0]`, x0 = 0x63bb7aa924 -- four modulo
 * eight -- during libc++_shared.so's static initialisers, on the very first
 * mutex libc++ touched. It reproduces every time, because the offending
 * object is a static in libc++ and its alignment is fixed at link time.
 *
 * THE FIX
 * -------
 * Use the first int32 of the caller's object as the handle. 32-bit atomics
 * need 4-byte alignment, which bionic guarantees, so the access is always
 * legal. The int32 holds an index into a table of real objects rather than a
 * pointer, because a pointer does not fit.
 *
 * That also reads the bionic initializers correctly: a freshly
 * PTHREAD_*_INITIALIZER'd object has 0 in its first word, and the recursive
 * and errorcheck variants have 0x4000 and 0x8000. Tagging our indices with
 * the top bit keeps them clear of all three.
 *
 * RACE-FREE BY CONSTRUCTION, and it has to be: libc++'s shared_ptr atomics
 * (__sp_mut::lock) reach this from any thread that touches a shared_ptr, and
 * this port has a decode thread and a mixer thread of its own. A
 * read-test-write would let two threads each allocate for the same slot; the
 * loser's object is then locked by one thread while the winner's is locked by
 * the other, both believing they exclude each other. That is silent data
 * corruption rather than a crash. The compare-exchange means one allocation
 * wins and the loser adopts it.
 */
#ifndef EXEC_LAZY_H
#define EXEC_LAZY_H

#include <stdint.h>

typedef enum {
  EXEC_LAZY_MUTEX = 0,
  EXEC_LAZY_COND,
  EXEC_LAZY_RWLOCK,
} exec_lazy_kind;

/* Return the native object backing `obj`, creating it on first use.
 * `obj` points at the caller's bionic object; only its first int32 is read
 * or written. NULL only if allocation failed or the table is full. */
void *exec_lazy_get(void *obj, exec_lazy_kind kind);

/* Release the backing for `obj` if it has one, and clear the handle word.
 * Safe on an object that was never used. */
void  exec_lazy_release(void *obj, exec_lazy_kind kind);

/* Has `obj` been backed? Lets a *_destroy shim avoid materialising an object
 * only to immediately destroy it. */
int   exec_lazy_present(void *obj);

#endif
