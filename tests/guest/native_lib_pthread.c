// Threads as far as a single-threaded runtime can provide them: the thread
// pointer (TPIDRURO) is real, pthread bookkeeping works, and lock calls succeed
// because there is nobody to contend with. The tests below this block create
// real threads.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

// bionic's inline TLS access, emitted by the compiler for __thread variables and
// by pthread_self()/errno in some configurations.
static inline unsigned read_thread_pointer(void) {
    unsigned value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

unsigned lib_tls_read(void) {
    return read_thread_pointer();
}

unsigned lib_tls_write(unsigned value) {
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 2" ::"r"(value));
    __asm__ volatile("isb" ::: "memory");
    return read_thread_pointer();
}

unsigned lib_pthread_self(void) {
    return (unsigned)pthread_self();
}

// The TLS block's second slot holds the thread id pointer, so a guest that
// reaches through pthread_self() finds a pointer to itself rather than garbage.
int lib_tls_slots(void) {
    const unsigned* slots = (const unsigned*)read_thread_pointer();
    return slots[0] == (unsigned)(size_t)slots && slots[1] == (unsigned)(size_t)slots;
}

int lib_pthread_mutex(void) {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_lock(&mutex) != 0) {
        return -1;
    }
    if (pthread_mutex_unlock(&mutex) != 0) {
        return -2;
    }
    if (pthread_mutex_init(&mutex, NULL) != 0) {
        return -3;
    }
    if (pthread_mutex_lock(&mutex) != 0) {
        return -4;
    }
    if (pthread_mutex_unlock(&mutex) != 0) {
        return -5;
    }
    if (pthread_mutex_destroy(&mutex) != 0) {
        return -6;
    }
    return 0;
}

int lib_pthread_cond(void) {
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    if (pthread_cond_signal(&cond) != 0) {
        return -1;
    }
    if (pthread_cond_broadcast(&cond) != 0) {
        return -2;
    }
    if (pthread_cond_destroy(&cond) != 0) {
        return -3;
    }
    return 0;
}

static pthread_key_t g_key;

int lib_pthread_key(void) {
    if (pthread_key_create(&g_key, NULL) != 0) {
        return -1;
    }
    if (pthread_setspecific(g_key, (void*)0x1234) != 0) {
        return -2;
    }
    if (pthread_getspecific(g_key) != (void*)0x1234) {
        return -3;
    }
    if (pthread_setspecific(g_key, NULL) != 0) {
        return -4;
    }
    if (pthread_getspecific(g_key) != NULL) {
        return -5;
    }
    if (pthread_key_delete(g_key) != 0) {
        return -6;
    }
    return 0;
}

// errno lives in the TLS block in bionic; the host points its errno cell there,
// so a failing libc call and the guest's own view must agree.
int lib_pthread_errno(void) {
    errno = 0;
    if (open("/nonexistent/armbat-tls", 0) >= 0) {
        return -1;
    }
    const int from_call = errno;
    const int from_tls = ((const int*)read_thread_pointer())[2];
    return from_call == from_tls ? from_call : -1;
}

static int g_once_runs = 0;

static void once_initializer(void) {
    ++g_once_runs;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

// pthread_once has to call guest code from inside an HLE handler, which means a
// nested JIT run. The counter proves the initializer ran exactly once.
int lib_pthread_once(void) {
    if (pthread_once(&g_once, once_initializer) != 0) {
        return -1;
    }
    if (pthread_once(&g_once, once_initializer) != 0) {
        return -2;
    }
    return g_once_runs;
}

// --- real threads ----------------------------------------------------------

static pthread_mutex_t g_counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_counter = 0;

static void* counter_worker(void* argument) {
    const int loops = (int)(size_t)argument;
    for (int index = 0; index < loops; ++index) {
        pthread_mutex_lock(&g_counter_lock);
        ++g_counter;
        pthread_mutex_unlock(&g_counter_lock);
    }
    return NULL;
}

// Every thread runs an unsynchronised loop of lock/increment/unlock. If the
// mutex were a no-op or the exclusive monitor were per-thread, increments would
// be lost; if join were wrong, the count would be short by whole threads.
int lib_pthread_counter(int threads, int loops) {
    pthread_t ids[8];
    if (threads < 1 || threads > 8) {
        return -1;
    }
    g_counter = 0;
    for (int index = 0; index < threads; ++index) {
        if (pthread_create(&ids[index], NULL, counter_worker, (void*)(size_t)loops) != 0) {
            return -2;
        }
    }
    for (int index = 0; index < threads; ++index) {
        if (pthread_join(ids[index], NULL) != 0) {
            return -3;
        }
    }
    return g_counter;
}

static void* value_worker(void* argument) {
    const int value = (int)(size_t)argument;
    return (void*)(size_t)(value * 3 + 1);
}

// The return value of a thread has to survive the join, which is the one thing a
// thread's register file cannot carry.
int lib_pthread_join_value(int count) {
    pthread_t ids[4];
    int sum = 0;
    for (int index = 0; index < count; ++index) {
        if (pthread_create(&ids[index], NULL, value_worker, (void*)(size_t)(index + 1)) != 0) {
            return -1;
        }
    }
    for (int index = 0; index < count; ++index) {
        void* result = NULL;
        if (pthread_join(ids[index], &result) != 0) {
            return -2;
        }
        sum += (int)(size_t)result;
    }
    return sum;
}

static pthread_key_t g_slot_key;
static unsigned g_seen[8];
static int g_seen_count = 0;
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;

static void* tls_worker(void* argument) {
    const unsigned marker = (unsigned)(size_t)argument;
    pthread_setspecific(g_slot_key, (void*)(size_t)marker);
    const unsigned read_back = (unsigned)(size_t)pthread_getspecific(g_slot_key);
    pthread_mutex_lock(&g_seen_lock);
    g_seen[g_seen_count++] = (marker == read_back) ? marker : 0xFFFFFFFFu;
    pthread_mutex_unlock(&g_seen_lock);
    return NULL;
}

// pthread_getspecific has to answer per thread, not per process: each thread
// stores a different value under the same key and reads its own back.
int lib_pthread_tls(int threads) {
    if (threads < 1 || threads > 8) {
        return -1;
    }
    if (pthread_key_create(&g_slot_key, NULL) != 0) {
        return -2;
    }
    g_seen_count = 0;
    pthread_t ids[8];
    for (int index = 0; index < threads; ++index) {
        if (pthread_create(&ids[index], NULL, tls_worker, (void*)(size_t)(index + 1)) != 0) {
            return -3;
        }
    }
    for (int index = 0; index < threads; ++index) {
        if (pthread_join(ids[index], NULL) != 0) {
            return -4;
        }
    }
    unsigned sum = 0;
    for (int index = 0; index < g_seen_count; ++index) {
        if (g_seen[index] == 0xFFFFFFFFu) {
            return -5;
        }
        sum += g_seen[index];
    }
    pthread_key_delete(g_slot_key);
    return (int)sum;
}

static pthread_mutex_t g_ping_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ping_cond = PTHREAD_COND_INITIALIZER;
static int g_ping_turn = 0;
static int g_ping_rounds = 0;

static void* ping_worker(void* argument) {
    const int which = (int)(size_t)argument;
    for (int round = 0; round < g_ping_rounds; ++round) {
        pthread_mutex_lock(&g_ping_lock);
        while (g_ping_turn != which) {
            pthread_cond_wait(&g_ping_cond, &g_ping_lock);
        }
        g_ping_turn = 1 - which;
        pthread_cond_signal(&g_ping_cond);
        pthread_mutex_unlock(&g_ping_lock);
    }
    return NULL;
}

// Two threads hand the turn back and forth. This is the classic lost-wakeup
// trap: with a condvar that does not really sleep, the loops either spin past
// each other or deadlock, and either way the final turn value is wrong.
int lib_pthread_pingpong(int rounds) {
    pthread_t first;
    pthread_t second;
    g_ping_rounds = rounds;
    g_ping_turn = 0;
    if (pthread_create(&first, NULL, ping_worker, (void*)(size_t)0) != 0) {
        return -1;
    }
    if (pthread_create(&second, NULL, ping_worker, (void*)(size_t)1) != 0) {
        return -2;
    }
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return g_ping_turn;
}

static void* exit_worker(void* argument) {
    // pthread_exit has to end this guest thread only and keep its value.
    pthread_exit((void*)((size_t)argument * 2));
    return (void*)0xDEAD;
}

int lib_pthread_exit(int value) {
    pthread_t id;
    if (pthread_create(&id, NULL, exit_worker, (void*)(size_t)value) != 0) {
        return -1;
    }
    void* result = NULL;
    if (pthread_join(id, &result) != 0) {
        return -2;
    }
    return (int)(size_t)result;
}

static pthread_mutex_t g_done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_cond = PTHREAD_COND_INITIALIZER;
static int g_done = 0;

static void* detach_worker(void* argument) {
    (void)argument;
    pthread_mutex_lock(&g_done_lock);
    g_done = 1;
    pthread_cond_signal(&g_done_cond);
    pthread_mutex_unlock(&g_done_lock);
    return NULL;
}

// A detached thread cannot be joined, so the test waits for its own signal.
int lib_pthread_detach(void) {
    g_done = 0;
    pthread_t id;
    if (pthread_create(&id, NULL, detach_worker, NULL) != 0) {
        return -1;
    }
    if (pthread_detach(id) != 0) {
        return -2;
    }
    pthread_mutex_lock(&g_done_lock);
    while (!g_done) {
        pthread_cond_wait(&g_done_cond, &g_done_lock);
    }
    pthread_mutex_unlock(&g_done_lock);
    return 0;
}

