#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Darwin feature exposure for RTLD_DEFAULT: with strict C11, defining
 * _POSIX_C_SOURCE alone lowers __DARWIN_C_LEVEL to the POSIX namespace
 * and <dlfcn.h> hides RTLD_DEFAULT on macOS/iOS.  Ask Darwin for its
 * full C surface before including any system header. */
#define _DARWIN_C_SOURCE 1
#endif

#include "DNNativeLinker.h"
#include "DNNativePatch.h"
#include "DNNativeShims.h"

#include <dlfcn.h>
#if defined(__APPLE__) && !defined(RTLD_DEFAULT)
/* Defensive fallback if a future SDK keeps the constant gated even
 * harder: Darwin's documented RTLD_DEFAULT handle value. */
#define RTLD_DEFAULT ((void *)-2)
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

/* ELF64 / AArch64 constants (defined locally: Darwin has no <elf.h>). */
enum {
    DN_ET_DYN = 3,
    DN_EM_AARCH64 = 183,

    DN_PT_LOAD = 1,
    DN_PT_DYNAMIC = 2,
    DN_PT_TLS = 7,
    DN_PF_X = 1,
    DN_PF_W = 2,

    DN_DT_NULL = 0,
    DN_DT_NEEDED = 1,
    DN_DT_PLTRELSZ = 2,
    DN_DT_HASH = 4,
    DN_DT_STRTAB = 5,
    DN_DT_SYMTAB = 6,
    DN_DT_RELA = 7,
    DN_DT_RELASZ = 8,
    DN_DT_RELAENT = 9,
    DN_DT_STRSZ = 10,
    DN_DT_SYMENT = 11,
    DN_DT_INIT = 12,
    DN_DT_FINI = 13,
    DN_DT_SONAME = 14,
    DN_DT_REL = 17,
    DN_DT_RELSZ = 18,
    DN_DT_RELENT = 19,
    DN_DT_PLTREL = 20,
    DN_DT_TEXTREL = 22,
    DN_DT_JMPREL = 23,
    DN_DT_INIT_ARRAY = 25,
    DN_DT_FINI_ARRAY = 26,
    DN_DT_INIT_ARRAYSZ = 27,
    DN_DT_FINI_ARRAYSZ = 28,
    DN_DT_FLAGS = 30,
    DN_DT_PREINIT_ARRAY = 32,
    DN_DT_PREINIT_ARRAYSZ = 33,
    DN_DT_RELRSZ = 35,
    DN_DT_RELR = 36,
    DN_DT_RELRENT = 37,

    DN_DT_GNU_HASH = 0x6ffffef5,
    DN_DT_VERNEED = 0x6ffffffe,
    DN_DT_ANDROID_REL = 0x6fffe000,
    DN_DT_ANDROID_RELSZ = 0x6fffe001,
    DN_DT_ANDROID_RELA = 0x6fffe002,
    DN_DT_ANDROID_RELASZ = 0x6fffe003,

    DN_DF_TEXTREL = 0x4,
    DN_DF_STATIC_TLS = 0x10,

    DN_STB_LOCAL = 0,
    DN_STB_GLOBAL = 1,
    DN_STB_WEAK = 2,
    DN_STT_FUNC = 2,
    DN_STT_TLS = 6,
    DN_STT_GNU_IFUNC = 10,
    DN_SHN_UNDEF = 0,
    DN_SHN_ABS = 0xfff1,

    DN_R_AARCH64_NONE = 0,
    DN_R_AARCH64_ABS64 = 257,
    DN_R_AARCH64_GLOB_DAT = 1025,
    DN_R_AARCH64_JUMP_SLOT = 1026,
    DN_R_AARCH64_RELATIVE = 1027,
    DN_R_AARCH64_TLS_DTPMOD64 = 1028,
    DN_R_AARCH64_TLS_DTPREL64 = 1029,
    DN_R_AARCH64_TLS_TPREL64 = 1030,
    DN_R_AARCH64_TLSDESC = 1031,
    DN_R_AARCH64_IRELATIVE = 1032
};

enum {
    DN_PATCH_ISLAND_MAGIC = 0x45474e48u, /* HNGE */
    DN_PATCH_ISLAND_VERSION = 1,
    DN_PATCH_SLOT_SYSCALL = 0,
    DN_PATCH_SLOT_TSD_OFFSET = 1,
    DN_PATCH_SLOT_TP_SLOW = 2,
    DN_PATCH_SLOT_RESERVED = 3
};

struct dn_load_range {
    uint64_t start;
    uint64_t end;
    uint32_t flags;
};

struct dn_native_image {
    char *path;
    char *soname;
    void *dlopen_handle;
    uint8_t *base;
    uint64_t map_size;

    struct dn_load_range *loads;
    size_t load_count;

    uint64_t dynsym;
    uint64_t dynstr;
    uint64_t dynstr_size;
    uint64_t hash;
    uint64_t gnu_hash;
    uint32_t symcount;

    uint64_t *needed_offsets;
    size_t needed_count;

    uint64_t rela;
    uint64_t rela_size;
    uint64_t rela_entry_size;
    uint64_t jmprel;
    uint64_t pltrel_size;
    uint64_t pltrel_type;
    uint64_t relr;
    uint64_t relr_size;
    uint64_t relr_entry_size;
    uint64_t rel;
    uint64_t rel_size;
    uint64_t rel_entry_size;
    uint64_t android_rel;
    uint64_t android_rel_size;
    uint64_t android_rela;
    uint64_t android_rela_size;
    bool has_textrel;
    bool has_verneed;
    bool has_static_tls;
    uint64_t tls_vaddr;
    uint64_t tls_file_size;
    uint64_t tls_mem_size;
    uint64_t tls_alignment;
    dn_native_tls_module_t *tls_module;

    uint64_t init;
    uint64_t fini;
    uint64_t init_array;
    uint64_t init_array_size;
    uint64_t fini_array;
    uint64_t fini_array_size;
    uint64_t preinit_array;
    uint64_t preinit_array_size;

    bool has_patch_island;
    bool patch_slots_writable;
    uint32_t patch_version;
    uint64_t patch_slots_vmaddr;
    uintptr_t patch_slots_address;
    uint32_t patch_svc_sites;
    uint32_t patch_tpidr_sites;

    bool suppress_initializers;
    bool linked;
    bool initializers_run;
    bool finalizers_run;
    int refcount;

    char *report;
    size_t report_len;
    size_t report_cap;

    struct dn_native_image *next;
};

static dn_native_image_t *dn_registry;
static pthread_mutex_t dn_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static char *dn_strndup_local(const char *s, size_t n)
{
    if (!s)
        return NULL;
    char *copy = (char *)malloc(n + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

static char *dn_strdup_local(const char *s)
{
    return s ? dn_strndup_local(s, strlen(s)) : NULL;
}

static char *dn_link_progress_path;

void dn_native_set_link_progress_path(const char *path)
{
    free(dn_link_progress_path);
    dn_link_progress_path = path ? dn_strdup_local(path) : NULL;
}

static void dn_append_link_progress(const char *line, size_t length)
{
    if (!dn_link_progress_path || !line || length == 0)
        return;
    FILE *file = fopen(dn_link_progress_path, "ab");
    if (!file)
        return;
    (void)fwrite(line, 1, length, file);
    (void)fflush(file);
    (void)fclose(file);
}

static bool dn_report_reserve(dn_native_image_t *image, size_t extra)
{
    if (!image)
        return false;
    size_t needed = image->report_len + extra + 1;
    if (needed <= image->report_cap)
        return true;
    size_t cap = image->report_cap ? image->report_cap : 1024;
    while (cap < needed) {
        if (cap > (SIZE_MAX / 2))
            return false;
        cap *= 2;
    }
    char *next = (char *)realloc(image->report, cap);
    if (!next)
        return false;
    image->report = next;
    image->report_cap = cap;
    if (image->report_len == 0)
        image->report[0] = '\0';
    return true;
}

static void dn_logf(dn_native_image_t *image, const char *fmt, ...)
{
    if (!image || !fmt)
        return;

    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return;
    }
    if (!dn_report_reserve(image, (size_t)needed + 1)) {
        va_end(ap);
        return;
    }
    size_t old_len = image->report_len;
    vsnprintf(image->report + image->report_len,
              image->report_cap - image->report_len, fmt, ap);
    va_end(ap);
    image->report_len += (size_t)needed;
    if (image->report_len == 0 || image->report[image->report_len - 1] != '\n') {
        if (dn_report_reserve(image, 1)) {
            image->report[image->report_len++] = '\n';
            image->report[image->report_len] = '\0';
        }
    }
    if (image->report_len > old_len)
        dn_append_link_progress(image->report + old_len,
                                image->report_len - old_len);
}

static uint16_t dn_rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t dn_rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t dn_rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void dn_wr64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, sizeof(v));
}

static bool dn_range_ok(const dn_native_image_t *image, uint64_t off, uint64_t size)
{
    if (!image || !image->base)
        return false;
    if (image->map_size == 0)
        return off <= (UINT64_MAX - size);
    return off <= image->map_size && size <= image->map_size - off;
}

static const uint8_t *dn_ptr(const dn_native_image_t *image, uint64_t off, uint64_t size)
{
    return dn_range_ok(image, off, size) ? image->base + off : NULL;
}

static bool dn_in_load_with_flags(const dn_native_image_t *image, uint64_t off,
                                  uint64_t size, uint32_t flags)
{
    if (!dn_range_ok(image, off, size))
        return false;
    for (size_t i = 0; i < image->load_count; ++i) {
        const struct dn_load_range *load = &image->loads[i];
        if ((load->flags & flags) != flags)
            continue;
        if (off >= load->start && off + size <= load->end)
            return true;
    }
    return false;
}

static const char *dn_basename_ptr(const char *path)
{
    if (!path)
        return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *dn_elf_strndup(const dn_native_image_t *image, uint64_t string_offset)
{
    if (!image || string_offset >= image->dynstr_size)
        return NULL;
    const uint8_t *start = dn_ptr(image, image->dynstr + string_offset, 1);
    if (!start)
        return NULL;
    uint64_t max = image->dynstr_size - string_offset;
    uint64_t len = 0;
    while (len < max && start[len] != 0)
        len++;
    if (len == max)
        return NULL;
    return dn_strndup_local((const char *)start, (size_t)len);
}

static bool dn_elf_string_equals(const dn_native_image_t *image,
                                 uint64_t string_offset, const char *target)
{
    if (!target || string_offset >= image->dynstr_size)
        return false;
    const uint8_t *start = dn_ptr(image, image->dynstr + string_offset, 1);
    if (!start)
        return false;
    uint64_t max = image->dynstr_size - string_offset;
    uint64_t len = 0;
    while (len < max && start[len] != 0)
        len++;
    return len < max && len == strlen(target) && memcmp(start, target, (size_t)len) == 0;
}

/* ------------------------------------------------------------------ */
/* ELF parsing                                                         */
/* ------------------------------------------------------------------ */

static bool dn_add_load(dn_native_image_t *image, uint64_t start, uint64_t end,
                        uint32_t flags)
{
    struct dn_load_range *next = (struct dn_load_range *)realloc(
        image->loads, (image->load_count + 1) * sizeof(*next));
    if (!next)
        return false;
    image->loads = next;
    image->loads[image->load_count].start = start;
    image->loads[image->load_count].end = end;
    image->loads[image->load_count].flags = flags;
    image->load_count++;
    return true;
}

static bool dn_add_needed(dn_native_image_t *image, uint64_t offset)
{
    uint64_t *next = (uint64_t *)realloc(image->needed_offsets,
                                         (image->needed_count + 1) * sizeof(*next));
    if (!next)
        return false;
    image->needed_offsets = next;
    image->needed_offsets[image->needed_count++] = offset;
    return true;
}

static bool dn_parse_symbol_count(dn_native_image_t *image)
{
    if (image->hash) {
        const uint8_t *hash = dn_ptr(image, image->hash, 8);
        if (!hash)
            return false;
        image->symcount = dn_rd32(hash + 4); /* nchain */
        if (image->symcount)
            return true;
    }

    if (image->gnu_hash) {
        const uint8_t *g = dn_ptr(image, image->gnu_hash, 16);
        if (!g)
            return false;
        uint32_t nbuckets = dn_rd32(g + 0);
        uint32_t symoffset = dn_rd32(g + 4);
        uint32_t bloom_size = dn_rd32(g + 8);
        uint64_t buckets = image->gnu_hash + 16 + (uint64_t)bloom_size * 8;
        uint64_t chain = buckets + (uint64_t)nbuckets * 4;
        if (!dn_ptr(image, buckets, (uint64_t)nbuckets * 4))
            return false;

        uint32_t max_symbol = 0;
        for (uint32_t i = 0; i < nbuckets; ++i) {
            const uint8_t *bucket = dn_ptr(image, buckets + (uint64_t)i * 4, 4);
            if (!bucket)
                return false;
            uint32_t index = dn_rd32(bucket);
            if (!index)
                continue;
            if (index < symoffset)
                return false;
            for (;;) {
                if (index > 10000000u)
                    return false;
                const uint8_t *chain_entry =
                    dn_ptr(image, chain + (uint64_t)(index - symoffset) * 4, 4);
                if (!chain_entry)
                    return false;
                uint32_t hash = dn_rd32(chain_entry);
                index++;
                if (hash & 1u)
                    break;
            }
            if (index > max_symbol)
                max_symbol = index;
        }
        image->symcount = max_symbol;
        if (image->symcount)
            return true;
    }

    if (image->dynstr > image->dynsym) {
        uint64_t count = (image->dynstr - image->dynsym) / 24u;
        if (count && count <= UINT32_MAX) {
            image->symcount = (uint32_t)count;
            return true;
        }
    }
    return false;
}

static bool dn_parse_elf(dn_native_image_t *image)
{
    const uint8_t *base = image->base;
    if (!base) {
        dn_logf(image, "link failed: no mapped ELF base");
        return false;
    }
    if (memcmp(base, "\177ELF", 4) != 0 || base[4] != 2 || base[5] != 1) {
        dn_logf(image, "link failed: __TEXT,__elf does not start with a little-endian ELF64 header");
        return false;
    }

    uint16_t type = dn_rd16(base + 16);
    uint16_t machine = dn_rd16(base + 18);
    uint64_t phoff = dn_rd64(base + 32);
    uint16_t phentsize = dn_rd16(base + 54);
    uint16_t phnum = dn_rd16(base + 56);
    if (type != DN_ET_DYN || machine != DN_EM_AARCH64) {
        dn_logf(image, "link failed: expected ET_DYN/AArch64, got type=%u machine=%u",
                type, machine);
        return false;
    }
    if (!phoff || phentsize < 56 || phnum == 0 || phnum == 0xffff || phnum > 256) {
        dn_logf(image, "link failed: implausible program header table");
        return false;
    }

    uint64_t dynamic = 0;
    uint64_t dynamic_size = 0;
    uint64_t map_size = 0;

    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t *ph = base + phoff + (uint64_t)i * phentsize;
        uint32_t p_type = dn_rd32(ph + 0);
        uint32_t p_flags = dn_rd32(ph + 4);
        uint64_t p_vaddr = dn_rd64(ph + 16);
        uint64_t p_filesz = dn_rd64(ph + 32);
        uint64_t p_memsz = dn_rd64(ph + 40);
        uint64_t p_align = dn_rd64(ph + 48);

        if (p_type == DN_PT_LOAD) {
            if (p_memsz && p_vaddr > UINT64_MAX - p_memsz)
                return false;
            uint64_t end = p_vaddr + p_memsz;
            if (!dn_add_load(image, p_vaddr, end, p_flags))
                return false;
            if (end > map_size)
                map_size = end;
        } else if (p_type == DN_PT_DYNAMIC) {
            dynamic = p_vaddr;
            dynamic_size = p_filesz;
        } else if (p_type == DN_PT_TLS) {
            image->tls_vaddr = p_vaddr;
            image->tls_file_size = p_filesz;
            image->tls_mem_size = p_memsz;
            image->tls_alignment = p_align;
        }
    }

    if (!dynamic || !map_size) {
        dn_logf(image, "link failed: missing PT_DYNAMIC or PT_LOAD");
        return false;
    }
    image->map_size = map_size;

    uint64_t soname_offset = UINT64_MAX;
    uint64_t entries = dynamic_size ? dynamic_size / 16 : 512;
    if (entries > 4096)
        entries = 4096;
    for (uint64_t i = 0; i < entries; ++i) {
        const uint8_t *entry = dn_ptr(image, dynamic + i * 16, 16);
        if (!entry) {
            dn_logf(image, "link failed: PT_DYNAMIC lies outside the mapped image");
            return false;
        }
        int64_t tag = (int64_t)dn_rd64(entry + 0);
        uint64_t value = dn_rd64(entry + 8);
        if (tag == DN_DT_NULL)
            break;

        switch (tag) {
        case DN_DT_NEEDED:
            if (!dn_add_needed(image, value))
                return false;
            break;
        case DN_DT_PLTRELSZ: image->pltrel_size = value; break;
        case DN_DT_HASH: image->hash = value; break;
        case DN_DT_STRTAB: image->dynstr = value; break;
        case DN_DT_SYMTAB: image->dynsym = value; break;
        case DN_DT_RELA: image->rela = value; break;
        case DN_DT_RELASZ: image->rela_size = value; break;
        case DN_DT_RELAENT: image->rela_entry_size = value; break;
        case DN_DT_STRSZ: image->dynstr_size = value; break;
        case DN_DT_INIT: image->init = value; break;
        case DN_DT_FINI: image->fini = value; break;
        case DN_DT_SONAME: soname_offset = value; break;
        case DN_DT_REL: image->rel = value; break;
        case DN_DT_RELSZ: image->rel_size = value; break;
        case DN_DT_RELENT: image->rel_entry_size = value; break;
        case DN_DT_PLTREL: image->pltrel_type = value; break;
        case DN_DT_TEXTREL: image->has_textrel = true; break;
        case DN_DT_JMPREL: image->jmprel = value; break;
        case DN_DT_INIT_ARRAY: image->init_array = value; break;
        case DN_DT_FINI_ARRAY: image->fini_array = value; break;
        case DN_DT_INIT_ARRAYSZ: image->init_array_size = value; break;
        case DN_DT_FINI_ARRAYSZ: image->fini_array_size = value; break;
        case DN_DT_FLAGS:
            if (value & DN_DF_TEXTREL)
                image->has_textrel = true;
            if (value & DN_DF_STATIC_TLS)
                image->has_static_tls = true;
            break;
        case DN_DT_PREINIT_ARRAY: image->preinit_array = value; break;
        case DN_DT_PREINIT_ARRAYSZ: image->preinit_array_size = value; break;
        case DN_DT_RELRSZ: image->relr_size = value; break;
        case DN_DT_RELR: image->relr = value; break;
        case DN_DT_RELRENT: image->relr_entry_size = value; break;
        case DN_DT_GNU_HASH: image->gnu_hash = value; break;
        case DN_DT_VERNEED: image->has_verneed = true; break;
        case DN_DT_ANDROID_REL: image->android_rel = value; break;
        case DN_DT_ANDROID_RELSZ: image->android_rel_size = value; break;
        case DN_DT_ANDROID_RELA: image->android_rela = value; break;
        case DN_DT_ANDROID_RELASZ: image->android_rela_size = value; break;
        default:
            break;
        }
    }

    if (!image->dynsym || !image->dynstr || !image->dynstr_size) {
        dn_logf(image, "link failed: missing DT_SYMTAB/DT_STRTAB/DT_STRSZ");
        return false;
    }
    if (!dn_ptr(image, image->dynstr, image->dynstr_size) ||
        !dn_ptr(image, image->dynsym, 24)) {
        dn_logf(image, "link failed: dynamic symbol/string table outside the image");
        return false;
    }
    if (!dn_parse_symbol_count(image) || !image->symcount) {
        dn_logf(image, "link failed: cannot determine .dynsym count");
        return false;
    }
    if (!dn_ptr(image, image->dynsym, (uint64_t)image->symcount * 24u)) {
        dn_logf(image, "link failed: .dynsym extends outside the image");
        return false;
    }

    if (soname_offset != UINT64_MAX) {
        image->soname = dn_elf_strndup(image, soname_offset);
        if (!image->soname) {
            dn_logf(image, "link failed: invalid DT_SONAME");
            return false;
        }
    } else {
        const char *leaf = dn_basename_ptr(image->path);
        image->soname = dn_strdup_local(leaf);
        if (image->soname) {
            char *dylib_suffix = strstr(image->soname, ".dylib");
            if (dylib_suffix && dylib_suffix[6] == '\0')
                strcpy(dylib_suffix, ".so");
        }
    }

    dn_logf(image, "image %s: base=%p mapped ELF=%llu bytes, dynsym=%u, needed=%zu",
            image->soname ? image->soname : "(unnamed)", (void *)image->base,
            (unsigned long long)image->map_size, image->symcount,
            image->needed_count);
    if (image->has_verneed)
        dn_logf(image, "warning: symbol version requirements are present; DN linker v1 binds by name only");
    return true;
}

/* ------------------------------------------------------------------ */
/* Symbols                                                             */
/* ------------------------------------------------------------------ */

struct dn_symbol {
    uint32_t name_offset;
    uint8_t info;
    uint8_t other;
    uint16_t section_index;
    uint64_t value;
    uint64_t size;
};

struct dn_symbol_result {
    uintptr_t value;
    bool found;
    bool is_function;
    bool is_ifunc;
    bool is_tls;
    dn_native_tls_module_t *tls_module;
    uint64_t tls_offset;
};

static bool dn_read_symbol(const dn_native_image_t *image, uint32_t index,
                           struct dn_symbol *out)
{
    if (!image || !out || index >= image->symcount)
        return false;
    const uint8_t *entry = dn_ptr(image, image->dynsym + (uint64_t)index * 24, 24);
    if (!entry)
        return false;
    out->name_offset = dn_rd32(entry + 0);
    out->info = entry[4];
    out->other = entry[5];
    out->section_index = dn_rd16(entry + 6);
    out->value = dn_rd64(entry + 8);
    out->size = dn_rd64(entry + 16);
    return true;
}

static bool dn_symbol_is_export(const struct dn_symbol *symbol)
{
    uint8_t bind = symbol->info >> 4;
    uint8_t visibility = symbol->other & 3u;
    return symbol->section_index != DN_SHN_UNDEF &&
           (bind == DN_STB_GLOBAL || bind == DN_STB_WEAK) &&
           (visibility == 0 || visibility == 3);
}

static struct dn_symbol_result dn_defined_symbol_result(
    const dn_native_image_t *image, const struct dn_symbol *symbol)
{
    struct dn_symbol_result result = { 0 };
    if (!image || symbol->section_index == DN_SHN_UNDEF)
        return result;

    result.found = true;
    result.is_function = ((symbol->info & 0xfu) == DN_STT_FUNC);
    result.is_ifunc = ((symbol->info & 0xfu) == DN_STT_GNU_IFUNC);
    result.is_tls = ((symbol->info & 0xfu) == DN_STT_TLS);
    if (result.is_tls) {
        result.tls_module = (dn_native_tls_module_t *)image->tls_module;
        result.tls_offset = symbol->value;
        result.value = (uintptr_t)symbol->value;
        return result;
    }
    if (symbol->section_index == DN_SHN_ABS) {
        result.value = (uintptr_t)symbol->value;
    } else {
        result.value = (uintptr_t)image->base + (uintptr_t)symbol->value;
    }
    return result;
}

static struct dn_symbol_result dn_lookup_export_in_image(
    const dn_native_image_t *image, const char *name)
{
    struct dn_symbol_result weak = { 0 };
    for (uint32_t i = 0; i < image->symcount; ++i) {
        struct dn_symbol symbol;
        if (!dn_read_symbol(image, i, &symbol) || !dn_symbol_is_export(&symbol))
            continue;
        if (!dn_elf_string_equals(image, symbol.name_offset, name))
            continue;
        struct dn_symbol_result found = dn_defined_symbol_result(image, &symbol);
        if ((symbol.info >> 4) == DN_STB_GLOBAL)
            return found;
        if (!weak.found)
            weak = found;
    }
    return weak;
}

typedef void *(*dn_ifunc_resolver_fn)(uint64_t, void *);

static bool dn_call_ifunc(uintptr_t resolver_address, uintptr_t *out)
{
#if defined(__aarch64__) || defined(__arm64__)
    struct {
        uint64_t size;
        uint64_t hwcap;
        uint64_t hwcap2;
    } arg;
    arg.size = sizeof(arg);
    arg.hwcap = 0;
    arg.hwcap2 = 0;
    if (!resolver_address || !out)
        return false;
    void *resolved = ((dn_ifunc_resolver_fn)resolver_address)(
        arg.hwcap | (1ULL << 62), &arg);
    if (!resolved)
        return false;
    *out = (uintptr_t)resolved;
    return true;
#else
    (void)resolver_address;
    (void)out;
    return false;
#endif
}

static struct dn_symbol_result dn_lookup_external(dn_native_image_t *image,
                                                  const char *name)
{
    struct dn_symbol_result result = { 0 };

    if (strcmp(name, "__tls_get_addr") == 0) {
        result.value = (uintptr_t)dn_native_tls_get_addr;
        result.found = true;
        return result;
    }

    void *shim = dn_native_shim_lookup(name);
    if (shim) {
        result.value = (uintptr_t)shim;
        result.found = true;
        return result;
    }

    pthread_mutex_lock(&dn_registry_mutex);
    for (dn_native_image_t *candidate = dn_registry; candidate; candidate = candidate->next) {
        if (candidate == image || !candidate->linked)
            continue;
        result = dn_lookup_export_in_image(candidate, name);
        if (result.found)
            break;
    }
    pthread_mutex_unlock(&dn_registry_mutex);
    if (result.found)
        return result;

    /*
     * Do not fall back to an arbitrary process-global host symbol.
     * Android AArch64 and Darwin arm64 differ for variadic calls and
     * for several C object layouts, so silently binding whatever
     * dlsym(RTLD_DEFAULT) happens to find can turn a missing DN
     * implementation into memory corruption.  Host functions are
     * exposed to guest code only through DNNativeShims/Bionic/
     * Android entries whose ABI has been selected deliberately;
     * everything else fails closed here as a missing import.
     */
    return result;
}

static struct dn_symbol_result dn_resolve_symbol_index(
    dn_native_image_t *image, uint32_t index, char **missing_name)
{
    struct dn_symbol_result result = { 0 };
    if (missing_name)
        *missing_name = NULL;
    if (index == 0) {
        result.found = true;
        return result;
    }

    struct dn_symbol symbol;
    if (!dn_read_symbol(image, index, &symbol))
        return result;
    if (symbol.section_index != DN_SHN_UNDEF)
        return dn_defined_symbol_result(image, &symbol);

    char *name = dn_elf_strndup(image, symbol.name_offset);
    if (!name || !name[0]) {
        free(name);
        if ((symbol.info >> 4) == DN_STB_WEAK)
            result.found = true;
        return result;
    }

    result = dn_lookup_external(image, name);
    if (result.found) {
        free(name);
        return result;
    }

    if ((symbol.info >> 4) == DN_STB_WEAK) {
        free(name);
        result.found = true;
        result.value = 0;
        return result;
    }

    if (missing_name)
        *missing_name = name;
    else
        free(name);
    return result;
}

/* ------------------------------------------------------------------ */
/* Dependencies                                                        */
/* ------------------------------------------------------------------ */

static bool dn_is_host_backed_needed(const char *name)
{
    return name &&
           (strcmp(name, "libc.so") == 0 || strcmp(name, "libm.so") == 0 ||
            strcmp(name, "libdl.so") == 0 || strcmp(name, "liblog.so") == 0 ||
            strcmp(name, "libvulkan.so") == 0 || strcmp(name, "libandroid.so") == 0 ||
            strcmp(name, "libEGL.so") == 0 || strcmp(name, "libGLESv1_CM.so") == 0 ||
            strcmp(name, "libGLESv2.so") == 0 || strcmp(name, "libGLESv3.so") == 0 ||
            strcmp(name, "libaaudio.so") == 0 || strcmp(name, "libjnigraphics.so") == 0 ||
            strcmp(name, "libmediandk.so") == 0 || strcmp(name, "libcamera2ndk.so") == 0 ||
            strcmp(name, "libOpenSLES.so") == 0 || strcmp(name, "libz.so") == 0);
}

static bool dn_needed_is_linked(const char *name)
{
    bool found = false;
    pthread_mutex_lock(&dn_registry_mutex);
    for (dn_native_image_t *candidate = dn_registry; candidate; candidate = candidate->next) {
        if (candidate->linked && candidate->soname &&
            strcmp(candidate->soname, name) == 0) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&dn_registry_mutex);
    return found;
}

static bool dn_check_needed(dn_native_image_t *image)
{
    for (size_t i = 0; i < image->needed_count; ++i) {
        char *needed = dn_elf_strndup(image, image->needed_offsets[i]);
        if (!needed) {
            dn_logf(image, "link failed: invalid DT_NEEDED string at index %zu", i);
            return false;
        }
        if (image->soname && strcmp(needed, image->soname) == 0) {
            dn_logf(image, "needed %s -> self", needed);
        } else if (dn_is_host_backed_needed(needed)) {
            dn_logf(image, "needed %s -> DN host compatibility layer (subset; symbols are still preflighted)", needed);
        } else if (dn_needed_is_linked(needed)) {
            dn_logf(image, "needed %s -> converted image already linked", needed);
        } else {
            dn_logf(image, "link failed: DT_NEEDED %s is not linked yet", needed);
            free(needed);
            return false;
        }
        free(needed);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Relocations                                                         */
/* ------------------------------------------------------------------ */

struct dn_rela_plan {
    uint64_t offset;
    uint32_t type;
    uint32_t symbol;
    int64_t addend;
    uintptr_t value;
    uintptr_t secondary_value; /* TLSDESC descriptor argument */
    bool implicit_addend;
    int phase; /* 0 ordinary, 1 IFUNC symbol, 2 IRELATIVE */
};

struct dn_rela_plan_list {
    struct dn_rela_plan *items;
    size_t count;
    size_t cap;
};

static bool dn_plan_push(struct dn_rela_plan_list *list,
                         const struct dn_rela_plan *plan)
{
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 128;
        struct dn_rela_plan *next =
            (struct dn_rela_plan *)realloc(list->items, cap * sizeof(*next));
        if (!next)
            return false;
        list->items = next;
        list->cap = cap;
    }
    list->items[list->count++] = *plan;
    return true;
}

static bool dn_collect_rela_range(dn_native_image_t *image, uint64_t start,
                                  uint64_t size, uint64_t skip_start,
                                  uint64_t skip_end,
                                  struct dn_rela_plan_list *plans)
{
    if (!start || !size)
        return true;
    uint64_t entry_size = image->rela_entry_size ? image->rela_entry_size : 24;
    if (entry_size != 24 || size % entry_size != 0) {
        dn_logf(image, "link failed: unsupported RELA entry size %llu",
                (unsigned long long)entry_size);
        return false;
    }
    if (!dn_ptr(image, start, size)) {
        dn_logf(image, "link failed: RELA table outside the mapped image");
        return false;
    }

    for (uint64_t i = 0; i < size / entry_size; ++i) {
        uint64_t entry_offset = start + i * entry_size;
        if (skip_end > skip_start && entry_offset >= skip_start && entry_offset < skip_end)
            continue; /* JMPREL range already included in DT_RELA. */
        const uint8_t *entry = image->base + entry_offset;
        uint64_t info = dn_rd64(entry + 8);
        struct dn_rela_plan plan;
        memset(&plan, 0, sizeof(plan));
        plan.offset = dn_rd64(entry + 0);
        plan.type = (uint32_t)info;
        plan.symbol = (uint32_t)(info >> 32);
        plan.addend = (int64_t)dn_rd64(entry + 16);
        if (!dn_plan_push(plans, &plan))
            return false;
    }
    return true;
}

static bool dn_collect_rel_range(dn_native_image_t *image, uint64_t start,
                                 uint64_t size,
                                 struct dn_rela_plan_list *plans)
{
    if (!start || !size)
        return true;
    uint64_t entry_size = image->rel_entry_size ? image->rel_entry_size : 16;
    if (entry_size != 16 || size % entry_size != 0) {
        dn_logf(image, "link failed: unsupported REL entry size %llu",
                (unsigned long long)entry_size);
        return false;
    }
    if (!dn_ptr(image, start, size)) {
        dn_logf(image, "link failed: REL table outside the mapped image");
        return false;
    }
    for (uint64_t i = 0; i < size / entry_size; ++i) {
        const uint8_t *entry = image->base + start + i * entry_size;
        uint64_t info = dn_rd64(entry + 8);
        struct dn_rela_plan plan;
        memset(&plan, 0, sizeof(plan));
        plan.offset = dn_rd64(entry + 0);
        plan.type = (uint32_t)info;
        plan.symbol = (uint32_t)(info >> 32);
        plan.implicit_addend = true;
        if (!dn_plan_push(plans, &plan))
            return false;
    }
    return true;
}

static bool dn_read_sleb128(const uint8_t *data, uint64_t size,
                            uint64_t *position, int64_t *out)
{
    uint64_t result = 0;
    unsigned shift = 0;
    while (*position < size) {
        uint8_t byte = data[*position];
        (*position)++;
        if (shift >= 64)
            return false;
        result |= (uint64_t)(byte & 0x7fu) << shift;
        shift += 7;
        if ((byte & 0x80u) == 0) {
            if (shift < 64 && (byte & 0x40u))
                result |= UINT64_MAX << shift;
            *out = (int64_t)result;
            return true;
        }
    }
    return false;
}

static bool dn_collect_android_packed(dn_native_image_t *image, uint64_t start,
                                      uint64_t size, bool is_rela,
                                      struct dn_rela_plan_list *plans)
{
    if (!start || !size)
        return true;
    const uint8_t *data = dn_ptr(image, start, size);
    if (!data || size < 4 || memcmp(data, "APS2", 4) != 0) {
        dn_logf(image, "link failed: Android packed relocations are not APS2");
        return false;
    }

    uint64_t position = 4;
    int64_t signed_value = 0;
    if (!dn_read_sleb128(data, size, &position, &signed_value) ||
        signed_value < 0 || signed_value > 1000000) {
        dn_logf(image, "link failed: malformed APS2 relocation count");
        return false;
    }
    uint64_t total = (uint64_t)signed_value;
    if (!dn_read_sleb128(data, size, &position, &signed_value)) {
        dn_logf(image, "link failed: malformed APS2 initial offset");
        return false;
    }
    uint64_t offset = (uint64_t)signed_value;
    uint64_t info = 0;
    int64_t addend = 0;
    uint64_t processed = 0;

    enum {
        DN_APS2_GROUPED_BY_INFO = 1,
        DN_APS2_GROUPED_BY_OFFSET = 2,
        DN_APS2_GROUPED_BY_ADDEND = 4,
        DN_APS2_HAS_ADDEND = 8
    };

    while (processed < total) {
        int64_t group_size_raw = 0;
        int64_t group_flags_raw = 0;
        if (!dn_read_sleb128(data, size, &position, &group_size_raw) ||
            !dn_read_sleb128(data, size, &position, &group_flags_raw) ||
            group_size_raw <= 0 || group_flags_raw < 0) {
            dn_logf(image, "link failed: malformed APS2 group header");
            return false;
        }
        uint64_t group_size = (uint64_t)group_size_raw;
        uint64_t group_flags = (uint64_t)group_flags_raw;
        if (group_size > total - processed) {
            dn_logf(image, "link failed: APS2 group overruns relocation count");
            return false;
        }

        int64_t group_offset_delta = 0;
        if (group_flags & DN_APS2_GROUPED_BY_OFFSET) {
            if (!dn_read_sleb128(data, size, &position, &group_offset_delta))
                return false;
        }
        if (group_flags & DN_APS2_GROUPED_BY_INFO) {
            if (!dn_read_sleb128(data, size, &position, &signed_value))
                return false;
            info = (uint64_t)signed_value;
        }

        uint64_t addend_flags = group_flags &
            (DN_APS2_GROUPED_BY_ADDEND | DN_APS2_HAS_ADDEND);
        if (!is_rela && (group_flags & DN_APS2_HAS_ADDEND)) {
            dn_logf(image, "link failed: APS2 REL group unexpectedly has explicit addends");
            return false;
        }
        if (is_rela && addend_flags ==
                (DN_APS2_GROUPED_BY_ADDEND | DN_APS2_HAS_ADDEND)) {
            if (!dn_read_sleb128(data, size, &position, &signed_value))
                return false;
            addend += signed_value;
        } else if (is_rela && addend_flags != DN_APS2_HAS_ADDEND) {
            addend = 0;
        }

        for (uint64_t i = 0; i < group_size; ++i) {
            if (group_flags & DN_APS2_GROUPED_BY_OFFSET) {
                offset += (uint64_t)group_offset_delta;
            } else {
                if (!dn_read_sleb128(data, size, &position, &signed_value))
                    return false;
                offset += (uint64_t)signed_value;
            }
            if (!(group_flags & DN_APS2_GROUPED_BY_INFO)) {
                if (!dn_read_sleb128(data, size, &position, &signed_value))
                    return false;
                info = (uint64_t)signed_value;
            }
            if (is_rela && addend_flags == DN_APS2_HAS_ADDEND) {
                if (!dn_read_sleb128(data, size, &position, &signed_value))
                    return false;
                addend += signed_value;
            }

            struct dn_rela_plan plan;
            memset(&plan, 0, sizeof(plan));
            plan.offset = offset;
            plan.type = (uint32_t)info;
            plan.symbol = (uint32_t)(info >> 32);
            plan.addend = is_rela ? addend : 0;
            plan.implicit_addend = !is_rela;
            if (!dn_plan_push(plans, &plan))
                return false;
        }
        processed += group_size;
    }
    return true;
}

static bool dn_process_relr(dn_native_image_t *image, bool apply, size_t *out_count)
{
    size_t count = 0;
    if (!image->relr || !image->relr_size) {
        if (out_count)
            *out_count = 0;
        return true;
    }
    uint64_t entry_size = image->relr_entry_size ? image->relr_entry_size : 8;
    if (entry_size != 8 || image->relr_size % 8 != 0 ||
        !dn_ptr(image, image->relr, image->relr_size)) {
        dn_logf(image, "link failed: malformed DT_RELR table");
        return false;
    }

    uint64_t address = 0;
    for (uint64_t i = 0; i < image->relr_size / 8; ++i) {
        uint64_t word = dn_rd64(image->base + image->relr + i * 8);
        if ((word & 1u) == 0) {
            address = word & ~1ULL;
            if (!dn_in_load_with_flags(image, address, 8, DN_PF_W)) {
                dn_logf(image, "link failed: RELR target 0x%llx is not writable",
                        (unsigned long long)address);
                return false;
            }
            if (apply) {
                uint64_t old = dn_rd64(image->base + address);
                dn_wr64(image->base + address, old + (uint64_t)(uintptr_t)image->base);
            }
            count++;
            address += 8;
        } else {
            uint64_t bitmap = word;
            for (unsigned bit = 1; bit < 64; ++bit) {
                if (!(bitmap & (1ULL << bit)))
                    continue;
                uint64_t target = address + (uint64_t)(bit - 1) * 8;
                if (!dn_in_load_with_flags(image, target, 8, DN_PF_W)) {
                    dn_logf(image, "link failed: RELR bitmap target 0x%llx is not writable",
                            (unsigned long long)target);
                    return false;
                }
                if (apply) {
                    uint64_t old = dn_rd64(image->base + target);
                    dn_wr64(image->base + target,
                            old + (uint64_t)(uintptr_t)image->base);
                }
                count++;
            }
            address += 63 * 8;
        }
    }

    if (out_count)
        *out_count = count;
    return true;
}

struct dn_tls_resolution {
    bool found;
    dn_native_tls_module_t *module;
    uint64_t offset;
};

static struct dn_tls_resolution dn_resolve_tls_symbol(
    dn_native_image_t *image, uint32_t index, char **missing_name)
{
    struct dn_tls_resolution result;
    memset(&result, 0, sizeof(result));
    if (missing_name)
        *missing_name = NULL;
    if (index == 0) {
        result.found = image->tls_module != NULL;
        result.module = (dn_native_tls_module_t *)image->tls_module;
        return result;
    }

    struct dn_symbol symbol;
    if (!dn_read_symbol(image, index, &symbol))
        return result;
    if (symbol.section_index != DN_SHN_UNDEF) {
        struct dn_symbol_result defined = dn_defined_symbol_result(image, &symbol);
        result.found = defined.found && defined.is_tls && defined.tls_module != NULL;
        result.module = defined.tls_module;
        result.offset = defined.tls_offset;
        return result;
    }

    char *name = dn_elf_strndup(image, symbol.name_offset);
    if (!name || !name[0]) {
        free(name);
        if ((symbol.info >> 4) == DN_STB_WEAK)
            result.found = true;
        return result;
    }
    struct dn_symbol_result external = dn_lookup_external(image, name);
    if (external.found && external.is_tls && external.tls_module) {
        result.found = true;
        result.module = external.tls_module;
        result.offset = external.tls_offset;
        free(name);
        return result;
    }
    if ((symbol.info >> 4) == DN_STB_WEAK) {
        free(name);
        result.found = true;
        return result;
    }
    if (missing_name)
        *missing_name = name;
    else
        free(name);
    return result;
}

static bool dn_apply_relocations(dn_native_image_t *image)
{
    if (image->pltrel_type && image->pltrel_type != 7 /* DT_RELA */) {
        dn_logf(image, "link failed: DT_PLTREL type %llu is not RELA",
                (unsigned long long)image->pltrel_type);
        return false;
    }

    struct dn_rela_plan_list plans;
    memset(&plans, 0, sizeof(plans));

    if (!dn_collect_rela_range(image, image->rela, image->rela_size, 0, 0, &plans))
        goto fail;
    if (!dn_collect_rela_range(image, image->jmprel, image->pltrel_size,
                               image->rela, image->rela_size ? image->rela + image->rela_size : 0,
                               &plans))
        goto fail;
    if (!dn_collect_rel_range(image, image->rel, image->rel_size, &plans))
        goto fail;
    if (!dn_collect_android_packed(image, image->android_rela,
                                   image->android_rela_size, true, &plans))
        goto fail;
    if (!dn_collect_android_packed(image, image->android_rel,
                                   image->android_rel_size, false, &plans))
        goto fail;

    size_t relr_count = 0;
    if (!dn_process_relr(image, false, &relr_count))
        goto fail;

    size_t missing = 0;
    size_t unsupported = 0;
    size_t count_none = 0, count_relative = 0, count_glob = 0;
    size_t count_jump = 0, count_abs = 0, count_ifunc = 0, count_irelative = 0;
    size_t count_tls_dtpmod = 0, count_tls_dtprel = 0;
    size_t count_tls_tprel = 0, count_tlsdesc = 0;

    for (size_t i = 0; i < plans.count; ++i) {
        struct dn_rela_plan *plan = &plans.items[i];
        if (plan->type == DN_R_AARCH64_NONE) {
            count_none++;
            continue;
        }
        if (!dn_in_load_with_flags(image, plan->offset, 8, DN_PF_W)) {
            dn_logf(image, "link failed: relocation target 0x%llx is outside writable PT_LOAD",
                    (unsigned long long)plan->offset);
            goto fail;
        }

        if (plan->implicit_addend &&
            (plan->type == DN_R_AARCH64_RELATIVE ||
             plan->type == DN_R_AARCH64_GLOB_DAT ||
             plan->type == DN_R_AARCH64_ABS64)) {
            plan->addend = (int64_t)dn_rd64(image->base + plan->offset);
        }

        switch (plan->type) {
        case DN_R_AARCH64_RELATIVE:
            plan->value = (uintptr_t)((intptr_t)(uintptr_t)image->base + plan->addend);
            count_relative++;
            break;
        case DN_R_AARCH64_TLS_DTPMOD64:
        case DN_R_AARCH64_TLS_DTPREL64:
        case DN_R_AARCH64_TLS_TPREL64:
        case DN_R_AARCH64_TLSDESC: {
            char *missing_name = NULL;
            struct dn_tls_resolution tls =
                dn_resolve_tls_symbol(image, plan->symbol, &missing_name);
            if (!tls.found) {
                dn_logf(image,
                        "link failed: unresolved TLS relocation type %u for symbol '%s' index %u",
                        plan->type, missing_name ? missing_name : "",
                        plan->symbol);
                free(missing_name);
                goto fail;
            }
            uint64_t tls_value = tls.offset + (uint64_t)plan->addend;
            if (plan->type == DN_R_AARCH64_TLS_DTPMOD64) {
                plan->value = tls.module
                    ? (uintptr_t)dn_native_tls_module_id(tls.module) : 0;
                count_tls_dtpmod++;
            } else if (plan->type == DN_R_AARCH64_TLS_DTPREL64) {
                plan->value = (uintptr_t)tls_value;
                count_tls_dtprel++;
            } else if (plan->type == DN_R_AARCH64_TLS_TPREL64) {
                if (!tls.module) {
                    dn_logf(image,
                            "link failed: TLS module slot is unavailable for TPREL relocation");
                    goto fail;
                }
                plan->value = (uintptr_t)(
                    tls_value + dn_native_tls_module_offset(tls.module));
                count_tls_tprel++;
            } else {
                if (!dn_in_load_with_flags(image, plan->offset, 16, DN_PF_W)) {
                    dn_logf(image,
                            "link failed: TLSDESC slot at relocation offset 0x%llx is outside writable PT_LOAD",
                            (unsigned long long)plan->offset);
                    goto fail;
                }
                void *argument = dn_native_tls_descriptor_arg(
                    tls.module, tls_value);
                if (!argument) {
                    dn_logf(image,
                            "link failed: could not allocate TLSDESC argument for symbol index %u",
                            plan->symbol);
                    goto fail;
                }
                plan->value = (uintptr_t)dn_native_tlsdesc_entry;
                plan->secondary_value = (uintptr_t)argument;
                count_tlsdesc++;
            }
            break;
        }
        case DN_R_AARCH64_GLOB_DAT:
        case DN_R_AARCH64_JUMP_SLOT:
        case DN_R_AARCH64_ABS64: {
            char *missing_name = NULL;
            struct dn_symbol_result symbol =
                dn_resolve_symbol_index(image, plan->symbol, &missing_name);
            if (!symbol.found) {
                missing++;
                if (missing <= 12) {
                    dn_logf(image, "unresolved import: %s",
                            missing_name ? missing_name : "<unknown>");
                }
                free(missing_name);
                break;
            }
            free(missing_name);
            plan->value = symbol.value;
            if (symbol.is_ifunc) {
                plan->phase = 1;
                count_ifunc++;
            }
            if (plan->type == DN_R_AARCH64_GLOB_DAT)
                count_glob++;
            else if (plan->type == DN_R_AARCH64_JUMP_SLOT)
                count_jump++;
            else
                count_abs++;
            break;
        }
        case DN_R_AARCH64_IRELATIVE:
            plan->phase = 2;
            count_irelative++;
            break;
        default:
            unsupported++;
            if (unsupported <= 12) {
                dn_logf(image, "unsupported relocation type %u at 0x%llx",
                        plan->type, (unsigned long long)plan->offset);
            }
            break;
        }
    }

#if !defined(__aarch64__) && !defined(__arm64__)
    if (count_ifunc || count_irelative) {
        dn_logf(image, "link failed: IFUNC resolvers cannot run on this non-AArch64 test host");
        goto fail;
    }
#endif

    if (missing || unsupported) {
        dn_logf(image, "link failed: %zu unresolved import(s), %zu unsupported relocation(s); no relocations were applied",
                missing, unsupported);
        goto fail;
    }

    if (!dn_process_relr(image, true, &relr_count))
        goto fail;

    for (int phase = 0; phase <= 2; ++phase) {
        for (size_t i = 0; i < plans.count; ++i) {
            struct dn_rela_plan *plan = &plans.items[i];
            if (plan->type == DN_R_AARCH64_NONE || plan->phase != phase)
                continue;

            uintptr_t value = plan->value;
            if (phase == 1) {
                if (!dn_call_ifunc(plan->value, &value)) {
                    dn_logf(image, "link failed: IFUNC resolver at 0x%llx returned NULL",
                            (unsigned long long)plan->offset);
                    goto fail;
                }
            } else if (phase == 2) {
                uintptr_t resolver = (uintptr_t)((intptr_t)(uintptr_t)image->base + plan->addend);
                if (!dn_call_ifunc(resolver, &value)) {
                    dn_logf(image, "link failed: IRELATIVE resolver returned NULL");
                    goto fail;
                }
                dn_wr64(image->base + plan->offset, (uint64_t)value);
                continue;
            }

            if (plan->type == DN_R_AARCH64_TLSDESC) {
                dn_wr64(image->base + plan->offset, (uint64_t)value);
                dn_wr64(image->base + plan->offset + 8,
                        (uint64_t)plan->secondary_value);
            } else if (plan->type == DN_R_AARCH64_RELATIVE ||
                       plan->type == DN_R_AARCH64_TLS_DTPMOD64 ||
                       plan->type == DN_R_AARCH64_TLS_DTPREL64 ||
                       plan->type == DN_R_AARCH64_TLS_TPREL64) {
                dn_wr64(image->base + plan->offset, (uint64_t)value);
            } else {
                uintptr_t with_addend =
                    (uintptr_t)((intptr_t)value + plan->addend);
                dn_wr64(image->base + plan->offset, (uint64_t)with_addend);
            }
        }
    }

    dn_logf(image,
            "relocations OK: RELR=%zu RELATIVE=%zu GLOB_DAT=%zu JUMP_SLOT=%zu ABS64=%zu IFUNC=%zu IRELATIVE=%zu NONE=%zu TLS_DTPMOD=%zu TLS_DTPREL=%zu TLS_TPREL=%zu TLSDESC=%zu",
            relr_count, count_relative, count_glob, count_jump, count_abs,
            count_ifunc, count_irelative, count_none, count_tls_dtpmod,
            count_tls_dtprel, count_tls_tprel, count_tlsdesc);
    free(plans.items);
    return true;

fail:
    free(plans.items);
    return false;
}

/* ------------------------------------------------------------------ */
/* Initializers / finalizers                                           */
/* ------------------------------------------------------------------ */

typedef void (*dn_void_fn)(void);

static bool dn_normalize_code_pointer(dn_native_image_t *image, uint64_t raw,
                                      uintptr_t *out)
{
    if (!out)
        return false;
    *out = 0;
    if (!raw)
        return true;

    uint64_t base_value = (uint64_t)(uintptr_t)image->base;
    uint64_t offset;
    if (raw >= base_value && raw - base_value < image->map_size) {
        offset = raw - base_value;
    } else if (raw < image->map_size) {
        offset = raw;
    } else {
        return false;
    }
    if (!dn_in_load_with_flags(image, offset, 4, DN_PF_X))
        return false;
    *out = (uintptr_t)(image->base + offset);
    return true;
}

static bool dn_call_code_pointer(dn_native_image_t *image, uint64_t raw,
                                 const char *what)
{
    uintptr_t address = 0;
    if (!dn_normalize_code_pointer(image, raw, &address)) {
        dn_logf(image, "link failed: %s pointer 0x%llx is outside executable PT_LOAD",
                what, (unsigned long long)raw);
        return false;
    }
    if (address && !image->suppress_initializers) {
        dn_logf(image, "stage: calling %s at 0x%llx", what,
                (unsigned long long)address);
        ((dn_void_fn)address)();
        dn_logf(image, "stage OK: returned from %s", what);
    }
    return true;
}

static bool dn_run_initializers(dn_native_image_t *image)
{
    if (image->initializers_run)
        return true;
    if (image->preinit_array) {
        dn_logf(image, "link failed: DT_PREINIT_ARRAY in a shared library is not supported");
        return false;
    }
    if ((image->has_patch_island || image->tls_module) &&
        !dn_native_thread_pointer()) {
        dn_logf(image,
                "link failed: could not allocate DN thread pointer before initializers");
        return false;
    }

    size_t total = 0;
    if (image->init) {
        if (!dn_call_code_pointer(image, image->init, "DT_INIT"))
            return false;
        total++;
    }
    if (image->init_array && image->init_array_size) {
        if (image->init_array_size % 8 != 0 ||
            !dn_ptr(image, image->init_array, image->init_array_size)) {
            dn_logf(image, "link failed: malformed DT_INIT_ARRAY");
            return false;
        }
        size_t count = (size_t)(image->init_array_size / 8);
        for (size_t i = 0; i < count; ++i) {
            uint64_t raw = dn_rd64(image->base + image->init_array + i * 8);
            if (!dn_call_code_pointer(image, raw, "DT_INIT_ARRAY"))
                return false;
            if (raw)
                total++;
        }
    }

    image->initializers_run = true;
    dn_logf(image, "initializers OK: %zu function(s)%s", total,
            image->suppress_initializers ? " (suppressed on this test host)" : "");
    return true;
}

int dn_native_image_run_finalizers(dn_native_image_t *image)
{
    if (!image || !image->linked || !image->initializers_run)
        return -1;
    if (image->finalizers_run)
        return 0;
    image->finalizers_run = true;

    size_t total = 0;
    if (image->fini_array && image->fini_array_size) {
        if (image->fini_array_size % 8 != 0 ||
            !dn_ptr(image, image->fini_array, image->fini_array_size)) {
            dn_logf(image, "finalizers failed: malformed DT_FINI_ARRAY");
            return -1;
        }
        size_t count = (size_t)(image->fini_array_size / 8);
        for (size_t i = count; i > 0; --i) {
            uint64_t raw = dn_rd64(image->base + image->fini_array + (i - 1) * 8);
            if (!dn_call_code_pointer(image, raw, "DT_FINI_ARRAY"))
                return -1;
            if (raw)
                total++;
        }
    }
    if (image->fini) {
        if (!dn_call_code_pointer(image, image->fini, "DT_FINI"))
            return -1;
        total++;
    }
    dn_logf(image, "finalizers OK: %zu function(s)", total);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry / lifecycle                                                */
/* ------------------------------------------------------------------ */

static dn_native_image_t *dn_find_registered_by_path(const char *path)
{
    dn_native_image_t *found = NULL;
    pthread_mutex_lock(&dn_registry_mutex);
    for (dn_native_image_t *candidate = dn_registry; candidate; candidate = candidate->next) {
        if (candidate->path && strcmp(candidate->path, path) == 0) {
            candidate->refcount++;
            found = candidate;
            break;
        }
    }
    pthread_mutex_unlock(&dn_registry_mutex);
    return found;
}

static void dn_register_image(dn_native_image_t *image)
{
    pthread_mutex_lock(&dn_registry_mutex);
    image->next = dn_registry;
    dn_registry = image;
    pthread_mutex_unlock(&dn_registry_mutex);
}

static dn_native_image_t *dn_new_image(const char *path, void *handle)
{
    dn_native_image_t *image = (dn_native_image_t *)calloc(1, sizeof(*image));
    if (!image)
        return NULL;
    image->path = dn_strdup_local(path ? path : "");
    image->dlopen_handle = handle;
    image->refcount = 1;
    dn_report_reserve(image, 1024);
    return image;
}

/* Fail closed if executable bytes still contain the two Android
 * instructions Darwin cannot execute with Android semantics.  A
 * correctly converted image has had every svc #0 and every
 * mrs xN,tpidr_el0 in its executable loads rewritten into branches to
 * the patch island, so the raw encodings below must be absent.  If an
 * older converter left a site untouched (or this linker failed to see
 * the island that would serve it), executing the image on device
 * would read Darwin's CPU number as a bionic thread pointer - or trap
 * with a Linux syscall number - and crash the whole app at the first
 * such instruction.  Refuse the link instead, with counts the user
 * can act on: reconvert with a current patch-island converter. */
static bool dn_scan_unpatched_sites(dn_native_image_t *image)
{
    uint64_t svc_sites = 0;
    uint64_t mrs_sites = 0;
    uint64_t msr_sites = 0;

    for (size_t i = 0; i < image->load_count; ++i) {
        const struct dn_load_range *load = &image->loads[i];
        if ((load->flags & DN_PF_X) == 0 || load->end <= load->start)
            continue;
        uint64_t length = load->end - load->start;
        const uint8_t *bytes = dn_ptr(image, load->start, length);
        if (!bytes) {
            dn_logf(image, "link failed: executable PT_LOAD lies outside the mapped image");
            return false;
        }
        for (uint64_t offset = 0; offset + 4 <= length; offset += 4) {
            uint32_t word = dn_rd32(bytes + offset);
            if (word == 0xd4000001u) {           /* svc #0 */
                svc_sites++;
            } else if ((word & 0xffffffe0u) == 0xd53bd040u) {
                if ((word & 0x1fu) != 31u)       /* mrs xN,tpidr_el0 (xzr is a nop) */
                    mrs_sites++;
            } else if ((word & 0xffffffe0u) == 0xd51bd040u) {
                msr_sites++;                     /* msr tpidr_el0,xN */
            }
        }
    }

    if (svc_sites == 0 && mrs_sites == 0 && msr_sites == 0)
        return true;

    dn_logf(image,
            "link failed: converted image still contains unpatched svc=%llu mrs-tpidr=%llu msr-tpidr=%llu site(s); patch island %s. Rebuild this fixture on the Mac so the converter rewrites those instructions (the make scripts now refresh the converter and verify the island section).",
            (unsigned long long)svc_sites, (unsigned long long)mrs_sites,
            (unsigned long long)msr_sites,
            image->has_patch_island ? "was found but does not cover them"
                                    : "was not found in the Mach-O");
    return false;
}

static bool dn_register_image_tls(dn_native_image_t *image);
static bool dn_setup_patch_island(dn_native_image_t *image);

static int dn_link_common(dn_native_image_t *image)
{
    if (!dn_parse_elf(image))
        return -1;

    if (!dn_scan_unpatched_sites(image))
        return -1;
    dn_logf(image, "stage OK: unpatched-site scan (no raw svc/tpidr sites)");
    if (!dn_setup_patch_island(image))
        return -1;
    dn_logf(image, "stage OK: patch-island setup");
    if (!dn_register_image_tls(image))
        return -1;
    dn_logf(image, "stage OK: ELF TLS registration");
    if (image->preinit_array && image->preinit_array_size) {
        dn_logf(image,
                "link failed: DT_PREINIT_ARRAY belongs to an executable image, not a shared library; refusing");
        return -1;
    }
    if (image->has_textrel) {
        dn_logf(image, "link failed: DT_TEXTREL would require modifying signed code pages");
        return -1;
    }
    if (!dn_check_needed(image))
        return -1;
    dn_logf(image, "stage OK: DT_NEEDED preflight");
    if (!dn_apply_relocations(image))
        return -1;

    image->linked = true;
    dn_register_image(image);

    if (!dn_run_initializers(image))
        return -1;

    dn_logf(image, "dn-link OK: %s is bound and initialized",
            image->soname ? image->soname : "(unnamed)");
    return 0;
}

static bool dn_register_image_tls(dn_native_image_t *image)
{
    if (image->tls_module)
        return true;
    if (!image->tls_mem_size) {
        if (image->has_static_tls) {
            dn_logf(image,
                    "DF_STATIC_TLS is set but PT_TLS carries no storage; continuing without an empty TLS module");
        }
        return true;
    }
    if (image->tls_file_size > image->tls_mem_size) {
        dn_logf(image, "link failed: PT_TLS filesz exceeds memsz");
        return false;
    }
    const void *template_data = NULL;
    if (image->tls_file_size) {
        template_data = dn_ptr(image, image->tls_vaddr, image->tls_file_size);
        if (!template_data) {
            dn_logf(image, "link failed: PT_TLS template is outside the mapped image");
            return false;
        }
    }
    image->tls_module = dn_native_tls_register(
        template_data, (size_t)image->tls_file_size,
        (size_t)image->tls_mem_size, (size_t)image->tls_alignment);
    if (!image->tls_module) {
        dn_logf(image, "link failed: DN TLS registration failed");
        return false;
    }
    if (!dn_native_thread_pointer()) {
        dn_logf(image, "link failed: DN thread-pointer allocation failed");
        return false;
    }
    dn_logf(image,
            "TLS module registered: file=%llu mem=%llu align=%llu",
            (unsigned long long)image->tls_file_size,
            (unsigned long long)image->tls_mem_size,
            (unsigned long long)image->tls_alignment);
    return true;
}

static bool dn_setup_patch_island(dn_native_image_t *image)
{
    if (!image->has_patch_island)
        return true;
    if (image->patch_version != DN_PATCH_ISLAND_VERSION ||
        !image->patch_slots_address || !image->patch_slots_writable ||
        (image->patch_slots_address % (uintptr_t)sizeof(uintptr_t)) != 0) {
        dn_logf(image,
                "link failed: unsupported DN patch-island version %u or missing/unwritable slots",
                image->patch_version);
        return false;
    }
    char patch_error[256];
    patch_error[0] = '\0';
    if (dn_native_patch_init(patch_error, sizeof(patch_error)) != 0) {
        dn_logf(image, "link failed: DN patch runtime init failed: %s",
                patch_error[0] ? patch_error : "unknown error");
        return false;
    }
    uintptr_t *slots = (uintptr_t *)image->patch_slots_address;
    slots[DN_PATCH_SLOT_SYSCALL] = (uintptr_t)dn_native_svc_entry;
    slots[DN_PATCH_SLOT_TSD_OFFSET] = dn_native_patch_tp_key_offset();
    slots[DN_PATCH_SLOT_TP_SLOW] = (uintptr_t)dn_native_tp_slow_entry;
    slots[DN_PATCH_SLOT_RESERVED] = 0;
    if (!dn_native_thread_pointer()) {
        dn_logf(image,
                "link failed: DN patched-image thread-pointer allocation failed");
        return false;
    }
    dn_logf(image,
            "patch island armed: svc sites=%u tpidr sites=%u tsd_offset=%llu",
            image->patch_svc_sites, image->patch_tpidr_sites,
            (unsigned long long)dn_native_patch_tp_key_offset());
    return true;
}

void dn_native_image_dispose(dn_native_image_t *image)
{
    if (!image)
        return;

    bool should_free = false;
    pthread_mutex_lock(&dn_registry_mutex);
    image->refcount--;
    if (image->refcount <= 0) {
        should_free = true;
        dn_native_image_t **cursor = &dn_registry;
        while (*cursor) {
            if (*cursor == image) {
                *cursor = image->next;
                break;
            }
            cursor = &(*cursor)->next;
        }
    }
    pthread_mutex_unlock(&dn_registry_mutex);

    if (!should_free)
        return;

    if (image->tls_module)
        dn_native_tls_unregister(image->tls_module);
    free(image->path);
    free(image->soname);
    free(image->loads);
    free(image->needed_offsets);
    free(image->report);
    free(image);
}

const char *dn_native_image_report(dn_native_image_t *image)
{
    return image && image->report ? image->report : "";
}

void *dn_native_image_symbol(dn_native_image_t *image, const char *name)
{
    if (!image || !image->linked || !name)
        return NULL;
    struct dn_symbol_result result = dn_lookup_export_in_image(image, name);
    if (!result.found)
        return NULL;
    if (result.is_ifunc) {
        uintptr_t resolver = 0;
        uintptr_t resolved = 0;
        if (!dn_normalize_code_pointer(image, result.value, &resolver) ||
            !dn_call_ifunc(resolver, &resolved) ||
            !dn_normalize_code_pointer(image, resolved, &resolved))
            return NULL;
        return (void *)resolved;
    }
    if (result.is_tls)
        return dn_native_tls_address(result.tls_module, result.tls_offset);
    if (result.is_function) {
        uintptr_t normalized = 0;
        if (!dn_normalize_code_pointer(image, result.value, &normalized))
            return NULL;
        return (void *)normalized;
    }
    return (void *)result.value;
}

void *dn_native_global_symbol(const char *name)
{
    if (!name || !name[0])
        return NULL;
    if (strcmp(name, "__tls_get_addr") == 0)
        return (void *)dn_native_tls_get_addr;

    void *shim = dn_native_shim_lookup(name);
    if (shim)
        return shim;

    /*
     * dn_native_image_symbol itself resolves IFUNCs and current-thread TLS.
     * Hold the registry lock only while copying the image pointer; images
     * are reference-counted by their owners for this lookup path and are not
     * unloaded by the DN runtime.
     */
    pthread_mutex_lock(&dn_registry_mutex);
    for (dn_native_image_t *candidate = dn_registry; candidate;
         candidate = candidate->next) {
        if (!candidate->linked)
            continue;
        struct dn_symbol_result result =
            dn_lookup_export_in_image(candidate, name);
        if (!result.found)
            continue;
        candidate->refcount++;
        pthread_mutex_unlock(&dn_registry_mutex);
        void *resolved = dn_native_image_symbol(candidate, name);
        dn_native_image_dispose(candidate);
        return resolved;
    }
    pthread_mutex_unlock(&dn_registry_mutex);
    /* Same fail-closed rule as static import resolution above: no
     * arbitrary host-global fallback with an unverified guest ABI. */
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Mach-O image discovery (Apple) / test entry                         */
/* ------------------------------------------------------------------ */

#if defined(__APPLE__)
static bool dn_path_has_leaf(const char *path, const char *leaf)
{
    if (!path || !leaf)
        return false;
    const char *path_leaf = dn_basename_ptr(path);
    return strcmp(path_leaf, leaf) == 0;
}

static bool dn_find_mapped_elf(dn_native_image_t *image)
{
    const char *leaf = dn_basename_ptr(image->path);
    uint32_t count = _dyld_image_count();

    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (!name || (strcmp(name, image->path) != 0 && !dn_path_has_leaf(name, leaf)))
            continue;

        const struct mach_header_64 *header =
            (const struct mach_header_64 *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        if (!header || header->magic != MH_MAGIC_64)
            continue;

        uint64_t segment_vmaddrs[32];
        uint64_t segment_vmsizes[32];
        int segment_initprots[32];
        size_t segment_count = 0;
        const uint8_t *cursor = (const uint8_t *)header + sizeof(*header);
        for (uint32_t c = 0; c < header->ncmds; ++c) {
            const struct load_command *command = (const struct load_command *)cursor;
            if (command->cmdsize < sizeof(*command))
                break;
            if (command->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *segment =
                    (const struct segment_command_64 *)command;
                if (segment_count < 32) {
                    segment_vmaddrs[segment_count] = segment->vmaddr;
                    segment_vmsizes[segment_count] = segment->vmsize;
                    segment_initprots[segment_count] = segment->initprot;
                    segment_count++;
                }
                const struct section_64 *sections =
                    (const struct section_64 *)(cursor + sizeof(*segment));
                for (uint32_t s = 0; s < segment->nsects; ++s) {
                    const struct section_64 *section = &sections[s];
                    uintptr_t runtime = (uintptr_t)(section->addr + (uint64_t)slide);
                    if (strncmp(section->segname, "__TEXT", 16) == 0 &&
                        strncmp(section->sectname, "__elf", 16) == 0) {
                        image->base = (uint8_t *)runtime;
                    } else if (strncmp(section->segname, "__TEXT", 16) == 0 &&
                               strncmp(section->sectname, "__henge", 16) == 0 &&
                               section->size >= 32) {
                        const uint8_t *island = (const uint8_t *)runtime;
                        if (dn_rd32(island + 0) == DN_PATCH_ISLAND_MAGIC) {
                            image->has_patch_island = true;
                            image->patch_version = dn_rd32(island + 4);
                            image->patch_slots_vmaddr = dn_rd64(island + 8);
                            image->patch_slots_address =
                                (uintptr_t)header +
                                (uintptr_t)image->patch_slots_vmaddr;
                            image->patch_svc_sites = dn_rd32(island + 16);
                            image->patch_tpidr_sites = dn_rd32(island + 20);
                        }
                    }
                }
            }
            cursor += command->cmdsize;
        }
        if (image->base) {
            if (image->has_patch_island) {
                image->patch_slots_writable = false;
                for (size_t s = 0; s < segment_count; ++s) {
                    uint64_t start = segment_vmaddrs[s];
                    uint64_t size = segment_vmsizes[s];
                    uint64_t slots = image->patch_slots_vmaddr;
                    if ((segment_initprots[s] & VM_PROT_WRITE) != 0 &&
                        slots >= start && slots - start <= size &&
                        size - (slots - start) >= 4 * sizeof(uintptr_t)) {
                        image->patch_slots_writable = true;
                        break;
                    }
                }
                if (!image->patch_slots_writable) {
                    dn_logf(image,
                            "link failed: patch-island slots at Mach-O vmaddr 0x%llx are not inside a writable segment; refusing to write them",
                            (unsigned long long)image->patch_slots_vmaddr);
                    return false;
                }
            }
            return true;
        }
    }

    dn_logf(image, "link failed: loaded Mach-O has no __TEXT,__elf section (not a converted ELF image?)");
    return false;
}
#endif

int dn_native_link_image(const char *path, void *dlopen_handle,
                         dn_native_image_t **out_image)
{
    if (out_image)
        *out_image = NULL;
    if (!path || !out_image)
        return -1;

    dn_native_image_t *existing = dn_find_registered_by_path(path);
    if (existing) {
        dn_logf(existing, "already linked; reusing registered image");
        *out_image = existing;
        return 0;
    }

    dn_native_image_t *image = dn_new_image(path, dlopen_handle);
    if (!image)
        return -1;
    *out_image = image;

#if defined(__APPLE__)
    (void)dlopen_handle;
    if (!dn_find_mapped_elf(image))
        return -1;
    return dn_link_common(image);
#else
    (void)dlopen_handle;
    dn_logf(image, "link failed: Mach-O image discovery is only available on Apple platforms");
    return -1;
#endif
}

#if defined(DN_NATIVE_LINKER_TEST_HOOKS)
int dn_native_link_memory_for_test(void *base, const char *path_label,
                                   int run_initializers,
                                   dn_native_image_t **out_image)
{
    if (out_image)
        *out_image = NULL;
    if (!base || !out_image)
        return -1;
    dn_native_image_t *image = dn_new_image(path_label ? path_label : "memory-image", NULL);
    if (!image)
        return -1;
    *out_image = image;
    image->base = (uint8_t *)base;
    image->suppress_initializers = !run_initializers;
    return dn_link_common(image);
}

int dn_native_link_memory_with_patch_for_test(
    void *base, const char *path_label, int run_initializers,
    void *patch_slots, unsigned patch_svc_sites,
    unsigned patch_tpidr_sites, dn_native_image_t **out_image)
{
    if (out_image)
        *out_image = NULL;
    if (!base || !out_image)
        return -1;
    dn_native_image_t *image = dn_new_image(
        path_label ? path_label : "memory-image", NULL);
    if (!image)
        return -1;
    *out_image = image;
    image->base = (uint8_t *)base;
    image->suppress_initializers = !run_initializers;
    if (patch_slots) {
        image->has_patch_island = true;
        image->patch_slots_writable = true;
        image->patch_version = DN_PATCH_ISLAND_VERSION;
        image->patch_slots_address = (uintptr_t)patch_slots;
        image->patch_svc_sites = patch_svc_sites;
        image->patch_tpidr_sites = patch_tpidr_sites;
    }
    return dn_link_common(image);
}
#endif
