/* exec_log.h -- __android_log_* backing, and the port's own log file. */
#ifndef EXEC_LOG_H
#define EXEC_LOG_H
#include <stdarg.h>

enum { EXEC_LOG_VERBOSE = 2, EXEC_LOG_DEBUG, EXEC_LOG_INFO,
       EXEC_LOG_WARN, EXEC_LOG_ERROR, EXEC_LOG_FATAL };

/* THE FLOOR, AND WHY IT IS NOT ZERO IN A RELEASE BUILD
 * ----------------------------------------------------
 * A shipping build drops VERBOSE and DEBUG and keeps everything from INFO up.
 * `make DEBUG=1` lowers the floor to VERBOSE.
 *
 * DEBUG is where the noise lives: RMSystemAndroid::resolve_asset_name probes
 * six directories for every file it opens, so five "asset miss" lines per hit
 * is the steady state, and there are thousands of files. Compiling those out
 * is most of the point.
 *
 * INFO is not noise, and turning it off would be a mistake. Every crash in
 * this port so far was diagnosed from lines at that level -- the two module
 * base addresses turned an Atmosphere report into two subtractions, the EGL
 * surface line would have caught the 720p window immediately, and the
 * six-axis handle line answered "the gyro does not work" before any analysis.
 * A release build that says nothing is a release build whose bug reports are
 * unactionable. */
#ifndef EXEC_LOG_MIN
#  if defined(EXEC_LOG_SILENT) && EXEC_LOG_SILENT
     /* `make LOG=0`. A floor above FATAL means every exec_log() call site
      * folds away completely -- no call, no argument evaluation, no format
      * string left in .rodata -- and exec_log_open never creates the file, so
      * nothing is written and nothing is left on the card.
      *
      * The three resolver targets below stay real functions, because
      * libexecutive_android.so and libc++_shared.so import them by name and a
      * missing symbol is a load-time abort. They cost a null check: emit()
      * returns immediately when no file is open.
      *
      * Understand what this gives up. A crash on a silent build leaves you the
      * Atmosphere report and nothing else -- no module load addresses, so
      * every faulting address has to be matched to a module by guesswork
      * rather than by subtraction. Every crash in this lineage was diagnosed
      * from INFO-level lines. This exists because it was asked for, not
      * because it is the right default. */
#    define EXEC_LOG_MIN 99
#  elif defined(DEBUG_LOG) && DEBUG_LOG
#    define EXEC_LOG_MIN EXEC_LOG_VERBOSE
#  else
#    define EXEC_LOG_MIN EXEC_LOG_INFO
#  endif
#endif

void exec_log_open(const char *path);
void exec_log_close(void);

/* Push the buffer to the card. main.c calls this once a second.
 *
 * Without it the log is EMPTY on any run that does not exit cleanly, which
 * includes every crash -- the one case it exists for. The buffer is 64 KB and
 * only WARN and above flush; demoting the per-file "asset miss" line from
 * WARN to DEBUG (a correct fix for a different problem) removed the last
 * thing that was flushing incidentally, and the next run produced a 0-byte
 * log. */
void exec_log_flush(void);
void exec_log_impl(int prio, const char *fmt, ...) __attribute__((format(printf,2,3)));

/* A macro, so a call below the floor costs nothing at all -- not the call,
 * not the argument evaluation, not the format string in .rodata. The
 * do/while keeps it usable as a statement after a bare `if`. The `prio` is a
 * constant at every call site, so the compiler folds the test away. */
#define exec_log(prio, ...) \
  do { if ((prio) >= EXEC_LOG_MIN) exec_log_impl((prio), __VA_ARGS__); } while (0)

/* Resolver targets. libexecutive_android.so imports the first; libc++_shared.so
 * imports android_set_abort_message.
 *
 * These stay real functions with a RUNTIME check: their priority comes from
 * the engine at call time, so there is no constant to fold. */
int  exec_log_print(int prio, const char *tag, const char *fmt, ...);
int  exec_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);
int  exec_log_write(int prio, const char *tag, const char *text);
void exec_set_abort_message(const char *msg);
#endif
