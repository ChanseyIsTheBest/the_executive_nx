/* sj_trace.h -- forwarder.
 *
 * libc_shim.c is reused unmodified from the reference ports and includes this
 * header by name. This port needs none of what it originally declared: there
 * is no APK asset pack (assets/ is a real directory on the SD card),
 * libexecutive_android.so imports no socket symbols, and nothing opens a fake
 * descriptor. The declarations live in compat_stubs.h and this exists only so
 * the include resolves -- keeping libc_shim.c untouched is what lets fixes be
 * pulled back from upstream.
 */
#ifndef EXEC_FWD_SJ_TRACE_H
#define EXEC_FWD_SJ_TRACE_H
#include "compat_stubs.h"
#endif
