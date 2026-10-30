#ifndef DS4_WIN32_PTHREAD_H
#define DS4_WIN32_PTHREAD_H

/* Deliberately limited to ds4's pthread subset; no cancellation or shared locks. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stddef.h>
#include <time.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ds4_win32_thread;
typedef struct {
    struct ds4_win32_thread *control;
    DWORD id;
} pthread_t;

enum {
    PTHREAD_CREATE_JOINABLE = 0,
    PTHREAD_CREATE_DETACHED = 1,
    PTHREAD_PROCESS_PRIVATE = 0,
    PTHREAD_PROCESS_SHARED = 1,
    PTHREAD_MUTEX_NORMAL = 0,
    PTHREAD_MUTEX_RECURSIVE = 1,
    PTHREAD_MUTEX_ERRORCHECK = 2,
    PTHREAD_MUTEX_DEFAULT = PTHREAD_MUTEX_NORMAL
};

#define PTHREAD_STACK_MIN ((size_t)65536)

typedef struct {
    size_t stacksize; /* Zero selects the executable's default stack reserve. */
    int detachstate;
    int valid;
} pthread_attr_t;

typedef struct {
    int type;
    int valid;
} pthread_mutexattr_t;

typedef struct {
    SRWLOCK native;
    volatile LONG owner;
    unsigned depth;
    int type;
    int valid;
} pthread_mutex_t;

typedef struct {
    int valid;
} pthread_condattr_t;

typedef struct {
    CONDITION_VARIABLE native;
    volatile LONG waiters;
    int valid;
} pthread_cond_t;

typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
#define PTHREAD_MUTEX_INITIALIZER { SRWLOCK_INIT, 0, 0, PTHREAD_MUTEX_DEFAULT, 1 }
#define PTHREAD_COND_INITIALIZER { CONDITION_VARIABLE_INIT, 0, 1 }

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t left, pthread_t right);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared);
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_setpshared(pthread_condattr_t *attr, int shared);
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
/* Absolute CLOCK_REALTIME deadline, including when already expired. */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                         const struct timespec *deadline);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_once(pthread_once_t *once, void (*init)(void));

#ifdef __cplusplus
}
#endif
#endif
