/* android_native.h -- forwarder.
 *
 * libc_shim.c is reused unmodified from the reference ports and includes this
 * header by name. It originally declared a NativeActivity's Java-side glue;
 * libexecutive_android.so is a GLSurfaceView title and imports no ANativeActivity,
 * ALooper, AInputQueue or ANativeWindow symbol at all, so there is nothing to
 * declare. This exists only so the include resolves.
 *
 * Deleting it was a real bug during this port's bring-up: the file looked
 * unused because nothing in `source/` referenced the name, and the reference
 * to it lives inside a file that is deliberately never edited.
 */
#ifndef EXEC_FWD_ANDROID_NATIVE_H
#define EXEC_FWD_ANDROID_NATIVE_H
#include "compat_stubs.h"
#endif
