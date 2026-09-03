/* exec_jni.h -- the JNI environment libexecutive_android.so expects.
 *
 * MIT licensed. See LICENSE.
 *
 * THE OUTBOUND SURFACE, ENUMERATED RATHER THAN GUESSED
 * ----------------------------------------------------
 * Every Java class string in .rodata was cross-referenced against its call
 * sites (tools/jni_scan.py, tools/xref_jni.py). There is no instance method
 * anywhere in the binary and no field access: GetFieldID and GetStaticFieldID
 * are never reached.
 *
 * Twenty rows are bound; SEVENTEEN of them are reachable in this game. The
 * three marked below appear nowhere in libexecutive_android.so -- verified by
 * extracting every NUL-terminated string in the image, where the other
 * seventeen names and every signature are present. They are inherited from the
 * Pizza Vs. Skeletons table, where the engine did call them, and are kept
 * because an unused row costs nothing while a missing one is a call into a
 * default trampoline. Do not read their presence as evidence about this game.
 *
 *   android/graphics/BitmapFactory
 *     decodeByteArray        ([BII)Landroid/graphics/Bitmap;
 *
 *   com/rivermanmedia/theexecutive/MainActivity
 *     setKeepScreenOn        (Z)V                      [absent here]
 *     preferredLanguageTags  ()[Ljava/lang/String;
 *     shareImage             (Ljava/lang/String;[III)V  [absent here]
 *     shareText              (Ljava/lang/String;)V      [absent here]
 *
 *   com/rivermanmedia/theexecutive/ExecutiveAudio
 *     registerSound          (Ljava/lang/String;Z)I
 *     releaseSound           (I)V
 *     play                   (IFFFF)V
 *     stop                   (I)V
 *     setRepeats             (IZ)V
 *     setVolume              (IF)V
 *     setPan                 (IF)V
 *
 *   com/rivermanmedia/theexecutive/PlayGamesBridge
 *     startAuthentication    (Z)V
 *     isAuthenticated        ()Z
 *     postScore              (II)V
 *     reportAchievement      (IFZ)V
 *     showLeaderboards       (I)V
 *     showAchievements       ()V
 *
 *   com/rivermanmedia/theexecutive/CloudSaveBridge
 *     write                  (Ljava/lang/String;[B)V
 *     read                   (Ljava/lang/String;)V
 *
 * All three bridge classes reach back into the engine through exported
 * natives, so a stub must not merely return -- it must complete the
 * round trip or the engine waits forever. See exec_jni.c: cloud_read()
 * calls nativeReadMissing, and gamecenter calls post back through
 * nativeScorePosted / nativeAchievementPosted.
 *
 * VTABLE INDICES ARE LOAD-BEARING
 * -------------------------------
 * The module dispatches by slot, not by name: `ldr x8,[x8,#0x388]; blr x8` is
 * GetStaticMethodID because 0x388/8 == 113. Getting a slot wrong calls the
 * wrong function with the wrong arguments and does not fail loudly. The
 * indices below were read out of the binary's own call sites, and
 * tools/check_jni_slots.py re-verifies them against this file.
 */

#ifndef EXEC_JNI_H
#define EXEC_JNI_H

#include <stdint.h>
#include <stddef.h>

/* Set when the engine asks to finish. */
extern volatile int jni_quit_requested;

void  jni_init(void);

/* Resolve the six natives the CloudSave and PlayGames bridges call back into.
 * Separate from jni_init because they live in libexecutive_android.so and cannot be
 * looked up until it is loaded and mapped. */
void  jni_bind_natives(void);

void *jni_env(void);            /* JNIEnv *                        */
void *jni_vm(void);             /* JavaVM *  -- nativeInit takes it */

/* The jclass passed as argument 2 of every static native call. The engine
 * never inspects it, but it has to be a valid pooled object so anything
 * calling GetObjectClass on it does not fault. */
void *jni_activity_class(void);

/* Object construction for the inbound direction (main.c builds the arguments
 * nativeInit and the CloudSave natives expect). */
void *jni_new_string(const char *utf8);
void *jni_new_bytearray(const void *data, int len);
void *jni_bytearray_data(void *arr, int *len_out);

/* Pool handle -> exec_bitmap *, for the AndroidBitmap_* trio. */
void *jni_bitmap_of(void *jbitmap);

/* CloudSaveBridge callbacks are deferred by one frame; main.c pumps this. */
void  jni_pump_deferred(void);

/* The AssetManager handle handed to nativeInit. Opaque; exec_asset.c is what
 * actually resolves it. */
void *jni_asset_manager(void);

#endif
