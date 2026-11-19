#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Same Darwin feature-exposure rule as DNNativeLinker.c: do not let a
 * POSIX-only feature level hide Darwin declarations on Apple SDKs. */
#define _DARWIN_C_SOURCE 1
#endif

#include "DNNativeShims.h"
#include "DNNativeBionic.h"
#include "DNNativeAndroid.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Small self-test surface (translation/link)                         */
/* ------------------------------------------------------------------ */

int dn_native_selftest_add(int a, int b)
{
    return a + b;
}

size_t dn_native_selftest_strlen(const char *s)
{
    return s ? strlen(s) : 0;
}

/* ------------------------------------------------------------------ */
/* bionic errno + stack guard                                         */
/* ------------------------------------------------------------------ */

static _Thread_local int dn_bionic_errno_value;

static int *dn_shim_errno_location(void)
{
    return &dn_bionic_errno_value;
}

/* Some Android objects import the guard as a variable.  Most real code
 * reads it from the bionic thread-pointer slot; that path arrives with
 * the DN patch-island/TLS driver, not guessed here. */
static uintptr_t dn_stack_chk_guard_storage = (uintptr_t)0x44524f4944313631ULL;

static void dn_shim_stack_chk_fail(void)
{
    fputs("[native-runtime] stack protector failure in translated code\n", stderr);
    abort();
}

/* ------------------------------------------------------------------ */
/* Itanium C++ ABI pieces that are ABI-stable enough to provide now   */
/* ------------------------------------------------------------------ */

typedef void (*dn_cxa_destructor_fn)(void *);

struct dn_cxa_atexit_entry {
    dn_cxa_destructor_fn fn;
    void *arg;
    void *dso;
    bool done;
};

enum { DN_CXA_ATEXIT_MAX = 256 };

static struct dn_cxa_atexit_entry dn_cxa_entries[DN_CXA_ATEXIT_MAX];
static size_t dn_cxa_entry_count;
static pthread_mutex_t dn_cxa_mutex = PTHREAD_MUTEX_INITIALIZER;

static int dn_shim_cxa_atexit(dn_cxa_destructor_fn fn, void *arg, void *dso)
{
    if (!fn)
        return -1;
    pthread_mutex_lock(&dn_cxa_mutex);
    if (dn_cxa_entry_count >= DN_CXA_ATEXIT_MAX) {
        pthread_mutex_unlock(&dn_cxa_mutex);
        return -1;
    }
    dn_cxa_entries[dn_cxa_entry_count].fn = fn;
    dn_cxa_entries[dn_cxa_entry_count].arg = arg;
    dn_cxa_entries[dn_cxa_entry_count].dso = dso;
    dn_cxa_entries[dn_cxa_entry_count].done = false;
    dn_cxa_entry_count++;
    pthread_mutex_unlock(&dn_cxa_mutex);
    return 0;
}

static void dn_shim_cxa_finalize(void *dso)
{
    for (;;) {
        dn_cxa_destructor_fn fn = NULL;
        void *arg = NULL;

        pthread_mutex_lock(&dn_cxa_mutex);
        for (size_t i = dn_cxa_entry_count; i > 0; --i) {
            struct dn_cxa_atexit_entry *e = &dn_cxa_entries[i - 1];
            if (e->done)
                continue;
            if (dso && e->dso != dso)
                continue;
            e->done = true;
            fn = e->fn;
            arg = e->arg;
            break;
        }
        pthread_mutex_unlock(&dn_cxa_mutex);

        if (!fn)
            return;
        fn(arg);
    }
}

static pthread_mutex_t dn_cxa_guard_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dn_cxa_guard_cond = PTHREAD_COND_INITIALIZER;

static int dn_shim_cxa_guard_acquire(uint64_t *guard)
{
    unsigned char *bytes = (unsigned char *)guard;
    if (!bytes)
        return 0;

    pthread_mutex_lock(&dn_cxa_guard_mutex);
    while (!bytes[0] && bytes[1])
        pthread_cond_wait(&dn_cxa_guard_cond, &dn_cxa_guard_mutex);
    if (bytes[0]) {
        pthread_mutex_unlock(&dn_cxa_guard_mutex);
        return 0;
    }
    bytes[1] = 1;
    pthread_mutex_unlock(&dn_cxa_guard_mutex);
    return 1;
}

static void dn_shim_cxa_guard_release(uint64_t *guard)
{
    unsigned char *bytes = (unsigned char *)guard;
    if (!bytes)
        return;
    pthread_mutex_lock(&dn_cxa_guard_mutex);
    bytes[0] = 1;
    bytes[1] = 0;
    pthread_cond_broadcast(&dn_cxa_guard_cond);
    pthread_mutex_unlock(&dn_cxa_guard_mutex);
}

static void dn_shim_cxa_guard_abort(uint64_t *guard)
{
    unsigned char *bytes = (unsigned char *)guard;
    if (!bytes)
        return;
    pthread_mutex_lock(&dn_cxa_guard_mutex);
    bytes[1] = 0;
    pthread_cond_broadcast(&dn_cxa_guard_cond);
    pthread_mutex_unlock(&dn_cxa_guard_mutex);
}

static void dn_shim_cxa_pure_virtual(void)
{
    fputs("[native-runtime] pure virtual function called\n", stderr);
    abort();
}

struct dn_thread_atexit_entry {
    dn_cxa_destructor_fn fn;
    void *arg;
    void *dso;
    struct dn_thread_atexit_entry *next;
};

static pthread_key_t dn_thread_atexit_key;
static pthread_once_t dn_thread_atexit_once = PTHREAD_ONCE_INIT;

static void dn_thread_atexit_destroy(void *opaque)
{
    struct dn_thread_atexit_entry *entry = (struct dn_thread_atexit_entry *)opaque;
    while (entry) {
        struct dn_thread_atexit_entry *next = entry->next;
        entry->fn(entry->arg);
        free(entry);
        entry = next;
    }
}

static void dn_thread_atexit_key_init(void)
{
    (void)pthread_key_create(&dn_thread_atexit_key, dn_thread_atexit_destroy);
}

static int dn_shim_cxa_thread_atexit_impl(dn_cxa_destructor_fn fn, void *arg, void *dso)
{
    if (!fn)
        return -1;
    pthread_once(&dn_thread_atexit_once, dn_thread_atexit_key_init);
    struct dn_thread_atexit_entry *entry =
        (struct dn_thread_atexit_entry *)malloc(sizeof(*entry));
    if (!entry)
        return -1;
    entry->fn = fn;
    entry->arg = arg;
    entry->dso = dso;
    entry->next = (struct dn_thread_atexit_entry *)pthread_getspecific(dn_thread_atexit_key);
    return pthread_setspecific(dn_thread_atexit_key, entry) == 0 ? 0 : -1;
}

struct dn_emutls_control {
    size_t size;
    size_t alignment;
    union {
        uintptr_t index;
        void *address;
    } object;
    void *value;
};

static pthread_key_t dn_emutls_key;
static pthread_once_t dn_emutls_once = PTHREAD_ONCE_INIT;
static uintptr_t dn_emutls_next_index = 1;
static pthread_mutex_t dn_emutls_mutex = PTHREAD_MUTEX_INITIALIZER;

static void dn_emutls_destroy(void *opaque)
{
    void **array = (void **)opaque;
    if (!array)
        return;
    uintptr_t count = (uintptr_t)array[0];
    for (uintptr_t i = 1; i <= count; ++i)
        free(array[i]);
    free(array);
}

static void dn_emutls_key_init(void)
{
    (void)pthread_key_create(&dn_emutls_key, dn_emutls_destroy);
}

static void *dn_shim_emutls_get_address(struct dn_emutls_control *control)
{
    if (!control)
        return NULL;
    pthread_once(&dn_emutls_once, dn_emutls_key_init);
    pthread_mutex_lock(&dn_emutls_mutex);
    if (control->object.index == 0)
        control->object.index = dn_emutls_next_index++;
    uintptr_t index = control->object.index;
    pthread_mutex_unlock(&dn_emutls_mutex);

    void **array = (void **)pthread_getspecific(dn_emutls_key);
    uintptr_t capacity = array ? (uintptr_t)array[0] : 0;
    if (index > capacity) {
        uintptr_t new_capacity = capacity ? capacity : 16;
        while (new_capacity < index)
            new_capacity *= 2;
        void **next = (void **)realloc(array, (new_capacity + 1) * sizeof(void *));
        if (!next)
            return NULL;
        for (uintptr_t i = capacity + 1; i <= new_capacity; ++i)
            next[i] = NULL;
        next[0] = (void *)new_capacity;
        array = next;
        if (pthread_setspecific(dn_emutls_key, array) != 0)
            return NULL;
    }
    if (!array[index]) {
        size_t alignment = control->alignment ? control->alignment : sizeof(void *);
        if (alignment < sizeof(void *))
            alignment = sizeof(void *);
        void *storage = NULL;
        if (posix_memalign(&storage, alignment, control->size ? control->size : 1) != 0)
            return NULL;
        if (control->value)
            memcpy(storage, control->value, control->size);
        else
            memset(storage, 0, control->size);
        array[index] = storage;
    }
    return array[index];
}

static void dn_shim_assert2(const char *file, int line, const char *function,
                            const char *expression)
{
    fprintf(stderr, "[native-runtime] assertion failed: %s (%s:%d in %s)\n",
            expression ? expression : "<unknown>", file ? file : "<file>",
            line, function ? function : "<function>");
    abort();
}

static void dn_shim_assert_fail(const char *assertion, const char *file,
                                unsigned int line, const char *function)
{
    dn_shim_assert2(file, (int)line, function, assertion);
}

static int dn_shim_register_atfork(void (*prepare)(void), void (*parent)(void),
                                   void (*child)(void), void *dso)
{
    (void)dso;
    return pthread_atfork(prepare, parent, child);
}

/* ------------------------------------------------------------------ */
/* Android log + clock wrappers                                       */
/* ------------------------------------------------------------------ */

static int dn_shim_android_log_write(int prio, const char *tag, const char *text)
{
    fprintf(stderr, "[android-log %d %s] %s\n", prio,
            tag ? tag : "(null)", text ? text : "(null)");
    return 1;
}

static int dn_shim_clock_gettime(int linux_clock_id, struct timespec *ts)
{
    clockid_t host_clock;
    switch (linux_clock_id) {
    case 0: /* CLOCK_REALTIME */
        host_clock = CLOCK_REALTIME;
        break;
    case 1: /* CLOCK_MONOTONIC */
    case 4: /* CLOCK_MONOTONIC_RAW: closest portable host clock below */
    case 7: /* CLOCK_BOOTTIME */
#ifdef CLOCK_MONOTONIC_RAW
        host_clock = (linux_clock_id == 4) ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
#else
        host_clock = CLOCK_MONOTONIC;
#endif
        break;
    default:
        dn_bionic_errno_value = EINVAL;
        return -1;
    }

    int rc = clock_gettime(host_clock, ts);
    if (rc != 0)
        dn_bionic_errno_value = errno;
    return rc;
}

static int dn_shim_usleep(unsigned int useconds)
{
    struct timespec request;
    request.tv_sec = (time_t)(useconds / 1000000u);
    request.tv_nsec = (long)(useconds % 1000000u) * 1000L;
    int rc = nanosleep(&request, NULL);
    if (rc != 0)
        dn_bionic_errno_value = errno;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Lookup table                                                        */
/* ------------------------------------------------------------------ */

struct dn_shim_entry {
    const char *name;
    void *address;
};

static const struct dn_shim_entry dn_shim_entries[] = {
    /* DN runtime self-test */
    { "dn_native_selftest_add", (void *)dn_native_selftest_add },
    { "dn_native_selftest_strlen", (void *)dn_native_selftest_strlen },

    /* Memory/string functions with identical fixed-argument ABI. */
    { "memcpy", (void *)memcpy },
    { "memmove", (void *)memmove },
    { "memset", (void *)memset },
    { "memcmp", (void *)memcmp },
    { "memchr", (void *)memchr },
    { "strlen", (void *)strlen },
    { "strcmp", (void *)strcmp },
    { "strncmp", (void *)strncmp },
    { "strcpy", (void *)strcpy },
    { "strncpy", (void *)strncpy },
    { "strcat", (void *)strcat },
    { "strchr", (void *)strchr },
    { "strrchr", (void *)strrchr },
    { "strstr", (void *)strstr },
    { "strdup", (void *)strdup },
    { "strndup", (void *)strndup },
    { "malloc", (void *)malloc },
    { "calloc", (void *)calloc },
    { "realloc", (void *)realloc },
    { "free", (void *)free },
    { "abort", (void *)abort },

    /* bionic/runtime glue implemented by DN (not forwarded blindly). */
    { "__errno", (void *)dn_shim_errno_location },
    { "__stack_chk_fail", (void *)dn_shim_stack_chk_fail },
    { "__stack_chk_guard", (void *)&dn_stack_chk_guard_storage },
    { "__cxa_atexit", (void *)dn_shim_cxa_atexit },
    { "__cxa_finalize", (void *)dn_shim_cxa_finalize },
    { "__cxa_guard_acquire", (void *)dn_shim_cxa_guard_acquire },
    { "__cxa_guard_release", (void *)dn_shim_cxa_guard_release },
    { "__cxa_guard_abort", (void *)dn_shim_cxa_guard_abort },
    { "__cxa_pure_virtual", (void *)dn_shim_cxa_pure_virtual },
    { "__cxa_deleted_virtual", (void *)dn_shim_cxa_pure_virtual },
    { "__register_atfork", (void *)dn_shim_register_atfork },
    { "__android_log_write", (void *)dn_shim_android_log_write },
    { "clock_gettime", (void *)dn_shim_clock_gettime },
    { "usleep", (void *)dn_shim_usleep },
    { "sleep", (void *)sleep },
    { "getpid", (void *)getpid },
    { "sched_yield", (void *)sched_yield },
    { "__cxa_thread_atexit_impl", (void *)dn_shim_cxa_thread_atexit_impl },
    { "__cxa_thread_atexit", (void *)dn_shim_cxa_thread_atexit_impl },
    { "__emutls_get_address", (void *)dn_shim_emutls_get_address },
    { "__assert2", (void *)dn_shim_assert2 },
    { "__assert_fail", (void *)dn_shim_assert_fail },
    { "strnlen", (void *)strnlen },
    { "strtok", (void *)strtok },
    { "strtok_r", (void *)strtok_r },
    { "strspn", (void *)strspn },
    { "strcspn", (void *)strcspn },
    { "strpbrk", (void *)strpbrk },
    { "tolower", (void *)tolower },
    { "toupper", (void *)toupper },
    { "isalpha", (void *)isalpha },
    { "isdigit", (void *)isdigit },
    { "isspace", (void *)isspace },
    { "isupper", (void *)isupper },
    { "islower", (void *)islower },
    { "isalnum", (void *)isalnum },
    { "ispunct", (void *)ispunct },
    { "isxdigit", (void *)isxdigit },
    { "atoi", (void *)atoi },
    { "atol", (void *)atol },
    { "atoll", (void *)atoll },
    { "atof", (void *)atof },
    { "strtol", (void *)strtol },
    { "strtoll", (void *)strtoll },
    { "strtoul", (void *)strtoul },
    { "strtoull", (void *)strtoull },
    { "strtof", (void *)strtof },
    { "strtod", (void *)strtod },
    { "strtold", (void *)strtold },
    { "abs", (void *)abs },
    { "labs", (void *)labs },
    { "llabs", (void *)llabs },
    { "qsort", (void *)qsort },
    { "bsearch", (void *)bsearch },
    { "rand", (void *)rand },
    { "srand", (void *)srand },
    { "random", (void *)random },
    { "srandom", (void *)srandom },
    { "sin", (void *)sin }, { "sinf", (void *)sinf },
    { "cos", (void *)cos }, { "cosf", (void *)cosf },
    { "tan", (void *)tan }, { "tanf", (void *)tanf },
    { "asin", (void *)asin }, { "asinf", (void *)asinf },
    { "acos", (void *)acos }, { "acosf", (void *)acosf },
    { "atan", (void *)atan }, { "atanf", (void *)atanf },
    { "atan2", (void *)atan2 }, { "atan2f", (void *)atan2f },
    { "sinh", (void *)sinh }, { "sinhf", (void *)sinhf },
    { "cosh", (void *)cosh }, { "coshf", (void *)coshf },
    { "tanh", (void *)tanh }, { "tanhf", (void *)tanhf },
    { "exp", (void *)exp }, { "expf", (void *)expf },
    { "log", (void *)log }, { "logf", (void *)logf },
    { "log10", (void *)log10 }, { "log10f", (void *)log10f },
    { "pow", (void *)pow }, { "powf", (void *)powf },
    { "sqrt", (void *)sqrt }, { "sqrtf", (void *)sqrtf },
    { "ceil", (void *)ceil }, { "ceilf", (void *)ceilf },
    { "floor", (void *)floor }, { "floorf", (void *)floorf },
    { "fabs", (void *)fabs }, { "fabsf", (void *)fabsf },
    { "fmod", (void *)fmod }, { "fmodf", (void *)fmodf },
    { "ldexp", (void *)ldexp }, { "ldexpf", (void *)ldexpf },
    { "frexp", (void *)frexp }, { "frexpf", (void *)frexpf },
    { "modf", (void *)modf }, { "modff", (void *)modff },
};

void *dn_native_shim_lookup(const char *name)
{
    if (!name)
        return NULL;
    void *bionic = dn_native_bionic_lookup(name);
    if (bionic)
        return bionic;
    void *android = dn_native_android_lookup(name);
    if (android)
        return android;
    const size_t count = sizeof(dn_shim_entries) / sizeof(dn_shim_entries[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(name, dn_shim_entries[i].name) == 0)
            return dn_shim_entries[i].address;
    }
    if (name[0] == 'v' && name[1] == 'k')
        return dn_vulkan_lookup_symbol(name);
    return NULL;
}

int dn_native_shim_provides(const char *name)
{
    return dn_native_shim_lookup(name) != NULL;
}
