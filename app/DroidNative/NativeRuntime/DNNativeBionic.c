#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "DNNativeBionic.h"
#include "DNNativePatch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

static _Thread_local int dn_bionic_errno_value;

int *dn_native_errno_location(void)
{
    return &dn_bionic_errno_value;
}

static int dn_errno_from_linux_result(long result)
{
    if (result < 0) {
        dn_bionic_errno_value = (int)(-result);
        return -1;
    }
    return 0;
}

static long dn_sys(long a0, long a1, long a2, long a3, long a4, long a5, long nr)
{
    return dn_native_syscall_dispatch(a0, a1, a2, a3, a4, a5, nr);
}

/* ------------------------------------------------------------------ */
/* Guest-object registry for pthread/semaphore objects                 */
/* ------------------------------------------------------------------ */

enum dn_sync_kind {
    DN_SYNC_MUTEX = 1,
    DN_SYNC_COND,
    DN_SYNC_RWLOCK,
    DN_SYNC_SEM,
    DN_SYNC_BARRIER,
    DN_SYNC_MUTEXATTR,
    DN_SYNC_CONDATTR,
    DN_SYNC_RWLOCKATTR,
    DN_SYNC_THREADATTR,
    DN_SYNC_BARRIERATTR
};

struct dn_host_sync {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_rwlock_t rwlock;
    unsigned sem_count;
    unsigned barrier_count;
    unsigned barrier_waiting;
    unsigned barrier_phase;
    int type;
    int clock_id;
    int detach_state;
    size_t stack_size;
    void *stack_addr;
    int sched_policy;
    int sched_priority;
    size_t guard_size;
};

struct dn_sync_entry {
    void *guest;
    int kind;
    struct dn_host_sync *host;
    struct dn_sync_entry *next;
};

static pthread_mutex_t dn_sync_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct dn_sync_entry *dn_sync_entries;

static struct dn_host_sync *dn_sync_get(void *guest, int kind, bool create)
{
    if (!guest)
        return NULL;
    pthread_mutex_lock(&dn_sync_registry_mutex);
    for (struct dn_sync_entry *e = dn_sync_entries; e; e = e->next) {
        if (e->guest == guest && e->kind == kind) {
            struct dn_host_sync *host = e->host;
            pthread_mutex_unlock(&dn_sync_registry_mutex);
            return host;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&dn_sync_registry_mutex);
        return NULL;
    }
    struct dn_sync_entry *entry = (struct dn_sync_entry *)calloc(1, sizeof(*entry));
    struct dn_host_sync *host = (struct dn_host_sync *)calloc(1, sizeof(*host));
    if (!entry || !host) {
        free(entry);
        free(host);
        pthread_mutex_unlock(&dn_sync_registry_mutex);
        return NULL;
    }
    host->clock_id = 0;
    host->sched_policy = SCHED_OTHER;
    if (kind == DN_SYNC_MUTEX)
        pthread_mutex_init(&host->mutex, NULL);
    if (kind == DN_SYNC_COND || kind == DN_SYNC_SEM || kind == DN_SYNC_BARRIER) {
        pthread_mutex_init(&host->mutex, NULL);
        pthread_cond_init(&host->cond, NULL);
    }
    if (kind == DN_SYNC_RWLOCK)
        pthread_rwlock_init(&host->rwlock, NULL);
    entry->guest = guest;
    entry->kind = kind;
    entry->host = host;
    entry->next = dn_sync_entries;
    dn_sync_entries = entry;
    pthread_mutex_unlock(&dn_sync_registry_mutex);
    return host;
}

static void dn_sync_destroy_entry(void *guest, int kind)
{
    struct dn_host_sync *host = NULL;
    pthread_mutex_lock(&dn_sync_registry_mutex);
    struct dn_sync_entry **cursor = &dn_sync_entries;
    while (*cursor) {
        if ((*cursor)->guest == guest && (*cursor)->kind == kind) {
            struct dn_sync_entry *dead = *cursor;
            host = dead->host;
            *cursor = dead->next;
            free(dead);
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&dn_sync_registry_mutex);
    if (!host)
        return;
    if (kind == DN_SYNC_MUTEX || kind == DN_SYNC_COND ||
        kind == DN_SYNC_SEM || kind == DN_SYNC_BARRIER) {
        pthread_mutex_destroy(&host->mutex);
        if (kind != DN_SYNC_MUTEX)
            pthread_cond_destroy(&host->cond);
    }
    if (kind == DN_SYNC_RWLOCK)
        pthread_rwlock_destroy(&host->rwlock);
    free(host);
}

static int dn_pthread_result(int rc)
{
    return rc == 0 ? 0 : dn_native_linux_errno_from_host(rc);
}

/* pthread mutex + attributes */
static int dn_pthread_mutexattr_init(void *attr)
{
    struct dn_host_sync *host = dn_sync_get(attr, DN_SYNC_MUTEXATTR, true);
    if (!host) return dn_native_linux_errno_from_host(ENOMEM);
    host->type = 0;
    return 0;
}

static int dn_pthread_mutexattr_destroy(void *attr)
{
    dn_sync_destroy_entry(attr, DN_SYNC_MUTEXATTR);
    return 0;
}

static int dn_pthread_mutexattr_settype(void *attr, int type)
{
    struct dn_host_sync *host = dn_sync_get(attr, DN_SYNC_MUTEXATTR, true);
    if (!host) return dn_native_linux_errno_from_host(ENOMEM);
    host->type = type;
    return 0;
}

static int dn_pthread_mutexattr_gettype(const void *attr, int *type)
{
    struct dn_host_sync *host = dn_sync_get((void *)attr, DN_SYNC_MUTEXATTR, false);
    if (type)
        *type = host ? host->type : 0;
    return 0;
}

static int dn_pthread_mutexattr_setpshared(void *attr, int pshared)
{
    (void)pshared;
    return dn_pthread_mutexattr_init(attr);
}

static int dn_pthread_mutexattr_getpshared(const void *attr, int *pshared)
{
    (void)attr;
    if (pshared)
        *pshared = 0; /* PTHREAD_PROCESS_PRIVATE */
    return 0;
}

static int dn_pthread_mutex_init(void *mutex, const void *attr)
{
    /* Recreate any implicit static-initializer entry with the type
     * requested now. */
    dn_sync_destroy_entry(mutex, DN_SYNC_MUTEX);
    struct dn_host_sync *host = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    if (!host)
        return dn_native_linux_errno_from_host(ENOMEM);
    int type = 0;
    if (attr) {
        struct dn_host_sync *a = dn_sync_get((void *)attr, DN_SYNC_MUTEXATTR, false);
        if (a)
            type = a->type;
    }
    host->type = type;
    pthread_mutex_destroy(&host->mutex);
    pthread_mutexattr_t host_attr;
    pthread_mutexattr_init(&host_attr);
    if (type == 1)
        pthread_mutexattr_settype(&host_attr, PTHREAD_MUTEX_RECURSIVE);
    else if (type == 2)
        pthread_mutexattr_settype(&host_attr, PTHREAD_MUTEX_ERRORCHECK);
    int rc = pthread_mutex_init(&host->mutex, &host_attr);
    pthread_mutexattr_destroy(&host_attr);
    return dn_pthread_result(rc);
}

static int dn_pthread_mutex_destroy(void *mutex)
{
    dn_sync_destroy_entry(mutex, DN_SYNC_MUTEX);
    return 0;
}

static int dn_pthread_mutex_lock(void *mutex)
{
    struct dn_host_sync *host = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    return host ? dn_pthread_result(pthread_mutex_lock(&host->mutex))
                : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_mutex_trylock(void *mutex)
{
    struct dn_host_sync *host = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    return host ? dn_pthread_result(pthread_mutex_trylock(&host->mutex))
                : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_mutex_unlock(void *mutex)
{
    struct dn_host_sync *host = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    return host ? dn_pthread_result(pthread_mutex_unlock(&host->mutex))
                : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_mutex_timedlock(void *mutex, const struct timespec *abstime)
{
    struct dn_host_sync *host = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    if (!host)
        return dn_native_linux_errno_from_host(ENOMEM);
#if defined(__APPLE__)
    /* Darwin has no public pthread_mutex_timedlock in older SDKs; a
     * bounded trylock loop is portable and preserves timeout semantics. */
    if (!abstime)
        return dn_pthread_result(pthread_mutex_lock(&host->mutex));
    for (;;) {
        int rc = pthread_mutex_trylock(&host->mutex);
        if (rc != EBUSY)
            return dn_pthread_result(rc);
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > abstime->tv_sec ||
            (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
            return dn_native_linux_errno_from_host(ETIMEDOUT);
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
#else
    return dn_pthread_result(pthread_mutex_timedlock(&host->mutex, abstime));
#endif
}

/* pthread condition variables */
static int dn_pthread_condattr_init(void *attr)
{
    struct dn_host_sync *host = dn_sync_get(attr, DN_SYNC_CONDATTR, true);
    if (!host) return dn_native_linux_errno_from_host(ENOMEM);
    host->clock_id = 0;
    return 0;
}

static int dn_pthread_condattr_destroy(void *attr)
{
    dn_sync_destroy_entry(attr, DN_SYNC_CONDATTR);
    return 0;
}

static int dn_pthread_condattr_setclock(void *attr, int clock_id)
{
    struct dn_host_sync *host = dn_sync_get(attr, DN_SYNC_CONDATTR, true);
    if (!host) return dn_native_linux_errno_from_host(ENOMEM);
    host->clock_id = clock_id;
    return 0;
}

static int dn_pthread_condattr_getclock(const void *attr, int *clock_id)
{
    struct dn_host_sync *host = dn_sync_get((void *)attr, DN_SYNC_CONDATTR, false);
    if (clock_id)
        *clock_id = host ? host->clock_id : 0;
    return 0;
}

static int dn_pthread_condattr_setpshared(void *attr, int pshared)
{
    (void)pshared;
    return dn_pthread_condattr_init(attr);
}

static int dn_pthread_condattr_getpshared(const void *attr, int *pshared)
{
    (void)attr;
    if (pshared)
        *pshared = 0;
    return 0;
}

static int dn_pthread_cond_init(void *cond, const void *attr)
{
    dn_sync_destroy_entry(cond, DN_SYNC_COND);
    struct dn_host_sync *host = dn_sync_get(cond, DN_SYNC_COND, true);
    if (!host)
        return dn_native_linux_errno_from_host(ENOMEM);
    if (attr) {
        struct dn_host_sync *a = dn_sync_get((void *)attr, DN_SYNC_CONDATTR, false);
        if (a)
            host->clock_id = a->clock_id;
    }
    return 0;
}

static int dn_pthread_cond_destroy(void *cond)
{
    dn_sync_destroy_entry(cond, DN_SYNC_COND);
    return 0;
}

static int dn_pthread_cond_signal(void *cond)
{
    struct dn_host_sync *host = dn_sync_get(cond, DN_SYNC_COND, true);
    return host ? dn_pthread_result(pthread_cond_signal(&host->cond))
                : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_cond_broadcast(void *cond)
{
    struct dn_host_sync *host = dn_sync_get(cond, DN_SYNC_COND, true);
    return host ? dn_pthread_result(pthread_cond_broadcast(&host->cond))
                : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_cond_wait(void *cond, void *mutex)
{
    struct dn_host_sync *c = dn_sync_get(cond, DN_SYNC_COND, true);
    struct dn_host_sync *m = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    if (!c || !m)
        return dn_native_linux_errno_from_host(ENOMEM);
    return dn_pthread_result(pthread_cond_wait(&c->cond, &m->mutex));
}

static void dn_cond_deadline_realtime(int guest_clock,
                                      const struct timespec *guest_deadline,
                                      struct timespec *out)
{
    *out = *guest_deadline;
    if (guest_clock == 0)
        return;
    struct timespec guest_now, real_now;
    clockid_t host_clock = CLOCK_MONOTONIC;
    if (clock_gettime(host_clock, &guest_now) != 0 ||
        clock_gettime(CLOCK_REALTIME, &real_now) != 0)
        return;
    out->tv_sec = real_now.tv_sec + (guest_deadline->tv_sec - guest_now.tv_sec);
    out->tv_nsec = real_now.tv_nsec + (guest_deadline->tv_nsec - guest_now.tv_nsec);
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec++;
        out->tv_nsec -= 1000000000L;
    } else if (out->tv_nsec < 0) {
        out->tv_sec--;
        out->tv_nsec += 1000000000L;
    }
}

static int dn_pthread_cond_timedwait(void *cond, void *mutex,
                                     const struct timespec *abstime)
{
    struct dn_host_sync *c = dn_sync_get(cond, DN_SYNC_COND, true);
    struct dn_host_sync *m = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    if (!c || !m || !abstime)
        return dn_native_linux_errno_from_host(EINVAL);
    struct timespec deadline;
    dn_cond_deadline_realtime(c->clock_id, abstime, &deadline);
    return dn_pthread_result(pthread_cond_timedwait(&c->cond, &m->mutex, &deadline));
}

static int dn_pthread_cond_clockwait(void *cond, void *mutex, int clock_id,
                                     const struct timespec *abstime)
{
    struct dn_host_sync *c = dn_sync_get(cond, DN_SYNC_COND, true);
    struct dn_host_sync *m = dn_sync_get(mutex, DN_SYNC_MUTEX, true);
    if (!c || !m || !abstime)
        return dn_native_linux_errno_from_host(EINVAL);
    struct timespec deadline;
    dn_cond_deadline_realtime(clock_id, abstime, &deadline);
    return dn_pthread_result(pthread_cond_timedwait(&c->cond, &m->mutex, &deadline));
}

/* pthread rwlocks */
static int dn_pthread_rwlockattr_init(void *attr)
{
    return dn_sync_get(attr, DN_SYNC_RWLOCKATTR, true)
               ? 0 : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_rwlockattr_destroy(void *attr)
{
    dn_sync_destroy_entry(attr, DN_SYNC_RWLOCKATTR);
    return 0;
}

static int dn_pthread_rwlockattr_setpshared(void *attr, int pshared)
{
    (void)pshared;
    return dn_pthread_rwlockattr_init(attr);
}

static int dn_pthread_rwlockattr_getpshared(const void *attr, int *pshared)
{
    (void)attr;
    if (pshared) *pshared = 0;
    return 0;
}

static int dn_pthread_rwlockattr_setkind_np(void *attr, int kind)
{
    (void)kind;
    return dn_pthread_rwlockattr_init(attr);
}

static int dn_pthread_rwlockattr_getkind_np(const void *attr, int *kind)
{
    (void)attr;
    if (kind) *kind = 0;
    return 0;
}

static int dn_pthread_rwlock_init(void *rwlock, const void *attr)
{
    (void)attr;
    dn_sync_destroy_entry(rwlock, DN_SYNC_RWLOCK);
    return dn_sync_get(rwlock, DN_SYNC_RWLOCK, true)
               ? 0 : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_rwlock_destroy(void *rwlock)
{
    dn_sync_destroy_entry(rwlock, DN_SYNC_RWLOCK);
    return 0;
}

static int dn_pthread_rwlock_rdlock(void *rwlock)
{
    struct dn_host_sync *h = dn_sync_get(rwlock, DN_SYNC_RWLOCK, true);
    return h ? dn_pthread_result(pthread_rwlock_rdlock(&h->rwlock))
             : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_rwlock_tryrdlock(void *rwlock)
{
    struct dn_host_sync *h = dn_sync_get(rwlock, DN_SYNC_RWLOCK, true);
    return h ? dn_pthread_result(pthread_rwlock_tryrdlock(&h->rwlock))
             : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_rwlock_wrlock(void *rwlock)
{
    struct dn_host_sync *h = dn_sync_get(rwlock, DN_SYNC_RWLOCK, true);
    return h ? dn_pthread_result(pthread_rwlock_wrlock(&h->rwlock))
             : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_rwlock_trywrlock(void *rwlock)
{
    struct dn_host_sync *h = dn_sync_get(rwlock, DN_SYNC_RWLOCK, true);
    return h ? dn_pthread_result(pthread_rwlock_trywrlock(&h->rwlock))
             : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_rwlock_unlock(void *rwlock)
{
    struct dn_host_sync *h = dn_sync_get(rwlock, DN_SYNC_RWLOCK, true);
    return h ? dn_pthread_result(pthread_rwlock_unlock(&h->rwlock))
             : dn_native_linux_errno_from_host(ENOMEM);
}

#if !defined(__APPLE__)
static int dn_pthread_rwlock_timedrdlock(void *rwlock, const struct timespec *abstime)
{
    struct dn_host_sync *h = dn_sync_get(rwlock, DN_SYNC_RWLOCK, true);
    return h ? dn_pthread_result(pthread_rwlock_timedrdlock(&h->rwlock, abstime))
             : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_rwlock_timedwrlock(void *rwlock, const struct timespec *abstime)
{
    struct dn_host_sync *h = dn_sync_get(rwlock, DN_SYNC_RWLOCK, true);
    return h ? dn_pthread_result(pthread_rwlock_timedwrlock(&h->rwlock, abstime))
             : dn_native_linux_errno_from_host(ENOMEM);
}
#endif

/* pthread once + keys */
static pthread_mutex_t dn_once_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dn_once_cond = PTHREAD_COND_INITIALIZER;

static int dn_pthread_once(volatile int *once_control, void (*init_routine)(void))
{
    if (!once_control || !init_routine)
        return dn_native_linux_errno_from_host(EINVAL);
    pthread_mutex_lock(&dn_once_mutex);
    while (*once_control == 2)
        pthread_cond_wait(&dn_once_cond, &dn_once_mutex);
    if (*once_control == 1) {
        pthread_mutex_unlock(&dn_once_mutex);
        return 0;
    }
    *once_control = 2;
    pthread_mutex_unlock(&dn_once_mutex);

    init_routine();

    pthread_mutex_lock(&dn_once_mutex);
    *once_control = 1;
    pthread_cond_broadcast(&dn_once_cond);
    pthread_mutex_unlock(&dn_once_mutex);
    return 0;
}

enum { DN_GUEST_PTHREAD_KEYS = 128 };

struct dn_guest_key {
    bool used;
    void (*destructor)(void *);
};

static struct dn_guest_key dn_guest_keys[DN_GUEST_PTHREAD_KEYS];
static pthread_mutex_t dn_guest_keys_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t dn_guest_values_key;
static pthread_once_t dn_guest_values_once = PTHREAD_ONCE_INIT;

static void dn_guest_values_destroy(void *storage)
{
    void **values = (void **)storage;
    if (!values)
        return;
    for (int iteration = 0; iteration < 4; ++iteration) {
        bool any = false;
        for (int i = 0; i < DN_GUEST_PTHREAD_KEYS; ++i) {
            void *value = values[i];
            if (!value)
                continue;
            values[i] = NULL;
            void (*destructor)(void *) = NULL;
            pthread_mutex_lock(&dn_guest_keys_mutex);
            if (dn_guest_keys[i].used)
                destructor = dn_guest_keys[i].destructor;
            pthread_mutex_unlock(&dn_guest_keys_mutex);
            if (destructor) {
                any = true;
                destructor(value);
            }
        }
        if (!any)
            break;
    }
    free(values);
}

static void dn_guest_values_key_init(void)
{
    (void)pthread_key_create(&dn_guest_values_key, dn_guest_values_destroy);
}

static void **dn_guest_values(bool create)
{
    pthread_once(&dn_guest_values_once, dn_guest_values_key_init);
    void **values = (void **)pthread_getspecific(dn_guest_values_key);
    if (!values && create) {
        values = (void **)calloc(DN_GUEST_PTHREAD_KEYS, sizeof(void *));
        if (values)
            (void)pthread_setspecific(dn_guest_values_key, values);
    }
    return values;
}

static int dn_pthread_key_create(uint32_t *key, void (*destructor)(void *))
{
    if (!key)
        return dn_native_linux_errno_from_host(EINVAL);
    pthread_mutex_lock(&dn_guest_keys_mutex);
    for (int i = 0; i < DN_GUEST_PTHREAD_KEYS; ++i) {
        if (!dn_guest_keys[i].used) {
            dn_guest_keys[i].used = true;
            dn_guest_keys[i].destructor = destructor;
            *key = (uint32_t)i;
            pthread_mutex_unlock(&dn_guest_keys_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&dn_guest_keys_mutex);
    return dn_native_linux_errno_from_host(EAGAIN);
}

static int dn_pthread_key_delete(uint32_t key)
{
    if (key >= DN_GUEST_PTHREAD_KEYS)
        return dn_native_linux_errno_from_host(EINVAL);
    pthread_mutex_lock(&dn_guest_keys_mutex);
    dn_guest_keys[key].used = false;
    dn_guest_keys[key].destructor = NULL;
    pthread_mutex_unlock(&dn_guest_keys_mutex);
    return 0;
}

static int dn_pthread_setspecific(uint32_t key, const void *value)
{
    if (key >= DN_GUEST_PTHREAD_KEYS)
        return dn_native_linux_errno_from_host(EINVAL);
    void **values = dn_guest_values(true);
    if (!values)
        return dn_native_linux_errno_from_host(ENOMEM);
    values[key] = (void *)value;
    return 0;
}

static void *dn_pthread_getspecific(uint32_t key)
{
    if (key >= DN_GUEST_PTHREAD_KEYS)
        return NULL;
    void **values = dn_guest_values(false);
    return values ? values[key] : NULL;
}

/* pthread creation/attributes */
static int dn_pthread_attr_init(void *attr)
{
    struct dn_host_sync *h = dn_sync_get(attr, DN_SYNC_THREADATTR, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->detach_state = 0;
    h->stack_size = 0;
    h->stack_addr = NULL;
    h->sched_policy = SCHED_OTHER;
    h->sched_priority = 0;
    return 0;
}

static int dn_pthread_attr_destroy(void *attr)
{
    dn_sync_destroy_entry(attr, DN_SYNC_THREADATTR);
    return 0;
}

static int dn_pthread_attr_setdetachstate(void *attr, int detach)
{
    struct dn_host_sync *h = dn_sync_get(attr, DN_SYNC_THREADATTR, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->detach_state = detach;
    return 0;
}

static int dn_pthread_attr_getdetachstate(const void *attr, int *detach)
{
    struct dn_host_sync *h = dn_sync_get((void *)attr, DN_SYNC_THREADATTR, false);
    if (detach) *detach = h ? h->detach_state : 0;
    return 0;
}

static int dn_pthread_attr_setstacksize(void *attr, size_t size)
{
    struct dn_host_sync *h = dn_sync_get(attr, DN_SYNC_THREADATTR, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->stack_size = size;
    return 0;
}

static int dn_pthread_attr_getstacksize(const void *attr, size_t *size)
{
    struct dn_host_sync *h = dn_sync_get((void *)attr, DN_SYNC_THREADATTR, false);
    if (size) *size = h && h->stack_size ? h->stack_size : 8u * 1024u * 1024u;
    return 0;
}

static int dn_pthread_attr_setstack(void *attr, void *addr, size_t size)
{
    struct dn_host_sync *h = dn_sync_get(attr, DN_SYNC_THREADATTR, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->stack_addr = addr;
    h->stack_size = size;
    return 0;
}

static int dn_pthread_attr_getstack(const void *attr, void **addr, size_t *size)
{
    struct dn_host_sync *h = dn_sync_get((void *)attr, DN_SYNC_THREADATTR, false);
    if (addr) *addr = h ? h->stack_addr : NULL;
    if (size) *size = h && h->stack_size ? h->stack_size : 8u * 1024u * 1024u;
    return 0;
}

static int dn_pthread_attr_setguardsize(void *attr, size_t size)
{
    struct dn_host_sync *h = dn_sync_get(attr, DN_SYNC_THREADATTR, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->guard_size = size;
    return 0;
}

static int dn_pthread_attr_getguardsize(const void *attr, size_t *size)
{
    struct dn_host_sync *h = dn_sync_get((void *)attr, DN_SYNC_THREADATTR, false);
    if (size) *size = h ? h->guard_size : 0;
    return 0;
}

static int dn_pthread_attr_setschedpolicy(void *attr, int policy)
{
    struct dn_host_sync *h = dn_sync_get(attr, DN_SYNC_THREADATTR, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->sched_policy = policy == 1 ? SCHED_FIFO : policy == 2 ? SCHED_RR : SCHED_OTHER;
    return 0;
}

static int dn_pthread_attr_getschedpolicy(const void *attr, int *policy)
{
    struct dn_host_sync *h = dn_sync_get((void *)attr, DN_SYNC_THREADATTR, false);
    if (policy) *policy = h && h->sched_policy == SCHED_FIFO ? 1 : h && h->sched_policy == SCHED_RR ? 2 : 0;
    return 0;
}

struct dn_sched_param_guest {
    int sched_priority;
};

static int dn_pthread_attr_setschedparam(void *attr, const struct dn_sched_param_guest *param)
{
    struct dn_host_sync *h = dn_sync_get(attr, DN_SYNC_THREADATTR, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->sched_priority = param ? param->sched_priority : 0;
    return 0;
}

static int dn_pthread_attr_getschedparam(const void *attr, struct dn_sched_param_guest *param)
{
    struct dn_host_sync *h = dn_sync_get((void *)attr, DN_SYNC_THREADATTR, false);
    if (param) param->sched_priority = h ? h->sched_priority : 0;
    return 0;
}

static int dn_pthread_attr_setscope(void *attr, int scope)
{
    (void)scope;
    return dn_pthread_attr_init(attr);
}

static int dn_pthread_attr_getscope(const void *attr, int *scope)
{
    (void)attr;
    if (scope) *scope = 0;
    return 0;
}

static int dn_pthread_attr_setinheritsched(void *attr, int inherit)
{
    (void)inherit;
    return dn_pthread_attr_init(attr);
}

static int dn_pthread_attr_getinheritsched(const void *attr, int *inherit)
{
    (void)attr;
    if (inherit) *inherit = 0;
    return 0;
}

struct dn_thread_start {
    void *(*routine)(void *);
    void *arg;
};

static void *dn_thread_trampoline(void *opaque)
{
    struct dn_thread_start *start = (struct dn_thread_start *)opaque;
    void *(*routine)(void *) = start->routine;
    void *arg = start->arg;
    free(start);
    (void)dn_native_thread_pointer();
    return routine(arg);
}

typedef uintptr_t dn_guest_pthread_t;

static int dn_pthread_create(dn_guest_pthread_t *thread, const void *attr,
                             void *(*start_routine)(void *), void *arg)
{
    if (!thread || !start_routine)
        return dn_native_linux_errno_from_host(EINVAL);
    struct dn_thread_start *start = (struct dn_thread_start *)malloc(sizeof(*start));
    if (!start)
        return dn_native_linux_errno_from_host(ENOMEM);
    start->routine = start_routine;
    start->arg = arg;

    pthread_attr_t host_attr;
    pthread_attr_init(&host_attr);
    struct dn_host_sync *cfg = attr ? dn_sync_get((void *)attr, DN_SYNC_THREADATTR, false) : NULL;
    if (cfg) {
        if (cfg->stack_size)
            pthread_attr_setstacksize(&host_attr, cfg->stack_size);
        if (cfg->detach_state == 1)
            pthread_attr_setdetachstate(&host_attr, PTHREAD_CREATE_DETACHED);
        if (cfg->sched_policy != SCHED_OTHER) {
            pthread_attr_setschedpolicy(&host_attr, cfg->sched_policy);
            struct sched_param param;
            memset(&param, 0, sizeof(param));
            param.sched_priority = cfg->sched_priority;
            pthread_attr_setschedparam(&host_attr, &param);
        }
    }
    pthread_t host_thread;
    int rc = pthread_create(&host_thread, &host_attr, dn_thread_trampoline, start);
    pthread_attr_destroy(&host_attr);
    if (rc != 0) {
        free(start);
        return dn_pthread_result(rc);
    }
    *thread = (dn_guest_pthread_t)(uintptr_t)host_thread;
    return 0;
}

static int dn_pthread_join(dn_guest_pthread_t thread, void **retval)
{
    return dn_pthread_result(pthread_join((pthread_t)(uintptr_t)thread, retval));
}

static int dn_pthread_detach(dn_guest_pthread_t thread)
{
    return dn_pthread_result(pthread_detach((pthread_t)(uintptr_t)thread));
}

static dn_guest_pthread_t dn_pthread_self(void)
{
    return (dn_guest_pthread_t)(uintptr_t)pthread_self();
}

static int dn_pthread_equal(dn_guest_pthread_t a, dn_guest_pthread_t b)
{
    return pthread_equal((pthread_t)(uintptr_t)a, (pthread_t)(uintptr_t)b);
}

static void dn_pthread_exit(void *retval)
{
    pthread_exit(retval);
}

static int dn_pthread_cancel(dn_guest_pthread_t thread)
{
    (void)thread;
    return dn_native_linux_errno_from_host(ENOSYS);
}

static int dn_pthread_setcancelstate(int state, int *oldstate)
{
    (void)state;
    if (oldstate) *oldstate = 1; /* PTHREAD_CANCEL_DISABLE */
    return 0;
}

static int dn_pthread_setcanceltype(int type, int *oldtype)
{
    (void)type;
    if (oldtype) *oldtype = 0;
    return 0;
}

static void dn_pthread_testcancel(void) { }

static int dn_pthread_setschedparam(dn_guest_pthread_t thread, int policy,
                                    const struct dn_sched_param_guest *param)
{
    int host_policy = policy == 1 ? SCHED_FIFO : policy == 2 ? SCHED_RR : SCHED_OTHER;
    struct sched_param host_param;
    memset(&host_param, 0, sizeof(host_param));
    host_param.sched_priority = param ? param->sched_priority : 0;
    return dn_pthread_result(pthread_setschedparam(
        (pthread_t)(uintptr_t)thread, host_policy, &host_param));
}

static int dn_pthread_getschedparam(dn_guest_pthread_t thread, int *policy,
                                    struct dn_sched_param_guest *param)
{
    int host_policy = SCHED_OTHER;
    struct sched_param host_param;
    int rc = pthread_getschedparam((pthread_t)(uintptr_t)thread, &host_policy, &host_param);
    if (policy) *policy = host_policy == SCHED_FIFO ? 1 : host_policy == SCHED_RR ? 2 : 0;
    if (param) param->sched_priority = host_param.sched_priority;
    return dn_pthread_result(rc);
}

static int dn_pthread_getattr_np(dn_guest_pthread_t thread, void *attr)
{
    (void)thread;
    return dn_pthread_attr_init(attr);
}

static int dn_host_signal_number(int guest_signal)
{
    switch (guest_signal) {
    case 1: return SIGHUP;
    case 2: return SIGINT;
    case 3: return SIGQUIT;
    case 4: return SIGILL;
    case 5: return SIGTRAP;
    case 6: return SIGABRT;
    case 7: return SIGBUS;
    case 8: return SIGFPE;
    case 9: return SIGKILL;
    case 10: return SIGUSR1;
    case 11: return SIGSEGV;
    case 12: return SIGUSR2;
    case 13: return SIGPIPE;
    case 14: return SIGALRM;
    case 15: return SIGTERM;
    case 17: return SIGCHLD;
    case 18: return SIGCONT;
    case 19: return SIGSTOP;
    case 20: return SIGTSTP;
    default: return -1;
    }
}

static int dn_pthread_kill(dn_guest_pthread_t thread, int signal)
{
    if (signal == 0)
        return 0;
    int host_signal = dn_host_signal_number(signal);
    if (host_signal < 0)
        return dn_native_linux_errno_from_host(EINVAL);
    return dn_pthread_result(pthread_kill((pthread_t)(uintptr_t)thread, host_signal));
}

static int dn_pthread_sigmask(int how, const void *guest_set, void *guest_oldset)
{
    sigset_t host_old;
    if (!guest_set) {
        int rc = pthread_sigmask(SIG_SETMASK, NULL, &host_old);
        if (rc == 0 && guest_oldset)
            memset(guest_oldset, 0, 128);
        return dn_pthread_result(rc);
    }
    sigset_t host_set;
    sigemptyset(&host_set);
    const uint64_t *words = (const uint64_t *)guest_set;
    for (int sig = 1; sig <= 64; ++sig) {
        if (words[(sig - 1) / 64] & (1ULL << ((sig - 1) % 64))) {
            int host_signal = dn_host_signal_number(sig);
            if (host_signal > 0)
                sigaddset(&host_set, host_signal);
        }
    }
    int host_how = how == 1 ? SIG_UNBLOCK : how == 2 ? SIG_SETMASK : SIG_BLOCK;
    int rc = pthread_sigmask(host_how, &host_set, guest_oldset ? &host_old : NULL);
    if (rc == 0 && guest_oldset)
        memset(guest_oldset, 0, 128);
    return dn_pthread_result(rc);
}

/* POSIX semaphores + barriers */
static int dn_sem_init(void *sem, int pshared, unsigned value)
{
    (void)pshared;
    dn_sync_destroy_entry(sem, DN_SYNC_SEM);
    struct dn_host_sync *h = dn_sync_get(sem, DN_SYNC_SEM, true);
    if (!h) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(ENOMEM);
        return -1;
    }
    h->sem_count = value;
    return 0;
}

static int dn_sem_destroy(void *sem)
{
    dn_sync_destroy_entry(sem, DN_SYNC_SEM);
    return 0;
}

static int dn_sem_wait(void *sem)
{
    struct dn_host_sync *h = dn_sync_get(sem, DN_SYNC_SEM, true);
    if (!h) { dn_bionic_errno_value = dn_native_linux_errno_from_host(ENOMEM); return -1; }
    pthread_mutex_lock(&h->mutex);
    while (h->sem_count == 0)
        pthread_cond_wait(&h->cond, &h->mutex);
    h->sem_count--;
    pthread_mutex_unlock(&h->mutex);
    return 0;
}

static int dn_sem_trywait(void *sem)
{
    struct dn_host_sync *h = dn_sync_get(sem, DN_SYNC_SEM, true);
    if (!h) { dn_bionic_errno_value = dn_native_linux_errno_from_host(ENOMEM); return -1; }
    pthread_mutex_lock(&h->mutex);
    if (h->sem_count == 0) {
        pthread_mutex_unlock(&h->mutex);
        dn_bionic_errno_value = dn_native_linux_errno_from_host(EAGAIN);
        return -1;
    }
    h->sem_count--;
    pthread_mutex_unlock(&h->mutex);
    return 0;
}

static int dn_sem_timedwait(void *sem, const struct timespec *abstime)
{
    struct dn_host_sync *h = dn_sync_get(sem, DN_SYNC_SEM, true);
    if (!h || !abstime) { dn_bionic_errno_value = dn_native_linux_errno_from_host(EINVAL); return -1; }
    pthread_mutex_lock(&h->mutex);
    while (h->sem_count == 0) {
        int rc = pthread_cond_timedwait(&h->cond, &h->mutex, abstime);
        if (rc != 0) {
            pthread_mutex_unlock(&h->mutex);
            dn_bionic_errno_value = dn_native_linux_errno_from_host(rc);
            return -1;
        }
    }
    h->sem_count--;
    pthread_mutex_unlock(&h->mutex);
    return 0;
}

static int dn_sem_post(void *sem)
{
    struct dn_host_sync *h = dn_sync_get(sem, DN_SYNC_SEM, true);
    if (!h) { dn_bionic_errno_value = dn_native_linux_errno_from_host(ENOMEM); return -1; }
    pthread_mutex_lock(&h->mutex);
    h->sem_count++;
    pthread_cond_signal(&h->cond);
    pthread_mutex_unlock(&h->mutex);
    return 0;
}

static int dn_sem_getvalue(void *sem, int *value)
{
    struct dn_host_sync *h = dn_sync_get(sem, DN_SYNC_SEM, true);
    if (!h || !value) { dn_bionic_errno_value = dn_native_linux_errno_from_host(EINVAL); return -1; }
    pthread_mutex_lock(&h->mutex);
    *value = (int)h->sem_count;
    pthread_mutex_unlock(&h->mutex);
    return 0;
}

static int dn_pthread_barrierattr_init(void *attr)
{
    return dn_sync_get(attr, DN_SYNC_BARRIERATTR, true)
               ? 0 : dn_native_linux_errno_from_host(ENOMEM);
}

static int dn_pthread_barrierattr_destroy(void *attr)
{
    dn_sync_destroy_entry(attr, DN_SYNC_BARRIERATTR);
    return 0;
}

static int dn_pthread_barrierattr_setpshared(void *attr, int pshared)
{
    (void)pshared;
    return dn_pthread_barrierattr_init(attr);
}

static int dn_pthread_barrierattr_getpshared(const void *attr, int *pshared)
{
    (void)attr;
    if (pshared) *pshared = 0;
    return 0;
}

static int dn_pthread_barrier_init(void *barrier, const void *attr, unsigned count)
{
    (void)attr;
    if (count == 0)
        return dn_native_linux_errno_from_host(EINVAL);
    dn_sync_destroy_entry(barrier, DN_SYNC_BARRIER);
    struct dn_host_sync *h = dn_sync_get(barrier, DN_SYNC_BARRIER, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    h->barrier_count = count;
    h->barrier_waiting = 0;
    h->barrier_phase = 0;
    return 0;
}

static int dn_pthread_barrier_destroy(void *barrier)
{
    dn_sync_destroy_entry(barrier, DN_SYNC_BARRIER);
    return 0;
}

static int dn_pthread_barrier_wait(void *barrier)
{
    struct dn_host_sync *h = dn_sync_get(barrier, DN_SYNC_BARRIER, true);
    if (!h) return dn_native_linux_errno_from_host(ENOMEM);
    pthread_mutex_lock(&h->mutex);
    unsigned phase = h->barrier_phase;
    h->barrier_waiting++;
    int serial = 0;
    if (h->barrier_waiting >= h->barrier_count) {
        h->barrier_waiting = 0;
        h->barrier_phase++;
        serial = -1; /* PTHREAD_BARRIER_SERIAL_THREAD */
        pthread_cond_broadcast(&h->cond);
    } else {
        while (h->barrier_phase == phase)
            pthread_cond_wait(&h->cond, &h->mutex);
    }
    pthread_mutex_unlock(&h->mutex);
    return serial;
}

/* ------------------------------------------------------------------ */
/* File descriptors and filesystem (Linux ABI over DN VFS)             */
/* ------------------------------------------------------------------ */

#include <dlfcn.h>
#include "DNNativeLinker.h"
#include "DNNativeShims.h"

static int dn_bionic_open(const char *path, int flags, mode_t mode)
{
    long result = dn_sys(-100, (long)(uintptr_t)path, flags, (long)mode, 0, 0, 56);
    if (dn_errno_from_linux_result(result) != 0)
        return -1;
    return (int)result;
}

static int dn_bionic_openat(int dirfd, const char *path, int flags, mode_t mode)
{
    long result = dn_sys(dirfd, (long)(uintptr_t)path, flags, (long)mode, 0, 0, 56);
    if (dn_errno_from_linux_result(result) != 0)
        return -1;
    return (int)result;
}

static int dn_bionic_close(int fd)
{
    return dn_errno_from_linux_result(dn_sys(fd, 0, 0, 0, 0, 0, 57));
}

static ssize_t dn_bionic_read(int fd, void *buffer, size_t count)
{
    long result = dn_sys(fd, (long)(uintptr_t)buffer, (long)count, 0, 0, 0, 63);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static ssize_t dn_bionic_write(int fd, const void *buffer, size_t count)
{
    long result = dn_sys(fd, (long)(uintptr_t)buffer, (long)count, 0, 0, 0, 64);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static ssize_t dn_bionic_readv(int fd, const struct iovec *iov, int count)
{
    long result = dn_sys(fd, (long)(uintptr_t)iov, count, 0, 0, 0, 65);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static ssize_t dn_bionic_writev(int fd, const struct iovec *iov, int count)
{
    long result = dn_sys(fd, (long)(uintptr_t)iov, count, 0, 0, 0, 66);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static ssize_t dn_bionic_pread(int fd, void *buffer, size_t count, int64_t offset)
{
    long result = dn_sys(fd, (long)(uintptr_t)buffer, (long)count, (long)offset, 0, 0, 67);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static ssize_t dn_bionic_pwrite(int fd, const void *buffer, size_t count, int64_t offset)
{
    long result = dn_sys(fd, (long)(uintptr_t)buffer, (long)count, (long)offset, 0, 0, 68);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static int64_t dn_bionic_lseek(int fd, int64_t offset, int whence)
{
    long result = dn_sys(fd, (long)offset, whence, 0, 0, 0, 62);
    return dn_errno_from_linux_result(result) ? -1 : (int64_t)result;
}

static int dn_bionic_dup(int fd)
{
    long result = dn_sys(fd, 0, 0, 0, 0, 0, 23);
    return dn_errno_from_linux_result(result) ? -1 : (int)result;
}

static int dn_bionic_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd)
        return newfd;
    long result = dn_sys(oldfd, newfd, 0, 0, 0, 0, 24);
    return dn_errno_from_linux_result(result) ? -1 : (int)result;
}

static int dn_bionic_dup3(int oldfd, int newfd, int flags)
{
    long result = dn_sys(oldfd, newfd, flags, 0, 0, 0, 24);
    return dn_errno_from_linux_result(result) ? -1 : (int)result;
}

static int dn_bionic_fcntl(int fd, int command, long arg)
{
    long result = dn_sys(fd, command, arg, 0, 0, 0, 25);
    return dn_errno_from_linux_result(result) ? -1 : (int)result;
}

static int dn_bionic_ioctl(int fd, unsigned long request, void *arg)
{
    long result = dn_sys(fd, (long)request, (long)(uintptr_t)arg, 0, 0, 0, 29);
    return dn_errno_from_linux_result(result) ? -1 : (int)result;
}

static int dn_bionic_access(const char *path, int mode)
{
    return dn_errno_from_linux_result(
        dn_sys(-100, (long)(uintptr_t)path, mode, 0, 0, 0, 48));
}

static int dn_bionic_faccessat(int dirfd, const char *path, int mode, int flags)
{
    (void)flags;
    return dn_errno_from_linux_result(
        dn_sys(dirfd, (long)(uintptr_t)path, mode, 0, 0, 0, 48));
}

static int dn_bionic_stat(const char *path, void *statbuf)
{
    return dn_errno_from_linux_result(
        dn_sys(-100, (long)(uintptr_t)path, (long)(uintptr_t)statbuf, 0, 0, 0, 79));
}

static int dn_bionic_lstat(const char *path, void *statbuf)
{
    return dn_errno_from_linux_result(
        dn_sys(-100, (long)(uintptr_t)path, (long)(uintptr_t)statbuf, 0x100, 0, 0, 79));
}

static int dn_bionic_fstat(int fd, void *statbuf)
{
    return dn_errno_from_linux_result(
        dn_sys(fd, (long)(uintptr_t)statbuf, 0, 0, 0, 0, 80));
}

static int dn_bionic_fstatat(int dirfd, const char *path, void *statbuf, int flags)
{
    return dn_errno_from_linux_result(
        dn_sys(dirfd, (long)(uintptr_t)path, (long)(uintptr_t)statbuf, flags, 0, 0, 79));
}

static int dn_bionic_mkdir(const char *path, mode_t mode)
{
    return dn_errno_from_linux_result(
        dn_sys(-100, (long)(uintptr_t)path, (long)mode, 0, 0, 0, 34));
}

static int dn_bionic_mkdirat(int dirfd, const char *path, mode_t mode)
{
    return dn_errno_from_linux_result(
        dn_sys(dirfd, (long)(uintptr_t)path, (long)mode, 0, 0, 0, 34));
}

static int dn_bionic_rmdir(const char *path)
{
    return dn_errno_from_linux_result(
        dn_sys(-100, (long)(uintptr_t)path, 0x200, 0, 0, 0, 35));
}

static int dn_bionic_unlink(const char *path)
{
    return dn_errno_from_linux_result(
        dn_sys(-100, (long)(uintptr_t)path, 0, 0, 0, 0, 35));
}

static int dn_bionic_unlinkat(int dirfd, const char *path, int flags)
{
    return dn_errno_from_linux_result(
        dn_sys(dirfd, (long)(uintptr_t)path, flags, 0, 0, 0, 35));
}

static int dn_bionic_rename(const char *oldpath, const char *newpath)
{
    return dn_errno_from_linux_result(dn_sys(
        -100, (long)(uintptr_t)oldpath, -100, (long)(uintptr_t)newpath, 0, 0, 38));
}

static int dn_bionic_renameat(int olddir, const char *oldpath,
                              int newdir, const char *newpath)
{
    return dn_errno_from_linux_result(dn_sys(
        olddir, (long)(uintptr_t)oldpath, newdir, (long)(uintptr_t)newpath, 0, 0, 38));
}

static int dn_bionic_link(const char *oldpath, const char *newpath)
{
    return dn_errno_from_linux_result(dn_sys(
        -100, (long)(uintptr_t)oldpath, -100, (long)(uintptr_t)newpath, 0, 0, 86));
}

static int dn_bionic_symlink(const char *target, const char *linkpath)
{
    return dn_errno_from_linux_result(dn_sys(
        (long)(uintptr_t)target, -100, (long)(uintptr_t)linkpath, 0, 0, 0, 88));
}

static ssize_t dn_bionic_readlink(const char *path, char *buffer, size_t size)
{
    long result = dn_sys(-100, (long)(uintptr_t)path,
                         (long)(uintptr_t)buffer, (long)size, 0, 0, 78);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static ssize_t dn_bionic_readlinkat(int dirfd, const char *path, char *buffer, size_t size)
{
    long result = dn_sys(dirfd, (long)(uintptr_t)path,
                         (long)(uintptr_t)buffer, (long)size, 0, 0, 78);
    return dn_errno_from_linux_result(result) ? -1 : (ssize_t)result;
}

static int dn_bionic_chdir(const char *path)
{
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)path, 0, 0, 0, 0, 0, 49));
}

static int dn_bionic_fchdir(int fd)
{
    return dn_errno_from_linux_result(dn_sys(fd, 0, 0, 0, 0, 0, 50));
}

static char *dn_bionic_getcwd(char *buffer, size_t size)
{
    long result = dn_sys((long)(uintptr_t)buffer, (long)size, 0, 0, 0, 0, 17);
    if (dn_errno_from_linux_result(result) != 0)
        return NULL;
    return buffer;
}

static int dn_bionic_chmod(const char *path, mode_t mode)
{
    return dn_errno_from_linux_result(dn_sys(
        -100, (long)(uintptr_t)path, (long)mode, 0, 0, 0, 53));
}

static int dn_bionic_fchmod(int fd, mode_t mode)
{
    return dn_errno_from_linux_result(dn_sys(fd, (long)mode, 0, 0, 0, 0, 52));
}

static int dn_bionic_fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    (void)flags;
    return dn_errno_from_linux_result(dn_sys(
        dirfd, (long)(uintptr_t)path, (long)mode, 0, 0, 0, 53));
}

static int dn_bionic_fchown(int fd, int uid, int gid)
{
    return dn_errno_from_linux_result(dn_sys(fd, uid, gid, 0, 0, 0, 54));
}

static int dn_bionic_truncate(const char *path, int64_t length)
{
    int fd = dn_bionic_open(path, 1, 0);
    if (fd < 0)
        return -1;
    long result = dn_sys(fd, (long)length, 0, 0, 0, 0, 46);
    int saved = dn_bionic_errno_value;
    (void)dn_bionic_close(fd);
    if (result < 0)
        dn_bionic_errno_value = (int)(-result);
    else
        dn_bionic_errno_value = saved;
    return result < 0 ? -1 : 0;
}

static int dn_bionic_ftruncate(int fd, int64_t length)
{
    return dn_errno_from_linux_result(dn_sys(fd, (long)length, 0, 0, 0, 0, 46));
}

static int dn_bionic_fsync(int fd)
{
    return dn_errno_from_linux_result(dn_sys(fd, 0, 0, 0, 0, 0, 82));
}

static int dn_bionic_fdatasync(int fd)
{
    return dn_errno_from_linux_result(dn_sys(fd, 0, 0, 0, 0, 0, 83));
}

static int dn_bionic_pipe(int fds[2])
{
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)fds, 0, 0, 0, 0, 0, 59));
}

static int dn_bionic_pipe2(int fds[2], int flags)
{
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)fds, flags, 0, 0, 0, 0, 59));
}

static int dn_bionic_poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    int rc = poll(fds, (nfds_t)nfds, timeout);
    if (rc < 0)
        dn_bionic_errno_value = dn_native_linux_errno_from_host(errno);
    return rc;
}

static bool dn_fdset_test(const uint64_t *set, int fd)
{
    return set && (set[fd / 64] & (1ULL << (fd % 64))) != 0;
}

static void dn_fdset_set(uint64_t *set, int fd)
{
    if (set)
        set[fd / 64] |= 1ULL << (fd % 64);
}

static int dn_bionic_select(int nfds, uint64_t *readfds, uint64_t *writefds,
                            uint64_t *exceptfds, struct timeval *timeout)
{
    if (nfds < 0 || nfds > 1024) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(EINVAL);
        return -1;
    }
    struct pollfd polls[1024];
    int count = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (dn_fdset_test(readfds, fd)) events |= POLLIN;
        if (dn_fdset_test(writefds, fd)) events |= POLLOUT;
        if (dn_fdset_test(exceptfds, fd)) events |= POLLPRI;
        if (events) {
            polls[count].fd = fd;
            polls[count].events = events;
            polls[count].revents = 0;
            count++;
        }
    }
    int timeout_ms = -1;
    if (timeout)
        timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    int rc = poll(polls, (nfds_t)count, timeout_ms);
    if (readfds) memset(readfds, 0, 128);
    if (writefds) memset(writefds, 0, 128);
    if (exceptfds) memset(exceptfds, 0, 128);
    if (rc < 0) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(errno);
        return -1;
    }
    int ready = 0;
    for (int i = 0; i < count; ++i) {
        if (polls[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            dn_fdset_set(readfds, polls[i].fd);
            ready++;
        }
        if (polls[i].revents & POLLOUT) {
            dn_fdset_set(writefds, polls[i].fd);
            ready++;
        }
        if (polls[i].revents & (POLLPRI | POLLERR)) {
            dn_fdset_set(exceptfds, polls[i].fd);
            ready++;
        }
    }
    return ready;
}

/* DIR/dirent with Linux guest layout */
struct dn_guest_dir {
    DIR *host;
    bool owns_fd;
    uint8_t entry[19 + 256];
};

static void dn_fill_guest_dirent(uint8_t *out, const struct dirent *entry)
{
    memset(out, 0, 19 + 256);
    uint64_t ino = (uint64_t)entry->d_ino;
    int64_t off = 0;
    uint16_t reclen = 19 + 256;
    memcpy(out + 0, &ino, 8);
    memcpy(out + 8, &off, 8);
    memcpy(out + 16, &reclen, 2);
    out[18] = entry->d_type;
    strncpy((char *)out + 19, entry->d_name, 255);
}

static void *dn_bionic_opendir(const char *path)
{
    char translated[4096];
    int rc = dn_native_translate_path(path, translated, sizeof(translated));
    if (rc != 0) {
        dn_bionic_errno_value = (int)(-rc);
        return NULL;
    }
    DIR *host = opendir(translated);
    if (!host) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(errno);
        return NULL;
    }
    struct dn_guest_dir *dir = (struct dn_guest_dir *)calloc(1, sizeof(*dir));
    if (!dir) {
        closedir(host);
        dn_bionic_errno_value = dn_native_linux_errno_from_host(ENOMEM);
        return NULL;
    }
    dir->host = host;
    return dir;
}

static void *dn_bionic_fdopendir(int fd)
{
    int dupfd = dup(fd);
    DIR *host = dupfd >= 0 ? fdopendir(dupfd) : NULL;
    if (!host) {
        if (dupfd >= 0) close(dupfd);
        dn_bionic_errno_value = dn_native_linux_errno_from_host(errno);
        return NULL;
    }
    struct dn_guest_dir *dir = (struct dn_guest_dir *)calloc(1, sizeof(*dir));
    if (!dir) {
        closedir(host);
        return NULL;
    }
    dir->host = host;
    dir->owns_fd = true;
    return dir;
}

static void *dn_bionic_readdir(void *opaque)
{
    struct dn_guest_dir *dir = (struct dn_guest_dir *)opaque;
    if (!dir || !dir->host)
        return NULL;
    errno = 0;
    struct dirent *entry = readdir(dir->host);
    if (!entry) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(errno);
        return NULL;
    }
    dn_fill_guest_dirent(dir->entry, entry);
    return dir->entry;
}

static int dn_bionic_readdir_r(void *opaque, void *entry_out, void **result)
{
    void *entry = dn_bionic_readdir(opaque);
    if (!entry) {
        if (result) *result = NULL;
        return dn_bionic_errno_value;
    }
    memcpy(entry_out, entry, 19 + 256);
    if (result) *result = entry_out;
    return 0;
}

static int dn_bionic_closedir(void *opaque)
{
    struct dn_guest_dir *dir = (struct dn_guest_dir *)opaque;
    if (!dir) return -1;
    int rc = closedir(dir->host);
    free(dir);
    if (rc != 0)
        dn_bionic_errno_value = dn_native_linux_errno_from_host(errno);
    return rc;
}

static void dn_bionic_rewinddir(void *opaque)
{
    struct dn_guest_dir *dir = (struct dn_guest_dir *)opaque;
    if (dir && dir->host)
        rewinddir(dir->host);
}

static int dn_bionic_dirfd(void *opaque)
{
    struct dn_guest_dir *dir = (struct dn_guest_dir *)opaque;
    return dir && dir->host ? dirfd(dir->host) : -1;
}

static long dn_bionic_telldir(void *opaque)
{
    struct dn_guest_dir *dir = (struct dn_guest_dir *)opaque;
    return dir && dir->host ? telldir(dir->host) : -1;
}

static void dn_bionic_seekdir(void *opaque, long offset)
{
    struct dn_guest_dir *dir = (struct dn_guest_dir *)opaque;
    if (dir && dir->host)
        seekdir(dir->host, offset);
}

/* Minimal unbuffered FILE* implementation over DN file descriptors. */
struct dn_guest_file {
    int fd;
    bool readable;
    bool writable;
    bool eof;
    bool error;
    bool close_fd;
    bool has_unget;
    int unget;
};

/*
 * Modern bionic (>= API 23) imports stdin/stdout/stderr as pointer
 * variables.  Older code imports the three-element __sF array and
 * computes & __sF[i] with bionic's public LP64 FILE size of 152
 * bytes.  Keep each DN standard stream at offset zero of a 152-byte
 * slot so both ABIs address the same three stream objects.
 */
struct dn_guest_file_slot {
    struct dn_guest_file file;
    unsigned char private_storage[152 - sizeof(struct dn_guest_file)];
};
_Static_assert(sizeof(struct dn_guest_file_slot) == 152,
               "bionic LP64 FILE slot must remain 152 bytes");
static struct dn_guest_file_slot dn_std_files[3] = {
    { { 0, true, false, false, false, false, false, 0 }, { 0 } },
    { { 1, false, true, false, false, false, false, 0 }, { 0 } },
    { { 2, false, true, false, false, false, false, 0 }, { 0 } }
};
static void *dn_stdin_pointer = &dn_std_files[0].file;
static void *dn_stdout_pointer = &dn_std_files[1].file;
static void *dn_stderr_pointer = &dn_std_files[2].file;

static void *dn_bionic_fopen(const char *path, const char *mode)
{
    if (!path || !mode) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(EINVAL);
        return NULL;
    }
    int flags;
    bool plus = strchr(mode, '+') != NULL;
    if (mode[0] == 'r')
        flags = plus ? 2 : 0;
    else if (mode[0] == 'w')
        flags = (plus ? 2 : 1) | 64 | 512;
    else if (mode[0] == 'a')
        flags = (plus ? 2 : 1) | 64 | 1024;
    else {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(EINVAL);
        return NULL;
    }
    if (strchr(mode, 'x'))
        flags |= 128;
    if (strchr(mode, 'e'))
        flags |= 524288;
    int fd = dn_bionic_open(path, flags, 0666);
    if (fd < 0)
        return NULL;
    struct dn_guest_file *file = (struct dn_guest_file *)calloc(1, sizeof(*file));
    if (!file) {
        dn_bionic_close(fd);
        return NULL;
    }
    file->fd = fd;
    file->readable = mode[0] == 'r' || plus;
    file->writable = mode[0] != 'r' || plus;
    file->close_fd = true;
    if (mode[0] == 'a' && plus)
        (void)dn_bionic_lseek(fd, 0, SEEK_SET);
    return file;
}

static void *dn_bionic_fdopen(int fd, const char *mode)
{
    struct dn_guest_file *file = (struct dn_guest_file *)calloc(1, sizeof(*file));
    if (!file)
        return NULL;
    file->fd = fd;
    file->readable = !mode || mode[0] == 'r' || (mode && strchr(mode, '+'));
    file->writable = mode && (mode[0] != 'r' || strchr(mode, '+'));
    file->close_fd = true;
    return file;
}

static int dn_bionic_fclose(void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (!file) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(EINVAL);
        return -1;
    }
    int rc = 0;
    if (file == &dn_std_files[0].file || file == &dn_std_files[1].file ||
        file == &dn_std_files[2].file)
        return 0;
    if (file->close_fd)
        rc = dn_bionic_close(file->fd);
    free(file);
    return rc;
}

static size_t dn_bionic_fread(void *ptr, size_t size, size_t nmemb, void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (!file || !ptr || size == 0 || nmemb == 0)
        return 0;
    size_t wanted = size * nmemb;
    size_t done = 0;
    uint8_t *out = (uint8_t *)ptr;
    if (file->has_unget && wanted) {
        out[done++] = (uint8_t)file->unget;
        file->has_unget = false;
    }
    while (done < wanted) {
        ssize_t got = dn_bionic_read(file->fd, out + done, wanted - done);
        if (got == 0) {
            file->eof = true;
            break;
        }
        if (got < 0) {
            file->error = true;
            break;
        }
        done += (size_t)got;
    }
    return done / size;
}

static size_t dn_bionic_fwrite(const void *ptr, size_t size, size_t nmemb, void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (!file || !ptr || size == 0 || nmemb == 0)
        return 0;
    size_t wanted = size * nmemb;
    size_t done = 0;
    const uint8_t *in = (const uint8_t *)ptr;
    file->has_unget = false;
    while (done < wanted) {
        ssize_t wrote = dn_bionic_write(file->fd, in + done, wanted - done);
        if (wrote <= 0) {
            file->error = true;
            break;
        }
        done += (size_t)wrote;
    }
    return done / size;
}

static int dn_bionic_fgetc(void *opaque)
{
    unsigned char ch;
    return dn_bionic_fread(&ch, 1, 1, opaque) == 1 ? ch : -1;
}

static int dn_bionic_getchar(void)
{
    return dn_bionic_fgetc(&dn_std_files[0].file);
}

static int dn_bionic_ungetc(int ch, void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (!file || ch < 0)
        return -1;
    file->unget = ch;
    file->has_unget = true;
    file->eof = false;
    return ch;
}

static int dn_bionic_fputc(int ch, void *opaque)
{
    unsigned char byte = (unsigned char)ch;
    return dn_bionic_fwrite(&byte, 1, 1, opaque) == 1 ? byte : -1;
}

static int dn_bionic_putchar(int ch)
{
    return dn_bionic_fputc(ch, &dn_std_files[1].file);
}

static int dn_bionic_fputs(const char *text, void *opaque)
{
    if (!text)
        return -1;
    size_t len = strlen(text);
    return dn_bionic_fwrite(text, 1, len, opaque) == len ? (int)len : -1;
}

static int dn_bionic_puts(const char *text)
{
    if (dn_bionic_fputs(text, &dn_std_files[1].file) < 0)
        return -1;
    return dn_bionic_putchar('\n');
}

static char *dn_bionic_fgets(char *buffer, int size, void *opaque)
{
    if (!buffer || size <= 0)
        return NULL;
    int i = 0;
    while (i + 1 < size) {
        int ch = dn_bionic_fgetc(opaque);
        if (ch < 0)
            break;
        buffer[i++] = (char)ch;
        if (ch == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    buffer[i] = '\0';
    return buffer;
}

static ssize_t dn_bionic_getdelim(char **lineptr, size_t *capacity, int delim, void *opaque)
{
    if (!lineptr || !capacity)
        return -1;
    size_t used = 0;
    for (;;) {
        int ch = dn_bionic_fgetc(opaque);
        if (ch < 0) {
            if (used == 0)
                return -1;
            break;
        }
        if (used + 2 > *capacity) {
            size_t new_capacity = *capacity ? *capacity * 2 : 128;
            char *next = (char *)realloc(*lineptr, new_capacity);
            if (!next)
                return -1;
            *lineptr = next;
            *capacity = new_capacity;
        }
        (*lineptr)[used++] = (char)ch;
        if (ch == delim)
            break;
    }
    if (*lineptr)
        (*lineptr)[used] = '\0';
    return (ssize_t)used;
}

static ssize_t dn_bionic_getline(char **lineptr, size_t *capacity, void *opaque)
{
    return dn_bionic_getdelim(lineptr, capacity, '\n', opaque);
}

static int dn_bionic_fseek(void *opaque, long offset, int whence)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (!file) return -1;
    file->has_unget = false;
    int64_t result = dn_bionic_lseek(file->fd, offset, whence);
    if (result < 0) {
        file->error = true;
        return -1;
    }
    file->eof = false;
    return 0;
}

static long dn_bionic_ftell(void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (!file) return -1;
    int64_t result = dn_bionic_lseek(file->fd, 0, SEEK_CUR);
    if (result > 0 && file->has_unget)
        result--;
    return (long)result;
}

static void dn_bionic_rewind(void *opaque)
{
    (void)dn_bionic_fseek(opaque, 0, SEEK_SET);
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (file) file->error = false;
}

static int dn_bionic_feof(void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    return file && file->eof ? 1 : 0;
}

static int dn_bionic_ferror(void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    return file && file->error ? 1 : 0;
}

static void dn_bionic_clearerr(void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    if (file) {
        file->eof = false;
        file->error = false;
    }
}

static int dn_bionic_fileno(void *opaque)
{
    struct dn_guest_file *file = (struct dn_guest_file *)opaque;
    return file ? file->fd : -1;
}

static int dn_bionic_fflush(void *opaque)
{
    (void)opaque;
    return 0;
}

static int dn_bionic_setvbuf(void *opaque, char *buffer, int mode, size_t size)
{
    (void)opaque; (void)buffer; (void)mode; (void)size;
    return 0;
}

static void dn_bionic_setbuf(void *opaque, char *buffer)
{
    (void)opaque; (void)buffer;
}

/* Memory mapping and allocation extras */
static void *dn_bionic_mmap(void *addr, size_t length, int prot, int flags,
                            int fd, int64_t offset)
{
    long result = dn_sys((long)(uintptr_t)addr, (long)length, prot, flags, fd, (long)offset, 222);
    if (dn_errno_from_linux_result(result) != 0)
        return (void *)-1;
    return (void *)(uintptr_t)result;
}

static int dn_bionic_munmap(void *addr, size_t length)
{
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)addr, (long)length, 0, 0, 0, 0, 215));
}

static int dn_bionic_mprotect(void *addr, size_t length, int prot)
{
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)addr, (long)length, prot, 0, 0, 0, 226));
}

static int dn_bionic_msync(void *addr, size_t length, int flags)
{
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)addr, (long)length, flags, 0, 0, 0, 227));
}

static int dn_bionic_madvise(void *addr, size_t length, int advice)
{
    (void)addr; (void)length; (void)advice;
    return 0;
}

static int dn_bionic_posix_memalign(void **out, size_t alignment, size_t size)
{
    int rc = posix_memalign(out, alignment, size);
    return dn_native_linux_errno_from_host(rc);
}

static void *dn_bionic_aligned_alloc(size_t alignment, size_t size)
{
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        dn_bionic_errno_value = dn_native_linux_errno_from_host(ENOMEM);
        return NULL;
    }
    return ptr;
}

static void *dn_bionic_memalign(size_t alignment, size_t size)
{
    return dn_bionic_aligned_alloc(alignment, size);
}

static void *dn_bionic_valloc(size_t size)
{
    long page = sysconf(_SC_PAGESIZE);
    return dn_bionic_aligned_alloc(page > 0 ? (size_t)page : 4096, size);
}

static size_t dn_bionic_malloc_usable_size(void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#else
    extern size_t malloc_usable_size(void *);
    return malloc_usable_size(ptr);
#endif
}

static int dn_bionic_mallopt(int param, int value)
{
    (void)param; (void)value;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Dynamic loading over DN's converted-library registry                */
/* ------------------------------------------------------------------ */

static char *dn_library_directory;
static _Thread_local char dn_dl_error_text[256];
static int dn_dl_global_handle_value;
static int dn_dl_system_handle_value;

void dn_native_set_library_directory(const char *path);

void dn_native_set_library_directory(const char *path)
{
    free(dn_library_directory);
    dn_library_directory = path ? strdup(path) : NULL;
}

struct dn_dl_handle {
    dn_native_image_t *image;
    void *dlopen_handle;
    bool owned;
};

static void dn_dl_set_error(const char *message)
{
    snprintf(dn_dl_error_text, sizeof(dn_dl_error_text), "%s", message ? message : "");
}

static bool dn_is_system_library_name(const char *name)
{
    static const char *const names[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so",
        "libvulkan.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so",
        "libGLESv3.so", "libOpenSLES.so", "libaaudio.so", "libmediandk.so",
        "libcamera2ndk.so", "libjnigraphics.so", "libz.so"
    };
    if (!name)
        return false;
    const char *leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(leaf, names[i]) == 0)
            return true;
    }
    return false;
}

static void *dn_bionic_dlopen(const char *filename, int flags)
{
    (void)flags;
    dn_dl_error_text[0] = '\0';
    if (!filename)
        return &dn_dl_global_handle_value;
    if (dn_is_system_library_name(filename))
        return &dn_dl_system_handle_value;

    char path[4096] = "";
    const char *candidate = filename;
    if (!strchr(filename, '/')) {
        if (!dn_library_directory) {
            dn_dl_set_error("DN native library directory is not configured");
            return NULL;
        }
        char leaf[512];
        if (strlen(filename) >= sizeof(leaf)) {
            dn_dl_set_error("library name too long");
            return NULL;
        }
        strcpy(leaf, filename);
        char *dot = strrchr(leaf, '.');
        if (dot && strcmp(dot, ".so") == 0)
            strcpy(dot, ".dylib");
        if (snprintf(path, sizeof(path), "%s/%s", dn_library_directory, leaf) >= (int)sizeof(path)) {
            dn_dl_set_error("library path too long");
            return NULL;
        }
        candidate = path;
    }

    (void)dlerror();
    void *handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *message = dlerror();
        dn_dl_set_error(message ? message : "dlopen failed");
        return NULL;
    }
    dn_native_image_t *image = NULL;
    int rc = dn_native_link_image(candidate, handle, &image);
    if (rc != 0 || !image) {
        if (image) {
            dn_dl_set_error(dn_native_image_report(image));
            dn_native_image_dispose(image);
        } else {
            dn_dl_set_error("DN mini-linker could not link the library");
        }
        dlclose(handle);
        return NULL;
    }
    struct dn_dl_handle *box = (struct dn_dl_handle *)calloc(1, sizeof(*box));
    if (!box) {
        dn_native_image_dispose(image);
        dlclose(handle);
        dn_dl_set_error("out of memory");
        return NULL;
    }
    box->image = image;
    box->dlopen_handle = handle;
    box->owned = true;
    return box;
}

static void *dn_bionic_dlsym(void *handle, const char *symbol)
{
    dn_dl_error_text[0] = '\0';
    if (!symbol)
        return NULL;
    if (!handle || handle == &dn_dl_global_handle_value || handle == (void *)-1) {
        void *found = dn_native_global_symbol(symbol);
        if (!found)
            dn_dl_set_error("symbol not found in DN global scope");
        return found;
    }
    if (handle == &dn_dl_system_handle_value) {
        void *shim = dn_native_shim_lookup(symbol);
        if (!shim)
            dn_dl_set_error("symbol is not implemented by the DroidNative compatibility layer");
        return shim;
    }
    struct dn_dl_handle *box = (struct dn_dl_handle *)handle;
    void *found = dn_native_image_symbol(box->image, symbol);
    if (!found)
        dn_dl_set_error("ELF symbol not found in converted library");
    return found;
}

static int dn_bionic_dlclose(void *handle)
{
    if (!handle || handle == &dn_dl_global_handle_value ||
        handle == &dn_dl_system_handle_value)
        return 0;
    struct dn_dl_handle *box = (struct dn_dl_handle *)handle;
    if (box->image) {
        (void)dn_native_image_run_finalizers(box->image);
        dn_native_image_dispose(box->image);
    }
    if (box->dlopen_handle)
        dlclose(box->dlopen_handle);
    free(box);
    return 0;
}

static char *dn_bionic_dlerror(void)
{
    if (!dn_dl_error_text[0])
        return NULL;
    char *result = dn_dl_error_text;
    /* POSIX clears the pending error on read; preserve the returned
     * pointer by moving the text to a second thread-local slot. */
    static _Thread_local char returned[256];
    strcpy(returned, result);
    dn_dl_error_text[0] = '\0';
    return returned;
}

static int dn_bionic_dladdr(const void *addr, void *info)
{
    (void)addr;
    if (info)
        memset(info, 0, sizeof(void *) * 4);
    return 0;
}

static int dn_bionic_dl_iterate_phdr(void *callback, void *data)
{
    (void)callback;
    (void)data;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Process/time/system facts                                           */
/* ------------------------------------------------------------------ */

static long dn_bionic_syscall(long number, long a0, long a1, long a2,
                              long a3, long a4, long a5)
{
    long result = dn_sys(a0, a1, a2, a3, a4, a5, number);
    if (result < 0) {
        dn_bionic_errno_value = (int)(-result);
        return -1;
    }
    return result;
}

static int dn_bionic_gettid(void)
{
    return (int)dn_sys(0, 0, 0, 0, 0, 0, 178);
}

static long dn_bionic_sysconf(int name)
{
    switch (name) {
    case 0: return 128 * 1024;             /* _SC_ARG_MAX (conservative) */
    case 2: return 100;                    /* _SC_CLK_TCK */
    case 4: return 1024;                   /* _SC_OPEN_MAX */
    case 30: return sysconf(_SC_PAGESIZE); /* _SC_PAGESIZE */
    case 39: return 1;                     /* _SC_THREAD_SAFE_FUNCTIONS */
    case 83: return sysconf(_SC_NPROCESSORS_CONF);
    case 84: return sysconf(_SC_NPROCESSORS_ONLN);
    case 85: return sysconf(_SC_PHYS_PAGES);
    case 86: return sysconf(_SC_PHYS_PAGES);
    case 89: return 200809L;               /* _SC_VERSION */
    case 149: return 200809L;              /* _SC_MONOTONIC_CLOCK */
    case 154: return 200809L;              /* _SC_CPUTIME */
    case 155: return 200809L;              /* _SC_THREAD_CPUTIME */
    default:
        dn_bionic_errno_value = dn_native_linux_errno_from_host(EINVAL);
        return -1;
    }
}

static unsigned long dn_bionic_getauxval(unsigned long type)
{
    static unsigned char random_bytes[16] = {
        0x44, 0x72, 0x6f, 0x69, 0x64, 0x4e, 0x61, 0x74,
        0x69, 0x76, 0x65, 0x41, 0x52, 0x4d, 0x36, 0x34
    };
    switch (type) {
    case 6: { long p = sysconf(_SC_PAGESIZE); return p > 0 ? (unsigned long)p : 4096; }
    case 11: return (unsigned long)getuid();
    case 12: return (unsigned long)geteuid();
    case 13: return (unsigned long)getgid();
    case 14: return (unsigned long)getegid();
    case 16: return 0xffffffffUL;      /* AT_HWCAP: broad AArch64 user features */
    case 17: return 100;               /* AT_CLKTCK */
    case 23: return 0;                 /* AT_SECURE */
    case 25: return (unsigned long)(uintptr_t)random_bytes;
    case 26: return 0xffffffffUL;      /* AT_HWCAP2 */
    case 31: return (unsigned long)(uintptr_t)"iDroid";
    default: return 0;
    }
}

static int dn_bionic_uname(void *out)
{
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)out, 0, 0, 0, 0, 0, 160));
}

static int dn_bionic_gettimeofday(void *timeval_out, void *timezone_out)
{
    (void)timezone_out;
    return dn_errno_from_linux_result(
        dn_sys((long)(uintptr_t)timeval_out, 0, 0, 0, 0, 0, 169));
}

static int dn_bionic_nanosleep(const struct timespec *request, struct timespec *remain)
{
    return dn_errno_from_linux_result(dn_sys(
        (long)(uintptr_t)request, (long)(uintptr_t)remain, 0, 0, 0, 0, 101));
}

static unsigned int dn_bionic_sleep(unsigned int seconds)
{
    struct timespec request = { (time_t)seconds, 0 };
    struct timespec remain = { 0, 0 };
    if (dn_bionic_nanosleep(&request, &remain) == 0)
        return 0;
    return (unsigned int)remain.tv_sec + (remain.tv_nsec ? 1 : 0);
}

static int dn_bionic_isatty(int fd)
{
    int rc = isatty(fd);
    if (!rc)
        dn_bionic_errno_value = dn_native_linux_errno_from_host(errno ? errno : ENOTTY);
    return rc;
}

static int dn_bionic_getpagesize(void)
{
    long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? (int)page : 4096;
}

static long dn_bionic_page_size_value = 4096;

/* ------------------------------------------------------------------ */
/* Lookup + self-test                                                  */
/* ------------------------------------------------------------------ */

struct dn_bionic_entry {
    const char *name;
    void *address;
};

static const struct dn_bionic_entry dn_bionic_entries[] = {
    { "__errno", (void *)dn_native_errno_location },
    { "__errno_location", (void *)dn_native_errno_location },

    { "pthread_mutex_init", (void *)dn_pthread_mutex_init },
    { "pthread_mutex_destroy", (void *)dn_pthread_mutex_destroy },
    { "pthread_mutex_lock", (void *)dn_pthread_mutex_lock },
    { "pthread_mutex_trylock", (void *)dn_pthread_mutex_trylock },
    { "pthread_mutex_unlock", (void *)dn_pthread_mutex_unlock },
    { "pthread_mutex_timedlock", (void *)dn_pthread_mutex_timedlock },
    { "pthread_mutexattr_init", (void *)dn_pthread_mutexattr_init },
    { "pthread_mutexattr_destroy", (void *)dn_pthread_mutexattr_destroy },
    { "pthread_mutexattr_settype", (void *)dn_pthread_mutexattr_settype },
    { "pthread_mutexattr_gettype", (void *)dn_pthread_mutexattr_gettype },
    { "pthread_mutexattr_setpshared", (void *)dn_pthread_mutexattr_setpshared },
    { "pthread_mutexattr_getpshared", (void *)dn_pthread_mutexattr_getpshared },
    { "pthread_cond_init", (void *)dn_pthread_cond_init },
    { "pthread_cond_destroy", (void *)dn_pthread_cond_destroy },
    { "pthread_cond_signal", (void *)dn_pthread_cond_signal },
    { "pthread_cond_broadcast", (void *)dn_pthread_cond_broadcast },
    { "pthread_cond_wait", (void *)dn_pthread_cond_wait },
    { "pthread_cond_timedwait", (void *)dn_pthread_cond_timedwait },
    { "pthread_cond_clockwait", (void *)dn_pthread_cond_clockwait },
    { "pthread_condattr_init", (void *)dn_pthread_condattr_init },
    { "pthread_condattr_destroy", (void *)dn_pthread_condattr_destroy },
    { "pthread_condattr_setclock", (void *)dn_pthread_condattr_setclock },
    { "pthread_condattr_getclock", (void *)dn_pthread_condattr_getclock },
    { "pthread_condattr_setpshared", (void *)dn_pthread_condattr_setpshared },
    { "pthread_condattr_getpshared", (void *)dn_pthread_condattr_getpshared },
    { "pthread_rwlock_init", (void *)dn_pthread_rwlock_init },
    { "pthread_rwlock_destroy", (void *)dn_pthread_rwlock_destroy },
    { "pthread_rwlock_rdlock", (void *)dn_pthread_rwlock_rdlock },
    { "pthread_rwlock_tryrdlock", (void *)dn_pthread_rwlock_tryrdlock },
    { "pthread_rwlock_wrlock", (void *)dn_pthread_rwlock_wrlock },
    { "pthread_rwlock_trywrlock", (void *)dn_pthread_rwlock_trywrlock },
    { "pthread_rwlock_unlock", (void *)dn_pthread_rwlock_unlock },
#if !defined(__APPLE__)
    { "pthread_rwlock_timedrdlock", (void *)dn_pthread_rwlock_timedrdlock },
    { "pthread_rwlock_timedwrlock", (void *)dn_pthread_rwlock_timedwrlock },
#endif
    { "pthread_rwlockattr_init", (void *)dn_pthread_rwlockattr_init },
    { "pthread_rwlockattr_destroy", (void *)dn_pthread_rwlockattr_destroy },
    { "pthread_rwlockattr_setpshared", (void *)dn_pthread_rwlockattr_setpshared },
    { "pthread_rwlockattr_getpshared", (void *)dn_pthread_rwlockattr_getpshared },
    { "pthread_rwlockattr_setkind_np", (void *)dn_pthread_rwlockattr_setkind_np },
    { "pthread_rwlockattr_getkind_np", (void *)dn_pthread_rwlockattr_getkind_np },
    { "pthread_once", (void *)dn_pthread_once },
    { "pthread_key_create", (void *)dn_pthread_key_create },
    { "pthread_key_delete", (void *)dn_pthread_key_delete },
    { "pthread_setspecific", (void *)dn_pthread_setspecific },
    { "pthread_getspecific", (void *)dn_pthread_getspecific },
    { "pthread_attr_init", (void *)dn_pthread_attr_init },
    { "pthread_attr_destroy", (void *)dn_pthread_attr_destroy },
    { "pthread_attr_setdetachstate", (void *)dn_pthread_attr_setdetachstate },
    { "pthread_attr_getdetachstate", (void *)dn_pthread_attr_getdetachstate },
    { "pthread_attr_setstacksize", (void *)dn_pthread_attr_setstacksize },
    { "pthread_attr_getstacksize", (void *)dn_pthread_attr_getstacksize },
    { "pthread_attr_setstack", (void *)dn_pthread_attr_setstack },
    { "pthread_attr_getstack", (void *)dn_pthread_attr_getstack },
    { "pthread_attr_setguardsize", (void *)dn_pthread_attr_setguardsize },
    { "pthread_attr_getguardsize", (void *)dn_pthread_attr_getguardsize },
    { "pthread_attr_setschedpolicy", (void *)dn_pthread_attr_setschedpolicy },
    { "pthread_attr_getschedpolicy", (void *)dn_pthread_attr_getschedpolicy },
    { "pthread_attr_setschedparam", (void *)dn_pthread_attr_setschedparam },
    { "pthread_attr_getschedparam", (void *)dn_pthread_attr_getschedparam },
    { "pthread_attr_setscope", (void *)dn_pthread_attr_setscope },
    { "pthread_attr_getscope", (void *)dn_pthread_attr_getscope },
    { "pthread_attr_setinheritsched", (void *)dn_pthread_attr_setinheritsched },
    { "pthread_attr_getinheritsched", (void *)dn_pthread_attr_getinheritsched },
    { "pthread_create", (void *)dn_pthread_create },
    { "pthread_join", (void *)dn_pthread_join },
    { "pthread_detach", (void *)dn_pthread_detach },
    { "pthread_self", (void *)dn_pthread_self },
    { "pthread_equal", (void *)dn_pthread_equal },
    { "pthread_exit", (void *)dn_pthread_exit },
    { "pthread_cancel", (void *)dn_pthread_cancel },
    { "pthread_setcancelstate", (void *)dn_pthread_setcancelstate },
    { "pthread_setcanceltype", (void *)dn_pthread_setcanceltype },
    { "pthread_testcancel", (void *)dn_pthread_testcancel },
    { "pthread_setschedparam", (void *)dn_pthread_setschedparam },
    { "pthread_getschedparam", (void *)dn_pthread_getschedparam },
    { "pthread_getattr_np", (void *)dn_pthread_getattr_np },
    { "pthread_kill", (void *)dn_pthread_kill },
    { "pthread_sigmask", (void *)dn_pthread_sigmask },
    { "sem_init", (void *)dn_sem_init },
    { "sem_destroy", (void *)dn_sem_destroy },
    { "sem_wait", (void *)dn_sem_wait },
    { "sem_trywait", (void *)dn_sem_trywait },
    { "sem_timedwait", (void *)dn_sem_timedwait },
    { "sem_post", (void *)dn_sem_post },
    { "sem_getvalue", (void *)dn_sem_getvalue },
    { "pthread_barrier_init", (void *)dn_pthread_barrier_init },
    { "pthread_barrier_destroy", (void *)dn_pthread_barrier_destroy },
    { "pthread_barrier_wait", (void *)dn_pthread_barrier_wait },
    { "pthread_barrierattr_init", (void *)dn_pthread_barrierattr_init },
    { "pthread_barrierattr_destroy", (void *)dn_pthread_barrierattr_destroy },
    { "pthread_barrierattr_setpshared", (void *)dn_pthread_barrierattr_setpshared },
    { "pthread_barrierattr_getpshared", (void *)dn_pthread_barrierattr_getpshared },

    { "open", (void *)dn_bionic_open },
    { "open64", (void *)dn_bionic_open },
    { "__open_2", (void *)dn_bionic_open },
    { "openat", (void *)dn_bionic_openat },
    { "openat64", (void *)dn_bionic_openat },
    { "__openat_2", (void *)dn_bionic_openat },
    { "close", (void *)dn_bionic_close },
    { "read", (void *)dn_bionic_read },
    { "write", (void *)dn_bionic_write },
    { "readv", (void *)dn_bionic_readv },
    { "writev", (void *)dn_bionic_writev },
    { "pread", (void *)dn_bionic_pread },
    { "pread64", (void *)dn_bionic_pread },
    { "pwrite", (void *)dn_bionic_pwrite },
    { "pwrite64", (void *)dn_bionic_pwrite },
    { "lseek", (void *)dn_bionic_lseek },
    { "lseek64", (void *)dn_bionic_lseek },
    { "dup", (void *)dn_bionic_dup },
    { "dup2", (void *)dn_bionic_dup2 },
    { "dup3", (void *)dn_bionic_dup3 },
    { "fcntl", (void *)dn_bionic_fcntl },
    { "fcntl64", (void *)dn_bionic_fcntl },
    { "ioctl", (void *)dn_bionic_ioctl },
    { "access", (void *)dn_bionic_access },
    { "faccessat", (void *)dn_bionic_faccessat },
    { "stat", (void *)dn_bionic_stat },
    { "stat64", (void *)dn_bionic_stat },
    { "lstat", (void *)dn_bionic_lstat },
    { "lstat64", (void *)dn_bionic_lstat },
    { "fstat", (void *)dn_bionic_fstat },
    { "fstat64", (void *)dn_bionic_fstat },
    { "fstatat", (void *)dn_bionic_fstatat },
    { "fstatat64", (void *)dn_bionic_fstatat },
    { "__xstat", (void *)dn_bionic_stat },
    { "__lxstat", (void *)dn_bionic_lstat },
    { "__fxstat", (void *)dn_bionic_fstat },
    { "mkdir", (void *)dn_bionic_mkdir },
    { "mkdirat", (void *)dn_bionic_mkdirat },
    { "rmdir", (void *)dn_bionic_rmdir },
    { "unlink", (void *)dn_bionic_unlink },
    { "unlinkat", (void *)dn_bionic_unlinkat },
    { "rename", (void *)dn_bionic_rename },
    { "renameat", (void *)dn_bionic_renameat },
    { "link", (void *)dn_bionic_link },
    { "symlink", (void *)dn_bionic_symlink },
    { "readlink", (void *)dn_bionic_readlink },
    { "readlinkat", (void *)dn_bionic_readlinkat },
    { "chdir", (void *)dn_bionic_chdir },
    { "fchdir", (void *)dn_bionic_fchdir },
    { "getcwd", (void *)dn_bionic_getcwd },
    { "chmod", (void *)dn_bionic_chmod },
    { "fchmod", (void *)dn_bionic_fchmod },
    { "fchmodat", (void *)dn_bionic_fchmodat },
    { "fchown", (void *)dn_bionic_fchown },
    { "truncate", (void *)dn_bionic_truncate },
    { "truncate64", (void *)dn_bionic_truncate },
    { "ftruncate", (void *)dn_bionic_ftruncate },
    { "ftruncate64", (void *)dn_bionic_ftruncate },
    { "fsync", (void *)dn_bionic_fsync },
    { "fdatasync", (void *)dn_bionic_fdatasync },
    { "pipe", (void *)dn_bionic_pipe },
    { "pipe2", (void *)dn_bionic_pipe2 },
    { "poll", (void *)dn_bionic_poll },
    { "select", (void *)dn_bionic_select },
    { "opendir", (void *)dn_bionic_opendir },
    { "fdopendir", (void *)dn_bionic_fdopendir },
    { "readdir", (void *)dn_bionic_readdir },
    { "readdir64", (void *)dn_bionic_readdir },
    { "readdir_r", (void *)dn_bionic_readdir_r },
    { "closedir", (void *)dn_bionic_closedir },
    { "rewinddir", (void *)dn_bionic_rewinddir },
    { "dirfd", (void *)dn_bionic_dirfd },
    { "telldir", (void *)dn_bionic_telldir },
    { "seekdir", (void *)dn_bionic_seekdir },

    { "fopen", (void *)dn_bionic_fopen },
    { "fopen64", (void *)dn_bionic_fopen },
    { "fdopen", (void *)dn_bionic_fdopen },
    { "fclose", (void *)dn_bionic_fclose },
    { "fread", (void *)dn_bionic_fread },
    { "fwrite", (void *)dn_bionic_fwrite },
    { "fgetc", (void *)dn_bionic_fgetc },
    { "getc", (void *)dn_bionic_fgetc },
    { "getchar", (void *)dn_bionic_getchar },
    { "ungetc", (void *)dn_bionic_ungetc },
    { "fputc", (void *)dn_bionic_fputc },
    { "putc", (void *)dn_bionic_fputc },
    { "putchar", (void *)dn_bionic_putchar },
    { "fputs", (void *)dn_bionic_fputs },
    { "puts", (void *)dn_bionic_puts },
    { "fgets", (void *)dn_bionic_fgets },
    { "getline", (void *)dn_bionic_getline },
    { "getdelim", (void *)dn_bionic_getdelim },
    { "fseek", (void *)dn_bionic_fseek },
    { "fseeko", (void *)dn_bionic_fseek },
    { "ftell", (void *)dn_bionic_ftell },
    { "ftello", (void *)dn_bionic_ftell },
    { "rewind", (void *)dn_bionic_rewind },
    { "feof", (void *)dn_bionic_feof },
    { "ferror", (void *)dn_bionic_ferror },
    { "clearerr", (void *)dn_bionic_clearerr },
    { "fileno", (void *)dn_bionic_fileno },
    { "fflush", (void *)dn_bionic_fflush },
    { "setvbuf", (void *)dn_bionic_setvbuf },
    { "setbuf", (void *)dn_bionic_setbuf },
    { "stdin", (void *)&dn_stdin_pointer },
    { "stdout", (void *)&dn_stdout_pointer },
    { "stderr", (void *)&dn_stderr_pointer },
    { "__sF", (void *)dn_std_files },

    { "mmap", (void *)dn_bionic_mmap },
    { "mmap64", (void *)dn_bionic_mmap },
    { "munmap", (void *)dn_bionic_munmap },
    { "mprotect", (void *)dn_bionic_mprotect },
    { "msync", (void *)dn_bionic_msync },
    { "madvise", (void *)dn_bionic_madvise },
    { "posix_memalign", (void *)dn_bionic_posix_memalign },
    { "aligned_alloc", (void *)dn_bionic_aligned_alloc },
    { "memalign", (void *)dn_bionic_memalign },
    { "valloc", (void *)dn_bionic_valloc },
    { "malloc_usable_size", (void *)dn_bionic_malloc_usable_size },
    { "mallopt", (void *)dn_bionic_mallopt },

    { "dlopen", (void *)dn_bionic_dlopen },
    { "dlsym", (void *)dn_bionic_dlsym },
    { "dlclose", (void *)dn_bionic_dlclose },
    { "dlerror", (void *)dn_bionic_dlerror },
    { "dladdr", (void *)dn_bionic_dladdr },
    { "dl_iterate_phdr", (void *)dn_bionic_dl_iterate_phdr },
    { "dn_native_set_library_directory", (void *)dn_native_set_library_directory },

    { "syscall", (void *)dn_bionic_syscall },
    { "gettid", (void *)dn_bionic_gettid },
    { "sysconf", (void *)dn_bionic_sysconf },
    { "getauxval", (void *)dn_bionic_getauxval },
    { "uname", (void *)dn_bionic_uname },
    { "gettimeofday", (void *)dn_bionic_gettimeofday },
    { "nanosleep", (void *)dn_bionic_nanosleep },
    { "sleep", (void *)dn_bionic_sleep },
    { "isatty", (void *)dn_bionic_isatty },
    { "getpagesize", (void *)dn_bionic_getpagesize },
    { "page_size", (void *)&dn_bionic_page_size_value },
};

void *dn_native_bionic_lookup(const char *name)
{
    if (!name)
        return NULL;
    const size_t count = sizeof(dn_bionic_entries) / sizeof(dn_bionic_entries[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(name, dn_bionic_entries[i].name) == 0)
            return dn_bionic_entries[i].address;
    }
    return NULL;
}

static void dn_bionic_append(char *out, size_t out_len, const char *fmt, ...)
{
    if (!out || out_len == 0)
        return;
    size_t used = strlen(out);
    if (used >= out_len - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, out_len - used, fmt, ap);
    va_end(ap);
    size_t len = strlen(out);
    if (len + 1 < out_len && (len == 0 || out[len - 1] != '\n')) {
        out[len] = '\n';
        out[len + 1] = '\0';
    }
}

static void *dn_bionic_selftest_thread(void *arg)
{
    uint32_t key = *(uint32_t *)arg;
    (void)dn_pthread_setspecific(key, (void *)(uintptr_t)0x2a);
    void *value = dn_pthread_getspecific(key);
    return value;
}

int dn_native_bionic_run_selftest(char *out, size_t out_len)
{
    if (out && out_len)
        out[0] = '\0';
    int failures = 0;

    _Alignas(16) uint8_t mutex_storage[64] = { 0 };
    bool mutex_ok = dn_pthread_mutex_lock(mutex_storage) == 0 &&
                    dn_pthread_mutex_trylock(mutex_storage) != 0 &&
                    dn_pthread_mutex_unlock(mutex_storage) == 0 &&
                    dn_pthread_mutex_destroy(mutex_storage) == 0;
    dn_bionic_append(out, out_len, "guest pthread mutex registry: %s", mutex_ok ? "OK" : "MISMATCH");
    failures += mutex_ok ? 0 : 1;

    uint32_t key = UINT32_MAX;
    dn_guest_pthread_t thread = 0;
    void *thread_result = NULL;
    bool thread_ok = dn_pthread_key_create(&key, NULL) == 0 &&
                     dn_pthread_create(&thread, NULL, dn_bionic_selftest_thread, &key) == 0 &&
                     dn_pthread_join(thread, &thread_result) == 0 &&
                     thread_result == (void *)(uintptr_t)0x2a;
    if (key != UINT32_MAX)
        (void)dn_pthread_key_delete(key);
    dn_bionic_append(out, out_len, "guest pthread create/key/join: %s", thread_ok ? "OK" : "MISMATCH");
    failures += thread_ok ? 0 : 1;

    _Alignas(16) uint8_t sem_storage[64] = { 0 };
    int sem_value = -1;
    bool sem_ok = dn_sem_init(sem_storage, 0, 0) == 0 &&
                  dn_sem_post(sem_storage) == 0 &&
                  dn_sem_wait(sem_storage) == 0 &&
                  dn_sem_getvalue(sem_storage, &sem_value) == 0 && sem_value == 0 &&
                  dn_sem_destroy(sem_storage) == 0;
    dn_bionic_append(out, out_len, "guest semaphore: %s", sem_ok ? "OK" : "MISMATCH");
    failures += sem_ok ? 0 : 1;

    void *file = dn_bionic_fopen(
        "/data/data/ro.droidnative.app/files/dn-bionic-selftest.txt", "w+");
    bool file_ok = false;
    if (file) {
        char buffer[16] = "";
        file_ok = dn_bionic_fputs("bionic42\n", file) >= 0 &&
                  dn_bionic_fseek(file, 0, SEEK_SET) == 0 &&
                  dn_bionic_fgets(buffer, sizeof(buffer), file) &&
                  strcmp(buffer, "bionic42\n") == 0;
        (void)dn_bionic_fclose(file);
    }
    dn_bionic_append(out, out_len, "guest stdio over DN VFS: %s", file_ok ? "OK" : "MISMATCH");
    failures += file_ok ? 0 : 1;

    void *sf = dn_native_bionic_lookup("__sF");
    void **stdin_slot = (void **)dn_native_bionic_lookup("stdin");
    bool std_streams_ok = sf == (void *)dn_std_files && stdin_slot &&
                          *stdin_slot == (void *)&dn_std_files[0].file &&
                          (char *)&dn_std_files[1] - (char *)&dn_std_files[0] == 152 &&
                          dn_std_files[0].file.fd == 0 &&
                          dn_std_files[1].file.fd == 1 &&
                          dn_std_files[2].file.fd == 2;
    dn_bionic_append(out, out_len, "guest stdin/__sF stream slots: %s", std_streams_ok ? "OK" : "MISMATCH");
    failures += std_streams_ok ? 0 : 1;

    typedef size_t (*strlen_fn)(const char *);
    void *handle = dn_bionic_dlopen("libc.so", 0);
    strlen_fn guest_strlen = handle ? (strlen_fn)dn_bionic_dlsym(handle, "strlen") : NULL;
    bool dl_ok = guest_strlen && guest_strlen("droidnative") == 11;
    dn_bionic_append(out, out_len, "guest dlopen/dlsym system handle: %s", dl_ok ? "OK" : "MISMATCH");
    failures += dl_ok ? 0 : 1;

    int pipefds[2] = { -1, -1 };
    bool select_ok = dn_bionic_pipe(pipefds) == 0;
    if (select_ok) {
        uint64_t readset[16] = { 0 };
        struct timeval timeout = { 0, 0 };
        (void)dn_bionic_write(pipefds[1], "x", 1);
        readset[pipefds[0] / 64] |= 1ULL << (pipefds[0] % 64);
        select_ok = dn_bionic_select(pipefds[0] + 1, readset, NULL, NULL, &timeout) == 1;
        (void)dn_bionic_close(pipefds[0]);
        (void)dn_bionic_close(pipefds[1]);
    }
    dn_bionic_append(out, out_len, "guest pipe/select: %s", select_ok ? "OK" : "MISMATCH");
    failures += select_ok ? 0 : 1;

    dn_bionic_append(out, out_len, "%s", failures == 0 ? "host verdict: PASS" : "host verdict: FAIL");
    return failures == 0 ? 0 : -1;
}
