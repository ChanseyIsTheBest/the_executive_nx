/* config.h -- settings, and the logging surface the reused files expect.
 *
 * MIT licensed. See LICENSE.
 *
 * Two unrelated things live behind one name because libc_shim.c, so_util.c,
 * error.c, compat_stubs.c and imports_helpers.c are reused unmodified from the
 * reference ports and all of them `#include "config.h"` for log_write. Keeping
 * that include satisfied is what lets upstream fixes be pulled back.
 *
 * The settings half is this port's own.
 */
#ifndef EXEC_CONFIG_H
#define EXEC_CONFIG_H

#include <stddef.h>

/* exec_diag.h defines this too; both guard it, and both must agree.
 * OFF by default: a shipping build should not carry per-wait instrumentation.
 * `make DEBUG=1` turns it on. */
#ifndef EXEC_DIAG
#define EXEC_DIAG 0
#endif

/* ---- logging (used by the reused files) ---------------------------------- */

void log_init(const char *path);
void log_close(void);
void log_write(char level, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));

/* compat_stubs.c, libc_shim.c and so_util.c call these by name -- 24 sites
 * between them. They are macros rather than functions so the compiled-out
 * levels cost nothing, including evaluating their arguments.
 *
 * LOGW/LOGE always compile in: they are rare, and a silent failure is the
 * worst possible outcome. LOGB is the boot summary. LOGI is chatty enough to
 * be a per-frame cost, so it follows EXEC_DIAG. */
#if defined(EXEC_LOG_SILENT) && EXEC_LOG_SILENT
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#else
#define LOGW(...) log_write('W', __VA_ARGS__)
#define LOGE(...) log_write('E', __VA_ARGS__)
#endif

/* OFF by default. See exec_log.h for what a release build still records and
 * why it is not nothing. `make DEBUG=1` turns it on. */
#ifndef DEBUG_LOG
#define DEBUG_LOG 0
#endif

#if DEBUG_LOG
#define LOGB(...) log_write('I', __VA_ARGS__)
#else
#define LOGB(...) ((void)0)
#endif

#if DEBUG_LOG && EXEC_DIAG
#define LOGI(...) log_write('I', __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif

/* ---- settings ------------------------------------------------------------ */

/* Fixed, not configurable. See main.c for why 1080p in both modes.
 * Exposed as constants rather than settings because there is one right
 * answer here and a wrong one costs a confusing bug report. */
#define EXEC_RENDER_W 1920
#define EXEC_RENDER_H 1080

typedef struct {
  /* Pointer */
  float stick_speed;        /* cursor px/frame at full deflection            */
  float mouse_sens;
  int   cursor_docked;      /* -1 auto, 0 off, 1 on                          */
  int   cursor_handheld;

  int   cursor_stick;       /* 0 left, 1 right                               */

  /* Swipes. The game asks for them constantly -- stunts, strong kicks,
   * flame kicks, silverstrikes -- so this is the control that matters most
   * on a pad. See exec_input.c for what each mode does. */
  int   swipe_mode;         /* SWIPE_FLICK / SWIPE_DRAG                      */
  int   swipe_stick;        /* which stick flicks: 0 left, 1 right           */
  int   swipe_len;          /* swipe length in pixels at 1080p               */
  int   swipe_frames;       /* frames the synthetic swipe is spread over     */
  int   swipe_deadzone;     /* percent of full deflection that arms a flick  */

  /* Accelerometer, for nativeAccelerometerChanged. Nothing in this game is
   * known to read it; see exec_sensor.c for how that was established. */
  int   accel_mode;         /* ACCEL_STATIC / ACCEL_GYRO                     */
  int   accel_invert;
  int   accel_sens;         /* percent, gyro mode only                       */

  /* Language: "auto", or a tag the engine understands. */
  char  language[16];
} exec_config;

extern exec_config config;

/* Reads <dir>/config.txt, writing a documented default file if absent and
 * adding any newly-introduced key to an older file while keeping its values. */
void exec_config_load(const char *dir);
void exec_config_save(void);

/* The tag list handed back from MainActivity.preferredLanguageTags().
 *
 * The game ships five languages and no more -- assets/data/strings holds
 * exec_strings_en.txt plus _de, _es, _fr and _it, and the EFIGS artwork is
 * literally named exec_efigs_strip. Offering anything else would produce a
 * tag the engine has no translation for, which it answers by falling back to
 * English anyway; the list is restricted so the menu does not advertise
 * languages that do not exist.
 *
 * Always ends with "en", exactly as the Java did. */
void exec_config_languages(const char *const **tags, int *count);

/* THE ENGINE'S OWN INSTRUMENTATION
 * -------------------------------
 * MainActivity.applyNativeEnvironmentExtras reads 15 Intent extras and passes
 * each to nativeSetEnv, which is a literal setenv(). The engine reads them
 * itself, so turning any of these on costs nothing to implement and is almost
 * always a better first move than adding a printf. The exact 15, in the order
 * the dex lists them:
 *
 *   PVS_AUTOMATION_LEVEL   PVS_AUTOMATION_MAP     PVS_AUTOMATION_INSTRUCTIONS
 *   PVS_AUTOMATION_OPEN_PAUSE                     PVS_AUTOMATION_GAMECENTER
 *   PVS_AUTOMATION_DUMP_STATE                     PVS_AUTOMATION_CLOUD
 *   PVS_DEBUG_INPUT        PVS_DEBUG_GAMECENTER   PVS_DEBUG_EXTENDER
 *   PVS_DEBUG_AUDIO        PVS_DEBUG_ACCEL        PVS_DEBUG_FRAME_TIMING
 *   PVS_DEBUG_LANGUAGE     PVS_DEBUG_DWARP
 *
 * THE PREFIX IS PVS_, NOT EXEC_, AND THAT IS NOT A TYPO. Riverman kept Pizza
 * Vs. Skeletons' names when they reused the engine, and these are the game's
 * own strings -- renaming them here would silently disable every one of them.
 * Any config.txt line whose key starts with PVS_ is passed straight through,
 * so no new key needs code here when the next one is discovered. */
void exec_config_apply_env(void (*set_env)(const char *k, const char *v));

#endif
