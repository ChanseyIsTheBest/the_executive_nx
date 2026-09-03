/* main.c -- The Executive wrapper for Nintendo Switch
 *
 * MIT licensed. See LICENSE. Ships no game code and no game assets.
 *
 * WHY THIS FILE DRIVES THE GAME INSTEAD OF HOSTING IT
 * ---------------------------------------------------
 * libexecutive_android.so exports no ANativeActivity_onCreate and no android_main.
 * It is a GLSurfaceView + JNI title: Java owned the window, the EGL context,
 * the frame loop and the input, and called down. So the wrapper takes the
 * Java side's job. Nothing here is a callback from the engine; every line
 * below is something MainActivity or PVSRenderer used to do.
 *
 * TWO MODULES, AND THE ORDER IS NOT OPTIONAL
 * ------------------------------------------
 * Unlike the Osmos port, which loads one self-contained library, this needs
 * libc++_shared.so as well: libexecutive_android.so leaves 67 symbols undefined
 * that only libc++_shared exports, and libc++_shared has 161 imports of its
 * own. so_resolve_symbol already prefers our shim table and falls back to
 * other loaded modules, so the whole requirement is: load and finalise
 * libc++_shared FIRST, then libexecutive_android resolves against it.
 *
 * Both are linked BIND_NOW, so a gap in the table is a load-time abort
 * rather than a crash somewhere later. That is the behaviour we want.
 *
 * THE BRING-UP SEQUENCE
 * ---------------------
 * MainActivity.onCreate, in its own order:
 *
 *     nativeInit(getAssets(), getFilesDir().getAbsolutePath())
 *     ExecutiveAudio.setDebug(...)
 *     applyNativeEnvironmentExtras()   -> nativeSetEnv(k, v) per QA key
 *     ExecutiveAudio.init(this)
 *     PlayGamesBridge.initialize(this)
 *     CloudSaveBridge.initialize(this)
 *     new PVSView(this)                -> GLSurfaceView, EGL context v2
 *
 * and then, on the GL thread only:
 *
 *     onSurfaceCreated  -> nativeSurfaceCreated()
 *     onSurfaceChanged  -> nativeSurfaceChanged(w, h)
 *     onDrawFrame       -> nativeDrawFrame()          every frame
 *
 * nativeInit comes before everything, including the GL context: it is where
 * AAssetManager_fromJava is called and the files dir is stashed, and the
 * engine's constructor RMSystemAndroid(AAssetManager*, string, JavaVM*) runs
 * off it. Creating the context first and calling nativeInit second happens to
 * work on Android and is not what the game does.
 *
 * EVERY NATIVE CALL IS ON ONE THREAD
 * ----------------------------------
 * PVSView.onTouchEvent does not call the natives. It queues them with
 * GLSurfaceView.queueEvent, so they run on the render thread between frames.
 * The same is true of key events via queueNativeKey. So input is drained at
 * the top of the frame here rather than dispatched from wherever libnx
 * noticed it, and the cloud-save callbacks are deferred a frame for the same
 * reason -- see jni_pump_deferred.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "so_util.h"
#include "imports.h"
#include "util.h"
#include "error.h"
#include "exec_entry.h"
#include "exec_jni.h"
#include "exec_paths.h"
#include "exec_asset.h"
#include "exec_audio.h"
#include "exec_save.h"
#include "exec_shim.h"
#include "exec_input.h"
#include "nx_pointer.h"
#include "exec_log.h"
#include "exec_io.h"
#include "exec_diag.h"
#include "exec_bionic.h"
#include "config.h"

/* THE WINDOW IS 1280x720 UNLESS YOU ASK OTHERWISE
 *
 * libnx's default NWindow is 1280x720. Creating the EGL surface from it and
 * then telling the engine 1920x1080 is exactly what this port shipped, and it
 * looks like the game is zoomed in: the engine sets glViewport(0,0,1920,1080)
 * on a 1280x720 framebuffer, so the top-left 67% of the frame fills the
 * screen and everything else is scissored away.
 *
 * nwindowSetDimensions has to happen BEFORE eglCreateWindowSurface -- it sets
 * the dimensions of the buffers the surface will be built from, and afterwards
 * it is too late. And whatever comes back is then read out of EGL rather than
 * assumed, so the engine and the input mapping are told the truth even if the
 * request was not honoured.
 *
 * 1080p in both handheld and docked keeps the UI from being rebuilt on a dock;
 * the compositor scales down in handheld. */
#define RENDER_W EXEC_RENDER_W
#define RENDER_H EXEC_RENDER_H

static int g_surf_w = RENDER_W, g_surf_h = RENDER_H;

#define CXX_LOAD_SIZE  (8  * 1024 * 1024)
#define EXEC_LOAD_SIZE  (48 * 1024 * 1024)

static so_module cxx_mod, exec_mod;
static uint8_t  *cxx_area, *exec_area;

exec_entry exec;

/* exec_jni.c binds the CloudSave/PlayGames callbacks through this. */
uintptr_t exec_find_export(const char *name) {
  return so_try_find_addr_rx(&exec_mod, name);
}

/* ------------------------------------------------------------------ */
/* entry points                                                       */
/* ------------------------------------------------------------------ */

#define PFX "Java_com_rivermanmedia_theexecutive_MainActivity_"

int exec_entry_bind(void) {
  int missing = 0;
#define B(field, sym)                                                     \
  do {                                                                    \
    uintptr_t a = so_try_find_addr_rx(&exec_mod, PFX sym);                 \
    if (!a) { exec_log(EXEC_LOG_ERROR, "missing entry point %s", sym); missing++; } \
    *(uintptr_t *)&exec.field = a;                                         \
  } while (0)

  B(init,                  "nativeInit");
  B(set_env,               "nativeSetEnv");
  B(surface_created,       "nativeSurfaceCreated");
  B(surface_changed,       "nativeSurfaceChanged");
  B(draw_frame,            "nativeDrawFrame");
  B(reset_frame_scheduler, "nativeResetFrameScheduler");
  B(pointer_down,          "nativePointerDown");
  B(pointer_move,          "nativePointerMove");
  B(pointer_up,            "nativePointerUp");
  B(key_pressed,           "nativeKeyPressed");
  B(key_released,          "nativeKeyReleased");
  B(accel_changed,         "nativeAccelerometerChanged");
#undef B
  return missing ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* EGL                                                                */
/* ------------------------------------------------------------------ */

static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLSurface egl_surf = EGL_NO_SURFACE;
static EGLContext egl_ctx  = EGL_NO_CONTEXT;

static int egl_setup(void) {
  egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl_dpy == EGL_NO_DISPLAY) return -1;
  if (!eglInitialize(egl_dpy, NULL, NULL)) return -2;
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return -3;

  /* PVSView asks for setEGLContextClientVersion(2) and nothing else, so the
   * config is the default one a GLSurfaceView would have picked: RGB565 or
   * better with a depth buffer. An 8888 config with alpha is requested here
   * because the engine clears to an opaque colour anyway and the Osmos port
   * lost a day to a surface whose alpha channel was being composited. */
  static const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg; EGLint n = 0;
  if (!eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &n) || n < 1) return -4;

  NWindow *win = nwindowGetDefault();

  if (R_FAILED(nwindowSetDimensions(win, RENDER_W, RENDER_H)))
    exec_log(EXEC_LOG_WARN, "nwindowSetDimensions(%d, %d) failed; using the "
                          "window's own size", RENDER_W, RENDER_H);

  egl_surf = eglCreateWindowSurface(egl_dpy, cfg, win, NULL);
  if (egl_surf == EGL_NO_SURFACE) return -5;

  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (egl_ctx == EGL_NO_CONTEXT) return -6;

  if (!eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) return -7;
  eglSwapInterval(egl_dpy, 1);

  /* Ask, do not assume. */
  EGLint w = 0, h = 0;
  if (eglQuerySurface(egl_dpy, egl_surf, EGL_WIDTH, &w) &&
      eglQuerySurface(egl_dpy, egl_surf, EGL_HEIGHT, &h) && w > 0 && h > 0) {
    g_surf_w = (int)w;
    g_surf_h = (int)h;
  }
  exec_log(EXEC_LOG_INFO, "EGL surface is %dx%d (asked for %dx%d), %s",
          g_surf_w, g_surf_h, RENDER_W, RENDER_H,
          appletGetOperationMode() == AppletOperationMode_Console
            ? "docked" : "handheld");
  if (g_surf_w != RENDER_W || g_surf_h != RENDER_H)
    exec_log(EXEC_LOG_WARN, "the surface is not the requested size; the engine "
                          "and the pointer are being told %dx%d",
            g_surf_w, g_surf_h);
  if (g_surf_w > 1280 && appletGetOperationMode() != AppletOperationMode_Console)
    exec_log(EXEC_LOG_INFO, "handheld at %dx%d: the panel is 1280x720, so this "
                          "is supersampled", g_surf_w, g_surf_h);
  return 0;
}

static void egl_teardown(void) {
  if (egl_dpy == EGL_NO_DISPLAY) return;
  eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (egl_ctx)  eglDestroyContext(egl_dpy, egl_ctx);
  if (egl_surf) eglDestroySurface(egl_dpy, egl_surf);
  eglTerminate(egl_dpy);
  egl_dpy = EGL_NO_DISPLAY;
}

/* ------------------------------------------------------------------ */
/* module loading                                                     */
/* ------------------------------------------------------------------ */

/* error_screen takes a plain string, so anything with a value in it is
 * formatted here first. */
static char g_msg[1024];

static int load_module(so_module *mod, const char *path, uint8_t **area,
                       size_t size, const char *what) {
  *area = memalign(0x1000, size);
  if (!*area) {
    snprintf(g_msg, sizeof(g_msg), "Out of memory reserving %s.", what);
    error_screen(g_msg);
    return -1;
  }
  if (so_load(mod, path, *area, size) < 0) {
    snprintf(g_msg, sizeof(g_msg), "Could not load %s.\n\n  %s", what, path);
    error_screen(g_msg);
    return -1;
  }

  /* so_relocate BEFORE so_resolve, and neither is optional.
   *
   * so_load only reads the image in; every pointer in it is still a
   * link-time address. so_relocate applies R_AARCH64_RELATIVE and rebases
   * GLOB_DAT/JUMP_SLOT entries whose symbol is defined inside this module --
   * which so_resolve deliberately skips, because it only handles SHN_UNDEF.
   *
   * Leaving it out is not a subtle failure but it is a confusing one: the
   * .got.plt entries keep their initial value, which is the address of PLT0,
   * and the first call through any intra-module PLT stub branches to a small
   * absolute address belonging to no mapped region. This port shipped a build
   * that did exactly that and took an Instruction Abort at 0x128b90 --
   * libc++_shared.so's link-time .plt, reached from nativeInit's first
   * std::string assignment.
   *
   * The order matters too: so_relocate writes r_addend into ABS64 slots whose
   * symbol is undefined, leaving them for so_resolve to finish. Running
   * resolve first would have that overwritten. */
  if (so_relocate(mod) < 0) {
    snprintf(g_msg, sizeof(g_msg), "so_relocate failed for %s.", what);
    error_screen(g_msg);
    return -1;
  }

  /* taint_missing_imports = 1: an unresolved symbol becomes a deliberate
   * fault at a recognisable address instead of a jump to zero. */
  if (so_resolve(mod, dynlib_functions, (int)dynlib_numfunctions, 1) < 0) {
    snprintf(g_msg, sizeof(g_msg),
             "so_resolve failed for %s.\n\n  Run: make check", what);
    error_screen(g_msg);
    return -1;
  }

  /* so_patch_stack_canaries is deliberately NOT called. install_bionic_tls
   * gives each thread a real bionic TLS block, so the engine's own guard
   * checks pass on their own; neither reference port patches them, and one
   * documents that NOPing 2000+ b.ne sites risks matching branches that are
   * not canary checks at all. If it is ever enabled it must run here, before
   * so_finalize: it writes to .text, and the kernel never permits a W->X
   * transition on code memory once mapped. */

  /* so_load only RESERVES the virtual range. so_finalize performs the
   * svcMapProcessCodeMemory that aliases load_base to load_virtbase, so
   * so_flush_caches -- which touches load_virtbase directly -- must come
   * after it, not before. */
  so_finalize(mod);
  so_flush_caches(mod);
  return 0;
}

/* ------------------------------------------------------------------ */

static void set_env_str(const char *k, const char *v) {
  if (!exec.set_env) return;
  void *jk = jni_new_string(k), *jv = jni_new_string(v);
  exec.set_env(jni_env(), jni_activity_class(), jk, jv);
}

int main(int argc, char *argv[]) {
  /* No socketInitializeDefault and no romfsInit.
   *
   * Neither loaded module imports a single socket symbol -- checked against
   * both .dynsym tables -- and this port has no romfs. socketInitializeDefault
   * in particular reserves a multi-megabyte transfer buffer up front and is
   * one more thing that can fail before anything useful has happened. */

  io_init();
  diag_init();
  diag_thread_register((const void *)(uintptr_t)&main, 1);

  /* exec_paths_init says WHICH file is missing; saying so beats making
   * someone check three things when only one is wrong. */
  const int perr = exec_paths_init(argc > 0 ? argv[0] : NULL);
  if (perr) {
    const char *what =
        (perr == -1) ? "libexecutive_android.so is missing (APK: lib/arm64-v8a/)" :
        (perr == -2) ? "libc++_shared.so is missing (APK: lib/arm64-v8a/)"  :
                       "assets/ is missing (APK: the whole assets/ folder)";
    snprintf(g_msg, sizeof(g_msg),
      "Game files not found.\n\n"
      "  %s\n\n"
      "All three go next to the .nro:\n"
      "  libexecutive_android.so\n"
      "  libc++_shared.so\n"
      "  assets/\n\n"
      "Looked in: %s\n\n"
      "tools/prepare_game.sh lays this out from your APKs.", what, exec_dir());
    error_screen(g_msg);
    return 0;
  }

  char logpath[600];
  snprintf(logpath, sizeof(logpath), "%s/executive_nx.log", exec_dir());
  log_init(logpath);
  exec_log(EXEC_LOG_INFO, "executive_nx starting, dir=%s", exec_dir());

  exec_config_load(exec_dir());
  exec_shim_init();
  exec_asset_init(exec_assets_dir());
  exec_save_init(exec_files_dir());
  jni_init();

  /* The arena has to exist before anything can reach mmap. g_mmap_big_align
   * is a divisor inside libc_shim's mmap scan, so leaving it zero is an
   * infinite loop with no syscall and no allocation -- which presents as
   * 100% CPU and a black screen. See compat_stubs.h. */
  exec_mmap_arena_init();

  /* install_bionic_tls before any module code runs: the loaded modules read
   * their stack canary from TPIDR_EL0+0x28, which is not where newlib keeps
   * anything. Each thread needs its own block; this one is main's. See util.c
   * and the -mtp=soft note in the Makefile. */
  static uint8_t main_tls[BIONIC_TLS_SIZE];
  install_bionic_tls(main_tls);

  if (load_module(&cxx_mod, exec_cxx_path(), &cxx_area, CXX_LOAD_SIZE,
                  "libc++_shared.so") < 0) return 0;
  if (load_module(&exec_mod, exec_lib_path(), &exec_area, EXEC_LOAD_SIZE,
                  "libexecutive_android.so") < 0) return 0;

  /* Static initialisers, after both modules are mapped and resolved:
   * libc++_shared's own globals have to be constructed before anything the
   * game's .init_array touches can use them. Kept out of load_module for
   * exactly that reason -- running libc++'s init_array while libexecutive is still
   * unrelocated would be the same class of bug in the other direction. */
  /* Log where each module landed.
   *
   * An Atmosphère crash report gives absolute addresses and a module list
   * that, for manually mapped code, has been wrong or duplicated every time
   * so far. With these two lines in the log, `addr - base` is immediate and
   * the faulting function can be named from the .so's own symbol table.
   * Both of this port's crashes were diagnosed by doing that subtraction by
   * hand; the numbers may as well be written down. */
  exec_log(EXEC_LOG_INFO, "module libc++_shared.so at %p (%zu bytes)",
          cxx_mod.load_virtbase, cxx_mod.load_size);
  exec_log(EXEC_LOG_INFO, "module libexecutive_android.so at %p (%zu bytes)",
          exec_mod.load_virtbase, exec_mod.load_size);

  so_execute_init_array(&cxx_mod);
  so_execute_init_array(&exec_mod);

  /* Only now can the bridge callbacks be resolved: they live in the module
   * that was just mapped. See jni_bind_natives. */
  jni_bind_natives();

  if (exec_entry_bind() < 0) {
    error_screen("libexecutive_android.so is missing native entry points.\n\n"
                 "  This build expects The Executive 1.1.0.\n"
                 "  See executive_nx.log for which ones.");
    return 0;
  }

  /* JNI_OnLoad is exported and the real runtime would have called it before
   * any native ran. It is 24 bytes and only stashes the JavaVM, but calling
   * it is free and skipping it is the kind of omission that costs an evening. */
  int (*jni_onload)(void *vm, void *reserved) =
      (void *)so_try_find_addr_rx(&exec_mod, "JNI_OnLoad");
  if (jni_onload) jni_onload(jni_vm(), NULL);

  /* Input before the first engine call. nxp_init reads cursor.png off the SD
   * card and writes pointer.cfg, and the engine starts spawning workers
   * inside nativeInit -- the reference ports both do this early for the same
   * reason. Nothing here calls into the engine, so it is safe this side of
   * the entry points being bound. */
  /* The surface size is not known until EGL has been set up, and input has to
   * be ready before the engine spawns workers -- so the pointer is told the
   * requested size here and corrected below if EGL gave us something else. */
  exec_input_init(RENDER_W, RENDER_H);

  /* ---- onCreate, in order ---- */
  exec.init(jni_env(), jni_activity_class(),
           jni_asset_manager(), jni_new_string(exec_files_dir()));

  /* The engine ships its own instrumentation and MainActivity turned it on
   * from Intent extras. Nothing else here has to be built: PVS_DEBUG_INPUT,
   * PVS_DEBUG_AUDIO, PVS_DEBUG_FRAME_TIMING, PVS_DEBUG_DWARP,
   * PVS_DEBUG_LANGUAGE, PVS_DEBUG_GAMECENTER and the PVS_AUTOMATION_* keys
   * are all read by the engine itself. config.txt exposes them. */
  exec_config_apply_env(set_env_str);

  if (exec_audio_init() < 0)
    exec_log(EXEC_LOG_WARN, "audio init failed -- continuing silently");

  /* PlayGamesBridge.initialize / CloudSaveBridge.initialize had no native
   * side; they only wired up Java state. Their slot in the order is kept
   * so this reads against onCreate line for line. */

  if (egl_setup() < 0) {
    error_screen("could not create an OpenGL ES 2.0 context");
    return 0;
  }

  /* From here the console cannot be opened without giving the window back,
   * and so_util.c's fatal_error() can fire at any time during bring-up. */
  error_set_gfx_release(egl_teardown);

  /* ---- the GL thread ---- */
  exec.surface_created(jni_env(), jni_activity_class());
  exec.surface_changed(jni_env(), jni_activity_class(), g_surf_w, g_surf_h);
  exec_input_set_surface(g_surf_w, g_surf_h);
  if (exec.reset_frame_scheduler)
    exec.reset_frame_scheduler(jni_env(), jni_activity_class());

  /* A crash loses whatever is still in the log buffer, and a crash is exactly
   * when the log matters. Once a second is frequent enough to keep the tail
   * meaningful and rare enough not to matter: the flush is one SD write
   * against sixty frames. */
  unsigned frame = 0;

  appletLockExit();
  while (appletMainLoop() && !jni_quit_requested) {
    /* Order matches GLSurfaceView: queued events drain, then onDrawFrame. */
    jni_pump_deferred();
    exec_input_poll();
    exec.draw_frame(jni_env(), jni_activity_class());
    exec_audio_update();
    nxp_draw();                 /* cursor on top of the finished frame */
    eglSwapBuffers(egl_dpy, egl_surf);
    if ((++frame % 60) == 0) exec_log_flush();
  }
  appletUnlockExit();

  /* The raw ELF images are held until here. mod->syms and mod->dynstrtab
   * point into load_base and survive so_free_temp, but elf_hdr, sec_hdr and
   * shstrtab point into the buffer it releases, so freeing early risks a
   * use-after-free during any late symbol lookup. */
  so_free_temp(&exec_mod);
  so_free_temp(&cxx_mod);

  exec_input_exit();
  exec_audio_exit();
  egl_teardown();
  exec_log_close();
  return 0;
}
