/* exec_log.c -- MIT licensed. See LICENSE.
 *
 * The engine logs under the tag "ExecutiveAndroid" and is not shy about it: the
 * asset loader prints per-file lines. Writing every one straight to the SD
 * card turns a 60 Hz frame into a 6 Hz one, so output is buffered and only
 * flushed on warnings and above. The Osmos port learned this as "the stall
 * was a log flood, not a stall".
 *
 * THIS IS REACHED FROM EVERY THREAD IN THE PROCESS
 * ------------------------------------------------
 * Not just the port's own code: __android_log_print resolves here, so the
 * game thread, every engine worker pthread_create_fake spawns, the audio
 * decode thread and the audren mixer thread all land in the same FILE *.
 * devkitPro's newlib does not lock stdio, so concurrent fprintf calls on one
 * stream interleave at best and corrupt the stream's internal state at worst.
 *
 * So the emit path is serialised, and it takes the SAME recursive lock as the
 * rest of the port's file I/O rather than a private one. Two locks over one
 * non-thread-safe handle table would leave exactly the race each was added to
 * prevent.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "exec_log.h"
#include "exec_io.h"

static FILE *lf;
static int   suppressed;

void exec_log_open(const char *path) {
#if defined(EXEC_LOG_SILENT) && EXEC_LOG_SILENT
  /* `make LOG=0`. Not opening the file is the whole implementation: emit()
   * already returns on a null handle, so every remaining path -- including
   * the __android_log_* resolver targets the two modules import -- goes
   * quiet without a second mechanism to keep in step with this one. */
  (void)path;
  lf = NULL;
#else
  lf = fopen_locked(path, "w");
  if (lf) setvbuf(lf, NULL, _IOFBF, 1 << 16);
#endif
}

void exec_log_close(void) {
  FILE *f = lf;
  lf = NULL;                   /* stop new writers before the handle goes away */
  if (f) { io_enter(); fflush(f); io_leave(); fclose_locked(f); }
}

void exec_log_flush(void) {
  if (!lf) return;
  io_enter();
  if (lf) fflush(lf);
  io_leave();
}

static const char lvl[] = "??VDIWEF";

static void emit(int prio, const char *tag, const char *fmt, va_list ap) {
  if (!lf || prio < EXEC_LOG_MIN) return;

  io_enter();
  /* Re-check under the lock: exec_log_close may have cleared it between the
   * cheap test above and here. */
  if (lf && !(prio < EXEC_LOG_WARN && suppressed++ > 20000)) {
    fprintf(lf, "%c/%s: ", lvl[(prio >= 0 && prio < 8) ? prio : 0],
            tag ? tag : "-");
    vfprintf(lf, fmt, ap);
    fputc('\n', lf);
    if (prio >= EXEC_LOG_WARN) fflush(lf);
  }
  io_leave();
}

void exec_log_impl(int prio, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); emit(prio, "executive_nx", fmt, ap); va_end(ap);
}
int exec_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); emit(prio, tag, fmt, ap); va_end(ap); return 0;
}
int exec_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  emit(prio, tag, fmt, ap); return 0;
}
int exec_log_write(int prio, const char *tag, const char *text) {
  return exec_log_print(prio, tag, "%s", text ? text : "");
}
/* android_set_abort_message is the last thing bionic code does before it
 * dies, so this is the one place that flushes unconditionally. */
void exec_set_abort_message(const char *msg) {
  exec_log_impl(EXEC_LOG_FATAL, "abort: %s", msg ? msg : "(null)");
  if (lf) { io_enter(); fflush(lf); io_leave(); }
}
