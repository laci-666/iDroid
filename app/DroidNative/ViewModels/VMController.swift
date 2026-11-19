import Foundation
import SwiftUI
import UIKit

/// VM state machine. One VM per process (QEMU-on-iOS limitation);
/// full stop requires relaunching the app — the model UTM uses too.
@MainActor
final class VMController: ObservableObject {

    enum State: Equatable {
        case stopped
        case waitingForJIT       // JIT attach requested; waiting for CS_DEBUGGED + P_TRACED
        case booting
        case running
        case snapshotting
        case failed(String)
    }

    @Published var state: State = .stopped {
        didSet {
            // While the VM starts/runs, keep the screen on: an iOS
            // suspension during boot skips the guest clock, and the
            // Android watchdog kills system_server (the zygote loop from
            // the 2026-10-20 log: 38 s Binder transactions, soft-lockup).
            UIApplication.shared.isIdleTimerDisabled =
                (state == .waitingForJIT || state == .booting || state == .running)
        }
    }
    @Published var serialLog: String = ""
    @Published var probe = CapabilityProbe()
    /// Verdict of the last actions in the built-in JIT section (Capabilities).
    @Published var builtInJITStatus: String = "not prepared in this session"
    /// Proba de retea LocalDevVPN (ruta utun + lockdownd 62078), docs/08.
    @Published var localDevVPNStatus: String = "not checked"
    /// Pairing-ul exista in Keychain (nu-i aratam niciodata continutul).
    @Published var hasStoredPairing: Bool = DNPairingFileStore.hasPairing

    /// Guest serial accumulates here and is published into `serialLog` in
    /// rafale rare (vezi `enqueueSerial`): la boot kernelul tipareste in
    /// rafale de zeci/sute de chunk-uri pe secunda, iar fiecare publicare
    /// relayout-eaza tot Text-ul consolei pe MainActor — cauza sacadarii
    /// intregii aplicatii in timpul boot-ului (oct. 2026).
    private var pendingSerial = ""
    private var serialFlushTask: Task<Void, Never>?

    /// Persistent on-disk console (0.2.6, user request): everything that reaches
    /// `serialLog` is also written incrementally to `consola-sesiune.txt`, so
    /// a crash/exit in qemu_init no longer erases the boot evidence. On
    /// relaunch the content is restored into the Console (exportable), and
    /// the file + console are cleared only at the next "Start Android".
    /// Writes are plain write(): they survive process death
    /// (the page is already in the kernel cache), no fsync per chunk.
    private var consoleFileHandle: FileHandle?
    /// How many characters of the current `serialLog` are already on disk (delta).
    private var persistedSerialLength = 0
    private var persistedConsoleBytes: UInt64 = 0
    private var consoleSyncCounter = 0
    private static var persistedConsoleURL: URL {
        VMConfiguration.guestDirectory.appendingPathComponent("consola-sesiune.txt")
    }

    /// Boot timing (the J3 measurement, "measure first" decision):
    /// stages detected from serial, with guest time (kernel clock) vs.
    /// wall time — their ratio shows how much the engine slows down.
    private var bootWallStart: Date?
    private var seenBootMilestones: Set<String> = []
    private var milestoneScanTail = ""
    /// The ⏱ STAGE lines kept separately from the visible window (256 KB): on a
    /// long boot, console rotation would drop exactly the early ones.
    private var bootMilestoneReport: [String] = []

    /// Route 1 / P0 (0.2.8): summary of the current boot mode (for the
    /// ⏱ REPORT and icount-vs-MTTCG A/B comparisons kept across sessions),
    /// time to the first .running state (snapshot resume vs full
    /// boot) and the one-time automatic animation-scale shutoff per boot.
    private var bootModeSummary = ""
    private var runningReported = false
    private var animsZeroApplied = false
    private var bootUsedSnapshot = false
    private var bootedGPUTrial = false // isolated EAGL trial; snapshot button must refuse

    /// Did we pause the VM on backgrounding? Then we resume it
    /// automatically on foregrounding (before it stayed silently stopped).
    private var pausedByBackground = false

    // Phase 1: the real backend when the QEMU framework is bundled; the stub stays
    // only as a fallback for engine-less builds (message shows in Console).
    private let backend: VMBackend = QEMUBackend.isAvailable ? QEMUBackend() : StubVMBackend()
    private let jit = JITCoordinator()
    private var eventTask: Task<Void, Never>?

    var stateDescription: String {
        switch state {
        case .stopped: return "VM stopped. If Android is not installed, the Setup tab downloads and prepares it automatically."
        case .waitingForJIT: return "Waiting for built-in JIT authorization. Keep LocalDevVPN connected and the app in the foreground; the bundled helper handles debugger attachment."
        case .booting: return "Android is starting… follow progress in the Console."
        case .running: return "Android is running."
        case .snapshotting: return "Saving snapshot…"
        case .failed(let msg): return "Error: \(msg)"
        }
    }

    init() {
        // Capcana de crash cat mai devreme: orice semnal fatal (QEMU, ANGLE,
        // virglrenderer) lasa o linie „99 crash-trap” in breadcrumb. O moarte
        // no such line = iOS jetsam/watchdog (Log crash 1 lesson).
        let guestDir = VMConfiguration.guestDirectory
        try? FileManager.default.createDirectory(at: guestDir, withIntermediateDirectories: true)
        dn_install_crash_trap(guestDir.appendingPathComponent("qemu-breadcrumb.txt").path)
        // Previous session console, restored from disk BEFORE any
        // new-session line: after a crash, the boot process stays in the
        // Console and can be exported. Cleared at "Start Android".
        restorePersistedConsole()
        // JIT breadcrumb: if the previous session died in the self-test
        // arena (TXM kills directly, no verdict), say it explicitly in the UI
        // instead of letting the user blindly repeat the same start.
        if JITCoordinator.consumeSelfTestBreadcrumb() {
            state = .failed("JIT authorization was not confirmed last time (TXM stopped the process at the self-test). Check the built-in JIT method in Capabilities (LocalDevVPN + imported pairing + \"Prepare built-in JIT\") and try again. If it repeats, Android can still start on the interpreter, slower.")
            serialLog += "[DroidNative] The previous session ended during the JIT arena self-test.\n"
        }
        if !QEMUBackend.isAvailable {
            serialLog += "[DroidNative] WARNING: the QEMU engine is not bundled in this build (running the stub backend). On the Mac: scripts/stage-qemu-frameworks.sh, then Build.\n"
        }
        // Built-in JIT state, visible immediately in the Console (no navigating through
        // Capabilitati): prezenta randului e si marcajul ca build-ul instalat
        // contine interfata B1.1. Acelasi diagnostic apare in Capabilitati ->
        // the "JIT acquisition" section, the "Built-in helper" row.
        serialLog += "[DroidNative] Built-in JIT (build mark B1.1): \(DNBuiltInJITHost.helperDiagnostic); " +
            "pairing Keychain: \(DNPairingFileStore.hasPairing ? "saved" : "not imported"); " +
            "selected method: \(JITAcquisitionMethod.selected.label).\n"
        serialLog += "[DroidNative] Bundled QEMU engine: \(qemuEngineText).\n"
        // Diagnostic touch (Log 12): GuestDisplayView raporteaza primele
        // pointer events sent to QEMU, so future exports
        // to prove the Swift -> bridge link without guesswork.
        NotificationCenter.default.addObserver(
            forName: .dnPointerDiag, object: nil, queue: .main) { [weak self] note in
            guard let self, let text = note.object as? String else { return }
            self.serialLog += "[DroidNative] 👆 \(text)\n"
        }
        // If the previous QEMU session died abruptly (exit/abort in qemu_init),
        // stages + qemu.log tail are on disk: show them in the Console, with an
        // explicit, instead of a "clean" relaunch hiding the cause.
        if let report = QEMUBackend.previousLaunchReport() {
            // If the previous session reached the main loop, it RAN and
            // was force-closed (normal in our tests) — the report is
            // informational state, not an error blocking start.
            if !QEMUBackend.previousLaunchReachedMainLoop() {
                state = .failed("The previous QEMU session ended abruptly before the main loop. The full diagnostic is in the Console tab (stages + qemu.log).")
            }
            serialLog += report
        }
        if let nativeBreadcrumb = dnNativeFullReadBreadcrumb(),
           nativeBreadcrumb != "complete" {
            serialLog += "[DroidNative] [native-full] previous complete-native run stopped at: \(nativeBreadcrumb)\n"
        }
        eventTask = Task { [weak self] in
            guard let self else { return }
            for await event in backend.events {
                switch event {
                case .serialOutput(let text):
                    self.enqueueSerial(text)
                case .displayResized(_, _):
                    if self.state == .booting {
                        self.state = .running
                        self.noteReachedRunning()
                    }
                case .exited(_):
                    self.bootedGPUTrial = false
                    self.state = .stopped
                }
            }
        }
        persistConsoleNow()
    }

    func refreshProbe() {
        probe.refresh()
        hasStoredPairing = DNPairingFileStore.hasPairing
    }

    /// Append one diagnostic line and put it on disk before returning.
    /// The complete-native harness uses this at launch and before a
    /// synchronous guest-code run: a hard crash in guest code must not
    /// take the only copy of the "previous run stopped at ..." line
    /// down with the process.
    func appendPersistentDiagnosticLine(_ text: String) {
        serialLog += "[DroidNative] \(text)\n"
        persistConsoleNow()
    }

    /// Coalescing for the serial console: publish every ~50 ms (20/s)
    /// or when 16 KB gathered. The console is a UITextView with incremental append
    /// (delta only), so cadence stays smooth with no global relayout.
    /// Logul vizibil e borneat la 256 KB; istoricul complet al consolei
    /// stays on disk in Guest/consola-sesiune.txt (incremental persistence
    /// in `flushSerial`/`persistConsoleDelta`) si intra in export.
    private func enqueueSerial(_ text: String) {
        pendingSerial += text
        if pendingSerial.count >= 16_384 {
            flushSerial()
            return
        }
        guard serialFlushTask == nil else { return }
        serialFlushTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(50))
            guard let self else { return }
            self.serialFlushTask = nil
            self.flushSerial()
        }
    }

    private func flushSerial() {
        guard !pendingSerial.isEmpty else { return }
        let flushed = pendingSerial
        serialLog += flushed
        pendingSerial.removeAll(keepingCapacity: true)
        persistConsoleDelta()
        if serialLog.count > 262_144 {
            serialLog = String(serialLog.suffix(196_608))
            // The RAM window rotated; on disk the history stays whole,
            // so future deltas start from the new RAM length.
            persistedSerialLength = serialLog.count
        }
        scanBootMilestones(chunk: flushed)
        persistConsoleDelta()
    }

    // MARK: - Persistent console (consola-sesiune.txt)

    private func openConsoleFile() {
        guard consoleFileHandle == nil else { return }
        let url = Self.persistedConsoleURL
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        if let handle = try? FileHandle(forWritingTo: url) {
            persistedConsoleBytes = (try? handle.seekToEnd()) ?? 0
            consoleFileHandle = handle
        }
    }

    /// Writes to disk only the part of `serialLog` not yet persisted.
    /// Also catches directly-added lines (not just guest serial), because
    /// delta se calculeaza pe tot sirul, indiferent cine a adaugat.
    private func persistConsoleDelta() {
        let current = serialLog.count
        guard current > persistedSerialLength else {
            if current < persistedSerialLength { persistedSerialLength = current }
            return
        }
        let delta = String(serialLog.dropFirst(persistedSerialLength))
        persistedSerialLength = current
        guard let data = delta.data(using: .utf8), !data.isEmpty else { return }
        openConsoleFile()
        if persistedConsoleBytes > 8 * 1024 * 1024 {
            trimPersistedConsoleFile()
        }
        guard let handle = consoleFileHandle else { return }
        try? handle.write(contentsOf: data)
        persistedConsoleBytes += UInt64(data.count)
        consoleSyncCounter += 1
        if consoleSyncCounter % 20 == 0 {
            try? handle.synchronize()
        }
    }

    /// File cap: keep the last ~4 MB (enough for any
    /// boot + coada decisiva a crash-ului), restul e istoric vechi.
    private func trimPersistedConsoleFile() {
        if let handle = consoleFileHandle {
            try? handle.close()
            consoleFileHandle = nil
        }
        let url = Self.persistedConsoleURL
        if let data = try? Data(contentsOf: url), data.count > 4 * 1024 * 1024 {
            try? data.suffix(4 * 1024 * 1024).write(to: url, options: .atomic)
        }
        persistedConsoleBytes = 0
        openConsoleFile()
    }

    /// Flush + persist + synchronize: used at export and on
    /// backgrounding, when "on disk, now" really matters.
    private func persistConsoleNow() {
        flushSerial()
        persistConsoleDelta()
        try? consoleFileHandle?.synchronize()
    }

    /// Restores the previous session console from disk (called in init,
    /// before new-session lines). The file is NOT deleted here: it stays
    /// for export until the next boot.
    private func restorePersistedConsole() {
        let url = Self.persistedConsoleURL
        guard let data = try? Data(contentsOf: url), !data.isEmpty else { return }
        let tailBytes = 512 * 1024
        let sliced = data.count > tailBytes ? data.suffix(tailBytes) : data
        let text = String(decoding: sliced, as: UTF8.self)
        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        serialLog += "===== PREVIOUS SESSION CONSOLE (saved on disk; export it from here — cleared at \"Start Android\") =====\n"
        if data.count > tailBytes {
            serialLog += "[... the first \(data.count - tailBytes) bytes do not fit the console window; the export includes the full file ...]\n"
        }
        serialLog += text
        if !serialLog.hasSuffix("\n") { serialLog += "\n" }
        serialLog += "===== END OF PREVIOUS CONSOLE — the current session starts below =====\n"
        // Header/footer are RAM-only; what is on disk stays untouched,
        // iar deltele viitoare se calculeaza peste tot serialLog-ul curent.
        persistedSerialLength = serialLog.count
        persistedConsoleBytes = UInt64(data.count)
    }

    /// At "Start Android": the old console (restored after crash) goes away
    /// from RAM and disk; the new boot starts with a clean console, which
    /// se va persista din nou, linie cu linie.
    private func clearPersistedConsoleForNewBoot() {
        if let handle = consoleFileHandle {
            try? handle.close()
            consoleFileHandle = nil
        }
        try? FileManager.default.removeItem(at: Self.persistedConsoleURL)
        persistedConsoleBytes = 0
        persistedSerialLength = 0
        pendingSerial = ""
        // New console, but with the same diagnostic header as at launch
        // (helper JIT + motor) — exporturile raman auto-descriptive.
        serialLog = "[DroidNative] — New console: start requested now (the previous console was cleared at this moment) —\n"
        serialLog += "[DroidNative] Built-in JIT (build mark B1.1): \(DNBuiltInJITHost.helperDiagnostic); " +
            "pairing Keychain: \(DNPairingFileStore.hasPairing ? "saved" : "not imported"); " +
            "selected method: \(JITAcquisitionMethod.selected.label).\n"
        serialLog += "[DroidNative] Bundled QEMU engine: \(qemuEngineText).\n"
    }

    private static let bootMilestones: [(id: String, label: String, pattern: String)] = [
        ("kernel", "kernel started", "Booting Linux"),
        ("ramdisk-init", "init from ramdisk", "Run /init as init process"),
        ("android-init", "Android init (first stage)", "init first stage started"),
        ("late-fs", "late-fs (preparing /data)", "processing action (late-fs"),
        ("zygote", "zygote started", "starting service 'zygote'"),
        ("bootanim", "boot animation started", "starting service 'bootanim'"),
        ("boot-completed", "BOOT COMPLETED — Android ready", "sys.boot_completed=1"),
    ]

    /// Searches boot stages in freshly published serial text (with a
    /// 512-char tail kept between chunks, for patterns split at the
    /// boundary) and writes one "⏱ STAGE" line with measured times.
    private func scanBootMilestones(chunk: String) {
        guard let start = bootWallStart else { return }
        let scan = milestoneScanTail + chunk
        milestoneScanTail = String(scan.suffix(512))
        let nsScan = scan as NSString
        for milestone in Self.bootMilestones where !seenBootMilestones.contains(milestone.id) {
            let range = nsScan.range(of: milestone.pattern)
            guard range.location != NSNotFound else { continue }
            seenBootMilestones.insert(milestone.id)
            let wall = Date().timeIntervalSince(start)
            // The last kernel timestamp [ 123.456789] before the stage.
            var guestText = ""
            var slowText = ""
            if let regex = try? NSRegularExpression(pattern: "\\[\\s*(\\d+\\.\\d+)\\]") {
                let matches = regex.matches(in: scan, range: NSRange(location: 0, length: nsScan.length))
                if let m = matches.last(where: { $0.range.location <= range.location }),
                   let guest = Double(nsScan.substring(with: m.range(at: 1))) {
                    guestText = String(format: ", guest T+%.1fs", guest)
                    if guest > 1 {
                        // Clock ratio, NOT CPU speed: ~1x = guest clock
                        // urmareste peretele; sub icount creste (timp diluat).
                        slowText = String(format: " (wall/guest ~%.1fx)", wall / guest)
                    }
                }
            }
            let line = String(format: "[DroidNative] ⏱ STAGE %@: wall T+%.1fs%@%@\n",
                              milestone.label, wall, guestText, slowText)
            serialLog += line
            bootMilestoneReport.append(line)
            if milestone.id == "boot-completed" {
                if state == .booting {
                    state = .running
                    noteReachedRunning()
                }
                // Raportul de boot (A/B icount vs MTTCG, GL on/off): linia
                // stays in console + export and is kept across sessions in
                // UserDefaults, ca comparatia sa fie la indemana.
                let report = String(format: "[DroidNative] ⏱ BOOT REPORT: Android ready in T+%.1fs wall (%@).\n",
                                    wall, bootModeSummary)
                serialLog += report
                bootMilestoneReport.append(report)
                UserDefaults.standard.set(report, forKey: "dnLastBootReport")
                applyZeroAnimationScalesOnce()
            }
        }
    }

    /// Prima trecere in .running pe boot-ul curent: la reluarea din
    /// snapshot, THIS number is the end-to-end measured "instant resume" from
    /// the "Start" tap (JIT included), not an estimate.
    private func noteReachedRunning() {
        guard !runningReported, let start = bootWallStart else { return }
        runningReported = true
        let wall = Date().timeIntervalSince(start)
        let mode = bootUsedSnapshot ? "snapshot resume" : "full boot"
        serialLog += String(format: "[DroidNative] ⏱ First RUNNING state at T+%.1fs wall (%@).\n", wall, mode)
    }

    /// Animation scales 0 in guest, automatically after BOOT COMPLETED: on TCG,
    /// WindowManager/animator animations are wasted work (composed frames
    /// for transitions we see choppy anyway). Commands go over the
    /// serial console; on images without a serial shell they stay echoes
    /// inofensive in log. Idempotent; o singura data pe boot.
    private func applyZeroAnimationScalesOnce() {
        guard !animsZeroApplied else { return }
        animsZeroApplied = true
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(3))
            guard let self else { return }
            self.serialLog += "[DroidNative] Turning off guest animation scales for fluidity (settings put global *_animation_scale 0).\n"
            for key in ["window_animation_scale", "transition_animation_scale", "animator_duration_scale"] {
                self.sendConsoleInput("settings put global \(key) 0")
                try? await Task.sleep(for: .milliseconds(400))
            }
        }
    }

    /// The diagnostic text for the Console export (share button):
    /// visible serial console + launch breadcrumb + QEMU stdout/stderr
    /// from disk. Phone photos lose exactly the important lines
    /// (storage, QMP BLOCK_IO_ERROR, argv) — the .txt file keeps them.
    /// The displayed version (MARKETING_VERSION) + build — single source of
    /// truth for the UI (Settings) and for diagnostic export headers
    /// diagnostic: fiecare log trimis se identifica singur.
    static var appVersionDisplay: String {
        let info = Bundle.main.infoDictionary
        let v = info?["CFBundleShortVersionString"] as? String ?? "?"
        let b = info?["CFBundleVersion"] as? String ?? "?"
        return "\(v) (\(b))"
    }

    func makeConsoleExportText() -> String {
        persistConsoleNow()
        let guestDir = VMConfiguration.guestDirectory
        let kind = GuestKind(rawValue: UserDefaults.standard.string(forKey: "guestKind") ?? "") ?? .android
        let cfg = VMConfiguration.default(probe: probe, kind: kind)
        var out = "iDroid — console diagnostic export\n"
        out += "App version: \(Self.appVersionDisplay)\n"
        out += "Device: \(probe.deviceModel), iOS \(UIDevice.current.systemVersion)\n"
        out += "Data: \(ISO8601DateFormatter().string(from: Date()))\n"
        out += "Bundled QEMU engine: \(qemuEngineText)\n"
        out += "VM state: \(stateDescription)\n"
        if let lastReport = UserDefaults.standard.string(forKey: "dnLastBootReport") {
            out += "Last recorded boot report: \(lastReport)"
            if !lastReport.hasSuffix("\n") { out += "\n" }
        }
        if let cert = DevSigningIdentity.storedSummary {
            out += "Native Mode signing identity (Keychain): \(cert)\n"
        }
        if let certMsg = DevSigningIdentity.lastImportMessage {
            out += "Last certificate import: \(certMsg)\n"
        }
        if !bootMilestoneReport.isEmpty {
            out += "Boot timer (detected stages, current session):\n"
            out += bootMilestoneReport.joined()
        }
        if kind == .android {
            out += "Storage/disks at export: \(QEMUBackend.storageDiagnostics(config: cfg))\n"
        }
        out += "\n===== SERIAL CONSOLE (visible in the app) =====\n"
        out += serialLog.isEmpty ? "(gol)\n" : serialLog
        if !out.hasSuffix("\n") { out += "\n" }
        let breadcrumbTail = readTailText(at: guestDir.appendingPathComponent("qemu-breadcrumb.txt"),
                                          maxBytes: 64 * 1024)
        out += "\n===== qemu-breadcrumb.txt (launch stages) =====\n"
        out += breadcrumbTail
        if !out.hasSuffix("\n") { out += "\n" }
        out += "\n===== native-full-breadcrumb.txt (complete-native last stage) =====\n"
        out += dnNativeFullBreadcrumbText()
        out += "\n===== native-full-progress.txt (complete-native synchronous stages) =====\n"
        out += dnNativeFullProgressText()
        let qemuTail = readTailText(at: guestDir.appendingPathComponent("qemu.log"),
                                    maxBytes: 512 * 1024)
        let eagl = qemuTail.contains("gl early init OK: ANGLE OpenGL/EAGL") ||
                   breadcrumbTail.contains("11g bridge: GL early init OK")
        let scanout = qemuTail.contains("gl scanout texture #")
        if eagl || scanout || qemuTail.contains("[dn-dpy] gl readback: mode=") {
            out += "GPU host evidence (no ADB): EAGL/ES3 init=\(eagl ? "yes" : "not found in log tail"), QEMU GL/virgl scanout callback=\(scanout ? "seen" : "not found in log tail"). These host signals alone do not identify the guest EGL driver; a separate SurfaceFlinger probe confirmed Mesa VirGL.\n"
            if let last = qemuTail.split(separator: "\n").last(where: { $0.contains("[dn-dpy] gl readback: mode=") }) {
                out += "Most recent host readback sample: \(last)\n"
            }
            if let last = qemuTail.split(separator: "\n").last(where: { $0.contains("[dn-dpy] gl stages:") }) {
                out += "Most recent host GL stages: \(last)\n"
            }
            if let last = qemuTail.split(separator: "\n").last(where: { $0.contains("[dn-dpy] gl publish damage:") }) {
                out += "Most recent host upload damage: \(last)\n"
            }
        }
        out += "\n===== qemu.log (512 KB tail) =====\n"
        out += qemuTail
        if !out.hasSuffix("\n") { out += "\n" }
        // The full console history, including the part rotated out of the window
        // visible (256 KB) and, after a crash, the previous session's boot.
        out += "\n===== consola-sesiune.txt (full persisted console history) =====\n"
        out += readTailText(at: Self.persistedConsoleURL, maxBytes: 2 * 1024 * 1024)
        if !out.hasSuffix("\n") { out += "\n" }
        return out
    }

    private func readTailText(at url: URL, maxBytes: Int) -> String {
        do {
            let handle = try FileHandle(forReadingFrom: url)
            defer { try? handle.close() }
            let size = (try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber)?.uint64Value ?? 0
            if size > UInt64(maxBytes) {
                try handle.seek(toOffset: size - UInt64(maxBytes))
            }
            let data = try handle.read(upToCount: maxBytes) ?? Data()
            let text = String(decoding: data, as: UTF8.self)
            if size > UInt64(maxBytes) {
                return "[... the first \(size - UInt64(maxBytes)) bytes omitted at export ...]\n" + text
            }
            return text.isEmpty ? "(gol)\n" : text
        } catch {
            return "(missing or unreadable: \(error.localizedDescription))\n"
        }
    }

    /// Full JIT probe (with execution jump) — only on explicit request
    /// from the UI; on TXM it can close the app, itself an answer.
    func runFullJITProbe() { probe.runFullClassicProbe() }

    /// The built-in helper really is in the bundle (false in LiveContainer /
    /// extension-less builds) — the UI hides its section when missing.
    var builtInJITAvailable: Bool { DNBuiltInJITHost.isHelperInstalled }

    /// Flavor of the bundled QEMU engine, for Capabilities/Console:
    /// the TCI interpreter needs no JIT; the TCG engine won't start without it.
    var qemuEngineText: String {
        guard QEMUBackend.isAvailable else { return "absent (backend stub)" }
        switch QEMUBackend.engineFlavor {
        case .tci: return "TCI interpreter (no JIT)"
        case .tcg: return "TCG with JIT (needs JIT acquisition at boot)"
        }
    }

    /// Text diagnostic of helper discovery (for the row in
    /// Capabilitati): prezenta reala sau motivul concret al absentei.
    var builtInJITDiagnostic: String { DNBuiltInJITHost.helperDiagnostic }

    /// Import pairing din Files in Keychain (Capabilitati). Nu retinem URL-ul.
    func importPairingFile(from url: URL) {
        do {
            try DNPairingFileStore.importPairing(from: url)
            hasStoredPairing = true
            builtInJITStatus = "Pairing imported and saved in Keychain. Next step: \"Prepare built-in JIT\", with LocalDevVPN connected."
        } catch {
            builtInJITStatus = "Import failed: \(error.localizedDescription)"
        }
    }

    func deletePairingFile() {
        DNPairingFileStore.delete()
        hasStoredPairing = false
        builtInJITStatus = "Pairing deleted from Keychain."
    }

    /// DDI preparation via helper only — gate B1 (docs/08), no boot.
    func prepareBuiltInJIT() async {
        builtInJITStatus = "Preparing via helper (the first time it may download/mount the DDI — can take a while)…"
        let response = await jit.prepareBuiltIn()
        builtInJITStatus = (response.ok ? "OK: " : "Error: ") + response.message
        serialLog += "[DroidNative] Built-in JIT preparation: \(response.message)\n"
        refreshProbe()
    }

    func resetBuiltInDDI() async {
        builtInJITStatus = "Resetting the helper DDI cache…"
        let response = await jit.resetBuiltInDDI()
        builtInJITStatus = (response.ok ? "OK: " : "Error: ") + response.message
    }

    /// Proba LocalDevVPN pe fir de fundal (sockets sincrone, ~0.5 s worst case).
    func checkLocalDevVPN() async {
        localDevVPNStatus = "checking…"
        let result = await Task.detached(priority: .utility) {
            DNLocalDevVPNProbe.probe()
        }.value
        localDevVPNStatus = result.summary
    }

    /// Fluxul complet de boot, cu cascada de acceleratoare:
    /// HVF -> TCG-JIT (TXM) -> TCTI. Each failure drops one rung, never stops the app.
    func boot() async {
        // The previous console (restored after a possible crash) is cleared
        // abia acum — pana la apasarea asta a putut fi exportata.
        clearPersistedConsoleForNewBoot()
        refreshProbe()
        // Guest type chosen in the Setup tab (Android by default).
        let kind = GuestKind(rawValue: UserDefaults.standard.string(forKey: "guestKind") ?? "") ?? .android
        // var (not let): the JIT gate can lower tb-size to the maximum
        // TXM accepts in this session (RX-mmap sweep).
        var config = VMConfiguration.default(probe: probe, kind: kind)
        let parallelRequested = kind == .android &&
            UserDefaults.standard.bool(forKey: "dnParallelGPUTrialNextBoot")
        let gpuTrialRequested = kind == .android &&
            (UserDefaults.standard.bool(forKey: "dnGPUTrialNextBoot") || parallelRequested)
        bootedGPUTrial = false
        // Protectie snapshot (2026-10-21): snapshot-ul intern e valid doar
        // in the mode it was saved. Toggling icount also changes the count of
        // de vCPU (2 vs 4) si ceasul virtual; incarcarea lui in celalalt
        // mode would fail/corrupt. On mismatch we ignore the snapshot for
            // this boot and say so loudly, not silently. Missing flag (saved
        // before 0.1.9) = past icount (the only mode back then).
        if config.loadSnapshot && !gpuTrialRequested {
            // The machine shape (i=icount, g=GL acceleration) must be
            // identical between save and resume: GPU type, vCPU count
            // and the virtual clock are part of the saved state.
            // EFFECTIVE shape: if ANGLE is missing from the bundle, the backend
            // coboara singur pe software (g0) — comparatia tine cont de asta.
            let shapeNow = "i\(config.useIcount ? 1 : 0)g\((config.gpuAcceleration && QEMUBackend.glFrameworksPresent) ? 1 : 0)m\(config.memoryMegabytes)r\(config.displayWidth)x\(config.displayHeight)d2"
            var shapeSaved = UserDefaults.standard.string(forKey: "dnSnapshotShape")
                ?? "i\((UserDefaults.standard.object(forKey: "dnSnapshotIcount") as? Bool ?? true) ? 1 : 0)g0"
            // Old shapes (before 0.2.8) had no RAM component;
            // back then the guest always ran with 4096 MB (probe plan).
            if !shapeSaved.contains("m") { shapeSaved += "m4096" }
            // Pre-0.2.19 shapes had no resolution + devices
            // "d2" (virtio keyboard): they will never match — correct,
            // the machine really changed; normalize only for the message.
            if !shapeSaved.contains("r") { shapeSaved += "r0d1" }
            if shapeSaved != shapeNow {
                config.loadSnapshot = false
                let advice = shapeNow.contains("g1")
                    ? "With GL on, snapshots cannot be saved at all (\"virgl is not yet migratable\") — instant resume stays available only for starts with GL off."
                    : "Save a new snapshot in the desired shape, then instant resume works again."
                serialLog += "[DroidNative] ⚠️ Snapshot saved in shape \(shapeSaved), now starting \(shapeNow) (i=icount, g=GL) — ignoring the snapshot this start (full boot). \(advice)\n"
            }
        }
        config.prepareStorage()
        guard config.guestImageInstalled else {
            state = .failed(kind == .android
                ? "Android is not installed. Go to the Setup tab and tap \"Automatic setup\"."
                : "The test guest is not installed. Download it from the Setup tab (~41 MB).")
            return
        }
        if gpuTrialRequested {
            // Explicit ONE-SHOT. Never resume software g0 on a GL device, and
            // never boot GL on the user's original work disks. Consume the
            // flag before any QEMU/JIT step so a crash cannot repeat GL.
            UserDefaults.standard.set(false, forKey: "dnGPUTrialNextBoot")
            UserDefaults.standard.set(false, forKey: "dnParallelGPUTrialNextBoot")
            guard QEMUBackend.isAvailable && QEMUBackend.glFrameworksPresent else {
                state = .failed("GPU trial refused: ANGLE EGL/GLESv2 or QEMU is not bundled. Original software VM and snapshot are unchanged. On the Mac, stage the TCG QEMU frameworks first.")
                return
            }
            do {
                if parallelRequested {
                    // Missing preference => the device-tested 2-vCPU 0.4.9
                    // profile. Invalid stored counts fail CLOSED in the clone
                    // builder; never silently redirect into the 2-vCPU disk.
                    let trialCPUs = UserDefaults.standard.object(forKey: "dnParallelCPUCount") as? Int ?? 2
                    config = try config.isolatedParallelGPUTrial(cpuCount: trialCPUs)
                    let trialFolder = trialCPUs == 2 ? "ParallelGPUTrial" : "ParallelGPUTrial\(trialCPUs)"
                    serialLog += "[DroidNative] Parallel CPU + GPU trial: \(trialCPUs) vCPU MTTCG, icount OFF, ANGLE OpenGL/EAGL; APFS clone \(trialFolder) from the working GPU trial. Android real-time watchdog or host contention may cause a regression. Software/g0, GPUTrial and the other CPU-count clones are untouched. One shot; next start defaults to software.\n"
                } else {
                    config = try config.isolatedGPUTrial()
                    serialLog += "[DroidNative] GPU trial: ANGLE OpenGL/EAGL + virtio-gpu-gl, independent APFS work-disk clones. Software g0 snapshot and main userdata are untouched. Trial data does not sync back; GL snapshots are unavailable. Next start defaults to software.\n"
                }
                bootedGPUTrial = true
            } catch {
                state = .failed(error.localizedDescription)
                serialLog += "[DroidNative] GPU trial refused: \(error.localizedDescription) Software disks/snapshot unchanged.\n"
                return
            }
        }
        if kind == .android {
            // Which Lineage build starts, exactly — essential when diagnosing
            // images (e.g. init vs libc++ mismatched in the same package).
            let stateURL = VMConfiguration.guestDirectory.appendingPathComponent("setup-state.json")
            let dec = JSONDecoder(); dec.dateDecodingStrategy = .iso8601
            if let data = try? Data(contentsOf: stateURL),
               let st = try? dec.decode(SetupState.self, from: data),
               let tag = st.lineageReleaseTag {
                serialLog += "[DroidNative] Android instalat: release \(tag) (\(st.lineageAssetName ?? "asset necunoscut")).\n"
            }
            serialLog += "[DroidNative] Storage/disks before boot: \(QEMUBackend.storageDiagnostics(config: config)).\n"
        }
        var accelerator = probe.chosenAccelerator
        var arena: JITArena? = nil

        // TCI stage: the bundled engine is the interpreter, so we skip JIT acquisition
        // (no StikDebug and no 384 MB arena for a guest that does not
        // genereaza cod). Se reactiveaza singura la build-ul TCG.
        if accelerator == .tcgJIT, QEMUBackend.isAvailable, QEMUBackend.bundledEngineIsInterpreterOnly {
            accelerator = .tcgTCTI
            serialLog += "[DroidNative] Bundled TCI engine: starting on the interpreter, no JIT acquisition.\n"
        }

        if config.isParallelTrial &&
            (accelerator != .tcgJIT || !QEMUBackend.isAvailable ||
             QEMUBackend.bundledEngineIsInterpreterOnly) {
            state = .failed("Parallel CPU trial requires the bundled TCG-JIT engine and JIT authorization. No TCI/HVF fallback is allowed on these separate disks. Working GPU and software profiles are unchanged.")
            serialLog += "[DroidNative] Parallel CPU trial refused before QEMU: native TCG-JIT unavailable.\n"
            return
        }

        // Bundled TCG engine? Then TXM authorization is NOT done via our arena
        // noastra, ci chiar de QEMU (fork UTM, tcg/region.c): in qemu_init,
        // if it sees an attached debugger, executes brk 0x69 and the LEGACY script
        // ii autorizeaza bufferul de cod, apoi se detaseaza singur (docs/09).
        // Mandatory sequence: legacy-script attach -> backend start
        // WITH the debugger still attached -> detach happens in qemu_init.
        let engineIsNativeTCG = QEMUBackend.isAvailable && !QEMUBackend.bundledEngineIsInterpreterOnly
        var legacyTCGBoot = false
        var bgTask: UIBackgroundTaskIdentifier = .invalid

        if engineIsNativeTCG {
            // Prewarm BEFORE attach: the cold dlopen (several seconds
            // on first launch after a fresh install) has no place in
            // the window where the legacy script waits for the brk from
            // qemu_init — authorization can die of old age by then.
            let prewarmStart = Date()
            let prewarmMessage = QEMUBackend.prewarmLibrary()
            let prewarmSeconds = String(format: "%.2f", Date().timeIntervalSince(prewarmStart))
            serialLog += "[DroidNative] QEMU library prewarm (before JIT attach): \(prewarmMessage), \(prewarmSeconds) s.\n"
        }

        if accelerator == .tcgJIT {
            state = .waitingForJIT
            // We keep the process awake during acquisition (the user may be
            // in StikDebug, and the built-in helper works in another process):
            // without the background assertion, iOS suspends us and the protocol — which
            // se finalizeaza in procesul nostru — gaseste sesiunea expirata.
            bgTask = UIApplication.shared.beginBackgroundTask(withName: "JIT acquisition") {
                if bgTask != .invalid {
                    UIApplication.shared.endBackgroundTask(bgTask)
                    bgTask = .invalid
                }
            }
            if engineIsNativeTCG {
                // Two-stage gate, with one retry: observed attach
                // (CS_DEBUGGED + P_TRACED) nu garanteaza ca autorizarea mai e
                // alive when qemu_init reaches the RX mmap of the TCG buffer
                // (debugger detached meanwhile or without TXM rights, e.g.
                // DDI not prepared) — the real case of 21.10.2026: EPERM at
                // "allocate … jit buffer" + QEMU exit(), the app seemed to
                // as "dies instantly", no .ips. We probe exactly that operation
                // and do NOT start QEMU on a dead authorization.
                var tcgFailure: String?
                var attempt = 0
                while attempt < 2, tcgFailure == nil, !legacyTCGBoot {
                    attempt += 1
                    serialLog += "[DroidNative] TCG engine: requesting legacy-script attach (attempt \(attempt)/2); QEMU authorizes its own code buffer in qemu_init (brk 0x69).\n"
                    serialLog += "[DroidNative] \(jit.requestAttach(txmPresent: probe.txmPresent, scriptKind: .legacy))\n"
                    let attached = await jit.waitForDebugger()
                    guard attached, jit.isBrkProtocolUnlocked else {
                        if jit.debuggerAttachedAndLeft {
                            tcgFailure = JITError.attachedAndLeft.localizedDescription
                        } else if let diagnostic = jit.lastWaitDiagnostic {
                            tcgFailure = "The scripted debugger did not attach. \(diagnostic) Check in Capabilities: \"Check LocalDevVPN\", then \"Prepare built-in JIT (DDI)\"."
                        } else {
                            tcgFailure = "The scripted debugger did not attach or is not the compatible one."
                        }
                        break
                    }
                    // Size sweep: TXM may accept small mappings and
                    // refuza bufferul intreg (observat: 16 KB OK, 384 MB
                    // EPERM, acelasi debugger). Pornim cu maximul acceptat.
                    let sweep = jit.rxMmapLargestOKMegabytes(
                        candidates: [config.jitArenaMegabytes, 256, 192, 128, 96, 64])
                    if sweep.largestOK >= config.jitArenaMegabytes {
                        legacyTCGBoot = true
                        serialLog += "[DroidNative] Debugger attached (legacy) + RX-mmap probe OK at \(config.jitArenaMegabytes) MB. Starting QEMU TCG; buffer authorization ends with the script detaching in qemu_init.\n"
                        break
                    }
                    if sweep.largestOK >= 64 {
                        serialLog += "[DroidNative] TXM accepts at most \(sweep.largestOK) MB executable now (requested \(config.jitArenaMegabytes) MB, errno=\(sweep.firstErrno)) — starting with tb-size reduced to \(sweep.largestOK) MB (slower translation, but it starts).\n"
                        config.jitArenaMegabytes = sweep.largestOK
                        legacyTCGBoot = true
                        break
                    }
                    if jit.isTracedNow {
                        var helperNote = ""
                        if let response = jit.lastBuiltInEnableResponse {
                            helperNote = " The helper reports: \(response.message)"
                        }
                        tcgFailure = "The debugger is attached, but TXM refuses any useful executable mapping (RX-mmap sweep down to 64 MB, errno=\(sweep.firstErrno)) — most likely DDI not prepared for this install or a debugger without TXM rights. Tap \"Prepare built-in JIT (DDI)\" in Capabilities and try again.\(helperNote)"
                    } else if attempt < 2 {
                        serialLog += "[DroidNative] The RX-mmap sweep failed completely (errno=\(sweep.firstErrno)) and the debugger is no longer attached — it detached between gate and start. Requesting attach again.\n"
                    } else {
                        tcgFailure = "The debugger detaches before QEMU allocates its buffer (RX-mmap sweep, errno=\(sweep.firstErrno) on both attempts)."
                    }
                }
                if let reason = tcgFailure {
                    // Do NOT start QEMU: without a live authorization, qemu_init exits with
                    // exit() at buffer allocation and the app "disappears".
                    serialLog += "[DroidNative] TCG boot stopped before QEMU: \(reason)\n"
                    if bgTask != .invalid {
                        UIApplication.shared.endBackgroundTask(bgTask)
                        bgTask = .invalid
                    }
                    state = .failed("The TCG engine needs TXM-authorized JIT before start: \(reason) Check LocalDevVPN + \"Prepare built-in JIT\", launch the app from its icon (not from Xcode) and try again.")
                    return
                }
                // bgTask stays active across qemu_init; closed after detach.
            } else {
                serialLog += "[DroidNative] \(jit.requestAttach(txmPresent: probe.txmPresent))\n"
                let attached = await jit.waitForDebugger()
                if attached {
                    do {
                        arena = try jit.acquireArena(sizeBytes: config.jitArenaMegabytes * 1024 * 1024)
                        serialLog += "[DroidNative] JIT arena authorized via TXM (\(config.jitArenaMegabytes) MB, execution self-test passed).\n"
                    } catch {
                        serialLog += "[DroidNative] JIT unavailable (\(error.localizedDescription)) — falling back to TCTI (slow).\n"
                        accelerator = .tcgTCTI
                    }
                } else if jit.debuggerAttachedAndLeft {
                    serialLog += "[DroidNative] \(JITError.attachedAndLeft.localizedDescription) Cobor pe TCTI (lent).\n"
                    accelerator = .tcgTCTI
                } else {
                    var detail = ""
                    if let response = jit.lastBuiltInEnableResponse, !response.ok {
                        detail = " The helper reports: \(response.message)"
                    }
                    serialLog += "[DroidNative] The debugger did not attach in time — falling back to TCTI (slow). For JIT: LocalDevVPN connected + method chosen in Capabilities.\(detail)\n"
                    accelerator = .tcgTCTI
                }
                if bgTask != .invalid {
                    UIApplication.shared.endBackgroundTask(bgTask)
                    bgTask = .invalid
                }
            }
        }

        bootWallStart = Date()
        seenBootMilestones.removeAll()
        bootMilestoneReport.removeAll()
        milestoneScanTail = ""
        runningReported = false
        animsZeroApplied = false
        bootUsedSnapshot = config.loadSnapshot
        bootModeSummary = "vCPU=\(config.cpuCount), RAM=\(config.memoryMegabytes)MB, \(config.displayWidth)x\(config.displayHeight), icount=\(config.useIcount ? "on" : "off/MTTCG"), GL=\(config.gpuAcceleration ? "on (EAGL trial)" : "off"), profile=\(config.isParallelTrial ? "parallel GPU APFS clone" : (config.isGPUTrial ? "isolated APFS clone" : "main")), source=\(config.loadSnapshot ? "snapshot" : "full boot")"
        serialLog += "[DroidNative] Effective boot config: \(config.cpuCount) vCPU, \(config.memoryMegabytes) MB RAM, guest screen \(config.displayWidth)x\(config.displayHeight), icount=\(config.useIcount ? "on" : "off"), GPU=\(config.isParallelTrial ? "separate parallel CPU + EAGL trial" : (config.isGPUTrial ? "isolated ANGLE OpenGL/EAGL trial" : "software")), accelerator=\(accelerator.rawValue).\n"
        serialLog += "[DroidNative] ⏱ Boot timer started — stages appear below, guest vs. wall time.\n"
        state = .booting
        do {
            try await backend.start(config: config, accelerator: accelerator, arena: arena)
        } catch {
            state = .failed(error.localizedDescription)
        }

        if legacyTCGBoot {
            // Verdictul autorizarii din qemu_init: scriptul legacy se
            // detaches after exactly one prepared region. If the debugger
            // stays attached, the brk was not processed (wrong script/method)
            // and execution from the buffer will die — say it in the console now,
            // not after the crash. The QEMU thread already runs independently.
            let detached = await jit.waitForDetach()
            if detached {
                var helperNote = ""
                if let response = jit.lastBuiltInEnableResponse {
                    helperNote = " Helper: \(response.message)"
                }
                serialLog += "[DroidNative] The legacy script detached: the TCG buffer was TXM-authorized in qemu_init.\(helperNote)\n"
            } else if seenBootMilestones.contains("kernel") {
                // Observed 2026-10-20: the debug session can stay attached
                // well after qemu_init, although the guest already executes translated code
                // (so the buffer IS TXM-authorized). No more false alarms.
                serialLog += "[DroidNative] Note: the debugger is still attached, but the guest already runs TCG code (kernel started) — TXM authorization is effective; the late detach is only cosmetic.\n"
            } else {
                serialLog += "[DroidNative] WARNING: the debugger is still attached after QEMU start — TXM authorization of the TCG buffer is uncertain. If the app dies at the first guest code, the cause is here (unprocessed legacy script).\n"
            }
            if bgTask != .invalid {
                UIApplication.shared.endBackgroundTask(bgTask)
                bgTask = .invalid
            }
        }
    }

    func snapshotAndPause() async {
        // Log 13: taps during a save started a second
        // save over the first ("migration in progress" + a false ❌).
        guard state != .snapshotting else {
            serialLog += "[DroidNative] Snapshot already in progress — wait for the ✅ confirmation before tapping again.\n"
            return
        }
        // Log crash 3 (0.2.13): with GL on, QEMU refused the save with
        // "[dn-snap] save FAILED: virgl is not yet migratable" — virgl state
        // is not serializable. Say it BEFORE trying,
        // without touching QEMU; the snapshot stays available in
        // starts with GL off (and resuming from it likewise).
        let runConfig = VMConfiguration.default(probe: probe)
        if bootedGPUTrial {
            serialLog += "[DroidNative] Snapshot unavailable in the isolated GPU trial (virgl is not migratable). Your main software g0 snapshot is still intact. VM not paused.\n"
            return
        }
        state = .snapshotting
        let dir = runConfig.snapshotDirectory
        do {
            try await backend.snapshot(to: dir)
            serialLog += "[DroidNative] ✅ Snapshot saved. Pausing the VM (resume it with the Resume button).\n"
            await backend.pause()
        } catch {
            // Log 12: the failure was swallowed by try? and pause ran anyway —
            // the button seemed to "do nothing" and the guest stayed
            // frozen without explanation. Now: no pause, with a visible reason.
            serialLog += "[DroidNative] ❌ Snapshot failed: \(error). The VM stays running (not paused). The QEMU detail is on the [dn-snap] line in the export.\n"
        }
        state = .running // on success it stays "alive" but frozen (QMP pause)
    }

    /// Resume after Snapshot+Pause (or any QMP pause): Log 12 showed
    /// that without a dedicated button, the frozen guest had no way back
    /// from the UI (auto-resume existed only for the background pause).
    func resumeVM() {
        serialLog += "[DroidNative] ▶️ VM resume requested.\n"
        Task { await backend.resume() }
    }

    /// Sends a line on the guest serial console (input bar in
    /// Console). Useful for diagnostics: on userdebug images, serial
    /// can offer a shell (Enter -> prompt), from where getprop/logcat work.
    func sendConsoleInput(_ line: String) {
        backend.sendSerialInput(line + "\n")
        serialLog += "[DroidNative] ⌨️ serial → \(line.isEmpty ? "(Enter)" : line)\n"
    }

    /// Types text into Android as real key events
    /// (virtio-keyboard, 0.2.19). Direct call into the C bridge —
    /// unlike serial (no shell on the GSI), THIS reaches the
    /// focused field in Android. Non-ASCII characters are skipped
    /// by the bridge (US map). "\n" = Enter.
    func sendKeyboardText(_ text: String) {
        dn_display_send_text(text)
        let shown = text.count > 40 ? String(text.prefix(40)) + "…" : text
        serialLog += "[DroidNative] ⌨️ keyboard → \(shown == "\n" ? "(Enter)" : shown)\n"
    }

    func requestStop() {
        backend.requestPowerDown()
        // QEMU does not restart in the same process: at the next boot in the same
        // session we show the relaunch hint (state == .stopped + flag, Phase 1).
    }

    func handleBackground() {
        // QMP pause only, no emergency snapshot: saving guest RAM
        // does not fit the iOS background window (~5 s), and a torn save at
        // halfway is worse than none. The snapshot stays a manual action.
        // During boot we do not pause at all (iOS freezes anyway
        // procesul la suspendare; la revenire, firele QEMU continua).
        guard state == .running, !pausedByBackground else { return }
        pausedByBackground = true
        serialLog += "[DroidNative] App going to background — pausing the VM (auto-resume on return).\n"
        // Breadcrumb mark (Log crash 3): iOS closes apps
        // suspended with a large footprint (3+ GB) — without line 18 in the report
        // of the previous session, the ending looked like a causeless crash.
        QEMUBackend.appendBreadcrumb(config: VMConfiguration.default(probe: probe),
                                     "18 backend: app going to background, VM paused (if nothing follows, iOS killed the suspended app)")
        persistConsoleNow()
        Task { await backend.pause() }
    }

    func handleForeground() {
        refreshProbe()
        if pausedByBackground {
            pausedByBackground = false
            serialLog += "[DroidNative] Back to foreground — resuming the VM.\n"
            QEMUBackend.appendBreadcrumb(config: VMConfiguration.default(probe: probe),
                                         "19 backend: back to foreground, VM resumed")
            Task { await backend.resume() }
        }
    }
}
