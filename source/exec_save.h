/* exec_save.h -- backing for CloudSaveBridge.
 *
 * The engine treats cloud save as an async request/callback pair, so these
 * are synchronous only in the sense that the callback fires before the call
 * returns. Return values: 1 = present, 0 = absent, -1 = error. The
 * distinction matters -- nativeReadMissing and nativeReadFailed put the
 * engine into different states.
 */
#ifndef EXEC_SAVE_H
#define EXEC_SAVE_H
void exec_save_init(const char *dir);
int  exec_save_read(const char *key, void **buf, int *len);
int  exec_save_write(const char *key, const void *buf, int len);
#endif
