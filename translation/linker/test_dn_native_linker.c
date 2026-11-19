/* Host-side unit test for DroidNative's own ELF mini-linker.
 *
 * It builds two synthetic AArch64 ET_DYN images in memory: a dependency
 * exporting dn_base_add and a main library with a DT_NEEDED entry,
 * GLOB_DAT/JUMP_SLOT imports, one RELATIVE relocation and one RELR
 * relocation.  No guest code is executed, so this runs on x86_64 too.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../app/DroidNative/NativeRuntime/DNNativeLinker.h"
#include "../../app/DroidNative/NativeRuntime/DNNativePatch.h"

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void check(bool condition, const char *message)
{
    if (!condition)
        fail(message);
}

static void w16(uint8_t *base, size_t off, uint16_t value)
{
    memcpy(base + off, &value, sizeof(value));
}

static void w32(uint8_t *base, size_t off, uint32_t value)
{
    memcpy(base + off, &value, sizeof(value));
}

static void w64(uint8_t *base, size_t off, uint64_t value)
{
    memcpy(base + off, &value, sizeof(value));
}

static uint64_t r64(const uint8_t *base, size_t off)
{
    uint64_t value;
    memcpy(&value, base + off, sizeof(value));
    return value;
}

static uint32_t put_string(uint8_t *strtab, uint32_t *cursor, const char *value)
{
    uint32_t offset = *cursor;
    size_t len = strlen(value) + 1;
    memcpy(strtab + offset, value, len);
    *cursor += (uint32_t)len;
    return offset;
}

static void write_phdr(uint8_t *base, int index, uint32_t type, uint32_t flags,
                       uint64_t vaddr, uint64_t filesz, uint64_t memsz)
{
    size_t off = 0x40 + (size_t)index * 56;
    w32(base, off + 0, type);
    w32(base, off + 4, flags);
    w64(base, off + 8, vaddr);
    w64(base, off + 16, vaddr);
    w64(base, off + 24, vaddr);
    w64(base, off + 32, filesz);
    w64(base, off + 40, memsz);
    w64(base, off + 48, 0x4000);
}

static void write_dyn(uint8_t *base, size_t off, int64_t tag, uint64_t value)
{
    w64(base, off + 0, (uint64_t)tag);
    w64(base, off + 8, value);
}

static void write_symbol(uint8_t *base, size_t off, uint32_t name,
                         uint8_t info, uint16_t shndx, uint64_t value)
{
    w32(base, off + 0, name);
    base[off + 4] = info;
    base[off + 5] = 0;
    w16(base, off + 6, shndx);
    w64(base, off + 8, value);
    w64(base, off + 16, 0);
}

static uint8_t *new_image(void)
{
    uint8_t *base = (uint8_t *)calloc(1, 0x5000);
    if (!base)
        fail("calloc image");
    memcpy(base, "\177ELF", 4);
    base[4] = 2; /* ELFCLASS64 */
    base[5] = 1; /* little endian */
    base[6] = 1;
    w16(base, 16, 3);   /* ET_DYN */
    w16(base, 18, 183); /* AArch64 */
    w64(base, 32, 0x40);
    w16(base, 54, 56);
    w16(base, 56, 4);
    write_phdr(base, 0, 1, 5, 0x0000, 0x4000, 0x4000); /* LOAD R+X */
    write_phdr(base, 1, 1, 6, 0x4000, 0x1000, 0x1000); /* LOAD RW */
    write_phdr(base, 3, 7, 4, 0x2000, 8, 32);          /* PT_TLS */
    w64(base, 0x40 + 3 * 56 + 48, 16);                 /* PT_TLS align */
    memcpy(base + 0x2000, "DNTLS42!", 8);
    return base;
}

int main(void)
{
    /* ---------------- dependency: libdnbase.so ---------------- */
    uint8_t *dep = new_image();
    uint32_t dep_str_cursor = 1;
    uint32_t dep_soname = put_string(dep + 0x300, &dep_str_cursor, "libdnbase.so");
    uint32_t dep_add = put_string(dep + 0x300, &dep_str_cursor, "dn_base_add");
    uint32_t dep_tls = put_string(dep + 0x300, &dep_str_cursor, "dn_tls_counter");

    write_symbol(dep, 0x400 + 24, dep_add, (1u << 4) | 2u, 1, 0x1230);
    write_symbol(dep, 0x400 + 48, dep_tls, (1u << 4) | 6u, 1, 4);
    w32(dep, 0x500 + 0, 1); /* nbucket */
    w32(dep, 0x500 + 4, 3); /* nchain */

    size_t d = 0x180;
    write_dyn(dep, d, 14, dep_soname); d += 16; /* SONAME */
    write_dyn(dep, d, 5, 0x300); d += 16;      /* STRTAB */
    write_dyn(dep, d, 10, 0x80); d += 16;      /* STRSZ */
    write_dyn(dep, d, 6, 0x400); d += 16;      /* SYMTAB */
    write_dyn(dep, d, 4, 0x500); d += 16;      /* HASH */
    write_dyn(dep, d, 0, 0); d += 16;
    write_phdr(dep, 2, 2, 4, 0x180, d - 0x180, d - 0x180);

    dn_native_image_t *dep_image = NULL;
    int rc = dn_native_link_memory_for_test(dep, "libdnbase.dylib", 0, &dep_image);
    if (rc != 0) {
        fprintf(stderr, "dependency report:\n%s\n", dn_native_image_report(dep_image));
        fail("dependency did not link");
    }
    check(dn_native_image_symbol(dep_image, "dn_base_add") ==
          (void *)(dep + 0x1230), "dependency export address");
    uint8_t *dep_tls_address =
        (uint8_t *)dn_native_image_symbol(dep_image, "dn_tls_counter");
    check(dep_tls_address && memcmp(dep_tls_address, "S42!", 4) == 0,
          "dependency TLS export/template");

    /* ---------------- main: libdnlink.so ---------------- */
    uint8_t *main_image = new_image();
    uint32_t str_cursor = 1;
    uint32_t main_soname = put_string(main_image + 0x300, &str_cursor, "libdnlink.so");
    uint32_t main_needed = put_string(main_image + 0x300, &str_cursor, "libdnbase.so");
    uint32_t main_add = put_string(main_image + 0x300, &str_cursor, "dn_base_add");
    uint32_t main_compute = put_string(main_image + 0x300, &str_cursor, "dn_link_compute");
    uint32_t main_tls = put_string(main_image + 0x300, &str_cursor, "dn_tls_counter");
    uint32_t main_tls_get_addr =
        put_string(main_image + 0x300, &str_cursor, "__tls_get_addr");

    write_symbol(main_image, 0x400 + 24, main_add, (1u << 4) | 2u, 0, 0);
    write_symbol(main_image, 0x400 + 48, main_compute, (1u << 4) | 2u, 1, 0x1240);
    write_symbol(main_image, 0x400 + 72, main_tls, (1u << 4) | 6u, 0, 0);
    write_symbol(main_image, 0x400 + 96, main_tls_get_addr,
                 (1u << 4) | 2u, 0, 0);
    w32(main_image, 0x500 + 0, 1);
    w32(main_image, 0x500 + 4, 5);

    /* RELA: ordinary imports plus the complete AArch64 TLS family. */
    w64(main_image, 0x600 + 0, 0x4000);
    w64(main_image, 0x600 + 8, ((uint64_t)1 << 32) | 1025u);
    w64(main_image, 0x600 + 16, 0);
    w64(main_image, 0x618 + 0, 0x4008);
    w64(main_image, 0x618 + 8, 1027u);
    w64(main_image, 0x618 + 16, 0x123);
    w64(main_image, 0x630 + 0, 0x4020); /* TLS_DTPMOD64 */
    w64(main_image, 0x630 + 8, ((uint64_t)3 << 32) | 1028u);
    w64(main_image, 0x630 + 16, 0);
    w64(main_image, 0x648 + 0, 0x4028); /* TLS_DTPREL64 */
    w64(main_image, 0x648 + 8, ((uint64_t)3 << 32) | 1029u);
    w64(main_image, 0x648 + 16, 0);
    w64(main_image, 0x660 + 0, 0x4030); /* TLS_TPREL64 */
    w64(main_image, 0x660 + 8, ((uint64_t)3 << 32) | 1030u);
    w64(main_image, 0x660 + 16, 0);
    w64(main_image, 0x678 + 0, 0x4038); /* TLSDESC */
    w64(main_image, 0x678 + 8, ((uint64_t)3 << 32) | 1031u);
    w64(main_image, 0x678 + 16, 0);
    w64(main_image, 0x690 + 0, 0x4048); /* GLOB_DAT(__tls_get_addr) */
    w64(main_image, 0x690 + 8, ((uint64_t)4 << 32) | 1025u);
    w64(main_image, 0x690 + 16, 0);
    w64(main_image, 0x6a8 + 0, 0x4050); /* own-module TLS_DTPMOD64 */
    w64(main_image, 0x6a8 + 8, 1028u);
    w64(main_image, 0x6a8 + 16, 0);

    /* JMPREL: JUMP_SLOT(dn_base_add). */
    w64(main_image, 0x6c0 + 0, 0x4018);
    w64(main_image, 0x6c0 + 8, ((uint64_t)1 << 32) | 1026u);
    w64(main_image, 0x6c0 + 16, 0);

    /* RELR: relocate the word at 0x4010, initially containing 0x77. */
    w64(main_image, 0x6e0, 0x4010);
    w64(main_image, 0x4010, 0x77);

    /* APS2 RELA: one RELATIVE at 0x4058 with addend 0x321. */
    static const uint8_t aps2[] = {
        'A', 'P', 'S', '2', 0x01, 0x00, 0x01, 0x08,
        0xd8, 0x80, 0x01, 0x83, 0x08, 0xa1, 0x06
    };
    memcpy(main_image + 0x700, aps2, sizeof(aps2));

    d = 0x180;
    write_dyn(main_image, d, 14, main_soname); d += 16;
    write_dyn(main_image, d, 1, main_needed); d += 16;
    write_dyn(main_image, d, 5, 0x300); d += 16;
    write_dyn(main_image, d, 10, 0x80); d += 16;
    write_dyn(main_image, d, 6, 0x400); d += 16;
    write_dyn(main_image, d, 4, 0x500); d += 16;
    write_dyn(main_image, d, 7, 0x600); d += 16;
    write_dyn(main_image, d, 8, 192); d += 16;
    write_dyn(main_image, d, 9, 24); d += 16;
    write_dyn(main_image, d, 23, 0x6c0); d += 16;
    write_dyn(main_image, d, 2, 24); d += 16;
    write_dyn(main_image, d, 20, 7); d += 16;
    write_dyn(main_image, d, 36, 0x6e0); d += 16;
    write_dyn(main_image, d, 35, 8); d += 16;
    write_dyn(main_image, d, 37, 8); d += 16;
    write_dyn(main_image, d, (int64_t)0x6fffe002, 0x700); d += 16;
    write_dyn(main_image, d, (int64_t)0x6fffe003, sizeof(aps2)); d += 16;
    write_dyn(main_image, d, 0, 0); d += 16;
    write_phdr(main_image, 2, 2, 4, 0x180, d - 0x180, d - 0x180);

    uintptr_t patch_slots[4] = {
        UINTPTR_MAX, UINTPTR_MAX, UINTPTR_MAX, UINTPTR_MAX
    };
    dn_native_image_t *link_image = NULL;
    rc = dn_native_link_memory_with_patch_for_test(
        main_image, "libdnlink.dylib", 0, patch_slots, 3, 2, &link_image);
    if (rc != 0) {
        fprintf(stderr, "main report:\n%s\n", dn_native_image_report(link_image));
        fail("main image did not link");
    }

    check(r64(main_image, 0x4000) == (uint64_t)(uintptr_t)(dep + 0x1230),
          "GLOB_DAT did not bind to dependency export");
    check(r64(main_image, 0x4008) == (uint64_t)(uintptr_t)(main_image + 0x123),
          "RELATIVE relocation is wrong");
    check(r64(main_image, 0x4010) == (uint64_t)(uintptr_t)(main_image + 0x77),
          "RELR relocation is wrong");
    check(r64(main_image, 0x4018) == (uint64_t)(uintptr_t)(dep + 0x1230),
          "JUMP_SLOT did not bind to dependency export");
    check(dn_native_image_symbol(link_image, "dn_link_compute") ==
          (void *)(main_image + 0x1240), "main export address");

    check(patch_slots[0] == (uintptr_t)dn_native_svc_entry,
          "patch slot 0 is not the DN svc entry");
    check(patch_slots[1] == dn_native_patch_tp_key_offset(),
          "patch slot 1 is not the DN TP TSD offset");
    check(patch_slots[2] == (uintptr_t)dn_native_tp_slow_entry,
          "patch slot 2 is not the DN TP slow entry");
    check(patch_slots[3] == 0, "patch slot 3 is not reserved/zero");

    check(r64(main_image, 0x4020) != 0, "TLS_DTPMOD64 produced no module id");
    check(r64(main_image, 0x4028) == 4, "TLS_DTPREL64 offset is wrong");
    uint64_t tls_index[2] = {
        r64(main_image, 0x4020), r64(main_image, 0x4028)
    };
    check(dn_native_tls_get_addr(tls_index) == dep_tls_address,
          "__tls_get_addr(DTPMOD,DTPREL) address is wrong");
    check(r64(main_image, 0x4030) ==
          (uint64_t)(dep_tls_address -
                     (uint8_t *)dn_native_thread_pointer()),
          "TLS_TPREL64 offset is wrong");
    check(r64(main_image, 0x4038) ==
          (uint64_t)(uintptr_t)dn_native_tlsdesc_entry,
          "TLSDESC resolver is wrong");
    uintptr_t descriptor[2] = {
        (uintptr_t)r64(main_image, 0x4038),
        (uintptr_t)r64(main_image, 0x4040)
    };
    check(descriptor[1] != 0 &&
          dn_native_tlsdesc_resolve(descriptor) ==
              (intptr_t)r64(main_image, 0x4030),
          "TLSDESC delta is wrong");
    check(r64(main_image, 0x4048) ==
          (uint64_t)(uintptr_t)dn_native_tls_get_addr,
          "__tls_get_addr import did not bind to DN");
    check(r64(main_image, 0x4050) != 0 &&
          r64(main_image, 0x4050) != r64(main_image, 0x4020),
          "own-module TLS_DTPMOD64 is wrong");
    check(r64(main_image, 0x4058) ==
          (uint64_t)(uintptr_t)(main_image + 0x321),
          "APS2 RELATIVE relocation is wrong");

    const char *report = dn_native_image_report(link_image);
    check(strstr(report, "relocations OK") != NULL, "report lacks relocation verdict");
    check(strstr(report, "dn-link OK") != NULL, "report lacks final verdict");
    check(strstr(report, "patch island armed") != NULL,
          "report lacks patch-island verdict");
    check(strstr(report, "TLS_DTPMOD=2") != NULL &&
          strstr(report, "TLSDESC=1") != NULL,
          "report lacks TLS relocation counts");

    dn_native_image_dispose(link_image);
    dn_native_image_dispose(dep_image);
    free(main_image);
    free(dep);

    puts("DN NATIVE LINKER TEST: PASS");
    return 0;
}
