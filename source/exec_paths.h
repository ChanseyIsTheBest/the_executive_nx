/* exec_paths.h -- where the game's files live.
 *
 * The .nro can sit in any folder under sdmc:/switch/, so the layout is found
 * at runtime relative to the running program rather than hardcoded.
 */
#ifndef EXEC_PATHS_H
#define EXEC_PATHS_H
int         exec_paths_init(const char *argv0);
const char *exec_dir(void);          /* folder holding the .nro          */
const char *exec_assets_dir(void);   /* <dir>/assets                     */
const char *exec_files_dir(void);    /* <dir>/files -- Android filesDir  */
const char *exec_lib_path(void);     /* <dir>/libexecutive_android.so          */
const char *exec_cxx_path(void);     /* <dir>/libc++_shared.so           */
#endif
