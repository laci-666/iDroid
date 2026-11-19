import Foundation
import os

/// Accelerator posibile, in ordinea preferintei. Vezi docs/01 §3 (matricea).
enum Accelerator: String, CaseIterable {
    case hvf        = "HVF (virtualizare hardware EL2) — necesita entitlement Apple"
    case tcgJIT     = "TCG with JIT (iOS 26/27 TXM protocol, built-in StikJIT helper)"
    case tcgTCTI    = "TCG TCTI interpreter (fallback engine, no runtime-generated code)"
    // User decision: everything on-device -> there is no remote mode. (docs/05)
}

/// Reads the REAL environment at runtime. Nothing is assumed:
/// - the memory limit differs by entitlements/signing profile;
/// - the hypervisor could become available via a policy change -> we actually probe it;
/// - prezenta TXM decide protocolul JIT.
struct CapabilityProbe {
    private(set) var deviceModel: String = "—"
    private(set) var physicalMemoryBytes: UInt64 = 0
    private(set) var availableMemoryBytes: UInt64 = 0
    private(set) var hypervisorAvailable: Bool = false
    private(set) var txmPresent: Bool = false
    private(set) var isDebugged: Bool = false
    /// Debugger attached RIGHT NOW (P_TRACED) — separate from CS_DEBUGGED, which is
    /// sticky and stays after detach. The pair decides the "it attached" verdict
    /// and left" in JITCoordinator (docs/08).
    private(set) var isTraced: Bool = false
    /// The signature carries get-task-allow — the JIT attach precondition.
    private(set) var hasGetTaskAllow: Bool = false
    private(set) var thermal: ProcessInfo.ThermalState = .nominal
    /// The SAFE level of the classic probe: executable mappings only, no execution jump
    /// pagina. Automat la refresh, o singura data pe sesiune, risc zero.
    private(set) var classicJITMapText: String = "netestat"
    /// Nivelul COMPLET, cu salt efectiv in cod propriu: DOAR manual, din butonul
    /// dedicated. On TXM, unauthorized execution can kill the process directly
    /// (SIGKILL codesigning), bypassing the signal guard — learned on device
    /// real, so never automatically.
    private(set) var classicJITProbeText: String = "netestat (buton mai jos)"

    mutating func refresh() {
        deviceModel = Self.machineIdentifier()
        physicalMemoryBytes = ProcessInfo.processInfo.physicalMemory
        // os_proc_available_memory(): how much memory the process may still allocate before jetsam.
        // THAT is the real budget, not physical RAM. The project signs by default with
        // increased-memory-limit (DroidNative-Paid.entitlements, contul curent il
        // permite) -> valoarea e de ordinul ~6 GB pe iPhone 16 Pro Max. Daca
        // semnarea pierde entitlement-ul, valoarea cade singura la ~3.3 GB si
        // bugetul guest se adapteaza din formula de mai jos.
        availableMemoryBytes = UInt64(os_proc_available_memory())
        hypervisorAvailable = dn_probe_hypervisor()   // tries hv_vm_create(); fails without entitlement
        txmPresent = dn_txm_present()
        isDebugged = dn_is_debugged()                // csops(pid, CS_OPS_STATUS) & CS_DEBUGGED
        isTraced = dn_is_traced()                    // P_TRACED: debugger viu in clipa asta
        hasGetTaskAllow = dn_has_get_task_allow()    // csops & CS_GET_TASK_ALLOW
        thermal = ProcessInfo.processInfo.thermalState
        if classicJITMapText == "netestat" {
            // Mappings only, zero execution jumps: safe on any device,
            // inclusiv TXM. Saltul efectiv s-a mutat pe butonul manual.
            classicJITMapText = String(cString: dn_jit_probe_result_text(dn_jit_probe_classic_map_only()))
        }
    }

    /// Proba completa, cu executie reala a unei sentinele sub garda de semnal
    /// (docs/05, L1). Apelata DOAR din butonul dedicat din Capabilitati.
    /// Sub debugger sarim: lldb ar intercepta defectul controlat si ar opri
    /// the process in the console; and anyway the value under a debugger is not the real one.
    mutating func runFullClassicProbe() {
        if dn_is_debugged() {
            classicJITProbeText = "skipped under debugger (Xcode) — launch from the icon and retry"
            return
        }
        let result = dn_jit_probe_classic()
        classicJITProbeText = String(cString: dn_jit_probe_result_text(result))
    }

    /// Cascada de selectie a acceleratorului.
    var chosenAccelerator: Accelerator {
        if hypervisorAvailable { return .hvf }
        // JIT e posibil doar semnat cu get-task-allow (sideload/dev). Semnalul practic:
        // the process can be attached by a debugger. The JIT coordinator validates effectively at boot;
        // aici returnam intentia, nu garantia.
        return .tcgJIT   // cade pe .tcgTCTI in VMController daca achizitia JIT esueaza/e refuzata
    }

    /// Guest RAM: everything jetsam allows, minus a fixed host reserve
    /// (TCG arena/buffer up to ~512 MB, QEMU + devices, UI, margin),
    /// capped at 4 GB. With the increased-memory-limit entitlement (default
    /// acum, DroidNative-Paid.entitlements), os_proc_available_memory()
    /// reports ~6 GB on this iPhone -> the guest gets 4 GB; without it
    /// (~3.3 GB available) the formula gives ~2 GB, as before.
    var plannedGuestMemoryBytes: UInt64 {
        let cap: UInt64 = 4 * 1024 * 1024 * 1024
        let reserve: UInt64 = 1280 * 1024 * 1024
        let budget = availableMemoryBytes > reserve
            ? availableMemoryBytes - reserve
            : availableMemoryBytes / 2
        let floor: UInt64 = 1536 * 1024 * 1024   // sub ~1.5 GB Android modern nu e utilizabil
        return max(floor, min(cap, budget))
    }

    // MARK: - Formatting for the UI
    var physicalMemoryText: String { Self.format(physicalMemoryBytes) }
    var availableMemoryText: String { Self.format(availableMemoryBytes) }
    var plannedGuestMemoryText: String { Self.format(plannedGuestMemoryBytes) }
    var hypervisorText: String { hypervisorAvailable ? "AVAILABLE (entitlement active)" : "blocked (missing com.apple.private.hypervisor)" }
    var thermalText: String {
        switch thermal {
        case .nominal: return "nominal"
        case .fair: return "redusa"
        case .serious: return "serioasa (scalare rezolutie/FPS)"
        case .critical: return "critica (pauza recomandata)"
        @unknown default: return "necunoscut"
        }
    }

    private static func format(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
    }

    private static func machineIdentifier() -> String {
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        var buf = [CChar](repeating: 0, count: size)
        sysctlbyname("hw.machine", &buf, &size, nil, 0)
        return String(cString: buf)
    }
}
