/*
 * Guest-side acceptance fixture for iDroid's complete native path.
 *
 * This is deliberately not a "hello" library.  The same converted
 * ARM64 ELF image contains:
 *
 *   - raw Linux svc #0 and mrs tpidr_el0 sites for the converter's
 *     patch island;
 *   - ELF thread-local storage and a stack-protector probe;
 *   - constructors/finalizers and relocated imports;
 *   - DN bionic pthread, stdio, errno and dlopen paths;
 *   - libandroid assets/configuration/log/properties and local JNI
 *     object calls;
 *   - a real ANativeActivity_onCreate lifecycle, software window
 *     drawing and direct-touch input consumption; and
 *   - the dn_vk Vulkan fixture included below, so the identical
 *     image also drives DN Vulkan-on-Metal.
 *
 * The source declares imported functions manually because the Mac
 * build pipeline intentionally supports NDK, Homebrew LLVM and Zig
 * freestanding modes, not all of which provide Android headers.
 */

#include <stddef.h>
#include <stdint.h>

extern unsigned long strlen(const char *s);
extern int memcmp(const void *a, const void *b, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);
extern int dn_native_selftest_add(int a, int b);
extern int *__errno_location(void);

/* DN bionic pthread/stdio/dl surface.  Guest pthread_t is one word;
 * sync objects are opaque storage owned by the caller, exactly as
 * bionic exposes them to native libraries. */
typedef unsigned long dn_full_pthread_t;
extern int pthread_create(dn_full_pthread_t *thread, const void *attr,
                          void *(*start_routine)(void *), void *arg);
extern int pthread_join(dn_full_pthread_t thread, void **retval);
extern int pthread_mutex_init(void *mutex, const void *attr);
extern int pthread_mutex_destroy(void *mutex);
extern int pthread_mutex_lock(void *mutex);
extern int pthread_mutex_unlock(void *mutex);
extern int pthread_key_create(unsigned int *key, void (*destructor)(void *));
extern int pthread_key_delete(unsigned int key);
extern int pthread_setspecific(unsigned int key, const void *value);
extern void *pthread_getspecific(unsigned int key);

typedef struct dn_full_file dn_full_file_t;
extern dn_full_file_t *fopen(const char *path, const char *mode);
extern size_t fread(void *ptr, size_t size, size_t nmemb, dn_full_file_t *file);
extern size_t fwrite(const void *ptr, size_t size, size_t nmemb, dn_full_file_t *file);
extern int fclose(dn_full_file_t *file);

extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *symbol);
extern int dlclose(void *handle);

/* ------------------------------------------------------------------ */
/* Raw patched instructions                                            */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__) || defined(__arm64__)

static long dn_full_syscall6(long nr, long a0, long a1, long a2,
                             long a3, long a4, long a5)
{
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return x0;
}

static uintptr_t dn_full_thread_pointer(void)
{
    uintptr_t tp = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

#else

/* Lets host compilers syntax-check the fixture.  The shipped fixture
 * is always AArch64; these stubs are never the device acceptance. */
static long dn_full_syscall6(long nr, long a0, long a1, long a2,
                             long a3, long a4, long a5)
{
    (void)nr; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -1;
}

static uintptr_t dn_full_thread_pointer(void)
{
    return 0;
}

#endif

/* ------------------------------------------------------------------ */
/* Shared fixture state                                                */
/* ------------------------------------------------------------------ */

static volatile int dn_full_ctor_calls_value;
static volatile int dn_full_finalized_value;
static volatile int dn_full_last_step_value;
static volatile uint32_t dn_full_checksum_value;
static volatile int dn_full_shared_counter;
static volatile int dn_full_worker_result;

static __thread volatile int dn_full_tls_value = 41;
static __thread volatile int dn_full_tls_bss;

static unsigned char dn_full_mutex_storage[64]
    __attribute__((aligned(16)));
static unsigned int dn_full_thread_key;

static uint32_t dn_full_hash_bytes(const uint8_t *bytes, size_t count)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static int dn_full_fail(int step)
{
    dn_full_last_step_value = step;
    return -step;
}

__attribute__((constructor, used))
static void dn_full_init(void)
{
    dn_full_ctor_calls_value++;
    dn_full_checksum_value = dn_full_hash_bytes(
        (const uint8_t *)"DNFull42", 8);
}

__attribute__((destructor, used))
static void dn_full_fini(void)
{
    dn_full_finalized_value = 1;
}

static int dn_full_stack_probe(void)
{
    char bytes[32];
    volatile int base = 3;
    for (int i = 0; i < 32; ++i)
        bytes[i] = (char)(i + base);
    return bytes[0] + bytes[31];
}

static void *dn_full_worker(void *unused)
{
    (void)unused;
    dn_full_worker_result = -1;

    if (pthread_setspecific(dn_full_thread_key,
                            (void *)(uintptr_t)0x2a) != 0)
        return (void *)(uintptr_t)-2;
    if (pthread_getspecific(dn_full_thread_key) !=
            (void *)(uintptr_t)0x2a)
        return (void *)(uintptr_t)-3;

    /* A new DN-managed thread must see this module's ELF TLS template
     * and zero-filled BSS, not the main thread's later values. */
    if (dn_full_tls_value != 41 || dn_full_tls_bss != 0)
        return (void *)(uintptr_t)-4;
    dn_full_tls_value = 7;
    dn_full_tls_bss = 9;

    if (pthread_mutex_lock(dn_full_mutex_storage) != 0)
        return (void *)(uintptr_t)-5;
    dn_full_shared_counter += 5;
    if (pthread_mutex_unlock(dn_full_mutex_storage) != 0)
        return (void *)(uintptr_t)-6;

    dn_full_worker_result = 42;
    return (void *)(uintptr_t)42;
}

/* ------------------------------------------------------------------ */
/* Core guest gate                                                     */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int dn_full_run_test(void)
{
    dn_full_last_step_value = 0;

    /* 1: imported function calls and the stack protector. */
    if (dn_native_selftest_add(20, 22) != 42 ||
        strlen("dn-full") != 7 || dn_full_stack_probe() != 37)
        return dn_full_fail(1);
    dn_full_last_step_value = 1;

    /* 2: raw tpidr_el0, served by the patch island.  bionic keeps its
     * self pointer at TP+0 and the stack guard at TP+0x28; the guard's
     * low byte is zero by construction. */
    uintptr_t tp = dn_full_thread_pointer();
    if (!tp)
        return dn_full_fail(2);
    uintptr_t *slots = (uintptr_t *)(void *)tp;
    if (slots[0] != tp || slots[5] == 0 || (slots[5] & 0xffU) != 0)
        return dn_full_fail(2);
    dn_full_last_step_value = 2;

    /* 3: raw svc process/thread identity. */
    long pid = dn_full_syscall6(172, 0, 0, 0, 0, 0, 0);
    long tid = dn_full_syscall6(178, 0, 0, 0, 0, 0, 0);
    if (pid <= 0 || tid <= 0)
        return dn_full_fail(3);
    dn_full_last_step_value = 3;

    /* 4: raw svc Android VFS file I/O. */
    const char *raw_path =
        "/data/data/ro.droidnative.app/files/dn-full-raw.txt";
    long fd = dn_full_syscall6(56, -100, (long)(uintptr_t)raw_path,
                               00001102, 0644, 0, 0);
    if (fd < 0)
        return dn_full_fail(4);
    const char raw_payload[] = "DN42";
    long written = dn_full_syscall6(64, fd, (long)(uintptr_t)raw_payload,
                                    4, 0, 0, 0);
    long seek = dn_full_syscall6(62, fd, 0, 0, 0, 0, 0);
    char raw_readback[4];
    memset(raw_readback, 0, sizeof(raw_readback));
    long got = dn_full_syscall6(63, fd, (long)(uintptr_t)raw_readback,
                                4, 0, 0, 0);
    long close_rc = dn_full_syscall6(57, fd, 0, 0, 0, 0, 0);
    if (written != 4 || seek != 0 || got != 4 || close_rc != 0 ||
        memcmp(raw_readback, raw_payload, 4) != 0)
        return dn_full_fail(4);
    dn_full_last_step_value = 4;

    /* 5-6: ELF TLS isolation plus DN bionic pthread create/key/join. */
    if (pthread_mutex_init(dn_full_mutex_storage, NULL) != 0)
        return dn_full_fail(5);
    if (pthread_key_create(&dn_full_thread_key, NULL) != 0) {
        (void)pthread_mutex_destroy(dn_full_mutex_storage);
        return dn_full_fail(5);
    }

    dn_full_tls_value = 42;
    dn_full_tls_bss = 5;
    if (pthread_mutex_lock(dn_full_mutex_storage) != 0)
        return dn_full_fail(6);
    dn_full_shared_counter = 10;
    if (pthread_mutex_unlock(dn_full_mutex_storage) != 0)
        return dn_full_fail(6);

    dn_full_pthread_t worker = 0;
    void *worker_ret = NULL;
    int create_rc = pthread_create(&worker, NULL, dn_full_worker, NULL);
    int join_rc = create_rc == 0 ? pthread_join(worker, &worker_ret) : -1;
    int tls_ok = dn_full_tls_value == 42 && dn_full_tls_bss == 5;
    int key_delete_rc = pthread_key_delete(dn_full_thread_key);
    int mutex_destroy_rc = pthread_mutex_destroy(dn_full_mutex_storage);
    if (create_rc != 0 || join_rc != 0 || !tls_ok ||
        worker_ret != (void *)(uintptr_t)42 ||
        dn_full_worker_result != 42 || dn_full_shared_counter != 15 ||
        key_delete_rc != 0 || mutex_destroy_rc != 0)
        return dn_full_fail(6);
    dn_full_last_step_value = 6;

    /* 7: DN bionic stdio over the same Android VFS. */
    const char *stdio_path =
        "/data/data/ro.droidnative.app/files/dn-full-stdio.txt";
    dn_full_file_t *file = fopen(stdio_path, "wb+");
    if (!file)
        return dn_full_fail(7);
    const char stdio_payload[] = "FULL42";
    size_t stdio_written = fwrite(stdio_payload, 1, 6, file);
    int stdio_close_rc = fclose(file);
    file = fopen(stdio_path, "rb");
    if (!file || stdio_written != 6 || stdio_close_rc != 0)
        return dn_full_fail(7);
    char stdio_readback[6];
    memset(stdio_readback, 0, sizeof(stdio_readback));
    size_t stdio_got = fread(stdio_readback, 1, 6, file);
    stdio_close_rc = fclose(file);
    if (stdio_got != 6 || stdio_close_rc != 0 ||
        memcmp(stdio_readback, stdio_payload, 6) != 0)
        return dn_full_fail(7);
    dn_full_last_step_value = 7;

    /* 8: bionic errno storage is DN TLS, not Darwin's errno cell. */
    int *errno_cell = __errno_location();
    if (!errno_cell)
        return dn_full_fail(8);
    *errno_cell = 42;
    if (*__errno_location() != 42)
        return dn_full_fail(8);
    dn_full_last_step_value = 8;

    /* 9: DN dlopen/dlsym system-library surface. */
    void *libc = dlopen("libc.so", 2 /* RTLD_NOW */);
    if (!libc)
        return dn_full_fail(9);
    typedef unsigned long (*dn_full_strlen_fn)(const char *);
    dn_full_strlen_fn dynamic_strlen =
        (dn_full_strlen_fn)dlsym(libc, "strlen");
    int dynamic_ok = dynamic_strlen && dynamic_strlen("native") == 6;
    int dlclose_rc = dlclose(libc);
    if (!dynamic_ok || dlclose_rc != 0)
        return dn_full_fail(9);

    dn_full_last_step_value = 9;
    return 0;
}

/* ------------------------------------------------------------------ */
/* libandroid / NativeActivity guest gate                              */
/* ------------------------------------------------------------------ */

typedef struct dn_full_rect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} dn_full_rect_t;

typedef struct dn_full_activity dn_full_activity_t;

typedef struct dn_full_activity_callbacks {
    void (*onStart)(dn_full_activity_t *);
    void (*onResume)(dn_full_activity_t *);
    void *(*onSaveInstanceState)(dn_full_activity_t *, size_t *);
    void (*onPause)(dn_full_activity_t *);
    void (*onStop)(dn_full_activity_t *);
    void (*onDestroy)(dn_full_activity_t *);
    void (*onWindowFocusChanged)(dn_full_activity_t *, int);
    void (*onNativeWindowCreated)(dn_full_activity_t *, void *);
    void (*onNativeWindowResized)(dn_full_activity_t *, void *);
    void (*onNativeWindowRedrawNeeded)(dn_full_activity_t *, void *);
    void (*onNativeWindowDestroyed)(dn_full_activity_t *, void *);
    void (*onInputQueueCreated)(dn_full_activity_t *, void *);
    void (*onInputQueueDestroyed)(dn_full_activity_t *, void *);
    void (*onContentRectChanged)(dn_full_activity_t *, const dn_full_rect_t *);
    void (*onConfigurationChanged)(dn_full_activity_t *);
    void (*onLowMemory)(dn_full_activity_t *);
} dn_full_activity_callbacks_t;

struct dn_full_activity {
    dn_full_activity_callbacks_t *callbacks;
    void *vm;
    void *env;
    void *clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;
    void *assetManager;
    const char *obbPath;
};

typedef struct dn_full_window_buffer {
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
    void *bits;
    uint32_t reserved[6];
} dn_full_window_buffer_t;

extern void *AAssetManager_open(void *manager, const char *filename, int mode);
extern int AAsset_read(void *asset, void *buffer, size_t count);
extern long AAsset_getLength(void *asset);
extern void AAsset_close(void *asset);

extern void *AConfiguration_new(void);
extern void AConfiguration_delete(void *config);
extern void AConfiguration_fromAssetManager(void *config, void *manager);
extern int32_t AConfiguration_getSdkVersion(void *config);
extern int32_t AConfiguration_getDensity(void *config);
extern int32_t AConfiguration_getOrientation(void *config);

extern int32_t ANativeWindow_lock(void *window,
                                  dn_full_window_buffer_t *buffer,
                                  dn_full_rect_t *dirty_bounds);
extern int32_t ANativeWindow_unlockAndPost(void *window);

extern void *ALooper_prepare(int options);
extern int ALooper_pollAll(int timeout_ms, int *out_fd,
                           int *out_events, void **out_data);
extern void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                                     void *callback, void *data);
extern void AInputQueue_detachLooper(void *queue);
extern int32_t AInputQueue_getEvent(void *queue, void **out_event);
extern void AInputQueue_finishEvent(void *queue, void *event, int handled);
extern int32_t AInputEvent_getType(const void *event);
extern int32_t AMotionEvent_getAction(const void *event);
extern size_t AMotionEvent_getPointerCount(const void *event);
extern int32_t AMotionEvent_getPointerId(const void *event, size_t index);
extern float AMotionEvent_getX(const void *event, size_t index);
extern float AMotionEvent_getY(const void *event, size_t index);

extern int __android_log_write(int priority, const char *tag,
                               const char *text);
extern int __system_property_get(const char *name, char *value);

enum {
    DN_FULL_ACTIVITY_CREATED = 1 << 0,
    DN_FULL_ACTIVITY_STARTED = 1 << 1,
    DN_FULL_ACTIVITY_RESUMED = 1 << 2,
    DN_FULL_ACTIVITY_WINDOW_CREATED = 1 << 3,
    DN_FULL_ACTIVITY_INPUT_CREATED = 1 << 4,
    DN_FULL_ACTIVITY_FOCUSED = 1 << 5,
    DN_FULL_ACTIVITY_RESIZED = 1 << 6,
    DN_FULL_ACTIVITY_PAUSED = 1 << 7,
    DN_FULL_ACTIVITY_STOPPED = 1 << 8,
    DN_FULL_ACTIVITY_WINDOW_DESTROYED = 1 << 9,
    DN_FULL_ACTIVITY_INPUT_DESTROYED = 1 << 10,
    DN_FULL_ACTIVITY_DESTROYED = 1 << 11
};

static dn_full_activity_t *dn_full_activity;
static void *dn_full_window;
static void *dn_full_input_queue;
static void *dn_full_looper;
static volatile int dn_full_activity_state_value;
static volatile uint32_t dn_full_activity_checksum_value;

static void dn_full_mark_activity(int bit)
{
    dn_full_activity_state_value |= bit;
}

static void dn_full_draw_window(void *window)
{
    if (!window)
        return;
    dn_full_window_buffer_t buffer;
    memset(&buffer, 0, sizeof(buffer));
    if (ANativeWindow_lock(window, &buffer, NULL) != 0 || !buffer.bits)
        return;

    const uint32_t pixel = 0xff804020U;
    uint32_t *pixels = (uint32_t *)buffer.bits;
    for (int32_t y = 0; y < buffer.height; ++y) {
        for (int32_t x = 0; x < buffer.width; ++x)
            pixels[y * buffer.stride + x] = pixel;
    }
    const uint8_t *bytes = (const uint8_t *)buffer.bits;
    dn_full_activity_checksum_value = dn_full_hash_bytes(
        bytes, (size_t)buffer.width * (size_t)buffer.height * 4U);
    (void)ANativeWindow_unlockAndPost(window);
}

static void dn_full_on_start(dn_full_activity_t *activity)
{
    (void)activity;
    dn_full_mark_activity(DN_FULL_ACTIVITY_STARTED);
}

static void dn_full_on_resume(dn_full_activity_t *activity)
{
    (void)activity;
    dn_full_mark_activity(DN_FULL_ACTIVITY_RESUMED);
}

static void *dn_full_on_save(dn_full_activity_t *activity, size_t *out_size)
{
    (void)activity;
    if (out_size)
        *out_size = 0;
    return NULL;
}

static void dn_full_on_pause(dn_full_activity_t *activity)
{
    (void)activity;
    dn_full_mark_activity(DN_FULL_ACTIVITY_PAUSED);
}

static void dn_full_on_stop(dn_full_activity_t *activity)
{
    (void)activity;
    dn_full_mark_activity(DN_FULL_ACTIVITY_STOPPED);
}

static void dn_full_on_destroy(dn_full_activity_t *activity)
{
    (void)activity;
    dn_full_mark_activity(DN_FULL_ACTIVITY_DESTROYED);
}

static void dn_full_on_focus(dn_full_activity_t *activity, int has_focus)
{
    (void)activity;
    if (has_focus)
        dn_full_mark_activity(DN_FULL_ACTIVITY_FOCUSED);
}

static void dn_full_on_window_created(dn_full_activity_t *activity,
                                      void *window)
{
    (void)activity;
    dn_full_window = window;
    dn_full_mark_activity(DN_FULL_ACTIVITY_WINDOW_CREATED);
    dn_full_draw_window(window);
}

static void dn_full_on_window_resized(dn_full_activity_t *activity,
                                      void *window)
{
    (void)activity;
    dn_full_window = window;
    dn_full_mark_activity(DN_FULL_ACTIVITY_RESIZED);
    dn_full_draw_window(window);
}

static void dn_full_on_window_redraw(dn_full_activity_t *activity,
                                     void *window)
{
    (void)activity;
    dn_full_draw_window(window);
}

static void dn_full_on_window_destroyed(dn_full_activity_t *activity,
                                        void *window)
{
    (void)activity;
    (void)window;
    dn_full_window = NULL;
    dn_full_mark_activity(DN_FULL_ACTIVITY_WINDOW_DESTROYED);
}

static void dn_full_on_input_created(dn_full_activity_t *activity,
                                     void *queue)
{
    (void)activity;
    dn_full_input_queue = queue;
    dn_full_looper = ALooper_prepare(0);
    if (dn_full_looper)
        AInputQueue_attachLooper(queue, dn_full_looper, 3, NULL, NULL);
    dn_full_mark_activity(DN_FULL_ACTIVITY_INPUT_CREATED);
}

static void dn_full_on_input_destroyed(dn_full_activity_t *activity,
                                       void *queue)
{
    (void)activity;
    if (queue)
        AInputQueue_detachLooper(queue);
    dn_full_input_queue = NULL;
    dn_full_looper = NULL;
    dn_full_mark_activity(DN_FULL_ACTIVITY_INPUT_DESTROYED);
}

__attribute__((visibility("default")))
void ANativeActivity_onCreate(dn_full_activity_t *activity,
                              void *saved_state, size_t saved_size)
{
    (void)saved_state;
    (void)saved_size;
    dn_full_activity = activity;
    dn_full_activity_state_value = DN_FULL_ACTIVITY_CREATED;
    if (!activity || !activity->callbacks)
        return;
    activity->instance = activity;
    activity->callbacks->onStart = dn_full_on_start;
    activity->callbacks->onResume = dn_full_on_resume;
    activity->callbacks->onSaveInstanceState = dn_full_on_save;
    activity->callbacks->onPause = dn_full_on_pause;
    activity->callbacks->onStop = dn_full_on_stop;
    activity->callbacks->onDestroy = dn_full_on_destroy;
    activity->callbacks->onWindowFocusChanged = dn_full_on_focus;
    activity->callbacks->onNativeWindowCreated = dn_full_on_window_created;
    activity->callbacks->onNativeWindowResized = dn_full_on_window_resized;
    activity->callbacks->onNativeWindowRedrawNeeded = dn_full_on_window_redraw;
    activity->callbacks->onNativeWindowDestroyed = dn_full_on_window_destroyed;
    activity->callbacks->onInputQueueCreated = dn_full_on_input_created;
    activity->callbacks->onInputQueueDestroyed = dn_full_on_input_destroyed;
}

__attribute__((visibility("default")))
int dn_full_run_activity_test(void)
{
    if (!dn_full_activity || !dn_full_activity->assetManager)
        return dn_full_fail(10);

    /* 10: assets installed by Swift under DN's asset root. */
    void *asset = AAssetManager_open(dn_full_activity->assetManager,
                                     "dn-full-asset.txt", 3);
    if (!asset)
        return dn_full_fail(10);
    char asset_bytes[11];
    memset(asset_bytes, 0, sizeof(asset_bytes));
    long asset_length = AAsset_getLength(asset);
    int asset_read = AAsset_read(asset, asset_bytes, sizeof(asset_bytes));
    AAsset_close(asset);
    if (asset_length != 11 || asset_read != 11 ||
        memcmp(asset_bytes, "DN-ASSET-42", 11) != 0)
        return dn_full_fail(10);
    dn_full_last_step_value = 10;

    /* 11: Android configuration for the iPhone guest profile. */
    void *config = AConfiguration_new();
    if (!config)
        return dn_full_fail(11);
    AConfiguration_fromAssetManager(config, dn_full_activity->assetManager);
    int config_ok = AConfiguration_getSdkVersion(config) == 35 &&
                    AConfiguration_getDensity(config) == 560 &&
                    AConfiguration_getOrientation(config) == 1;
    AConfiguration_delete(config);
    if (!config_ok || dn_full_activity->sdkVersion != 35)
        return dn_full_fail(11);
    dn_full_last_step_value = 11;

    /* 12: local JNI object bridge (not a Java VM), Android log and
     * system properties.  Slots are the standard JNI table slots. */
    if (!dn_full_activity->vm)
        return dn_full_fail(12);
    void **vm_table = *(void ***)dn_full_activity->vm;
    typedef int32_t (*dn_full_attach_fn)(void *, void **, void *);
    dn_full_attach_fn attach = (dn_full_attach_fn)vm_table[4];
    void *env = NULL;
    if (!attach || attach(dn_full_activity->vm, &env, NULL) != 0 || !env)
        return dn_full_fail(12);

    void **env_table = *(void ***)env;
    typedef int32_t (*dn_full_get_version_fn)(void *);
    typedef void *(*dn_full_find_class_fn)(void *, const char *);
    typedef void *(*dn_full_new_string_fn)(void *, const char *);
    typedef int32_t (*dn_full_string_length_fn)(void *, void *);
    typedef const char *(*dn_full_get_utf_fn)(void *, void *, uint8_t *);
    dn_full_get_version_fn get_version =
        (dn_full_get_version_fn)env_table[4];
    dn_full_find_class_fn find_class = (dn_full_find_class_fn)env_table[6];
    dn_full_new_string_fn new_string = (dn_full_new_string_fn)env_table[167];
    dn_full_string_length_fn string_length =
        (dn_full_string_length_fn)env_table[168];
    dn_full_get_utf_fn get_utf = (dn_full_get_utf_fn)env_table[169];
    void *clazz = find_class ? find_class(env, "android/app/NativeActivity") : NULL;
    void *string = new_string ? new_string(env, "jni42") : NULL;
    const char *utf = string && get_utf ? get_utf(env, string, NULL) : NULL;
    if (!get_version || get_version(env) < 0x00010006 || !clazz || !utf ||
        string_length(env, string) != 5 || memcmp(utf, "jni42", 6) != 0)
        return dn_full_fail(12);

    /* __system_property_get requires a PROP_VALUE_MAX (92-byte)
     * buffer.  A shorter buffer is a guest-side buffer overflow, not
     * a shortened-property API. */
    char sdk[92];
    memset(sdk, 0, sizeof(sdk));
    if (__system_property_get("ro.build.version.sdk", sdk) != 2 ||
        memcmp(sdk, "35", 3) != 0 ||
        __android_log_write(4, "dnfull", "complete native fixture") < 0)
        return dn_full_fail(12);

    dn_full_last_step_value = 12;
    return 0;
}

__attribute__((visibility("default")))
int dn_full_pump_input(void)
{
    if (!dn_full_input_queue)
        return -1;
    int out_fd = -1;
    int out_events = 0;
    void *out_data = NULL;
    (void)ALooper_pollAll(0, &out_fd, &out_events, &out_data);

    void *event = NULL;
    if (AInputQueue_getEvent(dn_full_input_queue, &event) != 0 || !event)
        return -2;
    float x = AMotionEvent_getX(event, 0);
    float y = AMotionEvent_getY(event, 0);
    int ok = AInputEvent_getType(event) == 2 &&
             AMotionEvent_getAction(event) == 0 &&
             AMotionEvent_getPointerCount(event) == 1 &&
             AMotionEvent_getPointerId(event, 0) == 7 &&
             x > 17.0f && x < 18.0f && y > 29.0f && y < 30.0f;
    AInputQueue_finishEvent(dn_full_input_queue, event, ok ? 1 : 0);
    return ok ? 42 : -3;
}

__attribute__((visibility("default")))
int dn_full_last_step(void)
{
    return dn_full_last_step_value;
}

__attribute__((visibility("default")))
uint32_t dn_full_checksum(void)
{
    return dn_full_checksum_value;
}

__attribute__((visibility("default")))
int dn_full_ctor_calls(void)
{
    return dn_full_ctor_calls_value;
}

__attribute__((visibility("default")))
int dn_full_finalized(void)
{
    return dn_full_finalized_value;
}

__attribute__((visibility("default")))
int dn_full_activity_state(void)
{
    return dn_full_activity_state_value;
}

__attribute__((visibility("default")))
uint32_t dn_full_activity_checksum(void)
{
    return dn_full_activity_checksum_value;
}

/* The complete 0.3 guest Vulkan sequence is compiled into this same
 * ELF image.  Swift calls dn_vk_run_test after the patch/TLS/bionic
 * core above, proving one converted library can reach DN's GPU driver
 * as well as the CPU/runtime layers. */
#include "../vulkan/dn_vk.c"
