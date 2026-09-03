/* config.c -- see config.h. MIT licensed. */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "config.h"
#include "exec_log.h"
#include "exec_input.h"

exec_config config = {
  .stick_speed    = 14.0f,
  .mouse_sens     = 1.0f,
  .cursor_docked  = -1,
  .cursor_handheld = -1,
  /* BOTH, not STICK. Gyro was off by default and the only way to discover
   * that was to read this file, so "the gyro does not work" was the expected
   * outcome rather than a bug. */
  .cursor_stick   = 0,          /* left stick moves the cursor */

  /* SWIPE_FLICK: flick the right stick and the port draws the swipe for you.
   * The alternative, SWIPE_DRAG, is "hold A and move the cursor", which is
   * what a touchscreen drag literally is and needs no synthesis at all --
   * see exec_input.c. Flick is the default because the game grades stunt
   * swipes on angle AND timing, and a stick flick sets both in one motion
   * where a hand-drawn drag sets neither reliably. */
  .swipe_mode     = 0,          /* SWIPE_FLICK */
  .swipe_stick    = 1,          /* right stick flicks */
  .swipe_len      = 420,        /* px at 1080p; about a third of the height */
  .swipe_frames   = 5,
  .swipe_deadzone = 65,         /* percent -- a flick, not a nudge */

  .accel_mode     = 0,          /* ACCEL_STATIC */
  .accel_invert   = 0,
  .accel_sens     = 100,
  .language       = "auto",
};

/* ---- logging ------------------------------------------------------------- */

/* The reused files call log_write; exec_log.c owns the file. Routing one into
 * the other keeps a single log with a single flush policy rather than two
 * files that interleave badly. */
void log_init(const char *path) { exec_log_open(path); }
void log_close(void)            { exec_log_close(); }

void log_write(char level, const char *fmt, ...) {
  int prio;
  switch (level) {
    case 'E': case 'e': prio = EXEC_LOG_ERROR;   break;
    case 'W': case 'w': prio = EXEC_LOG_WARN;    break;
    case 'D': case 'd': prio = EXEC_LOG_DEBUG;   break;
    case 'V': case 'v': prio = EXEC_LOG_VERBOSE; break;
    default:            prio = EXEC_LOG_INFO;    break;
  }
  va_list ap;
  va_start(ap, fmt);
  exec_log_vprint(prio, "executive_nx", fmt, ap);
  va_end(ap);
}

/* ---- settings ------------------------------------------------------------ */

#define MAX_ENV 32
static struct { char k[48], v[64]; } g_env[MAX_ENV];
static int  g_env_n;
static char g_path[600];

static const char *DEFAULT_FILE =
"# executive_nx settings. Lines are `key value`; # starts a comment.\n"
"# Unknown keys are kept, so a file from an older build is safe to reuse.\n"
"\n"
"stick_speed      14     # cursor pixels per frame at full stick deflection\n"
"mouse_sens       1.0    # USB mouse multiplier\n"
"cursor_docked    -1     # -1 auto, 0 hide, 1 show\n"
"cursor_handheld  -1     # -1 auto (off: the touchscreen is the pointer)\n"
"\n"
"cursor_stick     0      # which stick moves the cursor: 0 left, 1 right\n"
"\n"
"# Swipes. The game asks for them constantly, so this is the setting worth\n"
"# experimenting with first.\n"
"#\n"
"#   0  FLICK -- flick swipe_stick and the port performs the swipe for you,\n"
"#      starting at the cursor and running swipe_len pixels in the direction\n"
"#      you flicked. One motion sets both the angle and the moment, which is\n"
"#      what the stunt grader scores.\n"
"#   1  DRAG  -- no synthesis: hold A and move the cursor, which is exactly\n"
"#      what a finger on the screen does. Slower and fully manual.\n"
"#\n"
"# Both modes always leave tap (A) and touch-and-hold (hold A) working, which\n"
"# is how you strike and how you block.\n"
"swipe_mode       0      # 0 flick, 1 drag\n"
"swipe_stick      1      # which stick flicks: 0 left, 1 right\n"
"                        # Keep it different from cursor_stick.\n"
"swipe_len        420    # swipe length in pixels at 1080p\n"
"swipe_frames     5      # frames to spread the swipe over; raise it if the\n"
"                        # game reads your swipes as taps\n"
"swipe_deadzone   65     # percent of full deflection that counts as a flick\n"
"\n"
"# The accelerometer. Nothing in The Executive is known to read it -- the\n"
"# engine's tiltMagnitude() has no callers in this binary -- so by default\n"
"# this reports a console held still and both sticks stay free. Set 1 if you\n"
"# want to find out; PVS_DEBUG_ACCEL 1 makes the engine log what it received.\n"
"accel_mode       0      # 0 static (0,1,0)g, 1 driven by the console gyro\n"
"accel_invert     0\n"
"accel_sens       100    # percent, gyro mode only\n"
"\n"
"language         auto   # auto, or one of: en fr de it es\n"
"                        # Those are the only five the game ships.\n";

static void trim(char *s) {
  char *p = s + strlen(s);
  while (p > s && isspace((unsigned char)p[-1])) *--p = 0;
}

static void apply(const char *k, const char *v) {
  if (!strncmp(k, "PVS_", 4)) {
    if (g_env_n < MAX_ENV) {
      snprintf(g_env[g_env_n].k, sizeof(g_env[0].k), "%s", k);
      snprintf(g_env[g_env_n].v, sizeof(g_env[0].v), "%s", v);
      g_env_n++;
    }
    return;
  }
#define F(name) if (!strcmp(k, #name)) { config.name = strtof(v, NULL); return; }
#define I(name) if (!strcmp(k, #name)) { config.name = (int)strtol(v, NULL, 10); return; }
  F(stick_speed) F(mouse_sens)
  I(cursor_docked) I(cursor_handheld)
  I(cursor_stick)
  I(swipe_mode) I(swipe_stick) I(swipe_len) I(swipe_frames) I(swipe_deadzone)
  I(accel_mode) I(accel_invert) I(accel_sens)
#undef F
#undef I
  if (!strcmp(k, "language")) {
    snprintf(config.language, sizeof(config.language), "%s", v);
    return;
  }
  exec_log(EXEC_LOG_WARN, "config: ignoring unknown key '%s'", k);
}

void exec_config_load(const char *dir) {
  snprintf(g_path, sizeof(g_path), "%s/config.txt", dir);

  FILE *f = fopen(g_path, "r");
  if (!f) {
    f = fopen(g_path, "w");
    if (f) { fputs(DEFAULT_FILE, f); fclose(f); }
    /* Defaults are already in `config`; nothing more to read. */
    return;
  }

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *h = strchr(line, '#');
    if (h) *h = 0;
    trim(line);
    char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) continue;
    char *k = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (!*p) continue;
    *p++ = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) continue;
    apply(k, p);
  }
  fclose(f);

  /* Additive upgrade: append any key the running build knows about that the
   * file does not mention, so an old config.txt gains new options without
   * losing the values already in it. Rewriting the file wholesale would
   * discard the user's comments, which people do edit. */
  f = fopen(g_path, "r");
  if (!f) return;
  static char whole[8192];
  size_t n = fread(whole, 1, sizeof(whole) - 1, f);
  whole[n] = 0;
  fclose(f);

  /* ONE TABLE, NOT TWO LISTS.
   *
   * This used to be a names array beside an if/else chain that turned each
   * name back into its value, and the two could drift: adding a key to the
   * array and forgetting the chain appends the key with an EMPTY value, which
   * parses as "use the default" and looks exactly like a deliberate blank.
   * A row per setting cannot drift, because there is only one list. */
  enum { K_INT, K_FLOAT0, K_FLOAT2, K_STR };
  static const struct { const char *name; int kind; size_t off; } keys[] = {
    { "stick_speed",     K_FLOAT0, offsetof(exec_config, stick_speed)     },
    { "mouse_sens",      K_FLOAT2, offsetof(exec_config, mouse_sens)      },
    { "cursor_docked",   K_INT,    offsetof(exec_config, cursor_docked)   },
    { "cursor_handheld", K_INT,    offsetof(exec_config, cursor_handheld) },
    { "cursor_stick",    K_INT,    offsetof(exec_config, cursor_stick)    },
    { "swipe_mode",      K_INT,    offsetof(exec_config, swipe_mode)      },
    { "swipe_stick",     K_INT,    offsetof(exec_config, swipe_stick)     },
    { "swipe_len",       K_INT,    offsetof(exec_config, swipe_len)       },
    { "swipe_frames",    K_INT,    offsetof(exec_config, swipe_frames)    },
    { "swipe_deadzone",  K_INT,    offsetof(exec_config, swipe_deadzone)  },
    { "accel_mode",      K_INT,    offsetof(exec_config, accel_mode)      },
    { "accel_invert",    K_INT,    offsetof(exec_config, accel_invert)    },
    { "accel_sens",      K_INT,    offsetof(exec_config, accel_sens)      },
    { "language",        K_STR,    offsetof(exec_config, language)        },
  };
  FILE *app = NULL;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (strstr(whole, keys[i].name)) continue;
    if (!app) {
      app = fopen(g_path, "a");
      if (!app) return;
      fputs("\n# --- added by a newer executive_nx ---\n", app);
    }
    /* Write the value, not a bare key. A bare key parses fine -- the reader
     * skips a line with no value and the default applies -- but it tells the
     * reader of the file nothing, and an appended `swipe_len` with no number
     * next to it looks like a mistake rather than a new option. */
    const void *field = (const char *)&config + keys[i].off;
    char val[32];
    switch (keys[i].kind) {
      case K_INT:    snprintf(val, sizeof(val), "%d", *(const int *)field); break;
      case K_FLOAT0: snprintf(val, sizeof(val), "%.0f", (double)*(const float *)field); break;
      case K_FLOAT2: snprintf(val, sizeof(val), "%.2f", (double)*(const float *)field); break;
      default:       snprintf(val, sizeof(val), "%s", (const char *)field); break;
    }
    fprintf(app, "%-16s %s\n", keys[i].name, val);
  }
  if (app) fclose(app);
}

void exec_config_save(void) { /* settings are edited by hand; nothing to write */ }

void exec_config_apply_env(void (*set_env)(const char *k, const char *v)) {
  if (!set_env) return;
  for (int i = 0; i < g_env_n; i++) {
    set_env(g_env[i].k, g_env[i].v);
    exec_log(EXEC_LOG_INFO, "setenv %s=%s", g_env[i].k, g_env[i].v);
  }
}

/* MainActivity built this from LocaleList and always appended "en" last. The
 * engine walks the array in order and takes the first tag it has a
 * translation for, so the order is the whole contract. */
void exec_config_languages(const char *const **tags, int *count) {
  static const char *const supported[] = { "en", "fr", "de", "it", "es" };
  static const char *list[3];
  int n = 0;
  if (strcmp(config.language, "auto") != 0) {
    /* Reject a tag the game has no strings for rather than passing it
     * through. The engine would fall back to English anyway, so the only
     * thing an unsupported tag buys is a confusing config file. */
    int ok = 0;
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
      if (!strcmp(config.language, supported[i])) { ok = 1; break; }
    if (ok) list[n++] = config.language;
    else exec_log(EXEC_LOG_WARN, "config: language '%s' is not one of "
                               "en/fr/de/it/es; using the system setting",
                 config.language);
  }
  if (n == 0)
  {
    /* SetLanguage gives the console's own choice; mapping it to one of the
     * five the game ships is done in exec_input.c, where the libnx call
     * already is. */
    const char *sys = exec_system_language_tag();
    if (sys && *sys) list[n++] = sys;
  }
  list[n++] = "en";
  *tags = list;
  *count = n;
}
