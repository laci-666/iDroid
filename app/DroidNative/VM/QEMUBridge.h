#ifndef DN_QEMU_BRIDGE_H
#define DN_QEMU_BRIDGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Dynamically loads the qemu-aarch64-softmmu framework from the bundle and resolves
/// API-ul de lansare al fork-ului UTM (`qemu_init`, `qemu_main_loop`,
/// `qemu_cleanup`, plus the BQL/replay primitive from `system/main.c`).
/// On failure, `error_out` gets a malloc'd message; the caller frees it
/// elibereaza cu `dn_qemu_free_error`.
bool dn_qemu_load(const char *library_path, char **error_out);

/// Runs QEMU on the current thread until guest stop.
/// Arguments are joined with the 0x01 separator (does not exist in valid argv),
/// incepand cu argv[0]. Replica secventa din `system/main.c`, dar in loc de
/// `exit(status)` returns the status to Swift. `breadcrumb_path` (can be
/// NULL) receives, synchronously on disk, the last stage reached — the diagnostic
/// survives an exit/abort in qemu_init.
int dn_qemu_run(const char *arguments_joined, const char *breadcrumb_path);

/* Ca dn_qemu_run, plus restaurarea snapshot-ului intern ("dn-ready") imediat
 * after qemu_init, before the main loop — instant-resume. If there is no
 * snapshot or the QEMU framework is old (no symbols), normal boot. */
int dn_qemu_run_ex(const char *arguments_joined, const char *breadcrumb_path, bool load_snapshot);

void dn_qemu_free_error(char *error_message);

/* Crash trap (0.2.8): installs async-signal-safe handlers for
 * the fatal signals (SEGV/BUS/ABRT/ILL/FPE/SYS) that write a marker line
 * into the breadcrumb file then re-raise the signal (the .ips report is
 * generated normally). A crash WITHOUT the "99 crash-trap" line in the breadcrumb
 * inseamna ca iOS a omorat procesul (jetsam/watchdog), nu o greseala de
 * memory in code — the distinction Log crash 1 could not make.
 * Idempotent; call at launch and after qemu_init. */
void dn_install_crash_trap(const char *breadcrumb_path);

/* Isolated GPU trial: APFS copy-on-write clone of ONE regular file into a
 * previously nonexistent path. Returns 0 or a POSIX errno; never falls back
 * to a slow full copy. The app clones only its work QCOW2 disks + EFI vars,
 * leaving the software profile and its g0 snapshot untouched. */
int dn_gpu_trial_clone_file(const char *source, const char *destination);

/* ---- Display/touch/snapshot din QEMU (app/QEMUExtra, docs/11) -------------
 * All optional at runtime: if the bundled QEMU framework does not export
 * the dnq_* symbols (old build, pre-integration), these functions
 * degrade silently (black screen/inactive touch), and serial boot works
 * as before. No crash on backwards compatibility. */

#include <stdint.h>

bool dn_display_available(void);

typedef struct DNDisplayFrame {
    const void *pixels;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t bpp;
    uint64_t generation;
    uint64_t sequence;
} DNDisplayFrame;

bool dn_display_lock_frame(DNDisplayFrame *out);
void dn_display_unlock_frame(void);
uint64_t dn_display_sequence(void);

typedef struct DNDisplayDamage {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint64_t generation;
    uint64_t sequence;
} DNDisplayDamage;

/* Coalesced guest damage since the previous take; false when the
 * bundled QEMU framework predates the damage bridge or nothing is
 * damaged.  The app uploads only this region of the scanout. */
bool dn_display_take_damage(DNDisplayDamage *out);
/* Current QEMU: nonblocking FIFO on QEMU's main AIO loop, preserving button
 * transitions; older frameworks fall back to their synchronous BQL path. */
void dn_display_send_pointer(int32_t x, int32_t y, bool button_down);
/// Tasteaza text ASCII in guest ca evenimente de tastatura virtio
/// ('\n' = Enter). No-op without dnq_display_send_text in the framework.
void dn_display_send_text(const char *utf8);
/// Direct Metal presentation (0.2.24): the current scanout's MTLTexture
/// (virgl/ANGLE native handle), +1 refcount for the caller;
/// NULL when there is none. Release: dn_display_metal_release.
void *dn_display_metal_acquire(int32_t *w, int32_t *h, bool *y0top, uint64_t *gen);
void dn_display_metal_release(void *tex);
/* Call before QEMU thread starts: only selected 2-vCPU isolated EAGL clone
 * at <=720x1280. The bridge creates four public BGRA IOSurfaces after QEMU
 * init; a missing config/extension/texture silently retains the PBO path. */
void dn_display_configure_iosurface(bool enabled, int32_t width, int32_t height);
/* Acquired +1 IOSurface belongs to QEMU until Metal command completion;
 * release exactly once even if no drawable/texture could be made. */
void *dn_display_iosurface_acquire(int32_t *width, int32_t *height,
                                   bool *flip_y, uint64_t *ticket);
void dn_display_iosurface_release(void *surface);
void dn_display_iosurface_disable(void); /* texture failure -> PBO next frame */
void dn_display_set_guest_dpi(int32_t dpi);
/* Preferred EDID refresh on the next guest start: 55/75 Hz. */
void dn_display_set_guest_refresh_rate(int32_t hz);
void dn_display_set_ui_size(int32_t width, int32_t height);
void dn_display_guest_size(int32_t *width, int32_t *height);

/* Internal snapshot on userdata (dn-ready). Asynchronous save; polled state:
 * 0 idle, 1 saving, 2 saved, 3 failed, -1 unavailable. */
void dn_snapshot_save(void);
int dn_snapshot_state(void);

#ifdef __cplusplus
}
#endif

#endif /* DN_QEMU_BRIDGE_H */
