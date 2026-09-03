/* exec_diag.h -- thread and wait instrumentation hooks.
 *
 * MIT licensed. See LICENSE.
 *
 * imports_helpers.c calls these from the pthread shims so a hang can be
 * attributed to a thread and a wait object rather than guessed at.
 *
 * Unlike the Osmos port, which had to build a stall watchdog and a GL trace
 * from nothing, this game ships its own instrumentation: PVS_DEBUG_INPUT,
 * PVS_DEBUG_AUDIO, PVS_DEBUG_FRAME_TIMING, PVS_DEBUG_DWARP and the
 * PVS_AUTOMATION_* keys all reach the engine through nativeSetEnv. So this
 * file stays small on purpose -- turn the engine's own logging on first, and
 * only reach for a watchdog if the engine has gone quiet.
 *
 * Off in a release build; `make DEBUG=1` compiles the hooks back in. With it
 * off every one of them reduces to nothing, so the pthread shims carry no
 * instrumentation cost at all.
 */
#ifndef EXEC_DIAG_H
#define EXEC_DIAG_H

enum { DIAG_W_MUTEX = 1, DIAG_W_COND, DIAG_W_JOIN, DIAG_W_SEM };

/* Must agree with config.h, which guards the same name. Off unless asked. */
#ifndef EXEC_DIAG
#define EXEC_DIAG 0
#endif

#if EXEC_DIAG

void diag_init(void);
void diag_thread_register(const void *entry, int is_main);
void diag_thread_unregister(void);
void diag_wait_enter(int kind, const void *obj);
void diag_wait_exit(void);

/* Write a one-line snapshot of every registered thread and what it is waiting
 * on. Safe to call from anywhere, including from a thread that is itself
 * wedged: it takes no locks. */
void diag_dump(void);

#else
#define diag_init()                ((void)0)
#define diag_thread_register(e, m) ((void)0)
#define diag_thread_unregister()   ((void)0)
#define diag_wait_enter(k, o)      ((void)0)
#define diag_wait_exit()           ((void)0)
#define diag_dump()                ((void)0)
#endif

#endif
