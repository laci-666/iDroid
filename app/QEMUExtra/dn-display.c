/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DroidNative DisplayChangeListener bridge, compiled into the QEMU library.
 * See dn-display.h. Uses only QEMU's public UI API (the same entry points
 * ui/cocoa.m and ui/gtk.c are built on).
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#ifdef CONFIG_OPENGL
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#include "ui/shader.h"
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h> /* CFRetain pe MTLTexture (0.2.24) */
#endif

#include "dn-display.h"

/* Sparse logging to stderr (the app redirects it into qemu.log). Anything
 * per-frame belongs behind the modulo below: this code runs under the BQL. */
#define DN_DLOG(fmt, ...) \
    do { fprintf(stderr, "[dn-dpy] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

typedef struct DNDisplayState {
    DisplayChangeListener dcl;
    QemuMutex lock;
    DisplaySurface *surface;
    uint64_t generation;
    uint64_t sequence;
    /* Coalesced damage (bounding union) since the last take; guarded
     * by lock.  At native phone resolution the scanout is ~15 MB, so
     * uploading only the union is the difference between a full-frame
     * CPU->GPU copy and a few damaged rows on mostly-static screens. */
    int32_t damage_x0;
    int32_t damage_y0;
    int32_t damage_x1;
    int32_t damage_y1;
    bool damage_valid;
    bool inited;
} DNDisplayState;

static DNDisplayState dn;

static void dn_damage_union_locked(int x, int y, int w, int h)
{
    if (!dn.surface || w <= 0 || h <= 0) {
        return;
    }
    /* A guest-controlled rectangle may reach INT_MAX.  Compute its
     * end points in 64 bits and clip *before* storing in int32_t:
     * signed x+w overflow would otherwise be undefined behavior. */
    int64_t x0 = x < 0 ? 0 : x;
    int64_t y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + w;
    int64_t y1 = (int64_t)y + h;
    int32_t surface_w = surface_width(dn.surface);
    int32_t surface_h = surface_height(dn.surface);
    if (x1 > surface_w) x1 = surface_w;
    if (y1 > surface_h) y1 = surface_h;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (!dn.damage_valid) {
        dn.damage_x0 = (int32_t)x0;
        dn.damage_y0 = (int32_t)y0;
        dn.damage_x1 = (int32_t)x1;
        dn.damage_y1 = (int32_t)y1;
        dn.damage_valid = true;
        return;
    }
    if (x0 < dn.damage_x0) dn.damage_x0 = (int32_t)x0;
    if (y0 < dn.damage_y0) dn.damage_y0 = (int32_t)y0;
    if (x1 > dn.damage_x1) dn.damage_x1 = (int32_t)x1;
    if (y1 > dn.damage_y1) dn.damage_y1 = (int32_t)y1;
}

/* ---- DisplayChangeListener callbacks (main loop thread, BQL held) ---- */

static void dn_dpy_gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    (void)dcl;
    qemu_mutex_lock(&dn.lock);
    dn_damage_union_locked(x, y, w, h);
    qemu_mutex_unlock(&dn.lock);
    uint64_t seq = qatomic_fetch_inc(&dn.sequence) + 1;
    if (seq <= 5 || (seq % 600) == 0) {
        DN_DLOG("gfx_update #%llu", (unsigned long long)seq);
    }
}

static void dn_dpy_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *new_surface)
{
    (void)dcl;
    qemu_mutex_lock(&dn.lock);
    dn.surface = new_surface;
    dn.generation++;
    dn.damage_valid = false;
    if (new_surface) {
        dn_damage_union_locked(0, 0, surface_width(new_surface),
                               surface_height(new_surface));
    }
    /* Surface replacement itself is a new frame. The guest can switch
     * scanout and wait before gfx_update; wake the event-driven Metal
     * presenter now rather than leaving a stale texture on screen. */
    qatomic_fetch_inc(&dn.sequence);
    uint64_t gen = dn.generation;
    qemu_mutex_unlock(&dn.lock);
    if (gen <= 5 || (gen % 600) == 0) {
        DN_DLOG("gfx_switch gen=%llu %dx%d", (unsigned long long)gen,
                new_surface ? surface_width(new_surface) : 0,
                new_surface ? surface_height(new_surface) : 0);
    }
}

static bool dn_dpy_gfx_check_format(DisplayChangeListener *dcl,
                                    pixman_format_code_t format)
{
    (void)dcl;
    /* BGRA in memory on little-endian ARM -- what MTLPixelFormat.bgra8Unorm
     * consumes directly, and what virtio-gpu hands us anyway. */
    return format == PIXMAN_x8r8g8b8 || format == PIXMAN_a8r8g8b8;
}

#ifdef CONFIG_OPENGL
static void dn_gl_drain_pending_readback(void);
static void dn_gl_io_drain_pending(void);
#endif

static void dn_dpy_refresh(DisplayChangeListener *dcl)
{
    /* With -display none there is no UI backend ticking the console; our
     * refresh callback is what makes virtio-gpu flip scanouts to us. */
    graphic_hw_update(dcl->con);
#ifdef CONFIG_OPENGL
    /* The QEMU GUI refresh timer already fires every 8 ms. Drain a ready
     * PBO even when Android does NOT submit another frame: the old double
     * buffer could leave a one-shot touch frame undisplayed indefinitely.
     * No extra timer, thread, busy-wait or BQL-held GL fence wait. */
    dn_gl_drain_pending_readback();
    dn_gl_io_drain_pending(); /* completed EAGL -> IOSurface fences, no CPU copy */
#endif
}

static const DisplayChangeListenerOps dn_dcl_ops = {
    .dpy_name             = "droidnative",
    .dpy_refresh          = dn_dpy_refresh,
    .dpy_gfx_update       = dn_dpy_gfx_update,
    .dpy_gfx_switch       = dn_dpy_gfx_switch,
    .dpy_gfx_check_format = dn_dpy_gfx_check_format,
};

/* ---- GL scanout (Faza B, docs/12): virtio-gpu-gl + virgl + ANGLE ------
 * Mirrors ui/egl-headless.c: a second listener receives GL scanout
 * textures, blits + reads them back into the console's 2D surface, then
 * calls dpy_gfx_update() so the raster listener above (and thus the app)
 * sees ordinary surface frames. Active only when dnq_gl_early_init()
 * ran before qemu_init (the app sets env DN_GL and the shim calls it);
 * otherwise nothing GL is registered and the 2D path is untouched. */

static bool dn_gl_ready;

/* ui/console.c; virtio-gpu-gl refuza realizarea cat e 0 („The display
 * backend does not have OpenGL support"). egl_init() upstream il seteaza
 * at the end; the cocoa path (used by us) does NOT touch it — that is why we
 * set it here, after successful init (Log 16: frozen black screen with GL on). */
extern int display_opengl;

bool dnq_gl_early_init(void)
{
#ifdef CONFIG_OPENGL
    if (dn_gl_ready) {
        return true;
    }
    /*
     * DISPLAY_GL_MODE_ES, nu ON: qemu_egl_init_dpy leaga API-ul desktop
     * (EGL_OPENGL_API + core config) for any mode != ES, and
     * ANGLE knows only OpenGL ES — the same fix the win32 path makes
     * face explicit („prefer GL ES, as that's what ANGLE supports").
     * Cu ON, init-ul esua si virtio-gpu-gl murea in qemu_init.
     */
    /* Pinned virglrenderer-451505ad raises the GLOBAL log level to DEBUG
     * whenever VREND_DEBUG is nonempty. The old default gles/feat/bgra/tweak
     * was a bring-up diagnostic, not a renderer optimization. Do not turn
     * on DEBUG work for every compositor frame; a developer can still set
     * VREND_DEBUG explicitly. Never enable 'khr' (ES>3 epoxy abort). */
    const char *vrend_debug = getenv("VREND_DEBUG");
    DN_DLOG("gl early init: VREND_DEBUG=%s", vrend_debug ? vrend_debug : "(off)");
    if (qemu_egl_init_dpy_cocoa(DISPLAY_GL_MODE_ES) < 0) {
        DN_DLOG("gl early init FAILED: qemu_egl_init_dpy_cocoa(ES)");
        return false;
    }
    /* For this Lineage VM, ANGLE Metal is a KNOWN black-UI backend in
     * the upstream UTM guide. If unset ANGLE_DEFAULT_PLATFORM somehow
     * still selected Metal, refuse the trial instead of reproducing the
     * black display on even our isolated copy. EAGL has no Metal device. */
    if (qemu_egl_angle_native_device != NULL) {
        DN_DLOG("gl early init FAILED: ANGLE selected Metal despite the OpenGL/EAGL request");
        return false;
    }
    /* QEMU's generic qemu_egl_init_ctx() requests ES2. Our presentation
     * shader uses #version 300 es and PBOs; SurfaceFlinger/VirGL also needs
     * ES3. Demand an ES3 context here, rather than silently allowing an
     * ES2 GL listener that can only draw a black screen. */
    const EGLint dn_ctx_es3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    qemu_egl_rn_ctx = eglCreateContext(qemu_egl_display, qemu_egl_config,
                                       EGL_NO_CONTEXT, dn_ctx_es3);
    if (qemu_egl_rn_ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, qemu_egl_rn_ctx)) {
        DN_DLOG("gl early init FAILED: ANGLE OpenGL/EAGL needs a surfaceless ES3 context (EGL error 0x%x)",
                eglGetError());
        if (qemu_egl_rn_ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(qemu_egl_display, qemu_egl_rn_ctx);
            qemu_egl_rn_ctx = EGL_NO_CONTEXT;
        }
        return false;
    }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    if (!version || strncmp(version, "OpenGL ES 3.", 12) != 0) {
        DN_DLOG("gl early init FAILED: ES3 required; renderer=%s version=%s",
                renderer ? renderer : "unknown", version ? version : "unknown");
        eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(qemu_egl_display, qemu_egl_rn_ctx);
        qemu_egl_rn_ctx = EGL_NO_CONTEXT;
        return false;
    }
    display_opengl = 1;
    dn_gl_ready = true;
    DN_DLOG("gl early init OK: ANGLE OpenGL/EAGL EGL=%s GL=%s renderer=%s",
            eglQueryString(qemu_egl_display, EGL_VERSION), version,
            renderer ? renderer : "unknown");
    return true;
#else
    DN_DLOG("gl early init: QEMU built without OpenGL/EGL");
    return false;
#endif
}

#ifdef CONFIG_OPENGL

#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1 /* EXT_texture_format_BGRA8888 */
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

/* A dirty rectangle belongs to the PBO frame that was QUEUED, not the
 * current dpy_gl_update(). Pixel readback is still full-frame; this only
 * reduces QEMU->MTKView uploads when guest damage/backing is trustworthy. */
typedef struct DNGLDamage {
    int x, y, w, h;
    bool valid;
} DNGLDamage;

#ifdef __APPLE__
/* EGL_ANGLE_iosurface_client_buffer from the pinned UTM ANGLE/EAGL build.
 * libepoxy's EGL headers may predate these enums; keep values from the
 * extension spec rather than depending on an unpinned SDK header. */
#ifndef EGL_IOSURFACE_ANGLE
#define EGL_IOSURFACE_ANGLE 0x3454
#endif
#ifndef EGL_IOSURFACE_PLANE_ANGLE
#define EGL_IOSURFACE_PLANE_ANGLE 0x345A
#endif
#ifndef EGL_TEXTURE_TYPE_ANGLE
#define EGL_TEXTURE_TYPE_ANGLE 0x345C
#endif
#ifndef EGL_TEXTURE_INTERNAL_FORMAT_ANGLE
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#endif
#ifndef EGL_BIND_TO_TEXTURE_TARGET_ANGLE
#define EGL_BIND_TO_TEXTURE_TARGET_ANGLE 0x348D
#endif
#define DN_IO_SLOTS 4

typedef struct DNIOSlot {
    void *surface;                /* QEMU owns +1 CF reference */
    EGLSurface pbuffer;
    GLuint texture, fbo;         /* GL objects, QEMU main loop only */
    GLsync fence;                /* still-writing GLES GPU; never expose yet */
    uint64_t ticket;
    unsigned metal_holds;        /* guarded by DNIOPool.lock */
    bool pending, base_released;
} DNIOSlot;

typedef struct DNIOPool {
    QemuMutex lock;              /* latest, metal_holds, disabled */
    bool lock_inited;
    bool ready;                  /* qatomic: lock/slots initialized */
    bool disabled, active;
    int width, height, latest;
    uint64_t ticket, latest_ticket, min_valid_ticket;
    uint64_t published, skipped;
    DNIOSlot slot[DN_IO_SLOTS];
} DNIOPool;
#endif

typedef struct DNGLState {
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    egl_fb guest_fb;
    egl_fb blit_fb;
    bool blit_is_rgba;
    bool y_0_top;
#ifdef __APPLE__
    DNIOPool io;                 /* optional EAGL->IOSurface->Metal, else PBO */
#endif
    uint64_t flushes;
    uint64_t dropped;
    uint64_t scanouts;
    GLuint blit_prog;
    GLuint blit_vao;
    GLuint blit_sampler;  /* ES3 sampler: don't mutate SurfaceFlinger's texture state */
    GLint blit_uflip;
    bool blit_prog_tried;
    /* Double-buffer PBO async readback (0.2.20, see
     * dn_gl_read_pbo): elimina blocajul glReadPixels sub BQL. */
    GLuint pbo[2];
    int pbo_write;
    int pbo_w, pbo_h;
    bool pbo_has_prev;
    bool pbo_pending;         /* newest frame to publish on QEMU's 8 ms refresh */
    int pbo_pending_index;
    GLsync pbo_fence;         /* nonblocking readiness check, GLES3 */
    bool pbo_failed;
    bool pbo_logged;
    DNGLDamage accumulated_damage;  /* union incl. frames skipped by 8ms coalescer */
    DNGLDamage pbo_damage[2];       /* frame metadata travels with its PBO */
    bool force_full_damage;         /* unknown rect/backing/modeset => full */
    bool surface_frame_valid;       /* no subrect until a complete image is published */
    uint64_t published_full, published_partial, published_area;
    /* Readback is performed under QEMU's big lock: record the actual
     * cost before attributing a slow finger to Android/SurfaceFlinger. */
    uint64_t readback_total_ns;
    uint64_t readback_max_ns;
    unsigned readback_samples;
    uint64_t blit_total_ns, blit_max_ns;
    unsigned blit_samples;
    uint64_t pbo_submit_total_ns, pbo_map_total_ns, pbo_fence_total_ns;
    unsigned pbo_submit_samples, pbo_map_samples, pbo_fence_samples;
    uint64_t drain_total_ns;
    uint64_t drain_max_ns;
    unsigned drain_samples;
    uint32_t last_backing_id;
    int64_t last_readback_ns;
    unsigned gl_errors_logged;
    /* Sonde de diagnostic faza-2-neagra (0.2.21): tranzitii de erori
     * GL (without the old blind cap of 10), direct read from the guest FBO
     * (bypasses blit+PBO) and the texture completeness probe. */
    GLenum err_last_entry, err_last_blit, err_last_read;
    uint32_t probe_buf[64 * 64];
    int probe_nz;
    uint32_t probe_center;
    GLenum probe_err;
    uint32_t probe_last_backing;
    uint32_t probe_ids[4];   /* 0.2.28: already-probed backing IDs (anti-spam) */
    unsigned probe_ids_i;
    unsigned ctx_creates;
    /* Direct Metal presentation (0.2.24, Husk/UTM method): the handle
     * MTLTexture al scanout-ului curent, retinut de noi pana la
     * replacement; the app draws it itself (no readback).
     * Swift reads from another thread => its own mutex. */
    QemuMutex mtl_lock;
    bool mtl_lock_inited;
    void *mtl_tex;
    int mtl_w, mtl_h;
    bool mtl_y0top;
    uint64_t mtl_gen;
    uint64_t mtl_seq;        /* 0.2.28: metal flush sequence (host latch key) */
    void *mtl_last_handle;   /* 0.2.28: last native handle (change log) */
    void *mtl_handles_seen[4]; /* 0.2.29: already-logged handles (anti-spam) */
    unsigned mtl_handles_i;
    int mtl_last_type;
    bool inited;
} DNGLState;

/* Forward declarations: the Metal helpers are defined further down,
 * but dn_gl_scanout_disable (above) already calls dn_gl_mtl_set. */
static void dn_gl_mtl_ensure_lock(DNGLState *gl);
static void dn_gl_mtl_set(DNGLState *gl, void *tex, int w, int h, bool y0top);

static DNGLState dn_gl;

#ifdef __APPLE__
static void dn_gl_io_invalidate(DNGLState *gl);
static bool dn_gl_io_draw(DNGLState *gl);
static void dn_gl_io_shutdown(DNGLState *gl);
#else
static void dn_gl_io_drain_pending(void) { }
#endif

static DNGLDamage dn_gl_full_damage(DNGLState *gl)
{
    return (DNGLDamage){ 0, 0, surface_width(gl->ds), surface_height(gl->ds), true };
}

/* The 8ms rate cap may discard several guest updates. Accumulate EVERY
 * rectangle before the cap, and fall back to fullscreen for malformed or
 * out-of-bounds data (including overflow in guest-supplied coordinates). */
static void dn_gl_damage_merge(DNGLState *gl, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
    if (!gl->ds || surface_width(gl->ds) <= 0 || surface_height(gl->ds) <= 0 ||
        w == 0 || h == 0 ||
        x >= (uint32_t)surface_width(gl->ds) ||
        y >= (uint32_t)surface_height(gl->ds) ||
        (uint64_t)x + w > (uint32_t)surface_width(gl->ds) ||
        (uint64_t)y + h > (uint32_t)surface_height(gl->ds)) {
        gl->force_full_damage = true;
        return;
    }
    DNGLDamage *d = &gl->accumulated_damage;
    if (!d->valid) {
        *d = (DNGLDamage){ (int)x, (int)y, (int)w, (int)h, true };
        return;
    }
    int64_t x0 = MIN((int64_t)d->x, (int64_t)x);
    int64_t y0 = MIN((int64_t)d->y, (int64_t)y);
    int64_t x1 = MAX((int64_t)d->x + d->w, (int64_t)x + w);
    int64_t y1 = MAX((int64_t)d->y + d->h, (int64_t)y + h);
    *d = (DNGLDamage){ (int)x0, (int)y0, (int)(x1 - x0),
                       (int)(y1 - y0), true };
}

static DNGLDamage dn_gl_take_damage(DNGLState *gl)
{
    DNGLDamage d = (gl->force_full_damage || !gl->surface_frame_valid ||
                    !gl->accumulated_damage.valid)
                   ? dn_gl_full_damage(gl) : gl->accumulated_damage;
    gl->accumulated_damage.valid = false;
    gl->force_full_damage = false;
    return d;
}

static void dn_gl_publish_damage(DNGLState *gl, DNGLDamage d)
{
    int sw = surface_width(gl->ds), sh = surface_height(gl->ds);
    if (!d.valid || d.w <= 0 || d.h <= 0 || d.x < 0 || d.y < 0 ||
        (int64_t)d.x + d.w > sw || (int64_t)d.y + d.h > sh) {
        d = dn_gl_full_damage(gl);
    }
    bool full = d.x == 0 && d.y == 0 && d.w == sw && d.h == sh;
    gl->published_full += full;
    gl->published_partial += !full;
    gl->published_area += (uint64_t)d.w * (uint64_t)d.h;
    dpy_gfx_update(gl->dcl.con, d.x, d.y, d.w, d.h);
    if (gl->published_full + gl->published_partial >= 128) {
        DN_DLOG("gl publish damage: samples=128 full=%llu partial=%llu mean-area=%.1f%% (announced; actual host upload may differ; readback full)",
                (unsigned long long)gl->published_full,
                (unsigned long long)gl->published_partial,
                (double)gl->published_area / (128.0 * sw * sh) * 100.0);
        gl->published_full = gl->published_partial = gl->published_area = 0;
    }
}

static void dn_gl_drain_errors(DNGLState *gl, const char *where); /* 0.2.22, definit mai jos */

static void dn_gl_cancel_pending_readback(DNGLState *gl)
{
    gl->pbo_pending = false;
    gl->pbo_has_prev = false;
    /* A surface switch can arrive without a GL context. In that case keep
     * the tiny sync handle until the next GL flush, which can delete it
     * safely; never publish a PBO belonging to the old surface. */
    if (gl->pbo_fence && eglGetCurrentContext() != EGL_NO_CONTEXT) {
        glDeleteSync(gl->pbo_fence);
        gl->pbo_fence = NULL;
    }
}

static void dn_gl_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *new_surface)
{
    DNGLState *gl = container_of(dcl, DNGLState, dcl);
    if (gl->ds != new_surface) {
        dn_gl_cancel_pending_readback(gl);
#ifdef __APPLE__
        dn_gl_io_invalidate(gl); /* never present a frame from the old scanout */
#endif
        gl->surface_frame_valid = false;
        gl->force_full_damage = true;
        gl->accumulated_damage.valid = false;
        DN_DLOG("gl gfx_switch ds=%p %dx%d", (void *)new_surface,
                new_surface ? surface_width(new_surface) : 0,
                new_surface ? surface_height(new_surface) : 0);
    }
    gl->ds = new_surface;
}

static void dn_gl_gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    (void)dcl; (void)x; (void)y; (void)w; (void)h; /* readback is in dpy_gl_update */
}

static void dn_gl_scanout_disable(DisplayChangeListener *dcl)
{
    DNGLState *gl = container_of(dcl, DNGLState, dcl);
    dn_gl_cancel_pending_readback(gl);
#ifdef __APPLE__
    dn_gl_io_invalidate(gl);
#endif
    gl->surface_frame_valid = false;
    gl->force_full_damage = true;
    gl->accumulated_damage.valid = false;
    egl_fb_destroy(&gl->guest_fb);
    egl_fb_destroy(&gl->blit_fb);
    dn_gl_mtl_set(gl, NULL, 0, 0, false);
    DN_DLOG("gl scanout disabled");
}

/*
 * Blit-framebuffer texture, ES-correct under ANGLE (Log crash 2).
 *
 * upstream egl_fb_setup_new_tex() does
 *   glTexImage2D(..., GL_RGBA (unsized internal), ..., GL_BGRA, ...)
 * which desktop Mesa accepts but ANGLE (ES3) rejects with
 * GL_INVALID_OPERATION: the BGRA pixel format requires the sized
 * GL_BGRA8_EXT internal format. The texture ends up without storage,
 * every following blit/read then fails with
 * GL_INVALID_FRAMEBUFFER_OPERATION, and the sticky error flags pollute
 * the context shared with virglrenderer. UTM never hits this because
 * it composites scanout textures itself instead of reading back.
 *
 * So create the texture ourselves with BGRA8_EXT (keeps egl_fb_read's
 * BGRA readback valid), fall back to RGBA8 + CPU swizzle if some
 * driver rejects even that, and log the resulting FBO completeness.
 */
static void dn_gl_setup_blit_fb(DNGLState *gl, uint32_t width, uint32_t height)
{
    GLuint texture = 0;
    GLenum err;
    GLenum status;

    while (glGetError() != GL_NO_ERROR) {
        /* drain stale flags so the checks below attribute exactly */
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA8_EXT, (GLsizei)width,
                 (GLsizei)height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    err = glGetError();
    gl->blit_is_rgba = false;
    if (err != GL_NO_ERROR) {
        DN_DLOG("gl blit tex: BGRA8_EXT refuzat err=0x%x, fallback RGBA8", err);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width,
                     (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        err = glGetError();
        gl->blit_is_rgba = true;
        if (err != GL_NO_ERROR) {
            DN_DLOG("gl blit tex: si RGBA8 refuzat err=0x%x", err);
        }
    }
    egl_fb_setup_for_tex(&gl->blit_fb, (int)width, (int)height, texture, true);
    glBindFramebuffer(GL_FRAMEBUFFER, gl->blit_fb.framebuffer);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    DN_DLOG("gl blit fb %ux%u status=0x%x format=%s", width, height, status,
            gl->blit_is_rgba ? "RGBA8" : "BGRA8_EXT");
}

/* Read GL_RGBA bytes as pixman BGRA by swapping red and blue. Alpha and
 * green are untouched. Used for both synchronous safety fallback and the
 * asynchronous EAGL PBO route when BGRA8_EXT is not supported. */
static uint32_t dn_gl_rgba_to_bgra(uint32_t v)
{
    return (v & 0xff00ff00u) | ((v & 0x000000ffu) << 16) |
           ((v >> 16) & 0x000000ffu);
}

static void dn_gl_copy_pbo_to_surface(DNGLState *gl, const uint8_t *src,
                                      int w, int h)
{
    /* Swift borrows the console pixels under dn.lock. Guard the write
     * as well: the 8ms refresh can now publish between GL guest updates. */
    qemu_mutex_lock(&dn.lock);
    uint8_t *dst = surface_data(gl->ds);
    int dstride = surface_stride(gl->ds);
    if (!gl->blit_is_rgba && dstride == w * 4) {
        memcpy(dst, src, (size_t)w * (size_t)h * 4u);
    } else {
        for (int r = 0; r < h; r++) {
            uint8_t *row = dst + (size_t)r * dstride;
            const uint8_t *in = src + (size_t)r * (size_t)w * 4u;
            memcpy(row, in, (size_t)w * 4u);
            if (gl->blit_is_rgba) {
                /* Row-by-row: never touch any pixman stride padding. */
                uint32_t *words = (uint32_t *)row;
                for (int col = 0; col < w; col++) {
                    words[col] = dn_gl_rgba_to_bgra(words[col]);
                }
            }
        }
    }
    qemu_mutex_unlock(&dn.lock);
}

/* Safe synchronous fallback if buffer objects are missing or mapping fails.
 * The destination stride is normally width*4, but honor the pixman stride
 * explicitly (OpenGL ES3 supports GL_PACK_ROW_LENGTH). */
static void dn_gl_read_rgba_swizzle(DNGLState *gl)
{
    int w = surface_width(gl->ds);
    int h = surface_height(gl->ds);
    uint8_t *dst = surface_data(gl->ds);
    int dstride = surface_stride(gl->ds);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->blit_fb.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ROW_LENGTH, dstride / 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    qemu_mutex_lock(&dn.lock);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    for (int row = 0; row < h; row++) {
        uint32_t *words = (uint32_t *)(dst + (size_t)row * dstride);
        for (int col = 0; col < w; col++) {
            words[col] = dn_gl_rgba_to_bgra(words[col]);
        }
    }
    qemu_mutex_unlock(&dn.lock);
}

static void dn_gl_read_sync(DNGLState *gl)
{
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (gl->blit_is_rgba) {
        dn_gl_read_rgba_swizzle(gl);
    } else {
        qemu_mutex_lock(&dn.lock);
        egl_fb_read(gl->ds, &gl->blit_fb);
        qemu_mutex_unlock(&dn.lock);
    }
}

static bool dn_gl_sync_current_frame(DNGLState *gl, DNGLDamage *published_damage)
{
    dn_gl_read_sync(gl);
    /* A synchronous read replaces ALL pixels, including those of any
     * abandoned PBO. Never announce a subrect from a failed async frame. */
    *published_damage = dn_gl_full_damage(gl);
    gl->surface_frame_valid = true;
    gl->accumulated_damage.valid = false;
    gl->force_full_damage = false;
    return true;
}

/*
 * Own ES3 fullscreen-triangle texture blit (black-after-bootanim).
 *
 * Why not QEMU's egl_texture_blit(): it calls glEnable(GL_TEXTURE_2D),
 * a desktop-GL-only cap that errors 0x500 under ANGLE/ES and copies
 * nothing (measured, 0.2.15). Why sampling at all vs glBlitFramebuffer:
 * the scanout texture is rendered by virglrenderer in guest GL
 * contexts and consumed here in ctx0; ANGLE synchronizes shared
 * texture contents on SAMPLE, while the framebuffer-blit path can
 * observe stale contents for buffers nobody swap-flushed (the client
 * composition target after bootanim). Guest texture filters are
 * forced complete (LINEAR/CLAMP) so sampling can never hit the
 * incomplete-texture black. Flip convention mirrors egl-headless'
 * texture path (!y_0_top); if the picture lands upside down the fix
 * is one boolean — visible beats invisible.
 */
static const char dn_blit_vs[] =
    "#version 300 es\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char dn_blit_fs[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uFlip;\n"
    "in vec2 vUv;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "    vec2 uv = vUv;\n"
    "    if (uFlip != 0) uv.y = 1.0 - uv.y;\n"
    "    oColor = texture(uTex, uv);\n"
    "}\n";

static GLuint dn_gl_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    if (!s) {
        return 0;
    }
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char logbuf[256];
        logbuf[0] = '\0';
        glGetShaderInfoLog(s, sizeof(logbuf), NULL, logbuf);
        DN_DLOG("gl texblit: compilare shader 0x%x esuata: %s", type, logbuf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool dn_gl_tex_prog_init(DNGLState *gl)
{
    GLuint vs = dn_gl_compile(GL_VERTEX_SHADER, dn_blit_vs);
    GLuint fs = dn_gl_compile(GL_FRAGMENT_SHADER, dn_blit_fs);
    GLuint prog;
    GLint ok = 0;
    GLint loc;

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        DN_DLOG("gl texblit: unavailable (staying on fb blit)");
        return false;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok || !prog) {
        char logbuf[256];
        logbuf[0] = '\0';
        if (prog) {
            glGetProgramInfoLog(prog, sizeof(logbuf), NULL, logbuf);
            glDeleteProgram(prog);
        }
        DN_DLOG("gl texblit: link failed: %s", logbuf);
        return false;
    }
    gl->blit_prog = prog;
    glGenVertexArrays(1, &gl->blit_vao);
    glGenSamplers(1, &gl->blit_sampler);
    if (!gl->blit_vao || !gl->blit_sampler || glGetError() != GL_NO_ERROR) {
        DN_DLOG("gl texblit: ES3 VAO/sampler allocation failed");
        if (gl->blit_sampler) glDeleteSamplers(1, &gl->blit_sampler);
        if (gl->blit_vao) glDeleteVertexArrays(1, &gl->blit_vao);
        glDeleteProgram(prog);
        gl->blit_prog = gl->blit_vao = gl->blit_sampler = 0;
        return false;
    }
    glSamplerParameteri(gl->blit_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(gl->blit_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(gl->blit_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(gl->blit_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUseProgram(prog);
    loc = glGetUniformLocation(prog, "uTex");
    if (loc >= 0) {
        glUniform1i(loc, 0);
    }
    gl->blit_uflip = glGetUniformLocation(prog, "uFlip");
    DN_DLOG("gl texblit: program OK (drawing via texture)");
    return true;
}

static bool dn_gl_tex_blit_to(DNGLState *gl, GLuint destination, int width, int height)
{
    if (!gl->blit_prog_tried) {
        gl->blit_prog_tried = true;
        dn_gl_tex_prog_init(gl);
    }
    if (!gl->blit_prog) {
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, destination);
    glViewport(0, 0, width, height);
    glUseProgram(gl->blit_prog);
    if (gl->blit_uflip >= 0) {
        /* 0.2.20: device verdict (0.2.19) — the 0.2.18 convention
         * (y_0_top ? 0 : 1) gave an upside-down image; inverted. */
        glUniform1i(gl->blit_uflip, gl->y_0_top ? 1 : 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->guest_fb.texture);
    /* Sampler state lives in OUR display context. Do not call
     * glTexParameteri on the virgl scanout object: it belongs to guest
     * SurfaceFlinger and may be shared between renderer contexts. */
    glBindSampler(0, gl->blit_sampler);
    glBindVertexArray(gl->blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true;
}

static bool dn_gl_tex_blit(DNGLState *gl)
{
    return dn_gl_tex_blit_to(gl, gl->blit_fb.framebuffer,
                             gl->blit_fb.width, gl->blit_fb.height);
}

/* ---- Optional EAGL -> IOSurface -> Metal scanout (0.4.11) ----
 * The pinned ANGLE EAGL config exposes EGL_PBUFFER_BIT and
 * EGL_BIND_TO_TEXTURE_TARGET_ANGLE=EGL_TEXTURE_2D. On a physical iOS
 * device eglBindTexImage() binds the app-created BGRA IOSurface to a GLES
 * texture/FBO. We draw the SAME guest texture/shader as the proven PBO path,
 * but publish only when a nonblocking GLES3 fence signals. Four slots +
 * Metal completion holds prevent writing while Metal still samples a slot.
 * EVERY capability/GL/queue failure returns to the already-working PBO.
 * This is a HOST copy elimination, not proof of Android 60/120 FPS. */
#ifdef __APPLE__
static bool dn_gl_io_register(DNGLState *gl, void *const surfaces[DN_IO_SLOTS],
                              int w, int h)
{
    DNIOPool *io = &gl->io;
    EGLint surface_type = 0, target = 0;
    if (!dn_gl_ready || !gl->inited || io->lock_inited ||
        qemu_egl_angle_native_device || !surfaces ||
        w <= 0 || h <= 0 || (uint64_t)w * h > 720u * 1280u ||
        !epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_ANGLE_iosurface_client_buffer") ||
        !eglGetConfigAttrib(qemu_egl_display, qemu_egl_config,
                            EGL_SURFACE_TYPE, &surface_type) ||
        !(surface_type & EGL_PBUFFER_BIT) ||
        !eglGetConfigAttrib(qemu_egl_display, qemu_egl_config,
                            EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &target) ||
        target != EGL_TEXTURE_2D) {
        DN_DLOG("iosurface: config/extension/backend/size unavailable; PBO retained (EGL 0x%x)",
                eglGetError());
        return false;
    }
    for (int i = 0; i < DN_IO_SLOTS; i++) {
        if (!surfaces[i]) {
            return false;
        }
    }
    qemu_mutex_init(&io->lock);
    io->lock_inited = true;
    io->width = w;
    io->height = h;
    io->latest = -1;
    io->min_valid_ticket = 1;
    for (int i = 0; i < DN_IO_SLOTS; i++) {
        io->slot[i].surface = surfaces[i];
        CFRetain((CFTypeRef)surfaces[i]);
    }
    qatomic_set(&io->ready, true);
    DN_DLOG("iosurface: 4 BGRA slots registered for %dx%d (EAGL path; PBO fallback kept)",
            w, h);
    return true;
}

static void dn_gl_io_invalidate(DNGLState *gl)
{
    DNIOPool *io = &gl->io;
    if (!qatomic_read(&io->ready)) {
        return;
    }
    qemu_mutex_lock(&io->lock);
    io->latest = -1;
    io->min_valid_ticket = io->ticket + 1;
    io->active = false;
    qemu_mutex_unlock(&io->lock);
}

static void dn_gl_io_disable(DNGLState *gl, const char *reason)
{
    DNIOPool *io = &gl->io;
    if (!qatomic_read(&io->ready)) {
        return;
    }
    qemu_mutex_lock(&io->lock);
    bool was_enabled = !io->disabled;
    io->disabled = true;
    io->latest = -1;
    io->active = false;
    qemu_mutex_unlock(&io->lock);
    if (was_enabled) {
        DN_DLOG("iosurface disabled (%s); using proven PBO readback for this VM",
                reason);
    }
}

static bool dn_gl_io_setup_slot(DNGLState *gl, DNIOSlot *slot)
{
    if (slot->pbuffer != EGL_NO_SURFACE) {
        return slot->texture && slot->fbo;
    }
    DNIOPool *io = &gl->io;
    const EGLint attrs[] = {
        EGL_WIDTH, io->width, EGL_HEIGHT, io->height,
        EGL_IOSURFACE_PLANE_ANGLE, 0,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_BGRA,
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE,
        EGL_NONE
    };
    slot->pbuffer = eglCreatePbufferFromClientBuffer(qemu_egl_display,
                         EGL_IOSURFACE_ANGLE, (EGLClientBuffer)slot->surface,
                         qemu_egl_config, attrs);
    if (slot->pbuffer == EGL_NO_SURFACE) {
        DN_DLOG("iosurface: eglCreatePbufferFromClientBuffer failed (0x%x)",
                eglGetError());
        return false;
    }
    glGenTextures(1, &slot->texture);
    glGenFramebuffers(1, &slot->fbo);
    if (!slot->texture || !slot->fbo || glGetError() != GL_NO_ERROR) {
        return false;
    }
    return true;
}

static bool dn_gl_io_draw(DNGLState *gl)
{
    DNIOPool *io = &gl->io;
    if (!qatomic_read(&io->ready) || !gl->ds ||
        !gl->guest_fb.texture || gl->mtl_tex) {
        return false;
    }
    if (surface_width(gl->ds) != io->width ||
        surface_height(gl->ds) != io->height ||
        gl->guest_fb.width != io->width ||
        gl->guest_fb.height != io->height) {
        dn_gl_io_invalidate(gl); /* firmware/resize: use PBO until matching */
        return false;
    }
    int chosen = -1;
    uint64_t oldest = UINT64_MAX;
    qemu_mutex_lock(&io->lock);
    if (!io->disabled) {
        for (int i = 0; i < DN_IO_SLOTS; i++) {
            DNIOSlot *s = &io->slot[i];
            if (i != io->latest && !s->metal_holds && !s->pending &&
                s->ticket < oldest) {
                chosen = i;
                oldest = s->ticket;
            }
        }
        if (chosen >= 0) {
            io->slot[chosen].pending = true; /* fence not installed yet */
            io->slot[chosen].ticket = ++io->ticket;
        }
    }
    bool disabled = io->disabled;
    qemu_mutex_unlock(&io->lock);
    if (disabled) {
        gl->force_full_damage = true;
        return false;
    }
    if (chosen < 0) {
        io->skipped++;
        /* Metal consumption/backpressure must NOT hide the final tap frame.
         * Disable fast path and publish via the already-working PBO. */
        dn_gl_io_disable(gl, "all slots in use");
        gl->force_full_damage = true;
        return false;
    }
    DNIOSlot *slot = &io->slot[chosen];
    if (!dn_gl_io_setup_slot(gl, slot)) {
        goto fail;
    }
    glBindTexture(GL_TEXTURE_2D, slot->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!eglBindTexImage(qemu_egl_display, slot->pbuffer, EGL_BACK_BUFFER)) {
        DN_DLOG("iosurface: eglBindTexImage failed (0x%x)", eglGetError());
        goto fail;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, slot->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, slot->texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR) {
        DN_DLOG("iosurface: FBO incomplete 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        eglReleaseTexImage(qemu_egl_display, slot->pbuffer, EGL_BACK_BUFFER);
        goto fail;
    }
    bool shader = dn_gl_tex_blit_to(gl, slot->fbo, io->width, io->height);
    if (!shader) {
        egl_fb dest = { .framebuffer = slot->fbo, .width = io->width,
                        .height = io->height };
        egl_fb_blit(&dest, &gl->guest_fb, gl->y_0_top);
    }
    GLenum draw_error = glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    EGLBoolean released = eglReleaseTexImage(qemu_egl_display, slot->pbuffer,
                                              EGL_BACK_BUFFER);
    if (draw_error != GL_NO_ERROR || !released || glGetError() != GL_NO_ERROR) {
        DN_DLOG("iosurface: blit/release error GL=0x%x EGL=0x%x", draw_error,
                eglGetError());
        goto fail;
    }
    /* ANGLE release flushes its EAGL texture. A *subsequent* ES3 fence
     * verifies GPU completion before Metal may sample this IOSurface. */
    slot->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLenum sync_error = glGetError();
    if (!slot->fence || sync_error != GL_NO_ERROR) {
        DN_DLOG("iosurface: GLES3 fence creation failed (0x%x)", sync_error);
        goto fail;
    }
    glFlush();
    if (glGetError() != GL_NO_ERROR) {
        goto fail;
    }
    /* Once a GPU-bound frame exists, no stale PBO publication may replace
     * it. If anything later fails, the very same flush falls back to PBO. */
    dn_gl_cancel_pending_readback(gl);
    gl->surface_frame_valid = false;
    gl->force_full_damage = true;
    gl->accumulated_damage.valid = false;
    return true;
fail:
    if (slot->fence) {
        glDeleteSync(slot->fence);
        slot->fence = NULL;
    }
    qemu_mutex_lock(&io->lock);
    slot->pending = false;
    qemu_mutex_unlock(&io->lock);
    dn_gl_io_disable(gl, "EGL/GL interop error");
    dn_gl_drain_errors(gl, "iosurface-fallback");
    gl->force_full_damage = true;
    return false;
}

static void dn_gl_io_drain_pending(void)
{
    DNGLState *gl = &dn_gl;
    DNIOPool *io = &gl->io;
    if (!qatomic_read(&io->ready)) {
        return;
    }
    qemu_mutex_lock(&io->lock);
    bool disabled = io->disabled;
    qemu_mutex_unlock(&io->lock);
    if (disabled || !gl->ds || gl->mtl_tex) {
        return;
    }
    bool adopted = eglGetCurrentContext() == EGL_NO_CONTEXT;
    if (adopted && !eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                                   EGL_NO_SURFACE, qemu_egl_rn_ctx)) {
        return; /* no GL operations without a current context */
    }
    bool publish = false, failed = false;
    for (int i = 0; i < DN_IO_SLOTS; i++) {
        DNIOSlot *s = &io->slot[i];
        if (!s->pending || !s->fence) {
            continue;
        }
        GLenum status = glClientWaitSync(s->fence, 0, 0);
        if (status == GL_TIMEOUT_EXPIRED) {
            continue;
        }
        glDeleteSync(s->fence);
        s->fence = NULL;
        qemu_mutex_lock(&io->lock);
        s->pending = false;
        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
            if (!io->disabled && s->ticket >= io->min_valid_ticket &&
                s->ticket > io->latest_ticket) {
                io->latest = i;
                io->latest_ticket = s->ticket;
                io->active = true;
                publish = true;
            } else {
                io->skipped++; /* newer signaled or modeset invalidated */
            }
        } else {
            failed = true;
        }
        qemu_mutex_unlock(&io->lock);
    }
    if (failed || glGetError() != GL_NO_ERROR) {
        dn_gl_io_disable(gl, "fence wait/GL error");
        publish = false;
        dn_gl_drain_errors(gl, "iosurface-fence");
    }
    if (publish) {
        /* The IOSurface now holds a complete frame. The raster surface is
         * deliberately NOT copied; this update only wakes the same Metal
         * presenter/poll sequence as the existing display bridge. */
        dpy_gfx_update(gl->dcl.con, 0, 0, io->width, io->height);
        io->published++;
        if ((io->published % 128) == 0) {
            DN_DLOG("iosurface: published=%llu skipped=%llu (GPU fence ready; no PBO/CPU upload; not guest FPS)",
                    (unsigned long long)io->published,
                    (unsigned long long)io->skipped);
        }
    }
    if (adopted) {
        eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                       EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

static void dn_gl_io_shutdown(DNGLState *gl)
{
    DNIOPool *io = &gl->io;
    if (!qatomic_read(&io->ready)) {
        return;
    }
    qatomic_set(&io->ready, false); /* acquire from UIKit stops immediately */
    qemu_mutex_lock(&io->lock);
    io->disabled = true;
    io->latest = -1;
    qemu_mutex_unlock(&io->lock);
    bool adopted = eglGetCurrentContext() == EGL_NO_CONTEXT;
    bool current = !adopted || eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                                               EGL_NO_SURFACE, qemu_egl_rn_ctx);
    for (int i = 0; i < DN_IO_SLOTS; i++) {
        DNIOSlot *s = &io->slot[i];
        if (current && s->fence) {
            glDeleteSync(s->fence);
        }
        s->fence = NULL;
        if (current && s->fbo) glDeleteFramebuffers(1, &s->fbo);
        if (current && s->texture) glDeleteTextures(1, &s->texture);
        if (s->pbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(qemu_egl_display, s->pbuffer);
            s->pbuffer = EGL_NO_SURFACE;
        }
        if (s->surface) {
            /* Outstanding Metal buffers retain their own +1 references;
             * dnq_display_iosurface_release balances them AFTER cleanup. */
            CFRelease((CFTypeRef)s->surface);
            s->base_released = true;
            if (!s->metal_holds) s->surface = NULL;
        }
    }
    if (adopted && current) {
        eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                       EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}
#endif /* __APPLE__: the generic QEMU build stays on PBO */

/* ---- Direct Metal presentation (0.2.24): utils ---- */
#ifdef __APPLE__
#define DN_MTL_RETAIN(p)  do { if (p) { CFRetain((CFTypeRef)(p)); } } while (0)
#define DN_MTL_RELEASE(p) do { if (p) { CFRelease((CFTypeRef)(p)); } } while (0)
#else
#define DN_MTL_RETAIN(p)  ((void)0)
#define DN_MTL_RELEASE(p) ((void)0)
#endif

static void dn_gl_mtl_ensure_lock(DNGLState *gl)
{
    if (!gl->mtl_lock_inited) {
        qemu_mutex_init(&gl->mtl_lock);
        gl->mtl_lock_inited = true;
    }
}

static void dn_gl_mtl_set(DNGLState *gl, void *tex, int w, int h, bool y0top)
{
    dn_gl_mtl_ensure_lock(gl);
    qemu_mutex_lock(&gl->mtl_lock);
    if (tex && tex != gl->mtl_tex) {
        DN_MTL_RETAIN(tex);
    }
    if (gl->mtl_tex && gl->mtl_tex != tex) {
        DN_MTL_RELEASE(gl->mtl_tex);
    }
    gl->mtl_tex = tex;
    gl->mtl_w = w;
    gl->mtl_h = h;
    gl->mtl_y0top = y0top;
    if (tex) {
        gl->mtl_gen++;
    }
    qemu_mutex_unlock(&gl->mtl_lock);
}

static void dn_gl_scanout_texture(DisplayChangeListener *dcl,
                                  uint32_t backing_id,
                                  bool backing_y_0_top,
                                  uint32_t backing_width,
                                  uint32_t backing_height,
                                  uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h,
                                  ScanoutTextureNative native)
{
    DNGLState *gl = container_of(dcl, DNGLState, dcl);
    /* Another scanout buffer may contain different pixels even if the
     * reported dirty rectangle is tiny. Also refuse subrects for offset
     * scanouts until their coordinate convention is device-validated. */
    if (!gl->scanouts || gl->last_backing_id != backing_id ||
        gl->y_0_top != backing_y_0_top || x != 0 || y != 0 ||
        w != backing_width || h != backing_height) {
        gl->force_full_damage = true;
    }
    gl->y_0_top = backing_y_0_top;
    gl->last_backing_id = backing_id;
    /* The native handle (MTLTexture) is the object holding the real pixels on
     * ANGLE/Metal; the GL name of the guest's 3D textures is not
     * lizibil din contextul nostru (Husk/UTM prezinta tot asa). */
    if (native.type == SCANOUT_TEXTURE_NATIVE_TYPE_METAL && native.handle) {
        if (native.handle != gl->mtl_last_handle) {
            gl->mtl_last_handle = native.handle;
            /* 0.2.29: log only handles never seen before (SF page-flips
             * between the same 2-3 buffers; per-change logging drowned
             * qemu.log with ~1900 lines in one session). */
            bool dn_hseen = false;
            for (int dn_i = 0; dn_i < 4; dn_i++) {
                if (gl->mtl_handles_seen[dn_i] == native.handle) {
                    dn_hseen = true;
                }
            }
            if (!dn_hseen) {
                gl->mtl_handles_seen[gl->mtl_handles_i % 4] = native.handle;
                gl->mtl_handles_i++;
                DN_DLOG("gl scanout tex change: id=%u handle=%p %ux%u",
                        backing_id, native.handle,
                        backing_width, backing_height);
            }
        }
        dn_gl_mtl_set(gl, native.handle,
                      (int)backing_width, (int)backing_height,
                      backing_y_0_top);
    } else if (gl->mtl_tex) {
        gl->mtl_last_handle = NULL;
        dn_gl_mtl_set(gl, NULL, 0, 0, false);
    }
    if ((int)native.type != gl->mtl_last_type) {
        gl->mtl_last_type = (int)native.type;
        DN_DLOG("gl scanout native: type=%s handle=%p (id=%u %ux%u y0top=%d)",
                native.type == SCANOUT_TEXTURE_NATIVE_TYPE_METAL ? "METAL"
                : native.type == SCANOUT_TEXTURE_NATIVE_TYPE_D3D ? "D3D"
                : "none",
                native.handle, backing_id,
                backing_width, backing_height, backing_y_0_top ? 1 : 0);
    }
    egl_fb_setup_for_tex(&gl->guest_fb, backing_width, backing_height,
                         backing_id, false);
    if (gl->blit_fb.width != (int)backing_width ||
        gl->blit_fb.height != (int)backing_height) {
        dn_gl_cancel_pending_readback(gl);
        gl->surface_frame_valid = false;
        gl->force_full_damage = true;
        gl->accumulated_damage.valid = false;
        egl_fb_destroy(&gl->blit_fb);
        dn_gl_setup_blit_fb(gl, backing_width, backing_height);
    }
    {
        GLenum serr = glGetError();
        if (serr != GL_NO_ERROR) {
            DN_DLOG("gl scanout setup: GL error 0x%x", serr);
        }
    }
    /* Probe 0.2.21, rewritten 0.2.23 in legal ES 3.0: ask the guest
     * FBO about its attachment (type+name), in the current context.
     * NU folosi functii >ES 3.0 aici: varianta originala cu
     * GetTexLevelParameteriv (ES 3.1) a facut libepoxy sa omoare
     * procesul la primul scanout (Log rau, 0.2.22) — ANGLE da doar
     * ES 3.0 contexts (the 4.6->3.0 create cascade in the same log). */
    bool dn_probe_seen = false;
    for (int dn_i = 0; dn_i < 4; dn_i++) {
        if (gl->probe_ids[dn_i] == backing_id) { dn_probe_seen = true; }
    }
    if (backing_id != gl->probe_last_backing && !dn_probe_seen) {
        gl->probe_last_backing = backing_id;
        gl->probe_ids[gl->probe_ids_i % 4] = backing_id;
        gl->probe_ids_i++;
        GLint atype = 0, aname = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, gl->guest_fb.framebuffer);
        GLenum gst = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
            &atype);
        if (atype == GL_TEXTURE) {
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &aname);
        }
        GLenum perr = glGetError();
        DN_DLOG("gl tex probe id=%u attachment type=0x%x name=%d guestfbo=0x%x perr=0x%x ctx=%p",
                backing_id, (unsigned)atype, aname, gst, perr,
                (void *)eglGetCurrentContext());
    }
    /* Throttled (Log crash 3): SurfaceFlinger double-buffering sets a
     * scanout per pageflip — an fprintf+fflush per flip under the BQL
     * burned real time and flooded qemu.log with thousands of lines. */
    dn_gl_drain_errors(gl, "scanout"); /* nu lasa nimic in urma (0.2.22) */
    gl->scanouts++;
    if (gl->scanouts <= 5 || (gl->scanouts % 600) == 0) {
        DN_DLOG("gl scanout texture #%llu id=%u %ux%u y0top=%d",
                (unsigned long long)gl->scanouts, backing_id,
                backing_width, backing_height, backing_y_0_top ? 1 : 0);
    }
}


/*
 * GL error hygiene (0.2.22): virglrenderer checks glGetError()
 * after EVERY guest command, in the current context (shared with us);
 * a single error found there aborts the submit and reports
 * contextul guest ca defect ("failed to dispatch DRAW_VBO: 22" ->
 * SIGABRT in SurfaceFlinger). So our code must not leave
 * NO error behind: we drain on exit from every GL callback.
 * Log on transition, to know whether we really generate errors.
 */
static void dn_gl_drain_errors(DNGLState *gl, const char *where)
{
    GLenum e, last = GL_NO_ERROR;
    int guard = 0;
    while ((e = glGetError()) != GL_NO_ERROR && ++guard < 64) {
        last = e;
    }
    if (last != GL_NO_ERROR && last != gl->err_last_entry) {
        gl->err_last_entry = last;
        DN_DLOG("gl drain (%s): error left in context 0x%x (ctx=%p)",
                where, last, (void *)eglGetCurrentContext());
    }
}

/*
 * Readback asincron (0.2.20): calea veche (egl_fb_read) face un
 * SYNCHRONOUS glReadPixels straight into the surface, under the BQL — the whole emulator
 * waits for the GPU pipeline to drain at EVERY frame (up to ~15 MB
 * at native 1320x2868), starving the vCPUs. The classic cure: two
 * pixel pack buffers. Each flush launches the read into the back PBO
 * (normally without a synchronous readback) and publishes the previous
 * PBO if needed. The 8 ms display refresh polls an ES3 fence WITHOUT
 * waiting and publishes the newest PBO even if the guest sends no further
 * frames (a tap must not remain one frame behind indefinitely). Mapping
 * the previous frame on a busy GL update can STILL stall the BQL; this
 * path logs mean/max times instead of claiming guaranteed speedups.
 * Any PBO/fence failure falls back safely to synchronous current-frame
 * readback for the remainder of this VM run.
 */
static bool dn_gl_pbo_ensure(DNGLState *gl, int w, int h)
{
    if (gl->pbo[0] && gl->pbo_w == w && gl->pbo_h == h) {
        return true;
    }
    dn_gl_cancel_pending_readback(gl);
    gl->surface_frame_valid = false;
    gl->force_full_damage = true;
    if (gl->pbo[0]) {
        glDeleteBuffers(2, gl->pbo);
        gl->pbo[0] = gl->pbo[1] = 0;
    }
    glGenBuffers(2, gl->pbo);
    if (!gl->pbo[0] || !gl->pbo[1]) {
        if (gl->pbo[0] || gl->pbo[1]) glDeleteBuffers(2, gl->pbo);
        gl->pbo[0] = gl->pbo[1] = 0;
        return false;
    }
    size_t bytes = (size_t)w * (size_t)h * 4u;
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(2, gl->pbo);
        gl->pbo[0] = gl->pbo[1] = 0;
        return false;
    }
    gl->pbo_w = w;
    gl->pbo_h = h;
    gl->pbo_write = 0;
    gl->pbo_has_prev = false;
    if (!gl->pbo_logged) {
        gl->pbo_logged = true;
        DN_DLOG("gl pbo: readback asincron activ (2 x %zu octeti, %dx%d format=%s)",
                bytes, w, h, gl->blit_is_rgba ? "RGBA8+swizzle" : "BGRA8");
    }
    return true;
}

/* True when the surface holds an image (one readback behind). This path
 * must handle BOTH BGRA8 and RGBA8: on iOS/EAGL a BGRA8_EXT texture may
 * be unavailable, but synchronous glReadPixels on every RGBA fallback
 * frame would block the BQL, delaying the Android vCPUs AND finger input. */
static bool dn_gl_read_pbo(DNGLState *gl, DNGLDamage *published_damage)
{
    int w = surface_width(gl->ds);
    int h = surface_height(gl->ds);
    /* The previous latest frame is either already published by the 8 ms
     * refresh or about to be mapped below. This GL callback is on the
     * QEMU main-loop thread, so a refresh cannot race this cancellation. */
    gl->pbo_pending = false;
    if (gl->pbo_fence) {
        glDeleteSync(gl->pbo_fence);
        gl->pbo_fence = NULL;
    }
    if (gl->pbo_failed) {
        return dn_gl_sync_current_frame(gl, published_damage);
    }
    if (!dn_gl_pbo_ensure(gl, w, h)) {
        gl->pbo_failed = true;
        DN_DLOG("gl pbo: allocation failed; safe synchronous %s fallback",
                gl->blit_is_rgba ? "RGBA+swizzle" : "BGRA");
        return dn_gl_sync_current_frame(gl, published_damage);
    }
    int wr = gl->pbo_write;
    int rd = wr ^ 1;
    size_t bytes = (size_t)w * (size_t)h * 4u;

    /* Queue readback into the back PBO, tightly packed (all surface
     * stride correction and RGBA swizzle happen after mapping). */
    int64_t submit_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->blit_fb.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[wr]);
    glReadPixels(0, 0, w, h, gl->blit_is_rgba ? GL_RGBA : GL_BGRA,
                 GL_UNSIGNED_BYTE, (void *)0);
    GLenum read_error = glGetError();
    int64_t submit_cost_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - submit_start_ns;
    if (submit_cost_ns >= 0) {
        gl->pbo_submit_total_ns += (uint64_t)submit_cost_ns;
        gl->pbo_submit_samples++;
    }
    if (read_error != GL_NO_ERROR) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gl->pbo_failed = true;
        DN_DLOG("gl pbo: glReadPixels error 0x%x; safe synchronous fallback",
                read_error);
        return dn_gl_sync_current_frame(gl, published_damage);
    }

    DNGLDamage queued_damage = dn_gl_take_damage(gl);
    bool published = false;
    if (gl->pbo_has_prev) {
        int64_t map_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[rd]);
        const uint8_t *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                              bytes, GL_MAP_READ_BIT);
        if (src) {
            dn_gl_copy_pbo_to_surface(gl, src, w, h);
            if (!glUnmapBuffer(GL_PIXEL_PACK_BUFFER)) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                gl->pbo_failed = true;
                DN_DLOG("gl pbo: unmap invalidated the data; synchronous fallback");
                return dn_gl_sync_current_frame(gl, published_damage);
            }
            *published_damage = gl->pbo_damage[rd];
            gl->surface_frame_valid = true;
            published = true;
        } else {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            gl->pbo_failed = true;
            DN_DLOG("gl pbo: mapping failed; safe synchronous fallback");
            return dn_gl_sync_current_frame(gl, published_damage);
        }
        int64_t map_cost_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - map_start_ns;
        if (map_cost_ns >= 0) {
            gl->pbo_map_total_ns += (uint64_t)map_cost_ns;
            gl->pbo_map_samples++;
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    int64_t fence_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLenum fence_error = glGetError();
    if (!fence || fence_error != GL_NO_ERROR) {
        if (fence) glDeleteSync(fence);
        gl->pbo_failed = true;
        DN_DLOG("gl pbo: GLES3 fence failed (0x%x); safe synchronous fallback",
                fence_error);
        return dn_gl_sync_current_frame(gl, published_damage);
    }
    glFlush(); /* submit the fence; refresh will poll it, never wait */
    int64_t fence_cost_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - fence_start_ns;
    if (fence_cost_ns >= 0) {
        gl->pbo_fence_total_ns += (uint64_t)fence_cost_ns;
        gl->pbo_fence_samples++;
    }
    gl->pbo_damage[wr] = queued_damage;
    gl->pbo_fence = fence;
    gl->pbo_pending_index = wr;
    gl->pbo_pending = true;
    gl->pbo_has_prev = true;
    gl->pbo_write = rd;
    return published;
}

static void dn_gl_restore_refresh_context(bool adopted)
{
    if (adopted && !eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                                   EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        DN_DLOG("gl pbo: unable to restore no-context after refresh (EGL 0x%x)",
                eglGetError());
    }
}

/* QEMU's existing 8 ms GUI refresh is already on the main-loop thread.
 * Drain ONLY a completed PBO: no glFinish, no blocking client wait, no
 * extra timer. Without this, the last frame of a touch or boot transition
 * could remain one entire guest frame behind until Android draws again. */
static void dn_gl_drain_pending_readback(void)
{
    DNGLState *gl = &dn_gl;
    if (!dn_gl_ready || !gl->pbo_pending || !gl->ds || !gl->pbo_fence ||
        gl->mtl_tex) {
        return;
    }
    bool adopted = eglGetCurrentContext() == EGL_NO_CONTEXT;
    if (adopted && !eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                                   EGL_NO_SURFACE, qemu_egl_rn_ctx)) {
        return; /* next refresh retries; never use GL without a context */
    }
    GLenum status = glClientWaitSync(gl->pbo_fence, 0, 0);
    if (status == GL_TIMEOUT_EXPIRED) {
        dn_gl_restore_refresh_context(adopted);
        return;
    }
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
        /* A broken fence must not strand the last visible frame. One
         * blocking PBO map still returns completed bytes by the ES3
         * contract; if it succeeds, publish once, then run synchronously
         * on future frames. This is an ERROR path, not the fast path. */
        DN_DLOG("gl pbo: refresh fence wait failed (0x%x); attempt one safe map, then sync",
                status);
        gl->pbo_failed = true;
    }
    int w = surface_width(gl->ds);
    int h = surface_height(gl->ds);
    size_t bytes = (size_t)w * (size_t)h * 4u;
    DNGLDamage frame_damage = gl->pbo_damage[gl->pbo_pending_index];
    int64_t drain_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[gl->pbo_pending_index]);
    const uint8_t *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                          bytes, GL_MAP_READ_BIT);
    bool valid = false;
    if (src) {
        dn_gl_copy_pbo_to_surface(gl, src, w, h);
        valid = glUnmapBuffer(GL_PIXEL_PACK_BUFFER) != 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    dn_gl_cancel_pending_readback(gl);
    if (!valid) {
        gl->pbo_failed = true;
        gl->surface_frame_valid = false;
        gl->force_full_damage = true;
        DN_DLOG("gl pbo: refresh map/unmap failed; sync on next guest flush");
        dn_gl_drain_errors(gl, "pbo-refresh-fail");
        dn_gl_restore_refresh_context(adopted);
        return;
    }
    /* PBO pixels were read full-frame, but the host only needs the dirty
     * rectangle of THIS queued frame, not an unconditional full upload. */
    gl->surface_frame_valid = true;
    dn_gl_publish_damage(gl, frame_damage);
    gl->flushes++; /* a published frame also counts without another guest flush */
    int64_t elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - drain_start_ns;
    if (elapsed_ns >= 0) {
        uint64_t cost = (uint64_t)elapsed_ns;
        gl->drain_total_ns += cost;
        if (cost > gl->drain_max_ns) gl->drain_max_ns = cost;
        gl->drain_samples++;
        if (gl->drain_samples >= 64) {
            DN_DLOG("gl pbo refresh: samples=%u mean=%.2fms max=%.2fms (BQL held; not guest FPS)",
                    gl->drain_samples,
                    (double)gl->drain_total_ns / gl->drain_samples / 1.0e6,
                    (double)gl->drain_max_ns / 1.0e6);
            gl->drain_samples = 0;
            gl->drain_total_ns = gl->drain_max_ns = 0;
        }
    }
    dn_gl_drain_errors(gl, "pbo-refresh");
    dn_gl_restore_refresh_context(adopted);
}

static void dn_gl_flush(DisplayChangeListener *dcl,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    DNGLState *gl = container_of(dcl, DNGLState, dcl);
    if (gl->mtl_tex) {
        /* Direct Metal presentation (0.2.24): the MTLTexture is already on
         * GPU and the app draws it itself; here we only announce
         * the new frame. ANGLE submission pump (0.2.26 experiment):
         * the old readback path forced ANGLE to execute via its sync
         * points; if the Metal scanouts stay black because ANGLE's
         * renders never get submitted/executed without them, a
         * glFlush per flush (+glFinish on census) makes content
         * appear. Harmless if the cause is elsewhere. */
        gl->flushes++;
        if (!eglGetCurrentContext()) {
            eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                           EGL_NO_SURFACE, qemu_egl_rn_ctx);
        }
        /* 0.2.29: glFinish only on census flushes. History: 0.2.27
         * finished ANGLE's stream on EVERY metal flush (cross-queue
         * sync theory); the 0.2.28 flush-synchronized GL census then
         * proved the guest FBO itself is all-zero after the boot
         * animation on this virgl/ANGLE stack (frames are born empty
         * below our display code), so GL is parked (docs/12) and a
         * per-flush glFinish would only burn host CPU here. */
        /* 0.2.28: publish a flush sequence so the app can latch the
         * completed frame (copy it at this post-flush instant)
         * instead of sampling the live guest buffer at arbitrary
         * 120 Hz phases while SurfaceFlinger reuses/clears it. */
        dn_gl_mtl_ensure_lock(gl);
        qemu_mutex_lock(&gl->mtl_lock);
        gl->mtl_seq++;
        qemu_mutex_unlock(&gl->mtl_lock);
        /* 0.2.28: GL-side census, flush-synchronized (after the
         * glFinish above): read the center of the guest FBO in THIS
         * context. gnz>0 here while the app's MTLTexture census reads
         * zeros splits "ANGLE renders but the shared texture never
         * sees it" from "virgl/ANGLE renders black after bootanim". */
        if ((gl->flushes % 60) == 0 && gl->guest_fb.framebuffer &&
            gl->guest_fb.width > 0 && gl->guest_fb.height > 0) {
            glFinish();
            int pw = gl->guest_fb.width < 64 ? gl->guest_fb.width : 64;
            int ph = gl->guest_fb.height < 64 ? gl->guest_fb.height : 64;
            int px0 = gl->guest_fb.width / 2 - pw / 2;
            int py0 = gl->guest_fb.height / 2 - ph / 2;
            if (px0 < 0) { px0 = 0; }
            if (py0 < 0) { py0 = 0; }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->guest_fb.framebuffer);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(px0, py0, pw, ph, GL_BGRA, GL_UNSIGNED_BYTE,
                         gl->probe_buf);
            GLenum cerr = glGetError();
            int gnz = 0;
            for (int i = 0; i < pw * ph; i++) {
                if (gl->probe_buf[i] & 0x00ffffffu) {
                    gnz++;
                }
            }
            DN_DLOG("gl metal census #%llu id=%u gnz=%d/%d gcenter=0x%08x err=0x%x handle=%p",
                    (unsigned long long)gl->flushes, gl->last_backing_id,
                    gnz, pw * ph,
                    gl->probe_buf[(ph / 2) * pw + pw / 2], cerr,
                    gl->mtl_tex);
            dn_gl_drain_errors(gl, "metal-census");
        }
        if ((gl->flushes % 600) == 0) {
            DN_DLOG("gl metal flush #%llu (MTLTexture %dx%d, presented directly, ctx=%p)",
                    (unsigned long long)gl->flushes, gl->mtl_w, gl->mtl_h,
                    (void *)eglGetCurrentContext());
        }
        dpy_gfx_update(gl->dcl.con, x, y, w, h);
        return;
    }
    if (!gl->guest_fb.texture || !gl->blit_fb.texture || !gl->ds) {
        return;
    }
    if (surface_width(gl->ds) <= 0 || surface_height(gl->ds) <= 0) {
        return;
    }
    dn_gl_damage_merge(gl, x, y, w, h); /* BEFORE rate cap: skipped flushes count */
    /*
     * Readback coalescing (Log crash 1): in early boot the guest fbcon
     * flips hundreds of times per second, and every flip cost a full
     * synchronous glReadPixels under the BQL — ANGLE pressure plus vCPU
     * starvation, for frames a 120 Hz MTKView could never show. Cap the
     * readback at ~120/s; the next flush still carries the latest image.
     * The guest-side rendering (virgl) is untouched — only our copy out
     * is coalesced, exactly like a vsync-bound presenter would do.
     */
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (gl->last_readback_ns != 0 &&
        now_ns - gl->last_readback_ns < 8 * 1000 * 1000LL) {
        gl->dropped++;
        return;
    }
    gl->last_readback_ns = now_ns;
    /*
     * Probe 0.2.21 (1/3): GL errors found on ENTRY are not
     * ours (virgl leaves them in this context) — drain and log them
     * pe tranzitie, ca err_blit sa ne masoare doar pe noi. Pana la
     * 0.2.20 eram orbiti de cap-ul de 10 logari exact in faza neagra.
     */
    {
        GLenum e_in = GL_NO_ERROR, e;
        while ((e = glGetError()) != GL_NO_ERROR) {
            e_in = e;
        }
        if (e_in != GL_NO_ERROR && e_in != gl->err_last_entry) {
            gl->err_last_entry = e_in;
            DN_DLOG("gl flush: error inherited on entry 0x%x (flush %llu ctx=%p)",
                    e_in, (unsigned long long)gl->flushes,
                    (void *)eglGetCurrentContext());
        }
    }
    /*
     * Probe 0.2.21 (2/3): DIRECT read from the guest framebuffer at
     * census rate, bypassing shader+PBO. If gnz>0 while
     * suprafata noastra e neagra, vina e in lantul nostru de copiere;
     * if the guest FBO is black here too, the texture really arrives black
     * in contextul asta (guest/virgl).
     */
    bool probe_now = (((gl->flushes + 1) % 600) == 0);
    if (probe_now && gl->guest_fb.framebuffer &&
        gl->guest_fb.width > 0 && gl->guest_fb.height > 0) {
        int pw = gl->guest_fb.width < 64 ? gl->guest_fb.width : 64;
        int ph = gl->guest_fb.height < 64 ? gl->guest_fb.height : 64;
        int px0 = gl->guest_fb.width / 2 - pw / 2;
        int py0 = gl->guest_fb.height / 2 - ph / 2;
        if (px0 < 0) { px0 = 0; }
        if (py0 < 0) { py0 = 0; }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->guest_fb.framebuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(px0, py0, pw, ph, GL_BGRA, GL_UNSIGNED_BYTE,
                     gl->probe_buf);
        gl->probe_err = glGetError();
        int gnz = 0;
        for (int i = 0; i < pw * ph; i++) {
            if (gl->probe_buf[i] & 0x00ffffffu) {
                gnz++;
            }
        }
        gl->probe_nz = gnz;
        gl->probe_center = gl->probe_buf[(ph / 2) * pw + pw / 2];
    }
#ifdef __APPLE__
    /* Same Mesa/VirGL guest frame and EAGL shader, but GPU -> IOSurface
     * rather than GPU -> CPU PBO -> Metal upload. If unsupported, slow,
     * resized, or broken, this SAME flush proceeds through proven PBO. */
    if (dn_gl_io_draw(gl)) {
        gl->flushes++; /* queued; actual publication counted at fence */
        return;
    }
#endif
    /*
     * Copy guest texture -> blit texture by DRAWING it (own ES3
     * shader; QEMU's egl_texture_blit helper is desktop-only and
     * errors 0x500 here). Fall back to framebuffer blit only if the
     * program could not be built.
     */
    int64_t blit_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!dn_gl_tex_blit(gl)) {
        egl_fb_blit(&gl->blit_fb, &gl->guest_fb, gl->y_0_top);
    }
    GLenum err_blit = glGetError();
    int64_t blit_cost_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - blit_start_ns;
    if (blit_cost_ns >= 0) {
        uint64_t cost = (uint64_t)blit_cost_ns;
        gl->blit_total_ns += cost;
        if (cost > gl->blit_max_ns) gl->blit_max_ns = cost;
        gl->blit_samples++;
    }
    int64_t readback_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    DNGLDamage published_damage = { 0 };
    bool published = dn_gl_read_pbo(gl, &published_damage);
    int64_t readback_elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME)
                                  - readback_start_ns;
    if (readback_elapsed_ns >= 0) {
        uint64_t cost = (uint64_t)readback_elapsed_ns;
        gl->readback_total_ns += cost;
        if (cost > gl->readback_max_ns) gl->readback_max_ns = cost;
        gl->readback_samples++;
        if (gl->readback_samples >= 64) {
            DN_DLOG("gl readback: mode=%s format=%s samples=%u mean=%.2fms max=%.2fms (BQL held; not guest FPS)",
                    gl->pbo_failed ? "sync fallback" : "PBO",
                    gl->blit_is_rgba ? "RGBA+swizzle" : "BGRA",
                    gl->readback_samples,
                    (double)gl->readback_total_ns / gl->readback_samples / 1.0e6,
                    (double)gl->readback_max_ns / 1.0e6);
            DN_DLOG("gl stages: blit=%u mean=%.2fms max=%.2fms pbo-submit=%u mean=%.2fms pbo-map+copy=%u mean=%.2fms pbo-fence=%u mean=%.2fms (BQL; not guest FPS)",
                    gl->blit_samples,
                    gl->blit_samples ? (double)gl->blit_total_ns / gl->blit_samples / 1.0e6 : 0.0,
                    (double)gl->blit_max_ns / 1.0e6,
                    gl->pbo_submit_samples,
                    gl->pbo_submit_samples ? (double)gl->pbo_submit_total_ns / gl->pbo_submit_samples / 1.0e6 : 0.0,
                    gl->pbo_map_samples,
                    gl->pbo_map_samples ? (double)gl->pbo_map_total_ns / gl->pbo_map_samples / 1.0e6 : 0.0,
                    gl->pbo_fence_samples,
                    gl->pbo_fence_samples ? (double)gl->pbo_fence_total_ns / gl->pbo_fence_samples / 1.0e6 : 0.0);
            gl->readback_samples = gl->blit_samples = 0;
            gl->pbo_submit_samples = gl->pbo_map_samples = gl->pbo_fence_samples = 0;
            gl->readback_total_ns = gl->readback_max_ns = 0;
            gl->blit_total_ns = gl->blit_max_ns = 0;
            gl->pbo_submit_total_ns = gl->pbo_map_total_ns = gl->pbo_fence_total_ns = 0;
        }
    }
    GLenum err_read = glGetError();
    dn_gl_drain_errors(gl, "flush"); /* nu lasa nimic in urma (0.2.22) */
    /* Probe 0.2.21 (3/3): log error TRANSITIONS (the old cap of
     * 10 logari ascundea tot ce se intampla in faza neagra). */
    if (err_blit != gl->err_last_blit || err_read != gl->err_last_read) {
        gl->err_last_blit = err_blit;
        gl->err_last_read = err_read;
        if (err_blit != GL_NO_ERROR || err_read != GL_NO_ERROR) {
            DN_DLOG("gl flush: erori blit=0x%x read=0x%x (flush %llu, ds %dx%d, blit %dx%d)",
                    err_blit, err_read, (unsigned long long)gl->flushes,
                    surface_width(gl->ds), surface_height(gl->ds),
                    gl->blit_fb.width, gl->blit_fb.height);
        }
    }
    if (!published) {
        return; /* first PBO frame in flight; 8ms refresh publishes when ready */
    }
    dn_gl_publish_damage(gl, published_damage);
    gl->flushes++;
    if ((gl->flushes % 600) == 0) {
        /* Frame census (black-after-bootanim): dense stride-16 sampling
         * of the WHOLE read back surface (3600 samples at 720x1280) so
         * sparse/dark content cannot hide between probe points anymore.
         * nz = samples with any RGB byte set; sum = checksum of sampled
         * words (changes iff content changes); maxc = largest channel
         * byte seen. nz=0 + sum=constant => digitally pure black. */
        int sw = surface_width(gl->ds);
        int sh = surface_height(gl->ds);
        int stride_px = surface_stride(gl->ds) / 4;
        const uint32_t *px = (const uint32_t *)surface_data(gl->ds);
        int xx, yy, nz = 0, ns = 0;
        uint64_t sum = 0;
        unsigned maxc = 0;
        for (yy = 8; yy < sh; yy += 16) {
            for (xx = 8; xx < sw; xx += 16) {
                uint32_t v = px[yy * stride_px + xx];
                ns++;
                sum += v;
                if (v & 0x00ffffffu) {
                    nz++;
                }
                if ((v & 0xffu) > maxc) {
                    maxc = v & 0xffu;
                }
                if (((v >> 8) & 0xffu) > maxc) {
                    maxc = (v >> 8) & 0xffu;
                }
                if (((v >> 16) & 0xffu) > maxc) {
                    maxc = (v >> 16) & 0xffu;
                }
            }
        }
        DN_DLOG("gl stat #%llu nz=%d/%d sum=%llu maxc=%u centru=0x%08x gnz=%d gcentru=0x%08x gerr=0x%x id=%u y0top=%d ds=%p ctx=%p",
                (unsigned long long)gl->flushes, nz, ns,
                (unsigned long long)sum, maxc,
                px[(sh / 2) * stride_px + (sw / 2)],
                gl->probe_nz, gl->probe_center, gl->probe_err,
                gl->last_backing_id, gl->y_0_top ? 1 : 0,
                (void *)gl->ds, (void *)eglGetCurrentContext());
    }
    if (gl->flushes <= 3 || (gl->flushes % 600) == 0) {
        DN_DLOG("gl flush #%llu (coalesced %llu)",
                (unsigned long long)gl->flushes,
                (unsigned long long)gl->dropped);
    }
}

static const DisplayChangeListenerOps dn_gl_ops = {
    .dpy_name               = "droidnative-gl",
    .dpy_gfx_update         = dn_gl_gfx_update,
    .dpy_gfx_switch         = dn_gl_gfx_switch,
    .dpy_gl_scanout_disable = dn_gl_scanout_disable,
    .dpy_gl_scanout_texture = dn_gl_scanout_texture,
    .dpy_gl_update          = dn_gl_flush,
};

static QEMUGLContext dn_gl_ctx_create(DisplayGLCtx *dgc, QEMUGLParams *params)
{
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   qemu_egl_rn_ctx);
    QEMUGLContext c = qemu_egl_create_context(dgc, params);
    /* Probe 0.2.21: context map (who is who: rn, virgl ctx0,
     * per-guest contexts) — without it, "ctx=0x4" in the census is mute. */
    dn_gl.ctx_creates++;
    DN_DLOG("gl ctx create #%u -> %p (share rn=%p, requested ES %d.%d)",
            dn_gl.ctx_creates, (void *)c, (void *)qemu_egl_rn_ctx,
            params ? params->major_ver : -1,
            params ? params->minor_ver : -1);
    return c;
}

static bool dn_gl_is_compatible_dcl(DisplayGLCtx *dgc, DisplayChangeListener *dcl)
{
    (void)dgc;
    if (!dcl->ops->dpy_gl_update) {
        return true; /* pure 2D listeners (our raster one) are fine */
    }
    return dcl->ops == &dn_gl_ops;
}

static const DisplayGLCtxOps dn_gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = dn_gl_is_compatible_dcl,
    .dpy_gl_ctx_create            = dn_gl_ctx_create,
    .dpy_gl_ctx_destroy           = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current      = qemu_egl_make_context_current,
};

static void dn_gl_attach(QemuConsole *con)
{
    if (dn_gl.inited || con == NULL) {
        return;
    }
    DisplayGLCtx *ctx = g_new0(DisplayGLCtx, 1);
    ctx->ops = &dn_gl_ctx_ops;
    qemu_console_set_display_gl_ctx(con, ctx);
    dn_gl.dcl.con = con;
    dn_gl.dcl.ops = &dn_gl_ops;
    register_displaychangelistener(&dn_gl.dcl);
    dn_gl.inited = true;
    DN_DLOG("gl listener attached (virgl scanout -> 2D surface readback)");
}

#endif /* CONFIG_OPENGL */

/* IOSurface handles are CF objects supplied by the app's bridge. This API is
 * intentionally plain C and optional at dlsym, so older engines and Linux
 * QEMU never need the IOSurface framework. The +1 acquire is released only
 * when Metal completes its command buffer, NOT when draw(in:) returns. */
bool dnq_display_iosurface_register(void *const surfaces[4], int32_t w, int32_t h)
{
#if defined(CONFIG_OPENGL) && defined(__APPLE__)
    return dn_gl_io_register(&dn_gl, surfaces, w, h);
#else
    (void)surfaces; (void)w; (void)h;
    return false;
#endif
}

void *dnq_display_iosurface_acquire(int32_t *w, int32_t *h,
                                    bool *flip_y, uint64_t *ticket)
{
#if defined(CONFIG_OPENGL) && defined(__APPLE__)
    DNIOPool *io = &dn_gl.io;
    if (!qatomic_read(&io->ready)) {
        return NULL;
    }
    void *surface = NULL;
    qemu_mutex_lock(&io->lock);
    int idx = io->latest;
    if (!io->disabled && idx >= 0 && idx < DN_IO_SLOTS &&
        !io->slot[idx].pending && !io->slot[idx].base_released) {
        DNIOSlot *s = &io->slot[idx];
        surface = s->surface;
        CFRetain((CFTypeRef)surface);
        s->metal_holds++;
        if (w) *w = io->width;
        if (h) *h = io->height;
        /* The previous glReadPixels output begins at GL's bottom row and
         * Swift displays that memory as Metal's first/top row. A directly
         * shared GLES FBO needs the corresponding vertical flip in Metal. */
        if (flip_y) *flip_y = true;
        if (ticket) *ticket = s->ticket;
    }
    qemu_mutex_unlock(&io->lock);
    return surface;
#else
    (void)w; (void)h; (void)flip_y; (void)ticket;
    return NULL;
#endif
}

void dnq_display_iosurface_release(void *surface)
{
#if defined(CONFIG_OPENGL) && defined(__APPLE__)
    if (!surface || !dn_gl.io.lock_inited) {
        return;
    }
    DNIOPool *io = &dn_gl.io;
    bool held = false;
    qemu_mutex_lock(&io->lock);
    for (int i = 0; i < DN_IO_SLOTS; i++) {
        DNIOSlot *s = &io->slot[i];
        if (s->surface == surface && s->metal_holds) {
            s->metal_holds--;
            if (s->base_released && !s->metal_holds) {
                s->surface = NULL;
            }
            held = true;
            break;
        }
    }
    qemu_mutex_unlock(&io->lock);
    if (held) {
        CFRelease((CFTypeRef)surface); /* balances acquire, even after VM cleanup */
    }
#else
    (void)surface;
#endif
}

void dnq_display_iosurface_disable(void)
{
#if defined(CONFIG_OPENGL) && defined(__APPLE__)
    dn_gl_io_disable(&dn_gl, "host Metal texture unavailable");
#endif
}

/* ------------------------------------------------------------ public API */

static void dn_apply_ui_info(QemuConsole *con);
static void dn_pointer_init(void);

void dnq_display_init(void)
{
    if (dn.inited) {
        return;
    }
    qemu_mutex_init(&dn.lock);
    dn.dcl.ops = &dn_dcl_ops;
    /* The default GUI refresh interval is 30 ms; on a 120 Hz phone
     * that alone caps scanout flips near 33 Hz and adds a frame of
     * touch-to-photon latency.  8 ms matches ProMotion while the
     * callback itself stays a cheap pending-flush check. */
    dn.dcl.update_interval = 8;
    dn.dcl.con = qemu_console_lookup_by_index(0);
    if (dn.dcl.con == NULL) {
        /* With -display none QEMU still creates a console for the graphics
         * device; NULL here means no virtio-gpu on the command line. Every
         * later symptom would just be a black screen, so say it loudly. */
        DN_DLOG("init FAILED: no graphics console[0] (virtio-gpu missing?)");
        return;
    }
#ifdef CONFIG_OPENGL
    /*
     * ORDER IS LOAD-BEARING (Log 16): register_displaychangelistener()
     * runs console_compatible_with(&error_fatal), and a virtio-gpu-gl
     * console (GRAPHIC_FLAGS_GL) without a DisplayGLCtx fails it with
     * "The console requires a GL context." -> process exit. egl-headless
     * sets the GL ctx BEFORE registering anything; we must too: attach
     * (ctx + GL listener) first, raster listener second.
     */
    if (dn_gl_ready) {
        dn_gl_attach(dn.dcl.con);
    }
#endif
    register_displaychangelistener(&dn.dcl);
    dn_pointer_init(); /* main-loop BH; UIKit never waits for the BQL */
    /* Announce the guest resolution + physical size (guest DPI) now
     * that the console exists; the guest reads the EDID seconds
     * later, when its DRM driver probes (0.2.25). */
    dn_apply_ui_info(dn.dcl.con);
    dn.inited = true;
    DN_DLOG("init OK: listener registered on console[0] graphic=%d",
            QEMU_IS_GRAPHIC_CONSOLE(dn.dcl.con) ? 1 : 0);
}

bool dnq_display_lock_frame(DNQFrameInfo *out)
{
    if (!dn.inited || out == NULL) {
        return false;
    }
    qemu_mutex_lock(&dn.lock);
    if (dn.surface == NULL) {
        qemu_mutex_unlock(&dn.lock);
        return false;
    }
    out->pixels     = surface_data(dn.surface);
    out->width      = surface_width(dn.surface);
    out->height     = surface_height(dn.surface);
    out->stride     = surface_stride(dn.surface);
    out->bpp        = surface_bits_per_pixel(dn.surface);
    out->generation = dn.generation;
    out->sequence   = qatomic_read(&dn.sequence);
    if (out->pixels == NULL || out->width <= 0 || out->height <= 0) {
        qemu_mutex_unlock(&dn.lock);
        return false;
    }
    return true; /* caller must dnq_display_unlock_frame() */
}

void dnq_display_unlock_frame(void)
{
    if (dn.inited) {
        qemu_mutex_unlock(&dn.lock);
    }
}

uint64_t dnq_display_sequence(void)
{
    return dn.inited ? qatomic_read(&dn.sequence) : 0;
}

bool dnq_display_take_damage(DNQDisplayDamage *out)
{
    if (!dn.inited || out == NULL) {
        return false;
    }
    qemu_mutex_lock(&dn.lock);
    if (dn.surface == NULL || !dn.damage_valid) {
        qemu_mutex_unlock(&dn.lock);
        return false;
    }
    int32_t surface_w = surface_width(dn.surface);
    int32_t surface_h = surface_height(dn.surface);
    int32_t x0 = dn.damage_x0 < 0 ? 0 : dn.damage_x0;
    int32_t y0 = dn.damage_y0 < 0 ? 0 : dn.damage_y0;
    int32_t x1 = dn.damage_x1 > surface_w ? surface_w : dn.damage_x1;
    int32_t y1 = dn.damage_y1 > surface_h ? surface_h : dn.damage_y1;
    dn.damage_valid = false;
    if (x1 <= x0 || y1 <= y0) {
        qemu_mutex_unlock(&dn.lock);
        return false;
    }
    out->x = x0;
    out->y = y0;
    out->width = x1 - x0;
    out->height = y1 - y0;
    out->generation = dn.generation;
    out->sequence = qatomic_read(&dn.sequence);
    qemu_mutex_unlock(&dn.lock);
    return true;
}

static QemuConsole *dn_input_console(void)
{
    if (dn.dcl.con) {
        return dn.dcl.con;
    }
    return qemu_console_lookup_by_index(0);
}

static void dn_console_size(QemuConsole *con, int *w, int *h)
{
    *w = con ? qemu_console_get_width(con, 0) : 0;
    *h = con ? qemu_console_get_height(con, 0) : 0;
    if (*w > 0 && *h > 0) {
        return;
    }
    qemu_mutex_lock(&dn.lock);
    *w = dn.surface ? surface_width(dn.surface) : 0;
    *h = dn.surface ? surface_height(dn.surface) : 0;
    qemu_mutex_unlock(&dn.lock);
}

void dnq_display_send_pointer(int32_t x, int32_t y, bool button_down)
{
    /* qemu_input_* needs the BQL; taking it for a few microseconds on the
     * UI thread beats marshalling through a bottom half for touch latency. */
    bool held = bql_locked();
    if (!held) {
        bql_lock();
    }
    QemuConsole *con = dn_input_console();
    int w = 0, h = 0;
    dn_console_size(con, &w, &h);
    if (con && w > 0 && h > 0) {
        if (x < 0) { x = 0; } else if (x >= w) { x = w - 1; }
        if (y < 0) { y = 0; } else if (y >= h) { y = h - 1; }
        qemu_input_queue_abs(con, INPUT_AXIS_X, x, 0, w);
        qemu_input_queue_abs(con, INPUT_AXIS_Y, y, 0, h);
        qemu_input_queue_btn(con, INPUT_BUTTON_LEFT, button_down);
        qemu_input_event_sync();
    }
    if (!held) {
        bql_unlock();
    }
}

/* Touch arrives on UIKit's main thread. The old direct wrapper waited for
 * QEMU's BQL there: a GPU readback / guest I/O could freeze a finger for an
 * entire frame. QEMU's main AIO BH is thread-safe to schedule from UIKit
 * (pinned QEMU util/async.c), and runs with the BQL already held. Keep a
 * FIFO for button transitions, but replace only the most recent redundant
 * motion in the same button state. Never coalesce a down/up edge. A bounded
 * overload keeps the LAST button state (no permanently stuck finger). */
#define DN_POINTER_MAX_BACKLOG 512

typedef struct DNPointerEvent {
    int32_t x, y;
    bool down, edge;
    struct DNPointerEvent *next;
} DNPointerEvent;

typedef struct DNPointerQueue {
    QemuMutex lock;
    QEMUBH *bh;
    DNPointerEvent *head, *tail;
    unsigned backlog;
    bool closing, queued_down, emergency_valid, overload_logged;
    DNPointerEvent emergency; /* only used at OOM / pathological overflow */
} DNPointerQueue;

static DNPointerQueue dn_pointer;
static bool dn_pointer_ready; /* qatomic: published AFTER lock/BH creation */

static void dn_pointer_drain_bh(void *opaque)
{
    DNPointerQueue *p = opaque;
    /* Detach a bounded FIFO snapshot. Any UIKit input arriving DURING this
     * callback schedules the BH for a later poll, rather than extending a
     * potentially unbounded BQL-held loop. QEMU 10 BHs run recursively if
     * rescheduled from themselves, so do NOT reschedule here. */
    qemu_mutex_lock(&p->lock);
    DNPointerEvent *node = p->head;
    DNPointerEvent emergency = p->emergency;
    bool has_emergency = p->emergency_valid;
    p->head = p->tail = NULL;
    p->backlog = 0;
    p->emergency_valid = false;
    bool closing = p->closing;
    qemu_mutex_unlock(&p->lock);
    while (node) {
        DNPointerEvent *next = node->next;
        if (!closing) {
            dnq_display_send_pointer(node->x, node->y, node->down);
        }
        g_free(node);
        node = next;
    }
    if (has_emergency && !closing) {
        dnq_display_send_pointer(emergency.x, emergency.y, emergency.down);
    }
}

static void dn_pointer_init(void)
{
    DNPointerQueue *p = &dn_pointer;
    if (qatomic_read(&dn_pointer_ready)) {
        return;
    }
    qemu_mutex_init(&p->lock);
    p->closing = false;
    p->bh = aio_bh_new(qemu_get_aio_context(), dn_pointer_drain_bh, p);
    qatomic_set(&dn_pointer_ready, true);
    DN_DLOG("pointer input: FIFO main-loop BH enabled; UIKit never takes BQL");
}

bool dnq_display_send_pointer_async(int32_t x, int32_t y, bool down)
{
    if (!qatomic_read(&dn_pointer_ready)) {
        return false; /* no QEMU UI yet / shutting down: never take BQL */
    }
    DNPointerQueue *p = &dn_pointer;
    bool log_overload = false;
    qemu_mutex_lock(&p->lock);
    if (p->closing || !p->bh) {
        qemu_mutex_unlock(&p->lock);
        return false;
    }
    bool edge = down != p->queued_down;
    if (!edge && !p->emergency_valid && p->tail &&
        !p->tail->edge && p->tail->down == down) {
        p->tail->x = x;
        p->tail->y = y; /* newest position of THIS motion interval */
    } else if (!p->emergency_valid && p->backlog < DN_POINTER_MAX_BACKLOG) {
        DNPointerEvent *node = g_try_new(DNPointerEvent, 1);
        if (node) {
            *node = (DNPointerEvent){ .x = x, .y = y, .down = down,
                                      .edge = edge };
            if (p->tail) {
                p->tail->next = node;
            } else {
                p->head = node;
            }
            p->tail = node;
            p->backlog++;
        } else {
            p->emergency_valid = true;
        }
    } else {
        p->emergency_valid = true;
    }
    if (p->emergency_valid) {
        p->emergency = (DNPointerEvent){ .x = x, .y = y, .down = down,
                                         .edge = edge };
        if (!p->overload_logged) {
            p->overload_logged = true;
            log_overload = true;
        }
    }
    p->queued_down = down;
    qemu_bh_schedule(p->bh); /* wait-free AIO wakeup; no BQL on UIKit */
    qemu_mutex_unlock(&p->lock);
    if (log_overload) {
        DN_DLOG("pointer FIFO overflow/OOM: retaining last button state after queued transitions");
    }
    return true;
}

void dnq_display_shutdown(void)
{
#if defined(CONFIG_OPENGL) && defined(__APPLE__)
    dn_gl_io_shutdown(&dn_gl); /* CF base refs; Metal holds outlive cleanup */
#endif
    if (!qatomic_read(&dn_pointer_ready)) {
        return;
    }
    qatomic_set(&dn_pointer_ready, false);
    DNPointerQueue *p = &dn_pointer;
    qemu_mutex_lock(&p->lock);
    p->closing = true;
    DNPointerEvent *node = p->head;
    p->head = p->tail = NULL;
    p->backlog = 0;
    p->emergency_valid = false;
    QEMUBH *bh = p->bh;
    p->bh = NULL;
    qemu_mutex_unlock(&p->lock);
    while (node) {
        DNPointerEvent *next = node->next;
        g_free(node);
        node = next;
    }
    if (bh) {
        qemu_bh_cancel(bh);
        qemu_bh_delete(bh);
    }
}

/* ASCII -> QKeyCode (US layout). NOTE: the QKeyCode enum is in QWERTY
 * physical order, not alphabetical -- never do arithmetic on it; the
 * tables below are indexed alphabetically / by digit explicitly. */
static bool dn_key_for_char(char c, QKeyCode *out, bool *shift)
{
    static const QKeyCode letters[26] = {
        Q_KEY_CODE_A, Q_KEY_CODE_B, Q_KEY_CODE_C, Q_KEY_CODE_D,
        Q_KEY_CODE_E, Q_KEY_CODE_F, Q_KEY_CODE_G, Q_KEY_CODE_H,
        Q_KEY_CODE_I, Q_KEY_CODE_J, Q_KEY_CODE_K, Q_KEY_CODE_L,
        Q_KEY_CODE_M, Q_KEY_CODE_N, Q_KEY_CODE_O, Q_KEY_CODE_P,
        Q_KEY_CODE_Q, Q_KEY_CODE_R, Q_KEY_CODE_S, Q_KEY_CODE_T,
        Q_KEY_CODE_U, Q_KEY_CODE_V, Q_KEY_CODE_W, Q_KEY_CODE_X,
        Q_KEY_CODE_Y, Q_KEY_CODE_Z,
    };
    static const QKeyCode digits[10] = {
        Q_KEY_CODE_0, Q_KEY_CODE_1, Q_KEY_CODE_2, Q_KEY_CODE_3,
        Q_KEY_CODE_4, Q_KEY_CODE_5, Q_KEY_CODE_6, Q_KEY_CODE_7,
        Q_KEY_CODE_8, Q_KEY_CODE_9,
    };
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        *out = letters[c - 'a'];
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *out = letters[c - 'A'];
        *shift = true;
        return true;
    }
    if (c >= '0' && c <= '9') {
        *out = digits[c - '0'];
        return true;
    }
    switch (c) {
    case '\n': case '\r': *out = Q_KEY_CODE_RET; return true;
    case '\t': *out = Q_KEY_CODE_TAB; return true;
    case 0x7f: *out = Q_KEY_CODE_BACKSPACE; return true;
    case ' ': *out = Q_KEY_CODE_SPC; return true;
    case '-': *out = Q_KEY_CODE_MINUS; return true;
    case '_': *out = Q_KEY_CODE_MINUS; *shift = true; return true;
    case '=': *out = Q_KEY_CODE_EQUAL; return true;
    case '+': *out = Q_KEY_CODE_EQUAL; *shift = true; return true;
    case '[': *out = Q_KEY_CODE_BRACKET_LEFT; return true;
    case '{': *out = Q_KEY_CODE_BRACKET_LEFT; *shift = true; return true;
    case ']': *out = Q_KEY_CODE_BRACKET_RIGHT; return true;
    case '}': *out = Q_KEY_CODE_BRACKET_RIGHT; *shift = true; return true;
    case ';': *out = Q_KEY_CODE_SEMICOLON; return true;
    case ':': *out = Q_KEY_CODE_SEMICOLON; *shift = true; return true;
    case '\'': *out = Q_KEY_CODE_APOSTROPHE; return true;
    case '"': *out = Q_KEY_CODE_APOSTROPHE; *shift = true; return true;
    case '`': *out = Q_KEY_CODE_GRAVE_ACCENT; return true;
    case '~': *out = Q_KEY_CODE_GRAVE_ACCENT; *shift = true; return true;
    case '\\': *out = Q_KEY_CODE_BACKSLASH; return true;
    case '|': *out = Q_KEY_CODE_BACKSLASH; *shift = true; return true;
    case ',': *out = Q_KEY_CODE_COMMA; return true;
    case '<': *out = Q_KEY_CODE_COMMA; *shift = true; return true;
    case '.': *out = Q_KEY_CODE_DOT; return true;
    case '>': *out = Q_KEY_CODE_DOT; *shift = true; return true;
    case '/': *out = Q_KEY_CODE_SLASH; return true;
    case '?': *out = Q_KEY_CODE_SLASH; *shift = true; return true;
    case '!': *out = Q_KEY_CODE_1; *shift = true; return true;
    case '@': *out = Q_KEY_CODE_2; *shift = true; return true;
    case '#': *out = Q_KEY_CODE_3; *shift = true; return true;
    case '$': *out = Q_KEY_CODE_4; *shift = true; return true;
    case '%': *out = Q_KEY_CODE_5; *shift = true; return true;
    case '^': *out = Q_KEY_CODE_6; *shift = true; return true;
    case '&': *out = Q_KEY_CODE_7; *shift = true; return true;
    case '*': *out = Q_KEY_CODE_8; *shift = true; return true;
    case '(': *out = Q_KEY_CODE_9; *shift = true; return true;
    case ')': *out = Q_KEY_CODE_0; *shift = true; return true;
    default: return false;
    }
}

void dnq_display_send_text(const char *utf8)
{
    if (!utf8) {
        return;
    }
    bool held = bql_locked();
    if (!held) {
        bql_lock();
    }
    QemuConsole *con = dn_input_console();
    if (con) {
        const unsigned char *p = (const unsigned char *)utf8;
        for (; *p; p++) {
            QKeyCode code;
            bool shift;
            if (*p >= 0x80) {
                continue; /* non-ASCII: skip (US keymap only) */
            }
            if (!dn_key_for_char((char)*p, &code, &shift)) {
                continue;
            }
            if (shift) {
                qemu_input_event_send_key_qcode(con, Q_KEY_CODE_SHIFT, true);
            }
            qemu_input_event_send_key_qcode(con, code, true);
            qemu_input_event_send_key_qcode(con, code, false);
            if (shift) {
                qemu_input_event_send_key_qcode(con, Q_KEY_CODE_SHIFT, false);
            }
            qemu_input_event_sync();
        }
    }
    if (!held) {
        bql_unlock();
    }
}

/* Guest display density (0.2.25): the app's chosen guest DPI and the
 * last UI size it announced. virtio-gpu turns QemuUIInfo.width_mm /
 * height_mm into the EDID physical size; the guest kernel reports it
 * on the DRM connector and Android's SurfaceFlinger derives the UI
 * density from it. Without mm the generated EDID claims 100 DPI and
 * Android renders everything tiny at native resolution. */
static int32_t dn_guest_dpi = 560;
static int32_t dn_ui_w, dn_ui_h;
/* QEMU 10's EDID generator uses 75 Hz when refresh_rate is zero. The
 * pinned UTM fork additionally treats a NONZERO host refresh >= 61 Hz as
 * a CVT timing: explicitly writing 75000 would NOT reproduce the old
 * EDID. We therefore send 0 to QEMU for the 75 Hz legacy fallback.
 * The 55 Hz experiment did not improve iPhone UI and would preclude a
 * true 60 Hz guest mode. Restore the working 75 Hz legacy EDID as the
 * fallback; preserve explicit 55 Hz choices for A/B. The fork adapts
 * the EDID range descriptor to the selected timing. */
static uint32_t dn_guest_edid_refresh_millihz = 75000;

static void dn_apply_ui_info(QemuConsole *con)
{
    if (!con || dn_ui_w <= 0 || dn_ui_h <= 0) {
        return;
    }
    QemuUIInfo info = *dpy_get_ui_info(con);
    info.width = dn_ui_w;
    info.height = dn_ui_h;
    info.width_mm = (dn_ui_w * 254 + dn_guest_dpi * 5) / (dn_guest_dpi * 10);
    info.height_mm = (dn_ui_h * 254 + dn_guest_dpi * 5) / (dn_guest_dpi * 10);
    /* 0 is the pre-0.4.6 behavior. The UTM EDID fork gives a nonzero
     * 75000 a DIFFERENT CVT timing, so preserve the legacy byte stream. */
    info.refresh_rate = dn_guest_edid_refresh_millihz == 75000 ?
        0 : dn_guest_edid_refresh_millihz;
    if (info.width_mm < 1) {
        info.width_mm = 1;
    }
    if (info.height_mm < 1) {
        info.height_mm = 1;
    }
    dpy_set_ui_info(con, &info, false);
    DN_DLOG("ui info: %dx%d px, %dx%d mm (guest dpi %d), selected EDID %u Hz (QEMU UI %u mHz)",
            info.width, info.height, info.width_mm, info.height_mm,
            dn_guest_dpi, dn_guest_edid_refresh_millihz / 1000,
            info.refresh_rate);
}

/* QEMU snapshots restore guest RAM and virtio device state, but not the UI
 * console's QemuUIInfo. Re-signal VIRTIO_GPU_EVENT_DISPLAY after a restore
 * so an existing 0.4.5 snapshot can re-probe the new EDID without a cold
 * boot. QEMU's dpy_set_ui_info ignores identical info, so briefly change
 * the stored refresh; its timer runs only after we restore the desired
 * value, and the guest sees ONE final notification, never the transient.
 * Caller: dnq_snapshot_load_at_startup, on the QEMU main thread, BQL held. */
void dn_display_reannounce_ui_info(void)
{
    QemuConsole *con = dn.dcl.con;
    if (!dn.inited || !con || dn_ui_w <= 0 || dn_ui_h <= 0) {
        return;
    }
    QemuUIInfo info = *dpy_get_ui_info(con);
    /* The legacy 75 Hz choice ALREADY stores zero in QEMU UI info.
     * Toggle to the other value so even a 75->75 restore still causes
     * exactly one notification after dn_apply_ui_info restores zero. */
    info.refresh_rate = dn_guest_edid_refresh_millihz == 75000 ? 55000 : 0;
    dpy_set_ui_info(con, &info, false);
    dn_apply_ui_info(con);
    DN_DLOG("snapshot restored: re-announced selected EDID %u Hz",
            dn_guest_edid_refresh_millihz / 1000);
}

void dnq_display_set_guest_refresh_rate(int32_t hz)
{
    /* Called by the app before qemu_init; an invalid/stale UserDefaults
     * value cannot expose an out-of-range timing. No live switch while a
     * guest is running: it would confound the controlled A/B interval. */
    if (!dn.inited && (hz == 55 || hz == 75)) {
        dn_guest_edid_refresh_millihz = (uint32_t)hz * 1000;
    }
}

void dnq_display_set_guest_dpi(int32_t dpi)
{
    if (dpi >= 120 && dpi <= 960) {
        dn_guest_dpi = dpi;
    }
}

void dnq_display_set_ui_size(int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    dn_ui_w = width;
    dn_ui_h = height;
    if (!dn.inited) {
        /* Called before qemu_init (the app announces size+DPI right
         * after dlopen): the BQL and the consoles do not exist yet —
         * touching bql_locked()/bql_lock() here aborts the process
         * (device, 0.2.25 first run). Just stash; dnq_display_init
         * applies the values once the console exists. */
        return;
    }
    bool held = bql_locked();
    if (!held) {
        bql_lock();
    }
    dn_apply_ui_info(dn_input_console());
    if (!held) {
        bql_unlock();
    }
}

void dnq_display_guest_size(int32_t *width, int32_t *height)
{
    bool held = bql_locked();
    if (!held) {
        bql_lock();
    }
    int w = 0, h = 0;
    dn_console_size(dn_input_console(), &w, &h);
    if (!held) {
        bql_unlock();
    }
    if (width)  *width = w;
    if (height) *height = h;
}

/* Direct Metal presentation (0.2.24): the current scanout's
 * MTLTexture, refcount +1 for the caller (release with
 * dnq_display_metal_release). NULL when the scanout has no native
 * handle (firmware/classic readback). Safe from any thread. */
void *dnq_display_metal_acquire(int32_t *w, int32_t *h,
                                bool *y0top, uint64_t *gen)
{
    DNGLState *gl = &dn_gl;
    dn_gl_mtl_ensure_lock(gl);
    qemu_mutex_lock(&gl->mtl_lock);
    void *tex = gl->mtl_tex;
    if (tex) {
        DN_MTL_RETAIN(tex);
        if (w) { *w = gl->mtl_w; }
        if (h) { *h = gl->mtl_h; }
        if (y0top) { *y0top = gl->mtl_y0top; }
        if (gen) { *gen = gl->mtl_seq; } /* 0.2.28: flush seq = host latch key */
    }
    qemu_mutex_unlock(&gl->mtl_lock);
    return tex;
}

void dnq_display_metal_release(void *tex)
{
    DN_MTL_RELEASE(tex);
}
