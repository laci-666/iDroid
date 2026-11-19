import Foundation
import Security
import UIKit

/// A TXM-authorized executable memory arena: two views over the same
/// physical pages — `rx` for execution, `rw` for writing generated code.
struct JITArena {
    let rx: UnsafeMutableRawPointer
    let rw: UnsafeMutableRawPointer
    let size: Int
}

enum JITError: Error, LocalizedError {
    case notDebugged
    case attachedAndLeft
    case prepareFailed
    case aliasFailed
    case selfTestFailed(String)
    case wrongDebugger
    var errorDescription: String? {
        switch self {
        case .notDebugged: return "No debugger attached: enable JIT (built-in method or StikDebug, with LocalDevVPN on)."
        case .attachedAndLeft: return "A debugger attached and left without authorizing anything (CS_DEBUGGED stayed set, but nobody is attached anymore). I will not execute a blind brk — restart the app and try again."
        case .prepareFailed: return "The debugger did not authorize the executable region — most likely the universal script is not active for DroidNative (in StikDebug check the universal.js assignment; with the built-in method, redo the preparation)."
        case .aliasFailed: return "Nu am putut crea aliasul scriibil (vm_remap) al arenei JIT."
        case .selfTestFailed(let detail): return "The arena looks authorized, but the execution self-test failed: \(detail). Falling back to TCTI instead of dying in QEMU."
        case .wrongDebugger: return "The attached debugger is not a compatible one (probably Xcode/lldb): the brk protocol would stop it with EXC_BREAKPOINT. For JIT: launch from the icon, no Xcode."
        }
    }
}

/// How the scripted debugger attach is obtained. iDroid 0.4 removed
/// the obsolete acquisition picker: the built-in helper is the only
/// supported production method. The legacy enum cases remain only so
/// old diagnostics/log text remains decodable; `selected` and
/// `resolvedMethod()` deliberately ignore stale UserDefaults and
/// always return `.builtIn`.
enum JITAcquisitionMethod: String, CaseIterable, Identifiable {
    case automatic
    case builtIn
    case stikDebug
    case waitForDebugger

    var id: String { rawValue }

    var label: String {
        switch self {
        case .automatic: return "Automatic"
        case .builtIn: return "Built-in (own helper)"
        case .stikDebug: return "StikDebug (separate app)"
        case .waitForDebugger: return "Wait for debugger (manual)"
        }
    }

    static let userDefaultsKey = "jitAcquisitionMethod"

    static var selected: JITAcquisitionMethod { .builtIn }
}

/// Executable memory acquisition for TCG, per the iOS 26/27 protocol.
/// Strict order (docs/01 §3): wait CS_DEBUGGED -> prepare -> alias -> detach.
/// The arena is allocated ONCE, large (384 MB default), and the TCG allocator in QEMU
/// sub-allocates from it. There is no "ask for another region later" without re-attach.
@MainActor
final class JITCoordinator {

    /// The built-in helper's final response for the current `enable` request.
    /// The request completes AFTER detach, so the result arrives asynchronously,
    /// when the coordinator is already further along; we keep it for the log.
    private(set) var lastBuiltInEnableResponse: DNBuiltInJITResponse?

    /// Readable reason for the last failed wait (e.g. the error reported by
    /// the built-in helper). VMController puts it in the console, so failures
    /// DDI/VPN/pairing no longer look like a mute 120 s wait.
    private(set) var lastWaitDiagnostic: String?

    /// The method used for the last attach attempt (resolved from `.automatic`).
    private(set) var lastResolvedMethod: JITAcquisitionMethod?

    /// The built-in helper is fixed; there is no supported fallback
    /// to an external JIT app.
    private func resolvedMethod() -> JITAcquisitionMethod { .builtIn }

    /// Starts the attach with the selected method and returns a log line for
    /// the console. Does not block: the actual attach is confirmed via
    /// `waitForDebugger()`, exactly like the device-validated StikDebug flow.
    func requestAttach(txmPresent: Bool, scriptKind: DNJITScriptKind = .universal) -> String {
        let method = resolvedMethod()
        lastResolvedMethod = method
        switch method {
        case .builtIn:
            return startBuiltInEnable(scriptKind: scriptKind)
        case .stikDebug:
            requestStikDebugAttach(txmPresent: txmPresent, scriptKind: scriptKind)
            return "JIT method: StikDebug (request sent; come back here after activation)."
        case .waitForDebugger, .automatic:
            return "JIT method: waiting for a manually attached debugger (e.g. from the StikDebug app list)."
        }
    }

    /// Asks our helper to attach this process with the universal script.
    /// The cheap checks happen HERE, before launching the extension:
    /// get-task-allow (altfel debugserver refuza oricum) si pairing prezent.
    private func startBuiltInEnable(scriptKind: DNJITScriptKind) -> String {
        guard dn_has_get_task_allow() else {
            return "Built-in JIT method cancelled: the process does NOT have get-task-allow (signing without debugging rights). Stay on the interpreter or sign development with an Xcode Personal Team."
        }
        guard let pairing = DNPairingFileStore.pairingData() else {
            return "Built-in JIT method cancelled: the pairing file is missing. Import it from Capabilities → \"Import pairing file…\", then tap \"Prepare built-in JIT (DDI)\"."
        }
        lastBuiltInEnableResponse = nil
        let force = false // obsolete debug switch removed in iDroid 0.4
        let request = DNBuiltInJITRequest(
            operation: .enable,
            targetPID: getpid(),
            pairingData: pairing,
            forceUniversalScript: force,
            scriptKind: scriptKind
        )
        DNBuiltInJITHost.send(request, timeout: 240) { [weak self] response in
            self?.lastBuiltInEnableResponse = response
        }
        return "JIT method: built-in, script \(scriptKind.fileName) (helper started, waiting for attach)."
    }

    /// The "Prepare built-in JIT" button: device preparation only
    /// (DDI), so the slow first start does not land during boot.
    func prepareBuiltIn() async -> DNBuiltInJITResponse {
        guard dn_has_get_task_allow() else {
            return .failure("The process has no get-task-allow; JIT cannot be attached under this signing.")
        }
        guard let pairing = DNPairingFileStore.pairingData() else {
            return .failure("Import the pairing file first.")
        }
        let request = DNBuiltInJITRequest(operation: .prepare, targetPID: getpid(), pairingData: pairing)
        return await sendToHelper(request, timeout: 300)
    }

    /// Resetarea cache-ului DDI din helper (calea de recuperare documentata).
    func resetBuiltInDDI() async -> DNBuiltInJITResponse {
        let request = DNBuiltInJITRequest(operation: .resetDDI, targetPID: 0, pairingData: nil)
        return await sendToHelper(request, timeout: 60)
    }

    private func sendToHelper(_ request: DNBuiltInJITRequest, timeout: TimeInterval) async -> DNBuiltInJITResponse {
        await withCheckedContinuation { continuation in
            DNBuiltInJITHost.send(request, timeout: timeout) { response in
                continuation.resume(returning: response)
            }
        }
    }

    /// Asks StikDebug to attach the debugger + universal script to this process.
    /// Formatul URL-ului e contractul public StikDebug (bundle id + PID curent +
    /// script name on TXM devices). The success of opening the URL
    /// NU inseamna JIT activ — confirmarea e aparitia lui CS_DEBUGGED.
    func requestStikDebugAttach(txmPresent: Bool, scriptKind: DNJITScriptKind = .universal) {
        var components = URLComponents()
        components.scheme = "stikdebug"
        components.host = "enable-jit"
        var items = [
            URLQueryItem(name: "bundle-id", value: Bundle.main.bundleIdentifier ?? ""),
            URLQueryItem(name: "pid", value: String(getpid())),
        ]
        if txmPresent {
            // The exact FILE name from the StikJIT docs (INTEGRATION.md):
            // "universal.js" / "legacy.js". Without the extension, StikDebug cannot find
            // the script, attaches the bare debugserver, and the first brk kills
            // the process. Legacy is for the QEMU TCG buffer (brk 0x69, docs/09).
            items.append(URLQueryItem(name: "script-name", value: scriptKind.fileName))
        }
        components.queryItems = items
        if let url = components.url {
            UIApplication.shared.open(url)
        }
    }

    /// When the brk protocol is allowed (two paths, both device-verified):
    /// 1. we observed the transition to "attached" in a flow we started
    ///    (StikDebug URL or the built-in helper), or
    /// 2. the process is launched from the icon (parent = launchd, ppid 1) and has a
    ///    debugger attached AFTER launch — typically manual activation.
    /// The Xcode/lldb case stays blocked: launch THROUGH debugserver (ppid != 1) with
    /// a pre-attached debugger; there a brk would be stopped as EXC_BREAKPOINT in
    /// the console (met on device, 20.10.2026). The initial lock
    /// (path 1 only) refused on device a legitimate manual attach done
    /// before Start — that is why path 2 exists. Without unlock, no brk.
    ///
    /// From docs/08: the attach signal is the PAIR CS_DEBUGGED + P_TRACED.
    /// CS_DEBUGGED alone is sticky (stays after detach and across suspend),
    /// so it can mean "someone was here and left". That verdict is detected
    /// separat (`debuggerAttachedAndLeft`) si nu deblocheaza nimic.
    private var brkProtocolUnlocked = false

    /// True after a `waitForDebugger` ending with the verdict "attached and
    /// left without authorizing" — for the correct console message.
    private(set) var debuggerAttachedAndLeft = false

    /// The brk protocol safety lock, visible to
    /// VMController: QEMU (fork UTM) auto-authorizes its TCG buffer in
    /// `qemu_init` if it sees an attached debugger — start the backend ONLY with
    /// the lock open (compatible scripted debugger, not Xcode/lldb).
    var isBrkProtocolUnlocked: Bool { brkProtocolUnlocked }

    /// P_TRACED chiar acum (diagnostic de poarta in VMController).
    var isTracedNow: Bool { dn_is_traced() }

    /// Sonda RX-mmap (vezi `dn_jit_rx_mmap_probe`): 0 = TXM accepta chiar
    /// now an anonymous executable mapping, i.e. exactly what
    /// qemu_init needs for the TCG buffer; otherwise errno (1 = EPERM).
    func rxMmapProbeErrno() -> Int32 { dn_jit_rx_mmap_probe() }

    /// Baleiaj descrescator de marimi: intoarce cel mai mare candidat (MB)
    /// TXM accepts right now + the first errno met (for
    /// diagnostics). On device (21.10.2026) one page passed, but
    /// 384 MB got EPERM with the same debugger attached — a size quota;
    /// QEMU can start degraded with the tb-size found here.
    func rxMmapLargestOKMegabytes(candidates: [Int]) -> (largestOK: Int, firstErrno: Int32) {
        var firstErrno: Int32 = 0
        for mb in Array(Set(candidates)).sorted(by: >) {
            let err = dn_jit_rx_mmap_probe_size(mb * 1024 * 1024)
            if err == 0 { return (mb, firstErrno) }
            if firstErrno == 0 { firstErrno = err }
        }
        return (0, firstErrno)
    }

    /// Waits for debugger detach (P_TRACED -> false). In the TCG
    /// QEMU flow, the legacy script detaches by itself after authorizing
    /// the region in `qemu_init`; we await it as the verdict that authorization
    /// ended and that the helper response can be read.
    func waitForDetach(timeout: Duration = .seconds(180)) async -> Bool {
        let deadline = ContinuousClock.now + timeout
        while ContinuousClock.now < deadline {
            if !dn_is_traced() { return true }
            try? await Task.sleep(for: .milliseconds(250))
        }
        return !dn_is_traced()
    }

    /// Waits (with timeout) for the debugger attach; unlocks the protocol
    /// brk conform regulilor de mai sus.
    func waitForDebugger(timeout: Duration = .seconds(120)) async -> Bool {
        debuggerAttachedAndLeft = false
        lastWaitDiagnostic = nil
        let flaggedAtEntry = dn_is_debugged()
        let tracedAtEntry = dn_is_traced()
        // Copil al launchd = lansat din iconita; copil al debugserver = Xcode Run.
        let launchedFromIcon = getppid() == 1
        var sawTraced = tracedAtEntry
        var flaggedWithoutDebuggerSince: ContinuousClock.Instant?

        func unlockIfAllowed(flagged: Bool, traced: Bool) -> Bool {
            guard flagged && traced else { return false }
            if !flaggedAtEntry || !tracedAtEntry || launchedFromIcon {
                brkProtocolUnlocked = true
            }
            return true
        }

        let deadline = ContinuousClock.now + timeout
        while ContinuousClock.now < deadline {
            let flagged = dn_is_debugged()
            let traced = dn_is_traced()
            // Built-in method: if the helper ENDED the request with an error,
            // the attach is not coming on this request — exit immediately with its
            // reason, not after 120 s of mute waiting (the real case "stuck at
            // Astept activarea JIT”, Log 14/15: utilizatorul nu vedea motivul
            // DDI/VPN/pairing raportat de StikJIT in helper).
            if lastResolvedMethod == .builtIn, !traced,
               let response = lastBuiltInEnableResponse, !response.ok {
                lastWaitDiagnostic = "The built-in helper finished the request with an error: \(response.message)"
                return false
            }
            if traced {
                sawTraced = true
                flaggedWithoutDebuggerSince = nil
            }
            if unlockIfAllowed(flagged: flagged, traced: traced) {
                return true
            }
            // Attached-and-left: the flag APPEARED in this window (or we saw
            // the live debugger) and now nobody is there. After 3 s the verdict is
            // stabil; nu lasam utilizatorul sa astepte timeout-ul intreg.
            let appearedDuringWait = !flaggedAtEntry || (sawTraced && !tracedAtEntry)
            if flagged && !traced && appearedDuringWait {
                if let since = flaggedWithoutDebuggerSince {
                    if ContinuousClock.now - since >= .seconds(3) {
                        debuggerAttachedAndLeft = true
                        return false
                    }
                } else {
                    flaggedWithoutDebuggerSince = ContinuousClock.now
                }
            } else if !flagged {
                flaggedWithoutDebuggerSince = nil
            }
            try? await Task.sleep(for: .milliseconds(250))
        }
        return unlockIfAllowed(flagged: dn_is_debugged(), traced: dn_is_traced())
    }

    /// Aloca + autorizeaza arena executabila, apoi se detaseaza.
    /// - pe TXM: brk #0xf00d (adresa NULL => debugger-ul aloca regiunea RX;
    ///   the ecosystem-verified path — the region mapped by the process itself can
    ///   looked authorized without being so).
    /// - no TXM (old devices): attach alone suffices (classic mmap arena).
    // MARK: - Breadcrumb de self-test (supravietuieste unei omorari TXM)

    /// Marker file written to disk BEFORE the verification jump into the arena and
    /// deleted right after success. The write is a synchronous syscall, so the marker
    /// supravietuieste chiar si unei omorari instantanee a procesului —
    /// UserDefaults n-ar garanta asta (scriere tamponata).
    private static var selfTestBreadcrumbURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base.appendingPathComponent("jit-selftest-pending.marker")
    }

    private static func setSelfTestBreadcrumb() {
        FileManager.default.createFile(atPath: selfTestBreadcrumbURL.path, contents: Data())
    }

    private static func clearSelfTestBreadcrumb() {
        try? FileManager.default.removeItem(at: selfTestBreadcrumbURL)
    }

    /// To call at app launch: true if the previous session died
    /// probabil in timpul self-test-ului arenei (autorizare esuata tacit).
    static func consumeSelfTestBreadcrumb() -> Bool {
        let url = selfTestBreadcrumbURL
        guard FileManager.default.fileExists(atPath: url.path) else { return false }
        try? FileManager.default.removeItem(at: url)
        return true
    }

    ///   manage memory itself, so we return a "classic" mmap arena.
    func acquireArena(sizeBytes: Int) throws -> JITArena {
        guard dn_is_debugged() else { throw JITError.notDebugged }

        if dn_txm_present() {
            // No brk toward a debugger that does not speak the universal protocol.
            guard brkProtocolUnlocked else { throw JITError.wrongDebugger }
            // And no blind brk: the flag alone can be the remnant of a
            // debugger gone (docs/08). Someone alive beyond is needed, now.
            guard dn_is_traced() else { throw JITError.attachedAndLeft }
            // DOAR variantele protejate: CS_DEBUGGED nu garanteaza scriptul.
            var rxRaw: UnsafeMutableRawPointer?
            guard dn_jit26_prepare_region_guarded(nil, sizeBytes, &rxRaw), let rx = rxRaw else {
                throw JITError.prepareFailed
            }
            guard let rw = dn_jit_make_writable_alias(rx, sizeBytes) else {
                throw JITError.aliasFailed
            }
            _ = dn_jit26_detach_guarded()
            // ADEVARUL DE TEREN (lectie S5LBox, docs/05 L1): API-urile de mai sus
            // can formally "succeed" without the page being executable under TXM.
            // We actually execute a sentinel through the arena; only then declare it good.
            // CAUTION (confirmed on a real device, 20.10.2026): if authorization
            // failed silently, TXM returns no catchable verdict — it kills the process
            // instantly, before the signal guard catches anything. That is why we put
            // the breadcrumb on disk before the jump: if we never come back,
            // the next launch knows exactly what happened and tells the user.
            Self.setSelfTestBreadcrumb()
            let verdict = dn_jit_arena_selftest(rx, rw)
            Self.clearSelfTestBreadcrumb()
            guard verdict == DN_JIT_PROBE_OK else {
                throw JITError.selfTestFailed(String(cString: dn_jit_probe_result_text(verdict)))
            }
            return JITArena(rx: rx, rw: rw, size: sizeBytes)
        } else {
            // Non-TXM: debugger attached => the process can use classic MAP_JIT/mprotect.
            // QEMU receives the pointers through the same JITArena contract.
            let rx = mmap(nil, sizeBytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0)
            guard let rx, rx != UnsafeMutableRawPointer(bitPattern: -1) else { throw JITError.prepareFailed } // (void *)-1 = MAP_FAILED
            return JITArena(rx: rx, rw: rx, size: sizeBytes) // aceeasi vedere (RWX permis)
        }
    }
}

/// The user's signing identity (developer .p12 certificate),
/// kept EXCLUSIVELY in the app's Keychain, for the "Native Mode" in
/// docs/13 (Route 3: ELF→Mach-O signed on device, henge/LiveContainer model).
///
/// Hard rules (docs/14): the private key never reaches the sources, the
/// app bundle or the project zip — the user imports it
/// alone, from Files, on the phone; we store the .p12 blob (with its password,
/// as JSON) in a protected generic-password Keychain item
/// AfterFirstUnlockThisDeviceOnly and re-import it in memory when needed.
/// This identity does NOT sign iDroid itself (Xcode does that on the Mac from
/// Keychain-ul Mac-ului) — ea va semna bibliotecile guest traduse, ca dyld
/// accept them without jailbreak/JIT.
enum DevSigningIdentity {
    private static let keychainService = "ro.droidnative.devsigning"
    private static let keychainAccount = "pkcs12"
    private static let summaryDefaultsKey = "dnDevCertSummary"

    /// The public summary (certificate name + import date + self-test), no secrets.
    static var storedSummary: String? {
        UserDefaults.standard.string(forKey: summaryDefaultsKey)
    }

    /// Imports, validates (with a real signature self-test) and stores.
    /// Returns a message for the UI. Failures report the REAL OSStatus code
    /// of the Security framework (0.2.11): the previous generic message
    /// covered wrong file / wrong password / algorithms not accepted by
    /// iOS / identity already in Keychain under the same text ("wrong
    /// password") and made remote diagnosis impossible.
    static func importAndStore(p12: Data, password: String) -> String {
        let message = doImport(p12: p12, password: password)
        UserDefaults.standard.set(message, forKey: "dnDevCertLastMessage")
        return message
    }

    static var lastImportMessage: String? {
        UserDefaults.standard.string(forKey: "dnDevCertLastMessage")
    }

    private static func doImport(p12: Data, password: String) -> String {
        if p12.count >= 2, p12[0] == 0x50, p12[1] == 0x4B {
            return "You picked the archive (.zip), not the certificate. In Files, tap the archive to unzip it, then choose the file ending in .p12."
        }
        guard p12.first == 0x30 else {
            let hex = p12.prefix(4).map { String(format: "%02x", $0) }.joined()
            return "The chosen file does not look like PKCS#12 (first bytes: \(hex), size \(p12.count)). Choose the .p12 file from the unzipped archive."
        }
        var attempts: [(label: String, value: String)] = [("entered password", password)]
        if !password.isEmpty {
            attempts.append(("empty password", ""))
        }
        var lastStatus: OSStatus = errSecSuccess
        for attempt in attempts {
            switch tryImport(p12: p12, password: attempt.value) {
            case .success(let identity, let subject):
                let testResult = selfTestSignature(identity: identity)
                let payload: [String: String] = [
                    "p12": p12.base64EncodedString(),
                    "pass": attempt.value,
                ]
                guard let blob = try? JSONSerialization.data(withJSONObject: payload),
                      storeInKeychain(blob: blob) else {
                    return "The certificate is valid (\(subject)), but writing to the Keychain failed."
                }
                let when = DateFormatter.localizedString(from: Date(), dateStyle: .medium, timeStyle: .short)
                let note = attempt.value == password ? "" : " (acceptat cu \(attempt.label))"
                let summary = "\(subject) · imported \(when) · signature self-test: \(testResult)"
                UserDefaults.standard.set(summary, forKey: summaryDefaultsKey)
                return "Certificate imported into Keychain\(note). \(summary)"
            case .failure(let status):
                lastStatus = status
            }
        }
        let hint: String
        if lastStatus == errSecAuthFailed {
            hint = "If the password is definitely correct (verified with openssl on the Mac), the file algorithms are not accepted by Security/iOS — convert the .p12 on the Mac with the docs/14 commands (classic re-export) and import the converted file."
        } else if lastStatus == errSecDecode {
            hint = "The file could not be decoded as PKCS#12 — probably not the .p12 file, or it was corrupted when copied."
        } else {
            hint = "Send me this code (it also appears in the diagnostic export)."
        }
        return "Import failed: \(statusText(lastStatus)) [OSStatus \(lastStatus)], file of \(p12.count) bytes. \(hint)"
    }

    static func deleteStored() {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: keychainService,
            kSecAttrAccount as String: keychainAccount,
        ]
        SecItemDelete(query as CFDictionary)
        UserDefaults.standard.removeObject(forKey: summaryDefaultsKey)
    }

    /// Re-imports the identity when needed (Route 3 will use it for Mach-O signing).
    static func loadIdentity() -> SecIdentity? {
        guard let (p12, password) = loadFromKeychain() else { return nil }
        if case .success(let identity, _) = tryImport(p12: p12, password: password) {
            return identity
        }
        return nil
    }

    // MARK: - Intern

    private enum ImportTry {
        case success(SecIdentity, String)
        case failure(OSStatus)
    }

    /// SecPKCS12Import adds the identity to the Keychain on every call;
    /// a re-import can return errSecDuplicateItem — then
    /// we recover the existing identity (it is the same) instead of failing.
    private static func tryImport(p12: Data, password: String) -> ImportTry {
        var items: CFArray?
        let options = [kSecImportExportPassphrase as String: password] as CFDictionary
        let status = SecPKCS12Import(p12 as CFData, options, &items)
        if status == errSecDuplicateItem, let existing = findIdentityInKeychain() {
            return .success(existing, subject(of: existing))
        }
        guard status == errSecSuccess,
              let array = items as? [[String: Any]],
              let first = array.first,
              let rawIdentity = first[kSecImportItemIdentity as String] else {
            return .failure(status)
        }
        let identity = rawIdentity as! SecIdentity // standard CF bridging for SecPKCS12Import
        return .success(identity, subject(of: identity))
    }

    private static func findIdentityInKeychain() -> SecIdentity? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassIdentity,
            kSecReturnRef as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let item else {
            return nil
        }
        return (item as! SecIdentity)
    }

    private static func subject(of identity: SecIdentity) -> String {
        var cert: SecCertificate?
        if SecIdentityCopyCertificate(identity, &cert) == errSecSuccess, let cert,
           let summary = SecCertificateCopySubjectSummary(cert) as String? {
            return summary
        }
        return "certificat dezvoltator"
    }

    private static func statusText(_ status: OSStatus) -> String {
        if let message = SecCopyErrorMessageString(status, nil) as String? {
            return message
        }
        return "unknown Security error"
    }

    /// Signs a probe message with the private key and verifies the signature —
    /// proof the imported identity can really sign on this device.
    private static func selfTestSignature(identity: SecIdentity) -> String {
        var privateKey: SecKey?
        guard SecIdentityCopyPrivateKey(identity, &privateKey) == errSecSuccess,
              let key = privateKey else {
            return "private key inaccessible"
        }
        let algorithm = SecKeyAlgorithm.rsaSignatureMessagePKCS1v15SHA256
        guard SecKeyIsAlgorithmSupported(key, .sign, algorithm) else {
            return "algorithm not supported by the imported key"
        }
        let message = Data("iDroid Native Mode — signing probe".utf8)
        var error: Unmanaged<CFError>?
        guard let signature = SecKeyCreateSignature(key, algorithm, message as CFData, &error) as Data?,
              let publicKey = SecKeyCopyPublicKey(key) else {
            return "probe signing failed"
        }
        let ok = SecKeyVerifySignature(publicKey, algorithm, message as CFData, signature as CFData, &error)
        return ok ? "OK" : "signature verification failed"
    }

    private static func storeInKeychain(blob: Data) -> Bool {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: keychainService,
            kSecAttrAccount as String: keychainAccount,
        ]
        SecItemDelete(query as CFDictionary)
        var attrs = query
        attrs[kSecValueData as String] = blob
        attrs[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(attrs as CFDictionary, nil) == errSecSuccess
    }

    private static func loadFromKeychain() -> (p12: Data, password: String)? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: keychainService,
            kSecAttrAccount as String: keychainAccount,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let blob = item as? Data,
              let obj = try? JSONSerialization.jsonObject(with: blob) as? [String: String],
              let b64 = obj["p12"], let pass = obj["pass"],
              let p12 = Data(base64Encoded: b64) else {
            return nil
        }
        return (p12, pass)
    }
}
