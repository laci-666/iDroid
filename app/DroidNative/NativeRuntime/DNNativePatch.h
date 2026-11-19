#ifndef DN_NATIVE_PATCH_H
#define DN_NATIVE_PATCH_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dn_native_tls_module dn_native_tls_module_t;

/*
 * Runtime half of DroidNative's ARM64 code-translation contract.
 *
 * Converted images may contain branch islands for the two Android
 * instructions Darwin cannot execute with Android semantics:
 *
 *   - svc #0            -> dn_native_svc_entry -> Linux syscall layer
 *   - mrs Xt,tpidr_el0 -> a DN-managed, bionic-shaped thread pointer
 *
 * The public island ABI consumed by DNNativeLinker.c is the version-1
 * three-slot interface: [0]=svc entry, [1]=byte offset of DN's TP
 * pthread key in Darwin's TSD array, [2]=TP allocator entry.  The
 * implementation in DNNativePatch.c is DN's own: bionic TLS slots,
 * ELF TLS storage, Linux errno/path/syscall translation and register
 * preserving AArch64 entry points.
 */
int dn_native_patch_init(char *error, size_t error_len);
uintptr_t dn_native_patch_tp_key_offset(void);
void dn_native_svc_entry(void);
void dn_native_tp_slow_entry(void);
void dn_native_tlsdesc_entry(void);
void *dn_native_tp_allocate_slow(void);
void *dn_native_thread_pointer(void);
long dn_native_syscall_dispatch(long a0, long a1, long a2, long a3,
                                long a4, long a5, long nr);

/* ELF TLS registry used by the mini-linker.  One registered module is
 * one PT_TLS segment.  Storage is part of DN's thread-pointer block,
 * so TLSDESC, DTPMOD/DTPREL (+__tls_get_addr) and initial-exec TPREL
 * all name the same bytes. */
dn_native_tls_module_t *dn_native_tls_register(const void *template_image,
                                               size_t file_size,
                                               size_t memory_size,
                                               size_t alignment);
void dn_native_tls_unregister(dn_native_tls_module_t *module);
uint64_t dn_native_tls_module_id(const dn_native_tls_module_t *module);
uint64_t dn_native_tls_module_offset(const dn_native_tls_module_t *module);
void *dn_native_tls_descriptor_arg(dn_native_tls_module_t *module,
                                   uint64_t offset_within_module);
void *dn_native_tls_address(dn_native_tls_module_t *module,
                            uint64_t offset_within_module);
intptr_t dn_native_tlsdesc_resolve(const uintptr_t *descriptor);
void *dn_native_tls_get_addr(void *tls_index);

/* Android-style per-app virtual filesystem.  Swift installs the
 * container root once; translated Linux/Android absolute paths are
 * confined to it. */
void dn_native_set_files_root(const char *path);
const char *dn_native_files_root(void);
int dn_native_translate_path(const char *guest_path, char *out, size_t out_len);
int dn_native_translate_at_path(int dirfd, const char *guest_path,
                                char *out, size_t out_len);
int dn_native_linux_errno_from_host(int host_errno);
int dn_native_open_translated(const char *guest_path, int linux_flags,
                              mode_t mode);
int dn_native_openat_translated(int dirfd, const char *guest_path,
                                int linux_flags, mode_t mode);
int dn_native_run_selftest(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* DN_NATIVE_PATCH_H */
