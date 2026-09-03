/* exec_shim.h -- the handful of bionic entry points with no newlib equivalent. */
#ifndef EXEC_SHIM_H
#define EXEC_SHIM_H
#include <stdio.h>
#include <stdint.h>

extern FILE *exec_stdout_ptr;    /* bionic exports stdout as a FILE* object */

int      exec_system_property_get(const char *name, char *value);
unsigned long exec_getauxval(unsigned long type);
int      exec_register_atfork(void (*p)(void), void (*c)(void), void (*x)(void), void *d);
void     exec_cxa_finalize(void *dso);
void    *exec_dlopen(const char *name, int flags);
void    *exec_dlsym(void *h, const char *name);
int      exec_dlclose(void *h);
char    *exec_dlerror(void);
void     exec_shim_init(void);

/* libc++_shared.so imports these two; devkitA64's newlib has neither.
 * Nothing in the filesystem layer here is a device or a socket, so there is
 * no correct behaviour to implement -- both fail rather than pretend. */
int      exec_ioctl(int fd, unsigned long req, ...);
long     exec_sendfile(int out_fd, int in_fd, long *offset, size_t count);

/* The *at() family, also from std::filesystem. devkitA64's newlib has
 * fchmodat, fdopendir and utimensat in its headers but not in libc.a, so the
 * identity mapping compiles and then fails at link -- which is exactly how
 * these were found. See the .c file for what each one returns and why. */
int      exec_fchmodat(int dirfd, const char *path, unsigned mode, int flags);
void    *exec_fdopendir(int fd);
int      exec_utimensat(int dirfd, const char *path, const void *times, int flags);
#endif
