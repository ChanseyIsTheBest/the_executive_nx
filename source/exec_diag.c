/* exec_diag.c -- see exec_diag.h. MIT licensed. */

#include <string.h>
#include <switch.h>

#include "exec_diag.h"
#include "exec_log.h"

#if EXEC_DIAG

#define MAX_THREADS 16

typedef struct {
  int          used;
  uint32_t     tid;
  const void  *entry;
  int          is_main;
  volatile int wait_kind;      /* 0 = running */
  const void  *wait_obj;
  volatile uint64_t wait_since;
} slot;

static slot g_slots[MAX_THREADS];

/* No lock anywhere in this file.
 *
 * The whole point of a diagnostic is to work when something is stuck, and a
 * wedged thread is very often stuck holding exactly the lock a naive
 * implementation would take here. Slots are claimed with a compare-and-swap
 * and every field a reader touches is written before `used` is published, so
 * a torn read is possible in principle and harmless in practice -- the worst
 * case is one garbled line in the log. The Osmos port's watchdog took a lock
 * and reported nothing for two test cycles because of it. */
static slot *find_self(void) {
  const uint32_t me = threadGetCurHandle();
  for (int i = 0; i < MAX_THREADS; i++)
    if (g_slots[i].used && g_slots[i].tid == me) return &g_slots[i];
  return NULL;
}

void diag_init(void) { memset(g_slots, 0, sizeof(g_slots)); }

void diag_thread_register(const void *entry, int is_main) {
  for (int i = 0; i < MAX_THREADS; i++) {
    if (__atomic_exchange_n(&g_slots[i].used, 1, __ATOMIC_ACQ_REL)) continue;
    g_slots[i].tid       = threadGetCurHandle();
    g_slots[i].entry     = entry;
    g_slots[i].is_main   = is_main;
    g_slots[i].wait_kind = 0;
    return;
  }
  exec_log(EXEC_LOG_WARN, "diag: no free thread slot (>%d threads)", MAX_THREADS);
}

void diag_thread_unregister(void) {
  slot *s = find_self();
  if (s) __atomic_store_n(&s->used, 0, __ATOMIC_RELEASE);
}

void diag_wait_enter(int kind, const void *obj) {
  slot *s = find_self();
  if (!s) return;
  s->wait_obj   = obj;
  s->wait_since = armGetSystemTick();
  __atomic_store_n(&s->wait_kind, kind, __ATOMIC_RELEASE);
}

void diag_wait_exit(void) {
  slot *s = find_self();
  if (s) __atomic_store_n(&s->wait_kind, 0, __ATOMIC_RELEASE);
}

static const char *kind_name(int k) {
  switch (k) {
    case DIAG_W_MUTEX: return "mutex";
    case DIAG_W_COND:  return "cond";
    case DIAG_W_JOIN:  return "join";
    case DIAG_W_SEM:   return "sem";
    default:           return "run";
  }
}

void diag_dump(void) {
  const uint64_t now = armGetSystemTick();
  for (int i = 0; i < MAX_THREADS; i++) {
    slot *s = &g_slots[i];
    if (!__atomic_load_n(&s->used, __ATOMIC_ACQUIRE)) continue;
    const int k = __atomic_load_n(&s->wait_kind, __ATOMIC_ACQUIRE);
    if (!k) {
      exec_log(EXEC_LOG_INFO, "diag: thread %u entry=%p%s running",
              s->tid, s->entry, s->is_main ? " (main)" : "");
    } else {
      const uint64_t ms = armTicksToNs(now - s->wait_since) / 1000000ull;
      exec_log(EXEC_LOG_INFO, "diag: thread %u entry=%p%s waiting on %s %p for %llu ms",
              s->tid, s->entry, s->is_main ? " (main)" : "",
              kind_name(k), s->wait_obj, (unsigned long long)ms);
    }
  }
}

#endif
