#import <Foundation/Foundation.h>
#include "JIT26Bridge.h"

#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Protocolul JIT iOS 26/27 ("universal script", StikDebug/StikJIT)
//
// Pe siliciu cu TXM/SPTM (A15+), o pagina scrisa de proces NU devine
// executable by merely attaching the debugger: each RX region
// must be prepared THROUGH the debug connection (the debugger writes one
// byte on each 16 KB page -> the page gains execution rights).
// The app asks for this through a conventional breakpoint:
//     brk #0xf00d, cu x16 = comanda (0=detach, 1=prepare-region), x0/x1 = arg.
// The StikDebug script intercepts the breakpoint, does the writes and
// intoarce adresa RX in x0.
//
// REGULI HARD (incalcarea = crash sau arena inutilizabila):
// 1. No brk without confirmed CS_DEBUGGED -> unhandled SIGTRAP -> process killed.
// 2. All regions are authorized BEFORE detach; later regions
//    need a new attach. That is why QEMU gets ONE single large arena.
// 3. The RX region is not writable: the TCG compiler writes via the RW alias
//    (vm_remap over the same physical pages) and executes via RX (W^X).
// ---------------------------------------------------------------------------

#if defined(__arm64__)
__attribute__((noinline, optnone, naked))
void *dn_jit26_prepare_region(void *address, size_t length) {
    __asm__(
        "mov x16, #1\n"
        "brk #0xf00d\n"
        "ret\n"
    );
}

__attribute__((noinline, optnone, naked))
void dn_jit26_detach(void) {
    __asm__(
        "mov x16, #0\n"
        "brk #0xf00d\n"
        "ret\n"
    );
}
#else
void *dn_jit26_prepare_region(void *address, size_t length) { (void)address; (void)length; return NULL; }
void dn_jit26_detach(void) {}
#endif

// csops(): private but stable syscall, used by the whole ecosystem (StikDebug, emulators).
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define DN_CS_OPS_STATUS 0
#define DN_CS_GET_TASK_ALLOW (1u << 2)
#define DN_CS_DEBUGGED (1u << 28)

static uint32_t dn_cs_flags(void) {
    uint32_t flags = 0;
    if (csops(getpid(), DN_CS_OPS_STATUS, &flags, sizeof(flags)) == 0) return flags;
    return 0;
}

bool dn_is_debugged(void) {
    uint32_t flags = dn_cs_flags();
    if (flags != 0) return (flags & DN_CS_DEBUGGED) != 0;
    // Fallback if csops does not answer: P_TRACED via sysctl.
    return dn_is_traced();
}

bool dn_is_traced(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, NULL, 0) == 0) {
        return (info.kp_proc.p_flag & P_TRACED) != 0;
    }
    return false;
}

int dn_jit_rx_mmap_probe_size(size_t bytes) {
    /* We write and execute nothing in the mapping — only check that TXM
     * accepta la marimea asta. mmap anonim nu comite RSS la mapare. */
    if (bytes == 0) {
        return EINVAL;
    }
    void *p = mmap(NULL, bytes, PROT_READ | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return errno;
    }
    munmap(p, bytes);
    return 0;
}

int dn_jit_rx_mmap_probe(void) {
    long page = sysconf(_SC_PAGESIZE);
    return dn_jit_rx_mmap_probe_size(page > 0 ? (size_t)page : 16384);
}

bool dn_has_get_task_allow(void) {
    return (dn_cs_flags() & DN_CS_GET_TASK_ALLOW) != 0;
}

bool dn_txm_present(void) {
    // Heuristic used in the ecosystem: presence of the TXM firmware in preboot.
    // The path contains a volume UUID -> enumerate /private/preboot/*.
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *volumes = [fm contentsOfDirectoryAtPath:@"/private/preboot" error:nil];
    for (NSString *vol in volumes) {
        NSString *p = [NSString stringWithFormat:
            @"/private/preboot/%@/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4", vol];
        if ([fm fileExistsAtPath:p]) return true;
    }
    // If the sandbox refuses enumeration, assume TXM on iOS >= 26
    // (this project's iOS 26/27 target devices are A15+, hence with TXM;
    //  the cost of a wrong guess is only asking for the full protocol, which is harmless).
    NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
    return v.majorVersion >= 26;
}

bool dn_probe_hypervisor(void) {
    // Hypervisor.framework nu e in SDK-ul public iOS -> cautam simbolurile dinamic.
    void *handle = dlopen("/System/Library/Frameworks/Hypervisor.framework/Hypervisor", RTLD_LAZY);
    if (!handle) return false;
    typedef int32_t (*hv_vm_create_t)(void *config);
    typedef int32_t (*hv_vm_destroy_t)(void);
    hv_vm_create_t createFn = (hv_vm_create_t)dlsym(handle, "hv_vm_create");
    hv_vm_destroy_t destroyFn = (hv_vm_destroy_t)dlsym(handle, "hv_vm_destroy");
    if (!createFn || !destroyFn) return false;
    if (createFn(NULL) != 0) return false;   // on stock iOS: failure (missing entitlement)
    destroyFn();
    return true;
}

void *dn_jit_make_writable_alias(void *rx_address, size_t length) {
    vm_address_t target = 0;
    vm_prot_t curProt = 0, maxProt = 0;
    kern_return_t kr = vm_remap(mach_task_self(),
                                &target,
                                (vm_size_t)length,
                                0,
                                VM_FLAGS_ANYWHERE,
                                mach_task_self(),
                                (vm_address_t)rx_address,
                                FALSE,
                                &curProt,
                                &maxProt,
                                VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) return NULL;
    kr = vm_protect(mach_task_self(), target, (vm_size_t)length, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), target, (vm_size_t)length);
        return NULL;
    }
    return (void *)target;
}

// ---------------------------------------------------------------------------
// Proba empirica de executabilitate — tipar S5LBox VMJitProbe (MIT).
// DroidNative adaptation: same sentinel payload and signal guard,
// plus the TXM arena-specific self-test (write via RW, execute via RX).
// ---------------------------------------------------------------------------

#define DN_PROBE_SENTINEL 0x5a5au
#if defined(__arm64__)
static const uint32_t DN_PROBE_CODE[] = {
    0xd28b4b40u,  // movz x0, #0x5a5a
    0xd65f03c0u,  // ret
};
#endif
typedef uint32_t (*dn_probe_fn_t)(void);

static sigjmp_buf g_dn_probe_jmp;
static volatile sig_atomic_t g_dn_probe_armed = 0;
static const int DN_PROBE_SIGNALS[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP };
#define DN_PROBE_NSIGNALS ((int)(sizeof(DN_PROBE_SIGNALS) / sizeof(DN_PROBE_SIGNALS[0])))

static void dn_probe_handler(int sig) {
    if (g_dn_probe_armed) {
        g_dn_probe_armed = 0;
        siglongjmp(g_dn_probe_jmp, 1);
    }
    // Not ours: restore the default behavior and re-raise,
    // ca un fault real din alta parte sa produca un crash report real.
    signal(sig, SIG_DFL);
    raise(sig);
}

static bool dn_install_handlers(struct sigaction *saved) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = dn_probe_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    for (int i = 0; i < DN_PROBE_NSIGNALS; i++) {
        if (sigaction(DN_PROBE_SIGNALS[i], &sa, &saved[i]) != 0) {
            for (int j = 0; j < i; j++)
                (void)sigaction(DN_PROBE_SIGNALS[j], &saved[j], NULL);
            return false;
        }
    }
    return true;
}

static void dn_restore_handlers(struct sigaction *saved) {
    for (int i = 0; i < DN_PROBE_NSIGNALS; i++)
        (void)sigaction(DN_PROBE_SIGNALS[i], &saved[i], NULL);
}

/// Writes the payload at rw (may be == rx), syncs the I-cache and
/// executa de la rx sub garda de semnal. Omiterea invalidarii I-cache e
/// the classic way to get a false FAULTED (what was there before executes).
static dn_jit_probe_result_t dn_write_and_execute(void *rx, void *rw) {
#if !defined(__arm64__)
    (void)rx; (void)rw;
    return DN_JIT_PROBE_UNTESTED;
#else
    memcpy(rw, DN_PROBE_CODE, sizeof DN_PROBE_CODE);
    sys_icache_invalidate(rw, sizeof DN_PROBE_CODE);
    // La vedere dubla (arena TXM), invalidam si pe adresa de executie:
    // the icache must be flushed for the range the CPU will fetch instructions from.
    if (rx != rw) sys_icache_invalidate(rx, sizeof DN_PROBE_CODE);

    struct sigaction saved[DN_PROBE_NSIGNALS];
    if (!dn_install_handlers(saved)) return DN_JIT_PROBE_UNTESTED;

    volatile uint32_t observed = 0;
    volatile dn_jit_probe_result_t result;
    g_dn_probe_armed = 1;
    if (sigsetjmp(g_dn_probe_jmp, 1) == 0) {
        observed = ((dn_probe_fn_t)rx)();
        g_dn_probe_armed = 0;
        result = (observed == DN_PROBE_SENTINEL) ? DN_JIT_PROBE_OK : DN_JIT_PROBE_WRONG_VALUE;
    } else {
        result = DN_JIT_PROBE_FAULTED;
    }
    dn_restore_handlers(saved);
    return result;
#endif
}

dn_jit_probe_result_t dn_jit_arena_selftest(void *rx_address, void *rw_alias) {
    if (!rx_address || !rw_alias) return DN_JIT_PROBE_UNTESTED;
    return dn_write_and_execute(rx_address, rw_alias);
}

static dn_jit_probe_result_t dn_probe_one_strategy(int extra_prot, int extra_flags, bool mprotect_after) {
    void * volatile page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | extra_prot,
                                 MAP_PRIVATE | MAP_ANON | extra_flags, -1, 0);
    if (page == MAP_FAILED) return DN_JIT_PROBE_MAP_REFUSED;
    volatile dn_jit_probe_result_t result;
    if (mprotect_after) {
        // Write while RW, then cut writing and ask for execution (W^X).
#if defined(__arm64__)
        memcpy(page, DN_PROBE_CODE, sizeof DN_PROBE_CODE);
#endif
        if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) {
            result = DN_JIT_PROBE_MAP_REFUSED;
            munmap(page, 4096);
            return result;
        }
        sys_icache_invalidate(page, 4096);
        // The page is no longer writable: execute what we already wrote.
        struct sigaction saved[DN_PROBE_NSIGNALS];
        if (!dn_install_handlers(saved)) { munmap(page, 4096); return DN_JIT_PROBE_UNTESTED; }
        volatile uint32_t observed = 0;
        g_dn_probe_armed = 1;
        if (sigsetjmp(g_dn_probe_jmp, 1) == 0) {
            observed = ((dn_probe_fn_t)page)();
            g_dn_probe_armed = 0;
            result = (observed == DN_PROBE_SENTINEL) ? DN_JIT_PROBE_OK : DN_JIT_PROBE_WRONG_VALUE;
        } else {
            result = DN_JIT_PROBE_FAULTED;
        }
        dn_restore_handlers(saved);
    } else {
        result = dn_write_and_execute(page, page);
    }
    munmap(page, 4096);
    return result;
}

dn_jit_probe_result_t dn_jit_probe_classic(void) {
    dn_jit_probe_result_t r = dn_probe_one_strategy(PROT_EXEC, 0, false);       // RWX
    if (r == DN_JIT_PROBE_OK) return r;
#ifdef MAP_JIT
    dn_jit_probe_result_t r2 = dn_probe_one_strategy(PROT_EXEC, MAP_JIT, false); // MAP_JIT
    if (r2 == DN_JIT_PROBE_OK) return r2;
    if (r2 == DN_JIT_PROBE_FAULTED) r = r2;
#endif
    dn_jit_probe_result_t r3 = dn_probe_one_strategy(0, 0, true);               // mprotect RW->RX
    if (r3 == DN_JIT_PROBE_OK) return r3;
    return (r == DN_JIT_PROBE_FAULTED || r3 == DN_JIT_PROBE_FAULTED)
               ? DN_JIT_PROBE_FAULTED : DN_JIT_PROBE_MAP_REFUSED;
}

/// Nivelul sigur: aceleasi trei strategii ca proba completa, dar ne oprim
/// BEFORE the jump. A successful mmap/mprotect does NOT guarantee execution on TXM
/// (only the debugger does authorization), but it cannot kill the process either.
dn_jit_probe_result_t dn_jit_probe_classic_map_only(void) {
    // Strategia 1: RWX anonim simplu.
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) { munmap(p, 4096); return DN_JIT_PROBE_MAPPED_UNTESTED; }
#ifdef MAP_JIT
    // Strategia 2: MAP_JIT (cu get-task-allow maparea reuseste; executia pe
    // TXM is a wholly different discussion — that is why we do not jump).
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p != MAP_FAILED) { munmap(p, 4096); return DN_JIT_PROBE_MAPPED_UNTESTED; }
#endif
    // Strategia 3: RW, apoi mprotect -> RX.
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) {
        int rc = mprotect(p, 4096, PROT_READ | PROT_EXEC);
        munmap(p, 4096);
        if (rc == 0) return DN_JIT_PROBE_MAPPED_UNTESTED;
    }
    return DN_JIT_PROBE_MAP_REFUSED;
}

const char *dn_jit_probe_result_text(dn_jit_probe_result_t result) {
    switch (result) {
        case DN_JIT_PROBE_OK:          return "OK: wrote and executed own code";
        case DN_JIT_PROBE_MAP_REFUSED: return "REFUSED: the executable mapping is rejected";
        case DN_JIT_PROBE_FAULTED:     return "REFUSED: mapped, but execution faults (typical TXM without authorization)";
        case DN_JIT_PROBE_WRONG_VALUE: return "RAN, but returned a wrong value — untrustworthy";
        case DN_JIT_PROBE_MAPPED_UNTESTED: return "MAPPED: executable page obtained (the jump is not auto-tested)";
        default:                       return "untested";
    }
}

// ---------------------------------------------------------------------------
// Apeluri brk protejate. Refolosesc garda probei (sigaction + sigsetjmp):
// - with the universal script attached, the brk is consumed by debugserver as
//   Mach exception and never becomes a signal in the process -> normal return;
// - without the script (bare debugserver only), the unhandled exception converts
//   in semnal -> garda sare inapoi cu siglongjmp -> intoarcem false in loc
//   sa murim din SIGTRAP.
// ---------------------------------------------------------------------------

bool dn_jit26_prepare_region_guarded(void *address, size_t length, void **out_rx) {
    if (out_rx) *out_rx = NULL;
    struct sigaction saved[DN_PROBE_NSIGNALS];
    if (!dn_install_handlers(saved)) return false;
    void *volatile rx = NULL;
    volatile bool ok = false;
    g_dn_probe_armed = 1;
    if (sigsetjmp(g_dn_probe_jmp, 1) == 0) {
        rx = dn_jit26_prepare_region(address, length);
        g_dn_probe_armed = 0;
        ok = (rx != NULL);
    } else {
        ok = false;   // the brk came back as a signal: no script runs beyond
    }
    dn_restore_handlers(saved);
    if (ok && out_rx) *out_rx = rx;
    return ok;
}

bool dn_jit26_detach_guarded(void) {
    struct sigaction saved[DN_PROBE_NSIGNALS];
    if (!dn_install_handlers(saved)) return false;
    volatile bool ok = false;
    g_dn_probe_armed = 1;
    if (sigsetjmp(g_dn_probe_jmp, 1) == 0) {
        dn_jit26_detach();
        g_dn_probe_armed = 0;
        ok = true;
    }
    dn_restore_handlers(saved);
    return ok;
}
