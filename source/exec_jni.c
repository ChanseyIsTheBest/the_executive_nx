/* exec_jni.c -- fake JNIEnv / JavaVM for libexecutive_android.so
 *
 * MIT licensed. See LICENSE.
 *
 * Slot indices come from the module's own call sites; see exec_jni.h.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "exec_jni.h"
#include "exec_audio.h"
#include "exec_bitmap.h"
#include "exec_asset.h"
#include "exec_log.h"
#include "exec_save.h"
#include "config.h"

volatile int jni_quit_requested = 0;

/* ------------------------------------------------------------------ */
/* object pool                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
  OBJ_FREE = 0, OBJ_CLASS, OBJ_STRING, OBJ_BYTEARRAY, OBJ_INTARRAY,
  OBJ_OBJARRAY, OBJ_BITMAP, OBJ_ASSETMGR
} obj_kind;

typedef struct {
  obj_kind kind;
  int      refs;          /* 0 = local, >0 = global                    */
  int      cls;           /* CLASS: which one (cls_id)                 */
  char    *str;           /* STRING: NUL-terminated UTF-8              */
  void    *data;          /* arrays: raw bytes; BITMAP: exec_bitmap *   */
  int      len;           /* arrays: element count                     */
} exec_obj;

#define POOL_MAX 1024
static exec_obj  pool[POOL_MAX];
static int      pool_next = 1;      /* index 0 reserved as "null"      */

static exec_obj *obj_of(void *h) {
  uintptr_t i = (uintptr_t)h;
  if (i == 0 || i >= POOL_MAX) return NULL;
  return pool[i].kind ? &pool[i] : NULL;
}

static int pool_scan(obj_kind k) {
  for (int n = 0; n < POOL_MAX; n++) {
    int i = pool_next;
    pool_next = (pool_next + 1 < POOL_MAX) ? pool_next + 1 : 1;
    if (pool[i].kind == OBJ_FREE) {
      memset(&pool[i], 0, sizeof(pool[i]));
      pool[i].kind = k;
      return i;
    }
  }
  return 0;
}

/* Real JNI frees every local reference when the native method returns. This
 * pool does not, because nothing here knows where a native call ends -- so a
 * caller that never calls DeleteLocalRef leaks a slot, and for a Bitmap it
 * leaks the decoded pixels too, which across a whole game's texture set is
 * not survivable -- the reference port counted 1508.
 *
 * Rather than reclaim on a guess about call boundaries -- which would free an
 * object the engine legitimately still held -- the sweep runs only when the
 * alternative is failing the allocation outright. Anything with refs == 0 is
 * a local, and a caller relying on a local surviving past the point where the
 * pool is full was already relying on undefined JNI behaviour. */
static void pool_sweep(void) {
  int freed = 0;
  for (int i = 1; i < POOL_MAX; i++) {
    if (pool[i].kind == OBJ_FREE || pool[i].refs > 0) continue;
    if (pool[i].kind == OBJ_CLASS || pool[i].kind == OBJ_ASSETMGR) continue;
    free(pool[i].str);
    if (pool[i].kind == OBJ_BITMAP) exec_bitmap_free((exec_bitmap *)pool[i].data);
    else                            free(pool[i].data);
    memset(&pool[i], 0, sizeof(pool[i]));
    freed++;
  }
  exec_log(EXEC_LOG_WARN, "JNI pool full; reclaimed %d unreferenced local(s). "
                        "Something is not calling DeleteLocalRef.", freed);
}

static void *obj_new(obj_kind k) {
  int i = pool_scan(k);
  if (!i) { pool_sweep(); i = pool_scan(k); }
  if (!i) { exec_log(EXEC_LOG_ERROR, "JNI object pool exhausted"); return NULL; }
  return (void *)(uintptr_t)i;
}

static void obj_free(void *h) {
  exec_obj *o = obj_of(h);
  if (!o || o->refs > 0) return;
  if (o->kind == OBJ_CLASS || o->kind == OBJ_ASSETMGR) return;  /* pinned */
  free(o->str);
  /* A BITMAP's data is a exec_bitmap, which owns a separate pixel buffer;
   * free()ing the struct alone leaks the pixels, and a 1080p RGBA texture is
   * 8 MB of them. */
  if (o->kind == OBJ_BITMAP) exec_bitmap_free((exec_bitmap *)o->data);
  else                       free(o->data);
  memset(o, 0, sizeof(*o));
}

/* ------------------------------------------------------------------ */
/* classes and methods                                                */
/* ------------------------------------------------------------------ */

enum cls_id { C_NONE = 0, C_BITMAPFACTORY, C_ACTIVITY, C_AUDIO, C_PLAYGAMES,
              C_CLOUD, C_COUNT };

static const char *const class_names[C_COUNT] = {
  [C_BITMAPFACTORY] = "android/graphics/BitmapFactory",
  [C_ACTIVITY]      = "com/rivermanmedia/theexecutive/MainActivity",
  [C_AUDIO]         = "com/rivermanmedia/theexecutive/ExecutiveAudio",
  [C_PLAYGAMES]     = "com/rivermanmedia/theexecutive/PlayGamesBridge",
  [C_CLOUD]         = "com/rivermanmedia/theexecutive/CloudSaveBridge",
};

static void *class_obj[C_COUNT];

/* One token per (class, name). Signatures are checked on lookup so a
 * mismatch is reported instead of silently dispatching. */
enum mid {
  M_NONE = 0,
  M_DECODE_BYTEARRAY,
  M_SET_KEEP_SCREEN_ON, M_PREFERRED_LANGS, M_SHARE_IMAGE, M_SHARE_TEXT,
  M_REGISTER_SOUND, M_RELEASE_SOUND, M_PLAY, M_STOP, M_SET_REPEATS,
  M_SET_VOLUME, M_SET_PAN,
  M_START_AUTH, M_IS_AUTH, M_POST_SCORE, M_REPORT_ACH,
  M_SHOW_LEADERBOARDS, M_SHOW_ACHIEVEMENTS,
  M_CLOUD_WRITE, M_CLOUD_READ,
  M_COUNT
};

static const struct { int cls; const char *name, *sig; } method_tab[M_COUNT] = {
  [M_DECODE_BYTEARRAY]   = { C_BITMAPFACTORY, "decodeByteArray",  "([BII)Landroid/graphics/Bitmap;" },
  [M_SET_KEEP_SCREEN_ON] = { C_ACTIVITY,  "setKeepScreenOn",      "(Z)V" },
  [M_PREFERRED_LANGS]    = { C_ACTIVITY,  "preferredLanguageTags","()[Ljava/lang/String;" },
  [M_SHARE_IMAGE]        = { C_ACTIVITY,  "shareImage",           "(Ljava/lang/String;[III)V" },
  [M_SHARE_TEXT]         = { C_ACTIVITY,  "shareText",            "(Ljava/lang/String;)V" },
  [M_REGISTER_SOUND]     = { C_AUDIO,     "registerSound",        "(Ljava/lang/String;Z)I" },
  [M_RELEASE_SOUND]      = { C_AUDIO,     "releaseSound",         "(I)V" },
  [M_PLAY]               = { C_AUDIO,     "play",                 "(IFFFF)V" },
  [M_STOP]               = { C_AUDIO,     "stop",                 "(I)V" },
  [M_SET_REPEATS]        = { C_AUDIO,     "setRepeats",           "(IZ)V" },
  [M_SET_VOLUME]         = { C_AUDIO,     "setVolume",            "(IF)V" },
  [M_SET_PAN]            = { C_AUDIO,     "setPan",               "(IF)V" },
  [M_START_AUTH]         = { C_PLAYGAMES, "startAuthentication",  "(Z)V" },
  [M_IS_AUTH]            = { C_PLAYGAMES, "isAuthenticated",      "()Z" },
  [M_POST_SCORE]         = { C_PLAYGAMES, "postScore",            "(II)V" },
  [M_REPORT_ACH]         = { C_PLAYGAMES, "reportAchievement",    "(IFZ)V" },
  [M_SHOW_LEADERBOARDS]  = { C_PLAYGAMES, "showLeaderboards",     "(I)V" },
  [M_SHOW_ACHIEVEMENTS]  = { C_PLAYGAMES, "showAchievements",     "()V" },
  [M_CLOUD_WRITE]        = { C_CLOUD,     "write",                "(Ljava/lang/String;[B)V" },
  [M_CLOUD_READ]         = { C_CLOUD,     "read",                 "(Ljava/lang/String;)V" },
};

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

/* Natives the bridges call back into. Resolved once in jni_init(). */
static void (*n_read_succeeded)(void *env, void *cls, void *bytes);
static void (*n_read_missing)(void *env, void *cls);
static void (*n_read_failed)(void *env, void *cls);
static void (*n_write_finished)(void *env, void *cls, int ok);
static void (*n_score_posted)(void *env, void *cls, int a, int b);
static void (*n_ach_posted)(void *env, void *cls, int id, float pct, int ok);

extern uintptr_t exec_find_export(const char *name);   /* main.c */

static const char *str_of(void *h) {
  exec_obj *o = obj_of(h);
  return (o && o->kind == OBJ_STRING && o->str) ? o->str : "";
}

/* On Android these callbacks arrive from a Play Games task on the UI thread,
 * never from inside the engine's own iCloud_read frame. Calling them inline
 * would re-enter the save code with its own call still on the stack, which is
 * a different execution than the game was written against. They are deferred
 * to the top of the next frame instead. */
enum { DEF_NONE = 0, DEF_READ_OK, DEF_READ_MISSING, DEF_READ_FAILED, DEF_WRITE };
static struct { int what; void *read_arr; int write_ok; } deferred;

static void defer_write_finished(int ok) {
  deferred.what = DEF_WRITE; deferred.write_ok = ok;
}

void jni_pump_deferred(void) {
  int what = deferred.what;
  if (!what) return;
  deferred.what = DEF_NONE;
  void *env = jni_env(), *cls = class_obj[C_CLOUD];
  switch (what) {
  case DEF_READ_OK:
    if (n_read_succeeded) n_read_succeeded(env, cls, deferred.read_arr);
    if (deferred.read_arr) {
      exec_obj *o = obj_of(deferred.read_arr);
      if (o && o->refs > 0) o->refs--;
      obj_free(deferred.read_arr);
      deferred.read_arr = NULL;
    }
    break;
  case DEF_READ_MISSING: if (n_read_missing)   n_read_missing(env, cls); break;
  case DEF_READ_FAILED:  if (n_read_failed)    n_read_failed(env, cls);  break;
  case DEF_WRITE:        if (n_write_finished) n_write_finished(env, cls, deferred.write_ok); break;
  }
}

/* The single point every static call funnels through. Returning a jlong-sized
 * value keeps one implementation for void/int/boolean/object results. */
static int64_t dispatch(int mid, va_list ap) {
  switch (mid) {

  /* ---- BitmapFactory ---- */
  case M_DECODE_BYTEARRAY: {
    void *arr = va_arg(ap, void *);
    int   off = va_arg(ap, int);
    int   len = va_arg(ap, int);
    exec_obj *a = obj_of(arr);
    if (!a || a->kind != OBJ_BYTEARRAY) return 0;
    if (off < 0 || len < 0 || off + len > a->len) return 0;
    exec_bitmap *bm = exec_bitmap_decode((const uint8_t *)a->data + off, len);
    if (!bm) {
      /* An all-zero buffer here means the caller's copy into the array never
       * happened -- which is what an unbound Set*ArrayRegion slot looks like
       * from this side. Say so, because the crash it causes lands several
       * frames away inside the engine with a null Bitmap. */
      const unsigned char *p = (const unsigned char *)a->data + off;
      int allzero = 1;
      for (int i = 0; i < len && i < 64; i++) if (p[i]) { allzero = 0; break; }
      exec_log(EXEC_LOG_ERROR, "decodeByteArray failed (%d bytes)%s", len,
              allzero ? " -- the array is all zeros, so nothing ever wrote "
                        "into it; check the Set*ArrayRegion slot bindings" : "");
      return 0;
    }
    void *h = obj_new(OBJ_BITMAP);
    if (!h) { exec_bitmap_free(bm); return 0; }
    obj_of(h)->data = bm;
    return (int64_t)(uintptr_t)h;
  }

  /* ---- MainActivity ---- */
  case M_SET_KEEP_SCREEN_ON:
    (void)va_arg(ap, int);          /* the panel never sleeps mid-frame */
    return 0;

  case M_PREFERRED_LANGS: {
    const char *const *tags; int n;
    exec_config_languages(&tags, &n);
    void *arr = obj_new(OBJ_OBJARRAY);
    if (!arr) return 0;
    exec_obj *a = obj_of(arr);
    a->len  = n;
    a->data = calloc((size_t)n, sizeof(void *));
    for (int i = 0; i < n; i++)
      ((void **)a->data)[i] = jni_new_string(tags[i]);
    return (int64_t)(uintptr_t)arr;
  }

  case M_SHARE_IMAGE:
  case M_SHARE_TEXT:
    /* No share sheet here. Consume the arguments so the va_list stays
     * balanced and return; the engine does not wait on a result. */
    return 0;

  /* setKeepScreenOn, shareImage and shareText are bound but UNREACHED.
   *
   * All three are absent from libexecutive_android.so -- checked by extracting
   * every NUL-terminated string in the image, where the other 17 method names
   * and every signature are present. They are inherited from the Pizza Vs.
   * Skeletons table, where the engine did call them.
   *
   * Kept rather than deleted: an unused row costs one table entry and nothing
   * at runtime, whereas a missing one is a call into a default trampoline, and
   * the studio ships this engine across several titles at several versions.
   * They are marked so nobody later reads their presence as evidence that this
   * game uses them. */

  /* ---- ExecutiveAudio ---- */
  case M_REGISTER_SOUND: {
    void *name = va_arg(ap, void *);
    int   is_music = va_arg(ap, int);
    return exec_audio_register(str_of(name), is_music != 0);
  }
  case M_RELEASE_SOUND: exec_audio_release(va_arg(ap, int)); return 0;
  case M_STOP:          exec_audio_stop(va_arg(ap, int));    return 0;
  case M_PLAY: {
    /* Argument meanings are no longer a hypothesis: ExecutiveAudio.play in
     * classes.dex reads
     *     volume = clamp(p2, 0, 1);  pan = clamp(p3, -1, 1);
     *     music  -> playMusic(handle, entry, max(0, p1))
     *     sfx    -> playSfx(entry, p4)
     * so p1 is a start offset in SECONDS (music only) and p4 is a playback
     * rate (sfx only), each ignored by the other path.
     *
     * JNI varargs promote jfloat to double. Reading these as float is the
     * classic way to get silence and no error. */
    int   id     = va_arg(ap, int);
    float start  = (float)va_arg(ap, double);
    float volume = (float)va_arg(ap, double);
    float pan    = (float)va_arg(ap, double);
    float rate   = (float)va_arg(ap, double);
    exec_audio_play(id, start, volume, pan, rate);
    return 0;
  }
  case M_SET_REPEATS: {
    int id = va_arg(ap, int), rep = va_arg(ap, int);
    exec_audio_set_repeats(id, rep != 0); return 0;
  }
  case M_SET_VOLUME: {
    int id = va_arg(ap, int); float v = (float)va_arg(ap, double);
    exec_audio_set_volume(id, v); return 0;
  }
  case M_SET_PAN: {
    int id = va_arg(ap, int); float p = (float)va_arg(ap, double);
    exec_audio_set_pan(id, p); return 0;
  }

  /* ---- PlayGamesBridge ----
   * There is no Play Games service here, so authentication resolves as
   * "signed out" and stays there. The post calls still have to complete
   * their round trip: the engine keeps a pending-operation count. */
  case M_START_AUTH:    (void)va_arg(ap, int); return 0;
  case M_IS_AUTH:       return 0;
  case M_POST_SCORE: {
    int a = va_arg(ap, int), b = va_arg(ap, int);
    if (n_score_posted) n_score_posted(jni_env(), class_obj[C_PLAYGAMES], a, b);
    return 0;
  }
  case M_REPORT_ACH: {
    int   id  = va_arg(ap, int);
    float pct = (float)va_arg(ap, double);
    int   unl = va_arg(ap, int);
    if (n_ach_posted) n_ach_posted(jni_env(), class_obj[C_PLAYGAMES], id, pct, unl);
    return 0;
  }
  case M_SHOW_LEADERBOARDS: (void)va_arg(ap, int); return 0;
  case M_SHOW_ACHIEVEMENTS: return 0;

  /* ---- CloudSaveBridge ----
   * Backed by a file next to the .nro rather than by Play Games Saved Games.
   * The callbacks are mandatory: the engine blocks its save flow until one
   * of nativeReadSucceeded / nativeReadMissing / nativeReadFailed lands. */
  case M_CLOUD_WRITE: {
    void *key = va_arg(ap, void *);
    void *arr = va_arg(ap, void *);
    exec_obj *a = obj_of(arr);
    int ok = (a && a->kind == OBJ_BYTEARRAY)
             ? exec_save_write(str_of(key), a->data, a->len) : 0;
    defer_write_finished(ok);
    return 0;
  }
  case M_CLOUD_READ: {
    void *key = va_arg(ap, void *);
    void *buf = NULL; int len = 0;
    int r = exec_save_read(str_of(key), &buf, &len);
    if (r > 0) {
      deferred.read_arr = jni_new_bytearray(buf, len);
      exec_obj *o = obj_of(deferred.read_arr);
      if (o) o->refs++;              /* survive until the callback runs */
      free(buf);
      deferred.what = DEF_READ_OK;
    } else {
      deferred.what = (r == 0) ? DEF_READ_MISSING : DEF_READ_FAILED;
    }
    return 0;
  }

  default:
    exec_log(EXEC_LOG_ERROR, "unhandled static method token %d", mid);
    return 0;
  }
}

/* ------------------------------------------------------------------ */
/* JNIEnv entry points                                                */
/* ------------------------------------------------------------------ */

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  for (int i = 1; i < C_COUNT; i++)
    if (class_names[i] && strcmp(class_names[i], name) == 0)
      return class_obj[i];
  exec_log(EXEC_LOG_WARN, "FindClass miss: %s", name);
  return NULL;
}

static void *jni_GetStaticMethodID(void *env, void *cls, const char *name,
                                   const char *sig) {
  (void)env;
  exec_obj *c = obj_of(cls);
  int cid = c ? c->cls : C_NONE;
  for (int m = 1; m < M_COUNT; m++) {
    if (method_tab[m].cls != cid) continue;
    if (strcmp(method_tab[m].name, name) != 0) continue;
    if (strcmp(method_tab[m].sig, sig) != 0) {
      exec_log(EXEC_LOG_ERROR, "signature drift on %s: binary wants %s, we have %s",
              name, sig, method_tab[m].sig);
      return NULL;
    }
    return (void *)(uintptr_t)m;
  }
  exec_log(EXEC_LOG_WARN, "GetStaticMethodID miss: %s %s", name, sig);
  return NULL;
}

static void *jni_GetMethodID(void *env, void *cls, const char *name,
                             const char *sig) {
  (void)env; (void)cls;
  exec_log(EXEC_LOG_WARN, "GetMethodID(%s %s) -- no instance method is reached "
                        "in this binary; treat this as a finding", name, sig);
  return NULL;
}

/* `r` is declared before the early-out because ret_expr mentions it on both
 * paths. An earlier version declared it only after the bounds check, so an
 * unknown method id referenced an undeclared variable -- which the compiler
 * catches, but only once every arm of the macro is instantiated. */
#define CALL_STATIC_BODY(ret_expr)                        \
  do {                                                    \
    (void)env; (void)cls;                                 \
    int64_t r = 0;                                        \
    const int m = (int)(uintptr_t)mid;                    \
    if (m > 0 && m < M_COUNT) {                           \
      va_list ap; va_start(ap, mid);                      \
      r = dispatch(m, ap);                                \
      va_end(ap);                                         \
    }                                                     \
    return (ret_expr);                                    \
  } while (0)

static void jni_CallStaticVoidMethod(void *env, void *cls, void *mid, ...) {
  (void)env; (void)cls;
  int m = (int)(uintptr_t)mid;
  if (m <= 0 || m >= M_COUNT) return;
  va_list ap; va_start(ap, mid); dispatch(m, ap); va_end(ap);
}
static void jni_CallStaticVoidMethodV(void *env, void *cls, void *mid, va_list ap) {
  (void)env; (void)cls;
  int m = (int)(uintptr_t)mid;
  if (m > 0 && m < M_COUNT) dispatch(m, ap);
}
static int jni_CallStaticIntMethod(void *env, void *cls, void *mid, ...) {
  CALL_STATIC_BODY((int)r);
}
static int jni_CallStaticIntMethodV(void *env, void *cls, void *mid, va_list ap) {
  (void)env; (void)cls;
  int m = (int)(uintptr_t)mid;
  return (m > 0 && m < M_COUNT) ? (int)dispatch(m, ap) : 0;
}
static unsigned char jni_CallStaticBooleanMethod(void *env, void *cls, void *mid, ...) {
  CALL_STATIC_BODY((unsigned char)(r != 0));
}
static unsigned char jni_CallStaticBooleanMethodV(void *env, void *cls, void *mid, va_list ap) {
  (void)env; (void)cls;
  int m = (int)(uintptr_t)mid;
  return (m > 0 && m < M_COUNT) ? (unsigned char)(dispatch(m, ap) != 0) : 0;
}
static void *jni_CallStaticObjectMethod(void *env, void *cls, void *mid, ...) {
  CALL_STATIC_BODY((void *)(uintptr_t)r);
}
static void *jni_CallStaticObjectMethodV(void *env, void *cls, void *mid, va_list ap) {
  (void)env; (void)cls;
  int m = (int)(uintptr_t)mid;
  return (m > 0 && m < M_COUNT) ? (void *)(uintptr_t)dispatch(m, ap) : NULL;
}

/* --- strings --- */
static void *jni_NewStringUTF(void *env, const char *s) {
  (void)env; return jni_new_string(s);
}
static const char *jni_GetStringUTFChars(void *env, void *s, unsigned char *copy) {
  (void)env; if (copy) *copy = 0; return str_of(s);
}
static void jni_ReleaseStringUTFChars(void *env, void *s, const char *c) {
  (void)env; (void)s; (void)c;
}
static int jni_GetStringUTFLength(void *env, void *s) {
  (void)env; return (int)strlen(str_of(s));
}

/* --- arrays --- */
static int jni_GetArrayLength(void *env, void *a) {
  (void)env; exec_obj *o = obj_of(a); return o ? o->len : 0;
}
static void *jni_NewByteArray(void *env, int len) {
  (void)env; return jni_new_bytearray(NULL, len);
}
static void *jni_GetByteArrayElements(void *env, void *a, unsigned char *copy) {
  (void)env; if (copy) *copy = 0;
  exec_obj *o = obj_of(a); return o ? o->data : NULL;
}
static void jni_ReleaseByteArrayElements(void *env, void *a, void *p, int mode) {
  (void)env; (void)a; (void)p; (void)mode;
}
static void jni_GetByteArrayRegion(void *env, void *a, int start, int len, void *buf) {
  (void)env; exec_obj *o = obj_of(a);
  if (o && o->data && start >= 0 && len >= 0 && start + len <= o->len)
    memcpy(buf, (char *)o->data + start, (size_t)len);
}
static void jni_SetByteArrayRegion(void *env, void *a, int start, int len, const void *buf) {
  (void)env; exec_obj *o = obj_of(a);
  if (o && o->data && start >= 0 && len >= 0 && start + len <= o->len)
    memcpy((char *)o->data + start, buf, (size_t)len);
}
static void *jni_NewIntArray(void *env, int len) {
  (void)env;
  void *h = obj_new(OBJ_INTARRAY); if (!h) return NULL;
  exec_obj *o = obj_of(h); o->len = len; o->data = calloc((size_t)len, 4);
  return h;
}
static void *jni_GetIntArrayElements(void *env, void *a, unsigned char *copy) {
  (void)env; if (copy) *copy = 0;
  exec_obj *o = obj_of(a); return o ? o->data : NULL;
}
static void jni_ReleaseIntArrayElements(void *env, void *a, void *p, int mode) {
  (void)env; (void)a; (void)p; (void)mode;
}
static void jni_SetIntArrayRegion(void *env, void *a, int start, int len, const void *buf) {
  (void)env; exec_obj *o = obj_of(a);
  if (o && o->data && start >= 0 && len >= 0 && start + len <= o->len)
    memcpy((int *)o->data + start, buf, (size_t)len * 4);
}
static void *jni_GetObjectArrayElement(void *env, void *a, int i) {
  (void)env; exec_obj *o = obj_of(a);
  if (!o || o->kind != OBJ_OBJARRAY || i < 0 || i >= o->len) return NULL;
  return ((void **)o->data)[i];
}

/* --- refs, exceptions, misc --- */
static void *jni_NewGlobalRef(void *env, void *o) {
  (void)env; exec_obj *p = obj_of(o); if (p) p->refs++; return o;
}
static void jni_DeleteGlobalRef(void *env, void *o) {
  (void)env; exec_obj *p = obj_of(o); if (p && p->refs > 0) { p->refs--; if (!p->refs) obj_free(o); }
}
static void jni_DeleteLocalRef(void *env, void *o) { (void)env; obj_free(o); }
static void *jni_NewLocalRef(void *env, void *o)   { (void)env; return o; }
static void *jni_GetObjectClass(void *env, void *o) {
  (void)env; exec_obj *p = obj_of(o);
  if (p && p->kind == OBJ_BITMAP) return class_obj[C_BITMAPFACTORY];
  return class_obj[C_ACTIVITY];
}
static unsigned char jni_ExceptionCheck(void *env)     { (void)env; return 0; }
static void *jni_ExceptionOccurred(void *env)          { (void)env; return NULL; }
static void jni_ExceptionClear(void *env)              { (void)env; }
static void jni_ExceptionDescribe(void *env)           { (void)env; }
static int  jni_PushLocalFrame(void *env, int n)       { (void)env; (void)n; return 0; }
static void *jni_PopLocalFrame(void *env, void *r)     { (void)env; return r; }
static int  jni_EnsureLocalCapacity(void *env, int n)  { (void)env; (void)n; return 0; }
static int  jni_GetVersion(void *env)                  { (void)env; return 0x00010006; }
static unsigned char jni_IsSameObject(void *env, void *a, void *b) {
  (void)env; return a == b;
}
static void jni_SetObjectArrayElement(void *env, void *a, int i, void *v) {
  (void)env;
  exec_obj *o = obj_of(a);
  if (o && o->kind == OBJ_OBJARRAY && o->data && i >= 0 && i < o->len)
    ((void **)o->data)[i] = v;
}
static int jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm) *vm = jni_vm();
  return 0;
}

/* ------------------------------------------------------------------ */
/* vtables                                                            */
/* ------------------------------------------------------------------ */

static const char *const jni_slot_names[233] = {
  "reserved0", "reserved1", "reserved2", "reserved3",
  "GetVersion", "DefineClass", "FindClass", "FromReflectedMethod",
  "FromReflectedField", "ToReflectedMethod", "GetSuperclass", "IsAssignableFrom",
  "ToReflectedField", "Throw", "ThrowNew", "ExceptionOccurred",
  "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame",
  "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef",
  "IsSameObject", "NewLocalRef", "EnsureLocalCapacity", "AllocObject",
  "NewObject", "NewObjectV", "NewObjectA", "GetObjectClass",
  "IsInstanceOf", "GetMethodID", "CallObjectMethod", "CallObjectMethodV",
  "CallObjectMethodA", "CallBooleanMethod", "CallBooleanMethodV", "CallBooleanMethodA",
  "CallByteMethod", "CallByteMethodV", "CallByteMethodA", "CallCharMethod",
  "CallCharMethodV", "CallCharMethodA", "CallShortMethod", "CallShortMethodV",
  "CallShortMethodA", "CallIntMethod", "CallIntMethodV", "CallIntMethodA",
  "CallLongMethod", "CallLongMethodV", "CallLongMethodA", "CallFloatMethod",
  "CallFloatMethodV", "CallFloatMethodA", "CallDoubleMethod", "CallDoubleMethodV",
  "CallDoubleMethodA", "CallVoidMethod", "CallVoidMethodV", "CallVoidMethodA",
  "CallNonvirtualObjectMethod", "CallNonvirtualObjectMethodV", "CallNonvirtualObjectMethodA", "CallNonvirtualBooleanMethod",
  "CallNonvirtualBooleanMethodV", "CallNonvirtualBooleanMethodA", "CallNonvirtualByteMethod", "CallNonvirtualByteMethodV",
  "CallNonvirtualByteMethodA", "CallNonvirtualCharMethod", "CallNonvirtualCharMethodV", "CallNonvirtualCharMethodA",
  "CallNonvirtualShortMethod", "CallNonvirtualShortMethodV", "CallNonvirtualShortMethodA", "CallNonvirtualIntMethod",
  "CallNonvirtualIntMethodV", "CallNonvirtualIntMethodA", "CallNonvirtualLongMethod", "CallNonvirtualLongMethodV",
  "CallNonvirtualLongMethodA", "CallNonvirtualFloatMethod", "CallNonvirtualFloatMethodV", "CallNonvirtualFloatMethodA",
  "CallNonvirtualDoubleMethod", "CallNonvirtualDoubleMethodV", "CallNonvirtualDoubleMethodA", "CallNonvirtualVoidMethod",
  "CallNonvirtualVoidMethodV", "CallNonvirtualVoidMethodA", "GetFieldID", "GetObjectField",
  "GetBooleanField", "GetByteField", "GetCharField", "GetShortField",
  "GetIntField", "GetLongField", "GetFloatField", "GetDoubleField",
  "SetObjectField", "SetBooleanField", "SetByteField", "SetCharField",
  "SetShortField", "SetIntField", "SetLongField", "SetFloatField",
  "SetDoubleField", "GetStaticMethodID", "CallStaticObjectMethod", "CallStaticObjectMethodV",
  "CallStaticObjectMethodA", "CallStaticBooleanMethod", "CallStaticBooleanMethodV", "CallStaticBooleanMethodA",
  "CallStaticByteMethod", "CallStaticByteMethodV", "CallStaticByteMethodA", "CallStaticCharMethod",
  "CallStaticCharMethodV", "CallStaticCharMethodA", "CallStaticShortMethod", "CallStaticShortMethodV",
  "CallStaticShortMethodA", "CallStaticIntMethod", "CallStaticIntMethodV", "CallStaticIntMethodA",
  "CallStaticLongMethod", "CallStaticLongMethodV", "CallStaticLongMethodA", "CallStaticFloatMethod",
  "CallStaticFloatMethodV", "CallStaticFloatMethodA", "CallStaticDoubleMethod", "CallStaticDoubleMethodV",
  "CallStaticDoubleMethodA", "CallStaticVoidMethod", "CallStaticVoidMethodV", "CallStaticVoidMethodA",
  "GetStaticFieldID", "GetStaticObjectField", "GetStaticBooleanField", "GetStaticByteField",
  "GetStaticCharField", "GetStaticShortField", "GetStaticIntField", "GetStaticLongField",
  "GetStaticFloatField", "GetStaticDoubleField", "SetStaticObjectField", "SetStaticBooleanField",
  "SetStaticByteField", "SetStaticCharField", "SetStaticShortField", "SetStaticIntField",
  "SetStaticLongField", "SetStaticFloatField", "SetStaticDoubleField", "NewString",
  "GetStringLength", "GetStringChars", "ReleaseStringChars", "NewStringUTF",
  "GetStringUTFLength", "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength",
  "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement", "NewBooleanArray",
  "NewByteArray", "NewCharArray", "NewShortArray", "NewIntArray",
  "NewLongArray", "NewFloatArray", "NewDoubleArray", "GetBooleanArrayElements",
  "GetByteArrayElements", "GetCharArrayElements", "GetShortArrayElements", "GetIntArrayElements",
  "GetLongArrayElements", "GetFloatArrayElements", "GetDoubleArrayElements", "ReleaseBooleanArrayElements",
  "ReleaseByteArrayElements", "ReleaseCharArrayElements", "ReleaseShortArrayElements", "ReleaseIntArrayElements",
  "ReleaseLongArrayElements", "ReleaseFloatArrayElements", "ReleaseDoubleArrayElements", "GetBooleanArrayRegion",
  "GetByteArrayRegion", "GetCharArrayRegion", "GetShortArrayRegion", "GetIntArrayRegion",
  "GetLongArrayRegion", "GetFloatArrayRegion", "GetDoubleArrayRegion", "SetBooleanArrayRegion",
  "SetByteArrayRegion", "SetCharArrayRegion", "SetShortArrayRegion", "SetIntArrayRegion",
  "SetLongArrayRegion", "SetFloatArrayRegion", "SetDoubleArrayRegion", "RegisterNatives",
  "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM",
  "GetStringRegion", "GetStringUTFRegion", "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical",
  "GetStringCritical", "ReleaseStringCritical", "NewWeakGlobalRef", "DeleteWeakGlobalRef",
  "ExceptionCheck", "NewDirectByteBuffer", "GetDirectBufferAddress", "GetDirectBufferCapacity",
  "GetObjectRefType",
};
/* ---- self-identifying defaults (generated) --------------------------------
 *
 * Every slot starts out pointing at a trampoline that knows its own index.
 * An unbound slot therefore says so, once, naming the JNI function the spec
 * puts there -- instead of returning 0 and letting the caller carry on.
 *
 * That distinction is not academic. SetByteArrayRegion was bound at 209
 * instead of 208, so the engine's copy into a freshly allocated byte array
 * landed on a no-op. NewByteArray had already zeroed the buffer, so nothing
 * failed: BitmapFactory.decodeByteArray got 81624 bytes of zeros, returned
 * null, and RMFont::build_from_filename dereferenced it three frames later.
 * One log line here would have named the problem immediately.
 */
static int64_t jni_unbound(int slot);
#define U(n) static int64_t jni_u##n(void) { return jni_unbound(n); }
U(0) U(1) U(2) U(3) U(4) U(5) U(6) U(7)
U(8) U(9) U(10) U(11) U(12) U(13) U(14) U(15)
U(16) U(17) U(18) U(19) U(20) U(21) U(22) U(23)
U(24) U(25) U(26) U(27) U(28) U(29) U(30) U(31)
U(32) U(33) U(34) U(35) U(36) U(37) U(38) U(39)
U(40) U(41) U(42) U(43) U(44) U(45) U(46) U(47)
U(48) U(49) U(50) U(51) U(52) U(53) U(54) U(55)
U(56) U(57) U(58) U(59) U(60) U(61) U(62) U(63)
U(64) U(65) U(66) U(67) U(68) U(69) U(70) U(71)
U(72) U(73) U(74) U(75) U(76) U(77) U(78) U(79)
U(80) U(81) U(82) U(83) U(84) U(85) U(86) U(87)
U(88) U(89) U(90) U(91) U(92) U(93) U(94) U(95)
U(96) U(97) U(98) U(99) U(100) U(101) U(102) U(103)
U(104) U(105) U(106) U(107) U(108) U(109) U(110) U(111)
U(112) U(113) U(114) U(115) U(116) U(117) U(118) U(119)
U(120) U(121) U(122) U(123) U(124) U(125) U(126) U(127)
U(128) U(129) U(130) U(131) U(132) U(133) U(134) U(135)
U(136) U(137) U(138) U(139) U(140) U(141) U(142) U(143)
U(144) U(145) U(146) U(147) U(148) U(149) U(150) U(151)
U(152) U(153) U(154) U(155) U(156) U(157) U(158) U(159)
U(160) U(161) U(162) U(163) U(164) U(165) U(166) U(167)
U(168) U(169) U(170) U(171) U(172) U(173) U(174) U(175)
U(176) U(177) U(178) U(179) U(180) U(181) U(182) U(183)
U(184) U(185) U(186) U(187) U(188) U(189) U(190) U(191)
U(192) U(193) U(194) U(195) U(196) U(197) U(198) U(199)
U(200) U(201) U(202) U(203) U(204) U(205) U(206) U(207)
U(208) U(209) U(210) U(211) U(212) U(213) U(214) U(215)
U(216) U(217) U(218) U(219) U(220) U(221) U(222) U(223)
U(224) U(225) U(226) U(227) U(228) U(229) U(230) U(231)
U(232)
#undef U

static void *const jni_default_tab[233] = {
  (void *)&jni_u0, (void *)&jni_u1, (void *)&jni_u2, (void *)&jni_u3, (void *)&jni_u4, (void *)&jni_u5, (void *)&jni_u6, (void *)&jni_u7,
  (void *)&jni_u8, (void *)&jni_u9, (void *)&jni_u10, (void *)&jni_u11, (void *)&jni_u12, (void *)&jni_u13, (void *)&jni_u14, (void *)&jni_u15,
  (void *)&jni_u16, (void *)&jni_u17, (void *)&jni_u18, (void *)&jni_u19, (void *)&jni_u20, (void *)&jni_u21, (void *)&jni_u22, (void *)&jni_u23,
  (void *)&jni_u24, (void *)&jni_u25, (void *)&jni_u26, (void *)&jni_u27, (void *)&jni_u28, (void *)&jni_u29, (void *)&jni_u30, (void *)&jni_u31,
  (void *)&jni_u32, (void *)&jni_u33, (void *)&jni_u34, (void *)&jni_u35, (void *)&jni_u36, (void *)&jni_u37, (void *)&jni_u38, (void *)&jni_u39,
  (void *)&jni_u40, (void *)&jni_u41, (void *)&jni_u42, (void *)&jni_u43, (void *)&jni_u44, (void *)&jni_u45, (void *)&jni_u46, (void *)&jni_u47,
  (void *)&jni_u48, (void *)&jni_u49, (void *)&jni_u50, (void *)&jni_u51, (void *)&jni_u52, (void *)&jni_u53, (void *)&jni_u54, (void *)&jni_u55,
  (void *)&jni_u56, (void *)&jni_u57, (void *)&jni_u58, (void *)&jni_u59, (void *)&jni_u60, (void *)&jni_u61, (void *)&jni_u62, (void *)&jni_u63,
  (void *)&jni_u64, (void *)&jni_u65, (void *)&jni_u66, (void *)&jni_u67, (void *)&jni_u68, (void *)&jni_u69, (void *)&jni_u70, (void *)&jni_u71,
  (void *)&jni_u72, (void *)&jni_u73, (void *)&jni_u74, (void *)&jni_u75, (void *)&jni_u76, (void *)&jni_u77, (void *)&jni_u78, (void *)&jni_u79,
  (void *)&jni_u80, (void *)&jni_u81, (void *)&jni_u82, (void *)&jni_u83, (void *)&jni_u84, (void *)&jni_u85, (void *)&jni_u86, (void *)&jni_u87,
  (void *)&jni_u88, (void *)&jni_u89, (void *)&jni_u90, (void *)&jni_u91, (void *)&jni_u92, (void *)&jni_u93, (void *)&jni_u94, (void *)&jni_u95,
  (void *)&jni_u96, (void *)&jni_u97, (void *)&jni_u98, (void *)&jni_u99, (void *)&jni_u100, (void *)&jni_u101, (void *)&jni_u102, (void *)&jni_u103,
  (void *)&jni_u104, (void *)&jni_u105, (void *)&jni_u106, (void *)&jni_u107, (void *)&jni_u108, (void *)&jni_u109, (void *)&jni_u110, (void *)&jni_u111,
  (void *)&jni_u112, (void *)&jni_u113, (void *)&jni_u114, (void *)&jni_u115, (void *)&jni_u116, (void *)&jni_u117, (void *)&jni_u118, (void *)&jni_u119,
  (void *)&jni_u120, (void *)&jni_u121, (void *)&jni_u122, (void *)&jni_u123, (void *)&jni_u124, (void *)&jni_u125, (void *)&jni_u126, (void *)&jni_u127,
  (void *)&jni_u128, (void *)&jni_u129, (void *)&jni_u130, (void *)&jni_u131, (void *)&jni_u132, (void *)&jni_u133, (void *)&jni_u134, (void *)&jni_u135,
  (void *)&jni_u136, (void *)&jni_u137, (void *)&jni_u138, (void *)&jni_u139, (void *)&jni_u140, (void *)&jni_u141, (void *)&jni_u142, (void *)&jni_u143,
  (void *)&jni_u144, (void *)&jni_u145, (void *)&jni_u146, (void *)&jni_u147, (void *)&jni_u148, (void *)&jni_u149, (void *)&jni_u150, (void *)&jni_u151,
  (void *)&jni_u152, (void *)&jni_u153, (void *)&jni_u154, (void *)&jni_u155, (void *)&jni_u156, (void *)&jni_u157, (void *)&jni_u158, (void *)&jni_u159,
  (void *)&jni_u160, (void *)&jni_u161, (void *)&jni_u162, (void *)&jni_u163, (void *)&jni_u164, (void *)&jni_u165, (void *)&jni_u166, (void *)&jni_u167,
  (void *)&jni_u168, (void *)&jni_u169, (void *)&jni_u170, (void *)&jni_u171, (void *)&jni_u172, (void *)&jni_u173, (void *)&jni_u174, (void *)&jni_u175,
  (void *)&jni_u176, (void *)&jni_u177, (void *)&jni_u178, (void *)&jni_u179, (void *)&jni_u180, (void *)&jni_u181, (void *)&jni_u182, (void *)&jni_u183,
  (void *)&jni_u184, (void *)&jni_u185, (void *)&jni_u186, (void *)&jni_u187, (void *)&jni_u188, (void *)&jni_u189, (void *)&jni_u190, (void *)&jni_u191,
  (void *)&jni_u192, (void *)&jni_u193, (void *)&jni_u194, (void *)&jni_u195, (void *)&jni_u196, (void *)&jni_u197, (void *)&jni_u198, (void *)&jni_u199,
  (void *)&jni_u200, (void *)&jni_u201, (void *)&jni_u202, (void *)&jni_u203, (void *)&jni_u204, (void *)&jni_u205, (void *)&jni_u206, (void *)&jni_u207,
  (void *)&jni_u208, (void *)&jni_u209, (void *)&jni_u210, (void *)&jni_u211, (void *)&jni_u212, (void *)&jni_u213, (void *)&jni_u214, (void *)&jni_u215,
  (void *)&jni_u216, (void *)&jni_u217, (void *)&jni_u218, (void *)&jni_u219, (void *)&jni_u220, (void *)&jni_u221, (void *)&jni_u222, (void *)&jni_u223,
  (void *)&jni_u224, (void *)&jni_u225, (void *)&jni_u226, (void *)&jni_u227, (void *)&jni_u228, (void *)&jni_u229, (void *)&jni_u230, (void *)&jni_u231,
  (void *)&jni_u232,
};

static const char *const *jni_slot_name_tab(void) { return jni_slot_names; }

static int64_t jni_unbound(int slot) {
  static unsigned char told[233];
  if (slot >= 0 && slot < 233 && !told[slot]) {
    told[slot] = 1;
    exec_log(EXEC_LOG_ERROR,
            "JNI slot %d (%s) is not implemented; returning 0", slot,
            jni_slot_name_tab()[slot]);
  }
  return 0;
}

static void  *env_vtbl[233];
static void  *env_ptr  = env_vtbl;
static void  *vm_vtbl[8];
static void  *vm_ptr   = vm_vtbl;

static int vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args; if (penv) *penv = &env_ptr; return 0;
}
static int vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static int vm_GetEnv(void *vm, void **penv, int ver) {
  (void)vm; (void)ver; if (penv) *penv = &env_ptr; return 0;
}
static int vm_DestroyJavaVM(void *vm) { (void)vm; jni_quit_requested = 1; return 0; }

void *jni_env(void) { return &env_ptr; }
void *jni_vm(void)  { return &vm_ptr;  }
void *jni_activity_class(void) { return class_obj[C_ACTIVITY]; }

void *jni_new_string(const char *utf8) {
  void *h = obj_new(OBJ_STRING); if (!h) return NULL;
  exec_obj *o = obj_of(h);
  o->str = strdup(utf8 ? utf8 : "");
  o->len = (int)strlen(o->str);
  return h;
}

void *jni_new_bytearray(const void *data, int len) {
  if (len < 0) len = 0;
  void *h = obj_new(OBJ_BYTEARRAY); if (!h) return NULL;
  exec_obj *o = obj_of(h);
  o->len  = len;
  o->data = calloc((size_t)len + 1, 1);
  if (data && len) memcpy(o->data, data, (size_t)len);
  return h;
}

void *jni_bytearray_data(void *arr, int *len_out) {
  exec_obj *o = obj_of(arr);
  if (!o || o->kind != OBJ_BYTEARRAY) { if (len_out) *len_out = 0; return NULL; }
  if (len_out) *len_out = o->len;
  return o->data;
}

void *jni_bitmap_of(void *jbitmap) {
  exec_obj *o = obj_of(jbitmap);
  return (o && o->kind == OBJ_BITMAP) ? o->data : NULL;
}

void *jni_asset_manager(void) {
  static void *h;
  if (!h) { h = obj_new(OBJ_ASSETMGR); }
  return h;
}

void jni_init(void) {
  /* Defaults first, then the implemented slots on top.
   *
   * THE INDICES ARE NOT HAND-COUNTED. They come from walking the
   * JNINativeInterface layout in tools/check_jni_slots.py, which builds the
   * same table from the spec's own structure -- the Call<T>Method families
   * are ten types by three forms, the array families are eight primitives,
   * and so on. Six of these were previously off by one from slot 192, and
   * the checker agreed with them because both were transcribed from the same
   * notes. Now only one of them counts, and the other verifies. */
  for (int i = 0; i < 233; i++) env_vtbl[i] = jni_default_tab[i];

  env_vtbl[  4] = (void *)&jni_GetVersion;                    /* GetVersion */
  env_vtbl[  6] = (void *)&jni_FindClass;                     /* FindClass */
  env_vtbl[ 15] = (void *)&jni_ExceptionOccurred;             /* ExceptionOccurred */
  env_vtbl[ 16] = (void *)&jni_ExceptionDescribe;             /* ExceptionDescribe */
  env_vtbl[ 17] = (void *)&jni_ExceptionClear;                /* ExceptionClear */
  env_vtbl[ 19] = (void *)&jni_PushLocalFrame;                /* PushLocalFrame */
  env_vtbl[ 20] = (void *)&jni_PopLocalFrame;                 /* PopLocalFrame */
  env_vtbl[ 21] = (void *)&jni_NewGlobalRef;                  /* NewGlobalRef */
  env_vtbl[ 22] = (void *)&jni_DeleteGlobalRef;               /* DeleteGlobalRef */
  env_vtbl[ 23] = (void *)&jni_DeleteLocalRef;                /* DeleteLocalRef */
  env_vtbl[ 24] = (void *)&jni_IsSameObject;                  /* IsSameObject */
  env_vtbl[ 25] = (void *)&jni_NewLocalRef;                   /* NewLocalRef */
  env_vtbl[ 26] = (void *)&jni_EnsureLocalCapacity;           /* EnsureLocalCapacity */
  env_vtbl[ 31] = (void *)&jni_GetObjectClass;                /* GetObjectClass */
  env_vtbl[ 33] = (void *)&jni_GetMethodID;                   /* GetMethodID */
  env_vtbl[113] = (void *)&jni_GetStaticMethodID;             /* GetStaticMethodID */
  env_vtbl[114] = (void *)&jni_CallStaticObjectMethod;        /* CallStaticObjectMethod */
  env_vtbl[115] = (void *)&jni_CallStaticObjectMethodV;       /* CallStaticObjectMethodV */
  env_vtbl[117] = (void *)&jni_CallStaticBooleanMethod;       /* CallStaticBooleanMethod */
  env_vtbl[118] = (void *)&jni_CallStaticBooleanMethodV;      /* CallStaticBooleanMethodV */
  env_vtbl[129] = (void *)&jni_CallStaticIntMethod;           /* CallStaticIntMethod */
  env_vtbl[130] = (void *)&jni_CallStaticIntMethodV;          /* CallStaticIntMethodV */
  env_vtbl[141] = (void *)&jni_CallStaticVoidMethod;          /* CallStaticVoidMethod */
  env_vtbl[142] = (void *)&jni_CallStaticVoidMethodV;         /* CallStaticVoidMethodV */
  env_vtbl[163] = (void *)&jni_NewStringUTF;                  /* NewString */
  env_vtbl[167] = (void *)&jni_NewStringUTF;                  /* NewStringUTF */
  env_vtbl[168] = (void *)&jni_GetStringUTFLength;            /* GetStringUTFLength */
  env_vtbl[169] = (void *)&jni_GetStringUTFChars;             /* GetStringUTFChars */
  env_vtbl[170] = (void *)&jni_ReleaseStringUTFChars;         /* ReleaseStringUTFChars */
  env_vtbl[171] = (void *)&jni_GetArrayLength;                /* GetArrayLength */
  env_vtbl[173] = (void *)&jni_GetObjectArrayElement;         /* GetObjectArrayElement */
  env_vtbl[174] = (void *)&jni_SetObjectArrayElement;         /* SetObjectArrayElement */
  env_vtbl[176] = (void *)&jni_NewByteArray;                  /* NewByteArray */
  env_vtbl[179] = (void *)&jni_NewIntArray;                   /* NewIntArray */
  env_vtbl[184] = (void *)&jni_GetByteArrayElements;          /* GetByteArrayElements */
  env_vtbl[187] = (void *)&jni_GetIntArrayElements;           /* GetIntArrayElements */
  env_vtbl[192] = (void *)&jni_ReleaseByteArrayElements;      /* ReleaseByteArrayElements */
  env_vtbl[195] = (void *)&jni_ReleaseIntArrayElements;       /* ReleaseIntArrayElements */
  env_vtbl[200] = (void *)&jni_GetByteArrayRegion;            /* GetByteArrayRegion */
  env_vtbl[208] = (void *)&jni_SetByteArrayRegion;            /* SetByteArrayRegion */
  env_vtbl[211] = (void *)&jni_SetIntArrayRegion;             /* SetIntArrayRegion */
  env_vtbl[222] = (void *)&jni_GetByteArrayElements;          /* GetPrimitiveArrayCritical */
  env_vtbl[223] = (void *)&jni_ReleaseByteArrayElements;      /* ReleasePrimitiveArrayCritical */
  env_vtbl[219] = (void *)&jni_GetJavaVM;                     /* GetJavaVM */
  env_vtbl[228] = (void *)&jni_ExceptionCheck;                /* ExceptionCheck */

  vm_vtbl[3] = (void *)&vm_DestroyJavaVM;
  vm_vtbl[4] = (void *)&vm_AttachCurrentThread;
  vm_vtbl[5] = (void *)&vm_DetachCurrentThread;
  vm_vtbl[6] = (void *)&vm_GetEnv;
  vm_vtbl[7] = (void *)&vm_AttachCurrentThread;   /* AsDaemon */

  for (int i = 1; i < C_COUNT; i++) {
    class_obj[i] = obj_new(OBJ_CLASS);
    obj_of(class_obj[i])->cls  = i;
    obj_of(class_obj[i])->refs = 1;
  }

}

/* SPLIT FROM jni_init ON PURPOSE.
 *
 * These six live in libexecutive_android.so, so they cannot be looked up until the
 * module is loaded and mapped. jni_init has to run earlier -- the vtables and
 * class objects it builds are needed to construct the arguments nativeInit
 * takes -- and an earlier version did the binding there too. On a module with
 * no symbol table yet, so_try_find_addr_rx loops zero times and returns 0, so
 * all six became NULL with no error anywhere.
 *
 * Nothing crashed: every call site is guarded. The cloud-save flow simply
 * never completed. CloudSaveBridge.read() would do the file read, and the
 * callback that tells the engine the read finished never fired, so its
 * operationInFlight flag stayed set and the save system wedged for the rest
 * of the session. A silent hang is worse than a crash, and it would have been
 * very hard to trace back to here. */
void jni_bind_natives(void) {
#define BIND(fp, sym)                                                        \
  do {                                                                       \
    *(uintptr_t *)&(fp) =                                                    \
        exec_find_export("Java_com_rivermanmedia_theexecutive_" sym);     \
    if (!(fp)) exec_log(EXEC_LOG_ERROR, "JNI callback %s did not resolve", sym); \
  } while (0)

  BIND(n_read_succeeded, "CloudSaveBridge_nativeReadSucceeded");
  BIND(n_read_missing,   "CloudSaveBridge_nativeReadMissing");
  BIND(n_read_failed,    "CloudSaveBridge_nativeReadFailed");
  BIND(n_write_finished, "CloudSaveBridge_nativeWriteFinished");
  BIND(n_score_posted,   "PlayGamesBridge_nativeScorePosted");
  BIND(n_ach_posted,     "PlayGamesBridge_nativeAchievementPosted");
#undef BIND
}