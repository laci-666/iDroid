#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "DNNativeAndroid.h"
#include "DNNativePatch.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define DN_ANDROID_MAGIC 0x444e414eu /* DNAN */
#define DN_PATH_CAP 4096

static int64_t dn_android_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static uint32_t dn_android_checksum(const uint8_t *bytes, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

/* ------------------------------------------------------------------ */
/* Assets                                                              */
/* ------------------------------------------------------------------ */

struct AAssetManager {
    uint32_t magic;
    char *root;
};

struct AAsset {
    uint32_t magic;
    int fd;
    int64_t length;
    int64_t position;
    uint8_t *buffer;
    bool buffer_loaded;
};

struct AAssetDir {
    uint32_t magic;
    DIR *host;
    char current[256];
};

static pthread_mutex_t dn_android_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *dn_asset_directory;
static char *dn_package_name;
static AAssetManager *dn_asset_manager;
static dn_native_window_frame_fn dn_window_frame_callback;
static void *dn_window_frame_user;

void dn_native_set_asset_directory(const char *path)
{
    pthread_mutex_lock(&dn_android_state_mutex);
    free(dn_asset_directory);
    dn_asset_directory = path ? strdup(path) : NULL;
    if (dn_asset_manager) {
        free(dn_asset_manager->root);
        dn_asset_manager->root = dn_asset_directory ? strdup(dn_asset_directory) : NULL;
    }
    pthread_mutex_unlock(&dn_android_state_mutex);
}

void dn_native_set_package_name(const char *package_name)
{
    pthread_mutex_lock(&dn_android_state_mutex);
    free(dn_package_name);
    dn_package_name = package_name ? strdup(package_name) : NULL;
    pthread_mutex_unlock(&dn_android_state_mutex);
}

void dn_native_set_window_frame_callback(dn_native_window_frame_fn callback, void *user)
{
    pthread_mutex_lock(&dn_android_state_mutex);
    dn_window_frame_callback = callback;
    dn_window_frame_user = user;
    pthread_mutex_unlock(&dn_android_state_mutex);
}

static bool dn_android_copy_asset_root(char *out, size_t out_len)
{
    pthread_mutex_lock(&dn_android_state_mutex);
    const char *root = dn_asset_directory;
    int written;
    if (root) {
        written = snprintf(out, out_len, "%s", root);
    } else {
        written = snprintf(out, out_len, "%s/assets", dn_native_files_root());
    }
    pthread_mutex_unlock(&dn_android_state_mutex);
    return written >= 0 && (size_t)written < out_len;
}

static AAssetManager *dn_asset_manager_singleton(void)
{
    pthread_mutex_lock(&dn_android_state_mutex);
    if (!dn_asset_manager) {
        dn_asset_manager = (AAssetManager *)calloc(1, sizeof(*dn_asset_manager));
        if (dn_asset_manager) {
            dn_asset_manager->magic = DN_ANDROID_MAGIC;
            dn_asset_manager->root = dn_asset_directory ? strdup(dn_asset_directory) : NULL;
        }
    }
    AAssetManager *result = dn_asset_manager;
    pthread_mutex_unlock(&dn_android_state_mutex);
    return result;
}

static AAssetManager *dn_AAssetManager_fromJava(void *env, void *asset_manager)
{
    (void)env;
    (void)asset_manager;
    return dn_asset_manager_singleton();
}

static bool dn_asset_path(AAssetManager *manager, const char *filename,
                          char *out, size_t out_len)
{
    if (!filename)
        return false;
    char root[DN_PATH_CAP];
    if (manager && manager->root) {
        if (strlen(manager->root) >= sizeof(root))
            return false;
        strcpy(root, manager->root);
    } else if (!dn_android_copy_asset_root(root, sizeof(root))) {
        return false;
    }
    if (filename[0] == '/') {
        char guest[DN_PATH_CAP];
        int written = snprintf(guest, sizeof(guest), "/android_asset/%s", filename + 1);
        if (written < 0 || (size_t)written >= sizeof(guest))
            return false;
        return dn_native_translate_path(guest, out, out_len) == 0;
    }
    /* Reuse the VFS normalizer by asking it to translate a synthetic
     * /android_asset path, then swap its root if an explicit asset
     * root is installed.  The common container layout is already
     * <files-root>/assets, so this normally needs no swap. */
    char guest[DN_PATH_CAP];
    int written = snprintf(guest, sizeof(guest), "/android_asset/%s", filename);
    if (written < 0 || (size_t)written >= sizeof(guest))
        return false;
    char translated[DN_PATH_CAP];
    if (dn_native_translate_path(guest, translated, sizeof(translated)) != 0)
        return false;
    const char *marker = strstr(translated, "/assets/");
    if (manager && manager->root && marker) {
        written = snprintf(out, out_len, "%s%s", root, marker + strlen("/assets"));
        return written >= 0 && (size_t)written < out_len;
    }
    if (manager && manager->root && strcmp(translated + strlen(translated) - 7, "/assets") == 0) {
        written = snprintf(out, out_len, "%s", root);
        return written >= 0 && (size_t)written < out_len;
    }
    if (strlen(translated) >= out_len)
        return false;
    strcpy(out, translated);
    return true;
}

static AAsset *dn_AAssetManager_open(AAssetManager *manager, const char *filename, int mode)
{
    (void)mode;
    char path[DN_PATH_CAP];
    if (!dn_asset_path(manager, filename, path, sizeof(path)))
        return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    AAsset *asset = (AAsset *)calloc(1, sizeof(*asset));
    if (!asset) {
        close(fd);
        return NULL;
    }
    asset->magic = DN_ANDROID_MAGIC;
    asset->fd = fd;
    asset->length = (int64_t)st.st_size;
    return asset;
}

static AAssetDir *dn_AAssetManager_openDir(AAssetManager *manager, const char *dir_name)
{
    char path[DN_PATH_CAP];
    if (!dn_asset_path(manager, dir_name ? dir_name : "", path, sizeof(path)))
        return NULL;
    DIR *host = opendir(path);
    if (!host)
        return NULL;
    AAssetDir *dir = (AAssetDir *)calloc(1, sizeof(*dir));
    if (!dir) {
        closedir(host);
        return NULL;
    }
    dir->magic = DN_ANDROID_MAGIC;
    dir->host = host;
    return dir;
}

static const char *dn_AAssetDir_getNextFileName(AAssetDir *dir)
{
    if (!dir || dir->magic != DN_ANDROID_MAGIC || !dir->host)
        return NULL;
    for (;;) {
        struct dirent *entry = readdir(dir->host);
        if (!entry)
            return NULL;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        strncpy(dir->current, entry->d_name, sizeof(dir->current) - 1);
        dir->current[sizeof(dir->current) - 1] = '\0';
        return dir->current;
    }
}

static void dn_AAssetDir_rewind(AAssetDir *dir)
{
    if (dir && dir->host)
        rewinddir(dir->host);
}

static void dn_AAssetDir_close(AAssetDir *dir)
{
    if (!dir)
        return;
    if (dir->host)
        closedir(dir->host);
    free(dir);
}

static int dn_AAsset_read(AAsset *asset, void *buffer, size_t count)
{
    if (!asset || asset->magic != DN_ANDROID_MAGIC || !buffer)
        return -1;
    ssize_t got = pread(asset->fd, buffer, count, (off_t)asset->position);
    if (got > 0)
        asset->position += got;
    return got < 0 ? -1 : (int)got;
}

static int64_t dn_AAsset_seek64(AAsset *asset, int64_t offset, int whence)
{
    if (!asset || asset->magic != DN_ANDROID_MAGIC)
        return -1;
    int64_t next;
    if (whence == SEEK_SET)
        next = offset;
    else if (whence == SEEK_CUR)
        next = asset->position + offset;
    else if (whence == SEEK_END)
        next = asset->length + offset;
    else
        return -1;
    if (next < 0)
        return -1;
    asset->position = next;
    return next;
}

static long dn_AAsset_seek(AAsset *asset, long offset, int whence)
{
    return (long)dn_AAsset_seek64(asset, offset, whence);
}

static void dn_AAsset_close(AAsset *asset)
{
    if (!asset)
        return;
    if (asset->fd >= 0)
        close(asset->fd);
    free(asset->buffer);
    free(asset);
}

static const void *dn_AAsset_getBuffer(AAsset *asset)
{
    if (!asset || asset->magic != DN_ANDROID_MAGIC)
        return NULL;
    if (asset->buffer_loaded)
        return asset->buffer;
    if (asset->length < 0 || asset->length > 256 * 1024 * 1024)
        return NULL;
    asset->buffer = (uint8_t *)malloc(asset->length ? (size_t)asset->length : 1);
    if (!asset->buffer)
        return NULL;
    int64_t old = asset->position;
    ssize_t got = pread(asset->fd, asset->buffer, (size_t)asset->length, 0);
    asset->position = old;
    if (got != asset->length) {
        free(asset->buffer);
        asset->buffer = NULL;
        return NULL;
    }
    asset->buffer_loaded = true;
    return asset->buffer;
}

static int64_t dn_AAsset_getLength64(AAsset *asset)
{
    return asset && asset->magic == DN_ANDROID_MAGIC ? asset->length : -1;
}

static long dn_AAsset_getLength(AAsset *asset)
{
    return (long)dn_AAsset_getLength64(asset);
}

static int64_t dn_AAsset_getRemainingLength64(AAsset *asset)
{
    if (!asset || asset->magic != DN_ANDROID_MAGIC)
        return -1;
    return asset->length > asset->position ? asset->length - asset->position : 0;
}

static long dn_AAsset_getRemainingLength(AAsset *asset)
{
    return (long)dn_AAsset_getRemainingLength64(asset);
}

static int dn_AAsset_openFileDescriptor64(AAsset *asset, int64_t *start, int64_t *length)
{
    if (!asset || asset->magic != DN_ANDROID_MAGIC)
        return -1;
    int dupfd = dup(asset->fd);
    if (dupfd < 0)
        return -1;
    if (start) *start = 0;
    if (length) *length = asset->length;
    return dupfd;
}

static int dn_AAsset_openFileDescriptor(AAsset *asset, long *start, long *length)
{
    int64_t start64 = 0, length64 = 0;
    int fd = dn_AAsset_openFileDescriptor64(asset, &start64, &length64);
    if (fd >= 0) {
        if (start) *start = (long)start64;
        if (length) *length = (long)length64;
    }
    return fd;
}

static int dn_AAsset_isAllocated(AAsset *asset)
{
    return asset && asset->magic == DN_ANDROID_MAGIC && asset->buffer_loaded ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

struct AConfiguration {
    uint32_t magic;
    char country[3];
    char language[3];
    int32_t orientation;
    int32_t touchscreen;
    int32_t density;
    int32_t keyboard;
    int32_t navigation;
    int32_t keys_hidden;
    int32_t nav_hidden;
    int32_t sdk_version;
    int32_t screen_size;
    int32_t screen_long;
    int32_t ui_mode;
    int32_t smallest_width_dp;
    int32_t width_dp;
    int32_t height_dp;
    int32_t color_mode;
};

static AConfiguration *dn_AConfiguration_new(void)
{
    AConfiguration *config = (AConfiguration *)calloc(1, sizeof(*config));
    if (!config)
        return NULL;
    config->magic = DN_ANDROID_MAGIC;
    strcpy(config->country, "RO");
    strcpy(config->language, "en");
    config->orientation = 1;      /* portrait */
    config->touchscreen = 3;      /* finger */
    config->density = 560;
    config->keyboard = 1;         /* nokeys */
    config->navigation = 1;       /* nonav */
    config->sdk_version = 35;
    config->screen_size = 3;      /* normal */
    config->screen_long = 2;      /* long */
    config->ui_mode = 0x11;       /* normal/day */
    config->smallest_width_dp = 411;
    config->width_dp = 411;
    config->height_dp = 891;
    return config;
}

static void dn_AConfiguration_delete(AConfiguration *config)
{
    free(config);
}

static void dn_AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *manager)
{
    (void)manager;
    if (!out)
        return;
    AConfiguration *fresh = dn_AConfiguration_new();
    if (!fresh)
        return;
    *out = *fresh;
    free(fresh);
}

static void dn_AConfiguration_copy(AConfiguration *dest, AConfiguration *source)
{
    if (dest && source)
        *dest = *source;
}

static int32_t dn_AConfiguration_diff(AConfiguration *a, AConfiguration *b)
{
    if (!a || !b)
        return 0;
    return memcmp(a, b, sizeof(*a)) == 0 ? 0 : 0xffffffff;
}

static int32_t dn_AConfiguration_match(AConfiguration *config, AConfiguration *requested)
{
    (void)config; (void)requested;
    return 0;
}

static int32_t dn_AConfiguration_isBetterThan(AConfiguration *config,
                                              AConfiguration *test,
                                              AConfiguration *requested)
{
    (void)config; (void)test; (void)requested;
    return 0;
}

static void dn_AConfiguration_getCountry(AConfiguration *config, char *out_country)
{
    if (!out_country) return;
    out_country[0] = config ? config->country[0] : '\0';
    out_country[1] = config ? config->country[1] : '\0';
}

static void dn_AConfiguration_setCountry(AConfiguration *config, const char *country)
{
    if (!config) return;
    config->country[0] = country ? country[0] : '\0';
    config->country[1] = country ? country[1] : '\0';
    config->country[2] = '\0';
}

static void dn_AConfiguration_getLanguage(AConfiguration *config, char *out_language)
{
    if (!out_language) return;
    out_language[0] = config ? config->language[0] : '\0';
    out_language[1] = config ? config->language[1] : '\0';
}

static void dn_AConfiguration_setLanguage(AConfiguration *config, const char *language)
{
    if (!config) return;
    config->language[0] = language ? language[0] : '\0';
    config->language[1] = language ? language[1] : '\0';
    config->language[2] = '\0';
}

#define DN_CONFIG_GETTER(field, name) \
    static int32_t dn_AConfiguration_get##name(AConfiguration *config) { return config ? config->field : 0; }
#define DN_CONFIG_SETTER(field, name) \
    static void dn_AConfiguration_set##name(AConfiguration *config, int32_t value) { if (config) config->field = value; }

DN_CONFIG_GETTER(orientation, Orientation)
DN_CONFIG_SETTER(orientation, Orientation)
DN_CONFIG_GETTER(touchscreen, Touchscreen)
DN_CONFIG_SETTER(touchscreen, Touchscreen)
DN_CONFIG_GETTER(density, Density)
DN_CONFIG_SETTER(density, Density)
DN_CONFIG_GETTER(keyboard, Keyboard)
DN_CONFIG_SETTER(keyboard, Keyboard)
DN_CONFIG_GETTER(navigation, Navigation)
DN_CONFIG_SETTER(navigation, Navigation)
DN_CONFIG_GETTER(keys_hidden, KeysHidden)
DN_CONFIG_SETTER(keys_hidden, KeysHidden)
DN_CONFIG_GETTER(nav_hidden, NavHidden)
DN_CONFIG_SETTER(nav_hidden, NavHidden)
DN_CONFIG_GETTER(sdk_version, SdkVersion)
DN_CONFIG_SETTER(sdk_version, SdkVersion)
DN_CONFIG_GETTER(screen_size, ScreenSize)
DN_CONFIG_SETTER(screen_size, ScreenSize)
DN_CONFIG_GETTER(screen_long, ScreenLong)
DN_CONFIG_SETTER(screen_long, ScreenLong)
DN_CONFIG_GETTER(ui_mode, UiMode)
DN_CONFIG_SETTER(ui_mode, UiMode)
DN_CONFIG_GETTER(smallest_width_dp, SmallestScreenWidthDp)
DN_CONFIG_SETTER(smallest_width_dp, SmallestScreenWidthDp)
DN_CONFIG_GETTER(width_dp, ScreenWidthDp)
DN_CONFIG_SETTER(width_dp, ScreenWidthDp)
DN_CONFIG_GETTER(height_dp, ScreenHeightDp)
DN_CONFIG_SETTER(height_dp, ScreenHeightDp)
DN_CONFIG_GETTER(color_mode, ColorMode)
DN_CONFIG_SETTER(color_mode, ColorMode)

static int32_t dn_AConfiguration_getMcc(AConfiguration *config) { (void)config; return 226; }
static void dn_AConfiguration_setMcc(AConfiguration *config, int32_t value) { (void)config; (void)value; }
static int32_t dn_AConfiguration_getMnc(AConfiguration *config) { (void)config; return 0; }
static void dn_AConfiguration_setMnc(AConfiguration *config, int32_t value) { (void)config; (void)value; }

/* ------------------------------------------------------------------ */
/* native window                                                    */
/* ------------------------------------------------------------------ */

struct ANativeWindow {
    uint32_t magic;
    int refcount;
    int32_t width;
    int32_t height;
    int32_t format;
    uint8_t *pixels;
    size_t pixel_bytes;
    void *native_layer;
    uint64_t frame_count;
    uint32_t checksum;
    pthread_mutex_t mutex;
};

static size_t dn_window_bytes_per_pixel(int32_t format)
{
    return format == 4 ? 2 : 4; /* RGB_565 versus 32-bit formats */
}

static ANativeWindow *dn_window_create_internal(int32_t width, int32_t height)
{
    ANativeWindow *window = (ANativeWindow *)calloc(1, sizeof(*window));
    if (!window)
        return NULL;
    window->magic = DN_ANDROID_MAGIC;
    window->refcount = 1;
    window->width = width > 0 ? width : 720;
    window->height = height > 0 ? height : 1280;
    window->format = 1; /* RGBA_8888 */
    pthread_mutex_init(&window->mutex, NULL);
    window->pixel_bytes = (size_t)window->width * (size_t)window->height *
                          dn_window_bytes_per_pixel(window->format);
    window->pixels = (uint8_t *)calloc(1, window->pixel_bytes ? window->pixel_bytes : 1);
    if (!window->pixels) {
        pthread_mutex_destroy(&window->mutex);
        free(window);
        return NULL;
    }
    return window;
}

static void dn_window_destroy_internal(ANativeWindow *window)
{
    if (!window)
        return;
    pthread_mutex_destroy(&window->mutex);
    free(window->pixels);
    free(window);
}

static void dn_ANativeWindow_acquire(ANativeWindow *window)
{
    if (!window || window->magic != DN_ANDROID_MAGIC)
        return;
    __atomic_add_fetch(&window->refcount, 1, __ATOMIC_SEQ_CST);
}

static void dn_ANativeWindow_release(ANativeWindow *window)
{
    if (!window || window->magic != DN_ANDROID_MAGIC)
        return;
    if (__atomic_sub_fetch(&window->refcount, 1, __ATOMIC_SEQ_CST) <= 0)
        dn_window_destroy_internal(window);
}

static int32_t dn_ANativeWindow_setBuffersGeometry(ANativeWindow *window,
                                                   int32_t width, int32_t height,
                                                   int32_t format)
{
    if (!window || window->magic != DN_ANDROID_MAGIC)
        return -1;
    if (width < 0 || height < 0 || width > 8192 || height > 8192)
        return -1;
    pthread_mutex_lock(&window->mutex);
    if (width > 0) window->width = width;
    if (height > 0) window->height = height;
    if (format > 0) window->format = format;
    size_t bytes = (size_t)window->width * (size_t)window->height *
                   dn_window_bytes_per_pixel(window->format);
    uint8_t *pixels = (uint8_t *)calloc(1, bytes ? bytes : 1);
    if (!pixels) {
        pthread_mutex_unlock(&window->mutex);
        return -1;
    }
    free(window->pixels);
    window->pixels = pixels;
    window->pixel_bytes = bytes;
    window->checksum = 0;
    pthread_mutex_unlock(&window->mutex);
    return 0;
}

static int32_t dn_ANativeWindow_lock(ANativeWindow *window,
                                     ANativeWindow_Buffer *buffer,
                                     ARect *dirty_bounds)
{
    if (!window || window->magic != DN_ANDROID_MAGIC || !buffer)
        return -1;
    pthread_mutex_lock(&window->mutex);
    memset(buffer, 0, sizeof(*buffer));
    buffer->width = window->width;
    buffer->height = window->height;
    buffer->stride = window->width;
    buffer->format = window->format;
    buffer->bits = window->pixels;
    if (dirty_bounds) {
        dirty_bounds->left = 0;
        dirty_bounds->top = 0;
        dirty_bounds->right = window->width;
        dirty_bounds->bottom = window->height;
    }
    return 0; /* mutex deliberately remains held until unlockAndPost */
}

static int32_t dn_ANativeWindow_unlockAndPost(ANativeWindow *window)
{
    if (!window || window->magic != DN_ANDROID_MAGIC)
        return -1;
    window->checksum = dn_android_checksum(window->pixels, window->pixel_bytes);
    window->frame_count++;
    dn_native_window_frame_fn callback;
    void *user;
    pthread_mutex_lock(&dn_android_state_mutex);
    callback = dn_window_frame_callback;
    user = dn_window_frame_user;
    pthread_mutex_unlock(&dn_android_state_mutex);
    if (callback) {
        callback(user, window->pixels, window->width, window->height,
                 window->width, window->format);
    }
    pthread_mutex_unlock(&window->mutex);
    return 0;
}

static int32_t dn_ANativeWindow_getWidth(ANativeWindow *window)
{
    return window && window->magic == DN_ANDROID_MAGIC ? window->width : -1;
}

static int32_t dn_ANativeWindow_getHeight(ANativeWindow *window)
{
    return window && window->magic == DN_ANDROID_MAGIC ? window->height : -1;
}

static int32_t dn_ANativeWindow_getFormat(ANativeWindow *window)
{
    return window && window->magic == DN_ANDROID_MAGIC ? window->format : -1;
}

static ANativeWindow *dn_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env;
    ANativeWindow *window = (ANativeWindow *)surface;
    if (window && window->magic == DN_ANDROID_MAGIC) {
        dn_ANativeWindow_acquire(window);
        return window;
    }
    return dn_window_create_internal(720, 1280);
}

static int32_t dn_ANativeWindow_setSwapInterval(ANativeWindow *window, int32_t interval)
{
    (void)window; (void)interval;
    return 0;
}

static int32_t dn_ANativeWindow_setAutoRefresh(ANativeWindow *window, bool auto_refresh)
{
    (void)window; (void)auto_refresh;
    return 0;
}

static int32_t dn_ANativeWindow_setFrameRate(ANativeWindow *window, float frame_rate,
                                             int8_t compatibility)
{
    (void)window; (void)frame_rate; (void)compatibility;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Looper + input                                                      */
/* ------------------------------------------------------------------ */

typedef int (*dn_looper_callback)(int fd, int events, void *data);

struct dn_looper_entry {
    bool used;
    int fd;
    int ident;
    int events;
    dn_looper_callback callback;
    void *data;
};

struct ALooper {
    uint32_t magic;
    int wake_read;
    int wake_write;
    int flags;
    struct dn_looper_entry entries[64];
    pthread_mutex_t mutex;
};

static pthread_key_t dn_looper_key;
static pthread_once_t dn_looper_once = PTHREAD_ONCE_INIT;

static void dn_looper_destroy_internal(ALooper *looper)
{
    if (!looper)
        return;
    if (looper->wake_read >= 0) close(looper->wake_read);
    if (looper->wake_write >= 0) close(looper->wake_write);
    pthread_mutex_destroy(&looper->mutex);
    free(looper);
}

static void dn_looper_thread_destroy(void *value)
{
    dn_looper_destroy_internal((ALooper *)value);
}

static void dn_looper_key_init(void)
{
    (void)pthread_key_create(&dn_looper_key, dn_looper_thread_destroy);
}

static ALooper *dn_ALooper_forThread(void)
{
    pthread_once(&dn_looper_once, dn_looper_key_init);
    return (ALooper *)pthread_getspecific(dn_looper_key);
}

static ALooper *dn_ALooper_prepare(int options)
{
    ALooper *existing = dn_ALooper_forThread();
    if (existing)
        return existing;
    ALooper *looper = (ALooper *)calloc(1, sizeof(*looper));
    if (!looper)
        return NULL;
    looper->magic = DN_ANDROID_MAGIC;
    looper->flags = options;
    looper->wake_read = -1;
    looper->wake_write = -1;
    pthread_mutex_init(&looper->mutex, NULL);
    int fds[2];
    if (pipe(fds) != 0) {
        dn_looper_destroy_internal(looper);
        return NULL;
    }
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
    looper->wake_read = fds[0];
    looper->wake_write = fds[1];
    pthread_setspecific(dn_looper_key, looper);
    return looper;
}

static void dn_ALooper_wake(ALooper *looper)
{
    if (!looper || looper->wake_write < 0)
        return;
    const uint8_t byte = 1;
    (void)!write(looper->wake_write, &byte, 1);
}

static int dn_ALooper_addFd(ALooper *looper, int fd, int ident, int events,
                            dn_looper_callback callback, void *data)
{
    if (!looper || fd < 0)
        return -1;
    pthread_mutex_lock(&looper->mutex);
    for (size_t i = 0; i < sizeof(looper->entries) / sizeof(looper->entries[0]); ++i) {
        if (!looper->entries[i].used || looper->entries[i].fd == fd) {
            looper->entries[i].used = true;
            looper->entries[i].fd = fd;
            looper->entries[i].ident = ident;
            looper->entries[i].events = events;
            looper->entries[i].callback = callback;
            looper->entries[i].data = data;
            pthread_mutex_unlock(&looper->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&looper->mutex);
    return -1;
}

static int dn_ALooper_removeFd(ALooper *looper, int fd)
{
    if (!looper)
        return -1;
    int result = -1;
    pthread_mutex_lock(&looper->mutex);
    for (size_t i = 0; i < sizeof(looper->entries) / sizeof(looper->entries[0]); ++i) {
        if (looper->entries[i].used && looper->entries[i].fd == fd) {
            memset(&looper->entries[i], 0, sizeof(looper->entries[i]));
            result = 0;
        }
    }
    pthread_mutex_unlock(&looper->mutex);
    return result;
}

static int dn_looper_poll_once_impl(ALooper *looper, int timeout_ms,
                                    int *out_fd, int *out_events, void **out_data)
{
    if (out_fd) *out_fd = -1;
    if (out_events) *out_events = 0;
    if (out_data) *out_data = NULL;
    if (!looper)
        return -4;

    struct dn_looper_entry entries[64];
    pthread_mutex_lock(&looper->mutex);
    memcpy(entries, looper->entries, sizeof(entries));
    pthread_mutex_unlock(&looper->mutex);

    struct pollfd polls[65];
    polls[0].fd = looper->wake_read;
    polls[0].events = POLLIN;
    polls[0].revents = 0;
    size_t poll_count = 1;
    int poll_entry_index[65];
    poll_entry_index[0] = -1;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (!entries[i].used)
            continue;
        short events = 0;
        if (entries[i].events & 1) events |= POLLIN;
        if (entries[i].events & 2) events |= POLLOUT;
        if (events == 0)
            events = POLLIN;
        polls[poll_count].fd = entries[i].fd;
        polls[poll_count].events = events;
        polls[poll_count].revents = 0;
        poll_entry_index[poll_count] = (int)i;
        poll_count++;
    }

    int rc = poll(polls, (nfds_t)poll_count, timeout_ms);
    if (rc < 0)
        return errno == EINTR ? -3 : -4;
    if (rc == 0)
        return -3;
    if (polls[0].revents & POLLIN) {
        uint8_t drain[64];
        while (read(looper->wake_read, drain, sizeof(drain)) > 0) {
        }
        return -1;
    }

    bool callback_ran = false;
    for (size_t i = 1; i < poll_count; ++i) {
        if (!polls[i].revents)
            continue;
        int entry_index = poll_entry_index[i];
        struct dn_looper_entry *entry = &entries[entry_index];
        int events = 0;
        if (polls[i].revents & POLLIN) events |= 1;
        if (polls[i].revents & POLLOUT) events |= 2;
        if (polls[i].revents & POLLERR) events |= 4;
        if (polls[i].revents & POLLHUP) events |= 8;
        if (polls[i].revents & POLLNVAL) events |= 16;
        if (entry->callback) {
            int keep = entry->callback(entry->fd, events, entry->data);
            callback_ran = true;
            if (!keep)
                (void)dn_ALooper_removeFd(looper, entry->fd);
        } else {
            if (out_fd) *out_fd = entry->fd;
            if (out_events) *out_events = events;
            if (out_data) *out_data = entry->data;
            return entry->ident;
        }
    }
    return callback_ran ? -2 : -3;
}

static int dn_ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    return dn_looper_poll_once_impl(dn_ALooper_forThread(), timeout_ms,
                                    out_fd, out_events, out_data);
}

static int dn_ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    int64_t deadline = timeout_ms >= 0 ? dn_android_now_ns() + (int64_t)timeout_ms * 1000000LL : 0;
    for (;;) {
        int remaining = timeout_ms;
        if (timeout_ms >= 0) {
            int64_t left = deadline - dn_android_now_ns();
            remaining = left <= 0 ? 0 : (int)(left / 1000000LL);
        }
        int result = dn_ALooper_pollOnce(remaining, out_fd, out_events, out_data);
        if (result != -2)
            return result;
        if (timeout_ms == 0)
            return -3;
    }
}

static void dn_ALooper_acquire(ALooper *looper) { (void)looper; }
static void dn_ALooper_release(ALooper *looper) { (void)looper; }

struct AInputEvent {
    uint32_t magic;
    int32_t type; /* 1 key, 2 motion */
    int32_t device_id;
    int32_t source;
    int32_t action;
    int32_t flags;
    int32_t meta_state;
    int32_t edge_flags;
    int32_t button_state;
    int64_t down_time;
    int64_t event_time;
    float x_precision;
    float y_precision;
    size_t pointer_count;
    struct {
        int32_t id;
        float x;
        float y;
        float pressure;
        float size;
        float touch_major;
        float touch_minor;
        float tool_major;
        float tool_minor;
        float orientation;
        float axis_values[48];
    } pointers[32];
    int32_t key_code;
    int32_t scan_code;
    int32_t repeat_count;
};

struct AInputQueue {
    uint32_t magic;
    AInputEvent *events[256];
    size_t head;
    size_t tail;
    size_t count;
    AInputEvent *current;
    int pipe_read;
    int pipe_write;
    ALooper *looper;
    pthread_mutex_t mutex;
};

static AInputQueue *dn_input_queue_create_internal(void)
{
    AInputQueue *queue = (AInputQueue *)calloc(1, sizeof(*queue));
    if (!queue)
        return NULL;
    queue->magic = DN_ANDROID_MAGIC;
    queue->pipe_read = -1;
    queue->pipe_write = -1;
    pthread_mutex_init(&queue->mutex, NULL);
    int fds[2];
    if (pipe(fds) == 0) {
        (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
        (void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
        queue->pipe_read = fds[0];
        queue->pipe_write = fds[1];
    }
    return queue;
}

static void dn_input_queue_destroy_internal(AInputQueue *queue)
{
    if (!queue)
        return;
    pthread_mutex_lock(&queue->mutex);
    for (size_t i = 0; i < queue->count; ++i)
        free(queue->events[(queue->head + i) % 256]);
    free(queue->current);
    pthread_mutex_unlock(&queue->mutex);
    if (queue->pipe_read >= 0) close(queue->pipe_read);
    if (queue->pipe_write >= 0) close(queue->pipe_write);
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

static bool dn_input_queue_push_internal(AInputQueue *queue, AInputEvent *event)
{
    if (!queue || !event)
        return false;
    pthread_mutex_lock(&queue->mutex);
    if (queue->count >= 256) {
        pthread_mutex_unlock(&queue->mutex);
        free(event);
        return false;
    }
    queue->events[queue->tail] = event;
    queue->tail = (queue->tail + 1) % 256;
    queue->count++;
    pthread_mutex_unlock(&queue->mutex);
    if (queue->pipe_write >= 0) {
        const uint8_t byte = 1;
        (void)!write(queue->pipe_write, &byte, 1);
    }
    return true;
}

struct dn_input_looper_bridge {
    AInputQueue *queue;
    dn_looper_callback callback;
    void *data;
};

static int dn_input_queue_looper_callback(int fd, int events, void *data)
{
    struct dn_input_looper_bridge *bridge = (struct dn_input_looper_bridge *)data;
    uint8_t drain[64];
    while (read(fd, drain, sizeof(drain)) > 0) {
    }
    if (bridge && bridge->callback)
        return bridge->callback(fd, events, bridge->data);
    return 1;
}

static void dn_AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper,
                                        int ident, dn_looper_callback callback, void *data)
{
    if (!queue || !looper || queue->pipe_read < 0)
        return;
    queue->looper = looper;
    struct dn_input_looper_bridge *bridge =
        (struct dn_input_looper_bridge *)calloc(1, sizeof(*bridge));
    if (!bridge)
        return;
    bridge->queue = queue;
    bridge->callback = callback;
    bridge->data = data;
    /* The bridge is intentionally retained for the queue lifetime;
     * detach removes the fd but DN keeps this tiny allocation until
     * process exit to avoid freeing it while another looper thread may
     * be inside the callback. */
    (void)dn_ALooper_addFd(looper, queue->pipe_read, ident, 1,
                           dn_input_queue_looper_callback, bridge);
}

static void dn_AInputQueue_detachLooper(AInputQueue *queue)
{
    if (!queue || !queue->looper)
        return;
    (void)dn_ALooper_removeFd(queue->looper, queue->pipe_read);
    queue->looper = NULL;
}

static int32_t dn_AInputQueue_hasEvents(AInputQueue *queue)
{
    if (!queue)
        return 0;
    pthread_mutex_lock(&queue->mutex);
    int32_t result = queue->count ? 1 : 0;
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

static int32_t dn_AInputQueue_getEvent(AInputQueue *queue, AInputEvent **out_event)
{
    if (!queue || !out_event)
        return -1;
    pthread_mutex_lock(&queue->mutex);
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }
    AInputEvent *event = queue->events[queue->head];
    queue->events[queue->head] = NULL;
    queue->head = (queue->head + 1) % 256;
    queue->count--;
    queue->current = event;
    *out_event = event;
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

static int32_t dn_AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event)
{
    (void)queue; (void)event;
    return 0;
}

static void dn_AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled)
{
    (void)handled;
    if (!queue || !event)
        return;
    pthread_mutex_lock(&queue->mutex);
    if (queue->current == event)
        queue->current = NULL;
    pthread_mutex_unlock(&queue->mutex);
    free(event);
}

static int32_t dn_AInputEvent_getType(const AInputEvent *event)
{
    return event ? event->type : 0;
}
static int32_t dn_AInputEvent_getDeviceId(const AInputEvent *event)
{
    return event ? event->device_id : 0;
}
static int32_t dn_AInputEvent_getSource(const AInputEvent *event)
{
    return event ? event->source : 0;
}
static int32_t dn_AKeyEvent_getAction(const AInputEvent *event)
{
    return event && event->type == 1 ? event->action : 0;
}
static int32_t dn_AKeyEvent_getFlags(const AInputEvent *event)
{
    return event && event->type == 1 ? event->flags : 0;
}
static int32_t dn_AKeyEvent_getKeyCode(const AInputEvent *event)
{
    return event && event->type == 1 ? event->key_code : 0;
}
static int32_t dn_AKeyEvent_getScanCode(const AInputEvent *event)
{
    return event && event->type == 1 ? event->scan_code : 0;
}
static int32_t dn_AKeyEvent_getMetaState(const AInputEvent *event)
{
    return event && event->type == 1 ? event->meta_state : 0;
}
static int32_t dn_AKeyEvent_getRepeatCount(const AInputEvent *event)
{
    return event && event->type == 1 ? event->repeat_count : 0;
}
static int64_t dn_AKeyEvent_getDownTime(const AInputEvent *event)
{
    return event && event->type == 1 ? event->down_time : 0;
}
static int64_t dn_AKeyEvent_getEventTime(const AInputEvent *event)
{
    return event && event->type == 1 ? event->event_time : 0;
}

static int32_t dn_AMotionEvent_getAction(const AInputEvent *event)
{
    return event && event->type == 2 ? event->action : 0;
}
static int32_t dn_AMotionEvent_getFlags(const AInputEvent *event)
{
    return event && event->type == 2 ? event->flags : 0;
}
static int32_t dn_AMotionEvent_getMetaState(const AInputEvent *event)
{
    return event && event->type == 2 ? event->meta_state : 0;
}
static int32_t dn_AMotionEvent_getButtonState(const AInputEvent *event)
{
    return event && event->type == 2 ? event->button_state : 0;
}
static int32_t dn_AMotionEvent_getEdgeFlags(const AInputEvent *event)
{
    return event && event->type == 2 ? event->edge_flags : 0;
}
static int64_t dn_AMotionEvent_getDownTime(const AInputEvent *event)
{
    return event && event->type == 2 ? event->down_time : 0;
}
static int64_t dn_AMotionEvent_getEventTime(const AInputEvent *event)
{
    return event && event->type == 2 ? event->event_time : 0;
}
static float dn_AMotionEvent_getX(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].x : 0;
}
static float dn_AMotionEvent_getY(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].y : 0;
}
static float dn_AMotionEvent_getPressure(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].pressure : 0;
}
static float dn_AMotionEvent_getSize(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].size : 0;
}
static float dn_AMotionEvent_getTouchMajor(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].touch_major : 0;
}
static float dn_AMotionEvent_getTouchMinor(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].touch_minor : 0;
}
static float dn_AMotionEvent_getToolMajor(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].tool_major : 0;
}
static float dn_AMotionEvent_getToolMinor(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].tool_minor : 0;
}
static float dn_AMotionEvent_getOrientation(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].orientation : 0;
}
static size_t dn_AMotionEvent_getPointerCount(const AInputEvent *event)
{
    return event && event->type == 2 ? event->pointer_count : 0;
}
static int32_t dn_AMotionEvent_getPointerId(const AInputEvent *event, size_t pointer_index)
{
    return event && event->type == 2 && pointer_index < event->pointer_count
               ? event->pointers[pointer_index].id : 0;
}
static size_t dn_AMotionEvent_findPointerIndex(const AInputEvent *event, int32_t pointer_id)
{
    if (!event || event->type != 2)
        return (size_t)-1;
    for (size_t i = 0; i < event->pointer_count; ++i) {
        if (event->pointers[i].id == pointer_id)
            return i;
    }
    return (size_t)-1;
}
static int64_t dn_AMotionEvent_getHistoricalEventTime(const AInputEvent *event, size_t historical_index)
{
    (void)historical_index;
    return event ? event->event_time : 0;
}
static size_t dn_AMotionEvent_getHistorySize(const AInputEvent *event)
{
    (void)event;
    return 0;
}
static float dn_AMotionEvent_getHistoricalX(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getX(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalY(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getY(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalPressure(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getPressure(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalSize(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getSize(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalTouchMajor(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getTouchMajor(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalTouchMinor(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getTouchMinor(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalToolMajor(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getToolMajor(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalToolMinor(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getToolMinor(event, pointer_index);
}
static float dn_AMotionEvent_getHistoricalOrientation(const AInputEvent *event, size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getOrientation(event, pointer_index);
}

static float dn_AMotionEvent_getAxisValue(const AInputEvent *event, int32_t axis, size_t pointer_index)
{
    if (!event || event->type != 2 || pointer_index >= event->pointer_count)
        return 0;
    switch (axis) {
    case 0: return event->pointers[pointer_index].x;
    case 1: return event->pointers[pointer_index].y;
    case 2: return event->pointers[pointer_index].pressure;
    case 3: return event->pointers[pointer_index].size;
    case 4: return event->pointers[pointer_index].touch_major;
    case 5: return event->pointers[pointer_index].touch_minor;
    case 6: return event->pointers[pointer_index].tool_major;
    case 7: return event->pointers[pointer_index].tool_minor;
    case 8: return event->pointers[pointer_index].orientation;
    default:
        return axis >= 0 && axis < 48 ? event->pointers[pointer_index].axis_values[axis] : 0;
    }
}

static float dn_AMotionEvent_getHistoricalAxisValue(const AInputEvent *event, int32_t axis,
                                                    size_t pointer_index, size_t historical_index)
{
    (void)historical_index;
    return dn_AMotionEvent_getAxisValue(event, axis, pointer_index);
}

/* ------------------------------------------------------------------ */
/* Local JNI object bridge (no Java VM)                                */
/* ------------------------------------------------------------------ */

struct dn_jni_object {
    uint32_t magic;
    int kind; /* 1 class, 2 string, 3 object array, 4 primitive array, 5 direct buffer */
    char *text;
    void **elements;
    void *data;
    int32_t length;
};

static uintptr_t dn_jni_null_stub(uintptr_t a, uintptr_t b, uintptr_t c,
                                  uintptr_t d, uintptr_t e, uintptr_t f,
                                  uintptr_t g, uintptr_t h)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    return 0;
}

static struct dn_jni_object *dn_jni_new_object(int kind, const char *text)
{
    struct dn_jni_object *object = (struct dn_jni_object *)calloc(1, sizeof(*object));
    if (!object)
        return NULL;
    object->magic = 0x4a4e4901u;
    object->kind = kind;
    object->text = text ? strdup(text) : NULL;
    return object;
}

static int32_t dn_jni_get_version(void *env)
{
    (void)env;
    return 0x00010006;
}

static void *dn_jni_find_class(void *env, const char *name)
{
    (void)env;
    return dn_jni_new_object(1, name ? name : "java/lang/Object");
}

static void *dn_jni_identity_ref(void *env, void *object)
{
    (void)env;
    return object;
}

static uint8_t dn_jni_is_same_object(void *env, void *a, void *b)
{
    (void)env;
    return a == b ? 1 : 0;
}

static void *dn_jni_get_object_class(void *env, void *object)
{
    (void)object;
    return dn_jni_find_class(env, "java/lang/Object");
}

static uint8_t dn_jni_is_instance_of(void *env, void *object, void *clazz)
{
    (void)env; (void)clazz;
    return object ? 1 : 0;
}

static void *dn_jni_member_id(void *env, void *clazz, const char *name, const char *sig)
{
    (void)env; (void)clazz; (void)sig;
    return dn_jni_new_object(6, name ? name : "<member>");
}

static void *dn_jni_new_string(void *env, const uint16_t *chars, int32_t length)
{
    (void)env;
    if (!chars || length < 0)
        return NULL;
    char *utf8 = (char *)malloc((size_t)length * 3 + 1);
    if (!utf8)
        return NULL;
    size_t out = 0;
    for (int32_t i = 0; i < length; ++i) {
        uint32_t ch = chars[i];
        if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < length) {
            uint32_t low = chars[i + 1];
            if (low >= 0xdc00 && low <= 0xdfff) {
                ch = 0x10000 + ((ch - 0xd800) << 10) + (low - 0xdc00);
                i++;
            }
        }
        if (ch < 0x80) {
            utf8[out++] = (char)ch;
        } else if (ch < 0x800) {
            utf8[out++] = (char)(0xc0 | (ch >> 6));
            utf8[out++] = (char)(0x80 | (ch & 0x3f));
        } else if (ch < 0x10000) {
            utf8[out++] = (char)(0xe0 | (ch >> 12));
            utf8[out++] = (char)(0x80 | ((ch >> 6) & 0x3f));
            utf8[out++] = (char)(0x80 | (ch & 0x3f));
        } else {
            utf8[out++] = (char)(0xf0 | (ch >> 18));
            utf8[out++] = (char)(0x80 | ((ch >> 12) & 0x3f));
            utf8[out++] = (char)(0x80 | ((ch >> 6) & 0x3f));
            utf8[out++] = (char)(0x80 | (ch & 0x3f));
        }
    }
    utf8[out] = '\0';
    struct dn_jni_object *object = dn_jni_new_object(2, utf8);
    free(utf8);
    return object;
}

static int32_t dn_jni_get_string_length(void *env, void *string_object)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)string_object;
    return object && object->magic == 0x4a4e4901u && object->text
               ? (int32_t)strlen(object->text) : 0;
}

static const uint16_t *dn_jni_get_string_chars(void *env, void *string_object, uint8_t *is_copy)
{
    (void)env;
    if (is_copy) *is_copy = 1;
    struct dn_jni_object *object = (struct dn_jni_object *)string_object;
    if (!object || object->magic != 0x4a4e4901u || !object->text)
        return NULL;
    size_t length = strlen(object->text);
    uint16_t *chars = (uint16_t *)malloc((length + 1) * sizeof(uint16_t));
    if (!chars)
        return NULL;
    for (size_t i = 0; i < length; ++i)
        chars[i] = (uint8_t)object->text[i];
    chars[length] = 0;
    return chars;
}

static void *dn_jni_new_string_utf(void *env, const char *bytes)
{
    (void)env;
    return dn_jni_new_object(2, bytes ? bytes : "");}

static const char *dn_jni_get_string_utf_chars(void *env, void *string_object, uint8_t *is_copy)
{
    (void)env;
    if (is_copy) *is_copy = 0;
    struct dn_jni_object *object = (struct dn_jni_object *)string_object;
    return object && object->magic == 0x4a4e4901u ? object->text : NULL;
}

static int32_t dn_jni_get_array_length(void *env, void *array)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)array;
    return object && object->magic == 0x4a4e4901u ? object->length : 0;
}

static void *dn_jni_new_object_array(void *env, int32_t length, void *clazz, void *initial)
{
    (void)env; (void)clazz;
    if (length < 0)
        return NULL;
    struct dn_jni_object *object = dn_jni_new_object(3, NULL);
    if (!object)
        return NULL;
    object->length = length;
    object->elements = (void **)calloc(length ? (size_t)length : 1, sizeof(void *));
    if (!object->elements) {
        free(object->text);
        free(object);
        return NULL;
    }
    for (int32_t i = 0; i < length; ++i)
        object->elements[i] = initial;
    return object;
}

static void *dn_jni_get_object_array_element(void *env, void *array, int32_t index)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)array;
    if (!object || object->magic != 0x4a4e4901u || index < 0 || index >= object->length)
        return NULL;
    return object->elements ? object->elements[index] : NULL;
}

static void dn_jni_set_object_array_element(void *env, void *array, int32_t index, void *value)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)array;
    if (object && object->magic == 0x4a4e4901u && object->elements &&
        index >= 0 && index < object->length)
        object->elements[index] = value;
}

static void *dn_jni_new_primitive_array(void *env, int32_t length)
{
    (void)env;
    if (length < 0)
        return NULL;
    struct dn_jni_object *object = dn_jni_new_object(4, NULL);
    if (!object)
        return NULL;
    object->length = length;
    object->data = calloc(length ? (size_t)length : 1, 8);
    if (!object->data) {
        free(object);
        return NULL;
    }
    return object;
}

static void *dn_jni_get_primitive_array_elements(void *env, void *array, uint8_t *is_copy)
{
    (void)env;
    if (is_copy) *is_copy = 0;
    struct dn_jni_object *object = (struct dn_jni_object *)array;
    return object && object->magic == 0x4a4e4901u ? object->data : NULL;
}

static void dn_jni_get_byte_array_region(void *env, void *array, int32_t start,
                                         int32_t length, int8_t *buffer)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)array;
    if (!object || object->magic != 0x4a4e4901u || !object->data || !buffer ||
        start < 0 || length < 0 || start + length > object->length)
        return;
    memcpy(buffer, (uint8_t *)object->data + start, (size_t)length);
}

static void dn_jni_set_byte_array_region(void *env, void *array, int32_t start,
                                         int32_t length, const int8_t *buffer)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)array;
    if (!object || object->magic != 0x4a4e4901u || !object->data || !buffer ||
        start < 0 || length < 0 || start + length > object->length)
        return;
    memcpy((uint8_t *)object->data + start, buffer, (size_t)length);
}

static void *dn_jni_new_direct_byte_buffer(void *env, void *address, int64_t capacity)
{
    (void)env;
    struct dn_jni_object *object = dn_jni_new_object(5, NULL);
    if (!object)
        return NULL;
    object->data = address;
    object->length = capacity > INT32_MAX ? INT32_MAX : (int32_t)capacity;
    return object;
}

static void *dn_jni_get_direct_buffer_address(void *env, void *buffer)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)buffer;
    return object && object->magic == 0x4a4e4901u ? object->data : buffer;
}

static int64_t dn_jni_get_direct_buffer_capacity(void *env, void *buffer)
{
    (void)env;
    struct dn_jni_object *object = (struct dn_jni_object *)buffer;
    return object && object->magic == 0x4a4e4901u ? object->length : -1;
}

static void *dn_jni_table[256];
static void *dn_jni_vm_table[8];
static void *dn_jni_env_pointer;
static void *dn_jni_vm_pointer;
static pthread_once_t dn_jni_once = PTHREAD_ONCE_INIT;

static int32_t dn_jni_vm_get_env(void *vm, void **env, int32_t version);
static int32_t dn_jni_vm_attach_current_thread(void *vm, void **env, void *args);
static int32_t dn_jni_vm_detach_current_thread(void *vm);
static int32_t dn_jni_get_java_vm(void *env, void **vm);

static void dn_jni_init_tables(void)
{
    for (size_t i = 0; i < sizeof(dn_jni_table) / sizeof(dn_jni_table[0]); ++i)
        dn_jni_table[i] = (void *)dn_jni_null_stub;
    dn_jni_table[4] = (void *)dn_jni_get_version;
    dn_jni_table[6] = (void *)dn_jni_find_class;
    dn_jni_table[21] = (void *)dn_jni_identity_ref; /* NewGlobalRef */
    dn_jni_table[24] = (void *)dn_jni_is_same_object;
    dn_jni_table[25] = (void *)dn_jni_identity_ref; /* NewLocalRef */
    dn_jni_table[31] = (void *)dn_jni_get_object_class;
    dn_jni_table[32] = (void *)dn_jni_is_instance_of;
    dn_jni_table[33] = (void *)dn_jni_member_id;    /* GetMethodID */
    dn_jni_table[94] = (void *)dn_jni_member_id;    /* GetFieldID */
    dn_jni_table[113] = (void *)dn_jni_member_id;   /* GetStaticMethodID */
    dn_jni_table[144] = (void *)dn_jni_member_id;   /* GetStaticFieldID */
    dn_jni_table[163] = (void *)dn_jni_new_string;
    dn_jni_table[164] = (void *)dn_jni_get_string_length;
    dn_jni_table[165] = (void *)dn_jni_get_string_chars;
    dn_jni_table[167] = (void *)dn_jni_new_string_utf;
    dn_jni_table[168] = (void *)dn_jni_get_string_length;
    dn_jni_table[169] = (void *)dn_jni_get_string_utf_chars;
    dn_jni_table[171] = (void *)dn_jni_get_array_length;
    dn_jni_table[172] = (void *)dn_jni_new_object_array;
    dn_jni_table[173] = (void *)dn_jni_get_object_array_element;
    dn_jni_table[174] = (void *)dn_jni_set_object_array_element;
    for (int slot = 175; slot <= 182; ++slot)
        dn_jni_table[slot] = (void *)dn_jni_new_primitive_array;
    for (int slot = 183; slot <= 190; ++slot)
        dn_jni_table[slot] = (void *)dn_jni_get_primitive_array_elements;
    dn_jni_table[200] = (void *)dn_jni_get_byte_array_region;
    dn_jni_table[208] = (void *)dn_jni_set_byte_array_region;
    dn_jni_table[219] = (void *)dn_jni_get_java_vm;
    dn_jni_table[222] = (void *)dn_jni_get_primitive_array_elements;
    dn_jni_table[229] = (void *)dn_jni_new_direct_byte_buffer;
    dn_jni_table[230] = (void *)dn_jni_get_direct_buffer_address;
    dn_jni_table[231] = (void *)dn_jni_get_direct_buffer_capacity;
    dn_jni_env_pointer = dn_jni_table;

    for (size_t i = 0; i < sizeof(dn_jni_vm_table) / sizeof(dn_jni_vm_table[0]); ++i)
        dn_jni_vm_table[i] = (void *)dn_jni_null_stub;
    dn_jni_vm_table[4] = (void *)dn_jni_vm_attach_current_thread;
    dn_jni_vm_table[5] = (void *)dn_jni_vm_detach_current_thread;
    dn_jni_vm_table[6] = (void *)dn_jni_vm_get_env;
    dn_jni_vm_table[7] = (void *)dn_jni_vm_attach_current_thread;
    dn_jni_vm_pointer = dn_jni_vm_table;
}

static void *dn_jni_env(void)
{
    pthread_once(&dn_jni_once, dn_jni_init_tables);
    return &dn_jni_env_pointer;
}

static void *dn_jni_vm(void)
{
    pthread_once(&dn_jni_once, dn_jni_init_tables);
    return &dn_jni_vm_pointer;
}

static int32_t dn_jni_vm_get_env(void *vm, void **env, int32_t version)
{
    (void)vm; (void)version;
    if (env)
        *env = dn_jni_env();
    return 0;
}

static int32_t dn_jni_vm_attach_current_thread(void *vm, void **env, void *args)
{
    (void)args;
    (void)dn_native_thread_pointer();
    return dn_jni_vm_get_env(vm, env, 0x00010006);
}

static int32_t dn_jni_vm_detach_current_thread(void *vm)
{
    (void)vm;
    return 0;
}

static int32_t dn_jni_get_java_vm(void *env, void **vm)
{
    (void)env;
    if (vm)
        *vm = dn_jni_vm();
    return 0;
}

static int32_t dn_JNI_GetCreatedJavaVMs(void **vm_buffer, int32_t buffer_length, int32_t *count)
{
    if (count)
        *count = 1;
    if (vm_buffer && buffer_length > 0)
        vm_buffer[0] = dn_jni_vm();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sensors (metadata + queue ABI; no synthetic sensor samples)         */
/* ------------------------------------------------------------------ */

struct ASensor {
    uint32_t magic;
    int32_t type;
    const char *name;
    const char *vendor;
    float resolution;
    int32_t min_delay;
};

struct ASensorManager {
    uint32_t magic;
    ASensor sensors[5];
    const ASensor *sensor_list[5];
};

struct ASensorEventQueue {
    uint32_t magic;
    ALooper *looper;
    int ident;
    void *data;
    bool enabled[32];
    int32_t rates[32];
};

static ASensorManager *dn_sensor_manager_singleton(void)
{
    static ASensorManager manager;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    /* pthread_once with a nested initializer is avoided for strict C;
     * initialize idempotently under the Android state mutex instead. */
    (void)once;
    pthread_mutex_lock(&dn_android_state_mutex);
    if (!manager.magic) {
        manager.magic = DN_ANDROID_MAGIC;
        const struct { int32_t type; const char *name; } defs[5] = {
            { 1, "iPhone accelerometer" }, { 2, "iPhone magnetic field" },
            { 4, "iPhone gyroscope" }, { 5, "iPhone light" },
            { 8, "iPhone proximity" }
        };
        for (int i = 0; i < 5; ++i) {
            manager.sensors[i].magic = DN_ANDROID_MAGIC;
            manager.sensors[i].type = defs[i].type;
            manager.sensors[i].name = defs[i].name;
            manager.sensors[i].vendor = "Apple";
            manager.sensors[i].resolution = 0.01f;
            manager.sensors[i].min_delay = 10000;
            manager.sensor_list[i] = &manager.sensors[i];
        }
    }
    pthread_mutex_unlock(&dn_android_state_mutex);
    return &manager;
}

static ASensorManager *dn_ASensorManager_getInstance(void)
{
    return dn_sensor_manager_singleton();
}

static ASensorManager *dn_ASensorManager_getInstanceForPackage(const char *package)
{
    (void)package;
    return dn_sensor_manager_singleton();
}

static const ASensor *dn_ASensorManager_getDefaultSensor(ASensorManager *manager, int type)
{
    if (!manager)
        return NULL;
    for (int i = 0; i < 5; ++i) {
        if (manager->sensors[i].type == type)
            return &manager->sensors[i];
    }
    return NULL;
}

static int dn_ASensorManager_getSensorList(ASensorManager *manager, const ASensor ***list)
{
    if (!manager || !list)
        return 0;
    *list = manager->sensor_list;
    return 5;
}

static ASensorEventQueue *dn_ASensorManager_createEventQueue(
    ASensorManager *manager, ALooper *looper, int ident,
    void *callback, void *data)
{
    (void)manager; (void)callback;
    ASensorEventQueue *queue = (ASensorEventQueue *)calloc(1, sizeof(*queue));
    if (!queue)
        return NULL;
    queue->magic = DN_ANDROID_MAGIC;
    queue->looper = looper;
    queue->ident = ident;
    queue->data = data;
    return queue;
}

static int dn_ASensorManager_destroyEventQueue(ASensorManager *manager, ASensorEventQueue *queue)
{
    (void)manager;
    free(queue);
    return 0;
}

static int dn_sensor_index(const ASensor *sensor)
{
    return sensor && sensor->type >= 0 && sensor->type < 32 ? sensor->type : -1;
}

static int dn_ASensorEventQueue_enableSensor(ASensorEventQueue *queue, const ASensor *sensor)
{
    int index = dn_sensor_index(sensor);
    if (!queue || index < 0) return -1;
    queue->enabled[index] = true;
    return 0;
}

static int dn_ASensorEventQueue_disableSensor(ASensorEventQueue *queue, const ASensor *sensor)
{
    int index = dn_sensor_index(sensor);
    if (!queue || index < 0) return -1;
    queue->enabled[index] = false;
    return 0;
}

static int dn_ASensorEventQueue_setEventRate(ASensorEventQueue *queue, const ASensor *sensor, int32_t usec)
{
    int index = dn_sensor_index(sensor);
    if (!queue || index < 0) return -1;
    queue->rates[index] = usec;
    return 0;
}

static int dn_ASensorEventQueue_hasEvents(ASensorEventQueue *queue)
{
    (void)queue;
    return 0;
}

static ssize_t dn_ASensorEventQueue_getEvents(ASensorEventQueue *queue, void *events, size_t count)
{
    (void)queue; (void)events; (void)count;
    return 0;
}

static const char *dn_ASensor_getName(const ASensor *sensor) { return sensor ? sensor->name : NULL; }
static const char *dn_ASensor_getVendor(const ASensor *sensor) { return sensor ? sensor->vendor : NULL; }
static int dn_ASensor_getType(const ASensor *sensor) { return sensor ? sensor->type : 0; }
static float dn_ASensor_getResolution(const ASensor *sensor) { return sensor ? sensor->resolution : 0; }
static int dn_ASensor_getMinDelay(const ASensor *sensor) { return sensor ? sensor->min_delay : 0; }
static int dn_ASensor_getFifoReservedEventCount(const ASensor *sensor) { (void)sensor; return 0; }
static int dn_ASensor_getFifoMaxEventCount(const ASensor *sensor) { (void)sensor; return 0; }
static const char *dn_ASensor_getStringType(const ASensor *sensor) { return sensor ? sensor->name : NULL; }
static int dn_ASensor_getReportingMode(const ASensor *sensor) { (void)sensor; return 0; }
static bool dn_ASensor_isWakeUpSensor(const ASensor *sensor) { (void)sensor; return false; }

/* ------------------------------------------------------------------ */
/* Android log + system properties                                     */
/* ------------------------------------------------------------------ */

struct dn_log_values {
    uintptr_t gp[16];
    double fp[16];
    size_t gp_index;
    size_t fp_index;
};

static void dn_android_log_format_to_buffer(const char *fmt,
                                            struct dn_log_values *values,
                                            char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!fmt)
        fmt = "(null)";
    size_t used = 0;
    for (const char *p = fmt; *p && used + 1 < out_len;) {
        if (*p != '%') {
            out[used++] = *p++;
            out[used] = '\0';
            continue;
        }
        const char *spec_start = p++;
        if (*p == '%') {
            out[used++] = *p++;
            out[used] = '\0';
            continue;
        }
        /* Skip flags/width/precision/length; only a star consumes a
         * GP value.  The piece is then printed by snprintf, preserving
         * padding semantics for the common conversions. */
        while (*p && strchr("-+ #0'", *p)) p++;
        if (*p == '*') {
            if (values && values->gp_index < 16) values->gp_index++;
            p++;
        } else {
            while (*p >= '0' && *p <= '9') p++;
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                if (values && values->gp_index < 16) values->gp_index++;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') p++;
            }
        }
        while (*p && strchr("hljztL", *p)) p++;
        char conversion = *p ? *p++ : '\0';
        if (!conversion)
            break;
        char spec[40];
        size_t spec_len = (size_t)(p - spec_start);
        if (spec_len >= sizeof(spec))
            continue;
        memcpy(spec, spec_start, spec_len);
        spec[spec_len] = '\0';
        char piece[256];
        piece[0] = '\0';
        uintptr_t value = values && values->gp_index < 16 ? values->gp[values->gp_index] : 0;
        double fp_value = values && values->fp_index < 16 ? values->fp[values->fp_index] : 0;
        switch (conversion) {
        case 'd': case 'i':
            snprintf(piece, sizeof(piece), "%lld", (long long)(int64_t)value);
            if (values) values->gp_index++;
            break;
        case 'u': case 'x': case 'X': case 'o':
            snprintf(piece, sizeof(piece), conversion == 'u' ? "%llu" : conversion == 'x' ? "%llx" : conversion == 'X' ? "%llX" : "%llo",
                     (unsigned long long)value);
            if (values) values->gp_index++;
            break;
        case 'p':
            snprintf(piece, sizeof(piece), "%p", (void *)value);
            if (values) values->gp_index++;
            break;
        case 'c':
            snprintf(piece, sizeof(piece), "%c", (int)value);
            if (values) values->gp_index++;
            break;
        case 's': {
            const char *text = value ? (const char *)(uintptr_t)value : "(null)";
            snprintf(piece, sizeof(piece), "%s", text);
            if (values) values->gp_index++;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            snprintf(piece, sizeof(piece), "%g", fp_value);
            if (values) values->fp_index++;
            break;
        default:
            snprintf(piece, sizeof(piece), "%s", spec);
            break;
        }
        size_t piece_len = strlen(piece);
        if (piece_len > out_len - used - 1)
            piece_len = out_len - used - 1;
        memcpy(out + used, piece, piece_len);
        used += piece_len;
        out[used] = '\0';
    }
}

static void dn_android_log_emit(int priority, const char *tag, const char *message)
{
    fprintf(stderr, "[android-log %d %s] %s\n", priority,
            tag ? tag : "DroidNative", message ? message : "");
    fflush(stderr);
}

static int dn___android_log_print(int priority, const char *tag, const char *fmt,
                                  uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                                  uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7,
                                  uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11,
                                  uintptr_t a12, uintptr_t a13, uintptr_t a14, uintptr_t a15)
{
    struct dn_log_values values;
    memset(&values, 0, sizeof(values));
    values.gp[0] = a0; values.gp[1] = a1; values.gp[2] = a2; values.gp[3] = a3;
    values.gp[4] = a4; values.gp[5] = a5; values.gp[6] = a6; values.gp[7] = a7;
    values.gp[8] = a8; values.gp[9] = a9; values.gp[10] = a10; values.gp[11] = a11;
    values.gp[12] = a12; values.gp[13] = a13; values.gp[14] = a14; values.gp[15] = a15;
    char message[1024];
    dn_android_log_format_to_buffer(fmt, &values, message, sizeof(message));
    dn_android_log_emit(priority, tag, message);
    return (int)strlen(message);
}

struct dn_aarch64_va_list {
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
};

static void dn_log_values_from_aarch64_va_list(const struct dn_aarch64_va_list *ap,
                                               struct dn_log_values *values)
{
    memset(values, 0, sizeof(*values));
    if (!ap)
        return;
    for (size_t i = 0; i < 16; ++i) {
        int offset = ap->gr_offs + (int)i * 8;
        const uint8_t *address = offset < 0
            ? (const uint8_t *)ap->gr_top + offset
            : (const uint8_t *)ap->stack + offset;
        if (address)
            memcpy(&values->gp[i], address, sizeof(uintptr_t));
    }
    for (size_t i = 0; i < 16; ++i) {
        int offset = ap->vr_offs + (int)i * 16;
        const uint8_t *address = offset < 0
            ? (const uint8_t *)ap->vr_top + offset
            : (const uint8_t *)ap->stack + (ap->vr_offs + (int)i * 8);
        if (address)
            memcpy(&values->fp[i], address, sizeof(double));
    }
}

static int dn___android_log_vprint(int priority, const char *tag, const char *fmt,
                                   struct dn_aarch64_va_list *ap)
{
    struct dn_log_values values;
    dn_log_values_from_aarch64_va_list(ap, &values);
    char message[1024];
    dn_android_log_format_to_buffer(fmt, &values, message, sizeof(message));
    dn_android_log_emit(priority, tag, message);
    return (int)strlen(message);
}

static int dn___android_log_write(int priority, const char *tag, const char *message)
{
    dn_android_log_emit(priority, tag, message);
    return 1;
}

static int dn___android_log_buf_write(int buffer_id, int priority,
                                      const char *tag, const char *message)
{
    (void)buffer_id;
    return dn___android_log_write(priority, tag, message);
}

static int dn___android_log_buf_print(int buffer_id, int priority, const char *tag,
                                      const char *fmt, uintptr_t a0, uintptr_t a1,
                                      uintptr_t a2, uintptr_t a3, uintptr_t a4,
                                      uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    (void)buffer_id;
    return dn___android_log_print(priority, tag, fmt, a0, a1, a2, a3, a4, a5, a6, a7, 0, 0, 0, 0, 0, 0, 0, 0);
}

static void dn___android_log_assert(const char *condition, const char *tag,
                                    const char *fmt, uintptr_t a0, uintptr_t a1,
                                    uintptr_t a2, uintptr_t a3)
{
    if (fmt) {
        (void)dn___android_log_print(7, tag, fmt, a0, a1, a2, a3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else {
        dn_android_log_emit(7, tag, condition ? condition : "assertion failed");
    }
    abort();
}

static int dn_android_get_device_api_level(void)
{
    return 35;
}

struct dn_prop_info {
    uint32_t magic;
    char name[96];
    char value[96];
    uint32_t serial;
};

static const char *dn_property_value_for_name(const char *name)
{
    if (!name)
        return NULL;
    static const struct { const char *name; const char *value; } properties[] = {
        { "ro.build.version.sdk", "35" },
        { "ro.build.version.release", "15" },
        { "ro.build.version.codename", "REL" },
        { "ro.build.type", "user" },
        { "ro.build.tags", "release-keys" },
        { "ro.product.model", "iPhone17,2" },
        { "ro.product.brand", "Apple" },
        { "ro.product.manufacturer", "Apple" },
        { "ro.product.name", "iDroid" },
        { "ro.product.device", "iDroid" },
        { "ro.hardware", "aarch64" },
        { "ro.product.cpu.abi", "arm64-v8a" },
        { "ro.product.cpu.abilist", "arm64-v8a" },
        { "ro.product.locale", "en-US" },
        { "ro.sf.lcd_density", "560" },
        { "persist.sys.timezone", "Europe/Bucharest" },
        { "ro.kernel.qemu", "0" },
        { "ro.droidnative.version", "0.4.13" }
    };
    for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); ++i) {
        if (strcmp(name, properties[i].name) == 0)
            return properties[i].value;
    }
    if (strcmp(name, "ro.product.package") == 0) {
        pthread_mutex_lock(&dn_android_state_mutex);
        const char *package = dn_package_name ? dn_package_name : "ro.droidnative.app";
        pthread_mutex_unlock(&dn_android_state_mutex);
        return package;
    }
    return NULL;
}

static int dn___system_property_get(const char *name, char *value)
{
    const char *found = dn_property_value_for_name(name);
    if (value) {
        if (found) {
            /* The Android contract still requires a 92-byte buffer,
             * but do not use strncpy(): its zero padding writes all
             * the way to byte 91 even for a two-byte value.  Copy only
             * the value plus its terminator (truncated at 91 chars),
             * so a short property cannot smash a caller's stack frame
             * merely because this DN implementation padded the rest. */
            size_t length = strlen(found);
            size_t copy = length > 91 ? 91 : length;
            memcpy(value, found, copy);
            value[copy] = '\0';
            return (int)copy;
        }
        value[0] = '\0';
        return 0;
    }
    return found ? (int)strlen(found) : 0;
}

static const struct dn_prop_info *dn___system_property_find(const char *name)
{
    const char *value = dn_property_value_for_name(name);
    if (!value)
        return NULL;
    struct dn_prop_info *info = (struct dn_prop_info *)calloc(1, sizeof(*info));
    if (!info)
        return NULL;
    info->magic = DN_ANDROID_MAGIC;
    strncpy(info->name, name, sizeof(info->name) - 1);
    strncpy(info->value, value, sizeof(info->value) - 1);
    info->serial = 1;
    return info;
}

static int dn___system_property_read(const struct dn_prop_info *info,
                                     char *name, char *value)
{
    if (!info || info->magic != DN_ANDROID_MAGIC)
        return 0;
    if (name) strcpy(name, info->name);
    if (value) strcpy(value, info->value);
    return (int)strlen(info->value);
}

typedef void (*dn_property_callback)(void *user, const char *name,
                                     const char *value, uint32_t serial);

static void dn___system_property_read_callback(const struct dn_prop_info *info,
                                               dn_property_callback callback, void *user)
{
    if (info && info->magic == DN_ANDROID_MAGIC && callback)
        callback(user, info->name, info->value, info->serial);
}

static int dn___system_property_wait(const struct dn_prop_info *old_info,
                                     uint32_t old_serial,
                                     const struct dn_prop_info **new_info,
                                     const struct timespec *timeout)
{
    (void)old_serial; (void)timeout;
    if (new_info)
        *new_info = old_info;
    return 0;
}

static int dn___system_property_set(const char *name, const char *value)
{
    (void)name; (void)value;
    return -1;
}

static int dn___system_property_foreach(void *callback, void *user)
{
    (void)callback; (void)user;
    return 0;
}

/* ------------------------------------------------------------------ */
/* AAudio silent clocked streams                                       */
/* ------------------------------------------------------------------ */

typedef int32_t dn_aaudio_result;
typedef int32_t (*dn_aaudio_data_callback)(void *stream, void *user,
                                           void *audio_data, int32_t num_frames);
typedef void (*dn_aaudio_error_callback)(void *stream, void *user, int32_t error);

struct dn_aaudio_builder {
    uint32_t magic;
    int32_t device_id;
    int32_t sample_rate;
    int32_t channel_count;
    int32_t format;
    int32_t direction;
    int32_t sharing_mode;
    int32_t performance_mode;
    int32_t usage;
    int32_t content_type;
    int32_t input_preset;
    int32_t buffer_capacity;
    dn_aaudio_data_callback data_callback;
    dn_aaudio_error_callback error_callback;
    void *user_data;
};

struct dn_aaudio_stream {
    uint32_t magic;
    struct dn_aaudio_builder config;
    int32_t state;
    int64_t start_time_ns;
    int64_t base_frames;
    pthread_t callback_thread;
    bool callback_thread_created;
    volatile bool callback_stop;
};

static int64_t dn_aaudio_stream_position(struct dn_aaudio_stream *stream)
{
    if (!stream)
        return 0;
    if (stream->state != 4 || stream->start_time_ns == 0)
        return stream->base_frames;
    int64_t elapsed = dn_android_now_ns() - stream->start_time_ns;
    int32_t rate = stream->config.sample_rate > 0 ? stream->config.sample_rate : 48000;
    return stream->base_frames + elapsed * rate / 1000000000LL;
}

static void *dn_aaudio_callback_thread(void *opaque)
{
    struct dn_aaudio_stream *stream = (struct dn_aaudio_stream *)opaque;
    const int32_t frames = 128;
    int32_t channels = stream->config.channel_count > 0 ? stream->config.channel_count : 2;
    size_t frame_bytes = stream->config.format == 1 ? sizeof(float) : sizeof(int16_t);
    void *buffer = calloc((size_t)frames * (size_t)channels, frame_bytes);
    if (!buffer)
        return NULL;
    while (!stream->callback_stop && stream->state == 4) {
        if (stream->config.direction == 1)
            memset(buffer, 0, (size_t)frames * (size_t)channels * frame_bytes);
        int32_t result = stream->config.data_callback(
            stream, stream->config.user_data, buffer, frames);
        if (result != 0)
            break;
        int32_t rate = stream->config.sample_rate > 0 ? stream->config.sample_rate : 48000;
        int64_t delay_ns = (int64_t)frames * 1000000000LL / rate;
        struct timespec nap = { (time_t)(delay_ns / 1000000000LL),
                                (long)(delay_ns % 1000000000LL) };
        nanosleep(&nap, NULL);
    }
    free(buffer);
    return NULL;
}

static dn_aaudio_result dn_AAudio_createStreamBuilder(struct dn_aaudio_builder **builder)
{
    if (!builder)
        return -1;
    struct dn_aaudio_builder *created =
        (struct dn_aaudio_builder *)calloc(1, sizeof(*created));
    if (!created)
        return -1;
    created->magic = DN_ANDROID_MAGIC;
    created->sample_rate = 48000;
    created->channel_count = 2;
    created->format = 2; /* PCM_I16 in DN's simplified internal map */
    created->buffer_capacity = 4096;
    *builder = created;
    return 0;
}

static dn_aaudio_result dn_AAudioStreamBuilder_delete(struct dn_aaudio_builder *builder)
{
    free(builder);
    return 0;
}

#define DN_AAUDIO_BUILDER_SETTER(field, name, type) \
    static void dn_AAudioStreamBuilder_set##name(struct dn_aaudio_builder *builder, type value) { if (builder) builder->field = value; }
#define DN_AAUDIO_BUILDER_GETTER(field, name, type) \
    static type dn_AAudioStreamBuilder_get##name(struct dn_aaudio_builder *builder) { return builder ? builder->field : 0; }

DN_AAUDIO_BUILDER_SETTER(device_id, DeviceId, int32_t)
DN_AAUDIO_BUILDER_GETTER(device_id, DeviceId, int32_t)
DN_AAUDIO_BUILDER_SETTER(sample_rate, SampleRate, int32_t)
DN_AAUDIO_BUILDER_GETTER(sample_rate, SampleRate, int32_t)
DN_AAUDIO_BUILDER_SETTER(channel_count, ChannelCount, int32_t)
DN_AAUDIO_BUILDER_GETTER(channel_count, ChannelCount, int32_t)
DN_AAUDIO_BUILDER_SETTER(format, Format, int32_t)
DN_AAUDIO_BUILDER_GETTER(format, Format, int32_t)
DN_AAUDIO_BUILDER_SETTER(direction, Direction, int32_t)
DN_AAUDIO_BUILDER_GETTER(direction, Direction, int32_t)
DN_AAUDIO_BUILDER_SETTER(sharing_mode, SharingMode, int32_t)
DN_AAUDIO_BUILDER_GETTER(sharing_mode, SharingMode, int32_t)
DN_AAUDIO_BUILDER_SETTER(performance_mode, PerformanceMode, int32_t)
DN_AAUDIO_BUILDER_GETTER(performance_mode, PerformanceMode, int32_t)
DN_AAUDIO_BUILDER_SETTER(usage, Usage, int32_t)
DN_AAUDIO_BUILDER_GETTER(usage, Usage, int32_t)
DN_AAUDIO_BUILDER_SETTER(content_type, ContentType, int32_t)
DN_AAUDIO_BUILDER_GETTER(content_type, ContentType, int32_t)
DN_AAUDIO_BUILDER_SETTER(input_preset, InputPreset, int32_t)
DN_AAUDIO_BUILDER_GETTER(input_preset, InputPreset, int32_t)
DN_AAUDIO_BUILDER_SETTER(buffer_capacity, BufferCapacityInFrames, int32_t)
DN_AAUDIO_BUILDER_GETTER(buffer_capacity, BufferCapacityInFrames, int32_t)

static void dn_AAudioStreamBuilder_setDataCallback(
    struct dn_aaudio_builder *builder, dn_aaudio_data_callback callback)
{
    if (builder) builder->data_callback = callback;
}
static void dn_AAudioStreamBuilder_setErrorCallback(
    struct dn_aaudio_builder *builder, dn_aaudio_error_callback callback)
{
    if (builder) builder->error_callback = callback;
}
static void dn_AAudioStreamBuilder_setUserData(struct dn_aaudio_builder *builder, void *user)
{
    if (builder) builder->user_data = user;
}
static void dn_AAudioStreamBuilder_setChannelMask(struct dn_aaudio_builder *builder, int32_t mask)
{
    if (!builder)
        return;
    int count = 0;
    uint32_t bits = (uint32_t)mask;
    while (bits) {
        count += bits & 1u;
        bits >>= 1;
    }
    builder->channel_count = count > 0 ? count : 2;
}
static int32_t dn_AAudioStreamBuilder_getChannelMask(struct dn_aaudio_builder *builder)
{
    return builder && builder->channel_count == 1 ? 0x1 : 0x3;
}
static void dn_AAudioStreamBuilder_setFramesPerDataCallback(struct dn_aaudio_builder *builder, int32_t frames)
{
    (void)builder; (void)frames;
}
static int32_t dn_AAudioStreamBuilder_getFramesPerDataCallback(struct dn_aaudio_builder *builder)
{
    (void)builder;
    return 128;
}

static dn_aaudio_result dn_AAudioStreamBuilder_openStream(
    struct dn_aaudio_builder *builder, struct dn_aaudio_stream **stream_out)
{
    if (!builder || !stream_out)
        return -1;
    struct dn_aaudio_stream *stream =
        (struct dn_aaudio_stream *)calloc(1, sizeof(*stream));
    if (!stream)
        return -1;
    stream->magic = DN_ANDROID_MAGIC;
    stream->config = *builder;
    stream->state = 2; /* OPEN */
    *stream_out = stream;
    return 0;
}

static dn_aaudio_result dn_AAudioStream_close(struct dn_aaudio_stream *stream)
{
    if (!stream)
        return -1;
    stream->callback_stop = true;
    stream->state = 12; /* CLOSED */
    if (stream->callback_thread_created)
        pthread_join(stream->callback_thread, NULL);
    free(stream);
    return 0;
}

static dn_aaudio_result dn_AAudioStream_requestStart(struct dn_aaudio_stream *stream)
{
    if (!stream)
        return -1;
    stream->base_frames = dn_aaudio_stream_position(stream);
    stream->start_time_ns = dn_android_now_ns();
    stream->state = 4; /* STARTED */
    stream->callback_stop = false;
    if (stream->config.data_callback && !stream->callback_thread_created) {
        if (pthread_create(&stream->callback_thread, NULL,
                           dn_aaudio_callback_thread, stream) == 0)
            stream->callback_thread_created = true;
    }
    return 0;
}

static dn_aaudio_result dn_AAudioStream_requestPause(struct dn_aaudio_stream *stream)
{
    if (!stream) return -1;
    stream->base_frames = dn_aaudio_stream_position(stream);
    stream->state = 6;
    return 0;
}

static dn_aaudio_result dn_AAudioStream_requestFlush(struct dn_aaudio_stream *stream)
{
    if (!stream) return -1;
    stream->base_frames = dn_aaudio_stream_position(stream);
    stream->state = 8;
    return 0;
}

static dn_aaudio_result dn_AAudioStream_requestStop(struct dn_aaudio_stream *stream)
{
    if (!stream) return -1;
    stream->base_frames = dn_aaudio_stream_position(stream);
    stream->state = 10;
    stream->callback_stop = true;
    return 0;
}

static dn_aaudio_result dn_AAudioStream_waitForStateChange(
    struct dn_aaudio_stream *stream, int32_t input_state,
    int32_t *next_state, int64_t timeout_nanoseconds)
{
    (void)input_state; (void)timeout_nanoseconds;
    if (!stream) return -1;
    if (next_state) *next_state = stream->state;
    return 0;
}

static dn_aaudio_result dn_AAudioStream_read(struct dn_aaudio_stream *stream,
                                             void *buffer, int32_t num_frames,
                                             int64_t timeout_nanoseconds)
{
    (void)timeout_nanoseconds;
    if (!stream || !buffer || num_frames < 0)
        return -1;
    int32_t channels = stream->config.channel_count > 0 ? stream->config.channel_count : 2;
    size_t frame_bytes = (stream->config.format == 1 ? sizeof(float) : sizeof(int16_t)) *
                         (size_t)channels;
    memset(buffer, 0, frame_bytes * (size_t)num_frames);
    if (stream->state != 4)
        (void)dn_AAudioStream_requestStart(stream);
    return num_frames;
}

static dn_aaudio_result dn_AAudioStream_write(struct dn_aaudio_stream *stream,
                                              const void *buffer, int32_t num_frames,
                                              int64_t timeout_nanoseconds)
{
    (void)buffer; (void)timeout_nanoseconds;
    if (!stream || num_frames < 0)
        return -1;
    if (stream->state != 4)
        (void)dn_AAudioStream_requestStart(stream);
    return num_frames;
}

static dn_aaudio_result dn_AAudioStream_setBufferSizeInFrames(
    struct dn_aaudio_stream *stream, int32_t requested_frames)
{
    if (!stream) return -1;
    stream->config.buffer_capacity = requested_frames;
    return requested_frames;
}

static int32_t dn_AAudioStream_getBufferSizeInFrames(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.buffer_capacity : 0;
}
static int32_t dn_AAudioStream_getBufferCapacityInFrames(struct dn_aaudio_stream *stream)
{
    return dn_AAudioStream_getBufferSizeInFrames(stream);
}
static int32_t dn_AAudioStream_getFramesPerBurst(struct dn_aaudio_stream *stream)
{
    (void)stream;
    return 128;
}
static int32_t dn_AAudioStream_getState(struct dn_aaudio_stream *stream)
{
    return stream ? stream->state : 0;
}
static int32_t dn_AAudioStream_getSampleRate(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.sample_rate : 0;
}
static int32_t dn_AAudioStream_getChannelCount(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.channel_count : 0;
}
static int32_t dn_AAudioStream_getDeviceId(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.device_id : 0;
}
static int32_t dn_AAudioStream_getFormat(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.format : 0;
}
static int32_t dn_AAudioStream_getSharingMode(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.sharing_mode : 0;
}
static int32_t dn_AAudioStream_getPerformanceMode(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.performance_mode : 0;
}
static int32_t dn_AAudioStream_getDirection(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.direction : 0;
}
static int64_t dn_AAudioStream_getFramesWritten(struct dn_aaudio_stream *stream)
{
    return dn_aaudio_stream_position(stream);
}
static int64_t dn_AAudioStream_getFramesRead(struct dn_aaudio_stream *stream)
{
    return dn_aaudio_stream_position(stream);
}
static dn_aaudio_result dn_AAudioStream_getTimestamp(
    struct dn_aaudio_stream *stream, int32_t clock_id,
    int64_t *frame_position, int64_t *time_nanoseconds)
{
    (void)clock_id;
    if (!stream) return -1;
    if (frame_position) *frame_position = dn_aaudio_stream_position(stream);
    if (time_nanoseconds) *time_nanoseconds = dn_android_now_ns();
    return 0;
}
static int32_t dn_AAudioStream_getUsage(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.usage : 0;
}
static int32_t dn_AAudioStream_getContentType(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.content_type : 0;
}
static int32_t dn_AAudioStream_getInputPreset(struct dn_aaudio_stream *stream)
{
    return stream ? stream->config.input_preset : 0;
}
static int64_t dn_AAudioStream_getFramesPerCallback(struct dn_aaudio_stream *stream)
{
    (void)stream;
    return 128;
}
static const char *dn_AAudio_convertResultToText(int32_t result)
{
    return result == 0 ? "AAUDIO_OK" : "AAUDIO_ERROR_DROIDNATIVE";
}
static const char *dn_AAudio_convertStreamStateToText(int32_t state)
{
    switch (state) {
    case 2: return "AAUDIO_STREAM_STATE_OPEN";
    case 4: return "AAUDIO_STREAM_STATE_STARTED";
    case 6: return "AAUDIO_STREAM_STATE_PAUSED";
    case 8: return "AAUDIO_STREAM_STATE_FLUSHED";
    case 10: return "AAUDIO_STREAM_STATE_STOPPED";
    case 12: return "AAUDIO_STREAM_STATE_CLOSED";
    default: return "AAUDIO_STREAM_STATE_UNKNOWN";
    }
}
static bool dn_AAudio_isDataCallbackSet(struct dn_aaudio_builder *builder)
{
    return builder && builder->data_callback != NULL;
}
static bool dn_AAudioStream_isDataCallbackSet(struct dn_aaudio_stream *stream)
{
    return stream && stream->config.data_callback != NULL;
}
static int32_t dn_AAudio_getMMapPolicy(void) { return 0; }
static int32_t dn_AAudio_setMMapPolicy(int32_t policy) { (void)policy; return 0; }
static bool dn_AAudio_isMMapSupported(void) { return false; }

/* Storage manager / OBB */
struct dn_storage_manager {
    uint32_t magic;
};

static struct dn_storage_manager *dn_AStorageManager_new(void)
{
    struct dn_storage_manager *manager =
        (struct dn_storage_manager *)calloc(1, sizeof(*manager));
    if (manager)
        manager->magic = DN_ANDROID_MAGIC;
    return manager;
}
static void dn_AStorageManager_delete(struct dn_storage_manager *manager) { free(manager); }
static int dn_AStorageManager_mountObb(struct dn_storage_manager *manager,
                                       const char *filename, const char *key,
                                       void *callback, void *data)
{
    (void)manager; (void)filename; (void)key; (void)callback; (void)data;
    return 0;
}
static int dn_AStorageManager_unmountObb(struct dn_storage_manager *manager,
                                         const char *filename, int force,
                                         void *callback, void *data)
{
    (void)manager; (void)filename; (void)force; (void)callback; (void)data;
    return 0;
}
static int dn_AStorageManager_isObbMounted(struct dn_storage_manager *manager,
                                           const char *filename)
{
    (void)manager; (void)filename;
    return 0;
}
static const char *dn_AStorageManager_getMountedObbPath(
    struct dn_storage_manager *manager, const char *filename)
{
    (void)manager; (void)filename;
    return NULL;
}
static const char *dn_AObbScanner_getObbInfoFilename(const char *package_name,
                                                      const char *obb_filename)
{
    (void)package_name;
    return obb_filename;
}

/* ------------------------------------------------------------------ */
/* NativeActivity host                                                 */
/* ------------------------------------------------------------------ */

struct dn_native_activity_host {
    ANativeActivity activity;
    ANativeActivityCallbacks callbacks;
    ANativeWindow *window;
    AInputQueue *input_queue;
    char internal_path[512];
    char external_path[512];
    char obb_path[512];
    bool created;
    bool started;
    bool resumed;
    bool window_delivered;
    bool input_delivered;
    bool destroyed;
    int64_t touch_down_time;
};

dn_native_activity_host_t *dn_native_activity_host_create(int32_t width, int32_t height)
{
    dn_native_activity_host_t *host =
        (dn_native_activity_host_t *)calloc(1, sizeof(*host));
    if (!host)
        return NULL;
    host->window = dn_window_create_internal(width, height);
    host->input_queue = dn_input_queue_create_internal();
    if (!host->window || !host->input_queue) {
        dn_native_activity_host_destroy(host);
        return NULL;
    }

    pthread_mutex_lock(&dn_android_state_mutex);
    const char *package = dn_package_name ? dn_package_name : "ro.droidnative.app";
    char package_copy[128];
    strncpy(package_copy, package, sizeof(package_copy) - 1);
    package_copy[sizeof(package_copy) - 1] = '\0';
    pthread_mutex_unlock(&dn_android_state_mutex);

    snprintf(host->internal_path, sizeof(host->internal_path),
             "/data/user/0/%s", package_copy);
    snprintf(host->external_path, sizeof(host->external_path),
             "/sdcard/Android/data/%s/files", package_copy);
    snprintf(host->obb_path, sizeof(host->obb_path),
             "/sdcard/Android/obb/%s", package_copy);

    memset(&host->activity, 0, sizeof(host->activity));
    memset(&host->callbacks, 0, sizeof(host->callbacks));
    host->activity.callbacks = &host->callbacks;
    host->activity.vm = dn_jni_vm();
    host->activity.env = dn_jni_env();
    host->activity.clazz = host;
    host->activity.internalDataPath = host->internal_path;
    host->activity.externalDataPath = host->external_path;
    host->activity.sdkVersion = 35;
    host->activity.assetManager = dn_asset_manager_singleton();
    host->activity.obbPath = host->obb_path;
    return host;
}

void dn_native_activity_host_destroy(dn_native_activity_host_t *host)
{
    if (!host)
        return;
    if (host->input_delivered && host->callbacks.onInputQueueDestroyed) {
        host->callbacks.onInputQueueDestroyed(&host->activity, host->input_queue);
        host->input_delivered = false;
    }
    if (host->window_delivered && host->callbacks.onNativeWindowDestroyed) {
        host->callbacks.onNativeWindowDestroyed(&host->activity, host->window);
        host->window_delivered = false;
    }
    if (host->created && !host->destroyed && host->callbacks.onDestroy)
        host->callbacks.onDestroy(&host->activity);
    if (host->input_queue)
        dn_input_queue_destroy_internal(host->input_queue);
    if (host->window)
        dn_ANativeWindow_release(host->window);
    free(host);
}

void *dn_native_activity_host_activity(dn_native_activity_host_t *host)
{
    return host ? &host->activity : NULL;
}

void dn_native_activity_host_set_native_layer(dn_native_activity_host_t *host, void *layer)
{
    if (host && host->window)
        host->window->native_layer = layer;
}

typedef void (*dn_activity_create_fn)(ANativeActivity *, void *, size_t);

int dn_native_activity_host_dispatch_create(dn_native_activity_host_t *host,
                                            void *ANativeActivity_onCreate)
{
    if (!host || !ANativeActivity_onCreate || host->created)
        return -1;
    dn_activity_create_fn create = (dn_activity_create_fn)ANativeActivity_onCreate;
    host->created = true;
    create(&host->activity, NULL, 0);
    return 0;
}

int dn_native_activity_host_start(dn_native_activity_host_t *host)
{
    if (!host || !host->created)
        return -1;
    if (host->started)
        return 0;
    host->started = true;
    if (host->callbacks.onStart)
        host->callbacks.onStart(&host->activity);
    if (host->callbacks.onNativeWindowCreated) {
        host->callbacks.onNativeWindowCreated(&host->activity, host->window);
        host->window_delivered = true;
    }
    if (host->callbacks.onInputQueueCreated) {
        host->callbacks.onInputQueueCreated(&host->activity, host->input_queue);
        host->input_delivered = true;
    }
    return 0;
}

int dn_native_activity_host_resume(dn_native_activity_host_t *host)
{
    if (!host || !host->created)
        return -1;
    if (!host->started)
        (void)dn_native_activity_host_start(host);
    if (host->resumed)
        return 0;
    host->resumed = true;
    if (host->callbacks.onResume)
        host->callbacks.onResume(&host->activity);
    if (host->callbacks.onWindowFocusChanged)
        host->callbacks.onWindowFocusChanged(&host->activity, 1);
    return 0;
}

int dn_native_activity_host_pause(dn_native_activity_host_t *host)
{
    if (!host || !host->created)
        return -1;
    if (host->resumed && host->callbacks.onWindowFocusChanged)
        host->callbacks.onWindowFocusChanged(&host->activity, 0);
    host->resumed = false;
    if (host->callbacks.onPause)
        host->callbacks.onPause(&host->activity);
    return 0;
}

int dn_native_activity_host_stop(dn_native_activity_host_t *host)
{
    if (!host || !host->created)
        return -1;
    if (host->resumed)
        (void)dn_native_activity_host_pause(host);
    if (host->callbacks.onStop)
        host->callbacks.onStop(&host->activity);
    host->started = false;
    return 0;
}

int dn_native_activity_host_destroy_callbacks(dn_native_activity_host_t *host)
{
    if (!host || !host->created)
        return -1;
    if (host->started || host->resumed)
        (void)dn_native_activity_host_stop(host);
    if (host->input_delivered && host->callbacks.onInputQueueDestroyed) {
        host->callbacks.onInputQueueDestroyed(&host->activity, host->input_queue);
        host->input_delivered = false;
    }
    if (host->window_delivered && host->callbacks.onNativeWindowDestroyed) {
        host->callbacks.onNativeWindowDestroyed(&host->activity, host->window);
        host->window_delivered = false;
    }
    if (!host->destroyed && host->callbacks.onDestroy)
        host->callbacks.onDestroy(&host->activity);
    host->destroyed = true;
    return 0;
}

void dn_native_activity_host_resize(dn_native_activity_host_t *host,
                                    int32_t width, int32_t height)
{
    if (!host || !host->window)
        return;
    (void)dn_ANativeWindow_setBuffersGeometry(host->window, width, height, 0);
    if (host->callbacks.onNativeWindowResized)
        host->callbacks.onNativeWindowResized(&host->activity, host->window);
    if (host->callbacks.onContentRectChanged) {
        ARect rect = { 0, 0, width, height };
        host->callbacks.onContentRectChanged(&host->activity, &rect);
    }
}

void dn_native_activity_host_touch(dn_native_activity_host_t *host, int phase,
                                   int32_t pointer_id, float x, float y)
{
    if (!host || !host->input_queue)
        return;
    AInputEvent *event = (AInputEvent *)calloc(1, sizeof(*event));
    if (!event)
        return;
    event->magic = DN_ANDROID_MAGIC;
    event->type = 2;
    event->device_id = 0;
    event->source = 0x1002; /* touch screen */
    event->event_time = dn_android_now_ns();
    if (phase == 0) {
        host->touch_down_time = event->event_time;
        event->action = 0;
    } else if (phase == 1) {
        event->action = 2;
    } else if (phase == 2) {
        event->action = 1;
    } else {
        event->action = 3;
    }
    event->down_time = host->touch_down_time ? host->touch_down_time : event->event_time;
    event->x_precision = 1.0f;
    event->y_precision = 1.0f;
    event->pointer_count = 1;
    event->pointers[0].id = pointer_id;
    event->pointers[0].x = x;
    event->pointers[0].y = y;
    event->pointers[0].pressure = phase == 2 || phase == 3 ? 0.0f : 1.0f;
    event->pointers[0].size = 0.1f;
    event->pointers[0].touch_major = 12.0f;
    event->pointers[0].touch_minor = 12.0f;
    event->pointers[0].tool_major = 12.0f;
    event->pointers[0].tool_minor = 12.0f;
    (void)dn_input_queue_push_internal(host->input_queue, event);
}

uint32_t dn_native_activity_host_window_checksum(dn_native_activity_host_t *host)
{
    return host && host->window ? host->window->checksum : 0;
}

uint64_t dn_native_activity_host_frame_count(dn_native_activity_host_t *host)
{
    return host && host->window ? host->window->frame_count : 0;
}

static void dn_ANativeActivity_finish(ANativeActivity *activity)
{
    (void)activity;
}
static void dn_ANativeActivity_setWindowFormat(ANativeActivity *activity, int32_t format)
{
    dn_native_activity_host_t *host = (dn_native_activity_host_t *)activity;
    if (host && host->window)
        (void)dn_ANativeWindow_setBuffersGeometry(host->window, 0, 0, format);
}
static void dn_ANativeActivity_setWindowFlags(ANativeActivity *activity,
                                              uint32_t add_flags, uint32_t remove_flags)
{
    (void)activity; (void)add_flags; (void)remove_flags;
}
static void dn_ANativeActivity_showSoftInput(ANativeActivity *activity, uint32_t flags)
{
    (void)activity; (void)flags;
}
static void dn_ANativeActivity_hideSoftInput(ANativeActivity *activity, uint32_t flags)
{
    (void)activity; (void)flags;
}

/* JNI graphics bitmap pointers */
struct dn_android_bitmap_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    int32_t format;
    uint32_t flags;
};

static int dn_AndroidBitmap_getInfo(void *env, void *bitmap, struct dn_android_bitmap_info *info)
{
    (void)env; (void)bitmap;
    if (!info)
        return -1;
    memset(info, 0, sizeof(*info));
    return -1;
}
static int dn_AndroidBitmap_lockPixels(void *env, void *bitmap, void **pixels)
{
    (void)env; (void)bitmap;
    if (pixels) *pixels = NULL;
    return -1;
}
static int dn_AndroidBitmap_unlockPixels(void *env, void *bitmap)
{
    (void)env; (void)bitmap;
    return -1;
}
static uint32_t dn_AndroidBitmap_getDataSpace(void *env, void *bitmap)
{
    (void)env; (void)bitmap;
    return 0;
}

/* ------------------------------------------------------------------ */
/* EGL/GLES delegation to bundled ANGLE/Metal                          */
/* ------------------------------------------------------------------ */

static void *dn_angle_egl_handle;
static void *dn_angle_gles_handle;
static pthread_once_t dn_angle_once = PTHREAD_ONCE_INIT;

static void dn_angle_load_once(void)
{
#if defined(__APPLE__)
    if (!getenv("ANGLE_DEFAULT_PLATFORM"))
        setenv("ANGLE_DEFAULT_PLATFORM", "metal", 1);
    char executable[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0)
        return;
    char *slash = strrchr(executable, '/');
    if (!slash)
        return;
    *slash = '\0';
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/../Frameworks/EGL.framework/EGL", executable) < (int)sizeof(path))
        dn_angle_egl_handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (snprintf(path, sizeof(path), "%s/../Frameworks/GLESv2.framework/GLESv2", executable) < (int)sizeof(path))
        dn_angle_gles_handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
}

typedef void *(*dn_egl_create_window_surface_fn)(void *, void *, void *, const int32_t *);
typedef void *(*dn_egl_create_pbuffer_surface_fn)(void *, void *, const int32_t *);

static void *dn_egl_create_window_surface(void *display, void *config,
                                          ANativeWindow *window, const int32_t *attribs)
{
    pthread_once(&dn_angle_once, dn_angle_load_once);
    if (!dn_angle_egl_handle)
        return NULL;
    if (window && window->magic == DN_ANDROID_MAGIC && window->native_layer) {
        dn_egl_create_window_surface_fn real =
            (dn_egl_create_window_surface_fn)dlsym(dn_angle_egl_handle, "eglCreateWindowSurface");
        return real ? real(display, config, window->native_layer, attribs) : NULL;
    }
    dn_egl_create_pbuffer_surface_fn create_pbuffer =
        (dn_egl_create_pbuffer_surface_fn)dlsym(dn_angle_egl_handle, "eglCreatePbufferSurface");
    if (!create_pbuffer)
        return NULL;
    int32_t pbuffer_attribs[] = {
        12375, window ? window->width : 720,   /* EGL_WIDTH */
        12374, window ? window->height : 1280, /* EGL_HEIGHT */
        12344                                  /* EGL_NONE */
    };
    return create_pbuffer(display, config, pbuffer_attribs);
}

static void *dn_egl_get_proc_address(const char *name)
{
    return dn_native_android_lookup(name);
}

/* ------------------------------------------------------------------ */
/* Symbol lookup                                                       */
/* ------------------------------------------------------------------ */

struct dn_android_entry {
    const char *name;
    void *address;
};

static const struct dn_android_entry dn_android_entries[] = {
    { "AAssetManager_fromJava", (void *)dn_AAssetManager_fromJava },
    { "AAssetManager_open", (void *)dn_AAssetManager_open },
    { "AAssetManager_openDir", (void *)dn_AAssetManager_openDir },
    { "AAssetDir_getNextFileName", (void *)dn_AAssetDir_getNextFileName },
    { "AAssetDir_rewind", (void *)dn_AAssetDir_rewind },
    { "AAssetDir_close", (void *)dn_AAssetDir_close },
    { "AAsset_read", (void *)dn_AAsset_read },
    { "AAsset_seek", (void *)dn_AAsset_seek },
    { "AAsset_seek64", (void *)dn_AAsset_seek64 },
    { "AAsset_close", (void *)dn_AAsset_close },
    { "AAsset_getBuffer", (void *)dn_AAsset_getBuffer },
    { "AAsset_getLength", (void *)dn_AAsset_getLength },
    { "AAsset_getLength64", (void *)dn_AAsset_getLength64 },
    { "AAsset_getRemainingLength", (void *)dn_AAsset_getRemainingLength },
    { "AAsset_getRemainingLength64", (void *)dn_AAsset_getRemainingLength64 },
    { "AAsset_openFileDescriptor", (void *)dn_AAsset_openFileDescriptor },
    { "AAsset_openFileDescriptor64", (void *)dn_AAsset_openFileDescriptor64 },
    { "AAsset_isAllocated", (void *)dn_AAsset_isAllocated },

    { "AConfiguration_new", (void *)dn_AConfiguration_new },
    { "AConfiguration_delete", (void *)dn_AConfiguration_delete },
    { "AConfiguration_fromAssetManager", (void *)dn_AConfiguration_fromAssetManager },
    { "AConfiguration_copy", (void *)dn_AConfiguration_copy },
    { "AConfiguration_diff", (void *)dn_AConfiguration_diff },
    { "AConfiguration_match", (void *)dn_AConfiguration_match },
    { "AConfiguration_isBetterThan", (void *)dn_AConfiguration_isBetterThan },
    { "AConfiguration_getCountry", (void *)dn_AConfiguration_getCountry },
    { "AConfiguration_setCountry", (void *)dn_AConfiguration_setCountry },
    { "AConfiguration_getLanguage", (void *)dn_AConfiguration_getLanguage },
    { "AConfiguration_setLanguage", (void *)dn_AConfiguration_setLanguage },
    { "AConfiguration_getMcc", (void *)dn_AConfiguration_getMcc },
    { "AConfiguration_setMcc", (void *)dn_AConfiguration_setMcc },
    { "AConfiguration_getMnc", (void *)dn_AConfiguration_getMnc },
    { "AConfiguration_setMnc", (void *)dn_AConfiguration_setMnc },
    { "AConfiguration_getOrientation", (void *)dn_AConfiguration_getOrientation },
    { "AConfiguration_setOrientation", (void *)dn_AConfiguration_setOrientation },
    { "AConfiguration_getTouchscreen", (void *)dn_AConfiguration_getTouchscreen },
    { "AConfiguration_setTouchscreen", (void *)dn_AConfiguration_setTouchscreen },
    { "AConfiguration_getDensity", (void *)dn_AConfiguration_getDensity },
    { "AConfiguration_setDensity", (void *)dn_AConfiguration_setDensity },
    { "AConfiguration_getKeyboard", (void *)dn_AConfiguration_getKeyboard },
    { "AConfiguration_setKeyboard", (void *)dn_AConfiguration_setKeyboard },
    { "AConfiguration_getNavigation", (void *)dn_AConfiguration_getNavigation },
    { "AConfiguration_setNavigation", (void *)dn_AConfiguration_setNavigation },
    { "AConfiguration_getKeysHidden", (void *)dn_AConfiguration_getKeysHidden },
    { "AConfiguration_setKeysHidden", (void *)dn_AConfiguration_setKeysHidden },
    { "AConfiguration_getNavHidden", (void *)dn_AConfiguration_getNavHidden },
    { "AConfiguration_setNavHidden", (void *)dn_AConfiguration_setNavHidden },
    { "AConfiguration_getSdkVersion", (void *)dn_AConfiguration_getSdkVersion },
    { "AConfiguration_setSdkVersion", (void *)dn_AConfiguration_setSdkVersion },
    { "AConfiguration_getScreenSize", (void *)dn_AConfiguration_getScreenSize },
    { "AConfiguration_setScreenSize", (void *)dn_AConfiguration_setScreenSize },
    { "AConfiguration_getScreenLong", (void *)dn_AConfiguration_getScreenLong },
    { "AConfiguration_setScreenLong", (void *)dn_AConfiguration_setScreenLong },
    { "AConfiguration_getUiMode", (void *)dn_AConfiguration_getUiMode },
    { "AConfiguration_setUiMode", (void *)dn_AConfiguration_setUiMode },
    { "AConfiguration_getScreenWidthDp", (void *)dn_AConfiguration_getScreenWidthDp },
    { "AConfiguration_setScreenWidthDp", (void *)dn_AConfiguration_setScreenWidthDp },
    { "AConfiguration_getScreenHeightDp", (void *)dn_AConfiguration_getScreenHeightDp },
    { "AConfiguration_setScreenHeightDp", (void *)dn_AConfiguration_setScreenHeightDp },
    { "AConfiguration_getSmallestScreenWidthDp", (void *)dn_AConfiguration_getSmallestScreenWidthDp },
    { "AConfiguration_setSmallestScreenWidthDp", (void *)dn_AConfiguration_setSmallestScreenWidthDp },
    { "AConfiguration_getColorMode", (void *)dn_AConfiguration_getColorMode },
    { "AConfiguration_setColorMode", (void *)dn_AConfiguration_setColorMode },

    { "ANativeWindow_acquire", (void *)dn_ANativeWindow_acquire },
    { "ANativeWindow_release", (void *)dn_ANativeWindow_release },
    { "ANativeWindow_setBuffersGeometry", (void *)dn_ANativeWindow_setBuffersGeometry },
    { "ANativeWindow_lock", (void *)dn_ANativeWindow_lock },
    { "ANativeWindow_unlockAndPost", (void *)dn_ANativeWindow_unlockAndPost },
    { "ANativeWindow_getWidth", (void *)dn_ANativeWindow_getWidth },
    { "ANativeWindow_getHeight", (void *)dn_ANativeWindow_getHeight },
    { "ANativeWindow_getFormat", (void *)dn_ANativeWindow_getFormat },
    { "ANativeWindow_fromSurface", (void *)dn_ANativeWindow_fromSurface },
    { "ANativeWindow_setSwapInterval", (void *)dn_ANativeWindow_setSwapInterval },
    { "ANativeWindow_setAutoRefresh", (void *)dn_ANativeWindow_setAutoRefresh },
    { "ANativeWindow_setFrameRate", (void *)dn_ANativeWindow_setFrameRate },

    { "ALooper_forThread", (void *)dn_ALooper_forThread },
    { "ALooper_prepare", (void *)dn_ALooper_prepare },
    { "ALooper_wake", (void *)dn_ALooper_wake },
    { "ALooper_addFd", (void *)dn_ALooper_addFd },
    { "ALooper_removeFd", (void *)dn_ALooper_removeFd },
    { "ALooper_pollOnce", (void *)dn_ALooper_pollOnce },
    { "ALooper_pollAll", (void *)dn_ALooper_pollAll },
    { "ALooper_acquire", (void *)dn_ALooper_acquire },
    { "ALooper_release", (void *)dn_ALooper_release },
    { "AInputQueue_attachLooper", (void *)dn_AInputQueue_attachLooper },
    { "AInputQueue_detachLooper", (void *)dn_AInputQueue_detachLooper },
    { "AInputQueue_hasEvents", (void *)dn_AInputQueue_hasEvents },
    { "AInputQueue_getEvent", (void *)dn_AInputQueue_getEvent },
    { "AInputQueue_preDispatchEvent", (void *)dn_AInputQueue_preDispatchEvent },
    { "AInputQueue_finishEvent", (void *)dn_AInputQueue_finishEvent },
    { "AInputEvent_getType", (void *)dn_AInputEvent_getType },
    { "AInputEvent_getDeviceId", (void *)dn_AInputEvent_getDeviceId },
    { "AInputEvent_getSource", (void *)dn_AInputEvent_getSource },
    { "AKeyEvent_getAction", (void *)dn_AKeyEvent_getAction },
    { "AKeyEvent_getFlags", (void *)dn_AKeyEvent_getFlags },
    { "AKeyEvent_getKeyCode", (void *)dn_AKeyEvent_getKeyCode },
    { "AKeyEvent_getScanCode", (void *)dn_AKeyEvent_getScanCode },
    { "AKeyEvent_getMetaState", (void *)dn_AKeyEvent_getMetaState },
    { "AKeyEvent_getRepeatCount", (void *)dn_AKeyEvent_getRepeatCount },
    { "AKeyEvent_getDownTime", (void *)dn_AKeyEvent_getDownTime },
    { "AKeyEvent_getEventTime", (void *)dn_AKeyEvent_getEventTime },
    { "AMotionEvent_getAction", (void *)dn_AMotionEvent_getAction },
    { "AMotionEvent_getFlags", (void *)dn_AMotionEvent_getFlags },
    { "AMotionEvent_getMetaState", (void *)dn_AMotionEvent_getMetaState },
    { "AMotionEvent_getButtonState", (void *)dn_AMotionEvent_getButtonState },
    { "AMotionEvent_getEdgeFlags", (void *)dn_AMotionEvent_getEdgeFlags },
    { "AMotionEvent_getDownTime", (void *)dn_AMotionEvent_getDownTime },
    { "AMotionEvent_getEventTime", (void *)dn_AMotionEvent_getEventTime },
    { "AMotionEvent_getX", (void *)dn_AMotionEvent_getX },
    { "AMotionEvent_getY", (void *)dn_AMotionEvent_getY },
    { "AMotionEvent_getPressure", (void *)dn_AMotionEvent_getPressure },
    { "AMotionEvent_getSize", (void *)dn_AMotionEvent_getSize },
    { "AMotionEvent_getTouchMajor", (void *)dn_AMotionEvent_getTouchMajor },
    { "AMotionEvent_getTouchMinor", (void *)dn_AMotionEvent_getTouchMinor },
    { "AMotionEvent_getToolMajor", (void *)dn_AMotionEvent_getToolMajor },
    { "AMotionEvent_getToolMinor", (void *)dn_AMotionEvent_getToolMinor },
    { "AMotionEvent_getOrientation", (void *)dn_AMotionEvent_getOrientation },
    { "AMotionEvent_getPointerCount", (void *)dn_AMotionEvent_getPointerCount },
    { "AMotionEvent_getPointerId", (void *)dn_AMotionEvent_getPointerId },
    { "AMotionEvent_findPointerIndex", (void *)dn_AMotionEvent_findPointerIndex },
    { "AMotionEvent_getHistoricalEventTime", (void *)dn_AMotionEvent_getHistoricalEventTime },
    { "AMotionEvent_getHistorySize", (void *)dn_AMotionEvent_getHistorySize },
    { "AMotionEvent_getHistoricalX", (void *)dn_AMotionEvent_getHistoricalX },
    { "AMotionEvent_getHistoricalY", (void *)dn_AMotionEvent_getHistoricalY },
    { "AMotionEvent_getHistoricalPressure", (void *)dn_AMotionEvent_getHistoricalPressure },
    { "AMotionEvent_getHistoricalSize", (void *)dn_AMotionEvent_getHistoricalSize },
    { "AMotionEvent_getHistoricalTouchMajor", (void *)dn_AMotionEvent_getHistoricalTouchMajor },
    { "AMotionEvent_getHistoricalTouchMinor", (void *)dn_AMotionEvent_getHistoricalTouchMinor },
    { "AMotionEvent_getHistoricalToolMajor", (void *)dn_AMotionEvent_getHistoricalToolMajor },
    { "AMotionEvent_getHistoricalToolMinor", (void *)dn_AMotionEvent_getHistoricalToolMinor },
    { "AMotionEvent_getHistoricalOrientation", (void *)dn_AMotionEvent_getHistoricalOrientation },
    { "AMotionEvent_getAxisValue", (void *)dn_AMotionEvent_getAxisValue },
    { "AMotionEvent_getHistoricalAxisValue", (void *)dn_AMotionEvent_getHistoricalAxisValue },

    { "ASensorManager_getInstance", (void *)dn_ASensorManager_getInstance },
    { "ASensorManager_getInstanceForPackage", (void *)dn_ASensorManager_getInstanceForPackage },
    { "ASensorManager_getDefaultSensor", (void *)dn_ASensorManager_getDefaultSensor },
    { "ASensorManager_getSensorList", (void *)dn_ASensorManager_getSensorList },
    { "ASensorManager_createEventQueue", (void *)dn_ASensorManager_createEventQueue },
    { "ASensorManager_destroyEventQueue", (void *)dn_ASensorManager_destroyEventQueue },
    { "ASensorEventQueue_enableSensor", (void *)dn_ASensorEventQueue_enableSensor },
    { "ASensorEventQueue_disableSensor", (void *)dn_ASensorEventQueue_disableSensor },
    { "ASensorEventQueue_setEventRate", (void *)dn_ASensorEventQueue_setEventRate },
    { "ASensorEventQueue_hasEvents", (void *)dn_ASensorEventQueue_hasEvents },
    { "ASensorEventQueue_getEvents", (void *)dn_ASensorEventQueue_getEvents },
    { "ASensor_getName", (void *)dn_ASensor_getName },
    { "ASensor_getVendor", (void *)dn_ASensor_getVendor },
    { "ASensor_getType", (void *)dn_ASensor_getType },
    { "ASensor_getResolution", (void *)dn_ASensor_getResolution },
    { "ASensor_getMinDelay", (void *)dn_ASensor_getMinDelay },
    { "ASensor_getFifoReservedEventCount", (void *)dn_ASensor_getFifoReservedEventCount },
    { "ASensor_getFifoMaxEventCount", (void *)dn_ASensor_getFifoMaxEventCount },
    { "ASensor_getStringType", (void *)dn_ASensor_getStringType },
    { "ASensor_getReportingMode", (void *)dn_ASensor_getReportingMode },
    { "ASensor_isWakeUpSensor", (void *)dn_ASensor_isWakeUpSensor },

    { "__android_log_print", (void *)dn___android_log_print },
    { "__android_log_vprint", (void *)dn___android_log_vprint },
    { "__android_log_write", (void *)dn___android_log_write },
    { "__android_log_buf_write", (void *)dn___android_log_buf_write },
    { "__android_log_buf_print", (void *)dn___android_log_buf_print },
    { "__android_log_assert", (void *)dn___android_log_assert },
    { "android_get_device_api_level", (void *)dn_android_get_device_api_level },
    { "__system_property_get", (void *)dn___system_property_get },
    { "__system_property_find", (void *)dn___system_property_find },
    { "__system_property_read", (void *)dn___system_property_read },
    { "__system_property_read_callback", (void *)dn___system_property_read_callback },
    { "__system_property_wait", (void *)dn___system_property_wait },
    { "__system_property_set", (void *)dn___system_property_set },
    { "__system_property_foreach", (void *)dn___system_property_foreach },

    { "AAudio_createStreamBuilder", (void *)dn_AAudio_createStreamBuilder },
    { "AAudioStreamBuilder_delete", (void *)dn_AAudioStreamBuilder_delete },
    { "AAudioStreamBuilder_setDeviceId", (void *)dn_AAudioStreamBuilder_setDeviceId },
    { "AAudioStreamBuilder_getDeviceId", (void *)dn_AAudioStreamBuilder_getDeviceId },
    { "AAudioStreamBuilder_setSampleRate", (void *)dn_AAudioStreamBuilder_setSampleRate },
    { "AAudioStreamBuilder_getSampleRate", (void *)dn_AAudioStreamBuilder_getSampleRate },
    { "AAudioStreamBuilder_setChannelCount", (void *)dn_AAudioStreamBuilder_setChannelCount },
    { "AAudioStreamBuilder_getChannelCount", (void *)dn_AAudioStreamBuilder_getChannelCount },
    { "AAudioStreamBuilder_setFormat", (void *)dn_AAudioStreamBuilder_setFormat },
    { "AAudioStreamBuilder_getFormat", (void *)dn_AAudioStreamBuilder_getFormat },
    { "AAudioStreamBuilder_setDirection", (void *)dn_AAudioStreamBuilder_setDirection },
    { "AAudioStreamBuilder_getDirection", (void *)dn_AAudioStreamBuilder_getDirection },
    { "AAudioStreamBuilder_setSharingMode", (void *)dn_AAudioStreamBuilder_setSharingMode },
    { "AAudioStreamBuilder_getSharingMode", (void *)dn_AAudioStreamBuilder_getSharingMode },
    { "AAudioStreamBuilder_setPerformanceMode", (void *)dn_AAudioStreamBuilder_setPerformanceMode },
    { "AAudioStreamBuilder_getPerformanceMode", (void *)dn_AAudioStreamBuilder_getPerformanceMode },
    { "AAudioStreamBuilder_setUsage", (void *)dn_AAudioStreamBuilder_setUsage },
    { "AAudioStreamBuilder_getUsage", (void *)dn_AAudioStreamBuilder_getUsage },
    { "AAudioStreamBuilder_setContentType", (void *)dn_AAudioStreamBuilder_setContentType },
    { "AAudioStreamBuilder_getContentType", (void *)dn_AAudioStreamBuilder_getContentType },
    { "AAudioStreamBuilder_setInputPreset", (void *)dn_AAudioStreamBuilder_setInputPreset },
    { "AAudioStreamBuilder_getInputPreset", (void *)dn_AAudioStreamBuilder_getInputPreset },
    { "AAudioStreamBuilder_setBufferCapacityInFrames", (void *)dn_AAudioStreamBuilder_setBufferCapacityInFrames },
    { "AAudioStreamBuilder_getBufferCapacityInFrames", (void *)dn_AAudioStreamBuilder_getBufferCapacityInFrames },
    { "AAudioStreamBuilder_setDataCallback", (void *)dn_AAudioStreamBuilder_setDataCallback },
    { "AAudioStreamBuilder_setErrorCallback", (void *)dn_AAudioStreamBuilder_setErrorCallback },
    { "AAudioStreamBuilder_setUserData", (void *)dn_AAudioStreamBuilder_setUserData },
    { "AAudioStreamBuilder_setChannelMask", (void *)dn_AAudioStreamBuilder_setChannelMask },
    { "AAudioStreamBuilder_getChannelMask", (void *)dn_AAudioStreamBuilder_getChannelMask },
    { "AAudioStreamBuilder_setFramesPerDataCallback", (void *)dn_AAudioStreamBuilder_setFramesPerDataCallback },
    { "AAudioStreamBuilder_getFramesPerDataCallback", (void *)dn_AAudioStreamBuilder_getFramesPerDataCallback },
    { "AAudioStreamBuilder_openStream", (void *)dn_AAudioStreamBuilder_openStream },
    { "AAudioStream_close", (void *)dn_AAudioStream_close },
    { "AAudioStream_requestStart", (void *)dn_AAudioStream_requestStart },
    { "AAudioStream_requestPause", (void *)dn_AAudioStream_requestPause },
    { "AAudioStream_requestFlush", (void *)dn_AAudioStream_requestFlush },
    { "AAudioStream_requestStop", (void *)dn_AAudioStream_requestStop },
    { "AAudioStream_waitForStateChange", (void *)dn_AAudioStream_waitForStateChange },
    { "AAudioStream_read", (void *)dn_AAudioStream_read },
    { "AAudioStream_write", (void *)dn_AAudioStream_write },
    { "AAudioStream_setBufferSizeInFrames", (void *)dn_AAudioStream_setBufferSizeInFrames },
    { "AAudioStream_getBufferSizeInFrames", (void *)dn_AAudioStream_getBufferSizeInFrames },
    { "AAudioStream_getBufferCapacityInFrames", (void *)dn_AAudioStream_getBufferCapacityInFrames },
    { "AAudioStream_getFramesPerBurst", (void *)dn_AAudioStream_getFramesPerBurst },
    { "AAudioStream_getState", (void *)dn_AAudioStream_getState },
    { "AAudioStream_getSampleRate", (void *)dn_AAudioStream_getSampleRate },
    { "AAudioStream_getChannelCount", (void *)dn_AAudioStream_getChannelCount },
    { "AAudioStream_getDeviceId", (void *)dn_AAudioStream_getDeviceId },
    { "AAudioStream_getFormat", (void *)dn_AAudioStream_getFormat },
    { "AAudioStream_getSharingMode", (void *)dn_AAudioStream_getSharingMode },
    { "AAudioStream_getPerformanceMode", (void *)dn_AAudioStream_getPerformanceMode },
    { "AAudioStream_getDirection", (void *)dn_AAudioStream_getDirection },
    { "AAudioStream_getFramesWritten", (void *)dn_AAudioStream_getFramesWritten },
    { "AAudioStream_getFramesRead", (void *)dn_AAudioStream_getFramesRead },
    { "AAudioStream_getTimestamp", (void *)dn_AAudioStream_getTimestamp },
    { "AAudioStream_getUsage", (void *)dn_AAudioStream_getUsage },
    { "AAudioStream_getContentType", (void *)dn_AAudioStream_getContentType },
    { "AAudioStream_getInputPreset", (void *)dn_AAudioStream_getInputPreset },
    { "AAudioStream_getFramesPerCallback", (void *)dn_AAudioStream_getFramesPerCallback },
    { "AAudio_convertResultToText", (void *)dn_AAudio_convertResultToText },
    { "AAudio_convertStreamStateToText", (void *)dn_AAudio_convertStreamStateToText },
    { "AAudio_isDataCallbackSet", (void *)dn_AAudio_isDataCallbackSet },
    { "AAudioStream_isDataCallbackSet", (void *)dn_AAudioStream_isDataCallbackSet },
    { "AAudio_getMMapPolicy", (void *)dn_AAudio_getMMapPolicy },
    { "AAudio_setMMapPolicy", (void *)dn_AAudio_setMMapPolicy },
    { "AAudio_isMMapSupported", (void *)dn_AAudio_isMMapSupported },

    { "AStorageManager_new", (void *)dn_AStorageManager_new },
    { "AStorageManager_delete", (void *)dn_AStorageManager_delete },
    { "AStorageManager_mountObb", (void *)dn_AStorageManager_mountObb },
    { "AStorageManager_unmountObb", (void *)dn_AStorageManager_unmountObb },
    { "AStorageManager_isObbMounted", (void *)dn_AStorageManager_isObbMounted },
    { "AStorageManager_getMountedObbPath", (void *)dn_AStorageManager_getMountedObbPath },
    { "AObbScanner_getObbInfoFilename", (void *)dn_AObbScanner_getObbInfoFilename },

    { "ANativeActivity_finish", (void *)dn_ANativeActivity_finish },
    { "ANativeActivity_setWindowFormat", (void *)dn_ANativeActivity_setWindowFormat },
    { "ANativeActivity_setWindowFlags", (void *)dn_ANativeActivity_setWindowFlags },
    { "ANativeActivity_showSoftInput", (void *)dn_ANativeActivity_showSoftInput },
    { "ANativeActivity_hideSoftInput", (void *)dn_ANativeActivity_hideSoftInput },
    { "AndroidBitmap_getInfo", (void *)dn_AndroidBitmap_getInfo },
    { "AndroidBitmap_lockPixels", (void *)dn_AndroidBitmap_lockPixels },
    { "AndroidBitmap_unlockPixels", (void *)dn_AndroidBitmap_unlockPixels },
    { "AndroidBitmap_getDataSpace", (void *)dn_AndroidBitmap_getDataSpace },
    { "JNI_GetCreatedJavaVMs", (void *)dn_JNI_GetCreatedJavaVMs },
    { "eglCreateWindowSurface", (void *)dn_egl_create_window_surface },
    { "eglGetProcAddress", (void *)dn_egl_get_proc_address }
};

void *dn_native_android_lookup(const char *name)
{
    if (!name)
        return NULL;
    const size_t count = sizeof(dn_android_entries) / sizeof(dn_android_entries[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(name, dn_android_entries[i].name) == 0)
            return dn_android_entries[i].address;
    }
    if (strncmp(name, "egl", 3) == 0) {
        pthread_once(&dn_angle_once, dn_angle_load_once);
        return dn_angle_egl_handle ? dlsym(dn_angle_egl_handle, name) : NULL;
    }
    if (strncmp(name, "gl", 2) == 0) {
        pthread_once(&dn_angle_once, dn_angle_load_once);
        return dn_angle_gles_handle ? dlsym(dn_angle_gles_handle, name) : NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* libandroid self-test                                                */
/* ------------------------------------------------------------------ */

static void dn_android_append(char *out, size_t out_len, const char *fmt, ...)
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

static AInputQueue *dn_android_selftest_queue;
static float dn_android_selftest_event_x;
static int dn_android_selftest_event_action;

static int dn_android_selftest_input_callback(int fd, int events, void *data)
{
    (void)fd; (void)events; (void)data;
    AInputEvent *event = NULL;
    if (dn_AInputQueue_getEvent(dn_android_selftest_queue, &event) == 0 && event) {
        dn_android_selftest_event_x = dn_AMotionEvent_getX(event, 0);
        dn_android_selftest_event_action = dn_AMotionEvent_getAction(event);
        dn_AInputQueue_finishEvent(dn_android_selftest_queue, event, 1);
    }
    return 1;
}

static int dn_android_selftest_activity_state;
static void dn_android_selftest_on_start(ANativeActivity *activity) { (void)activity; dn_android_selftest_activity_state |= 1; }
static void dn_android_selftest_on_resume(ANativeActivity *activity) { (void)activity; dn_android_selftest_activity_state |= 2; }
static void dn_android_selftest_on_window(ANativeActivity *activity, ANativeWindow *window)
{
    (void)activity;
    ANativeWindow_Buffer buffer;
    if (window && dn_ANativeWindow_lock(window, &buffer, NULL) == 0) {
        uint32_t *pixels = (uint32_t *)buffer.bits;
        if (pixels && buffer.width > 2 && buffer.height > 2)
            pixels[buffer.stride + 1] = 0xff336699u;
        (void)dn_ANativeWindow_unlockAndPost(window);
    }
    dn_android_selftest_activity_state |= 4;
}
static void dn_android_selftest_on_input(ANativeActivity *activity, AInputQueue *queue)
{
    (void)activity; (void)queue;
    dn_android_selftest_activity_state |= 8;
}
static void dn_android_selftest_on_destroy(ANativeActivity *activity)
{
    (void)activity;
    dn_android_selftest_activity_state |= 16;
}
static void dn_android_selftest_on_create(ANativeActivity *activity, void *saved, size_t size)
{
    (void)saved; (void)size;
    activity->callbacks->onStart = dn_android_selftest_on_start;
    activity->callbacks->onResume = dn_android_selftest_on_resume;
    activity->callbacks->onNativeWindowCreated = dn_android_selftest_on_window;
    activity->callbacks->onInputQueueCreated = dn_android_selftest_on_input;
    activity->callbacks->onDestroy = dn_android_selftest_on_destroy;
}

int dn_native_android_run_selftest(char *out, size_t out_len)
{
    if (out && out_len)
        out[0] = '\0';
    int failures = 0;

    /* Create the probe through the same resolver AAssetManager_open
     * uses below.  The Swift harness installs an explicit asset root
     * that is a sibling of the VFS files root, so translating the
     * guest /android_asset path through the VFS would write the file
     * under <files-root>/assets while the manager reads it from the
     * installed asset root - a mismatch the old host harness masked by
     * pointing both roots at the same directory. */
    AAssetManager *asset_manager = dn_asset_manager_singleton();
    char asset_host_path[DN_PATH_CAP] = "";
    bool asset_ok = asset_manager &&
                    dn_asset_path(asset_manager, "dn-android-selftest.txt",
                                  asset_host_path, sizeof(asset_host_path));
    if (asset_ok) {
        char *slash = strrchr(asset_host_path, '/');
        if (slash) {
            *slash = '\0';
            (void)mkdir(asset_host_path, 0755);
            *slash = '/';
        }
        FILE *file = fopen(asset_host_path, "wb");
        if (file) {
            fputs("asset42", file);
            fclose(file);
        } else {
            asset_ok = false;
        }
    }
    AAsset *asset = asset_ok ? dn_AAssetManager_open(asset_manager,
                                                     "dn-android-selftest.txt", 0) : NULL;
    if (asset) {
        char bytes[16] = "";
        int got = dn_AAsset_read(asset, bytes, 7);
        asset_ok = got == 7 && memcmp(bytes, "asset42", 7) == 0 &&
                   dn_AAsset_getLength64(asset) == 7;
        dn_AAsset_close(asset);
    } else {
        asset_ok = false;
    }
    dn_android_append(out, out_len, "AAssetManager file assets: %s", asset_ok ? "OK" : "MISMATCH");
    failures += asset_ok ? 0 : 1;

    AConfiguration *config = dn_AConfiguration_new();
    bool config_ok = config && dn_AConfiguration_getSdkVersion(config) == 35 &&
                     dn_AConfiguration_getDensity(config) == 560 &&
                     dn_AConfiguration_getOrientation(config) == 1;
    dn_AConfiguration_delete(config);
    dn_android_append(out, out_len, "AConfiguration: %s", config_ok ? "OK" : "MISMATCH");
    failures += config_ok ? 0 : 1;

    ANativeWindow *window = dn_window_create_internal(4, 3);
    bool window_ok = false;
    if (window) {
        ANativeWindow_Buffer buffer;
        if (dn_ANativeWindow_lock(window, &buffer, NULL) == 0) {
            uint32_t *pixels = (uint32_t *)buffer.bits;
            for (int i = 0; i < 12; ++i)
                pixels[i] = 0xff000000u | (uint32_t)i;
            window_ok = dn_ANativeWindow_unlockAndPost(window) == 0 &&
                        window->frame_count == 1 && window->checksum != 0;
        }
        dn_ANativeWindow_release(window);
    }
    dn_android_append(out, out_len, "ANativeWindow software buffer: %s", window_ok ? "OK" : "MISMATCH");
    failures += window_ok ? 0 : 1;

    ALooper *looper = dn_ALooper_prepare(0);
    AInputQueue *queue = dn_input_queue_create_internal();
    dn_android_selftest_queue = queue;
    dn_android_selftest_event_x = 0;
    dn_android_selftest_event_action = -1;
    bool input_ok = looper && queue;
    if (input_ok) {
        dn_AInputQueue_attachLooper(queue, looper, 42,
                                    dn_android_selftest_input_callback, NULL);
        AInputEvent *event = (AInputEvent *)calloc(1, sizeof(*event));
        event->magic = DN_ANDROID_MAGIC;
        event->type = 2;
        event->source = 0x1002;
        event->action = 0;
        event->pointer_count = 1;
        event->pointers[0].x = 123.5f;
        event->pointers[0].y = 456.0f;
        (void)dn_input_queue_push_internal(queue, event);
        int out_fd = -1, out_events = 0;
        void *out_data = NULL;
        int poll_result = dn_ALooper_pollAll(100, &out_fd, &out_events, &out_data);
        (void)poll_result;
        input_ok = dn_android_selftest_event_action == 0 &&
                   dn_android_selftest_event_x > 123.0f &&
                   dn_android_selftest_event_x < 124.0f;
        dn_AInputQueue_detachLooper(queue);
    }
    dn_input_queue_destroy_internal(queue);
    dn_android_selftest_queue = NULL;
    dn_android_append(out, out_len, "ALooper/AInputQueue direct touch: %s", input_ok ? "OK" : "MISMATCH");
    failures += input_ok ? 0 : 1;

    void **vm_table = *(void ***)dn_jni_vm();
    typedef int32_t (*attach_fn)(void *, void **, void *);
    void *env = NULL;
    attach_fn attach = (attach_fn)vm_table[4];
    bool jni_ok = attach && attach(dn_jni_vm(), &env, NULL) == 0 && env;
    if (jni_ok) {
        void **env_table = *(void ***)env;
        typedef void *(*find_class_fn)(void *, const char *);
        typedef void *(*new_string_fn)(void *, const char *);
        typedef const char *(*get_utf_fn)(void *, void *, uint8_t *);
        find_class_fn find_class = (find_class_fn)env_table[6];
        new_string_fn new_string = (new_string_fn)env_table[167];
        get_utf_fn get_utf = (get_utf_fn)env_table[169];
        void *clazz = find_class(env, "android/app/NativeActivity");
        void *string = new_string(env, "jni42");
        const char *utf = string ? get_utf(env, string, NULL) : NULL;
        jni_ok = clazz && utf && strcmp(utf, "jni42") == 0;
    }
    dn_android_append(out, out_len, "local JNI object bridge: %s", jni_ok ? "OK" : "MISMATCH");
    failures += jni_ok ? 0 : 1;

    struct dn_log_values values;
    memset(&values, 0, sizeof(values));
    values.gp[0] = 42;
    values.gp[1] = (uintptr_t)"native";
    char formatted[128] = "";
    dn_android_log_format_to_buffer("value=%d text=%s", &values, formatted, sizeof(formatted));
    bool log_ok = strcmp(formatted, "value=42 text=native") == 0;
    /* Android's property-get buffer contract is 92 bytes.  Check that
     * reading a short value changes only the bytes of the value and
     * its terminator: an earlier strncpy() zero-padded the other 89
     * bytes, overwriting a caller's saved LR when it had supplied a
     * too-small buffer. The full fixture now allocates all 92 bytes. */
    struct {
        char value[92];
        uint8_t canary[16];
    } property_probe;
    memset(&property_probe, 0xa5, sizeof(property_probe));
    bool property_ok =
        dn___system_property_get("ro.build.version.sdk", property_probe.value) == 2 &&
        memcmp(property_probe.value, "35", 3) == 0;
    for (size_t i = 3; i < sizeof(property_probe.value); ++i)
        property_ok = property_ok && (uint8_t)property_probe.value[i] == 0xa5;
    for (size_t i = 0; i < sizeof(property_probe.canary); ++i)
        property_ok = property_ok && property_probe.canary[i] == 0xa5;
    dn_android_append(out, out_len, "android log formatter/properties: %s",
                      log_ok && property_ok ? "OK" : "MISMATCH");
    failures += log_ok && property_ok ? 0 : 1;

    struct dn_aaudio_builder *builder = NULL;
    struct dn_aaudio_stream *stream = NULL;
    bool audio_ok = dn_AAudio_createStreamBuilder(&builder) == 0 && builder;
    if (audio_ok) {
        dn_AAudioStreamBuilder_setSampleRate(builder, 48000);
        dn_AAudioStreamBuilder_setChannelCount(builder, 2);
        audio_ok = dn_AAudioStreamBuilder_openStream(builder, &stream) == 0 && stream;
    }
    if (audio_ok) {
        int16_t samples[256] = { 0 };
        audio_ok = dn_AAudioStream_requestStart(stream) == 0 &&
                   dn_AAudioStream_write(stream, samples, 128, 0) == 128 &&
                   dn_AAudioStream_read(stream, samples, 128, 0) == 128;
        (void)dn_AAudioStream_close(stream);
    }
    (void)dn_AAudioStreamBuilder_delete(builder);
    dn_android_append(out, out_len, "AAudio clocked stream: %s", audio_ok ? "OK" : "MISMATCH");
    failures += audio_ok ? 0 : 1;

    dn_android_selftest_activity_state = 0;
    dn_native_activity_host_t *host = dn_native_activity_host_create(8, 8);
    bool activity_ok = host &&
        dn_native_activity_host_dispatch_create(host, (void *)dn_android_selftest_on_create) == 0 &&
        dn_native_activity_host_start(host) == 0 &&
        dn_native_activity_host_resume(host) == 0;
    if (host)
        dn_native_activity_host_touch(host, 0, 0, 4.0f, 4.0f);
    if (host)
        (void)dn_native_activity_host_destroy_callbacks(host);
    activity_ok = activity_ok && dn_android_selftest_activity_state == 31 &&
                  dn_native_activity_host_frame_count(host) == 1;
    dn_native_activity_host_destroy(host);
    dn_android_append(out, out_len, "NativeActivity lifecycle host: %s", activity_ok ? "OK" : "MISMATCH");
    failures += activity_ok ? 0 : 1;

    dn_android_append(out, out_len, "%s", failures == 0 ? "host verdict: PASS" : "host verdict: FAIL");
    return failures == 0 ? 0 : -1;
}
