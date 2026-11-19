#ifndef DN_NATIVE_SHIMS_H
#define DN_NATIVE_SHIMS_H

#include <stddef.h>

/*
 * Compatibility symbols provided by iDroid's own native runtime.
 *
 * The table is intentionally conservative: only non-variadic calls whose
 * argument/return conventions are identical for AAPCS64 bionic code and
 * Darwin arm64 are forwarded directly.  Variadic Android calls (printf,
 * __android_log_print, open with mode, ...) need an assembly ABI bridge:
 * Darwin's variadic convention is different, so pretending that a normal
 * Darwin variadic function can receive them would corrupt arguments.
 */
void *dn_native_shim_lookup(const char *name);
int dn_native_shim_provides(const char *name);

/* Implemented by DNGpuVulkan.c: DN's own Vulkan-on-Metal subset.
 * dn_native_shim_lookup() delegates vk* imports here. */
void *dn_vulkan_lookup_symbol(const char *name);
int dn_vulkan_run_selftest(char *out, size_t out_len);

/* Runtime self-test symbols used by translation/link. */
int dn_native_selftest_add(int a, int b);
size_t dn_native_selftest_strlen(const char *s);

/* Complete-native runtime gates and one-time path configuration used
 * by the Swift harness.  The setters copy their arguments. */
int dn_native_run_selftest(char *out, size_t out_len);
int dn_native_bionic_run_selftest(char *out, size_t out_len);
int dn_native_android_run_selftest(char *out, size_t out_len);
void dn_native_set_files_root(const char *path);
void dn_native_set_library_directory(const char *path);
void dn_native_set_asset_directory(const char *path);
void dn_native_set_package_name(const char *package_name);

#endif /* DN_NATIVE_SHIMS_H */
