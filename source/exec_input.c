/* exec_input.c -- Switch input onto the engine's pointer and key model.
 *
 * MIT licensed. See LICENSE. See exec_input.h for why the key codes are Mac
 * virtual key codes.
 *
 * ORDERING, AND WHY IT IS NOT ARBITRARY
 * -------------------------------------
 * MainActivity$PVSView.onTouchEvent does not call the natives. It calls
 * GLSurfaceView.queueEvent, so every pointer and key call ran on the render
 * thread, between frames, in the order the events arrived. queueNativeKey does
 * the same for keys. So this file is called once per frame from main.c before
 * nativeDrawFrame and dispatches immediately -- it does not hook libnx input
 * callbacks and it does not call the engine from anywhere else.
 *
 * THE PREVIOUS POSITION IS THE WHOLE JOB
 * --------------------------------------
 * nativePointerMove is (id, prevX, prevY, x, y), not (id, x, y). The Java side
 * kept a SparseArray of last positions per pointer id:
 *
 *     float[] last = lastPositions.get(id);
 *     float px = last != null ? last[0] : x;
 *     float py = last != null ? last[1] : y;
 *     lastPositions.put(id, new float[]{x, y});
 *
 * so on the first move after a down, prev == current. The engine takes the
 * delta from those four floats. Passing (x, y, x, y) makes every drag read as
 * zero motion, and the result looks like a physics bug rather than an input
 * bug, which is an expensive way to lose a day. `g_last` below is that
 * SparseArray.
 *
 * On ACTION_MOVE the Java side iterated EVERY pointer and emitted one move
 * each, not just the one that moved. emit_moves does the same.
 *
 * THE CONTROL MODEL: THIS IS A POINTER GAME
 * -----------------------------------------
 * The Executive is played with taps, holds and swipes. Its own strings say so
 * -- "Tap here to strike high", "Touch and hold here to defend low", "Swipe
 * from your chest to the enemy's chest" -- and so does KungController, whose
 * virtuals are getXY(), clickStatus(int) and blockClicker(). See exec_sensor.c
 * for why the accelerometer is NOT part of this, which is the largest
 * difference from the Pizza Vs. Skeletons port this file came from.
 *
 * Two of the three actions need no code here at all, because nx_pointer
 * already is a touchscreen: A (or ZL/ZR, or a mouse button) held down is a
 * finger down at the cursor, moving the cursor while held is a drag, and
 * letting go is a lift. Tap to strike and hold to block therefore work as
 * they are.
 *
 * The swipe is the one that does not, and it is the action the game asks for
 * most. Drawing a diagonal of the right length in the right number of frames
 * with a thumbstick, while a stunt timer runs, is a different skill from the
 * one the game is testing. So `swipe_mode 0` synthesises it: flick a stick,
 * and the port performs a proper press-drag-release along that direction,
 * starting at the cursor. `swipe_mode 1` turns the synthesis off for anyone
 * who would rather draw it by hand.
 */

#include <math.h>
#include <string.h>
#include <switch.h>

#include "exec_input.h"
#include "exec_sensor.h"
#include "exec_entry.h"
#include "exec_jni.h"
#include "nx_pointer.h"
#include "config.h"
#include "exec_paths.h"
#include "exec_io.h"
#include "exec_log.h"

#define MAX_POINTERS 10          /* 8 touch slots + cursor(8) + swipe(9). A
                                  * USB mouse has no id of its own; it drives
                                  * nx_pointer's cursor. */

static PadState g_pad;
static int      g_w = 1920, g_h = 1080;

/* The lastPositions SparseArray. `active` doubles as "is this id down". */
static struct { int active; float x, y; } g_last[MAX_POINTERS];

static void down(int id, float x, float y) {
  if (id < 0 || id >= MAX_POINTERS) return;
  g_last[id].active = 1;
  g_last[id].x = x; g_last[id].y = y;
  exec.pointer_down(jni_env(), jni_activity_class(), id, x, y);
}

static void move(int id, float x, float y) {
  if (id < 0 || id >= MAX_POINTERS || !g_last[id].active) return;
  const float px = g_last[id].x, py = g_last[id].y;
  g_last[id].x = x; g_last[id].y = y;
  /* Skip a move that did not move. Android would not have generated one, and
   * the engine's drag handling treats a zero delta as a real sample. */
  if (px == x && py == y) return;
  exec.pointer_move(jni_env(), jni_activity_class(), id, px, py, x, y);
}

static void up(int id, float x, float y) {
  if (id < 0 || id >= MAX_POINTERS || !g_last[id].active) return;
  g_last[id].active = 0;
  exec.pointer_up(jni_env(), jni_activity_class(), id, x, y);
}

/* ---- swipes -------------------------------------------------------------- */

/* WHY A SYNTHETIC SWIPE IS SPREAD OVER FRAMES
 *
 * The engine grades a stunt on the angle AND the timing of the swipe, and it
 * reads both out of the stream of pointer_move deltas. A swipe delivered as
 * one down, one enormous move and one up in a single frame is a shape the
 * engine never saw on a phone: no finger crosses a third of a screen in 16 ms.
 * Whether that reads as a swipe, as a tap, or as nothing is not something
 * this port can determine without hardware -- so it is spread over
 * swipe_frames instead, which is the shape a real finger makes, and the count
 * is a setting rather than a constant precisely because it is unverified.
 *
 * It uses its own pointer id. Ids 0-7 are the touch slots and 8 is
 * nx_pointer's cursor; a synthetic swipe on 8 would fight the cursor's own
 * down/up bookkeeping, and the engine tracks pointers by id exactly as
 * MotionEvent did. */
#define SWIPE_ID 9

static struct {
  int   active;
  int   frame;          /* 0 .. total; 0 emits the down, total emits the up */
  int   total;
  float x0, y0, x1, y1;
  int   armed;          /* the stick was outside the deadzone last frame    */
} g_swipe;

/* SHORTEN THE SWIPE, DO NOT CLAMP ITS ENDPOINT.
 *
 * The obvious way to keep a swipe on screen is to clamp x1 and y1 to the
 * bounds. That is wrong here, and wrong in the way that matters: clamping the
 * two axes independently moves the far end sideways, which CHANGES THE ANGLE
 * -- and the angle is precisely what this game grades a stunt swipe on
 * ("match the angle of the arrows as closely as you can"). A swipe aimed at
 * 45 degrees from near the right edge would arrive at something closer to
 * vertical, so swipes would quietly score worse near the edges of the screen
 * than in the middle, which is a miserable thing to diagnose from the
 * symptoms.
 *
 * Scaling the whole vector by the largest factor that fits keeps the
 * direction exactly and only shortens the stroke. */
static float fit_len(float cx, float cy, float dx, float dy, float len) {
  const float m = 2.0f;
  float f = 1.0f;
  if (dx > 0.0f) { const float t = ((float)g_w - m - cx) / (dx * len); if (t < f) f = t; }
  if (dx < 0.0f) { const float t = (m - cx) / (dx * len);              if (t < f) f = t; }
  if (dy > 0.0f) { const float t = ((float)g_h - m - cy) / (dy * len); if (t < f) f = t; }
  if (dy < 0.0f) { const float t = (m - cy) / (dy * len);              if (t < f) f = t; }
  if (f < 0.0f) f = 0.0f;
  return len * f;
}

static void swipe_begin(float cx, float cy, float dx, float dy) {
  float len = (float)config.swipe_len * ((float)g_h / (float)EXEC_RENDER_H);
  len = fit_len(cx, cy, dx, dy, len);

  /* A swipe with nowhere to go is not sent at all. Emitting a zero-length
   * down/up pair would reach the engine as a TAP, and a tap is a strike --
   * so a flick into the edge of the screen would attack instead of doing
   * nothing, which is worse than the flick being ignored. */
  if (len < 8.0f) return;

  const float x1 = cx + dx * len, y1 = cy + dy * len;

  g_swipe.active = 1;
  g_swipe.frame  = 0;
  g_swipe.total  = config.swipe_frames > 1 ? config.swipe_frames : 2;
  g_swipe.x0 = cx; g_swipe.y0 = cy;
  g_swipe.x1 = x1; g_swipe.y1 = y1;
}

/* Advance an in-flight swipe by one frame. Called before the cursor's own
 * events are drained, so a swipe and a tap in the same frame arrive in the
 * order a MotionEvent stream would have had them. */
static void swipe_step(void) {
  if (!g_swipe.active) return;
  const float u = (float)g_swipe.frame / (float)g_swipe.total;
  const float x = g_swipe.x0 + (g_swipe.x1 - g_swipe.x0) * u;
  const float y = g_swipe.y0 + (g_swipe.y1 - g_swipe.y0) * u;

  if (g_swipe.frame == 0)                  down(SWIPE_ID, x, y);
  else if (g_swipe.frame < g_swipe.total)  move(SWIPE_ID, x, y);
  else { up(SWIPE_ID, g_swipe.x1, g_swipe.y1); g_swipe.active = 0; return; }
  g_swipe.frame++;
}

/* Watch the flick stick and start a swipe on the frame it crosses out of the
 * deadzone. Edge-triggered on purpose -- a held stick is one swipe, not sixty
 * -- and rearmed only once it returns inside, so the player has to flick
 * again. */
static void swipe_poll_stick(void) {
  if (config.swipe_mode != SWIPE_FLICK) return;
  if (g_swipe.active) return;

  const HidAnalogStickState st = padGetStickPos(&g_pad, config.swipe_stick);
  float dx = (float)st.x / 32767.0f;
  float dy = (float)st.y / 32767.0f;
  const float mag = sqrtf(dx * dx + dy * dy);

  /* A centred stick has no direction, and dividing by its magnitude below
   * would be 0/0 -- which is NaN, not zero, and NaN survives every bounds
   * check in fit_len (each comparison against it is false) to arrive at the
   * engine as a pointer_down at (NaN, NaN). The deadzone below normally makes
   * this unreachable, but config.txt is a text file a person edits, and
   * `swipe_deadzone 0` should give an oversensitive flick rather than a
   * corrupt one. */
  if (mag < 1e-4f) { g_swipe.armed = 0; return; }

  float arm = (float)config.swipe_deadzone / 100.0f;
  if (arm < 0.05f) arm = 0.05f;      /* below this a resting stick arms it */
  if (arm > 0.95f) arm = 0.95f;      /* above this the corners cannot reach */

  /* Rearm well below the arming threshold rather than at it, so a stick
   * resting near the edge does not chatter one swipe per frame. */
  if (mag < arm * 0.6f) { g_swipe.armed = 0; return; }
  if (mag < arm || g_swipe.armed) return;
  g_swipe.armed = 1;

  dx /= mag; dy /= mag;
  /* The stick's +y is up; screen +y is down. */
  dy = -dy;

  float cx = (float)g_w * 0.5f, cy = (float)g_h * 0.5f;
  nxp_cursor_pos(&cx, &cy);
  swipe_begin(cx, cy, dx, dy);
}

/* ---- keys ---------------------------------------------------------------- */

typedef struct { uint64_t mask; int key; } keybind;

/* B and + both map to Escape because MainActivity mapped BUTTON_B and BACK to
 * the same code.
 *
 * ZL and ZR are deliberately NOT bound. They used to be a second Return, so
 * the game could be played one-handed -- but they are now the cursor chord,
 * and a chord whose individual buttons also do something fires that something
 * on the way in. Pressing ZL then ZR would confirm before it toggled. */
static const keybind g_binds[] = {
  { HidNpadButton_A,      EXEC_KEY_RETURN },
  { HidNpadButton_B,      EXEC_KEY_ESCAPE },
  { HidNpadButton_Plus,   EXEC_KEY_ESCAPE },
  { HidNpadButton_Y,      EXEC_KEY_SPACE  },
  { HidNpadButton_Left,   EXEC_KEY_LEFT   },
  { HidNpadButton_Right,  EXEC_KEY_RIGHT  },
  { HidNpadButton_Up,     EXEC_KEY_UP     },
  { HidNpadButton_Down,   EXEC_KEY_DOWN   },
};
#define NBINDS ((int)(sizeof(g_binds) / sizeof(g_binds[0])))

/* Two buttons can map to one key. Reference-count per key so releasing one
 * while the other is still held does not send a spurious key-up -- otherwise
 * letting go of ZR while A is down cancels the press. */
static int g_key_refs[128];

static void key_edge(int key, int pressed) {
  if (key < 0 || key >= (int)(sizeof(g_key_refs) / sizeof(g_key_refs[0]))) return;
  if (pressed) {
    if (g_key_refs[key]++ == 0)
      exec.key_pressed(jni_env(), jni_activity_class(), key);
  } else {
    if (g_key_refs[key] > 0 && --g_key_refs[key] == 0)
      exec.key_released(jni_env(), jni_activity_class(), key);
  }
}

/* ---- language ------------------------------------------------------------ */

static char g_lang_tag[16];

/* EFIGS, and nothing else.
 *
 * The APK ships assets/data/strings/exec_strings_en.txt plus _de, _es, _fr
 * and _it -- five languages. The EFIGS artwork is named exec_efigs_strip, which
 * is the same statement in the studio's own shorthand.
 *
 * A console set to Japanese used to be handed "ja", which the engine has no
 * translation for; it falls back to English on its own, so nothing broke, but
 * the honest answer is to say English rather than to name a language that
 * does not exist. Regional variants collapse into their base tag, because
 * pt-BR and pt are the same nothing here. */
const char *exec_system_language_tag(void) {
  if (g_lang_tag[0]) return g_lang_tag;
  uint64_t code = 0;
  SetLanguage lang = SetLanguage_ENUS;
  /* setGetSystemLanguage lives on the `set:` service, which libnx does NOT
   * open by default -- __appInit only touches set:sys, and only to read the
   * firmware version. Calling it unopened returns an error, which this
   * function would quietly turn into "en" for every console on earth. */
  const int have_set = R_SUCCEEDED(setInitialize());
  if (have_set &&
      R_SUCCEEDED(setGetSystemLanguage(&code)) &&
      R_SUCCEEDED(setMakeLanguage(code, &lang))) {
    switch (lang) {
      case SetLanguage_FR: case SetLanguage_FRCA:   strcpy(g_lang_tag, "fr"); break;
      case SetLanguage_DE:                          strcpy(g_lang_tag, "de"); break;
      case SetLanguage_IT:                          strcpy(g_lang_tag, "it"); break;
      case SetLanguage_ES: case SetLanguage_ES419:  strcpy(g_lang_tag, "es"); break;
      default:                                      strcpy(g_lang_tag, "en"); break;
    }
  } else {
    strcpy(g_lang_tag, "en");
  }
  if (have_set) setExit();
  return g_lang_tag;
}

/* ---- lifecycle ----------------------------------------------------------- */

PadState *exec_pad(void) { return &g_pad; }

void exec_input_init(int surface_w, int surface_h) {
  g_w = surface_w; g_h = surface_h;

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();

  exec_sensor_init(&g_pad);

  NxpConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.screen_w        = surface_w;
  cfg.screen_h        = surface_h;
  cfg.data_dir        = exec_dir();
  cfg.cursor_id       = 8;           /* above the 8 touch slots */
  cfg.max_touch_slots = 8;
  /* MINUS toggles the cursor, and ZL/ZR are left alone.
   *
   * The Pizza Vs. Skeletons port put the cursor toggle on ZL+ZR as a chord,
   * because it needed Minus for tilt recalibration and could not use Plus
   * (which is Escape to the engine). That trade is wrong for this game.
   * nx_pointer treats A, ZL and ZR as tap-at-the-cursor, and tapping is how
   * The Executive is PLAYED -- ZL and ZR are a comfortable second and third
   * strike button and one-handed play. Spending them on a chord to toggle a
   * cursor that is on by default would cost a control that matters to save
   * one that does not.
   *
   * Minus is free here because the accelerometer no longer needs recentring
   * from a face button; that moved to L3, and in the default static mode it
   * has nothing to do anyway.
   *
   * Gyro POINTING is left on its nx_pointer default. Unlike the reference
   * port there is no second consumer of the sensor to fight it -- this port
   * does not steer with the gyro -- so it is simply another way to aim, which
   * suits a game played entirely with a pointer. */
  cfg.toggle_cursor_mask = HidNpadButton_Minus;
  /* R3, and NOT 0.
   *
   * 0 does not mean "no toggle" -- nx_pointer reads it as "use my default",
   * and its default gyro toggle is MINUS, which is the button on the line
   * above. Both chords are evaluated independently, so one press of Minus ran
   * both handlers: it hid the cursor, then the gyro branch turned pointing on
   * and un-hid the cursor again (`if (s_gyro_on && s_visible <= 0)
   * s_visible = 1`). The visible symptom is a cursor toggle that only works on
   * every second press, in a game whose entire control scheme is the cursor.
   *
   * NXP_TOGGLE_OFF is the way to disable a toggle. R3 is used instead of
   * disabling it because gyro pointing is genuinely useful here -- unlike the
   * reference port, nothing else consumes the sensor -- and R3 is the only
   * button this port has not already spent. */
  cfg.toggle_gyro_mask   = HidNpadButton_StickR;
  cfg.cursor_stick    = config.cursor_stick;
  cfg.stick_speed     = config.stick_speed;
  cfg.mouse_sens      = config.mouse_sens;
  /* nx_pointer reads cursor.png and writes pointer.cfg. Our audio decode
   * thread is live by now, so its file access has to take the same lock
   * everything else does -- devkitPro's handle table is not thread-safe. */
  cfg.fopen_fn        = fopen_locked;
  cfg.fclose_fn       = fclose_locked;
  nxp_init(&cfg);

  exec_log(EXEC_LOG_INFO,
          "input ready: %dx%d, swipe=%s (stick %d, %d px over %d frames), "
          "cursor stick %d, accel=%s, lang=%s",
          g_w, g_h,
          config.swipe_mode == SWIPE_FLICK ? "flick" : "drag",
          config.swipe_stick, config.swipe_len, config.swipe_frames,
          config.cursor_stick,
          config.accel_mode == ACCEL_GYRO ? "gyro" : "static",
          exec_system_language_tag());
}

void exec_input_set_surface(int w, int h) {
  if (w <= 0 || h <= 0) return;
  g_w = w; g_h = h;
  nxp_set_screen(w, h);
  exec_log(EXEC_LOG_INFO, "pointer surface corrected to %dx%d", w, h);
}

void exec_input_exit(void) {
  /* Release anything still held so the engine does not keep a key latched
   * across a return to the home menu. */
  for (int k = 0; k < (int)(sizeof(g_key_refs) / sizeof(g_key_refs[0])); k++)
    while (g_key_refs[k] > 0) key_edge(k, 0);
  g_swipe.active = 0;
  for (int i = 0; i < MAX_POINTERS; i++)
    if (g_last[i].active) up(i, g_last[i].x, g_last[i].y);

  nxp_save_settings();
  exec_sensor_exit();
}

void exec_input_poll(void) {
  padUpdate(&g_pad);

  const uint64_t pressed  = padGetButtonsDown(&g_pad);
  const uint64_t released = padGetButtonsUp(&g_pad);

  /* L3 recentres the accelerometer's neutral pose. It does nothing in the
   * default ACCEL_STATIC mode, which has no pose -- that is deliberate rather
   * than an oversight, and it is why the binding is L3 and not a button
   * anybody reaches for by accident. */
  if (pressed & HidNpadButton_StickL) exec_sensor_recalibrate();

  for (int i = 0; i < NBINDS; i++) {
    if (pressed  & g_binds[i].mask) key_edge(g_binds[i].key, 1);
    if (released & g_binds[i].mask) key_edge(g_binds[i].key, 0);
  }

  /* The synthetic swipe advances BEFORE the cursor's own events are drained.
   * Both end up in the same per-frame batch, and this is the order a real
   * MotionEvent stream would have had them in: the finger that went down
   * first reports first. */
  swipe_poll_stick();
  swipe_step();

  /* nx_pointer merges touch, stick cursor, gyro pointing and USB mouse into
   * one stream of down/move/up with stable ids, which is exactly the shape
   * MotionEvent had. Its NXP_* phases are then replayed through the
   * lastPositions bookkeeping so the previous position is right. */
  nxp_update();

  NxpEvent ev[32];
  const int n = nxp_poll(ev, 32);
  for (int i = 0; i < n; i++) {
    switch (ev[i].phase) {
      case NXP_DOWN: down(ev[i].id, ev[i].x, ev[i].y); break;
      case NXP_MOVE: move(ev[i].id, ev[i].x, ev[i].y); break;
      case NXP_UP:   up  (ev[i].id, ev[i].x, ev[i].y); break;
      default: break;
    }
  }

  /* The Java side registered a SensorEventListener and forwarded every sample.
   * One per frame is the same rate the engine saw at SENSOR_DELAY_GAME. */
  exec_sensor_update();
  float ax, ay, az;
  exec_sensor_get(&ax, &ay, &az);
  exec.accel_changed(jni_env(), jni_activity_class(), ax, ay, az);
}
