/* exec_sensor.c -- what this port feeds nativeAccelerometerChanged.
 *
 * MIT licensed. See LICENSE.
 * The six-axis reading below descends from sonicjump_nx/source/sj_sensor.c by
 * way of the Pizza Vs. Skeletons port; the conclusion it is put to is this
 * game's own and is the opposite of that port's.
 *
 * WHY THIS FILE IS SHORT, AND WHY THE PIZZA VS. SKELETONS ONE WAS NOT
 * -------------------------------------------------------------------
 * That port made tilt the primary control and spent four rounds of bring-up
 * on it: units, stick range, gyro frame, handle selection, sign. All of that
 * was correct work, and none of it applies here, because The Executive does
 * not steer with the accelerometer. Three independent readings of the binary
 * say so:
 *
 *   1. KungController's vtable is
 *          [4] getXY()   [5] clickStatus(int)   [6] blockClicker()
 *      A pointer controller. tiltMagnitude() is a NON-virtual member of the
 *      same class and appears in no vtable.
 *
 *   2. tiltMagnitude() has zero callers -- no `bl` to it and no data
 *      reference anywhere in the image. It is compiled in because it is an
 *      exported member of a class shared with the studio's other titles, and
 *      it is never reached.
 *
 *   3. The game's own strings describe the controls, and they are all touch:
 *      "Tap here to strike high", "Touch and hold here to defend low",
 *      "Swipe from your chest to the enemy's chest", and of stunts:
 *      "you can swipe anywhere on the screen. Just try to match the angle and
 *      timing of the stunt icon."
 *
 *      Pizza Vs. Skeletons had PlanePlayer::updateTilting, tiltMagnitudeVert
 *      and a tutorial asset called intro_tilttoroll. This binary has none of
 *      the three -- checked by name against its .dynsym and its strings.
 *
 * So driving the accelerometer from a stick here would spend a stick to move
 * a number nothing reads, and the sticks are needed for the cursor and the
 * swipe. The default is therefore to report a console sitting still.
 *
 * WHAT "SITTING STILL" IS, IN THE ENGINE'S UNITS
 * ---------------------------------------------
 * Units of g, not m/s^2, and already rotated into screen space. Both facts
 * are from MainActivity.onSensorChanged in this game's own classes.dex --
 * the native entry point passes its three floats to AccelDataStd::accel_change
 * with no conversion, so the Java caller decides the convention entirely:
 *
 *     float ax = values[0] / 9.80665f;    // the dex literal is 1092413450,
 *     float ay = values[1] / 9.80665f;    // which is the bit pattern of
 *     float az = values[2] / 9.80665f;    // 9.80665f
 *     switch (getWindowManager().getDefaultDisplay().getRotation()) {
 *       case ROTATION_90:  outX =  ay; outY = -ax; break;
 *       case ROTATION_270: outX = -ay; outY =  ax; break;
 *       case ROTATION_180: outX = -ax; outY = -ay; break;
 *       default:           outX =  ax; outY =  ay; break;
 *     }
 *     nativeAccelerometerChanged(outX, outY, az);
 *
 * A normalised gravity vector in screen space, so a device held upright and
 * still reads about (0, 1, 0). That is the constant below. It is a plausible
 * resting pose rather than a measured requirement, because there is nothing
 * in this game measured to require anything -- but a plausible resting pose
 * is strictly better than zeroes, which is a device in free fall, and better
 * than not calling the entry point at all, which leaves AccelDataStd holding
 * whatever it was constructed with.
 *
 * WHY THE GYRO PATH IS STILL HERE
 * -------------------------------
 * "Nothing is known to read it" is not "nothing reads it". The tracing above
 * is static, and a virtual call through a pointer this port cannot see would
 * not show up in it. If something in the game does turn out to respond to
 * tilt, `accel_mode 1` in config.txt makes the console's own IMU drive it and
 * PVS_DEBUG_ACCEL makes the engine print what it received -- so finding out
 * costs a config line and a run, not a rebuild.
 *
 * The clamp, if it ever matters: KungController::tiltMagnitude() clamps
 * accel.x - baseline to +/- 0.25 g and multiplies by 4, so full deflection is
 * a quarter of a g -- a phone tilted asin(0.25) = 14.5 degrees. MAX_TILT_DEG
 * below is that angle, so the end of the range is the end of the game's range.
 * (Pizza Vs. Skeletons used 35 degrees at first, which saturated the control
 * at 41% of its travel.)
 */

#include <math.h>
#include <string.h>
#include <switch.h>

#include "exec_sensor.h"
#include "config.h"
#include "exec_log.h"

/* The engine's unit is g, so a resting device is 1.0 and not 9.80665. */
#define GRAVITY       1.0f
#define MAX_TILT_DEG  14.4775f      /* asin(0.25) in degrees */

static float g_x = 0.0f, g_y = GRAVITY, g_z = 0.0f;   /* at rest: (0, 1, 0) */
static int   g_calibrated;
static float g_ref_x;

static PadState              *g_pad;
static HidSixAxisSensorHandle g_six[4];
static int                    g_six_ready;

static int gyro_mode(void) { return config.accel_mode == ACCEL_GYRO; }

void exec_sensor_init(PadState *pad) {
  g_pad = pad;

  if (!gyro_mode()) {
    exec_log(EXEC_LOG_INFO,
            "accel: static (0, %.1f, 0) g -- nothing in this game is known to "
            "read the accelerometer; accel_mode 1 drives it from the gyro",
            (double)GRAVITY);
    return;
  }

  /* HANDLE LAYOUT -- read_sixaxis indexes this, so the pairing matters:
   *   [0]      Handheld            (one handle)
   *   [1] [2]  JoyDual             (two: [1] left Joy-Con, [2] right)
   *   [3]      FullKey / Pro       (one handle)
   * hidGetSixAxisSensorHandles writes `count` consecutive entries, which is
   * why the dual request occupies 1 and 2 and FullKey lands at 3. */
  int got = 0;
  hidGetSixAxisSensorHandles(&g_six[0], 1, HidNpadIdType_Handheld,
                             HidNpadStyleTag_NpadHandheld);
  hidGetSixAxisSensorHandles(&g_six[1], 2, HidNpadIdType_No1,
                             HidNpadStyleTag_NpadJoyDual);
  hidGetSixAxisSensorHandles(&g_six[3], 1, HidNpadIdType_No1,
                             HidNpadStyleTag_NpadFullKey);
  for (int i = 0; i < 4; i++)
    if (R_SUCCEEDED(hidStartSixAxisSensor(g_six[i]))) got++;
  g_six_ready = 1;

  /* Say whether the gyro is actually there. Silence here cost the reference
   * port a whole test cycle spent wondering why a mode did nothing, when the
   * answer could as easily have been "no handle started". */
  exec_log(EXEC_LOG_INFO, "accel: gyro mode, %d of 4 six-axis handles started",
          got);
}

void exec_sensor_exit(void) {
  if (!g_six_ready) return;
  for (int i = 0; i < 4; i++) hidStopSixAxisSensor(g_six[i]);
  g_six_ready = 0;
}

/* PICK THE HANDLE BY STYLE SET, NOT BY WHICHEVER ANSWERS FIRST.
 *
 * Walking the four handles and taking the first that returns a state works in
 * handheld and silently fails everywhere else: handle 0 answers even with the
 * Joy-Cons detached, with the console's own reading, so the loop never
 * reaches the Joy-Con handles. "The Joy-Con gyro does not work" was literally
 * true in the reference port -- it was never being read.
 *
 * A JOY-CON'S IMU IS MIRRORED RELATIVE TO A PRO CONTROLLER'S: it sits rotated
 * 180 degrees about its pointing axis, so x and y both come out negated.
 * Negating both is a rotation; negating x alone would be a reflection, which
 * is not something a physical sensor mounting can be. */
static int read_sixaxis(HidSixAxisSensorState *out, float *sign) {
  if (!g_six_ready) return 0;
  *sign = -1.0f;                   /* Joy-Con / handheld frame */

  const u32 style = padGetStyleSet(g_pad);
  int idx = -1;

  if (style & HidNpadStyleTag_NpadFullKey) {
    idx = 3;                       /* Pro Controller */
    *sign = +1.0f;
  } else if (style & HidNpadStyleTag_NpadHandheld) {
    idx = 0;
  } else if (style & HidNpadStyleTag_NpadJoyDual) {
    const u32 attr = padGetAttributes(g_pad);
    if      (attr & HidNpadAttribute_IsRightConnected) idx = 2;
    else if (attr & HidNpadAttribute_IsLeftConnected)  idx = 1;
    else                                               idx = 1;
  }
  if (idx < 0) return 0;

  HidSixAxisSensorState st;
  if (hidGetSixAxisSensorStates(g_six[idx], &st, 1) <= 0) return 0;
  *out = st;

  static int told[4];
  if (idx < 4 && !told[idx]) {
    told[idx] = 1;
    exec_log(EXEC_LOG_INFO, "accel: six-axis from handle %d (style 0x%x), %s frame",
            idx, (unsigned)style, *sign < 0 ? "Joy-Con" : "Pro");
  }
  return 1;
}

void exec_sensor_recalibrate(void) { g_calibrated = 0; }

void exec_sensor_update(void) {
  if (!gyro_mode()) return;        /* the constant is already in g_x/g_y/g_z */

  /* If the gyro was asked for and nothing has ever arrived, say so once.
   * Two seconds is long enough that a slow HID start is not a failure, and
   * "unavailable in this controller mode" and "wrong axis mapping" need
   * completely different fixes -- so they must not look the same in a log. */
  static unsigned quiet;
  if (quiet != ~0u && ++quiet == 120) {
    HidSixAxisSensorState probe; float psign;
    if (!read_sixaxis(&probe, &psign)) {
      exec_log(EXEC_LOG_WARN, "accel: no six-axis data after 2 s -- the gyro is "
                            "unavailable in this controller mode");
      quiet = ~0u;
    }
  }

  HidSixAxisSensorState six; float sign = -1.0f;
  memset(&six, 0, sizeof(six));
  if (!read_sixaxis(&six, &sign)) return;   /* keep the last good vector */

  /* libnx already reports acceleration in g, which is the unit the engine
   * wants, so these are used as-is rather than scaled.
   *
   * The axes do not line up with a phone's: a Joy-Con's +y runs along the
   * rail towards the shoulder buttons, which is the phone's -z, and +z points
   * out of the face, which is the phone's +y. */
  const float ax = six.acceleration.x * sign;
  const float ay = six.acceleration.y * sign;
  const float az = six.acceleration.z;

  float gx = -ax;      /* across the screen                        */
  float gy =  az;      /* out of the face, rotation-invariant      */
  float gz =  ay;      /* up the screen                            */

  /* Calibrate to however the controller is being held. A phone tilt game
   * assumes a neutral pose of "flat, facing you"; nobody holds a Joy-Con like
   * that, and held naturally it sits at twenty or thirty degrees, which as a
   * raw gravity vector is a large constant bias. */
  if (!g_calibrated) {
    g_ref_x = gx;
    g_calibrated = 1;
    exec_log(EXEC_LOG_INFO, "accel: calibrated, this pose is neutral (ref x = %.3f g)",
            (double)g_ref_x);
  }
  gx -= g_ref_x;

  if (config.accel_invert) gx = -gx;

  /* Scale, then clamp to the engine's own limit. The clamp is +/- 0.25 g in
   * tiltMagnitude(), so reporting more than that is reporting a lean no phone
   * player could have produced. */
  gx *= (float)config.accel_sens / 100.0f;
  const float lim = sinf(MAX_TILT_DEG * (float)M_PI / 180.0f);   /* 0.25 */
  if (gx >  lim) gx =  lim;
  if (gx < -lim) gx = -lim;

  g_x = gx; g_y = gy; g_z = gz;
}

void exec_sensor_get(float *x, float *y, float *z) {
  if (x) *x = g_x;
  if (y) *y = g_y;
  if (z) *z = g_z;
}
