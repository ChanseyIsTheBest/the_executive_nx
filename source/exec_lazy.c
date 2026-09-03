/* exec_lazy.c -- see exec_lazy.h. MIT licensed. */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <switch.h>

#include "exec_lazy.h"
#include "exec_log.h"

/* Bionic's static initialisers, as they appear in the object's first int32. */
#define BIONIC_INIT_NORMAL      0x0000
#define BIONIC_INIT_RECURSIVE   0x4000
#define BIONIC_INIT_ERRORCHECK  0x8000

/* Our handles are tagged so they cannot collide with any of the above. */
#define HANDLE_TAG              0x80000000u
#define HANDLE_MASK             0x7fffffffu

/* Every std::mutex, std::condition_variable and shared_ptr control block in
 * libc++ that is ever locked takes a slot, plus whatever the engine uses.
 * 4096 is far more than either needs and costs 32 KB. */
#define MAX_SLOTS 4096

static void *g_slots[MAX_SLOTS];
static uint32_t g_next = 1;              /* 0 is never handed out */

/* A libnx Mutex is a u32 whose unlocked state is 0, and mutexInit does
 * nothing but store 0 -- so a file-scope one is already initialised before
 * main runs. An earlier version called mutexInit lazily on first use, which
 * was both unnecessary and itself a race: two threads could pass the
 * `!ready` test together and one would reset the lock the other held. */
static Mutex g_table_lock;

static void *make_object(exec_lazy_kind kind, uint32_t init_word) {
  switch (kind) {
  case EXEC_LAZY_MUTEX: {
    pthread_mutex_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    int rc;
    if (init_word == BIONIC_INIT_RECURSIVE) {
      pthread_mutexattr_t a;
      pthread_mutexattr_init(&a);
      pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
      rc = pthread_mutex_init(m, &a);
      pthread_mutexattr_destroy(&a);
    } else {
      /* ERRORCHECK is treated as normal: newlib's errorcheck reporting is
       * not what any caller here branches on, and a normal mutex is the
       * strictly more permissive choice. */
      rc = pthread_mutex_init(m, NULL);
    }
    if (rc != 0) { free(m); return NULL; }
    return m;
  }
  case EXEC_LAZY_COND: {
    pthread_cond_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (pthread_cond_init(c, NULL) != 0) { free(c); return NULL; }
    return c;
  }
  case EXEC_LAZY_RWLOCK: {
    RwLock *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    rwlockInit(l);
    return l;
  }
  }
  return NULL;
}

static void destroy_object(exec_lazy_kind kind, void *p) {
  if (!p) return;
  switch (kind) {
  case EXEC_LAZY_MUTEX:  pthread_mutex_destroy((pthread_mutex_t *)p); break;
  case EXEC_LAZY_COND:   pthread_cond_destroy((pthread_cond_t *)p);   break;
  case EXEC_LAZY_RWLOCK: break;                       /* RwLock needs no teardown */
  }
  free(p);
}

void *exec_lazy_get(void *obj, exec_lazy_kind kind) {
  if (!obj) return NULL;
  uint32_t *word = (uint32_t *)obj;      /* 4-aligned by bionic's own layout */

  uint32_t cur = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  if (cur & HANDLE_TAG) {
    const uint32_t idx = cur & HANDLE_MASK;
    return (idx < MAX_SLOTS) ? g_slots[idx] : NULL;
  }

  void *o = make_object(kind, cur);
  if (!o) return NULL;

  mutexLock(&g_table_lock);
  const uint32_t idx = g_next;
  if (idx >= MAX_SLOTS) {
    mutexUnlock(&g_table_lock);
    destroy_object(kind, o);
    exec_log(EXEC_LOG_ERROR, "exec_lazy: table full (%d slots)", MAX_SLOTS);
    return NULL;
  }
  g_slots[idx] = o;
  g_next = idx + 1;
  mutexUnlock(&g_table_lock);

  /* Publish. The slot is written before the tag becomes visible, so any
   * thread that sees the tag also sees the object. */
  uint32_t expected = cur;
  if (__atomic_compare_exchange_n(word, &expected, HANDLE_TAG | idx,
                                  0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return o;

  /* Another thread got there first. Drop ours and adopt the winner's; the
   * slot we took is leaked, which is bounded by the number of races and is
   * far cheaper than making slot reuse thread-safe. */
  destroy_object(kind, o);
  mutexLock(&g_table_lock);
  g_slots[idx] = NULL;
  mutexUnlock(&g_table_lock);

  const uint32_t widx = expected & HANDLE_MASK;
  return (expected & HANDLE_TAG) && widx < MAX_SLOTS ? g_slots[widx] : NULL;
}

int exec_lazy_present(void *obj) {
  if (!obj) return 0;
  return (__atomic_load_n((uint32_t *)obj, __ATOMIC_ACQUIRE) & HANDLE_TAG) != 0;
}

void exec_lazy_release(void *obj, exec_lazy_kind kind) {
  if (!obj) return;
  uint32_t *word = (uint32_t *)obj;
  const uint32_t cur = __atomic_exchange_n(word, 0u, __ATOMIC_ACQ_REL);
  if (!(cur & HANDLE_TAG)) return;

  const uint32_t idx = cur & HANDLE_MASK;
  if (idx >= MAX_SLOTS) return;

  mutexLock(&g_table_lock);
  void *o = g_slots[idx];
  g_slots[idx] = NULL;
  mutexUnlock(&g_table_lock);
  destroy_object(kind, o);
}
