/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DroidNative display/input bridge -- compiled INSIDE the QEMU library
 * (ui/dn-display.c), following the same architecture Husk uses: QEMU's
 * console.h cannot be compiled from an Xcode app target, so everything the
 * app needs is exported from the dylib as plain C. NOTE: on Darwin the
 * dylib link is restricted by system/qemu.symbols (-exported_symbols_list,
 * only ~66 symbols leave the library), so DN_QEMU_EXPORT alone is NOT
 * enough -- every dnq_* symbol must also be listed there (done at build
 * time by patches/dn-qemu-symbols.patch; verified with nm -gU).
 *
 * Written against the public QEMU UI API (DisplayChangeListener,
 * qemu_input_*), the same one ui/cocoa.m uses. See docs/11-HUSK-INTEGRARE.md.
 */
#ifndef DN_DISPLAY_H
#define DN_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DN_QEMU_EXPORT __attribute__((visibility("default")))

typedef struct DNQFrameInfo {
    const void *pixels;
    int32_t width;
    int32_t height;
    int32_t stride;       /* bytes per row */
    uint32_t bpp;         /* 32 for the formats we accept */
    uint64_t generation;  /* bumps when the surface is replaced (resize) */
    uint64_t sequence;    /* bumps on every guest draw */
} DNQFrameInfo;

/* Coalesced guest damage since the previous take (bounding union of
 * every dpy_gfx_update rectangle).  The app uploads only this region
 * of the scanout into its Metal texture instead of re-uploading the
 * full phone-sized surface on every guest draw. */
typedef struct DNQDisplayDamage {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint64_t generation;  /* surface generation the rectangle refers to */
    uint64_t sequence;    /* latest guest draw included in the union */
} DNQDisplayDamage;

/* Register the DisplayChangeListener. Call on the QEMU thread after
 * qemu_init() returns (BQL held) and before qemu_main_loop(). */
DN_QEMU_EXPORT void dnq_display_init(void);
/* Faza B (docs/12): EGL/ANGLE display bring-up. MUST run before
 * qemu_init() — virtio-gpu-gl initializes virgl at device realize and
 * needs the EGL display to already exist. Returns false (and GL stays
 * off) when QEMU was built without OpenGL/EGL or ANGLE fails to init. */
DN_QEMU_EXPORT bool dnq_gl_early_init(void);

/* Borrow the current frame (pixels valid until unlock). False = no surface
 * yet. The lock blocks surface replacement: copy/upload, then unlock fast. */
DN_QEMU_EXPORT bool dnq_display_lock_frame(DNQFrameInfo *out);
DN_QEMU_EXPORT void dnq_display_unlock_frame(void);

/* Cheap poll: current draw sequence (0 = nothing drawn / unavailable). */
DN_QEMU_EXPORT uint64_t dnq_display_sequence(void);

/* Take (and clear) the coalesced damage rectangle accumulated since
 * the previous take.  False = nothing damaged / no surface yet.  Call
 * it without holding the frame lock, then lock the frame and compare
 * generations before uploading only the damaged region. */
DN_QEMU_EXPORT bool dnq_display_take_damage(DNQDisplayDamage *out);

/* Absolute pointer (virtio-tablet), guest pixel coordinates. Legacy call
 * takes the BQL synchronously (kept for old clients, never used by the
 * current UIKit path). */
DN_QEMU_EXPORT void dnq_display_send_pointer(int32_t x, int32_t y, bool button_down);
/* Enqueue UIKit touch without taking BQL. A main-loop BH preserves down/up
 * order, coalesces only moves and applies guest input under QEMU's BQL.
 * Returns false if the guest UI is not running; do not sync-fallback then. */
DN_QEMU_EXPORT bool dnq_display_send_pointer_async(int32_t x, int32_t y, bool button_down);
/* Called by the QEMU launch bridge with BQL held BEFORE qemu_cleanup;
 * prevents an outstanding BH from touching a destroyed guest console. */
DN_QEMU_EXPORT void dnq_display_shutdown(void);

/* Optional iOS EAGL -> BGRA IOSurface -> Metal path (four surfaces created
 * by the app, validated against the pinned ANGLE EGL config by QEMU).
 * Register on QEMU's thread after dnq_display_init, before its main loop.
 * No IOSurface dependencies in generic Linux QEMU builds. On any failure
 * PBO remains the active presenter; this is not Android FPS telemetry. */
DN_QEMU_EXPORT bool dnq_display_iosurface_register(void *const surfaces[4],
                                                   int32_t width, int32_t height);
/* +1 CF object, with a QEMU hold. Release AFTER the Metal command buffer
 * finishes, or immediately if makeTexture/presentation cannot start. The
 * y-flip is the GLES FBO -> Metal native-storage orientation. */
DN_QEMU_EXPORT void *dnq_display_iosurface_acquire(int32_t *width, int32_t *height,
                                                   bool *flip_y, uint64_t *ticket);
DN_QEMU_EXPORT void dnq_display_iosurface_release(void *surface);
/* Host Metal texture failure: stop advertising IOSurfaces, use PBO next flush. */
DN_QEMU_EXPORT void dnq_display_iosurface_disable(void);

/* Type ASCII text into the guest as virtio-keyboard key events
 * (US layout; uppercase/punctuation get SHIFT wrapped). '\n' = Enter,
 * '\t' = Tab, 0x7f = Backspace; non-ASCII bytes are skipped. The
 * serial console of the current GSI has no shell (androidboot.console
 * is hvc0, which we do not wire), so this keyboard path is how the
 * user actually types into Android. Takes the BQL internally. */
DN_QEMU_EXPORT void dnq_display_send_text(const char *utf8);

/* Ask the guest to modeset to w*h (dpy_set_ui_info); guest may refuse. */
DN_QEMU_EXPORT void dnq_display_set_ui_size(int32_t width, int32_t height);

/* The guest's actual display size right now (console, not surface). */
DN_QEMU_EXPORT void dnq_display_guest_size(int32_t *width, int32_t *height);

/* Direct Metal presentation (0.2.24, docs/12): the current
 * scanout's MTLTexture (virgl/ANGLE native handle), retained +1 for
 * the caller; release with dnq_display_metal_release. On
 * ANGLE/Metal the GL texture name of guest 3D scanouts is not
 * readable from the listener context (FBO blit = 0x506, sampling =
 * opaque black) while the firmware console works -- so the app
 * draws this texture itself, Husk/UTM-style, no readback. NULL =
 * no native handle for the current scanout (classic path). */
DN_QEMU_EXPORT void *dnq_display_metal_acquire(int32_t *w, int32_t *h,
                                               bool *y0top, uint64_t *gen);
DN_QEMU_EXPORT void dnq_display_metal_release(void *tex);
/* Guest display density for the EDID physical size (default 560). */
DN_QEMU_EXPORT void dnq_display_set_guest_dpi(int32_t dpi);
/* Guest preferred EDID refresh, set BEFORE qemu_init. Only 55 Hz
 * (test mode) and 75 Hz (pre-0.4.6 fallback, QEMU UI info = 0) are
 * accepted. The host refresh interval (8 ms) is independent. */
DN_QEMU_EXPORT void dnq_display_set_guest_refresh_rate(int32_t hz);

/* Internal-only handoff: after restoring a snapshot, signal virtio-gpu
 * to re-probe the current UI EDID. Called with the QEMU BQL held. This
 * is linked inside the engine; it is NOT an app-facing dylib export. */
void dn_display_reannounce_ui_info(void);

/* Snapshot (implemented in ui/dn-snapshot.c). Save is asynchronous
 * (bottom half on the main loop); poll dnq_snapshot_state():
 * 0 idle, 1 saving, 2 saved ok, 3 failed. */
DN_QEMU_EXPORT void dnq_snapshot_save(void);
DN_QEMU_EXPORT int dnq_snapshot_state(void);
/* Call right after qemu_init() (BQL held). False = nothing to restore. */
DN_QEMU_EXPORT bool dnq_snapshot_load_at_startup(void);

#ifdef __cplusplus
}
#endif

#endif /* DN_DISPLAY_H */
