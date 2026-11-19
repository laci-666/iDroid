/*
 * Linux/QEMU-user, AArch64-only execution gate for the *converted Mach-O*.
 *
 * This is not an iOS loader and does not verify an Apple signature. It maps
 * the converter's segments at their actual virtual offsets, invokes the DN
 * ELF mini-linker, then executes constructors, all complete-fixture guest
 * exports, Vulkan transfer/clear/copy, NativeActivity callbacks, touch,
 * finalizers and file IO.  The REAL DN AArch64 svc/TP assembly is compiled
 * into the Linux host by the companion test script. One instruction per
 * TP register thunk in the *test copy* of the island is adapted: its
 * Darwin TSD fast-path load returns 0, forcing the original slow TP
 * allocator entry (one load for emulated TLS, two for PT_TLS/TLSDESC).
 * The signed iOS image, guest ELF, syscall trampolines and svc entry
 * are untouched.
 *
 * This catches faults host-only selftests cannot: an earlier 16-byte guest
 * property buffer + 92-byte strncpy() crashed after __android_log_write.
 * It cannot replace dyld/code-signing/Metal/iPhone validation.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../app/DroidNative/NativeRuntime/DNNativeAndroid.h"
#include "../../app/DroidNative/NativeRuntime/DNNativeBionic.h"
#include "../../app/DroidNative/NativeRuntime/DNNativeLinker.h"
#include "../../app/DroidNative/NativeRuntime/DNNativePatch.h"

#define DN_MACHO_MAGIC 0xfeedfacfu
#define DN_LC_SEGMENT_64 0x19u
#define DN_ISLAND_MAGIC 0x45474e48u
#define DN_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define DN_MAX_MAPPED_BYTES (64u * 1024u * 1024u)

struct dn_segment {
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int32_t initial_protection;
};

static int dn_failures;

static uint32_t dn_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint64_t dn_u64(const uint8_t *p)
{
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void dn_expect(const char *label, int64_t actual, int64_t expected)
{
    const int pass = actual == expected;
    printf("%s: %lld (expected %lld) %s\n", label,
           (long long)actual, (long long)expected, pass ? "OK" : "FAIL");
    dn_failures += !pass;
    fflush(stdout);
}

static void *dn_export(dn_native_image_t *image, const char *name)
{
    void *symbol = dn_native_image_symbol(image, name);
    if (!symbol) {
        printf("resolve %s: FAILED\n", name);
        dn_failures++;
    }
    return symbol;
}

static int dn_call_int(dn_native_image_t *image, const char *name)
{
    void *symbol = dn_export(image, name);
    if (!symbol)
        return -9999;
    printf("guest call: %s\n", name);
    fflush(stdout);
    return ((int (*)(void))symbol)();
}

static uint32_t dn_call_uint(dn_native_image_t *image, const char *name)
{
    void *symbol = dn_export(image, name);
    if (!symbol)
        return 0;
    printf("guest call: %s\n", name);
    fflush(stdout);
    return ((uint32_t (*)(void))symbol)();
}

static int dn_mkdir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    perror(path);
    return -1;
}

static int dn_paths(void)
{
    const char *root = getenv("DN_FULL_TEST_ROOT");
    if (!root || !root[0])
        root = "/tmp/dn-full-macho-exec";
    char files[4096], libraries[4096], assets[4096], asset_file[4096];
    if (snprintf(files, sizeof(files), "%s/native-files", root) >= (int)sizeof(files) ||
        snprintf(libraries, sizeof(libraries), "%s/native-libraries", root) >= (int)sizeof(libraries) ||
        snprintf(assets, sizeof(assets), "%s/native-assets", root) >= (int)sizeof(assets) ||
        snprintf(asset_file, sizeof(asset_file), "%s/native-assets/dn-full-asset.txt", root) >= (int)sizeof(asset_file) ||
        dn_mkdir(root) || dn_mkdir(files) || dn_mkdir(libraries) || dn_mkdir(assets))
        return -1;
    dn_native_set_files_root(files);
    dn_native_set_library_directory(libraries);
    dn_native_set_asset_directory(assets);
    dn_native_set_package_name("ro.droidnative.app");
    FILE *asset = fopen(asset_file, "wb");
    if (!asset)
        return -1;
    size_t written = fwrite("DN-ASSET-42", 1, 11, asset);
    int close_rc = fclose(asset);
    return written == 11 && close_rc == 0 ? 0 : -1;
}

static int dn_file_bytes(const char *path, const char *want, size_t length)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    char buffer[16] = {0};
    size_t got = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    return got == length && memcmp(buffer, want, length) == 0 ? 0 : -1;
}

static int dn_check_files(void)
{
    const char *root = getenv("DN_FULL_TEST_ROOT");
    if (!root || !root[0])
        root = "/tmp/dn-full-macho-exec";
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/native-files/internal/files/dn-full-raw.txt", root) >= (int)sizeof(path))
        return -1;
    if (dn_file_bytes(path, "DN42", 4))
        return -1;
    if (snprintf(path, sizeof(path), "%s/native-files/internal/files/dn-full-stdio.txt", root) >= (int)sizeof(path))
        return -1;
    return dn_file_bytes(path, "FULL42", 6);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s libdnfull.dylib\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in || fseek(in, 0, SEEK_END)) {
        fprintf(stderr, "cannot open the Mach-O file\n");
        return 2;
    }
    long file_length = ftell(in);
    if (file_length < 32 || file_length > DN_MAX_FILE_BYTES ||
        fseek(in, 0, SEEK_SET)) {
        fprintf(stderr, "Mach-O size is not within the test limit\n");
        return 2;
    }
    size_t file_size = (size_t)file_length;
    uint8_t *file = malloc(file_size);
    if (!file || fread(file, 1, file_size, in) != file_size)
        return 2;
    fclose(in);
    if (dn_u32(file) != DN_MACHO_MAGIC)
        return 2;

    struct dn_segment segments[16];
    size_t segment_count = 0;
    uint64_t mapped_end = 0, elf_vmaddr = 0, island_vmaddr = 0, island_size = 0;
    uint32_t commands = dn_u32(file + 16);
    size_t at = 32;
    if (commands > 64 || dn_u32(file + 20) > file_size - at)
        return 2;
    for (uint32_t i = 0; i < commands; ++i) {
        if (at > file_size - 8)
            return 2;
        uint32_t kind = dn_u32(file + at), length = dn_u32(file + at + 4);
        if (length < 8 || length > file_size - at)
            return 2;
        if (kind == DN_LC_SEGMENT_64) {
            if (length < 72 || segment_count == 16)
                return 2;
            struct dn_segment *segment = &segments[segment_count++];
            segment->vmaddr = dn_u64(file + at + 24);
            segment->vmsize = dn_u64(file + at + 32);
            segment->fileoff = dn_u64(file + at + 40);
            segment->filesize = dn_u64(file + at + 48);
            segment->initial_protection = (int32_t)dn_u32(file + at + 60);
            if (segment->vmaddr > DN_MAX_MAPPED_BYTES ||
                segment->vmsize > DN_MAX_MAPPED_BYTES - segment->vmaddr ||
                segment->fileoff > file_size ||
                segment->filesize > file_size - segment->fileoff ||
                segment->filesize > segment->vmsize)
                return 2;
            if (segment->vmaddr + segment->vmsize > mapped_end)
                mapped_end = segment->vmaddr + segment->vmsize;
            uint32_t sections = dn_u32(file + at + 64);
            if (sections > (length - 72) / 80)
                return 2;
            for (uint32_t s = 0; s < sections; ++s) {
                size_t off = at + 72 + 80 * s;
                char section[17] = {0};
                memcpy(section, file + off, 16);
                uint64_t addr = dn_u64(file + off + 32);
                uint64_t size = dn_u64(file + off + 40);
                if (addr >= DN_MAX_MAPPED_BYTES || size > DN_MAX_MAPPED_BYTES - addr)
                    return 2;
                if (strcmp(section, "__elf") == 0)
                    elf_vmaddr = addr;
                if (strcmp(section, "__henge") == 0) {
                    island_vmaddr = addr;
                    island_size = size;
                }
            }
        }
        at += length;
    }
    if (!elf_vmaddr || !island_vmaddr || island_size < 32 ||
        mapped_end > DN_MAX_MAPPED_BYTES || mapped_end < island_vmaddr + island_size)
        return 2;

    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t allocation = (size_t)((mapped_end + page - 1) / page) * page;
    uint8_t *mapped = mmap(NULL, allocation, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED)
        return 2;
    for (size_t i = 0; i < segment_count; ++i)
        memcpy(mapped + segments[i].vmaddr,
               file + segments[i].fileoff, (size_t)segments[i].filesize);
    free(file);
    if (dn_u32(mapped + island_vmaddr) != DN_ISLAND_MAGIC ||
        dn_u32(mapped + island_vmaddr + 4) != 1)
        return 2;
    uint64_t slots_vmaddr = dn_u64(mapped + island_vmaddr + 8);
    uint32_t svc_sites = dn_u32(mapped + island_vmaddr + 16);
    uint32_t tp_sites = dn_u32(mapped + island_vmaddr + 20);
    if (slots_vmaddr > mapped_end - 4 * sizeof(uintptr_t) ||
        svc_sites == 0 || tp_sites == 0)
        return 2;
    printf("Mach-O ELF=0x%llx island=0x%llx slots=0x%llx svc=%u TP=%u\n",
           (unsigned long long)elf_vmaddr, (unsigned long long)island_vmaddr,
           (unsigned long long)slots_vmaddr, svc_sites, tp_sites);

    unsigned test_only_fast_path_bypasses = 0;
    for (uint64_t off = island_vmaddr + 32; off + 12 <= island_vmaddr + island_size; off += 4) {
        uint32_t instruction = dn_u32(mapped + off);
        if ((instruction & 0xffffffe0u) != 0xd53bd060u) /* mrs xN,tpidrro_el0 */
            continue;
        uint32_t load = dn_u32(mapped + off + 8);
        if ((load & 0xffe0fc00u) != 0xf8606800u) /* ldr xN,[xN,xS] */
            return 2;
        unsigned reg = instruction & 31u;
        uint32_t test_only_zero = 0xaa1f03e0u | reg; /* mov xN,xzr */
        memcpy(mapped + off + 8, &test_only_zero, sizeof(test_only_zero));
        test_only_fast_path_bypasses++;
    }
    if (test_only_fast_path_bypasses == 0 ||
        test_only_fast_path_bypasses > tp_sites ||
        test_only_fast_path_bypasses > 31) {
        fprintf(stderr, "unexpected Linux-only TSD fast-path bypass count %u (TP sites %u)\n",
                test_only_fast_path_bypasses, tp_sites);
        return 2;
    }
    printf("Linux-only TSD adaptation: %u fast-path load(s); original TP slow entry preserved\n",
           test_only_fast_path_bypasses);
    if (dn_paths())
        return 2;
    dn_native_image_t *image = NULL;
    printf("linking converted Mach-O with constructors enabled...\n");
    fflush(stdout);
    int link_rc = dn_native_link_memory_with_patch_for_test(
        mapped + elf_vmaddr, "libdnfull.dylib", 1,
        mapped + slots_vmaddr, svc_sites, tp_sites, &image);
    printf("dn-link rc=%d\n%s", link_rc, dn_native_image_report(image));
    fflush(stdout);
    if (link_rc || !image)
        return 1;

    /* Simulate dyld's segment protections: the linker must not need to
     * write signed __TEXT, including while executing guest code. */
    for (size_t i = 0; i < segment_count; ++i) {
        struct dn_segment *seg = &segments[i];
        if (seg->vmaddr % page || seg->vmsize % page) {
            fprintf(stderr, "segment not page-aligned\n");
            return 2;
        }
        int prot = ((seg->initial_protection & 1) ? PROT_READ : 0) |
                   ((seg->initial_protection & 2) ? PROT_WRITE : 0) |
                   ((seg->initial_protection & 4) ? PROT_EXEC : 0);
        if (mprotect(mapped + seg->vmaddr, (size_t)seg->vmsize, prot)) {
            perror("mprotect");
            return 2;
        }
    }

    dn_expect("constructor once", dn_call_int(image, "dn_full_ctor_calls"), 1);
    dn_expect("core result", dn_call_int(image, "dn_full_run_test"), 0);
    dn_expect("core step", dn_call_int(image, "dn_full_last_step"), 9);
    if (dn_call_uint(image, "dn_full_checksum") == 0)
        dn_failures++;
    dn_expect("guest Vulkan result", dn_call_int(image, "dn_vk_run_test"), 0);
    dn_expect("guest Vulkan step", dn_call_int(image, "dn_vk_last_step"), 10);
    dn_expect("guest Vulkan checksum", dn_call_uint(image, "dn_vk_checksum"), 0x1de4992d);

    void *create = dn_export(image, "ANativeActivity_onCreate");
    dn_native_activity_host_t *host = dn_native_activity_host_create(64, 48);
    if (!create || !host)
        return 1;
    dn_expect("NativeActivity create", dn_native_activity_host_dispatch_create(host, create), 0);
    dn_expect("NativeActivity start", dn_native_activity_host_start(host), 0);
    dn_expect("NativeActivity resume", dn_native_activity_host_resume(host), 0);
    dn_expect("lifecycle after resume", dn_call_int(image, "dn_full_activity_state"), 63);
    dn_expect("window frame count", dn_native_activity_host_frame_count(host), 1);
    uint32_t host_checksum = dn_native_activity_host_window_checksum(host);
    uint32_t guest_checksum = dn_call_uint(image, "dn_full_activity_checksum");
    dn_expect("window checksum", guest_checksum, host_checksum);
    dn_failures += host_checksum == 0;
    dn_expect("assets/JNI/config/log", dn_call_int(image, "dn_full_run_activity_test"), 0);
    dn_expect("activity step", dn_call_int(image, "dn_full_last_step"), 12);
    dn_native_activity_host_touch(host, 0, 7, 17.5f, 29.25f);
    dn_expect("direct touch", dn_call_int(image, "dn_full_pump_input"), 42);
    dn_native_activity_host_resize(host, 32, 24);
    dn_expect("resized frame count", dn_native_activity_host_frame_count(host), 2);
    host_checksum = dn_native_activity_host_window_checksum(host);
    guest_checksum = dn_call_uint(image, "dn_full_activity_checksum");
    dn_expect("resized window checksum", guest_checksum, host_checksum);
    dn_failures += host_checksum == 0;
    dn_expect("NativeActivity pause", dn_native_activity_host_pause(host), 0);
    dn_expect("NativeActivity stop", dn_native_activity_host_stop(host), 0);
    dn_expect("NativeActivity destroy callbacks", dn_native_activity_host_destroy_callbacks(host), 0);
    dn_expect("lifecycle after destroy", dn_call_int(image, "dn_full_activity_state"), 4095);
    dn_native_activity_host_destroy(host);
    dn_expect("finalizer rc", dn_native_image_run_finalizers(image), 0);
    dn_expect("finalizer side effect", dn_call_int(image, "dn_full_finalized"), 1);
    dn_expect("translated raw + stdio file IO", dn_check_files(), 0);
    dn_native_image_dispose(image);
    printf("DN COMPLETE MACH-O ARM64 EXEC: %s\n", dn_failures ? "FAIL" : "PASS");
    return dn_failures ? 1 : 0;
}
