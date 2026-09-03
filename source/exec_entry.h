/* exec_entry.h -- the native entry points, exactly as classes.dex declares them.
 *
 * Read out of MainActivity's `private static native` declarations rather than
 * inferred from the assembly, so the argument order and count are the game's
 * own and not a reading of register usage.
 *
 *   nativeInit                 (Landroid/content/res/AssetManager;Ljava/lang/String;)V
 *   nativeSetEnv               (Ljava/lang/String;Ljava/lang/String;)V
 *   nativeSurfaceCreated       ()V
 *   nativeSurfaceChanged       (II)V
 *   nativeDrawFrame            ()V
 *   nativeResetFrameScheduler  ()V
 *   nativePointerDown          (IFF)V      id, x, y
 *   nativePointerMove          (IFFFF)V    id, prevX, prevY, x, y
 *   nativePointerUp            (IFF)V      id, x, y
 *   nativeKeyPressed           (I)V        mac virtual key code
 *   nativeKeyReleased          (I)V
 *   nativeAccelerometerChanged (FFF)V
 *
 * nativePointerMove carrying the PREVIOUS position as well as the current one
 * is the detail worth not getting wrong. MainActivity$PVSView keeps a
 * SparseArray of last positions per pointer id and passes both; on the first
 * move after a down, prev == current. The engine uses the delta for its drag
 * handling, so feeding it (x, y, x, y) makes every drag read as zero motion
 * and the game becomes unplayable in a way that looks like a physics bug.
 */
#ifndef EXEC_ENTRY_H
#define EXEC_ENTRY_H

typedef struct {
  void (*init)(void *env, void *cls, void *assetmgr, void *files_dir);
  void (*set_env)(void *env, void *cls, void *k, void *v);
  void (*surface_created)(void *env, void *cls);
  void (*surface_changed)(void *env, void *cls, int w, int h);
  void (*draw_frame)(void *env, void *cls);
  void (*reset_frame_scheduler)(void *env, void *cls);
  void (*pointer_down)(void *env, void *cls, int id, float x, float y);
  void (*pointer_move)(void *env, void *cls, int id, float px, float py, float x, float y);
  void (*pointer_up)(void *env, void *cls, int id, float x, float y);
  void (*key_pressed)(void *env, void *cls, int key);
  void (*key_released)(void *env, void *cls, int key);
  void (*accel_changed)(void *env, void *cls, float x, float y, float z);
} exec_entry;

extern exec_entry exec;
int exec_entry_bind(void);       /* 0 on success; logs each missing symbol */
#endif
