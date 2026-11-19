#ifndef DN_NATIVE_ANDROID_H
#define DN_NATIVE_ANDROID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct AAssetDir AAssetDir;
typedef struct ANativeWindow ANativeWindow;
typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;
typedef struct ALooper ALooper;
typedef struct AConfiguration AConfiguration;
typedef struct ASensorManager ASensorManager;
typedef struct ASensor ASensor;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct ANativeActivity ANativeActivity;

typedef struct ARect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} ARect;

typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity *activity);
    void (*onResume)(ANativeActivity *activity);
    void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *outSize);
    void (*onPause)(ANativeActivity *activity);
    void (*onStop)(ANativeActivity *activity);
    void (*onDestroy)(ANativeActivity *activity);
    void (*onWindowFocusChanged)(ANativeActivity *activity, int hasFocus);
    void (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowDestroyed)(ANativeActivity *activity, ANativeWindow *window);
    void (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
    void (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *queue);
    void (*onContentRectChanged)(ANativeActivity *activity, const ARect *rect);
    void (*onConfigurationChanged)(ANativeActivity *activity);
    void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    void *vm;
    void *env;
    void *clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;
    AAssetManager *assetManager;
    const char *obbPath;
};

typedef struct ANativeWindow_Buffer {
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
    void *bits;
    uint32_t reserved[6];
} ANativeWindow_Buffer;

typedef struct dn_native_activity_host dn_native_activity_host_t;
typedef void (*dn_native_window_frame_fn)(void *user, const void *pixels,
                                          int32_t width, int32_t height,
                                          int32_t stride_pixels, int32_t format);

void *dn_native_android_lookup(const char *name);
int dn_native_android_run_selftest(char *out, size_t out_len);
void dn_native_set_asset_directory(const char *path);
void dn_native_set_package_name(const char *package_name);
void dn_native_set_window_frame_callback(dn_native_window_frame_fn callback, void *user);

dn_native_activity_host_t *dn_native_activity_host_create(int32_t width, int32_t height);
void dn_native_activity_host_destroy(dn_native_activity_host_t *host);
void *dn_native_activity_host_activity(dn_native_activity_host_t *host);
void dn_native_activity_host_set_native_layer(dn_native_activity_host_t *host, void *layer);
int dn_native_activity_host_dispatch_create(dn_native_activity_host_t *host,
                                            void *ANativeActivity_onCreate);
int dn_native_activity_host_start(dn_native_activity_host_t *host);
int dn_native_activity_host_resume(dn_native_activity_host_t *host);
int dn_native_activity_host_pause(dn_native_activity_host_t *host);
int dn_native_activity_host_stop(dn_native_activity_host_t *host);
int dn_native_activity_host_destroy_callbacks(dn_native_activity_host_t *host);
void dn_native_activity_host_resize(dn_native_activity_host_t *host,
                                    int32_t width, int32_t height);
void dn_native_activity_host_touch(dn_native_activity_host_t *host, int phase,
                                   int32_t pointer_id, float x, float y);
uint32_t dn_native_activity_host_window_checksum(dn_native_activity_host_t *host);
uint64_t dn_native_activity_host_frame_count(dn_native_activity_host_t *host);

#ifdef __cplusplus
}
#endif

#endif /* DN_NATIVE_ANDROID_H */
