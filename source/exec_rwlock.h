/* exec_rwlock.h -- see exec_rwlock.c. MIT licensed. */
#ifndef EXEC_RWLOCK_H
#define EXEC_RWLOCK_H
int exec_rwlock_rdlock(void *rw);
int exec_rwlock_wrlock(void *rw);
int exec_rwlock_tryrdlock(void *rw);
int exec_rwlock_trywrlock(void *rw);
int exec_rwlock_unlock(void *rw);
int exec_rwlock_init(void *rw, const void *attr);
int exec_rwlock_destroy(void *rw);
#endif
