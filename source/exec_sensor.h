/* exec_sensor.h -- what to feed nativeAccelerometerChanged.
 *
 * MIT licensed. See exec_sensor.c for the measurements behind the default.
 *
 * The short version: The Executive is a pointer game and nothing in it is
 * known to read the accelerometer, so by default this reports a console held
 * still and upright. The live-gyro path is kept because "nothing is known to
 * read it" is not the same as "nothing reads it", and finding out should cost
 * a config line rather than a rebuild.
 */
#ifndef EXEC_SENSOR_H
#define EXEC_SENSOR_H
#include <switch.h>

/* config.accel_mode */
#define ACCEL_STATIC 0    /* a resting console: (0, 1, 0) g. The default.   */
#define ACCEL_GYRO   1    /* derive a tilt vector from the console's own IMU */

void exec_sensor_init(PadState *pad);
void exec_sensor_exit(void);
void exec_sensor_update(void);                      /* once per frame */
void exec_sensor_get(float *x, float *y, float *z); /* units of g, screen space */

/* Treat the controller's current pose as neutral. Only meaningful in
 * ACCEL_GYRO; a no-op in ACCEL_STATIC, which has no pose to speak of. */
void exec_sensor_recalibrate(void);
#endif
