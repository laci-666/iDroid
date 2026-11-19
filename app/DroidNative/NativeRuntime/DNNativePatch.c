#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "DNNativePatch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* Linux generic errno values used at the translated ABI boundary. */
enum {
    DNLE_PERM = 1, DNLE_NOENT = 2, DNLE_INTR = 4, DNLE_IO = 5,
    DNLE_BADF = 9, DNLE_AGAIN = 11, DNLE_NOMEM = 12, DNLE_ACCES = 13,
    DNLE_FAULT = 14, DNLE_EXIST = 17, DNLE_NOTDIR = 20, DNLE_ISDIR = 21,
    DNLE_INVAL = 22, DNLE_MFILE = 24, DNLE_NOSPC = 28, DNLE_PIPE = 32,
    DNLE_RANGE = 34, DNLE_NAMETOOLONG = 36, DNLE_NOSYS = 38,
    DNLE_NOTEMPTY = 39, DNLE_LOOP = 40, DNLE_NOTSUP = 95,
    DNLE_TIMEDOUT = 110
};

static int dn_patch_fail(char *error, size_t error_len, const char *message)
{
    if (error && error_len)
        snprintf(error, error_len, "%s", message ? message : "native patch runtime failure");
    return -1;
}

int dn_native_linux_errno_from_host(int host_errno)
{
#if defined(__APPLE__)
    switch (host_errno) {
    case EPERM: return 1;
    case ENOENT: return 2;
    case ESRCH: return 3;
    case EINTR: return 4;
    case EIO: return 5;
    case ENXIO: return 6;
    case E2BIG: return 7;
    case ENOEXEC: return 8;
    case EBADF: return 9;
    case ECHILD: return 10;
    case EAGAIN: return 11;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EFAULT: return 14;
    case EBUSY: return 16;
    case EEXIST: return 17;
    case EXDEV: return 18;
    case ENODEV: return 19;
    case ENOTDIR: return 20;
    case EISDIR: return 21;
    case EINVAL: return 22;
    case ENFILE: return 23;
    case EMFILE: return 24;
    case ENOTTY: return 25;
    case ETXTBSY: return 26;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case ESPIPE: return 29;
    case EROFS: return 30;
    case EMLINK: return 31;
    case EPIPE: return 32;
    case EDOM: return 33;
    case ERANGE: return 34;
    case EDEADLK: return 35;
    case ENAMETOOLONG: return 36;
    case ENOLCK: return 37;
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case ELOOP: return 40;
    case ENOMSG: return 42;
    case EIDRM: return 43;
    case EPROTO: return 71;
    case EOVERFLOW: return 75;
    case ETIMEDOUT: return DNLE_TIMEDOUT;
    default: return host_errno > 0 ? DNLE_IO : DNLE_IO;
    }
#else
    return host_errno;
#endif
}

static long dn_sys_result(long result)
{
    return result < 0 ? -(long)dn_native_linux_errno_from_host(errno) : result;
}

/* ------------------------------------------------------------------ */
/* Bionic-shaped thread pointer + ELF TLS                              */
/* ------------------------------------------------------------------ */

#define DN_TP_PREFIX_BYTES 128u
#define DN_TP_SLOT_BYTES 128u
#define DN_TLS_FIRST_OFFSET 256u
#define DN_TP_TLS_CAPACITY (1024u * 1024u)
#define DN_TP_MAGIC ((uintptr_t)0x444e54504c533031ULL) /* DNTPLS01 */

struct dn_tls_descriptor_arg {
    dn_native_tls_module_t *module;
    uint64_t offset;
    struct dn_tls_descriptor_arg *next;
};

struct dn_native_tls_module {
    uint64_t id;
    uint64_t offset;
    const uint8_t *template_image;
    size_t file_size;
    size_t memory_size;
    struct dn_tls_descriptor_arg *args;
    struct dn_native_tls_module *next;
};

static pthread_once_t dn_patch_once = PTHREAD_ONCE_INIT;
static pthread_key_t dn_tp_key;
static int dn_patch_init_error;
static uintptr_t dn_stack_guard;
static pthread_mutex_t dn_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
static dn_native_tls_module_t *dn_tls_modules;
static uint64_t dn_tls_next_id = 1;
static uint64_t dn_tls_next_offset = DN_TLS_FIRST_OFFSET;
static uint64_t dn_tls_generation = 0;

static uintptr_t dn_align_up_uintptr(uintptr_t value, uintptr_t alignment)
{
    if (alignment <= 1)
        return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

static void dn_tp_free(void *tp)
{
    if (tp)
        free((uint8_t *)tp - DN_TP_PREFIX_BYTES);
}

#if defined(__APPLE__) && defined(__aarch64__)
static uintptr_t dn_darwin_tsd_base(void)
{
    uintptr_t value = 0;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value & ~(uintptr_t)7;
}
#endif

static void dn_patch_once_init(void)
{
    uint64_t random_value = 0;
#if defined(__APPLE__)
    arc4random_buf(&random_value, sizeof(random_value));
#else
    {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t got = read(fd, &random_value, sizeof(random_value));
            (void)got;
            close(fd);
        }
    }
#endif
    if (!random_value)
        random_value = (uint64_t)(uintptr_t)&dn_stack_guard ^ (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ULL;
    dn_stack_guard = (uintptr_t)(random_value & ~(uint64_t)0xff);

    if (pthread_key_create(&dn_tp_key, dn_tp_free) != 0) {
        dn_patch_init_error = 1;
        return;
    }

#if defined(__APPLE__) && defined(__aarch64__)
    static int probe;
    pthread_setspecific(dn_tp_key, &probe);
    void **tsd = (void **)(void *)dn_darwin_tsd_base();
    void *direct = tsd ? tsd[(size_t)dn_tp_key] : NULL;
    pthread_setspecific(dn_tp_key, NULL);
    if (direct != &probe)
        dn_patch_init_error = 2;
#endif
}

int dn_native_patch_init(char *error, size_t error_len)
{
    pthread_once(&dn_patch_once, dn_patch_once_init);
    if (dn_patch_init_error == 1)
        return dn_patch_fail(error, error_len, "pthread_key_create failed for the native thread pointer");
    if (dn_patch_init_error == 2)
        return dn_patch_fail(error, error_len, "Darwin TSD layout does not match the patch-island fast path");
    return 0;
}

uintptr_t dn_native_patch_tp_key_offset(void)
{
    if (dn_native_patch_init(NULL, 0) != 0)
        return 0;
    return (uintptr_t)dn_tp_key * sizeof(void *);
}

static void dn_tp_set_slots(void *tp)
{
    void **slots = (void **)tp;
    uint64_t *prefix_words = (uint64_t *)((uint8_t *)tp - DN_TP_PREFIX_BYTES);
    slots[0] = tp;                                  /* TLS_SLOT_SELF */
    slots[1] = (void *)(uintptr_t)pthread_self();   /* TLS_SLOT_THREAD_ID */
    slots[2] = &prefix_words[2];                    /* per-thread errno storage */
    slots[5] = (void *)dn_stack_guard;              /* TLS_SLOT_STACK_GUARD, tp+0x28 */
    slots[7] = tp;                                  /* ART thread-self placeholder */
    slots[8] = tp;                                  /* DTV placeholder */
    slots[9] = tp;                                  /* bionic TLS placeholder */
}

static void dn_tp_initialize_tls_modules(void *tp)
{
    uint64_t *header = (uint64_t *)tp;
    uint64_t done_generation = header[-2];

    pthread_mutex_lock(&dn_tls_mutex);
    if (done_generation < dn_tls_generation) {
        for (dn_native_tls_module_t *m = dn_tls_modules; m; m = m->next) {
            if (m->id <= done_generation)
                continue;
            uint8_t *dst = (uint8_t *)tp + (size_t)m->offset;
            if (m->memory_size)
                memset(dst, 0, m->memory_size);
            if (m->file_size && m->template_image)
                memcpy(dst, m->template_image, m->file_size);
        }
        header[-2] = dn_tls_generation;
    }
    pthread_mutex_unlock(&dn_tls_mutex);
}

void *dn_native_thread_pointer(void)
{
    if (dn_native_patch_init(NULL, 0) != 0)
        return NULL;
    void *tp = pthread_getspecific(dn_tp_key);
    if (!tp) {
        size_t total = DN_TP_PREFIX_BYTES + DN_TP_TLS_CAPACITY;
        uint8_t *allocation = (uint8_t *)calloc(1, total);
        if (!allocation)
            return NULL;
        tp = allocation + DN_TP_PREFIX_BYTES;
        ((uint64_t *)tp)[-1] = DN_TP_MAGIC;
        ((uint64_t *)tp)[-2] = 0;
        if (pthread_setspecific(dn_tp_key, tp) != 0) {
            free(allocation);
            return NULL;
        }
    }
    if (((uint64_t *)tp)[-1] != DN_TP_MAGIC)
        return NULL;
    dn_tp_set_slots(tp);
    dn_tp_initialize_tls_modules(tp);
    return tp;
}

void *dn_native_tp_allocate_slow(void)
{
    void *tp = dn_native_thread_pointer();
    if (!tp)
        abort();
    return tp;
}

dn_native_tls_module_t *dn_native_tls_register(const void *template_image,
                                               size_t file_size,
                                               size_t memory_size,
                                               size_t alignment)
{
    if (file_size > memory_size)
        return NULL;
    if (alignment == 0)
        alignment = 8;
    if ((alignment & (alignment - 1)) != 0 || alignment > 4096)
        return NULL;

    dn_native_tls_module_t *module = (dn_native_tls_module_t *)calloc(1, sizeof(*module));
    if (!module)
        return NULL;
    module->template_image = (const uint8_t *)template_image;
    module->file_size = file_size;
    module->memory_size = memory_size;

    pthread_mutex_lock(&dn_tls_mutex);
    uint64_t offset = dn_align_up_uintptr((uintptr_t)dn_tls_next_offset, (uintptr_t)alignment);
    uint64_t end = offset + (uint64_t)memory_size;
    if (end < offset || end > DN_TP_TLS_CAPACITY) {
        pthread_mutex_unlock(&dn_tls_mutex);
        free(module);
        return NULL;
    }
    module->id = dn_tls_next_id++;
    module->offset = offset;
    dn_tls_next_offset = end;
    dn_tls_generation = module->id;
    module->next = dn_tls_modules;
    dn_tls_modules = module;
    pthread_mutex_unlock(&dn_tls_mutex);
    return module;
}

void dn_native_tls_unregister(dn_native_tls_module_t *module)
{
    if (!module)
        return;
    pthread_mutex_lock(&dn_tls_mutex);
    dn_native_tls_module_t **cursor = &dn_tls_modules;
    while (*cursor) {
        if (*cursor == module) {
            *cursor = module->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&dn_tls_mutex);

    struct dn_tls_descriptor_arg *arg = module->args;
    while (arg) {
        struct dn_tls_descriptor_arg *next = arg->next;
        free(arg);
        arg = next;
    }
    free(module);
}

uint64_t dn_native_tls_module_id(const dn_native_tls_module_t *module)
{
    return module ? module->id : 0;
}

uint64_t dn_native_tls_module_offset(const dn_native_tls_module_t *module)
{
    return module ? module->offset : 0;
}

void *dn_native_tls_descriptor_arg(dn_native_tls_module_t *module,
                                   uint64_t offset_within_module)
{
    if (!module)
        return NULL;
    struct dn_tls_descriptor_arg *arg =
        (struct dn_tls_descriptor_arg *)calloc(1, sizeof(*arg));
    if (!arg)
        return NULL;
    arg->module = module;
    arg->offset = offset_within_module;
    pthread_mutex_lock(&dn_tls_mutex);
    arg->next = module->args;
    module->args = arg;
    pthread_mutex_unlock(&dn_tls_mutex);
    return arg;
}

static dn_native_tls_module_t *dn_tls_module_by_id(uint64_t id)
{
    dn_native_tls_module_t *found = NULL;
    pthread_mutex_lock(&dn_tls_mutex);
    for (dn_native_tls_module_t *m = dn_tls_modules; m; m = m->next) {
        if (m->id == id) {
            found = m;
            break;
        }
    }
    pthread_mutex_unlock(&dn_tls_mutex);
    return found;
}

void *dn_native_tls_address(dn_native_tls_module_t *module,
                            uint64_t offset_within_module)
{
    if (!module)
        return NULL;
    void *tp = dn_native_thread_pointer();
    if (!tp)
        return NULL;
    return (uint8_t *)tp + (size_t)module->offset + (size_t)offset_within_module;
}

intptr_t dn_native_tlsdesc_resolve(const uintptr_t *descriptor)
{
    if (!descriptor)
        return 0;
    const struct dn_tls_descriptor_arg *arg =
        (const struct dn_tls_descriptor_arg *)(void *)descriptor[1];
    if (!arg || !arg->module)
        return 0;
    void *tp = dn_native_thread_pointer();
    void *address = dn_native_tls_address(arg->module, arg->offset);
    if (!tp || !address)
        return 0;
    return (intptr_t)((uint8_t *)address - (uint8_t *)tp);
}

void *dn_native_tls_get_addr(void *tls_index)
{
    if (!tls_index)
        return NULL;
    const uint64_t *pair = (const uint64_t *)tls_index;
    dn_native_tls_module_t *module = dn_tls_module_by_id(pair[0]);
    return module ? dn_native_tls_address(module, pair[1]) : NULL;
}

/* ------------------------------------------------------------------ */
/* Android virtual filesystem                                          */
/* ------------------------------------------------------------------ */

#define DN_PATH_CAP 4096

static pthread_mutex_t dn_files_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *dn_files_root;
static char dn_virtual_cwd[DN_PATH_CAP] = "/";

static bool dn_has_prefix_boundary(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static int dn_mkdir_all(const char *path)
{
    if (!path || !*path)
        return -1;
    char copy[DN_PATH_CAP];
    if (strlen(path) >= sizeof(copy))
        return -1;
    strcpy(copy, path);
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int dn_ensure_parent_dirs(const char *path)
{
    if (!path)
        return -1;
    char copy[DN_PATH_CAP];
    if (strlen(path) >= sizeof(copy))
        return -1;
    strcpy(copy, path);
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy)
        return 0;
    *slash = '\0';
    return dn_mkdir_all(copy);
}

static bool dn_copy_files_root(char *out, size_t out_len)
{
    pthread_mutex_lock(&dn_files_mutex);
    const char *root = dn_files_root ? dn_files_root : "/tmp/DroidNativeNative";
    bool ok = strlen(root) < out_len;
    if (ok)
        strcpy(out, root);
    pthread_mutex_unlock(&dn_files_mutex);
    return ok;
}

void dn_native_set_files_root(const char *path)
{
    if (!path || !*path)
        return;
    char *copy = strdup(path);
    if (!copy)
        return;
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';

    pthread_mutex_lock(&dn_files_mutex);
    free(dn_files_root);
    dn_files_root = copy;
    pthread_mutex_unlock(&dn_files_mutex);

    static const char *const subdirs[] = {
        "", "/files", "/cache", "/internal", "/external", "/obb",
        "/sdcard", "/assets", "/tmp", "/vfs"
    };
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
        char full[DN_PATH_CAP];
        if (snprintf(full, sizeof(full), "%s%s", copy, subdirs[i]) < (int)sizeof(full))
            (void)dn_mkdir_all(full);
    }
}

const char *dn_native_files_root(void)
{
    static _Thread_local char root[DN_PATH_CAP];
    if (!dn_copy_files_root(root, sizeof(root)))
        return "/tmp/DroidNativeNative";
    return root;
}

struct dn_slice {
    const char *ptr;
    size_t len;
};

static int dn_append_normalized_suffix(char *out, size_t out_len,
                                       size_t *used_io, const char *suffix)
{
    struct dn_slice components[128];
    size_t count = 0;
    const char *p = suffix ? suffix : "";
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (count)
                count--;
            continue;
        }
        if (count >= sizeof(components) / sizeof(components[0]))
            return -DNLE_NAMETOOLONG;
        components[count].ptr = start;
        components[count].len = len;
        count++;
    }

    size_t used = *used_io;
    for (size_t i = 0; i < count; ++i) {
        if (used + 1 + components[i].len >= out_len)
            return -DNLE_NAMETOOLONG;
        out[used++] = '/';
        memcpy(out + used, components[i].ptr, components[i].len);
        used += components[i].len;
    }
    out[used] = '\0';
    *used_io = used;
    return 0;
}

int dn_native_translate_path(const char *guest_path, char *out, size_t out_len)
{
    if (!guest_path)
        return -DNLE_FAULT;
    if (!out || out_len == 0)
        return -DNLE_FAULT;

    static const char *const host_devices[] = {
        "/dev/null", "/dev/zero", "/dev/full", "/dev/urandom", "/dev/random"
    };
    for (size_t i = 0; i < sizeof(host_devices) / sizeof(host_devices[0]); ++i) {
        if (strcmp(guest_path, host_devices[i]) == 0) {
            if (strlen(guest_path) >= out_len)
                return -DNLE_NAMETOOLONG;
            strcpy(out, guest_path);
            return 0;
        }
    }

    char root[DN_PATH_CAP];
    if (!dn_copy_files_root(root, sizeof(root)))
        return -DNLE_NAMETOOLONG;

    const char *subdir = "/vfs";
    const char *suffix = guest_path;
    if (guest_path[0] != '/') {
        subdir = "/files";
        suffix = guest_path;
    } else if (dn_has_prefix_boundary(guest_path, "/data/data") ||
               dn_has_prefix_boundary(guest_path, "/data/user/0")) {
        const char *prefix = dn_has_prefix_boundary(guest_path, "/data/data")
                                 ? "/data/data" : "/data/user/0";
        const char *pkg = guest_path + strlen(prefix);
        const char *rest = strchr(pkg + 1, '/');
        subdir = "/internal";
        suffix = rest ? rest + 1 : "";
    } else if (dn_has_prefix_boundary(guest_path, "/sdcard")) {
        subdir = "/sdcard";
        suffix = guest_path + strlen("/sdcard");
    } else if (dn_has_prefix_boundary(guest_path, "/storage/emulated/0")) {
        subdir = "/sdcard";
        suffix = guest_path + strlen("/storage/emulated/0");
    } else if (dn_has_prefix_boundary(guest_path, "/storage/self/primary")) {
        subdir = "/sdcard";
        suffix = guest_path + strlen("/storage/self/primary");
    } else if (dn_has_prefix_boundary(guest_path, "/data/local/tmp")) {
        subdir = "/tmp";
        suffix = guest_path + strlen("/data/local/tmp");
    } else if (dn_has_prefix_boundary(guest_path, "/data/local")) {
        subdir = "/tmp";
        suffix = guest_path + strlen("/data/local");
    } else if (dn_has_prefix_boundary(guest_path, "/android_asset")) {
        subdir = "/assets";
        suffix = guest_path + strlen("/android_asset");
    } else if (strncmp(guest_path, "/assets/", 9) == 0) {
        subdir = "/assets";
        suffix = guest_path + strlen("/assets/");
    } else if (dn_has_prefix_boundary(guest_path, "/data")) {
        subdir = "/internal";
        suffix = guest_path + strlen("/data");
    }

    int written = snprintf(out, out_len, "%s%s", root, subdir);
    if (written < 0 || (size_t)written >= out_len)
        return -DNLE_NAMETOOLONG;
    size_t used = (size_t)written;
    return dn_append_normalized_suffix(out, out_len, &used, suffix);
}

static int dn_fd_host_path(int fd, char *out, size_t out_len)
{
#if defined(__APPLE__)
    if (out_len > PATH_MAX)
        out_len = PATH_MAX;
    if (fcntl(fd, F_GETPATH, out) != 0)
        return -dn_native_linux_errno_from_host(errno);
    out[out_len - 1] = '\0';
    return 0;
#else
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    ssize_t got = readlink(proc, out, out_len - 1);
    if (got < 0)
        return -dn_native_linux_errno_from_host(errno);
    out[got] = '\0';
    return 0;
#endif
}

int dn_native_translate_at_path(int dirfd, const char *guest_path,
                                char *out, size_t out_len)
{
    if (!guest_path)
        return -DNLE_FAULT;
    if (guest_path[0] == '/')
        return dn_native_translate_path(guest_path, out, out_len);
    if (dirfd == -100) { /* AT_FDCWD */
        char joined[DN_PATH_CAP];
        pthread_mutex_lock(&dn_files_mutex);
        int written = snprintf(joined, sizeof(joined), "%s/%s", dn_virtual_cwd, guest_path);
        pthread_mutex_unlock(&dn_files_mutex);
        if (written < 0 || (size_t)written >= sizeof(joined))
            return -DNLE_NAMETOOLONG;
        return dn_native_translate_path(joined, out, out_len);
    }
    if (dirfd < 0)
        return -DNLE_BADF;
    int rc = dn_fd_host_path(dirfd, out, out_len);
    if (rc != 0)
        return rc;
    size_t used = strlen(out);
    return dn_append_normalized_suffix(out, out_len, &used, guest_path);
}

static int dn_host_open_flags(int linux_flags)
{
    int flags = linux_flags & 3;
    if (linux_flags & 00000100) flags |= O_CREAT;
    if (linux_flags & 00000200) flags |= O_EXCL;
    if (linux_flags & 00000400) flags |= O_NOCTTY;
    if (linux_flags & 00001000) flags |= O_TRUNC;
    if (linux_flags & 00002000) flags |= O_APPEND;
    if (linux_flags & 00004000) flags |= O_NONBLOCK;
#ifdef O_DSYNC
    if (linux_flags & 00010000) flags |= O_DSYNC;
#endif
    if (linux_flags & 00200000) flags |= O_DIRECTORY;
    if (linux_flags & 00400000) flags |= O_NOFOLLOW;
    if (linux_flags & 02000000) flags |= O_CLOEXEC;
    if (linux_flags & 04000000) flags |= O_SYNC;
    return flags;
}

static int dn_open_host_path(const char *host_path, int linux_flags, mode_t mode)
{
    int flags = dn_host_open_flags(linux_flags);
    if ((linux_flags & 010000000) /* O_PATH */ ||
        (linux_flags & 020200000) == 020200000 /* __O_TMPFILE */)
        return -DNLE_INVAL;
    if ((flags & O_CREAT) && dn_ensure_parent_dirs(host_path) != 0 && errno != EEXIST) {
        /* Continue: open() below will produce the precise error. */
    }
    int fd = (flags & O_CREAT) ? open(host_path, flags, mode) : open(host_path, flags);
    return fd < 0 ? -dn_native_linux_errno_from_host(errno) : fd;
}

static int dn_open_special_proc(const char *guest_path, int linux_flags, mode_t mode, int *handled)
{
    *handled = 0;
    if (!guest_path || guest_path[0] != '/')
        return 0;
    if (strcmp(guest_path, "/proc/self/exe") == 0 ||
        strcmp(guest_path, "/proc/thread-self/exe") == 0) {
        *handled = 1;
#if defined(__APPLE__)
        char exe[PATH_MAX];
        uint32_t size = sizeof(exe);
        if (_NSGetExecutablePath(exe, &size) != 0)
            return -DNLE_NAMETOOLONG;
        return dn_open_host_path(exe, linux_flags, mode);
#else
        return dn_open_host_path("/proc/self/exe", linux_flags, mode);
#endif
    }
    const char *fd_prefix = "/proc/self/fd/";
    if (strncmp(guest_path, fd_prefix, strlen(fd_prefix)) == 0) {
        *handled = 1;
        char *end = NULL;
        long number = strtol(guest_path + strlen(fd_prefix), &end, 10);
        if (!end || *end || number < 0 || number > INT_MAX)
            return -DNLE_NOENT;
        int dupfd = dup((int)number);
        return dupfd < 0 ? -dn_native_linux_errno_from_host(errno) : dupfd;
    }
    if (strcmp(guest_path, "/proc/self/cmdline") == 0 ||
        strcmp(guest_path, "/proc/self/status") == 0 ||
        strcmp(guest_path, "/proc/self/stat") == 0) {
        *handled = 1;
        return dn_open_host_path("/dev/null", linux_flags & 3, mode);
    }
    return 0;
}

int dn_native_open_translated(const char *guest_path, int linux_flags, mode_t mode)
{
    int handled = 0;
    int special = dn_open_special_proc(guest_path, linux_flags, mode, &handled);
    if (handled)
        return special;
    char host_path[DN_PATH_CAP];
    int rc = dn_native_translate_path(guest_path, host_path, sizeof(host_path));
    if (rc != 0)
        return rc;
    return dn_open_host_path(host_path, linux_flags, mode);
}

int dn_native_openat_translated(int dirfd, const char *guest_path,
                                int linux_flags, mode_t mode)
{
    if (guest_path && guest_path[0] == '/')
        return dn_native_open_translated(guest_path, linux_flags, mode);
    char host_path[DN_PATH_CAP];
    int rc = dn_native_translate_at_path(dirfd, guest_path, host_path, sizeof(host_path));
    if (rc != 0)
        return rc;
    return dn_open_host_path(host_path, linux_flags, mode);
}

/* ------------------------------------------------------------------ */
/* Linux syscall translation for patched svc #0                        */
/* ------------------------------------------------------------------ */

#define DN_PTR(v) ((void *)(uintptr_t)(v))
#define DN_CPTR(v) ((const void *)(uintptr_t)(v))

static clockid_t dn_host_clock(int linux_clock, int *ok)
{
    *ok = 1;
    switch (linux_clock) {
    case 0: return CLOCK_REALTIME;
    case 1: return CLOCK_MONOTONIC;
#ifdef CLOCK_MONOTONIC_RAW
    case 4: return CLOCK_MONOTONIC_RAW;
#endif
    case 6:
    case 7: return CLOCK_MONOTONIC;
#ifdef CLOCK_PROCESS_CPUTIME_ID
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
    case 3: return CLOCK_THREAD_CPUTIME_ID;
#endif
    default: *ok = 0; return CLOCK_REALTIME;
    }
}

static void dn_fill_linux_stat(void *out, const struct stat *st)
{
    uint8_t *p = (uint8_t *)out;
    memset(p, 0, 128);
    memcpy(p + 0, &st->st_dev, 8);
    uint64_t ino = (uint64_t)st->st_ino;
    memcpy(p + 8, &ino, 8);
    uint32_t mode = (uint32_t)st->st_mode;
    uint32_t nlink = (uint32_t)st->st_nlink;
    uint32_t uid = (uint32_t)st->st_uid;
    uint32_t gid = (uint32_t)st->st_gid;
    memcpy(p + 16, &mode, 4);
    memcpy(p + 20, &nlink, 4);
    memcpy(p + 24, &uid, 4);
    memcpy(p + 28, &gid, 4);
    memcpy(p + 32, &st->st_rdev, 8);
    int64_t size = (int64_t)st->st_size;
    int32_t blksize = (int32_t)st->st_blksize;
    int64_t blocks = (int64_t)st->st_blocks;
    memcpy(p + 48, &size, 8);
    memcpy(p + 56, &blksize, 4);
    memcpy(p + 64, &blocks, 8);
    int64_t atime = (int64_t)st->st_atime;
    int64_t mtime = (int64_t)st->st_mtime;
    int64_t ctime = (int64_t)st->st_ctime;
#if defined(__APPLE__)
    int64_t atime_ns = (int64_t)st->st_atimespec.tv_nsec;
    int64_t mtime_ns = (int64_t)st->st_mtimespec.tv_nsec;
    int64_t ctime_ns = (int64_t)st->st_ctimespec.tv_nsec;
#else
    int64_t atime_ns = (int64_t)st->st_atim.tv_nsec;
    int64_t mtime_ns = (int64_t)st->st_mtim.tv_nsec;
    int64_t ctime_ns = (int64_t)st->st_ctim.tv_nsec;
#endif
    memcpy(p + 72, &atime, 8);
    memcpy(p + 80, &atime_ns, 8);
    memcpy(p + 88, &mtime, 8);
    memcpy(p + 96, &mtime_ns, 8);
    memcpy(p + 104, &ctime, 8);
    memcpy(p + 112, &ctime_ns, 8);
}

struct dn_dir_cache_entry {
    int fd;
    DIR *dir;
};

static struct dn_dir_cache_entry dn_dir_cache[32];
static pthread_mutex_t dn_dir_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static DIR *dn_dir_for_fd(int fd)
{
    pthread_mutex_lock(&dn_dir_cache_mutex);
    for (size_t i = 0; i < sizeof(dn_dir_cache) / sizeof(dn_dir_cache[0]); ++i) {
        if (dn_dir_cache[i].dir && dn_dir_cache[i].fd == fd) {
            DIR *dir = dn_dir_cache[i].dir;
            pthread_mutex_unlock(&dn_dir_cache_mutex);
            return dir;
        }
    }
    int dupfd = dup(fd);
    DIR *dir = dupfd >= 0 ? fdopendir(dupfd) : NULL;
    if (!dir) {
        if (dupfd >= 0)
            close(dupfd);
        pthread_mutex_unlock(&dn_dir_cache_mutex);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(dn_dir_cache) / sizeof(dn_dir_cache[0]); ++i) {
        if (!dn_dir_cache[i].dir) {
            dn_dir_cache[i].fd = fd;
            dn_dir_cache[i].dir = dir;
            break;
        }
    }
    pthread_mutex_unlock(&dn_dir_cache_mutex);
    return dir;
}

static void dn_dir_forget_fd(int fd)
{
    pthread_mutex_lock(&dn_dir_cache_mutex);
    for (size_t i = 0; i < sizeof(dn_dir_cache) / sizeof(dn_dir_cache[0]); ++i) {
        if (dn_dir_cache[i].dir && dn_dir_cache[i].fd == fd) {
            closedir(dn_dir_cache[i].dir);
            dn_dir_cache[i].dir = NULL;
            dn_dir_cache[i].fd = -1;
        }
    }
    pthread_mutex_unlock(&dn_dir_cache_mutex);
}

static long dn_sys_getdents64(int fd, void *buffer, size_t capacity)
{
    DIR *dir = dn_dir_for_fd(fd);
    if (!dir)
        return -dn_native_linux_errno_from_host(errno ? errno : EBADF);
    size_t used = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (used == 0 && errno)
                return -dn_native_linux_errno_from_host(errno);
            break;
        }
        size_t name_len = strlen(entry->d_name);
        size_t record_len = (19 + name_len + 1 + 7) & ~(size_t)7;
        if (record_len > capacity - used) {
            if (used == 0)
                return -DNLE_INVAL;
            /* The entry was consumed; preserve the bytes already made.  A
             * following call continues after it, just as a too-small host
             * buffer is expected to be retried with a larger allocation by
             * well-behaved runtime code. */
            break;
        }
        uint8_t *record = (uint8_t *)buffer + used;
        memset(record, 0, record_len);
        uint64_t ino = (uint64_t)entry->d_ino;
        int64_t off = (int64_t)(used + record_len);
        uint16_t reclen = (uint16_t)record_len;
        memcpy(record + 0, &ino, 8);
        memcpy(record + 8, &off, 8);
        memcpy(record + 16, &reclen, 2);
#ifdef DT_UNKNOWN
        record[18] = entry->d_type;
#endif
        memcpy(record + 19, entry->d_name, name_len + 1);
        used += record_len;
    }
    return (long)used;
}

static pthread_mutex_t dn_futex_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dn_futex_cond = PTHREAD_COND_INITIALIZER;

static long dn_sys_futex(uint32_t *uaddr, int op, uint32_t value,
                         const struct timespec *timeout,
                         uint32_t *uaddr2, uint32_t value3)
{
    (void)uaddr2;
    (void)value3;
    int cmd = op & 127;
    if (!uaddr)
        return -DNLE_FAULT;
    if (cmd == 1 || cmd == 10) { /* WAKE / WAKE_BITSET */
        pthread_mutex_lock(&dn_futex_mutex);
        pthread_cond_broadcast(&dn_futex_cond);
        pthread_mutex_unlock(&dn_futex_mutex);
        return value > 0x7fffffffu ? 0 : (long)value;
    }
    if (cmd != 0 && cmd != 9) /* WAIT / WAIT_BITSET */
        return -DNLE_NOTSUP;

    pthread_mutex_lock(&dn_futex_mutex);
    uint32_t current;
    __atomic_load(uaddr, &current, __ATOMIC_SEQ_CST);
    if (current != value) {
        pthread_mutex_unlock(&dn_futex_mutex);
        return -DNLE_AGAIN;
    }
    int rc;
    if (!timeout) {
        rc = pthread_cond_wait(&dn_futex_cond, &dn_futex_mutex);
    } else if (cmd == 9) {
        rc = pthread_cond_timedwait(&dn_futex_cond, &dn_futex_mutex, timeout);
    } else {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        now.tv_sec += timeout->tv_sec;
        now.tv_nsec += timeout->tv_nsec;
        if (now.tv_nsec >= 1000000000L) {
            now.tv_sec++;
            now.tv_nsec -= 1000000000L;
        }
        rc = pthread_cond_timedwait(&dn_futex_cond, &dn_futex_mutex, &now);
    }
    pthread_mutex_unlock(&dn_futex_mutex);
    if (rc == ETIMEDOUT)
        return -DNLE_TIMEDOUT;
    return rc == 0 ? 0 : -(long)dn_native_linux_errno_from_host(rc);
}

static long dn_get_tid_value(void)
{
#if defined(__APPLE__)
    uint64_t tid = 0;
    (void)pthread_threadid_np(NULL, &tid);
    return tid ? (long)tid : (long)getpid();
#else
    return (long)pthread_self();
#endif
}

static int dn_host_signal(int linux_signal)
{
    switch (linux_signal) {
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

static long dn_sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
    return dn_native_openat_translated(dirfd, path, flags, mode);
}

static long dn_sys_fcntl(int fd, int cmd, long arg)
{
    int host_cmd;
    switch (cmd) {
    case 0: host_cmd = F_DUPFD; break;
    case 1: host_cmd = F_GETFD; break;
    case 2: host_cmd = F_SETFD; break;
    case 3: host_cmd = F_GETFL; break;
    case 4: {
        int host_flags = (int)arg & O_ACCMODE;
        if (arg & 00002000) host_flags |= O_APPEND;
        if (arg & 00004000) host_flags |= O_NONBLOCK;
#ifdef O_DSYNC
        if (arg & 00010000) host_flags |= O_DSYNC;
#endif
        return dn_sys_result(fcntl(fd, F_SETFL, host_flags));
    }
#ifdef F_DUPFD_CLOEXEC
    case 1030: host_cmd = F_DUPFD_CLOEXEC; break;
#endif
    default: return -DNLE_INVAL;
    }
    long result = dn_sys_result(fcntl(fd, host_cmd, arg));
    if (result >= 0 && cmd == 3) {
        long linux_flags = result & 3;
        if (result & O_APPEND) linux_flags |= 00002000;
        if (result & O_NONBLOCK) linux_flags |= 00004000;
#ifdef O_DSYNC
        if (result & O_DSYNC) linux_flags |= 00010000;
#endif
        return linux_flags;
    }
    return result;
}

static long dn_sys_ioctl(int fd, unsigned long request, void *arg)
{
    unsigned long host_request;
    switch (request) {
    case 0x541b: host_request = FIONREAD; break;
    case 0x5421: host_request = FIONBIO; break;
#ifdef TIOCGWINSZ
    case 0x5413: host_request = TIOCGWINSZ; break;
#endif
    case 0x5401: /* TCGETS: termios layouts differ; no guest tty exists. */
        return -25; /* ENOTTY */
    default:
        return -25;
    }
    return dn_sys_result(ioctl(fd, host_request, arg));
}

static int dn_set_virtual_cwd(const char *guest_path)
{
    char absolute[DN_PATH_CAP];
    if (!guest_path)
        return -DNLE_FAULT;
    if (guest_path[0] == '/') {
        if (strlen(guest_path) >= sizeof(absolute))
            return -DNLE_NAMETOOLONG;
        strcpy(absolute, guest_path);
    } else {
        pthread_mutex_lock(&dn_files_mutex);
        int written = snprintf(absolute, sizeof(absolute), "%s/%s", dn_virtual_cwd, guest_path);
        pthread_mutex_unlock(&dn_files_mutex);
        if (written < 0 || (size_t)written >= sizeof(absolute))
            return -DNLE_NAMETOOLONG;
    }
    char translated[DN_PATH_CAP];
    int rc = dn_native_translate_path(absolute, translated, sizeof(translated));
    if (rc != 0)
        return rc;
    if (chdir(translated) != 0)
        return -dn_native_linux_errno_from_host(errno);

    /* Store a normalized guest CWD by using the translated path's
     * existence only; lexical dot components are resolved on later
     * AT_FDCWD joins by dn_native_translate_path(). */
    pthread_mutex_lock(&dn_files_mutex);
    strncpy(dn_virtual_cwd, absolute, sizeof(dn_virtual_cwd) - 1);
    dn_virtual_cwd[sizeof(dn_virtual_cwd) - 1] = '\0';
    pthread_mutex_unlock(&dn_files_mutex);
    return 0;
}

long dn_native_syscall_dispatch(long a0, long a1, long a2, long a3,
                                long a4, long a5, long nr)
{
    switch (nr) {
    case 17: { /* getcwd */
        char *buffer = (char *)DN_PTR(a0);
        size_t size = (size_t)a1;
        if (!buffer)
            return -DNLE_FAULT;
        pthread_mutex_lock(&dn_files_mutex);
        size_t len = strlen(dn_virtual_cwd) + 1;
        if (len > size) {
            pthread_mutex_unlock(&dn_files_mutex);
            return -DNLE_RANGE;
        }
        memcpy(buffer, dn_virtual_cwd, len);
        pthread_mutex_unlock(&dn_files_mutex);
        return (long)len;
    }
    case 23: return dn_sys_result(dup((int)a0));
    case 24: { /* dup3 */
        int result = dup2((int)a0, (int)a1);
        if (result >= 0 && (a2 & 02000000))
            (void)fcntl(result, F_SETFD, FD_CLOEXEC);
        return dn_sys_result(result);
    }
    case 25: return dn_sys_fcntl((int)a0, (int)a1, a2);
    case 29: return dn_sys_ioctl((int)a0, (unsigned long)a1, DN_PTR(a2));
    case 34: { /* mkdirat */
        char p[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), p, sizeof(p));
        if (rc) return rc;
        if (dn_ensure_parent_dirs(p) != 0 && errno != EEXIST)
            return -dn_native_linux_errno_from_host(errno);
        return dn_sys_result(mkdir(p, (mode_t)a2));
    }
    case 35: { /* unlinkat */
        char p[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), p, sizeof(p));
        if (rc) return rc;
        return dn_sys_result((a2 & 0x200) ? rmdir(p) : unlink(p));
    }
    case 38: { /* renameat */
        char oldp[DN_PATH_CAP], newp[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), oldp, sizeof(oldp));
        if (rc) return rc;
        rc = dn_native_translate_at_path((int)a2, (const char *)DN_CPTR(a3), newp, sizeof(newp));
        if (rc) return rc;
        if (dn_ensure_parent_dirs(newp) != 0 && errno != EEXIST)
            return -dn_native_linux_errno_from_host(errno);
        return dn_sys_result(rename(oldp, newp));
    }
    case 46: return dn_sys_result(ftruncate((int)a0, (off_t)a1));
    case 47: /* fallocate: advisory for mode 0 */
        return a1 == 0 ? 0 : -DNLE_NOTSUP;
    case 48: { /* faccessat */
        char p[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), p, sizeof(p));
        if (rc) return rc;
        return dn_sys_result(access(p, (int)a2));
    }
    case 49: return dn_set_virtual_cwd((const char *)DN_CPTR(a0));
    case 50: {
        long r = dn_sys_result(fchdir((int)a0));
        if (r == 0) {
            pthread_mutex_lock(&dn_files_mutex);
            strcpy(dn_virtual_cwd, "/");
            pthread_mutex_unlock(&dn_files_mutex);
        }
        return r;
    }
    case 52: return dn_sys_result(fchmod((int)a0, (mode_t)a1));
    case 53: { /* fchmodat */
        char p[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), p, sizeof(p));
        if (rc) return rc;
        return dn_sys_result(chmod(p, (mode_t)a2));
    }
    case 54: return dn_sys_result(fchown((int)a0, (uid_t)a1, (gid_t)a2));
    case 56: return dn_sys_openat((int)a0, (const char *)DN_CPTR(a1), (int)a2, (mode_t)a3);
    case 57: dn_dir_forget_fd((int)a0); return dn_sys_result(close((int)a0));
    case 59: { /* pipe2 */
        int *fds = (int *)DN_PTR(a0);
        if (!fds) return -DNLE_FAULT;
        long r = dn_sys_result(pipe(fds));
        if (r == 0) {
            for (int i = 0; i < 2; ++i) {
                if (a1 & 02000000)
                    (void)fcntl(fds[i], F_SETFD, FD_CLOEXEC);
                if (a1 & 00004000)
                    (void)fcntl(fds[i], F_SETFL, O_NONBLOCK);
            }
        }
        return r;
    }
    case 61: return dn_sys_getdents64((int)a0, DN_PTR(a1), (size_t)a2);
    case 62: return dn_sys_result((long)lseek((int)a0, (off_t)a1, (int)a2));
    case 63: return dn_sys_result((long)read((int)a0, DN_PTR(a1), (size_t)a2));
    case 64: return dn_sys_result((long)write((int)a0, DN_CPTR(a1), (size_t)a2));
    case 65: return dn_sys_result((long)readv((int)a0, (const struct iovec *)DN_CPTR(a1), (int)a2));
    case 66: return dn_sys_result((long)writev((int)a0, (const struct iovec *)DN_CPTR(a1), (int)a2));
    case 67: return dn_sys_result((long)pread((int)a0, DN_PTR(a1), (size_t)a2, (off_t)a3));
    case 68: return dn_sys_result((long)pwrite((int)a0, DN_CPTR(a1), (size_t)a2, (off_t)a3));
    case 69: return dn_sys_result((long)preadv((int)a0, (const struct iovec *)DN_CPTR(a1), (int)a2, (off_t)a3));
    case 70: return dn_sys_result((long)pwritev((int)a0, (const struct iovec *)DN_CPTR(a1), (int)a2, (off_t)a3));
    case 73: { /* ppoll, signal mask ignored (no guest signal delivery yet) */
        int timeout_ms = -1;
        if (a2) {
            const struct timespec *ts = (const struct timespec *)DN_CPTR(a2);
            timeout_ms = (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
        }
        return dn_sys_result(poll(DN_PTR(a0), (unsigned long)a1, timeout_ms));
    }
    case 78: { /* readlinkat */
        char p[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), p, sizeof(p));
        if (rc) return rc;
        return dn_sys_result((long)readlink(p, (char *)DN_PTR(a2), (size_t)a3));
    }
    case 79: { /* newfstatat */
        const char *path = (const char *)DN_CPTR(a1);
        struct stat st;
        int r;
        if (!a3 || a3 == 0)
            (void)0;
        if ((!path || !path[0]) && (a3 & 0x1000)) {
            r = fstat((int)a0, &st);
        } else {
            char p[DN_PATH_CAP];
            int rc = dn_native_translate_at_path((int)a0, path, p, sizeof(p));
            if (rc) return rc;
            r = (a3 & 0x100) ? lstat(p, &st) : stat(p, &st);
        }
        if (r != 0)
            return -dn_native_linux_errno_from_host(errno);
        if (!a2) return -DNLE_FAULT;
        dn_fill_linux_stat(DN_PTR(a2), &st);
        return 0;
    }
    case 80: { /* fstat */
        struct stat st;
        if (fstat((int)a0, &st) != 0)
            return -dn_native_linux_errno_from_host(errno);
        if (!a1) return -DNLE_FAULT;
        dn_fill_linux_stat(DN_PTR(a1), &st);
        return 0;
    }
    case 82: return dn_sys_result(fsync((int)a0));
    case 83:
#if defined(__APPLE__)
        return dn_sys_result(fsync((int)a0));
#else
        return dn_sys_result(fdatasync((int)a0));
#endif
    case 84: { /* sync_file_range: closest durable host operation */
        return dn_sys_result(fsync((int)a0));
    }
    case 86: { /* linkat */
        char oldp[DN_PATH_CAP], newp[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), oldp, sizeof(oldp));
        if (rc) return rc;
        rc = dn_native_translate_at_path((int)a2, (const char *)DN_CPTR(a3), newp, sizeof(newp));
        if (rc) return rc;
        return dn_sys_result(link(oldp, newp));
    }
    case 88: { /* symlinkat */
        char p[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a1, (const char *)DN_CPTR(a2), p, sizeof(p));
        if (rc) return rc;
        return dn_sys_result(symlink((const char *)DN_CPTR(a0), p));
    }
    case 93:
    case 94:
        /* A guest _exit must not silently kill iDroid.  bionic abort
         * paths go through __stack_chk_fail/abort where termination is
         * intentional; raw exits are reported unimplemented. */
        return -DNLE_NOSYS;
    case 96: return dn_get_tid_value();       /* set_tid_address */
    case 98: return dn_sys_futex((uint32_t *)DN_PTR(a0), (int)a1, (uint32_t)a2,
                                 (const struct timespec *)DN_CPTR(a3),
                                 (uint32_t *)DN_PTR(a4), (uint32_t)a5);
    case 99: return 0;                        /* set_robust_list */
    case 101: return dn_sys_result(nanosleep((const struct timespec *)DN_CPTR(a0),
                                             (struct timespec *)DN_PTR(a1)));
    case 113: {
        int ok = 0;
        clockid_t clock_id = dn_host_clock((int)a0, &ok);
        if (!ok) return -DNLE_INVAL;
        return dn_sys_result(clock_gettime(clock_id, (struct timespec *)DN_PTR(a1)));
    }
    case 114: {
        int ok = 0;
        clockid_t clock_id = dn_host_clock((int)a0, &ok);
        if (!ok) return -DNLE_INVAL;
        return dn_sys_result(clock_getres(clock_id, (struct timespec *)DN_PTR(a1)));
    }
    case 115: {
        int ok = 0;
        clockid_t clock_id = dn_host_clock((int)a0, &ok);
        if (!ok) return -DNLE_INVAL;
        const struct timespec *request = (const struct timespec *)DN_CPTR(a2);
        struct timespec *remain = (struct timespec *)DN_PTR(a3);
        if (!request) return -DNLE_FAULT;
        struct timespec relative = *request;
        if (a1 & 1) { /* TIMER_ABSTIME */
            struct timespec now;
            if (clock_gettime(clock_id, &now) != 0)
                return -dn_native_linux_errno_from_host(errno);
            relative.tv_sec -= now.tv_sec;
            relative.tv_nsec -= now.tv_nsec;
            if (relative.tv_nsec < 0) {
                relative.tv_sec--;
                relative.tv_nsec += 1000000000L;
            }
            if (relative.tv_sec < 0) {
                relative.tv_sec = 0;
                relative.tv_nsec = 0;
            }
        }
        return dn_sys_result(nanosleep(&relative, remain));
    }
    case 122: return 0;                       /* sched_setaffinity (advisory) */
    case 123: { /* sched_getaffinity */
        if (a2 && a1 >= 8) {
            uint64_t mask = 0;
            long processors = sysconf(_SC_NPROCESSORS_ONLN);
            if (processors < 1) processors = 1;
            if (processors > 64) processors = 64;
            for (long i = 0; i < processors; ++i)
                mask |= 1ULL << i;
            memset(DN_PTR(a2), 0, (size_t)a1);
            memcpy(DN_PTR(a2), &mask, 8);
            return 8;
        }
        return -DNLE_INVAL;
    }
    case 124: return dn_sys_result(sched_yield());
    case 129: {
        int signal = dn_host_signal((int)a1);
        return signal < 0 ? -DNLE_INVAL : dn_sys_result(kill((pid_t)a0, signal));
    }
    case 134: /* rt_sigaction: accepted/no guest handler delivery */
    case 135: /* rt_sigprocmask */
        return 0;
    case 160: { /* uname: six 65-byte fields */
        if (!a0) return -DNLE_FAULT;
        char *fields = (char *)DN_PTR(a0);
        memset(fields, 0, 65 * 6);
        strcpy(fields + 65 * 0, "Linux");
        strcpy(fields + 65 * 1, "iDroid");
        strcpy(fields + 65 * 2, "6.6.0-droidnative");
        strcpy(fields + 65 * 3, "#1 DroidNative native translation");
        strcpy(fields + 65 * 4, "aarch64");
        strcpy(fields + 65 * 5, "localdomain");
        return 0;
    }
    case 167: { /* prctl */
        if (a0 == 15) { /* PR_SET_NAME */
#if defined(__APPLE__)
            (void)pthread_setname_np((const char *)DN_CPTR(a1));
#else
            (void)a1;
#endif
            return 0;
        }
        if (a0 == 16) { /* PR_GET_NAME */
            char *buffer = (char *)DN_PTR(a1);
            if (!buffer) return -DNLE_FAULT;
#if defined(__APPLE__)
            if (pthread_getname_np(pthread_self(), buffer, 16) != 0)
                strcpy(buffer, "iDroid");
#else
            strcpy(buffer, "iDroid");
#endif
            buffer[15] = '\0';
            return 0;
        }
        if (a0 == 38 || a0 == 41 || a0 == 535)
            return 0;
        return -DNLE_INVAL;
    }
    case 169: { /* gettimeofday */
        struct timeval tv;
        int r = gettimeofday(&tv, NULL);
        if (r == 0 && a0) {
            int64_t sec = (int64_t)tv.tv_sec;
            int64_t usec = (int64_t)tv.tv_usec;
            memcpy(DN_PTR(a0), &sec, 8);
            memcpy((uint8_t *)DN_PTR(a0) + 8, &usec, 8);
        }
        return dn_sys_result(r);
    }
    case 170: /* getcpu */
        if (a0) *(uint32_t *)DN_PTR(a0) = 0;
        if (a1) *(uint32_t *)DN_PTR(a1) = 0;
        return 0;
    case 172: return getpid();
    case 173: return getppid();
    case 174: return getuid();
    case 175: return geteuid();
    case 176: return getgid();
    case 177: return getegid();
    case 178: return dn_get_tid_value();
    case 179: { /* sysinfo (Linux layout, 112 bytes used) */
        if (!a0) return -DNLE_FAULT;
        uint8_t *p = (uint8_t *)DN_PTR(a0);
        memset(p, 0, 112);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t uptime = (int64_t)ts.tv_sec;
        memcpy(p + 0, &uptime, 8);
        uint64_t total = 0;
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0)
            total = (uint64_t)pages * (uint64_t)page_size;
        memcpy(p + 32, &total, 8);      /* totalram */
        memcpy(p + 40, &total, 8);      /* freeram (upper bound within app sandbox) */
        uint32_t unit = 1;
        memcpy(p + 96, &unit, 4);       /* mem_unit */
        return 0;
    }
    case 214: return -DNLE_NOMEM;             /* brk: guest mallocs must use mmap */
    case 215: return dn_sys_result(munmap(DN_PTR(a0), (size_t)a1));
    case 220: return -DNLE_NOSYS;             /* clone */
    case 221: return -8;                      /* execve -> ENOEXEC */
    case 222: { /* mmap */
        int linux_flags = (int)a3;
        int flags = 0;
        if (linux_flags & 0x01) flags |= MAP_SHARED;
        if (linux_flags & 0x02) flags |= MAP_PRIVATE;
        if (linux_flags & 0x10) flags |= MAP_FIXED;
        if (linux_flags & 0x20) flags |= MAP_ANON;
#ifdef MAP_NORESERVE
        if (linux_flags & 0x4000) flags |= MAP_NORESERVE;
#endif
        void *mapped = mmap(DN_PTR(a0), (size_t)a1, (int)a2, flags, (int)a4, (off_t)a5);
        if (mapped == MAP_FAILED)
            return -dn_native_linux_errno_from_host(errno);
        return (long)(uintptr_t)mapped;
    }
    case 226: return dn_sys_result(mprotect(DN_PTR(a0), (size_t)a1, (int)a2));
    case 227: return dn_sys_result(msync(DN_PTR(a0), (size_t)a1, (int)a2));
    case 232: /* mincore: report resident */
        if (a2 && a1 > 0) {
            size_t pages = ((size_t)a1 + 4095u) / 4096u;
            memset(DN_PTR(a2), 1, pages);
        }
        return 0;
    case 233: return 0;                       /* madvise is advisory */
    case 261: { /* prlimit64, common resource IDs only */
        int resource;
        switch ((int)a1) {
        case 0: resource = RLIMIT_CPU; break;
        case 1: resource = RLIMIT_FSIZE; break;
        case 2: resource = RLIMIT_DATA; break;
        case 3: resource = RLIMIT_STACK; break;
        case 4: resource = RLIMIT_CORE; break;
        case 7: resource = RLIMIT_NOFILE; break;
#ifdef RLIMIT_AS
        case 9: resource = RLIMIT_AS; break;
#endif
        default: return -DNLE_INVAL;
        }
        if (a2) {
            const uint64_t *new_limit = (const uint64_t *)DN_CPTR(a2);
            struct rlimit rl;
            rl.rlim_cur = (rlim_t)new_limit[0];
            rl.rlim_max = (rlim_t)new_limit[1];
            if (setrlimit(resource, &rl) != 0)
                return -dn_native_linux_errno_from_host(errno);
        }
        if (a3) {
            struct rlimit rl;
            if (getrlimit(resource, &rl) != 0)
                return -dn_native_linux_errno_from_host(errno);
            uint64_t *old_limit = (uint64_t *)DN_PTR(a3);
            old_limit[0] = rl.rlim_cur == RLIM_INFINITY ? UINT64_MAX : (uint64_t)rl.rlim_cur;
            old_limit[1] = rl.rlim_max == RLIM_INFINITY ? UINT64_MAX : (uint64_t)rl.rlim_max;
        }
        return 0;
    }
    case 276: { /* renameat2 without flags */
        if (a4 != 0) return -DNLE_INVAL;
        char oldp[DN_PATH_CAP], newp[DN_PATH_CAP];
        int rc = dn_native_translate_at_path((int)a0, (const char *)DN_CPTR(a1), oldp, sizeof(oldp));
        if (rc) return rc;
        rc = dn_native_translate_at_path((int)a2, (const char *)DN_CPTR(a3), newp, sizeof(newp));
        if (rc) return rc;
        return dn_sys_result(rename(oldp, newp));
    }
    case 278: { /* getrandom */
        uint8_t *buffer = (uint8_t *)DN_PTR(a0);
        size_t wanted = (size_t)a1;
        if (!buffer && wanted) return -DNLE_FAULT;
#if defined(__APPLE__)
        arc4random_buf(buffer, wanted);
        return (long)wanted;
#else
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -dn_native_linux_errno_from_host(errno);
        size_t done = 0;
        while (done < wanted) {
            ssize_t got = read(fd, buffer + done, wanted - done);
            if (got <= 0) {
                int e = errno;
                close(fd);
                return done ? (long)done : -dn_native_linux_errno_from_host(e);
            }
            done += (size_t)got;
        }
        close(fd);
        return (long)done;
#endif
    }
    case 283: return 0;                       /* membarrier query/private */
    case 334: return -DNLE_NOSYS;             /* rseq */
    case 435: return -DNLE_NOSYS;             /* clone3 */
    case 436: { /* close_range */
        unsigned first = (unsigned)a0;
        unsigned last = (unsigned)a1;
        if (last < first) return -DNLE_INVAL;
        if (last - first > 65536u) last = first + 65536u;
        for (unsigned fd = first; fd <= last; ++fd) {
            dn_dir_forget_fd((int)fd);
            (void)close((int)fd);
        }
        return 0;
    }
    default:
        return -DNLE_NOSYS;
    }
}

/* ------------------------------------------------------------------ */
/* Host/device self-test                                               */
/* ------------------------------------------------------------------ */

static void dn_selftest_append(char *out, size_t out_len, const char *fmt, ...)
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

int dn_native_run_selftest(char *out, size_t out_len)
{
    if (out && out_len)
        out[0] = '\0';
    int failures = 0;

    char error[160] = "";
    int init_rc = dn_native_patch_init(error, sizeof(error));
    dn_selftest_append(out, out_len, "patch init rc=%d %s: %s", init_rc, error,
                       init_rc == 0 ? "OK" : "MISMATCH");
    if (init_rc != 0)
        return -1;

    void *tp = dn_native_thread_pointer();
    uintptr_t guard = tp ? ((uintptr_t *)tp)[5] : 0;
    bool tp_ok = tp && ((void **)tp)[0] == tp && guard != 0 && (guard & 0xffu) == 0;
    dn_selftest_append(out, out_len, "bionic thread pointer/stack guard: %s",
                       tp_ok ? "OK" : "MISMATCH");
    failures += tp_ok ? 0 : 1;

    dn_native_set_files_root(dn_native_files_root());
    char translated[DN_PATH_CAP] = "";
    int path_rc = dn_native_translate_path(
        "/data/data/ro.droidnative.app/files/../files/probe.txt",
        translated, sizeof(translated));
    bool path_ok = path_rc == 0 && strstr(translated, "/internal/files/probe.txt") != NULL;
    dn_selftest_append(out, out_len, "Android VFS translation -> %s: %s",
                       translated, path_ok ? "OK" : "MISMATCH");
    failures += path_ok ? 0 : 1;

    long fd = dn_native_syscall_dispatch(
        -100,
        (long)(uintptr_t)"/data/data/ro.droidnative.app/files/dn-patch-selftest.txt",
        00001102 /* O_RDWR|O_CREAT|O_TRUNC in Linux octal ABI */,
        0644, 0, 0, 56);
    bool file_ok = fd >= 0;
    if (file_ok) {
        const char payload[] = "DN42";
        long written = dn_native_syscall_dispatch(fd, (long)(uintptr_t)payload, 4, 0, 0, 0, 64);
        long seek = dn_native_syscall_dispatch(fd, 0, 0, 0, 0, 0, 62);
        char readback[5] = "";
        long got = dn_native_syscall_dispatch(fd, (long)(uintptr_t)readback, 4, 0, 0, 0, 63);
        uint8_t statbuf[128];
        long stat_rc = dn_native_syscall_dispatch(fd, (long)(uintptr_t)statbuf, 0, 0, 0, 0, 80);
        uint64_t linux_size = 0;
        memcpy(&linux_size, statbuf + 48, 8);
        file_ok = written == 4 && seek == 0 && got == 4 &&
                  memcmp(readback, payload, 4) == 0 && stat_rc == 0 && linux_size == 4;
        (void)dn_native_syscall_dispatch(fd, 0, 0, 0, 0, 0, 57);
    }
    dn_selftest_append(out, out_len, "patched-svc openat/write/read/fstat path: %s",
                       file_ok ? "OK" : "MISMATCH");
    failures += file_ok ? 0 : 1;

    static const uint8_t tls_template[] = { 'D', 'N', '-', 'T', 'L', 'S', '4', '2' };
    dn_native_tls_module_t *tls_module =
        dn_native_tls_register(tls_template, sizeof(tls_template), 32, 16);
    bool tls_ok = false;
    if (tls_module) {
        void *bytes = dn_native_tls_address(tls_module, 0);
        tls_ok = bytes && memcmp(bytes, tls_template, sizeof(tls_template)) == 0 &&
                 ((uint8_t *)bytes)[16] == 0;
        void *arg = dn_native_tls_descriptor_arg(tls_module, 2);
        uintptr_t descriptor[2] = { (uintptr_t)dn_native_tlsdesc_entry, (uintptr_t)arg };
        intptr_t delta = dn_native_tlsdesc_resolve(descriptor);
        tls_ok = tls_ok && arg && delta ==
                 (intptr_t)(dn_native_tls_module_offset(tls_module) + 2);
        dn_native_tls_unregister(tls_module);
    }
    dn_selftest_append(out, out_len, "ELF TLS template/TLSDESC storage: %s",
                       tls_ok ? "OK" : "MISMATCH");
    failures += tls_ok ? 0 : 1;

    uint32_t futex_word = 7;
    long futex_rc = dn_native_syscall_dispatch(
        (long)(uintptr_t)&futex_word, 128 /* FUTEX_WAIT_PRIVATE */, 9, 0, 0, 0, 98);
    bool futex_ok = futex_rc == -DNLE_AGAIN;
    dn_selftest_append(out, out_len, "futex changed-value rejection: %s",
                       futex_ok ? "OK" : "MISMATCH");
    failures += futex_ok ? 0 : 1;

    dn_selftest_append(out, out_len, "%s",
                       failures == 0 ? "host verdict: PASS" : "host verdict: FAIL");
    return failures == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Register-preserving AArch64 island entries                          */
/* ------------------------------------------------------------------ */

#if defined(__APPLE__) && defined(__aarch64__)

/* svc contract: preserve every register except x0, including SIMD and
 * condition/FPSR state, exactly as a kernel syscall boundary does. */
__asm__(
    ".section __TEXT,__text,regular,pure_instructions\n"
    ".globl _dn_native_svc_entry\n"
    ".p2align 2\n"
    "_dn_native_svc_entry:\n"
    "sub sp, sp, #720\n"
    "stp x1, x2, [sp, #0]\n"
    "stp x3, x4, [sp, #16]\n"
    "stp x5, x6, [sp, #32]\n"
    "stp x7, x8, [sp, #48]\n"
    "stp x9, x10, [sp, #64]\n"
    "stp x11, x12, [sp, #80]\n"
    "stp x13, x14, [sp, #96]\n"
    "stp x15, x16, [sp, #112]\n"
    "stp x17, x18, [sp, #128]\n"
    "stp x29, x30, [sp, #144]\n"
    "mrs x16, nzcv\n"
    "mrs x17, fpsr\n"
    "stp x16, x17, [sp, #160]\n"
    "stp q0, q1, [sp, #192]\n"
    "stp q2, q3, [sp, #224]\n"
    "stp q4, q5, [sp, #256]\n"
    "stp q6, q7, [sp, #288]\n"
    "stp q8, q9, [sp, #320]\n"
    "stp q10, q11, [sp, #352]\n"
    "stp q12, q13, [sp, #384]\n"
    "stp q14, q15, [sp, #416]\n"
    "stp q16, q17, [sp, #448]\n"
    "stp q18, q19, [sp, #480]\n"
    "stp q20, q21, [sp, #512]\n"
    "stp q22, q23, [sp, #544]\n"
    "stp q24, q25, [sp, #576]\n"
    "stp q26, q27, [sp, #608]\n"
    "stp q28, q29, [sp, #640]\n"
    "stp q30, q31, [sp, #672]\n"
    "mov x6, x8\n"
    "bl _dn_native_syscall_dispatch\n"
    "str x0, [sp, #704]\n"
    "ldp q0, q1, [sp, #192]\n"
    "ldp q2, q3, [sp, #224]\n"
    "ldp q4, q5, [sp, #256]\n"
    "ldp q6, q7, [sp, #288]\n"
    "ldp q8, q9, [sp, #320]\n"
    "ldp q10, q11, [sp, #352]\n"
    "ldp q12, q13, [sp, #384]\n"
    "ldp q14, q15, [sp, #416]\n"
    "ldp q16, q17, [sp, #448]\n"
    "ldp q18, q19, [sp, #480]\n"
    "ldp q20, q21, [sp, #512]\n"
    "ldp q22, q23, [sp, #544]\n"
    "ldp q24, q25, [sp, #576]\n"
    "ldp q26, q27, [sp, #608]\n"
    "ldp q28, q29, [sp, #640]\n"
    "ldp q30, q31, [sp, #672]\n"
    "ldp x16, x17, [sp, #160]\n"
    "msr nzcv, x16\n"
    "msr fpsr, x17\n"
    "ldp x1, x2, [sp, #0]\n"
    "ldp x3, x4, [sp, #16]\n"
    "ldp x5, x6, [sp, #32]\n"
    "ldp x7, x8, [sp, #48]\n"
    "ldp x9, x10, [sp, #64]\n"
    "ldp x11, x12, [sp, #80]\n"
    "ldp x13, x14, [sp, #96]\n"
    "ldp x15, x16, [sp, #112]\n"
    "ldp x17, x18, [sp, #128]\n"
    "ldp x29, x30, [sp, #144]\n"
    "ldr x0, [sp, #704]\n"
    "add sp, sp, #720\n"
    "ret\n"
);

/* TP slow contract: return the new thread pointer in x16; preserve
 * all other guest-visible state. */
__asm__(
    ".section __TEXT,__text,regular,pure_instructions\n"
    ".globl _dn_native_tp_slow_entry\n"
    ".p2align 2\n"
    "_dn_native_tp_slow_entry:\n"
    "sub sp, sp, #720\n"
    "stp x0, x1, [sp, #0]\n"
    "stp x2, x3, [sp, #16]\n"
    "stp x4, x5, [sp, #32]\n"
    "stp x6, x7, [sp, #48]\n"
    "stp x8, x9, [sp, #64]\n"
    "stp x10, x11, [sp, #80]\n"
    "stp x12, x13, [sp, #96]\n"
    "stp x14, x15, [sp, #112]\n"
    "stp x16, x17, [sp, #128]\n"
    "stp x18, x30, [sp, #144]\n"
    "mrs x16, nzcv\n"
    "mrs x17, fpsr\n"
    "stp x16, x17, [sp, #160]\n"
    "stp q0, q1, [sp, #192]\n"
    "stp q2, q3, [sp, #224]\n"
    "stp q4, q5, [sp, #256]\n"
    "stp q6, q7, [sp, #288]\n"
    "stp q8, q9, [sp, #320]\n"
    "stp q10, q11, [sp, #352]\n"
    "stp q12, q13, [sp, #384]\n"
    "stp q14, q15, [sp, #416]\n"
    "stp q16, q17, [sp, #448]\n"
    "stp q18, q19, [sp, #480]\n"
    "stp q20, q21, [sp, #512]\n"
    "stp q22, q23, [sp, #544]\n"
    "stp q24, q25, [sp, #576]\n"
    "stp q26, q27, [sp, #608]\n"
    "stp q28, q29, [sp, #640]\n"
    "stp q30, q31, [sp, #672]\n"
    "bl _dn_native_tp_allocate_slow\n"
    "str x0, [sp, #704]\n"
    "ldp q0, q1, [sp, #192]\n"
    "ldp q2, q3, [sp, #224]\n"
    "ldp q4, q5, [sp, #256]\n"
    "ldp q6, q7, [sp, #288]\n"
    "ldp q8, q9, [sp, #320]\n"
    "ldp q10, q11, [sp, #352]\n"
    "ldp q12, q13, [sp, #384]\n"
    "ldp q14, q15, [sp, #416]\n"
    "ldp q16, q17, [sp, #448]\n"
    "ldp q18, q19, [sp, #480]\n"
    "ldp q20, q21, [sp, #512]\n"
    "ldp q22, q23, [sp, #544]\n"
    "ldp q24, q25, [sp, #576]\n"
    "ldp q26, q27, [sp, #608]\n"
    "ldp q28, q29, [sp, #640]\n"
    "ldp q30, q31, [sp, #672]\n"
    "ldp x16, x17, [sp, #160]\n"
    "msr nzcv, x16\n"
    "msr fpsr, x17\n"
    "ldp x0, x1, [sp, #0]\n"
    "ldp x2, x3, [sp, #16]\n"
    "ldp x4, x5, [sp, #32]\n"
    "ldp x6, x7, [sp, #48]\n"
    "ldp x8, x9, [sp, #64]\n"
    "ldp x10, x11, [sp, #80]\n"
    "ldp x12, x13, [sp, #96]\n"
    "ldp x14, x15, [sp, #112]\n"
    "ldp x17, x18, [sp, #136]\n"
    "ldr x30, [sp, #152]\n"
    "ldr x16, [sp, #704]\n"
    "add sp, sp, #720\n"
    "ret\n"
);

/* TLSDESC contract: descriptor address in x0, TP-relative offset out
 * in x0.  We preserve more state than the ABI strictly requires. */
__asm__(
    ".section __TEXT,__text,regular,pure_instructions\n"
    ".globl _dn_native_tlsdesc_entry\n"
    ".p2align 2\n"
    "_dn_native_tlsdesc_entry:\n"
    "sub sp, sp, #720\n"
    "stp x1, x2, [sp, #0]\n"
    "stp x3, x4, [sp, #16]\n"
    "stp x5, x6, [sp, #32]\n"
    "stp x7, x8, [sp, #48]\n"
    "stp x9, x10, [sp, #64]\n"
    "stp x11, x12, [sp, #80]\n"
    "stp x13, x14, [sp, #96]\n"
    "stp x15, x16, [sp, #112]\n"
    "stp x17, x18, [sp, #128]\n"
    "stp x29, x30, [sp, #144]\n"
    "mrs x16, nzcv\n"
    "mrs x17, fpsr\n"
    "stp x16, x17, [sp, #160]\n"
    "stp q0, q1, [sp, #192]\n"
    "stp q2, q3, [sp, #224]\n"
    "stp q4, q5, [sp, #256]\n"
    "stp q6, q7, [sp, #288]\n"
    "stp q8, q9, [sp, #320]\n"
    "stp q10, q11, [sp, #352]\n"
    "stp q12, q13, [sp, #384]\n"
    "stp q14, q15, [sp, #416]\n"
    "stp q16, q17, [sp, #448]\n"
    "stp q18, q19, [sp, #480]\n"
    "stp q20, q21, [sp, #512]\n"
    "stp q22, q23, [sp, #544]\n"
    "stp q24, q25, [sp, #576]\n"
    "stp q26, q27, [sp, #608]\n"
    "stp q28, q29, [sp, #640]\n"
    "stp q30, q31, [sp, #672]\n"
    "bl _dn_native_tlsdesc_resolve\n"
    "str x0, [sp, #704]\n"
    "ldp q0, q1, [sp, #192]\n"
    "ldp q2, q3, [sp, #224]\n"
    "ldp q4, q5, [sp, #256]\n"
    "ldp q6, q7, [sp, #288]\n"
    "ldp q8, q9, [sp, #320]\n"
    "ldp q10, q11, [sp, #352]\n"
    "ldp q12, q13, [sp, #384]\n"
    "ldp q14, q15, [sp, #416]\n"
    "ldp q16, q17, [sp, #448]\n"
    "ldp q18, q19, [sp, #480]\n"
    "ldp q20, q21, [sp, #512]\n"
    "ldp q22, q23, [sp, #544]\n"
    "ldp q24, q25, [sp, #576]\n"
    "ldp q26, q27, [sp, #608]\n"
    "ldp q28, q29, [sp, #640]\n"
    "ldp q30, q31, [sp, #672]\n"
    "ldp x16, x17, [sp, #160]\n"
    "msr nzcv, x16\n"
    "msr fpsr, x17\n"
    "ldp x1, x2, [sp, #0]\n"
    "ldp x3, x4, [sp, #16]\n"
    "ldp x5, x6, [sp, #32]\n"
    "ldp x7, x8, [sp, #48]\n"
    "ldp x9, x10, [sp, #64]\n"
    "ldp x11, x12, [sp, #80]\n"
    "ldp x13, x14, [sp, #96]\n"
    "ldp x15, x16, [sp, #112]\n"
    "ldp x17, x18, [sp, #128]\n"
    "ldp x29, x30, [sp, #144]\n"
    "ldr x0, [sp, #704]\n"
    "add sp, sp, #720\n"
    "ret\n"
);

#else

/* Host-side link seam: converted AArch64 islands can only run on the
 * target, but tests still verify slot values and the C dispatchers. */
void dn_native_svc_entry(void) { }
void dn_native_tp_slow_entry(void) { (void)dn_native_tp_allocate_slow(); }
void dn_native_tlsdesc_entry(void) { }

#endif
