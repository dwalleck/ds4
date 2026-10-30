/* ds4-owned C implementation, not the vendored gufo pthread stub. */
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>

enum { THREAD_JOINABLE, THREAD_DETACHED, THREAD_JOINING };

struct ds4_win32_thread {
    SRWLOCK startup;
    HANDLE handle;
    DWORD id;
    volatile LONG refs;
    volatile LONG state;
    void *(*start)(void *);
    void *arg;
    void *result;
};

static __declspec(thread) struct ds4_win32_thread *current_thread;

static int win32_error(DWORD error) {
    switch (error) {
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_INVALID_HANDLE: return ESRCH;
    case ERROR_TIMEOUT: return ETIMEDOUT;
    default: return EINVAL;
    }
}

static void thread_release(struct ds4_win32_thread *thread) {
    if (InterlockedDecrement(&thread->refs) == 0) {
        CloseHandle(thread->handle);
        free(thread);
    }
}

static unsigned __stdcall thread_start(void *arg) {
    struct ds4_win32_thread *thread = arg;
    /* Publish the handle and pthread_t before user code can detach itself. */
    AcquireSRWLockExclusive(&thread->startup);
    ReleaseSRWLockExclusive(&thread->startup);
    current_thread = thread;
    thread->result = thread->start(thread->arg);
    current_thread = NULL;
    thread_release(thread);
    /* Returning through _beginthreadex's trampoline performs CRT cleanup. */
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    attr->stacksize = 0;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    attr->valid = 1;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    if (!attr || !attr->valid) return EINVAL;
    attr->valid = 0;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
    if (!attr || !attr->valid || size < PTHREAD_STACK_MIN || size > UINT_MAX)
        return EINVAL;
    attr->stacksize = size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
    if (!attr || !attr->valid ||
        (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED))
        return EINVAL;
    attr->detachstate = state;
    return 0;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    if (!out || !start || (attr && !attr->valid)) return EINVAL;
    const int saved_errno = errno;
    struct ds4_win32_thread *thread = calloc(1, sizeof(*thread));
    if (!thread) {
        errno = saved_errno;
        return ENOMEM;
    }
    InitializeSRWLock(&thread->startup);
    thread->refs = 2; /* One running-thread reference and one join/detach owner. */
    thread->state = attr && attr->detachstate == PTHREAD_CREATE_DETACHED
                  ? THREAD_DETACHED : THREAD_JOINABLE;
    thread->start = start;
    thread->arg = arg;
    AcquireSRWLockExclusive(&thread->startup);
    unsigned id;
    const unsigned stacksize = attr ? (unsigned)attr->stacksize : 0;
    const uintptr_t handle = _beginthreadex(NULL, stacksize, thread_start, thread,
            stacksize ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, &id);
    if (!handle) {
        const int error = errno ? errno : EAGAIN;
        ReleaseSRWLockExclusive(&thread->startup);
        free(thread);
        errno = saved_errno;
        return error;
    }
    thread->handle = (HANDLE)handle;
    thread->id = (DWORD)id;
    out->control = thread;
    out->id = thread->id;
    const int detached = thread->state == THREAD_DETACHED;
    ReleaseSRWLockExclusive(&thread->startup);
    if (detached) thread_release(thread);
    errno = saved_errno;
    return 0;
}

pthread_t pthread_self(void) {
    const pthread_t self = {current_thread, GetCurrentThreadId()};
    return self;
}

int pthread_equal(pthread_t left, pthread_t right) {
    return left.id == right.id;
}

int pthread_join(pthread_t id, void **result) {
    if (id.id && id.id == GetCurrentThreadId()) return EDEADLK;
    struct ds4_win32_thread *thread = id.control;
    if (!thread || !id.id) return ESRCH;
    if (InterlockedCompareExchange(&thread->state, THREAD_JOINING,
                                   THREAD_JOINABLE) != THREAD_JOINABLE)
        return EINVAL;
    const DWORD status = WaitForSingleObject(thread->handle, INFINITE);
    if (status != WAIT_OBJECT_0) {
        const int error = win32_error(GetLastError());
        InterlockedExchange(&thread->state, THREAD_JOINABLE);
        return error;
    }
    /* Never use GetExitCodeThread: its DWORD would truncate a void * on ARM64. */
    if (result) *result = thread->result;
    thread_release(thread);
    return 0;
}

int pthread_detach(pthread_t id) {
    struct ds4_win32_thread *thread = id.control;
    if (!thread || !id.id) return ESRCH;
    if (InterlockedCompareExchange(&thread->state, THREAD_DETACHED,
                                   THREAD_JOINABLE) != THREAD_JOINABLE)
        return EINVAL;
    thread_release(thread);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    attr->type = PTHREAD_MUTEX_DEFAULT;
    attr->valid = 1;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    if (!attr || !attr->valid) return EINVAL;
    attr->valid = 0;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr || !attr->valid ||
        (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
         type != PTHREAD_MUTEX_ERRORCHECK)) return EINVAL;
    attr->type = type;
    return 0;
}

static int process_scope(int shared) {
    if (shared == PTHREAD_PROCESS_PRIVATE) return 0;
    return shared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared) {
    if (!attr || !attr->valid) return EINVAL;
    return process_scope(shared);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    if (!mutex || (attr && !attr->valid)) return EINVAL;
    InitializeSRWLock(&mutex->native);
    mutex->owner = 0;
    mutex->depth = 0;
    mutex->type = attr ? attr->type : PTHREAD_MUTEX_DEFAULT;
    mutex->valid = 1;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex || !mutex->valid) return EINVAL;
    if (!TryAcquireSRWLockExclusive(&mutex->native)) return EBUSY;
    mutex->valid = 0;
    ReleaseSRWLockExclusive(&mutex->native);
    return 0;
}

static int mutex_acquire(pthread_mutex_t *mutex, int try_only) {
    if (!mutex || !mutex->valid) return EINVAL;
    const LONG self = (LONG)GetCurrentThreadId();
    if (InterlockedCompareExchange(&mutex->owner, 0, 0) == self) {
        if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
            if (mutex->depth == UINT_MAX) return EAGAIN;
            mutex->depth++;
            return 0;
        }
        if (try_only) return EBUSY;
        if (mutex->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        /* Relocking a normal mutex has POSIX's normal deadlock semantics. */
    }
    if (try_only) {
        if (!TryAcquireSRWLockExclusive(&mutex->native)) return EBUSY;
    } else {
        AcquireSRWLockExclusive(&mutex->native);
    }
    mutex->depth = 1;
    InterlockedExchange(&mutex->owner, self);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    return mutex_acquire(mutex, 0);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    return mutex_acquire(mutex, 1);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex || !mutex->valid) return EINVAL;
    if (InterlockedCompareExchange(&mutex->owner, 0, 0) !=
        (LONG)GetCurrentThreadId()) return EPERM;
    if (--mutex->depth) return 0;
    InterlockedExchange(&mutex->owner, 0);
    ReleaseSRWLockExclusive(&mutex->native);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
    if (!attr) return EINVAL;
    attr->valid = 1;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    if (!attr || !attr->valid) return EINVAL;
    attr->valid = 0;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attr, int shared) {
    if (!attr || !attr->valid) return EINVAL;
    return process_scope(shared);
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    if (!cond || (attr && !attr->valid)) return EINVAL;
    InitializeConditionVariable(&cond->native);
    cond->waiters = 0;
    cond->valid = 1;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (!cond || !cond->valid) return EINVAL;
    if (InterlockedCompareExchange(&cond->waiters, 0, 0)) return EBUSY;
    cond->valid = 0;
    return 0;
}

/* Round UP to milliseconds; a relative Win32 wait must not time out early.
 * Long deadlines are split so INFINITE is never mistaken for a finite wait. */
static DWORD deadline_timeout(const struct timespec *deadline) {
    FILETIME filetime;
    ULARGE_INTEGER ticks;
    GetSystemTimePreciseAsFileTime(&filetime);
    ticks.LowPart = filetime.dwLowDateTime;
    ticks.HighPart = filetime.dwHighDateTime;
    const uint64_t unix_ticks = ticks.QuadPart - UINT64_C(116444736000000000);
    const int64_t seconds = (int64_t)(unix_ticks / UINT64_C(10000000));
    const long nanos = (long)(unix_ticks % UINT64_C(10000000)) * 100;
    if (deadline->tv_sec < seconds ||
        (deadline->tv_sec == seconds && deadline->tv_nsec <= nanos)) return 0;
    /* Compare before subtracting to avoid overflow for a far-future time_t. */
    if (deadline->tv_sec > seconds + (int64_t)(INFINITE - 1) / 1000)
        return INFINITE - 1;
    const int64_t delta = ((int64_t)deadline->tv_sec - seconds) * 1000000000
                        + deadline->tv_nsec - nanos;
    const uint64_t millis = ((uint64_t)delta + 999999) / 1000000;
    return millis >= INFINITE ? INFINITE - 1 : (DWORD)millis;
}

static int cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                     const struct timespec *deadline) {
    if (!cond || !cond->valid || !mutex || !mutex->valid) return EINVAL;
    if (deadline && (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000L))
        return EINVAL;
    if (InterlockedCompareExchange(&mutex->owner, 0, 0) !=
        (LONG)GetCurrentThreadId()) return EPERM;
    /* SRW condition waits can release one level only, never recursive depth. */
    if (mutex->depth != 1) return ENOTSUP;
    InterlockedIncrement(&cond->waiters);
    int error;
    for (;;) {
        const DWORD timeout = deadline ? deadline_timeout(deadline) : INFINITE;
        mutex->depth = 0;
        InterlockedExchange(&mutex->owner, 0);
        const BOOL woke = SleepConditionVariableSRW(&cond->native, &mutex->native,
                                                    timeout, 0);
        const DWORD status = woke ? ERROR_SUCCESS : GetLastError();
        mutex->depth = 1;
        InterlockedExchange(&mutex->owner, (LONG)GetCurrentThreadId());
        if (woke) {
            error = 0;
            break;
        }
        if (status != ERROR_TIMEOUT) {
            error = win32_error(status);
            break;
        }
        if (!deadline || !deadline_timeout(deadline)) {
            error = ETIMEDOUT;
            break;
        }
        /* Retry finite chunks or an early OS timeout with the original deadline. */
    }
    InterlockedDecrement(&cond->waiters);
    return error;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    return cond_wait(cond, mutex, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                         const struct timespec *deadline) {
    if (!deadline) return EINVAL;
    return cond_wait(cond, mutex, deadline);
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond || !cond->valid) return EINVAL;
    WakeConditionVariable(&cond->native);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond || !cond->valid) return EINVAL;
    WakeAllConditionVariable(&cond->native);
    return 0;
}

struct once_call { void (*init)(void); };

static BOOL CALLBACK once_init(PINIT_ONCE once, PVOID arg, PVOID *context) {
    (void)once;
    (void)context;
    const struct once_call *call = arg;
    call->init();
    return TRUE;
}

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (!once || !init) return EINVAL;
    struct once_call call = {init};
    return InitOnceExecuteOnce(once, once_init, &call, NULL)
         ? 0 : win32_error(GetLastError());
}
