/* Host-side aggregate test for DroidNative's complete native runtime.
 *
 * This links every NativeRuntime C translation unit into one process
 * and runs their four self-tests in dependency order.  It catches
 * duplicate symbols, lookup-chain collisions and cross-layer state
 * interactions that per-file object compilation cannot detect.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../app/DroidNative/NativeRuntime/DNNativeAndroid.h"
#include "../../app/DroidNative/NativeRuntime/DNNativeBionic.h"
#include "../../app/DroidNative/NativeRuntime/DNNativePatch.h"
#include "../../app/DroidNative/NativeRuntime/DNNativeShims.h"

typedef int (*dn_full_selftest_fn)(char *out, size_t out_len);

static bool dn_full_run_one(const char *label, dn_full_selftest_fn fn,
                            const char *pass_marker)
{
    char out[16384];
    memset(out, 0, sizeof(out));
    int rc = fn(out, sizeof(out));
    bool passed = rc == 0 && strstr(out, pass_marker) != NULL;

    printf("===== %s rc=%d =====\n%s\n", label, rc, out);
    printf("%s: %s\n", label, passed ? "PASS" : "FAIL");
    return passed;
}

int main(void)
{
    /* Mirror the on-device Swift layout exactly: three sibling roots
     * under one container directory.  An earlier version of this
     * harness used <files-root>/assets as the asset root, which made
     * the VFS /android_asset mapping and the AAssetManager root
     * coincide and masked a real device-only self-test failure. */
    dn_native_set_files_root("/tmp/DroidNativeNative/native-files");
    dn_native_set_library_directory("/tmp/DroidNativeNative/native-libraries");
    dn_native_set_asset_directory("/tmp/DroidNativeNative/native-assets");
    dn_native_set_package_name("ro.droidnative.app");

    int failures = 0;
    failures += dn_full_run_one("patch/TLS/VFS",
                                dn_native_run_selftest,
                                "host verdict: PASS") ? 0 : 1;
    failures += dn_full_run_one("bionic",
                                dn_native_bionic_run_selftest,
                                "host verdict: PASS") ? 0 : 1;
    failures += dn_full_run_one("libandroid",
                                dn_native_android_run_selftest,
                                "host verdict: PASS") ? 0 : 1;
    failures += dn_full_run_one(
        "Vulkan-on-Metal core",
        dn_vulkan_run_selftest,
        "PASS - DroidNative Vulkan-on-Metal subset self-test") ? 0 : 1;

    puts(failures == 0 ? "DN COMPLETE NATIVE HOST TEST: PASS"
                       : "DN COMPLETE NATIVE HOST TEST: FAIL");
    return failures == 0 ? 0 : 1;
}
