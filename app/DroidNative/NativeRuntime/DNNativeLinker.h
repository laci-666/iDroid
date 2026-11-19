#ifndef DN_NATIVE_LINKER_H
#define DN_NATIVE_LINKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DroidNative's own mini-linker for signed, converted ELF images.
 *
 * The Mach-O wrapper around the ELF is loaded by dyld (dlopen).  dyld
 * does not know anything about the ELF payload inside __TEXT,__elf;
 * this linker finishes the job an Android dynamic linker would do:
 *
 *   - parse ELF64 program/dynamic headers from the mapped image;
 *   - verify DT_NEEDED dependencies already linked (or host-backed);
 *   - preflight and apply RELR + AArch64 RELA relocations;
 *   - bind imports to DN shims, other converted images, then the host;
 *   - run DT_INIT / DT_INIT_ARRAY exactly once;
 *   - resolve exported ELF symbols for direct native calls.
 *
 * iDroid 0.4 closes the runtime contract needed by translated code:
 * version-1 patch islands are bound to DN's svc/thread-pointer
 * entries before guest code runs; PT_TLS is registered with DN's
 * bionic-shaped TLS storage; TLSDESC, TLS_DTPMOD64/DTPREL64 (+
 * __tls_get_addr), TLS_TPREL64 and Android APS2 packed RELA/REL are
 * decoded and preflighted.  TEXTREL remains refused because it would
 * require rewriting signed code pages.  Anything still outside the
 * implemented ELF/ABI surface fails closed before relocations run.
 */
typedef struct dn_native_image dn_native_image_t;

/* Link a converted library already dlopen'ed from `path`.  Returns 0
 * on success.  On failure an image may still be returned solely so the
 * report can be read; call dn_native_image_dispose() in both cases.
 * This function does not dlclose the handle. */
int dn_native_link_image(const char *path, void *dlopen_handle,
                         dn_native_image_t **out_image);

/* Optional synchronous progress sink for linker report lines.  The
 * in-memory image report is readable only if the process survives the
 * link; when a path is set, every report line is also appended to
 * that file before the linker moves on, so a crash inside an
 * initializer still leaves the exact sub-stage on disk.  Pass NULL
 * to disable.  The path is copied. */
void dn_native_set_link_progress_path(const char *path);

/* Human-readable, newline-separated linker report owned by image. */
const char *dn_native_image_report(dn_native_image_t *image);

/* Resolve an exported ELF symbol.  Returns NULL when absent. */
void *dn_native_image_symbol(dn_native_image_t *image, const char *name);

/* Resolve a symbol from any already-linked converted image.  This is
 * the global scope used by the guest-visible dlopen/dlsym layer. */
void *dn_native_global_symbol(const char *name);

/* Run DT_FINI_ARRAY (reverse) and DT_FINI once.  Call before dlclose. */
int dn_native_image_run_finalizers(dn_native_image_t *image);

/* Remove the image from the dependency registry and release one
 * reference.  Does not dlclose; the caller owns the dlopen handle. */
void dn_native_image_dispose(dn_native_image_t *image);

#if defined(DN_NATIVE_LINKER_TEST_HOOKS)
/* Host-side tests only: link an already mapped ELF image at `base`.
 * `run_initializers` may be 0 when the test host cannot execute the
 * guest ISA (the parser/relocation suite runs on x86_64 Linux too). */
int dn_native_link_memory_for_test(void *base, const char *path_label,
                                   int run_initializers,
                                   dn_native_image_t **out_image);

/* Same seam, with an explicit converter patch-island slot array, so
 * host tests can prove the linker fills all three runtime slots. */
int dn_native_link_memory_with_patch_for_test(void *base, const char *path_label,
                                              int run_initializers,
                                              void *patch_slots,
                                              unsigned patch_svc_sites,
                                              unsigned patch_tpidr_sites,
                                              dn_native_image_t **out_image);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DN_NATIVE_LINKER_H */
