#define _POSIX_C_SOURCE 200809L

#include "QEMUBridge.h"

/* APFS copy-on-write cloning for an isolated GPU trial. Source and destination
 * are regular files on the same iOS volume. No byte-copy fallback: if APFS
 * cloning is unavailable, never consume gigabytes or touch the work disks. */
#ifdef __APPLE__
#include <sys/clonefile.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
/* iPhoneOS 27.1 ships IOSurfaceRef.h (C API), not macOS's IOSurface.h. */
#include <IOSurface/IOSurfaceRef.h> /* four app-created BGRA surfaces */
#endif
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

int dn_gpu_trial_clone_file(const char *source, const char *destination)
{
    if (!source || !destination || !source[0] || !destination[0]) {
        return EINVAL;
    }
#ifdef __APPLE__
    /* clonefile requires a NONEXISTENT destination, and fails instead of
     * silently doing a full copy when APFS cloning is unavailable. Swift
     * creates a unique staging directory and validates the file sizes. */
    return clonefile(source, destination, 0) == 0 ? 0 : errno;
#else
    /* Linux host gates can syntax-check the bridge; they must not mistake
     * a normal filesystem copy for a successful iOS APFS clone. */
    return ENOTSUP;
#endif
}

/* ---- Capcana de crash: dovada sincrona pe disc la semnale fatale ---- */
static int g_crash_fd = -1;

static void dn_crash_append(char *buf, size_t *len, size_t cap, const char *s)
{
    while (*s && *len + 1 < cap) {
        buf[(*len)++] = *s++;
    }
}

static void dn_crash_append_num(char *buf, size_t *len, size_t cap,
                                unsigned long long v, int hex)
{
    char tmp[32];
    int i = 0;
    if (hex) {
        dn_crash_append(buf, len, cap, "0x");
        if (v == 0) {
            dn_crash_append(buf, len, cap, "0");
            return;
        }
        while (v && i < (int)sizeof(tmp)) {
            tmp[i++] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        }
    } else {
        if (v == 0) {
            dn_crash_append(buf, len, cap, "0");
            return;
        }
        while (v && i < (int)sizeof(tmp)) {
            tmp[i++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (i > 0) {
        char c[2] = { tmp[--i], '\0' };
        dn_crash_append(buf, len, cap, c);
    }
}

static void dn_crash_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)uctx;
    if (g_crash_fd >= 0) {
        char buf[256];
        size_t len = 0;
        dn_crash_append(buf, &len, sizeof(buf),
                        "99 crash-trap: semnal fatal ");
        dn_crash_append_num(buf, &len, sizeof(buf),
                            (unsigned long long)sig, 0);
        dn_crash_append(buf, &len, sizeof(buf), " la adresa ");
        dn_crash_append_num(buf, &len, sizeof(buf),
                            (unsigned long long)(uintptr_t)
                            (info ? info->si_addr : NULL), 1);
        dn_crash_append(buf, &len, sizeof(buf),
                        " - the process is dying from a native fault "
                        "(see also the .ips in Settings > Analytics). "
                        "Without this line, the death = iOS jetsam/watchdog.\n");
        (void)!write(g_crash_fd, buf, len);
        fsync(g_crash_fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

void dn_install_crash_trap(const char *breadcrumb_path)
{
    if (breadcrumb_path == NULL) {
        return;
    }
    if (g_crash_fd < 0) {
        g_crash_fd = open(breadcrumb_path, O_WRONLY | O_CREAT | O_APPEND,
                          0644);
        if (g_crash_fd < 0) {
            return;
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = dn_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT,
                                SIGILL, SIGFPE, SIGSYS };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

typedef void (*DNQEMUInitFn)(int argc, char **argv);
typedef int (*DNQEMUMainLoopFn)(void);
typedef void (*DNQEMUCleanupFn)(int status);
typedef void (*DNBQLLockFn)(const char *file, int line);
typedef void (*DNVoidFn)(void);

static void *g_library_handle;
static DNQEMUInitFn g_qemu_init;
static DNQEMUMainLoopFn g_qemu_main_loop;
static DNQEMUCleanupFn g_qemu_cleanup;
static DNBQLLockFn g_bql_lock;
static DNVoidFn g_bql_unlock;
static DNVoidFn g_replay_mutex_lock;
static DNVoidFn g_replay_mutex_unlock;

/* Simboluri optionale din podul compilat in QEMU (app/QEMUExtra). Structura
 * frame struct mirrors exactly DNQFrameInfo in dn-display.h (same layout). */
typedef struct DNQFrameInfoMirror {
    const void *pixels;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t bpp;
    uint64_t generation;
    uint64_t sequence;
} DNQFrameInfoMirror;

typedef struct DNQDisplayDamageMirror {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint64_t generation;
    uint64_t sequence;
} DNQDisplayDamageMirror;

typedef bool (*DNQLockFrameFn)(DNQFrameInfoMirror *out);
typedef uint64_t (*DNQSequenceFn)(void);
typedef bool (*DNQTakeDamageFn)(DNQDisplayDamageMirror *out);
typedef void (*DNQPointerFn)(int32_t x, int32_t y, bool button_down);
typedef bool (*DNQPointerAsyncFn)(int32_t x, int32_t y, bool button_down);
typedef void (*DNQSizeFn)(int32_t width, int32_t height);
typedef void (*DNQGuestSizeFn)(int32_t *width, int32_t *height);
typedef int (*DNQSnapshotStateFn)(void);
typedef bool (*DNQSnapshotLoadFn)(void);

static DNVoidFn g_dnq_display_init;
static DNQLockFrameFn g_dnq_display_lock_frame;
static DNVoidFn g_dnq_display_unlock_frame;
static DNQSequenceFn g_dnq_display_sequence;
static DNQTakeDamageFn g_dnq_display_take_damage;
typedef void (*DNQSendTextFn)(const char *utf8);
typedef void *(*DNQMetalAcquireFn)(int32_t *w, int32_t *h, bool *y0top, uint64_t *gen);
typedef void (*DNQMetalReleaseFn)(void *tex);
typedef bool (*DNQIORegisterFn)(void *const surfaces[4], int32_t w, int32_t h);
typedef void *(*DNQIOAcquireFn)(int32_t *w, int32_t *h, bool *flip_y, uint64_t *ticket);
typedef void (*DNQIOReleaseFn)(void *surface);
typedef void (*DNQGuestDpiFn)(int32_t dpi);

static DNQPointerFn g_dnq_display_send_pointer;
static DNQPointerAsyncFn g_dnq_display_send_pointer_async;
static DNVoidFn g_dnq_display_shutdown;
static DNQSendTextFn g_dnq_display_send_text;
static DNQMetalAcquireFn g_dnq_display_metal_acquire;
static DNQMetalReleaseFn g_dnq_display_metal_release;
static DNQIORegisterFn g_dnq_display_iosurface_register;
static DNQIOAcquireFn g_dnq_display_iosurface_acquire;
static DNQIOReleaseFn g_dnq_display_iosurface_release;
static DNVoidFn g_dnq_display_iosurface_disable;
static int32_t g_io_requested_w, g_io_requested_h; /* before run; 0 = PBO */
static DNQGuestDpiFn g_dnq_display_set_guest_dpi;
static DNQGuestDpiFn g_dnq_display_set_guest_refresh_rate;
static DNQSizeFn g_dnq_display_set_ui_size;
static DNQGuestSizeFn g_dnq_display_guest_size;
static DNVoidFn g_dnq_snapshot_save;
static DNQSnapshotStateFn g_dnq_snapshot_state;
static DNQSnapshotLoadFn g_dnq_snapshot_load_at_startup;
static DNQSnapshotLoadFn g_dnq_gl_early_init; /* bool (void), Faza B */

/* Writes a launch stage, synchronously to disk. If qemu_init exits with exit()
 * or abort, the last line stays proof of the exact place of death. */
static void dn_breadcrumb(const char *path, const char *format, ...)
{
    if (path == NULL) {
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    dprintf(fd, "%s\n", buffer);
    fsync(fd);
    close(fd);
}

static bool dn_set_error(char **error_out, const char *format, ...)
{
    if (error_out != NULL) {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        *error_out = strdup(buffer);
    }
    return false;
}

static void *dn_required_symbol(void *handle, const char *name, char **error_out)
{
    dlerror();
    void *symbol = dlsym(handle, name);
    const char *error = dlerror();
    if (symbol == NULL || error != NULL) {
        dn_set_error(error_out, "Missing QEMU symbol: %s (%s)",
                     name, error != NULL ? error : "dlsym a returnat NULL");
        return NULL;
    }
    return symbol;
}

bool dn_qemu_load(const char *library_path, char **error_out)
{
    if (error_out != NULL) {
        *error_out = NULL;
    }
    if (g_library_handle != NULL) {
        return true;
    }
    if (library_path == NULL || library_path[0] == '\0') {
        return dn_set_error(error_out, "The QEMU library path is empty.");
    }

    dlerror();
    void *handle = dlopen(library_path, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        const char *error = dlerror();
        return dn_set_error(error_out, "QEMU dlopen failed: %s",
                            error != NULL ? error : "unknown error");
    }

    void *sym_init = dn_required_symbol(handle, "qemu_init", error_out);
    void *sym_loop = dn_required_symbol(handle, "qemu_main_loop", error_out);
    void *sym_cleanup = dn_required_symbol(handle, "qemu_cleanup", error_out);
    void *sym_bql_lock = dn_required_symbol(handle, "bql_lock_impl", error_out);
    void *sym_bql_unlock = dn_required_symbol(handle, "bql_unlock", error_out);
    void *sym_replay_lock = dn_required_symbol(handle, "replay_mutex_lock", error_out);
    void *sym_replay_unlock = dn_required_symbol(handle, "replay_mutex_unlock", error_out);

    if (sym_init == NULL || sym_loop == NULL || sym_cleanup == NULL ||
        sym_bql_lock == NULL || sym_bql_unlock == NULL ||
        sym_replay_lock == NULL || sym_replay_unlock == NULL) {
        dlclose(handle);
        return false;
    }

    g_qemu_init = (DNQEMUInitFn)(uintptr_t)sym_init;
    g_qemu_main_loop = (DNQEMUMainLoopFn)(uintptr_t)sym_loop;
    g_qemu_cleanup = (DNQEMUCleanupFn)(uintptr_t)sym_cleanup;
    g_bql_lock = (DNBQLLockFn)(uintptr_t)sym_bql_lock;
    g_bql_unlock = (DNVoidFn)(uintptr_t)sym_bql_unlock;
    g_replay_mutex_lock = (DNVoidFn)(uintptr_t)sym_replay_lock;
    g_replay_mutex_unlock = (DNVoidFn)(uintptr_t)sym_replay_unlock;

    /* Display/touch/snapshot: OPTIONAL. An older framework (no
     * dn-display integration) leave them NULL and the app runs serial-only. */
    g_dnq_display_init = (DNVoidFn)(uintptr_t)dlsym(handle, "dnq_display_init");
    g_dnq_display_lock_frame = (DNQLockFrameFn)(uintptr_t)dlsym(handle, "dnq_display_lock_frame");
    g_dnq_display_unlock_frame = (DNVoidFn)(uintptr_t)dlsym(handle, "dnq_display_unlock_frame");
    g_dnq_display_sequence = (DNQSequenceFn)(uintptr_t)dlsym(handle, "dnq_display_sequence");
    g_dnq_display_take_damage = (DNQTakeDamageFn)(uintptr_t)dlsym(handle, "dnq_display_take_damage");
    g_dnq_display_send_pointer = (DNQPointerFn)(uintptr_t)dlsym(handle, "dnq_display_send_pointer");
    g_dnq_display_send_pointer_async = (DNQPointerAsyncFn)(uintptr_t)dlsym(handle, "dnq_display_send_pointer_async");
    g_dnq_display_shutdown = (DNVoidFn)(uintptr_t)dlsym(handle, "dnq_display_shutdown");
    g_dnq_display_send_text = (DNQSendTextFn)(uintptr_t)dlsym(handle, "dnq_display_send_text");
    g_dnq_display_metal_acquire = (DNQMetalAcquireFn)(uintptr_t)dlsym(handle, "dnq_display_metal_acquire");
    g_dnq_display_metal_release = (DNQMetalReleaseFn)(uintptr_t)dlsym(handle, "dnq_display_metal_release");
    g_dnq_display_iosurface_register = (DNQIORegisterFn)(uintptr_t)dlsym(handle, "dnq_display_iosurface_register");
    g_dnq_display_iosurface_acquire = (DNQIOAcquireFn)(uintptr_t)dlsym(handle, "dnq_display_iosurface_acquire");
    g_dnq_display_iosurface_release = (DNQIOReleaseFn)(uintptr_t)dlsym(handle, "dnq_display_iosurface_release");
    g_dnq_display_iosurface_disable = (DNVoidFn)(uintptr_t)dlsym(handle, "dnq_display_iosurface_disable");
    g_dnq_display_set_guest_dpi = (DNQGuestDpiFn)(uintptr_t)dlsym(handle, "dnq_display_set_guest_dpi");
    g_dnq_display_set_guest_refresh_rate = (DNQGuestDpiFn)(uintptr_t)dlsym(handle, "dnq_display_set_guest_refresh_rate");
    g_dnq_display_set_ui_size = (DNQSizeFn)(uintptr_t)dlsym(handle, "dnq_display_set_ui_size");
    g_dnq_display_guest_size = (DNQGuestSizeFn)(uintptr_t)dlsym(handle, "dnq_display_guest_size");
    g_dnq_snapshot_save = (DNVoidFn)(uintptr_t)dlsym(handle, "dnq_snapshot_save");
    g_dnq_snapshot_state = (DNQSnapshotStateFn)(uintptr_t)dlsym(handle, "dnq_snapshot_state");
    g_dnq_snapshot_load_at_startup = (DNQSnapshotLoadFn)(uintptr_t)dlsym(handle, "dnq_snapshot_load_at_startup");
    g_dnq_gl_early_init = (DNQSnapshotLoadFn)(uintptr_t)dlsym(handle, "dnq_gl_early_init");
    dlerror(); /* clear the optional dlsym error; not fatal */

    g_library_handle = handle;
    return true;
}

void dn_display_configure_iosurface(bool enabled, int32_t width, int32_t height)
{
    /* Set on Swift's boot thread BEFORE it creates the QEMU main thread.
     * Only the isolated, two-vCPU EAGL clone may ask for this allocation. */
    if (enabled && width > 0 && height > 0 &&
        (uint64_t)width * (uint64_t)height <= 720u * 1280u) {
        g_io_requested_w = width;
        g_io_requested_h = height;
    } else {
        g_io_requested_w = g_io_requested_h = 0;
    }
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
#define DN_BRIDGE_IO_SLOTS 4
static void dn_io_property(CFMutableDictionaryRef dict, CFStringRef key,
                           int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault,
                                        kCFNumberSInt32Type, &value);
    if (number) {
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }
}
#endif

static void dn_bridge_register_iosurfaces(const char *breadcrumb_path)
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    if (!g_io_requested_w || !g_io_requested_h ||
        !g_dnq_display_iosurface_register || !g_dnq_display_iosurface_acquire ||
        !g_dnq_display_iosurface_release || !g_dnq_display_iosurface_disable ||
        !getenv("DN_GL")) {
        return; /* software, old framework, or explicitly disabled: PBO */
    }
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                        &kCFTypeDictionaryKeyCallBacks,
                        &kCFTypeDictionaryValueCallBacks);
    if (!props) {
        dn_breadcrumb(breadcrumb_path, "12d bridge: IOSurface properties unavailable; PBO fallback");
        return;
    }
    dn_io_property(props, kIOSurfaceWidth, g_io_requested_w);
    dn_io_property(props, kIOSurfaceHeight, g_io_requested_h);
    dn_io_property(props, kIOSurfaceBytesPerElement, 4);
    /* Apple/CoreVideo BGRA fourCC: 'BGRA' (not GL_RGBA byte order). */
    dn_io_property(props, kIOSurfacePixelFormat, 0x42475241);
    IOSurfaceRef pool[DN_BRIDGE_IO_SLOTS] = { 0 };
    bool valid = true;
    for (int i = 0; i < DN_BRIDGE_IO_SLOTS; i++) {
        pool[i] = IOSurfaceCreate(props);
        if (!pool[i] ||
            IOSurfaceGetWidth(pool[i]) != (size_t)g_io_requested_w ||
            IOSurfaceGetHeight(pool[i]) != (size_t)g_io_requested_h ||
            IOSurfaceGetPixelFormat(pool[i]) != 0x42475241u ||
            IOSurfaceGetBytesPerElement(pool[i]) != 4 ||
            IOSurfaceGetBytesPerRow(pool[i]) < (size_t)g_io_requested_w * 4u ||
            IOSurfaceGetBytesPerRow(pool[i]) % 4u != 0u) {
            valid = false;
            break;
        }
    }
    CFRelease(props);
    if (valid) {
        void *borrowed[DN_BRIDGE_IO_SLOTS];
        for (int i = 0; i < DN_BRIDGE_IO_SLOTS; i++) {
            borrowed[i] = (void *)pool[i];
        }
        valid = g_dnq_display_iosurface_register(borrowed,
                              g_io_requested_w, g_io_requested_h);
    }
    dn_breadcrumb(breadcrumb_path,
                  valid ? "12d bridge: 4 BGRA IOSurfaces registered for guarded EAGL; PBO remains fallback"
                        : "12d bridge: EAGL/IOSurface unavailable; safe PBO fallback");
    for (int i = 0; i < DN_BRIDGE_IO_SLOTS; i++) {
        if (pool[i]) CFRelease(pool[i]); /* QEMU retained each on success */
    }
#else
    (void)breadcrumb_path;
#endif
}

int dn_qemu_run_ex(const char *arguments_joined, const char *breadcrumb_path, bool load_snapshot)
{
    dn_breadcrumb(breadcrumb_path, "10 bridge: dn_qemu_run started");
    if (g_library_handle == NULL || arguments_joined == NULL) {
        dn_breadcrumb(breadcrumb_path, "99 bridge: library or argv missing");
        return -1;
    }

    char *copy = strdup(arguments_joined);
    if (copy == NULL) {
        return -1;
    }

    size_t capacity = 32;
    int argc = 0;
    char **argv = calloc(capacity, sizeof(char *));
    if (argv == NULL) {
        free(copy);
        return -1;
    }

    char *cursor = copy;
    for (;;) {
        if ((size_t)argc + 2 > capacity) {
            capacity *= 2;
            char **grown = realloc(argv, capacity * sizeof(char *));
            if (grown == NULL) {
                free(argv);
                free(copy);
                return -1;
            }
            argv = grown;
        }
        argv[argc++] = cursor;
        char *separator = strchr(cursor, '\x01');
        if (separator == NULL) {
            break;
        }
        *separator = '\0';
        cursor = separator + 1;
    }
    argv[argc] = NULL;

    /* Phase B (docs/12): EGL/ANGLE must exist BEFORE qemu_init realizes
     * virtio-gpu-gl. For THIS Lineage VM, the official UTM guide warns that
     * ANGLE (Metal) makes the Android UI disappear after boot animation and
     * prescribes ANGLE (OpenGL). In the pinned UTM host code, OpenGL is
     * selected by UNSETTING ANGLE_DEFAULT_PLATFORM (NOT setting it to the
     * made-up string "opengl"). On iOS this selects ANGLE's EAGL backend;
     * the 0.4.7 iPhone trial displayed Android UI, but 0.4.8 latency
     * improvements still need a device comparison. The GL trial runs on
     * APFS clones and cannot load/save g0 snapshots. */
    if (getenv("DN_GL") != NULL) {
        unsetenv("ANGLE_DEFAULT_PLATFORM");
        dn_breadcrumb(breadcrumb_path,
                      "11f bridge: isolated GL trial: ANGLE OpenGL/EAGL requested (ANGLE_DEFAULT_PLATFORM unset)");
        bool gl_ok = false;
        if (g_dnq_gl_early_init != NULL) {
            gl_ok = g_dnq_gl_early_init();
            dn_breadcrumb(breadcrumb_path, "11g bridge: GL early init %s",
                          gl_ok ? "OK" : "FAILED (check [dn-dpy] in qemu.log)");
        } else {
            dn_breadcrumb(breadcrumb_path, "11g bridge: DN_GL set, but dnq_gl_early_init is missing from the framework (old QEMU?)");
        }
        if (!gl_ok) {
            if (getenv("DN_GPU_TRIAL") != NULL) {
                /* An explicit GPU-only experiment must not be reported as
                 * accelerated after silently booting the software device.
                 * Refuse BEFORE qemu_init. The normal VM is unaffected. */
                dn_breadcrumb(breadcrumb_path,
                              "99 bridge: isolated GPU trial refused: ANGLE OpenGL/EAGL ES3 unavailable; software profile intact");
                free(argv);
                free(copy);
                return -1;
            }
            /* Legacy GL safety net (the frozen black screen of Log 16): a
             * virtio-gpu-gl-pci without an EGL display or dies in qemu_init
             * (error la realize: display_opengl == 0) sau agata firul QEMU
             * — the user is left with a mute black screen. If GL init
             * failed, degrade the device in argv to the software variant
             * BEFORE qemu_init: boot succeeds, just without GPU. */
            const char *gldev = "virtio-gpu-gl-pci";
            size_t gl_len = strlen(gldev);
            bool degraded = false;
            for (int i = 0; i < argc; i++) {
                if (strncmp(argv[i], gldev, gl_len) == 0) {
                    char *p = argv[i] + strlen("virtio-gpu");
                    memmove(p, p + 3, strlen(p + 3) + 1); /* delete "-gl" */
                    degraded = true;
                }
            }
            dn_breadcrumb(breadcrumb_path,
                          "11h bridge: GL picat -> argv degradat la virtio-gpu-pci (software)=%s",
                          degraded ? "yes" : "not-needed");
        }
    }
    /* Aceeasi secventa ca in system/main.c din fork-ul UTM, minus exit(). */
    dn_breadcrumb(breadcrumb_path, "11 bridge: before qemu_init (argc=%d)", argc);
    g_qemu_init(argc, argv);
    dn_breadcrumb(breadcrumb_path, "12 bridge: qemu_init s-a intors");
    /* Reinstall the crash trap after qemu_init too (in case QEMU/glib
     * init replaced some handler): from here on, a fatal signal
     * in QEMU/ANGLE/virglrenderer lasa o linie „99 crash-trap”. */
    dn_install_crash_trap(breadcrumb_path);
    /* The BQL is still held by this thread (as in UTM's main()): the right moment
     * to register the display listener and to restore
     * the snapshot, both before the main loop. */
    if (g_dnq_display_init != NULL) {
        g_dnq_display_init();
        dn_breadcrumb(breadcrumb_path, "12b bridge: display listener inregistrat (dn-display)");
    } else {
        dn_breadcrumb(breadcrumb_path, "12b bridge: display unavailable (framework without dn-display)");
    }
    dn_bridge_register_iosurfaces(breadcrumb_path); /* capability-checked; PBO otherwise */
    if (load_snapshot) {
        if (g_dnq_snapshot_load_at_startup != NULL) {
            bool restored = g_dnq_snapshot_load_at_startup();
            dn_breadcrumb(breadcrumb_path, "12c bridge: snapshot dn-ready %s",
                          restored ? "restaurat" : "absent, boot normal");
        } else {
            dn_breadcrumb(breadcrumb_path, "12c bridge: snapshot requested but unavailable in the framework");
        }
    }
    g_bql_unlock();
    g_replay_mutex_unlock();

    g_replay_mutex_lock();
    g_bql_lock(__FILE__, __LINE__);
    dn_breadcrumb(breadcrumb_path, "13 bridge: before qemu_main_loop");
    int status = g_qemu_main_loop();
    dn_breadcrumb(breadcrumb_path, "14 bridge: qemu_main_loop s-a intors (status=%d)", status);
    /* Still on QEMU's main loop with BQL held: stop the input BH BEFORE
     * qemu_cleanup tears down the console. No background worker or pending
     * UIKit touch may access guest state after this point. */
    if (g_dnq_display_shutdown != NULL) {
        g_dnq_display_shutdown();
    }
    g_qemu_cleanup(status);
    dn_breadcrumb(breadcrumb_path, "15 bridge: qemu_cleanup s-a intors");
    g_bql_unlock();
    g_replay_mutex_unlock();

    free(argv);
    free(copy);
    return status;
}

int dn_qemu_run(const char *arguments_joined, const char *breadcrumb_path)
{
    return dn_qemu_run_ex(arguments_joined, breadcrumb_path, false);
}

void dn_qemu_free_error(char *error_message)
{
    free(error_message);
}

/* ---- Display/touch/snapshot: wrappers over the optional symbols ---- */

bool dn_display_available(void)
{
    return g_dnq_display_init != NULL && g_dnq_display_lock_frame != NULL;
}

bool dn_display_lock_frame(DNDisplayFrame *out)
{
    if (g_dnq_display_lock_frame == NULL || out == NULL) {
        return false;
    }
    DNQFrameInfoMirror mirror;
    if (!g_dnq_display_lock_frame(&mirror)) {
        return false;
    }
    out->pixels = mirror.pixels;
    out->width = mirror.width;
    out->height = mirror.height;
    out->stride = mirror.stride;
    out->bpp = mirror.bpp;
    out->generation = mirror.generation;
    out->sequence = mirror.sequence;
    return true;
}

void dn_display_unlock_frame(void)
{
    if (g_dnq_display_unlock_frame != NULL) {
        g_dnq_display_unlock_frame();
    }
}

uint64_t dn_display_sequence(void)
{
    return g_dnq_display_sequence != NULL ? g_dnq_display_sequence() : 0;
}

bool dn_display_take_damage(DNDisplayDamage *out)
{
    if (g_dnq_display_take_damage == NULL || out == NULL) {
        return false;
    }
    DNQDisplayDamageMirror mirror;
    if (!g_dnq_display_take_damage(&mirror)) {
        return false;
    }
    out->x = mirror.x;
    out->y = mirror.y;
    out->width = mirror.width;
    out->height = mirror.height;
    out->generation = mirror.generation;
    out->sequence = mirror.sequence;
    return true;
}

void dn_display_send_pointer(int32_t x, int32_t y, bool button_down)
{
    /* New engines enqueue on QEMU's main AIO loop without BQL on UIKit.
     * A false return means VM startup/shutdown: NEVER call the synchronous
     * symbol then (QEMU's console can already be gone). Older frameworks
     * retain their original, working synchronous pointer path. */
    if (g_dnq_display_send_pointer_async != NULL) {
        (void)g_dnq_display_send_pointer_async(x, y, button_down);
    } else if (g_dnq_display_send_pointer != NULL) {
        g_dnq_display_send_pointer(x, y, button_down);
    }
}

void dn_display_send_text(const char *utf8)
{
    if (g_dnq_display_send_text != NULL && utf8 != NULL) {
        g_dnq_display_send_text(utf8);
    }
}

void *dn_display_metal_acquire(int32_t *w, int32_t *h, bool *y0top, uint64_t *gen)
{
    if (g_dnq_display_metal_acquire != NULL) {
        return g_dnq_display_metal_acquire(w, h, y0top, gen);
    }
    return NULL;
}

void dn_display_set_guest_dpi(int32_t dpi)
{
    if (g_dnq_display_set_guest_dpi != NULL) {
        g_dnq_display_set_guest_dpi(dpi);
    }
}

void dn_display_set_guest_refresh_rate(int32_t hz)
{
    if (g_dnq_display_set_guest_refresh_rate != NULL) {
        g_dnq_display_set_guest_refresh_rate(hz);
    }
}

void dn_display_metal_release(void *tex)
{
    if (g_dnq_display_metal_release != NULL && tex != NULL) {
        g_dnq_display_metal_release(tex);
    }
}

void *dn_display_iosurface_acquire(int32_t *w, int32_t *h,
                                   bool *flip_y, uint64_t *ticket)
{
    if (g_dnq_display_iosurface_acquire) {
        return g_dnq_display_iosurface_acquire(w, h, flip_y, ticket);
    }
    return NULL;
}

void dn_display_iosurface_release(void *surface)
{
    if (surface && g_dnq_display_iosurface_release) {
        g_dnq_display_iosurface_release(surface);
    }
}

void dn_display_iosurface_disable(void)
{
    if (g_dnq_display_iosurface_disable) {
        g_dnq_display_iosurface_disable();
    }
}

void dn_display_set_ui_size(int32_t width, int32_t height)
{
    if (g_dnq_display_set_ui_size != NULL) {
        g_dnq_display_set_ui_size(width, height);
    }
}

void dn_display_guest_size(int32_t *width, int32_t *height)
{
    if (g_dnq_display_guest_size != NULL) {
        g_dnq_display_guest_size(width, height);
    } else {
        if (width)  *width = 0;
        if (height) *height = 0;
    }
}

void dn_snapshot_save(void)
{
    if (g_dnq_snapshot_save != NULL) {
        g_dnq_snapshot_save();
    }
}

int dn_snapshot_state(void)
{
    return g_dnq_snapshot_state != NULL ? g_dnq_snapshot_state() : -1;
}
