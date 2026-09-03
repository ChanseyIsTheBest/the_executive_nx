/* exec_input.h -- Switch input mapped onto the engine's key and pointer model.
 *
 * THE KEY CODES ARE macOS VIRTUAL KEY CODES
 * -----------------------------------------
 * MainActivity.nativeKeyForAndroidKey translates Android key codes into what
 * it passes to nativeKeyPressed, and the numbers it produces are not Android's:
 *
 *   BACK / ESCAPE / BUTTON_B                    -> 53   kVK_Escape
 *   DPAD_CENTER / ENTER / NUMPAD_ENTER / BUTTON_A -> 36 kVK_Return
 *   SPACE                                       -> 49   kVK_Space
 *   DPAD_LEFT / RIGHT / DOWN / UP               -> 123 124 125 126
 *
 * Those are Carbon virtual key codes, which fits: the engine is Riverman
 * Media's cross-platform C++ runtime and its Mac build's key constants leaked
 * into every other port. So the mapping below sends Mac key codes, and any
 * new binding should be looked up in <HIToolbox/Events.h> rather than guessed.
 *
 * Everything else the game does is pointer input, and the pointer is in
 * surface pixels with the origin at the top left.
 */
#ifndef EXEC_INPUT_H
#define EXEC_INPUT_H

#define EXEC_KEY_RETURN  36
#define EXEC_KEY_SPACE   49
#define EXEC_KEY_ESCAPE  53
#define EXEC_KEY_LEFT   123
#define EXEC_KEY_RIGHT  124
#define EXEC_KEY_DOWN   125
#define EXEC_KEY_UP     126

#include <switch.h>

/* config.swipe_mode. See exec_input.c for what each one does. */
#define SWIPE_FLICK 0   /* flick a stick; the port performs the swipe */
#define SWIPE_DRAG  1   /* no synthesis: hold A and move the cursor   */

void exec_input_init(int surface_w, int surface_h);
void exec_input_exit(void);

/* Correct the surface size after EGL reports what it actually gave us. The
 * pointer maps stick and touch into these coordinates, so a stale value puts
 * the cursor in the wrong place by the ratio of the two sizes. */
void exec_input_set_surface(int w, int h);

/* Called once per frame, before draw_frame. Reads pads, touch and mouse and
 * emits pointer/key events in the order the Java side would have queued them. */
void exec_input_poll(void);

/* The shared pad, so exec_sensor.c does not open a second one. */
PadState *exec_pad(void);

/* The console's language as a tag the engine understands, for
 * MainActivity.preferredLanguageTags(). Always non-NULL; "en" if unknown. */
const char *exec_system_language_tag(void);
#endif
