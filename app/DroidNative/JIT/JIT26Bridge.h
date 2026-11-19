#ifndef JIT26Bridge_h
#define JIT26Bridge_h

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// C annex of the iOS 26/27 JIT protocol (TXM/SPTM) — see JIT26Bridge.m and docs/01 §3.
/// Protocol source: StikDebug/StikJIT INTEGRATION.md ("universal script").

/// true if the device has TXM (Trusted Execution Monitor, A15+ silicon).
bool dn_txm_present(void);

/// true if the process has the CS_DEBUGGED flag. Careful: the flag is STICKY —
/// it stays set after the debugger leaves, including across suspend.
/// NEVER call dn_jit26_* based on it alone: an unhandled `brk`
/// omoara procesul. Combina-l cu dn_is_traced().
bool dn_is_debugged(void);

/// true if a debugger is attached RIGHT NOW (P_TRACED via sysctl).
/// The pair CS_DEBUGGED + !P_TRACED = "attached and left without
/// authorizing me" — the verdict after which no brk ever runs.
bool dn_is_traced(void);

/// JIT authorization probe: tries a small anonymous PROT_READ|PROT_EXEC mmap
/// (exactly the operation the UTM fork starts its TCG buffer allocation with on
/// iOS 26+ in tcg/region.c) si intoarce 0 la succes sau errno altfel (EPERM =
/// TXM refuses: debugger detached or without TXM rights). Executes nothing and
/// performs no brk — safe in any debugger state. We use it as
/// the gate RIGHT before starting QEMU: without it, qemu_init dies with
/// „allocate … bytes for jit buffer: Operation not permitted” + exit()
/// (the process disappears instantly, no crash report).
int dn_jit_rx_mmap_probe(void);

/// As above, but with a given size (bytes). Observed on device
/// (21.10.2026): the one-page probe passed, but QEMU's 384 MB mmap still
/// got EPERM in the same thread, debugger attached — TXM authorization
/// pare sa aiba o cota pe marimea maparii. De aceea poarta baleiaza
/// decreasing sizes and starts QEMU with the largest accepted tb-size.
int dn_jit_rx_mmap_probe_size(size_t bytes);

/// true if the process signature has get-task-allow (CS_GET_TASK_ALLOW from
/// csops). The precondition for StikDebug/StikJIT to attach us; its absence is
/// diagnosed BEFORE any attach attempt (docs/08).
bool dn_has_get_task_allow(void);

/// Tries hv_vm_create()/hv_vm_destroy(). On stock iOS it fails without
/// com.apple.private.hypervisor; if one day the entitlement becomes available,
/// the app discovers it itself and switches to -accel hvf.
bool dn_probe_hypervisor(void);

/// Asks the debugger to authorize/prepare an executable region (brk #0xf00d, x16=1).
/// address=NULL => debugger-ul aloca regiunea RX. Intoarce adresa RX sau NULL la esec.
/// Precondition: dn_is_debugged() == true AND the universal script is attached in StikDebug/StikJIT.
void *dn_jit26_prepare_region(void *address, size_t length);

/// Semnaleaza debugger-ului ca am terminat de autorizat regiunile (brk #0xf00d, x16=0).
/// After detach, NO new regions can be authorized without a new attach.
void dn_jit26_detach(void);

/// Creeaza un alias scriibil (RW) al acelorasi pagini fizice ca regiunea RX,
/// via vm_remap + vm_protect (W^X dual-mapping). Code is written via RW, executed via RX.
/// Intoarce adresa RW sau NULL la esec.
void *dn_jit_make_writable_alias(void *rx_address, size_t length);

/// Signal-guarded variants of the brk calls — USE ONLY THESE.
/// CS_DEBUGGED proves A debugger exists, not that the universal script
/// runs beyond it. Without the script, a bare brk comes back into the process as SIGTRAP and
/// omoara (documentat StikJIT: "a brk without the script attached will crash").
/// Variantele astea convertesc dezastrul in `false`; apelantul coboara pe
/// interpretor in loc sa moara.
bool dn_jit26_prepare_region_guarded(void *address, size_t length, void **out_rx);
bool dn_jit26_detach_guarded(void);

// ---------------------------------------------------------------------------
// Proba empirica de executabilitate (tipar preluat din S5LBox,
// app/Sources/VMJitProbe.c — MIT, (c) j0shua-SYSON; vezi docs/05, L1).
// Philosophy: flags are NOT the truth. We write a trivial payload
// (movz x0,#0x5a5a; ret) and verify the CPU really executes it, under the signal
// guard (sigaction + sigsetjmp). Results are distinct so the UI can
// explain correctly WHY it does not work, not just that it does not.
// ---------------------------------------------------------------------------

typedef enum {
    DN_JIT_PROBE_UNTESTED = 0,
    DN_JIT_PROBE_OK,            // a scris si a executat cod propriu
    DN_JIT_PROBE_MAP_REFUSED,   // the mapping was refused
    DN_JIT_PROBE_FAULTED,       // mapped, but the code jump faulted (the TXM-no-authorization case)
    DN_JIT_PROBE_WRONG_VALUE,   // a rulat, dar a intors altceva — nu ai incredere
    DN_JIT_PROBE_MAPPED_UNTESTED, // the mapping succeeded, but the execution jump was NOT tried
} dn_jit_probe_result_t;

/// Nivelul SIGUR al probei clasice: doar mapari (RWX / MAP_JIT / mprotect),
/// NO page jump. On TXM, executing a debugger-unauthorized page
/// can kill the process directly (SIGKILL codesigning), bypassing any signal
/// handler — so this level is the only one allowed automatically/on refresh.
dn_jit_probe_result_t dn_jit_probe_classic_map_only(void);

/// The COMPLETE "classic" probe (no debugger): tries RWX, MAP_JIT and
/// mprotect RW->RX, cu salt efectiv in cod sub garda de semnal.
/// CAUTION: on TXM it can kill the process before the guard catches anything.
/// A se apela DOAR manual (buton dedicat in UI), niciodata automat.
dn_jit_probe_result_t dn_jit_probe_classic(void);

/// TXM arena self-test: writes the payload via the RW alias, invalidates the
/// I-cache, executes it via the RX address. Called BEFORE QEMU start:
/// un esec aici inseamna coborare pe TCTI, nu crash la mijlocul boot-ului.
dn_jit_probe_result_t dn_jit_arena_selftest(void *rx_address, void *rw_alias);

/// Short text, displayable in the UI, for a probe result.
const char *dn_jit_probe_result_text(dn_jit_probe_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* JIT26Bridge_h */
