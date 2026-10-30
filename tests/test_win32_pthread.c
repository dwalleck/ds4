/* Model-independent native Windows tests. A watchdog bounds synchronization bugs. */
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
        abort(); \
    } \
} while (0)

enum { WORKERS = 8, WAIT_MS = 10000 };

static void wait_event(HANDLE event) {
    CHECK(WaitForSingleObject(event, WAIT_MS) == WAIT_OBJECT_0);
}

static HANDLE make_event(void) {
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(event != NULL);
    return event;
}

static struct timespec realtime(void) {
    FILETIME filetime;
    ULARGE_INTEGER ticks;
    GetSystemTimePreciseAsFileTime(&filetime);
    ticks.LowPart = filetime.dwLowDateTime;
    ticks.HighPart = filetime.dwHighDateTime;
    const uint64_t unix_ticks = ticks.QuadPart - UINT64_C(116444736000000000);
    struct timespec now = {
        (time_t)(unix_ticks / UINT64_C(10000000)),
        (long)(unix_ticks % UINT64_C(10000000)) * 100
    };
    return now;
}

static struct timespec after_ms(long millis) {
    struct timespec deadline = realtime();
    deadline.tv_sec += millis / 1000;
    deadline.tv_nsec += (millis % 1000) * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    return deadline;
}

static int at_or_after(struct timespec now, struct timespec deadline) {
    return now.tv_sec > deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

static void bounded_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    struct timespec deadline = after_ms(WAIT_MS);
    CHECK(pthread_cond_timedwait(cond, mutex, &deadline) == 0);
}

static DWORD handle_count(void) {
    DWORD count;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

struct identity_case {
    pthread_t thread;
    void *result;
};

static void *identity_thread(void *arg) {
    struct identity_case *test = arg;
    CHECK(pthread_equal(pthread_self(), test->thread));
    CHECK(pthread_join(pthread_self(), NULL) == EDEADLK);
    return test->result;
}

static void test_join_and_attributes(void) {
    CHECK(pthread_equal(pthread_self(), pthread_self()));
    CHECK(pthread_join(pthread_self(), NULL) == EDEADLK);
    pthread_t invalid = {0};
    CHECK(pthread_join(invalid, NULL) == ESRCH);
    CHECK(pthread_detach(invalid) == ESRCH);
    pthread_attr_t attr;
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN - 1) == EINVAL);
#if SIZE_MAX > UINT_MAX
    CHECK(pthread_attr_setstacksize(&attr, (size_t)UINT_MAX + 1) == EINVAL);
#endif
    CHECK(pthread_attr_setstacksize(&attr, 512u * 1024u) == 0);
    CHECK(pthread_attr_setdetachstate(&attr, -1) == EINVAL);
    CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE) == 0);
    struct identity_case test = {0};
    /* Deliberately not representable in a Win32 thread's DWORD exit status. */
    test.result = (void *)(uintptr_t)UINT64_C(0x1234567887654321);
    CHECK(sizeof(void *) == 8);
    DWORD baseline = 0;
    for (int i = 0; i < 129; i++) {
        void *result = NULL;
        CHECK(pthread_create(&test.thread, &attr, identity_thread, &test) == 0);
        CHECK(!pthread_equal(pthread_self(), test.thread));
        CHECK(pthread_join(test.thread, &result) == 0);
        CHECK(result == test.result);
        if (!i) baseline = handle_count(); /* Warm CRT startup before counting. */
    }
    CHECK(handle_count() == baseline);
    CHECK(pthread_attr_destroy(&attr) == 0);
    CHECK(pthread_create(&test.thread, &attr, identity_thread, &test) == EINVAL);
    CHECK(pthread_create(&test.thread, NULL, NULL, NULL) == EINVAL);
}

struct detach_case {
    HANDLE ready;
    HANDLE release;
    HANDLE native;
    int self_detach;
};

static void *detached_thread(void *arg) {
    struct detach_case *test = arg;
    CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                          GetCurrentProcess(), &test->native,
                          SYNCHRONIZE, FALSE, 0));
    if (test->self_detach) CHECK(pthread_detach(pthread_self()) == 0);
    const HANDLE release = test->release;
    CHECK(SetEvent(test->ready));
    wait_event(release);
    return NULL;
}

static void test_detach(void) {
    const DWORD baseline = handle_count();
    for (int repeat = 0; repeat < 16; repeat++) {
        for (int mode = 0; mode < 4; mode++) {
            /* Detach running, detach exited, detached attr, detach from self. */
            struct detach_case test = {make_event(), make_event(), NULL, mode == 3};
            pthread_attr_t attr;
            CHECK(pthread_attr_init(&attr) == 0);
            if (mode == 2)
                CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
            pthread_t thread;
            CHECK(pthread_create(&thread, &attr, detached_thread, &test) == 0);
            CHECK(pthread_attr_destroy(&attr) == 0);
            wait_event(test.ready);
            if (mode == 0) CHECK(pthread_detach(thread) == 0);
            if (mode != 1) {
                /* Still blocked, so the detached identity remains live here. */
                CHECK(pthread_join(thread, NULL) == EINVAL);
                CHECK(pthread_detach(thread) == EINVAL);
            }
            CHECK(SetEvent(test.release));
            /* A duplicate native handle lets us prove actual detached exit,
             * not merely a user callback announcing that it is about to exit. */
            wait_event(test.native);
            if (mode == 1) CHECK(pthread_detach(thread) == 0);
            CHECK(CloseHandle(test.native));
            CHECK(CloseHandle(test.release));
            CHECK(CloseHandle(test.ready));
        }
    }
    CHECK(handle_count() == baseline);
}

struct mutex_case { pthread_mutex_t *mutex; };

static void *wrong_owner(void *arg) {
    const struct mutex_case *test = arg;
    CHECK(pthread_mutex_unlock(test->mutex) == EPERM);
    CHECK(pthread_mutex_trylock(test->mutex) == EBUSY);
    return NULL;
}

static void test_mutexes(void) {
    pthread_mutexattr_t attr;
    CHECK(pthread_mutexattr_init(&attr) == 0);
    CHECK(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == ENOTSUP);
    CHECK(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE) == 0);
    CHECK(pthread_mutexattr_settype(&attr, -1) == EINVAL);
    CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    pthread_mutex_t mutex;
    CHECK(pthread_mutex_init(&mutex, &attr) == 0);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    CHECK(pthread_mutex_trylock(&mutex) == 0);
    CHECK(pthread_mutex_destroy(&mutex) == EBUSY);
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    CHECK(pthread_cond_wait(&cond, &mutex) == ENOTSUP);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == EPERM);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
    CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
    CHECK(pthread_mutex_init(&mutex, &attr) == 0);
    CHECK(pthread_mutexattr_destroy(&attr) == 0);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    CHECK(pthread_mutex_lock(&mutex) == EDEADLK);
    struct mutex_case test = {&mutex};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, wrong_owner, &test) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
    CHECK(pthread_mutex_lock(&mutex) == EINVAL);
    CHECK(pthread_cond_destroy(&cond) == 0);
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static HANDLE once_start, once_entered, once_release, once_attempted;
static volatile LONG init_count, attempts;
static unsigned once_payload[64];

static void initialize_once(void) {
    CHECK(InterlockedIncrement(&init_count) == 1);
    CHECK(SetEvent(once_entered));
    wait_event(once_release);
    for (unsigned i = 0; i < 64; i++) once_payload[i] = i * 17u + 3u;
}

static void *once_thread(void *arg) {
    (void)arg;
    wait_event(once_start);
    if (InterlockedIncrement(&attempts) == WORKERS) CHECK(SetEvent(once_attempted));
    CHECK(pthread_once(&once, initialize_once) == 0);
    for (unsigned i = 0; i < 64; i++) CHECK(once_payload[i] == i * 17u + 3u);
    return NULL;
}

static void test_once(void) {
    once_start = make_event();
    once_entered = make_event();
    once_release = make_event();
    once_attempted = make_event();
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        CHECK(pthread_create(&threads[i], NULL, once_thread, NULL) == 0);
    CHECK(SetEvent(once_start));
    wait_event(once_entered);
    wait_event(once_attempted);
    CHECK(SetEvent(once_release));
    for (int i = 0; i < WORKERS; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(pthread_once(&once, initialize_once) == 0);
    CHECK(init_count == 1);
    CHECK(CloseHandle(once_attempted));
    CHECK(CloseHandle(once_release));
    CHECK(CloseHandle(once_entered));
    CHECK(CloseHandle(once_start));
}

struct pool {
    pthread_mutex_t mutex;
    pthread_cond_t work, done;
    unsigned ready, generation, completed;
    int stop;
};

static void *pool_worker(void *arg) {
    struct pool *pool = arg;
    unsigned seen = 0;
    CHECK(pthread_mutex_lock(&pool->mutex) == 0);
    pool->ready++;
    CHECK(pthread_cond_signal(&pool->done) == 0);
    for (;;) {
        while (seen == pool->generation && !pool->stop)
            CHECK(pthread_cond_wait(&pool->work, &pool->mutex) == 0);
        if (pool->stop) break;
        seen = pool->generation;
        pool->completed++;
        if (pool->completed == WORKERS) CHECK(pthread_cond_signal(&pool->done) == 0);
    }
    CHECK(pthread_mutex_unlock(&pool->mutex) == 0);
    return NULL;
}

static void test_conditions_and_shutdown(void) {
    struct pool pool = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .work = PTHREAD_COND_INITIALIZER,
        .done = PTHREAD_COND_INITIALIZER
    };
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        CHECK(pthread_create(&threads[i], NULL, pool_worker, &pool) == 0);
    CHECK(pthread_mutex_lock(&pool.mutex) == 0);
    while (pool.ready != WORKERS) bounded_wait(&pool.done, &pool.mutex);
    CHECK(pthread_cond_destroy(&pool.work) == EBUSY);
    for (unsigned generation = 1; generation <= 32; generation++) {
        pool.completed = 0;
        pool.generation = generation;
        CHECK(pthread_cond_broadcast(&pool.work) == 0);
        while (pool.completed != WORKERS) bounded_wait(&pool.done, &pool.mutex);
    }
    pool.stop = 1;
    CHECK(pthread_cond_broadcast(&pool.work) == 0);
    CHECK(pthread_mutex_unlock(&pool.mutex) == 0);
    for (int i = 0; i < WORKERS; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(pthread_cond_destroy(&pool.done) == 0);
    CHECK(pthread_cond_destroy(&pool.work) == 0);
    CHECK(pthread_mutex_destroy(&pool.mutex) == 0);
}

static void test_timedwait(void) {
    pthread_condattr_t attr;
    CHECK(pthread_condattr_init(&attr) == 0);
    CHECK(pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == ENOTSUP);
    CHECK(pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE) == 0);
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    CHECK(pthread_cond_init(&cond, &attr) == 0);
    CHECK(pthread_condattr_destroy(&attr) == 0);
    CHECK(pthread_mutex_init(&mutex, NULL) == 0);
    CHECK(pthread_cond_wait(&cond, &mutex) == EPERM);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    struct timespec invalid = {0, 1000000000L};
    CHECK(pthread_cond_timedwait(&cond, &mutex, &invalid) == EINVAL);
    invalid.tv_nsec = -1;
    CHECK(pthread_cond_timedwait(&cond, &mutex, &invalid) == EINVAL);
    const struct timespec expired = {0, 0};
    int error;
    do {
        error = pthread_cond_timedwait(&cond, &mutex, &expired);
        CHECK(error == 0 || error == ETIMEDOUT); /* POSIX permits spurious wakes. */
    } while (!error);
    CHECK(pthread_mutex_trylock(&mutex) == EBUSY);
    const struct timespec deadline = after_ms(25);
    do {
        error = pthread_cond_timedwait(&cond, &mutex, &deadline);
        CHECK(error == 0 || error == ETIMEDOUT);
    } while (!error);
    CHECK(at_or_after(realtime(), deadline));
    /* Both timeout paths must have reacquired the caller's mutex. */
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_trylock(&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_cond_destroy(&cond) == 0);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
    CHECK(pthread_cond_signal(&cond) == EINVAL);
}

static DWORD WINAPI watchdog(void *arg) {
    if (WaitForSingleObject((HANDLE)arg, 60000) != WAIT_OBJECT_0)
        TerminateProcess(GetCurrentProcess(), 124);
    return 0;
}

int main(void) {
    HANDLE stop = make_event();
    HANDLE guard = CreateThread(NULL, 0, watchdog, stop, 0, NULL);
    CHECK(guard != NULL);
    test_join_and_attributes();
    test_detach();
    test_mutexes();
    test_once();
    test_conditions_and_shutdown();
    test_timedwait();
    CHECK(SetEvent(stop));
    wait_event(guard);
    CHECK(CloseHandle(guard));
    CHECK(CloseHandle(stop));
    puts("win32 pthread lifecycle, mutex, once, condition and timeout tests passed");
    return 0;
}
