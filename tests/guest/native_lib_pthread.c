// Threads as far as a single-threaded runtime can provide them: the thread
// pointer (TPIDRURO) is real, pthread bookkeeping works, and lock calls succeed
// because there is nobody to contend with. The tests below this block create
// real threads.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
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

// --- atomics between threads -----------------------------------------------
//
// Everything above synchronises through a mutex, so it would still pass with a
// broken ll/sc pair: the mutex would spin, but the increments would stay
// serialised by the slow path. These tests take the lock away. A lost update is
// exactly what a lost LDREX reservation looks like, and the failure mode in a
// real guest is a reference count that never reaches zero or reaches it early.

static volatile int g_atomic_word = 0;

static void* fetch_add_worker(void* argument) {
    const int loops = (int)(size_t)argument;
    for (int index = 0; index < loops; ++index) {
        __sync_fetch_and_add(&g_atomic_word, 1);
    }
    return NULL;
}

// What the compiler emits for a plain atomic increment: LDREX/STREX inline.
// Two threads that both succeed on the same word lose an increment.
int lib_atomic_fetch_add(int threads, int loops) {
    pthread_t ids[8];
    if (threads < 1 || threads > 8 || loops < 1) {
        return -1;
    }
    g_atomic_word = 0;
    for (int index = 0; index < threads; ++index) {
        if (pthread_create(&ids[index], NULL, fetch_add_worker, (void*)(size_t)loops) != 0) {
            return -2;
        }
    }
    for (int index = 0; index < threads; ++index) {
        if (pthread_join(ids[index], NULL) != 0) {
            return -3;
        }
    }
    return g_atomic_word;
}

// A hand-written LL/SC loop: no compiler builtin sits between the reservation
// and the store, so the exclusive monitor is the only thing keeping it atomic.
static void llsc_increment(volatile int* word) {
    unsigned value;
    unsigned status;
    do {
        __asm__ volatile("ldrex %0, [%2]\n"
                         "add   %0, %0, #1\n"
                         "strex %1, %0, [%2]\n"
                         : "=&r"(value), "=&r"(status)
                         : "r"(word)
                         : "memory");
    } while (status != 0);
}

static void* ldrex_worker(void* argument) {
    const int loops = (int)(size_t)argument;
    for (int index = 0; index < loops; ++index) {
        llsc_increment(&g_atomic_word);
    }
    return NULL;
}

int lib_atomic_ldrex(int threads, int loops) {
    pthread_t ids[8];
    if (threads < 1 || threads > 8 || loops < 1) {
        return -1;
    }
    g_atomic_word = 0;
    for (int index = 0; index < threads; ++index) {
        if (pthread_create(&ids[index], NULL, ldrex_worker, (void*)(size_t)loops) != 0) {
            return -2;
        }
    }
    for (int index = 0; index < threads; ++index) {
        if (pthread_join(ids[index], NULL) != 0) {
            return -3;
        }
    }
    return g_atomic_word;
}

// The kernel helper page, the path old bionic and pre-v7 code take:
// __kernel_cmpxchg(oldval, newval, ptr) in r0-r2, 0 when the store happened.
// Calling the address directly is what bionic does; a wrong helper here is
// invisible until two threads touch the same word.
typedef int (*KernelCmpxchg)(int oldval, int newval, volatile int* ptr);

static KernelCmpxchg kernel_cmpxchg(void) {
    return (KernelCmpxchg)(uintptr_t)0xffff0fc0u;
}

static void* kuser_worker(void* argument) {
    const int loops = (int)(size_t)argument;
    KernelCmpxchg cmpxchg = kernel_cmpxchg();
    for (int index = 0; index < loops; ++index) {
        int expected;
        int next;
        do {
            expected = g_atomic_word;
            next = expected + 1;
        } while (cmpxchg(expected, next, &g_atomic_word) != 0);
    }
    return NULL;
}

int lib_atomic_kuser_cmpxchg(int threads, int loops) {
    pthread_t ids[8];
    if (threads < 1 || threads > 8 || loops < 1) {
        return -1;
    }
    g_atomic_word = 0;
    for (int index = 0; index < threads; ++index) {
        if (pthread_create(&ids[index], NULL, kuser_worker, (void*)(size_t)loops) != 0) {
            return -2;
        }
    }
    for (int index = 0; index < threads; ++index) {
        if (pthread_join(ids[index], NULL) != 0) {
            return -3;
        }
    }
    return g_atomic_word;
}

// A copy-on-write string as libstdc++ lays it out: the reference count sits
// next to the payload, copy is an atomic increment, drop is an atomic
// decrement that frees the payload at zero. One lost increment frees the
// buffer while another thread still holds a copy, and the copy then reads a
// chunk that has been handed to the next allocation -- the "refcount reached
// zero too early" crash.
typedef struct {
    int refcount;
    int checksum;
    unsigned char payload[64];
} CowRep;

static CowRep g_cow_rep;
static volatile int g_cow_destroyed = 0;
static volatile int g_cow_corrupt = 0;

static int cow_checksum(const unsigned char* payload) {
    int sum = 0;
    for (int index = 0; index < 64; ++index) {
        sum = sum * 31 + payload[index];
    }
    return sum;
}
static void cow_retain(CowRep* rep) {
    __sync_fetch_and_add(&rep->refcount, 1);
}

// A plain loop, not memset: the guest module would then import
// __aeabi_memset8, a runtime helper this library never needed before.
static void cow_poison(unsigned char* payload) {
    volatile unsigned char* cursor = payload;
    for (int index = 0; index < 64; ++index) {
        cursor[index] = 0xA5;
    }
}

static void cow_release(CowRep* rep) {
    if (__sync_sub_and_fetch(&rep->refcount, 1) == 0) {
        __sync_fetch_and_add(&g_cow_destroyed, 1);
        rep->checksum = 0;
        cow_poison(rep->payload);
    }
}

static void* cow_worker(void* argument) {
    const int loops = (int)(size_t)argument;
    for (int index = 0; index < loops; ++index) {
        CowRep* rep = &g_cow_rep;
        cow_retain(rep);
        // Reading the copy has to see the original string. Checking every
        // round would spend all the time in the checksum, not in the atomics.
        if ((index & 31) == 0 && rep->checksum != cow_checksum(rep->payload)) {
            __sync_fetch_and_add(&g_cow_corrupt, 1);
        }
        cow_release(rep);
    }
    return NULL;
}

// 0 when the original handle is the only owner left and no copy ever saw a
// freed buffer; negative otherwise, naming the failure.
int lib_atomic_cow(int threads, int loops) {
    pthread_t ids[8];
    if (threads < 1 || threads > 8 || loops < 1) {
        return -1;
    }
    // Volatile stores keep the fill from becoming a NEON sequence: the guest
    // reports no NEON and the JIT has no vector path to run it.
    volatile unsigned char* fill = g_cow_rep.payload;
    for (int index = 0; index < 64; ++index) {
        fill[index] = (unsigned char)(index * 7 + 3);
    }
    g_cow_rep.checksum = cow_checksum(g_cow_rep.payload);
    g_cow_rep.refcount = 1;
    g_cow_destroyed = 0;
    g_cow_corrupt = 0;
    for (int index = 0; index < threads; ++index) {
        if (pthread_create(&ids[index], NULL, cow_worker, (void*)(size_t)loops) != 0) {
            return -2;
        }
    }
    for (int index = 0; index < threads; ++index) {
        if (pthread_join(ids[index], NULL) != 0) {
            return -3;
        }
    }
    if (g_cow_destroyed != 0) {
        return -4;
    }
    if (g_cow_corrupt != 0) {
        return -5;
    }
    if (g_cow_rep.refcount != 1) {
        return -6;
    }
    return 0;
}

static volatile int g_conflict_stage = 0;

static void* conflict_worker(void* argument) {
    (void)argument;
    while (g_conflict_stage != 1) {
    }
    // A plain store, not another exclusive one: the value the other thread
    // reserved is gone, so its STREX has to fail.
    __atomic_store_n(&g_atomic_word, 0x1234, __ATOMIC_RELEASE);
    g_conflict_stage = 2;
    return NULL;
}

// A store from another thread between LDREX and STREX must lose the
// reservation. Checking this deterministically matters: if it passed, a lost
// reservation would silently overwrite the other thread's write, and every
// other atomic test above would still count correctly most of the time.
int lib_atomic_exclusive_conflict(void) {
    pthread_t id;
    g_atomic_word = 0;
    g_conflict_stage = 0;
    if (pthread_create(&id, NULL, conflict_worker, NULL) != 0) {
        return -1;
    }
    unsigned value;
    unsigned status;
    __asm__ volatile("ldrex %0, [%1]" : "=&r"(value) : "r"(&g_atomic_word) : "memory");
    g_conflict_stage = 1;
    while (g_conflict_stage != 2) {
    }
    __asm__ volatile("strex %0, %2, [%1]"
                     : "=&r"(status)
                     : "r"(&g_atomic_word), "r"(value + 1)
                     : "memory");
    if (pthread_join(id, NULL) != 0) {
        return -2;
    }
    return (int)status;
}

// --- a producer, a consumer, and the state between them --------------------
//
// The load failure we chase has one shape: a consumer looks at a producer's
// result before the producer finished, and reports "not a valid bundle" for
// data that is still being decoded. This is the same protocol without Unity in
// it. Order is carried by the mutex and the condition variable, never by a
// sleep, so the rounds do not depend on the host's speed: a lost wakeup, a
// mutex that admits two threads, or a store that escapes before the lock is
// released changes the answer in every round.

static pthread_mutex_t g_handoff_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_handoff_cond = PTHREAD_COND_INITIALIZER;
static int g_handoff_started = 0;
static int g_handoff_allow_finish = 0;
static int g_handoff_completed = 0;
static int g_handoff_state = 0;
static unsigned g_handoff_checksum = 0;

static unsigned bundle_fill(unsigned char* buffer, int size) {
    unsigned sum = 0;
    for (int index = 0; index < size; ++index) {
        buffer[index] = (unsigned char)(index * 13 + 7);
        sum = sum * 131 + buffer[index];
    }
    return sum;
}

static unsigned bundle_check(const unsigned char* buffer, int size) {
    unsigned sum = 0;
    for (int index = 0; index < size; ++index) {
        sum = sum * 131 + buffer[index];
    }
    return sum;
}

static void* handoff_worker(void* argument) {
    (void)argument;
    pthread_mutex_lock(&g_handoff_lock);
    g_handoff_started = 1;
    pthread_cond_broadcast(&g_handoff_cond);
    while (!g_handoff_allow_finish) {
        pthread_cond_wait(&g_handoff_cond, &g_handoff_lock);
    }
    pthread_mutex_unlock(&g_handoff_lock);

    // Decoding runs outside the lock, like the engine's loader: long enough
    // for the consumer to look while it is still in progress.
    unsigned char payload[256];
    const unsigned checksum = bundle_fill(payload, (int)sizeof(payload));

    pthread_mutex_lock(&g_handoff_lock);
    g_handoff_checksum = checksum;
    g_handoff_state = 1;
    g_handoff_completed = 1;
    pthread_cond_broadcast(&g_handoff_cond);
    pthread_mutex_unlock(&g_handoff_lock);
    return NULL;
}

// 0 when every round saw "preparing" mid-flight and a complete bundle at the
// end; otherwise the number of rounds that got it wrong.
int lib_thread_handoff(int rounds) {
    if (rounds < 1) {
        return -1;
    }
    unsigned char expected[256];
    const unsigned expected_checksum = bundle_fill(expected, (int)sizeof(expected));
    int failures = 0;
    for (int round = 0; round < rounds; ++round) {
        pthread_mutex_lock(&g_handoff_lock);
        g_handoff_started = 0;
        g_handoff_allow_finish = 0;
        g_handoff_completed = 0;
        g_handoff_state = 0;
        g_handoff_checksum = 0;
        pthread_mutex_unlock(&g_handoff_lock);

        pthread_t id;
        if (pthread_create(&id, NULL, handoff_worker, NULL) != 0) {
            ++failures;
            break;
        }

        pthread_mutex_lock(&g_handoff_lock);
        while (!g_handoff_started) {
            pthread_cond_wait(&g_handoff_cond, &g_handoff_lock);
        }
        // The worker is mid-decode. "Still preparing" is the only correct
        // answer here; reading this as a failed bundle is the bug.
        if (g_handoff_state != 0) {
            ++failures;
        }
        g_handoff_allow_finish = 1;
        pthread_cond_broadcast(&g_handoff_cond);
        while (!g_handoff_completed) {
            pthread_cond_wait(&g_handoff_cond, &g_handoff_lock);
        }
        if (g_handoff_state != 1 || g_handoff_checksum != expected_checksum) {
            ++failures;
        }
        pthread_mutex_unlock(&g_handoff_lock);

        if (pthread_join(id, NULL) != 0) {
            ++failures;
        }
    }
    return failures;
}

// --- a short-lived thread on a recycled machine -----------------------------
//
// Armbat does not build a fresh A32Machine per guest thread any more: the
// machine of an exited thread is reset and handed to the next one on the same
// processor slot, keeping its translated code. That is a performance fix, so a
// correctness test cannot tell it from the old behaviour -- both must compute
// the same value. What the test does catch is a reset that leaves something
// behind that changes the result, and a pool that hands the same machine to
// two live threads: the second thread would then compute on the first one's
// register file.

static unsigned reuse_mix(unsigned seed) {
    unsigned value = seed * 2654435761u + 1u;
    for (int index = 0; index < 400; ++index) {
        value ^= value >> 15;
        value *= 2246822519u;
        value += (unsigned)index;
    }
    return value;
}

static void* reuse_worker(void* argument) {
    return (void*)(size_t)reuse_mix((unsigned)(size_t)argument);
}

// 0 when every thread returned the value the same code computes locally;
// otherwise the number of threads that disagreed.
int lib_thread_reuse(int rounds) {
    if (rounds < 1 || rounds > 512) {
        return -1;
    }
    int mismatches = 0;
    for (int round = 0; round < rounds; ++round) {
        const unsigned seed = (unsigned)round + 1u;
        pthread_t id;
        if (pthread_create(&id, NULL, reuse_worker, (void*)(size_t)seed) != 0) {
            ++mismatches;
            break;
        }
        void* result = NULL;
        if (pthread_join(id, &result) != 0) {
            ++mismatches;
            continue;
        }
        if ((unsigned)(size_t)result != reuse_mix(seed)) {
            ++mismatches;
        }
    }
    return mismatches;
}

// --- the guest allocator across threads ------------------------------------
//
// After the atomics, the next thing a short-lived worker shares with the main
// thread is the allocator: bionic's jemalloc, which grows and shrinks arenas
// with mmap/madvise/munmap. A block handed out twice, freed under a live
// pointer or lost in a purge reads back as someone else's bytes. Each thread
// keeps a few blocks alive with a checksum over their contents and replaces
// them one at a time; the large sizes go through the mmap path, not the small
// size classes.

#define HEAP_SLOTS 4

typedef struct {
    unsigned char* block;
    int size;
    unsigned checksum;
} HeapSlot;

static volatile int g_heap_corrupt = 0;

static void* heap_worker(void* argument) {
    const int rounds = (int)(size_t)argument;
    HeapSlot slots[HEAP_SLOTS];
    // Volatile stores keep the compiler from zeroing the array with NEON:
    // the guest reports no NEON and the JIT has no vector path for it.
    volatile HeapSlot* init = slots;
    for (int index = 0; index < HEAP_SLOTS; ++index) {
        init[index].block = NULL;
        init[index].size = 0;
        init[index].checksum = 0;
    }
    int operations = 0;
    for (int round = 0; round < rounds; ++round) {
        HeapSlot* slot = &slots[round % HEAP_SLOTS];
        if (slot->block != NULL) {
            if (bundle_check(slot->block, slot->size) != slot->checksum) {
                __sync_fetch_and_add(&g_heap_corrupt, 1);
            }
            free(slot->block);
            slot->block = NULL;
            ++operations;
            continue;
        }
        // Every seventh block is large enough that jemalloc takes it from the
        // kernel instead of a size class: that is the mmap/munmap path.
        const int size = (round % 7 == 0) ? 96 * 1024 : 24 + ((round * 37) % 3000);
        unsigned char* block = (unsigned char*)malloc((size_t)size);
        if (block == NULL) {
            return (void*)(size_t)-2;
        }
        slot->checksum = bundle_fill(block, size);
        slot->size = size;
        slot->block = block;
        ++operations;
    }
    for (int index = 0; index < HEAP_SLOTS; ++index) {
        if (slots[index].block != NULL) {
            if (bundle_check(slots[index].block, slots[index].size) != slots[index].checksum) {
                __sync_fetch_and_add(&g_heap_corrupt, 1);
            }
            free(slots[index].block);
        }
    }
    return (void*)(size_t)operations;
}

// The number of allocations and checks across all threads; -2 when malloc
// failed. The count is fixed by construction, so a worker that dies early or
// returns before finishing shows up as a short total.
int lib_heap_churn(int threads, int rounds) {
    pthread_t ids[8];
    if (threads < 1 || threads > 8 || rounds < 1) {
        return -1;
    }
    g_heap_corrupt = 0;
    for (int index = 0; index < threads; ++index) {
        if (pthread_create(&ids[index], NULL, heap_worker, (void*)(size_t)rounds) != 0) {
            return -3;
        }
    }
    int operations = 0;
    for (int index = 0; index < threads; ++index) {
        void* result = NULL;
        if (pthread_join(ids[index], &result) != 0) {
            return -4;
        }
        const int done = (int)(size_t)result;
        if (done < 0) {
            return done;
        }
        operations += done;
    }
    if (g_heap_corrupt != 0) {
        return -5;
    }
    return operations;
}
