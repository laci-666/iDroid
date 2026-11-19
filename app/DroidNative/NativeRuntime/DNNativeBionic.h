#ifndef DN_NATIVE_bionic_H
#define DN_NATIVE_bionic_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DN's bionic/POSIX compatibility surface for translated code.  The
 * important part is not the symbol count: it is that objects whose
 * Darwin and bionic layouts differ (pthread keys/attrs, sync objects,
 * DIR/dirent, FILE, Linux stat, open/fcntl flags and errno) are
 * represented by DN wrappers instead of being handed blindly to the
 * host libc. */
void *dn_native_bionic_lookup(const char *name);
int *dn_native_errno_location(void);
void dn_native_set_library_directory(const char *path);
int dn_native_bionic_run_selftest(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* DN_NATIVE_bionic_H */
