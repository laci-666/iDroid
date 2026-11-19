import Darwin
import Foundation

/// The real QEMU backend (Phase 1): dynamically loads
/// `qemu-aarch64-softmmu.framework` from the bundle (UTM model), runs
/// `qemu_init`/`qemu_main_loop`/`qemu_cleanup` on a dedicated thread via
/// the C bridge. Serial console and QMP go over socketpair (fd given to QEMU via
/// `-chardev socket,fd=N`); stdout/stderr QEMU + etapele de lansare se scriu
/// persistent in Guest (`qemu.log` + breadcrumb), ca un esec in qemu_init sa
/// lase dovada la relansare, nu doar o aplicatie inchisa.
///
/// The bundled engine can be the `ios-tci` build (interpreter; no generated code,
/// no JIT) or the `ios` build (native TCG; the code buffer is TXM-authorized
/// by QEMU itself, via brk 0x69 in qemu_init — docs/09). The flavor is read from
/// the marker written at staging into the framework (`DroidNativeEngine.txt`); with no
/// marker we assume the interpreter (compatible with the first bundled build).
final class QEMUBackend: VMBackend {

    enum EngineFlavor: String {
        case tci
        case tcg
    }

    static var engineFlavor: EngineFlavor {
        let marker = libraryURL.deletingLastPathComponent()
            .appendingPathComponent("DroidNativeEngine.txt")
        let text = (try? String(contentsOf: marker, encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return text == EngineFlavor.tcg.rawValue ? .tcg : .tci
    }

    static var bundledEngineIsInterpreterOnly: Bool { engineFlavor == .tci }

    /// The ANGLE frameworks epoxy (UTM fork) opens via dlopen
    /// la primul apel EGL: "EGL.framework/EGL" + "GLESv2.framework/GLESv2"
    /// (TARGET_OS_IPHONE in dispatch_common.c). If missing from the bundle,
    /// epoxy tipa „Couldn't open EGL.framework/EGL” si face abort() chiar in
    /// dnq_gl_early_init — the process dies with no treatable diagnostic (Log 15).
    /// Checked before any GL start; missing = software fallback.
    static var glFrameworksPresent: Bool {
        guard let frameworks = Bundle.main.privateFrameworksPath else { return false }
        let fm = FileManager.default
        return fm.fileExists(atPath: (frameworks as NSString).appendingPathComponent("EGL.framework/EGL"))
            && fm.fileExists(atPath: (frameworks as NSString).appendingPathComponent("GLESv2.framework/GLESv2"))
    }

    static var isAvailable: Bool {
        FileManager.default.fileExists(atPath: libraryURL.path)
    }

    private static var libraryURL: URL {
        Bundle.main.bundleURL.appendingPathComponent(
            "Frameworks/qemu-aarch64-softmmu.framework/qemu-aarch64-softmmu")
    }

    private var continuation: AsyncStream<VMEvent>.Continuation?
    private(set) lazy var events: AsyncStream<VMEvent> = AsyncStream { self.continuation = $0 }

    private let stateLock = NSLock()
    private var hasStarted = false
    private var qemuThread: Thread?
    private var activeConfig: VMConfiguration?

    // Serial console: host end of the socketpair (the guest gets
    // the other end via `-chardev socket,fd=N`; QEMU takes ownership of it).
    private let serialQueue = DispatchQueue(label: "ro.droidnative.qemu.serial", qos: .utility)
    private var serialSource: DispatchSourceRead?
    private var serialHostFD: Int32 = -1   // for input to the guest (accessed on serialQueue)

    // Redirectare stdout/stderr -> Guest/qemu.log in timpul rularii QEMU.
    private var savedStdoutFD: Int32 = -1
    private var savedStderrFD: Int32 = -1
    private var logRedirectInstalled = false

    // QMP: capatul gazda al socketpair-ului (`-mon chardev=dnqmp0,mode=control`).
    private let qmpQueue = DispatchQueue(label: "ro.droidnative.qemu.qmp", qos: .utility)
    private var qmpFD: Int32 = -1
    private var qmpSource: DispatchSourceRead?
    private var qmpBuffer = Data()
    private var qmpReady = false
    private var qmpAnnouncedDisplay = false
    private var qmpNextID = 1
    private var qmpPending: [Int: CheckedContinuation<Void, Error>] = [:]

    /// Loads the QEMU library (dlopen + dlsym) BEFORE the JIT attach.
    /// The window in which the scripted debugger waits for the brk in qemu_init
    /// must not be spent on the cold dlopen of the ~25 frameworks
    /// (on first launch after install it can take seconds; observed
    /// 21.10.2026: atasare OK + sonda OK, dar mmap-ul din qemu_init gasea
    /// dead authorization). dn_qemu_load is idempotent — start() retakes it
    /// at no cost. Returns a short message for the console.
    static func prewarmLibrary() -> String {
        guard isAvailable else { return "QEMU library missing from the bundle" }
        var errorPointer: UnsafeMutablePointer<CChar>?
        let loaded = dn_qemu_load(libraryURL.path, &errorPointer)
        let message = errorPointer.map { String(cString: $0) }
        if let errorPointer {
            dn_qemu_free_error(errorPointer)
        }
        return loaded ? "OK" : "FAILED: \(message ?? "no message")"
    }

    func start(config: VMConfiguration, accelerator: Accelerator, arena: JITArena?) async throws {
        stateLock.lock()
        let alreadyStarted = hasStarted
        stateLock.unlock()
        if alreadyStarted {
            throw QEMUBackendError.alreadyStarted
        }
        guard Self.isAvailable else {
            throw QEMUBackendError.libraryMissing(Self.libraryURL.path)
        }
        // Defense in depth: real-time MTTCG is ONLY permitted on the APFS
        // clone for this EXACT CPU shape. In particular, count 3/4 cannot
        // accidentally boot on the proven 2-vCPU disk if a future caller
        // constructs a VMConfiguration without the guarded clone builder.
        // Valid pairs: (normal, icount ON) and (parallel clone, icount OFF).
        let parallelFolder = config.cpuCount == 2 ? "ParallelGPUTrial" : "ParallelGPUTrial\(config.cpuCount)"
        let parallelRoot = VMConfiguration.guestDirectory.appendingPathComponent(parallelFolder,
                                                                                 isDirectory: true)
        let parallelDisksMatch =
            config.systemOverlayPath == parallelRoot.appendingPathComponent("system-overlay.qcow2").path &&
            config.userDiskPath == parallelRoot.appendingPathComponent("userdata.qcow2").path &&
            config.firmwareVarsPath == parallelRoot.appendingPathComponent("efi_vars.fd").path &&
            config.snapshotDirectory == parallelRoot.appendingPathComponent("snapshots").path
        if config.guestKind == .android &&
            (config.isParallelTrial == config.useIcount ||
             (config.isParallelTrial &&
              (!config.isGPUTrial || !config.gpuAcceleration || config.loadSnapshot ||
               !(2...4).contains(config.cpuCount) || !parallelDisksMatch ||
               accelerator != .tcgJIT || Self.bundledEngineIsInterpreterOnly))) {
            throw QEMUBackendError.invalidParallelTrialConfiguration
        }

        // Diagnostic persistent de la prima linie: daca procesul moare in
        // dlopen sau qemu_init, relansarea arata ultima etapa + qemu.log.
        try FileManager.default.createDirectory(at: VMConfiguration.guestDirectory, withIntermediateDirectories: true)
        Self.resetDiagnostics(config: config)
        Self.appendBreadcrumb(config: config, "01 backend: start guest=\(config.guestKind) accelerator=\(accelerator)")
        Self.appendBreadcrumb(config: config, "02 backend: before dlopen \(Self.libraryURL.path)")

        // dlopen + dlsym before anything else: if framework packaging
        // is incomplete, the failure must be here, with a clear message, not a crash.
        var errorPointer: UnsafeMutablePointer<CChar>?
        let loaded = dn_qemu_load(Self.libraryURL.path, &errorPointer)
        let loadMessage = errorPointer.map { String(cString: $0) }
        if let errorPointer {
            dn_qemu_free_error(errorPointer)
        }
        guard loaded else {
            Self.appendBreadcrumb(config: config, "99 backend: dlopen failed (handled): \(loadMessage ?? "no message")")
            throw QEMUBackendError.libraryLoadFailed(loadMessage ?? "dlopen failed with no message")
        }
        Self.appendBreadcrumb(config: config, "03 backend: dlopen + dlsym OK")

        // Guest display density + size (0.2.25): announced to the
        // display bridge right after load, before QEMU runs;
        // virtio-gpu turns the physical size (mm derived from DPI)
        // into the guest EDID, and Android derives its UI density
        // from that. Without it the EDID claims 100 DPI and
        // Android text renders tiny at native resolution.
        dn_display_set_guest_dpi(Int32(config.guestDPI))
        dn_display_set_guest_refresh_rate(Int32(config.guestRefreshRateHz))
        dn_display_set_ui_size(Int32(config.displayWidth), Int32(config.displayHeight))
        emit("[DroidNative] Guest EDID preferred refresh requested: \(config.guestRefreshRateHz) Hz (75 default / 55 optional A/B; not iPhone FPS).\n")

        // The Android disks must exist BEFORE qemu_init: a -drive
        // to a missing file kills the process in qemu_init (error_exit), much
        // uglier than a handled error here. The work disk is created at
        // first boot by cloning the golden one (see the dedicated function).
        do {
            try Self.ensureAndroidWorkDisks(config: config)
        } catch {
            Self.appendBreadcrumb(config: config, "99 backend: disk preparation failed (handled): \(error.localizedDescription)")
            throw error
        }
        if config.isGPUTrial && (!config.gpuAcceleration || !Self.glFrameworksPresent) {
            Self.appendBreadcrumb(config: config, "99 backend: isolated GPU trial refused (ANGLE missing/GL disabled)")
            throw QEMUBackendError.libraryLoadFailed(
                "isolated GPU trial needs bundled EGL.framework and GLESv2.framework; no software boot on trial disks")
        }

        stateLock.lock()
        if hasStarted {
            stateLock.unlock()
            throw QEMUBackendError.alreadyStarted
        }
        hasStarted = true
        stateLock.unlock()

        activeConfig = config

        // Etapa TCI: fortam interpretorul indiferent de proba JIT. Arena TXM
        // stays for the TCG build in the next stage; here we do not consume
        // 384 MB for nothing on an engine that generates no code.
        let effectiveAccelerator: Accelerator = Self.bundledEngineIsInterpreterOnly ? .tcgTCTI : accelerator

        // Defense in depth: a TCG engine started in interpreter mode would
        // genera tot cod nativ (aroma e din build, nu din argv) si ar muri la
        // first execution from the TXM-unauthorized buffer. VMController stops
        // this case already; here we categorically refuse the combination.
        if Self.engineFlavor == .tcg, effectiveAccelerator == .tcgTCTI {
            Self.appendBreadcrumb(config: config, "99 backend: TCG engine requested without JIT (handled)")
            throw QEMUBackendError.jitRequiredForTCGEngine
        }

        // Under the interpreter (TCI) vCPUs have no real parallel execution
        // (no MTTCG): beyond 2 vCPUs we pay context-switch and spin with no gain
        // and UI starvation. Cap them at 2 while on TCI; the TCG-JIT
        // build automatically returns to the configured cpuCount.
        var effectiveConfig = config
        if Self.bundledEngineIsInterpreterOnly, effectiveConfig.cpuCount > 2 {
            effectiveConfig.cpuCount = 2
            Self.appendBreadcrumb(config: config, "04a backend: TCI — vCPU plafonate la 2")
        }

        // GL preflight (Log 15): with virtio-gpu-gl-pci in argv, epoxy opens
        // EGL.framework via dlopen at dnq_gl_early_init; missing framework =
        // abort() in our process, no chance of recovery. If
        // ANGLE nu e in bundle, coboram SINGURI pe randare software si spunem
        // exactly which command repairs the packaging — boot succeeds anyway.
        if effectiveConfig.gpuAcceleration, !Self.glFrameworksPresent {
            effectiveConfig.gpuAcceleration = false
            Self.appendBreadcrumb(config: config, "04g backend: GL requested but EGL/GLESv2 missing from the bundle — software rendering fallback")
            emit("[DroidNative] GL acceleration is on in settings, but the ANGLE frameworks (EGL.framework/GLESv2.framework) are missing from the app. Starting with software rendering. Fix on the Mac (no QEMU rebuild): DN_ENGINE=tcg ./scripts/stage-qemu-frameworks.sh, then Run over the existing app.\n")
        }
        activeConfig = effectiveConfig
        // No extra vCPUs or changed Android disks. Only the user's proven
        // 2-vCPU MTTCG/EAGL APFS clone can register four BGRA IOSurfaces.
        // Old QEMU/EGL, resize, or Metal texture failure stays on PBO.
        let ioRequested = effectiveConfig.guestKind == .android &&
            effectiveConfig.isParallelTrial && effectiveConfig.cpuCount == 2 &&
            effectiveConfig.gpuAcceleration && effectiveConfig.enableIOSurfaceFastPath &&
            effectiveConfig.displayWidth > 0 && effectiveConfig.displayHeight > 0 &&
            effectiveConfig.displayWidth * effectiveConfig.displayHeight <= 720 * 1280
        dn_display_configure_iosurface(ioRequested,
                                       Int32(effectiveConfig.displayWidth),
                                       Int32(effectiveConfig.displayHeight))
        emit("[DroidNative] EAGL IOSurface presenter: \(ioRequested ? "requested for 2-vCPU GPU clone (runtime-checked, PBO fallback)" : "PBO/legacy path"). Android 60/120 FPS unverified.\n")

        // Console/control channels: socketpair. The "guest" end reaches
        // QEMU ca numar de fd in argv si ii apartine de la qemu_init incolo —
        // nu-l inchidem noi niciodata.
        var serialPair: (host: Int32, guest: Int32)?
        var qmpPair: (host: Int32, guest: Int32)?
        do {
            serialPair = try Self.makeSocketPair()
            qmpPair = try Self.makeSocketPair()
        } catch {
            if let serialPair {
                close(serialPair.host)
                close(serialPair.guest)
            }
            Self.appendBreadcrumb(config: config, "99 backend: socketpair failed (handled): \(error.localizedDescription)")
            throw error
        }
        guard let serialPair, let qmpPair else {
            throw QEMUBackendError.socketPairFailed("socketpair nu a produs fd-uri")
        }
        Self.appendBreadcrumb(config: config, "04 backend: socketpair serial host=\(serialPair.host) guest=\(serialPair.guest); qmp host=\(qmpPair.host) guest=\(qmpPair.guest)")

        let arguments = QEMUArgumentsBuilder.build(config: effectiveConfig,
                                                   accelerator: effectiveAccelerator,
                                                   serialFD: serialPair.guest,
                                                   qmpFD: qmpPair.guest)

        setenv("TMPDIR", NSTemporaryDirectory(), 1)
        setenv("HOME", NSHomeDirectory(), 1)

        emit("[DroidNative] QEMU engine loaded from the bundle. Starting the VM (\(effectiveAccelerator))...\n")
        emit("[DroidNative] " + arguments.joined(separator: " \\\n  ") + "\n")

        installQEMULog(config: config, arguments: arguments)
        Self.appendBreadcrumb(config: config, "05 backend: qemu.log prepared, serial/QMP sources attached")
        attachSerialSource(fd: serialPair.host)
        attachQMPHostSocket(fd: qmpPair.host, config: config)

        Self.appendBreadcrumb(config: config, "06 backend: before thread.start")
        let joinedArguments = arguments.joined(separator: "\u{01}")
        let breadcrumbPath = config.qemuBreadcrumbPath
        let loadSnapshot = config.loadSnapshot
        // Phase B (docs/12): the GL signal for shim + QEMU is an env read
        // before qemu_init (the EGL/ANGLE display must exist
        // before virtio-gpu-gl realizes). Set/clear it
        // explicitly at every start, from the current config.
        if effectiveConfig.gpuAcceleration {
            setenv("DN_GL", "1", 1)
            if effectiveConfig.isGPUTrial { setenv("DN_GPU_TRIAL", "1", 1) }
            else { unsetenv("DN_GPU_TRIAL") }
            // UTM's ANGLE (OpenGL) choice UNSETS this variable. Metal is
            // documented by the LineageOS maintainers as black after boot
            // for this VM. QEMUBridge repeats the unset before EGL init.
            unsetenv("ANGLE_DEFAULT_PLATFORM")
        } else {
            unsetenv("DN_GL")
            unsetenv("DN_GPU_TRIAL")
        }
        let thread = Thread { [weak self] in
            // JIT authorization state right before qemu_init: if the process
            // moare in alocarea bufferului TCG (EPERM + exit), linia asta spune
            // whether the debugger was still attached that second.
            Self.appendBreadcrumb(config: config, "07b backend: in firul QEMU — traced=\(dn_is_traced()) CS_DEBUGGED=\(dn_is_debugged())")
            // LATE probe (sister of VMController's): between the JIT gate and
            // this thread passes dlopen + seconds of preparation, during which
            // the debugger can detach. If authorization is dead HERE, we do not
            // enter qemu_init (it would exit() and the app would
            // "disappears") — stop in a controlled way, with the reason in the console.
            if accelerator == .tcgJIT {
                // At the REAL buffer size: a page can pass where
                // 384 MB primesc EPERM (cota TXM pe marime, 21.10.2026).
                let probeErrno = dn_jit_rx_mmap_probe_size(config.jitArenaMegabytes * 1024 * 1024)
                Self.appendBreadcrumb(config: config, "07c backend: sonda RX-mmap \(config.jitArenaMegabytes)MB errno=\(probeErrno) (0=OK) traced=\(dn_is_traced())")
                if probeErrno != 0 {
                    self?.emit("[DroidNative] JIT authorization lost right before qemu_init (RX-mmap probe \(config.jitArenaMegabytes) MB, errno=\(probeErrno), traced=\(dn_is_traced())). NOT entering QEMU — it would exit instantly at buffer allocation. Retry the start; if it repeats: Capabilities → \"Prepare built-in JIT (DDI)\", app launched from its icon.\n")
                    self?.handleExit(status: 126)
                    return
                }
            }
            let status = dn_qemu_run_ex(joinedArguments, breadcrumbPath, loadSnapshot)
            if status == -1 && config.isGPUTrial {
                self?.emit("[DroidNative] Isolated GPU trial refused before Android boot: ANGLE OpenGL/EAGL ES3 was unavailable. Software profile and g0 snapshot unchanged. Check qemu.log and qemu-breadcrumbs.txt; next start uses software by default.\n")
            }
            self?.handleExit(status: status)
        }
        thread.name = "ro.droidnative.qemu.main"
        thread.stackSize = 16 * 1024 * 1024
        // .userInteractive: when the VM runs (most of the time
        // fullscreen), the TCG/vCPU threads ARE the interface — the host has no other
        // heavy work (texture upload only). Maximum priority
        // reduce si sansele ca iOS sa le coboare pe nuclee E la boot.
        thread.qualityOfService = .userInteractive
        qemuThread = thread
        thread.start()
        Self.appendBreadcrumb(config: config, "07 backend: QEMU thread started")

        // Pulsul atasarii: esantioneaza P_TRACED la 100 ms si noteaza in
        // breadcrumb ONLY the transitions (with time since thread start).
        // If the legacy script detaches cleanly after the brk, the transition is
        // the authorization verdict; if it detaches BEFORE the QEMU thread
        // sa apuce qemu_init (si procesul moare la mmap), ramane dovada cu
        // timestamp on disk. Cheap thread, stops itself.
        if accelerator == .tcgJIT {
            let pulseConfig = config
            let pulse = Thread {
                let t0 = Date()
                var last = dn_is_traced()
                Self.appendBreadcrumb(config: pulseConfig, "07p backend: puls initial traced=\(last)")
                while Date().timeIntervalSince(t0) < 90 {
                    Thread.sleep(forTimeInterval: 0.1)
                    let now = dn_is_traced()
                    if now != last {
                        let dt = String(format: "%.1f", Date().timeIntervalSince(t0))
                        Self.appendBreadcrumb(config: pulseConfig, "07p backend: traced \(last)->\(now) at +\(dt)s since the QEMU thread start")
                        last = now
                        if !now { break }
                    }
                }
            }
            pulse.name = "ro.droidnative.trace-pulse"
            pulse.start()
        }

        // Memory telemetry (Log crash 1/2): the process footprint in
        // breadcrumb. Daca procesul moare mut si ultimele linii 07m urca
        // toward the limit, the verdict is jetsam; if it dies with a small footprint +
        // line 99 crash-trap, it is a native fault. Cadence is dense at
        // the start (the Log crash 2 death, at ~25 s, slipped between the first
        // 30 s measurement and start): 3 s, then 5 s up to a minute,
        // apoi 30 s.
        let footprintConfig = config
        let watchedThread = thread
        let footprint = Thread {
            let t0 = Date()
            let cadence: [Double] = [3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]
            var stepIndex = 0
            while Date().timeIntervalSince(t0) < 7200 {
                let step = stepIndex < cadence.count ? cadence[stepIndex] : 30
                stepIndex += 1
                Thread.sleep(forTimeInterval: step)
                if watchedThread.isFinished { break }
                let mb = Self.currentFootprintMB()
                let dt = String(format: "%.0f", Date().timeIntervalSince(t0))
                if mb > 0 {
                    Self.appendBreadcrumb(config: footprintConfig,
                                          "07m backend: footprint \(mb) MB at +\(dt)s since the QEMU thread start")
                }
            }
        }
        footprint.name = "ro.droidnative.footprint"
        footprint.start()
    }

    /// The process physical memory footprint (phys_footprint, the measure
    /// jetsam uses), in MB; -1 if the query fails.
    static func currentFootprintMB() -> Int {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(
            MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) { ptr in
            ptr.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { intPtr in
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), intPtr, &count)
            }
        }
        guard kr == KERN_SUCCESS else { return -1 }
        return Int(info.phys_footprint / (1024 * 1024))
    }

    func pause() async {
        try? await sendQMP(execute: "stop")
    }

    func resume() async {
        try? await sendQMP(execute: "cont")
    }

    func snapshot(to directory: String) async throws {
        guard activeConfig?.isGPUTrial != true else {
            // virgl is non-migratable; the software g0 snapshot belongs to
            // the untouched original userdata, never to this trial clone.
            throw QEMUBackendError.snapshotUnsupported
        }
        // Internal `dn-ready` snapshot in userdata.qcow2, via the bridge compiled
        // in QEMU (app/QEMUExtra/dn-snapshot.c). `directory` is ignored:
        // VM state does not live in a separate file. The save is asynchronous
        // in QEMU (bottom half); we follow it by state polling.
        _ = directory
        guard dn_snapshot_state() >= 0 else {
            throw QEMUBackendError.snapshotUnsupported
        }
        if let config = activeConfig {
            Self.appendBreadcrumb(config: config, "17 backend: dn-ready snapshot save requested")
        }
        dn_snapshot_save()
        // Guest RAM (up to ~4 GB) is written to disk with TCG stopped; can
        // dura minute. Timeout generos, dar nu infinit.
        for _ in 0..<(20 * 60 * 2) {           // ~20 min la 500 ms
            switch dn_snapshot_state() {
            case 2:
                if let config = activeConfig {
                    Self.appendBreadcrumb(config: config, "18 backend: dn-ready snapshot saved OK")
                    // Remember the save mode: the guard in VMController.boot
                    // compares the flag with the start mode (the snapshot is
                    // valid only in the mode it was saved in).
                    // Full machine shape at save (i=icount,
                    // g=GL acceleration): GPU type and vCPU count enter
                    // the saved state, so resume requires the same shape.
                    // Forma include si RAM-ul guest (0.2.8): o reluare cu
                    // a different -m would restore a state with another memory map.
                    // Plus resolution + the "d2" device set (0.2.19:
                    // virtio keyboard + native resolution): the saved state
                    // of virtio-gpu/devices requires the same machine.
                    let shape = "i\(config.useIcount ? 1 : 0)g\(config.gpuAcceleration ? 1 : 0)m\(config.memoryMegabytes)r\(config.displayWidth)x\(config.displayHeight)d2"
                    UserDefaults.standard.set(shape, forKey: "dnSnapshotShape")
                    UserDefaults.standard.set(config.useIcount, forKey: "dnSnapshotIcount")
                }
                return
            case 3:
                throw QEMUBackendError.snapshotUnsupported
            default:
                try? await Task.sleep(for: .milliseconds(500))
            }
        }
        throw QEMUBackendError.snapshotUnsupported
    }

    func requestPowerDown() {
        Task { try? await sendQMP(execute: "system_powerdown") }
    }

    // MARK: - Iesire QEMU

    private func handleExit(status: Int32) {
        serialSource?.cancel()
        qmpQueue.async { [weak self] in
            self?.closeQMPOnQueue(failPending: true)
        }
        restoreQEMULog()
        if let config = activeConfig {
            Self.appendBreadcrumb(config: config, "16 backend: fir QEMU incheiat, status=\(status)")
        }
        emit("[DroidNative] QEMU stopped (status \(status)). Relaunch the app for a new start.\n")
        continuation?.yield(.exited(code: status))
    }

    private func emit(_ text: String) {
        continuation?.yield(.serialOutput(text))
    }

    // MARK: - Serial console (socketpair, host end)

    private func attachSerialSource(fd: Int32) {
        serialHostFD = fd
        Self.setNonblocking(fd)
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: serialQueue)
        source.setEventHandler { [weak self] in
            var buffer = [UInt8](repeating: 0, count: 8192)
            let count = read(fd, &buffer, buffer.count)
            if count > 0 {
                let data = Data(buffer.prefix(count))
                let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) ?? ""
                if !text.isEmpty {
                    self?.emit(text)
                }
            } else if count == 0 {
                source.cancel() // QEMU a inchis capatul lui
            } else if errno != EAGAIN && errno != EINTR {
                source.cancel()
            }
        }
        source.setCancelHandler { [weak self] in
            if self?.serialHostFD == fd { self?.serialHostFD = -1 }
            close(fd)
        }
        serialSource = source
        source.resume()
    }

    /// Input to the guest console (diagnostics: serial shell, if
    /// the userdebug image offers one). Full write, on serialQueue.
    func sendSerialInput(_ text: String) {
        let data = Data(text.utf8)
        serialQueue.async { [weak self] in
            guard let self, self.serialHostFD >= 0 else { return }
            let fd = self.serialHostFD
            data.withUnsafeBytes { raw in
                guard let base = raw.baseAddress else { return }
                var offset = 0
                while offset < data.count {
                    let written = write(fd, base.advanced(by: offset), data.count - offset)
                    if written > 0 {
                        offset += written
                    } else if written < 0 && errno == EINTR {
                        continue
                    } else if written < 0 && errno == EAGAIN {
                        // Full buffer toward a guest that is not reading: we drop
                        // la rest decat sa blocam coada de citire.
                        break
                    } else {
                        break
                    }
                }
            }
        }
    }

    // MARK: - QMP (socketpair, capatul gazda)

    private func attachQMPHostSocket(fd: Int32, config: VMConfiguration) {
        qmpQueue.sync {
            qmpFD = fd
            activeConfig = config
            let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: qmpQueue)
            source.setEventHandler { [weak self] in
                self?.readAvailableQMPData()
            }
            source.setCancelHandler {
                close(fd)
            }
            qmpSource = source
            source.resume()
        }
    }

    /// Ensures the Android work disks before qemu_init.
    ///
    /// Golden/work split (reset = delete the copy, golden stays untouched)
    /// was designed on a QMP-created qcow2 overlay — never wired.
    /// The replacement: at first boot we clone the golden qcow2 file into the work
    /// disk. The copy goes via temp file + rename, with size
    /// de dimensiune: o copiere intrerupta (app suspendata/omorata) nu lasa in
    /// behind a truncated "disk" the next boot would consider valid.
    /// Runs on the backend's async executor, not on MainActor.
    private static func ensureAndroidWorkDisks(config: VMConfiguration) throws {
        guard config.guestKind == .android else { return }
        let fm = FileManager.default
        if config.isGPUTrial {
            // Storage was cloned atomically by VMConfiguration. In a GPU
            // trial, do not prune archives, repair the golden template, or
            // create/resize ANY original software-profile work disk here.
            for path in [config.systemOverlayPath, config.userDiskPath,
                         config.firmwareVarsPath] {
                guard let values = try? URL(fileURLWithPath: path).resourceValues(
                          forKeys: [.isRegularFileKey, .isSymbolicLinkKey]),
                      values.isRegularFile == true, values.isSymbolicLink != true,
                      let attributes = try? fm.attributesOfItem(atPath: path),
                      attributes[.type] as? FileAttributeType == FileAttributeType.typeRegular,
                      let size = attributes[.size] as? NSNumber, size.uint64Value > 0 else {
                    throw QEMUBackendError.diskPreparationFailed(
                        "isolated GPU trial disk missing/invalid: \(path). Original software VM unchanged.")
                }
            }
            appendBreadcrumb(config: config, "03 backend: isolated GPU trial uses only APFS clones (no original work-disk writes)")
            return
        }

        // After install, the UTM-VM archive (~1.1 GB) is no longer needed: the golden
        // and templates are the installed artifacts. Free it before
        // clonari/formatare, unde fiecare GB conteaza.
        let freedCache = pruneDownloadedArchivesIfInstalled(config: config)
        if freedCache > 0 {
            appendBreadcrumb(config: config, "03 backend: freed the Lineage cache archive: \(formatBytes(freedCache))")
        }
        // Instalari extrase cu vechiul bug DEFLATE au sablonul/userdata qcow2
        // truncated at 131072 bytes (missing the refcount block). Repair locally,
        // strictly for empty images, so boot does not ask for a re-download.
        repairTruncatedEmptyQCow2IfNeeded(path: config.goldenUserDiskPath, config: config, what: "sablonul userdata")
        repairTruncatedEmptyQCow2IfNeeded(path: config.userDiskPath, config: config, what: "userdata de lucru")
        appendBreadcrumb(config: config, "03 backend: storage/disks: \(storageDiagnostics(config: config))")

        // work userdata missing (first start or after factory reset):
        // re-clone it from the package template (vdb kept at setup).
        // Only if the template is missing too (old install, before it)
        // do we ask for one re-setup — once, with no re-download.
        if !fm.fileExists(atPath: config.userDiskPath) {
            guard fm.fileExists(atPath: config.goldenUserDiskPath) else {
                throw QEMUBackendError.diskPreparationFailed(
                    "the userdata disk and its template are missing (\(URL(fileURLWithPath: config.goldenUserDiskPath).lastPathComponent)). Old install: run the automatic setup once from the Setup tab (it does not re-download the archive, only restores the template).")
            }
            appendBreadcrumb(config: config, "03a backend: clonez userdata de lucru din sablon")
            try cloneDiskFile(from: config.goldenUserDiskPath, to: config.userDiskPath, what: "userdata")
            capFreshUserdataDiskIfNeeded(config: config)
            appendBreadcrumb(config: config, "03a backend: userdata de lucru creata din sablon")
        }

        if fm.fileExists(atPath: config.systemOverlayPath) {
            appendBreadcrumb(config: config, "03b backend: Android work disk already present")
            return
        }
        guard fm.fileExists(atPath: config.goldenSystemDiskPath) else {
            throw QEMUBackendError.diskPreparationFailed(
                "the base image is missing (\(URL(fileURLWithPath: config.goldenSystemDiskPath).lastPathComponent)). Run the automatic setup from the Setup tab.")
        }

        appendBreadcrumb(config: config, "03b backend: cloning the work disk from golden (first Android boot)")
        try cloneDiskFile(from: config.goldenSystemDiskPath, to: config.systemOverlayPath, what: "the work disk")
        appendBreadcrumb(config: config, "03c backend: Android work disk created from golden")
    }

    /// Clones a qcow2 disk file via temp copy + rename, with
    /// size verification: an interrupted copy does not leave behind a
    /// truncated "disk" the next boot would consider valid.
    private static func cloneDiskFile(from sourcePath: String, to destPath: String, what: String) throws {
        let fm = FileManager.default
        let source = URL(fileURLWithPath: sourcePath)
        let dest = URL(fileURLWithPath: destPath)
        let staging = dest.deletingLastPathComponent()
            .appendingPathComponent("clone-\(UUID().uuidString).qcow2")
        do {
            try fm.copyItem(at: source, to: staging)
            let sourceSize = (try fm.attributesOfItem(atPath: source.path)[.size] as? NSNumber)?.int64Value ?? -1
            let stagedSize = (try fm.attributesOfItem(atPath: staging.path)[.size] as? NSNumber)?.int64Value ?? -2
            guard sourceSize == stagedSize else {
                throw QEMUBackendError.diskPreparationFailed(
                    "cloning \(what) stopped at \(stagedSize) of \(sourceSize) bytes (insufficient space?). Free space and retry.")
            }
            try fm.moveItem(at: staging, to: dest)
        } catch let error as QEMUBackendError {
            try? fm.removeItem(at: staging)
            throw error
        } catch {
            try? fm.removeItem(at: staging)
            throw QEMUBackendError.diskPreparationFailed("cloning \(what) failed: \(error.localizedDescription)")
        }
    }

    // MARK: - Android disk diagnostics + hygiene

    /// Cap for userdata in bring-up. The upstream template (`empty-16G.qcow2`)
    /// has 16 GiB virtual; on device, /data formatting died with Buffer
    /// I/O in jurul jurnalului ext4 de la ~8 GiB. Clonele proaspete le limitam
    /// at 8 GiB virtual: enough for internal storage at this stage,
    /// half the formatting extent and no offset toward 16 GiB.
    private static let androidUserdataVirtualSizeCapBytes: UInt64 = 8 * 1024 * 1024 * 1024

    static func storageDiagnostics(config: VMConfiguration) -> String {
        func disk(_ label: String, _ path: String) -> String {
            guard let physical = fileSizeBytes(path: path) else { return "\(label)=missing" }
            let virtual = qcow2VirtualSizeBytes(path: path).map { formatBytes($0) } ?? "?"
            return "\(label) physical \(formatBytes(physical))/virtual \(virtual)"
        }
        let free = freeDiskBytes()
        let freeText = free > 0 ? formatBytes(free) : "unknown"
        let warn = (free > 0 && free < 2 * 1024 * 1024 * 1024) ? " (WARNING: low free space)" : ""
        return "free \(freeText)\(warn); " +
            "\(disk("system-work", config.systemOverlayPath)); " +
            "\(disk("userdata-work", config.userDiskPath)); " +
            "\(disk("userdata-template", config.goldenUserDiskPath))"
    }

    private static func pruneDownloadedArchivesIfInstalled(config: VMConfiguration) -> UInt64 {
        let fm = FileManager.default
        guard fm.fileExists(atPath: config.goldenSystemDiskPath),
              fm.fileExists(atPath: config.goldenUserDiskPath) else {
            return 0
        }
        let downloads = VMConfiguration.guestDirectory.appendingPathComponent("downloads", isDirectory: true)
        guard let items = try? fm.contentsOfDirectory(at: downloads, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles]) else {
            return 0
        }
        var freed: UInt64 = 0
        for item in items where item.pathExtension.lowercased() == "zip" {
            let size = fileSizeBytes(path: item.path) ?? 0
            do {
                try fm.removeItem(at: item)
                freed += size
            } catch {
                // Internal cache: if it cannot be deleted now, boot continues.
            }
        }
        return freed
    }

    /// Limiteaza o userdata PROASPAT clonata din sablonul gol la plafonul de
    /// above. Never touches an existing disk with data: called only
    /// immediately after cloning the template and refuses any image with backing,
    /// criptare qcow2, feature-uri incompatibile sau intrari L1 alocate peste
    /// plafon.
    private static func capFreshUserdataDiskIfNeeded(config: VMConfiguration) {
        let path = config.userDiskPath
        do {
            guard let header = readFileRange(path: path, offset: 0, count: 112), header.count >= 112,
                  header[0] == 0x51, header[1] == 0x46, header[2] == 0x49, header[3] == 0xfb,
                  let version = readUInt32BE(header, offset: 4), version == 2 || version == 3,
                  let backingOffset = readUInt64BE(header, offset: 8), backingOffset == 0,
                  let clusterBits = readUInt32BE(header, offset: 20), (9...21).contains(clusterBits),
                  let currentSize = readUInt64BE(header, offset: 24),
                  let cryptMethod = readUInt32BE(header, offset: 32), cryptMethod == 0,
                  let l1Size = readUInt32BE(header, offset: 36),
                  let l1TableOffset = readUInt64BE(header, offset: 40),
                  (version == 2 || (readUInt64BE(header, offset: 72) ?? 1) == 0) else {
                appendBreadcrumb(config: config, "03a backend: nu plafonez userdata (header qcow2 necunoscut/precautie)")
                return
            }

            let target = androidUserdataVirtualSizeCapBytes
            guard currentSize > target else { return }

            let clusterSize = UInt64(1) << UInt64(clusterBits)
            let entriesPerL2 = Swift.max(UInt64(1), clusterSize / 8)
            let guestClusters = (target + clusterSize - 1) / clusterSize
            let neededL1Entries = (guestClusters + entriesPerL2 - 1) / entriesPerL2
            guard neededL1Entries <= UInt64(l1Size), neededL1Entries <= 8192 else {
                appendBreadcrumb(config: config, "03a backend: nu plafonez userdata (L1 insuficient/neobisnuit)")
                return
            }

            if UInt64(l1Size) > neededL1Entries {
                let tailOffset = l1TableOffset + neededL1Entries * 8
                let tailCount = Int((UInt64(l1Size) - neededL1Entries) * 8)
                if let tail = readFileRange(path: path, offset: tailOffset, count: tailCount),
                   tail.contains(where: { $0 != 0 }) {
                    appendBreadcrumb(config: config, "03a backend: not capping userdata (template not empty above the cap)")
                    return
                }
            }

            let handle = try FileHandle(forUpdating: URL(fileURLWithPath: path))
            defer { try? handle.close() }
            try handle.seek(toOffset: 24)
            try handle.write(contentsOf: dataBE64(target))
            try handle.seek(toOffset: 36)
            try handle.write(contentsOf: dataBE32(UInt32(neededL1Entries)))
            try handle.synchronize()
            appendBreadcrumb(config: config,
                             "03a backend: userdata proaspata plafonata la \(formatBytes(target)) virtual (sablon \(formatBytes(currentSize)))")
        } catch {
            appendBreadcrumb(config: config,
                             "03a backend: userdata capping failed (\(error.localizedDescription)); continuing with the template")
        }
    }

    /// Repairs locally an *empty* qcow2 image truncated by the old extraction
    /// bug (the refcount block is missing; QEMU marks it "corrupt" at the first
    /// allocation). Conditions intentionally strict: no backing/encryption/
    /// snapshots, empty L1, no data clusters; outside this case we do not
    /// touch the file. Rebuilds exactly the missing bytes: extends the file
    /// to the end of the L1 entries and writes refcount=1 for the clusters of
    /// metadata (header, tabela refcount, blocul refcount, tabela L1).
    private static func repairTruncatedEmptyQCow2IfNeeded(path: String, config: VMConfiguration, what: String) {
        guard FileManager.default.fileExists(atPath: path),
              let fileSize = fileSizeBytes(path: path) else {
            return
        }
        do {
            guard let header = readFileRange(path: path, offset: 0, count: 112), header.count >= 112,
                  header[0] == 0x51, header[1] == 0x46, header[2] == 0x49, header[3] == 0xfb,
                  let version = readUInt32BE(header, offset: 4), version == 3,
                  let backingOffset = readUInt64BE(header, offset: 8), backingOffset == 0,
                  let clusterBits = readUInt32BE(header, offset: 20), (9...21).contains(clusterBits),
                  let cryptMethod = readUInt32BE(header, offset: 32), cryptMethod == 0,
                  let l1Size = readUInt32BE(header, offset: 36), l1Size > 0, l1Size <= 8192,
                  let l1TableOffset = readUInt64BE(header, offset: 40),
                  let refcountTableOffset = readUInt64BE(header, offset: 48),
                  let refcountTableClusters = readUInt32BE(header, offset: 56), refcountTableClusters == 1,
                  let snapshots = readUInt32BE(header, offset: 60), snapshots == 0,
                  let incompatible = readUInt64BE(header, offset: 72), (incompatible & ~UInt64(0b11)) == 0,
                  let refcountOrder = readUInt32BE(header, offset: 96), refcountOrder == 4 else {
                return
            }

            let clusterSize = UInt64(1) << UInt64(clusterBits)
            guard clusterSize > 0,
                  refcountTableOffset % clusterSize == 0,
                  l1TableOffset % clusterSize == 0 else {
                return
            }
            let minimalFileSize = l1TableOffset + UInt64(l1Size) * 8

            guard let tableEntryBytes = readFileRange(path: path, offset: refcountTableOffset, count: 8),
                  tableEntryBytes.count == 8,
                  let refcountBlockOffset = readUInt64BE(tableEntryBytes, offset: 0),
                  refcountBlockOffset != 0,
                  refcountBlockOffset % clusterSize == 0 else {
                return
            }

            let refcountBlockHead = readFileRange(path: path, offset: refcountBlockOffset, count: 8) ?? []
            let refcountBlockMissingOrZero = refcountBlockHead.count < 8 || refcountBlockHead.allSatisfy { $0 == 0 }
            guard fileSize <= minimalFileSize,
                  fileSize < minimalFileSize || refcountBlockMissingOrZero else {
                return
            }

            // Any present L1 byte must be zero: the image is empty.
            if fileSize > l1TableOffset {
                let presentCount = Int(Swift.min(fileSize - l1TableOffset, UInt64(l1Size) * 8))
                if presentCount > 0,
                   let presentL1 = readFileRange(path: path, offset: l1TableOffset, count: presentCount),
                   presentL1.contains(where: { $0 != 0 }) {
                    return
                }
            }

            var allocatedClusters = Set<UInt64>()
            allocatedClusters.insert(0) // header
            allocatedClusters.insert(refcountTableOffset / clusterSize)
            allocatedClusters.insert(refcountBlockOffset / clusterSize)
            let l1Clusters = (UInt64(l1Size) * 8 + clusterSize - 1) / clusterSize
            for i in 0..<l1Clusters {
                allocatedClusters.insert(l1TableOffset / clusterSize + i)
            }
            let entriesPerRefcountBlock = clusterSize / 2 // refcount_order 4 = u16
            guard allocatedClusters.allSatisfy({ $0 < entriesPerRefcountBlock }) else {
                return
            }

            let handle = try FileHandle(forUpdating: URL(fileURLWithPath: path))
            defer { try? handle.close() }
            if fileSize < minimalFileSize {
                try handle.truncate(atOffset: minimalFileSize)
            }
            if incompatible != 0 {
                try handle.seek(toOffset: 72)
                try handle.write(contentsOf: dataBE64(0)) // dirty/corrupt cleared after rebuild
            }
            let maxAllocated = allocatedClusters.max() ?? 0
            var refcountHead = [UInt8](repeating: 0, count: Int((maxAllocated + 1) * 2))
            for cluster in allocatedClusters {
                refcountHead[Int(cluster * 2)] = 0
                refcountHead[Int(cluster * 2) + 1] = 1
            }
            try handle.seek(toOffset: refcountBlockOffset)
            try handle.write(contentsOf: Data(refcountHead))
            try handle.synchronize()
            appendBreadcrumb(config: config,
                             "03a backend: repaired the truncated qcow2 \(what) (missing refcount) -> \(formatBytes(minimalFileSize))")
        } catch {
            appendBreadcrumb(config: config,
                             "03a backend: qcow2 repair for \(what) failed (\(error.localizedDescription))")
        }
    }

    private static func freeDiskBytes() -> UInt64 {
        let url = VMConfiguration.guestDirectory
        if let values = try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
           let capacity = values.volumeAvailableCapacityForImportantUsage {
            return capacity > 0 ? UInt64(capacity) : 0
        }
        let attrs = try? FileManager.default.attributesOfFileSystem(forPath: url.path)
        return (attrs?[.systemFreeSize] as? NSNumber)?.uint64Value ?? 0
    }

    private static func fileSizeBytes(path: String) -> UInt64? {
        (try? FileManager.default.attributesOfItem(atPath: path)[.size] as? NSNumber)?.uint64Value
    }

    private static func formatBytes(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }

    private static func qcow2VirtualSizeBytes(path: String) -> UInt64? {
        guard let header = readFileRange(path: path, offset: 0, count: 32), header.count >= 32,
              header[0] == 0x51, header[1] == 0x46, header[2] == 0x49, header[3] == 0xfb else {
            return nil
        }
        return readUInt64BE(header, offset: 24)
    }

    private static func readFileRange(path: String, offset: UInt64, count: Int) -> [UInt8]? {
        guard count >= 0 else { return nil }
        do {
            let handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: path))
            defer { try? handle.close() }
            if offset > 0 { try handle.seek(toOffset: offset) }
            let data = try handle.read(upToCount: count) ?? Data()
            return [UInt8](data)
        } catch {
            return nil
        }
    }

    private static func readUInt32BE(_ bytes: [UInt8], offset: Int) -> UInt32? {
        guard offset >= 0, bytes.count >= offset + 4 else { return nil }
        return (0..<4).reduce(UInt32(0)) { ($0 << 8) | UInt32(bytes[offset + $1]) }
    }

    private static func readUInt64BE(_ bytes: [UInt8], offset: Int) -> UInt64? {
        guard offset >= 0, bytes.count >= offset + 8 else { return nil }
        return (0..<8).reduce(UInt64(0)) { ($0 << 8) | UInt64(bytes[offset + $1]) }
    }

    private static func dataBE32(_ value: UInt32) -> Data {
        withUnsafeBytes(of: value.bigEndian) { Data($0) }
    }

    private static func dataBE64(_ value: UInt64) -> Data {
        withUnsafeBytes(of: value.bigEndian) { Data($0) }
    }

    private static func makeSocketPair() throws -> (host: Int32, guest: Int32) {
        var fds: [Int32] = [-1, -1]
        let result = fds.withUnsafeMutableBufferPointer { buffer in
            socketpair(AF_UNIX, SOCK_STREAM, 0, buffer.baseAddress)
        }
        guard result == 0 else {
            let code = errno
            throw QEMUBackendError.socketPairFailed("socketpair errno=\(code) (\(String(cString: strerror(code))))")
        }
        return (host: fds[0], guest: fds[1])
    }

    private static func setNonblocking(_ fd: Int32) {
        let flags = fcntl(fd, F_GETFL, 0)
        if flags >= 0 {
            _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
        }
    }

    /// Called only on qmpQueue (the read source is bound to it).
    private func readAvailableQMPData() {
        var buffer = [UInt8](repeating: 0, count: 8192)
        let count = read(qmpFD, &buffer, buffer.count)
        if count > 0 {
            qmpBuffer.append(contentsOf: buffer.prefix(count))
            while let newline = qmpBuffer.firstIndex(of: 0x0A) {
                let line = qmpBuffer[qmpBuffer.startIndex..<newline]
                qmpBuffer.removeSubrange(qmpBuffer.startIndex...newline)
                if !line.isEmpty {
                    processQMPLine(Data(line))
                }
            }
        } else if count == 0 {
            closeQMPOnQueue(failPending: true)
        }
    }

    /// Called only on qmpQueue.
    private func processQMPLine(_ line: Data) {
        guard let object = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any] else {
            return
        }
        if object["QMP"] != nil {
            // Salutul QMP: negociem capabilitatile, apoi putem da comenzi.
            _ = writeQMPObjectOnQueue(["execute": "qmp_capabilities", "id": 0])
            return
        }
        if let event = object["event"] as? String {
            if event == "BLOCK_IO_ERROR" {
                // Essential diagnostic for failures like /data formatting:
                // QEMU says the device, operation, action and whether it
                // was host out-of-space (nospace), instead of us guessing
                // doar din „Buffer I/O error” al kernelului guest.
                let data = object["data"] as? [String: Any] ?? [:]
                let device = (data["device"] as? String) ?? (data["node-name"] as? String) ?? "?"
                let operation = (data["operation"] as? String) ?? "?"
                let action = (data["action"] as? String) ?? "?"
                let nospace = (data["nospace"] as? NSNumber)?.boolValue
                let nospaceText = nospace.map { $0 ? "true" : "false" } ?? "?"
                emit("[QMP] BLOCK_IO_ERROR device=\(device) operation=\(operation) action=\(action) nospace=\(nospaceText)\n")
            } else if ["SHUTDOWN", "RESET", "STOP", "RESUME"].contains(event) {
                emit("[QMP] \(event)\n")
            }
            return
        }
        guard let id = (object["id"] as? NSNumber)?.intValue else {
            return
        }
        if id == 0 {
            if object["error"] == nil {
                qmpReady = true
                emit("[DroidNative] QMP connected; the guest is running.\n")
                if !qmpAnnouncedDisplay, let config = activeConfig {
                    qmpAnnouncedDisplay = true
                    // Etapa 1a e serial-only (`-display none`): anuntam "display-ul"
                    // for the UI to move to running. The real scanout comes
                    // cu DisplayChangeListener in etapa 1b.
                    continuation?.yield(.displayResized(width: config.displayWidth, height: config.displayHeight))
                }
            } else {
                emit("[DroidNative] QMP negotiation failed: \(String(describing: object["error"]))\n")
            }
            return
        }
        if let pending = qmpPending.removeValue(forKey: id) {
            if let error = object["error"] {
                pending.resume(throwing: QEMUBackendError.qmpCommandFailed("\(error)"))
            } else {
                pending.resume()
            }
        }
    }

    private func sendQMP(execute: String) async throws {
        try await withCheckedThrowingContinuation { (pending: CheckedContinuation<Void, Error>) in
            qmpQueue.async { [weak self] in
                guard let self else {
                    pending.resume(throwing: QEMUBackendError.qmpUnavailable)
                    return
                }
                guard self.qmpFD >= 0, self.qmpReady else {
                    pending.resume(throwing: QEMUBackendError.qmpUnavailable)
                    return
                }
                let id = self.qmpNextID
                self.qmpNextID += 1
                self.qmpPending[id] = pending
                if !self.writeQMPObjectOnQueue(["execute": execute, "id": id]) {
                    self.qmpPending.removeValue(forKey: id)
                    pending.resume(throwing: QEMUBackendError.qmpUnavailable)
                    return
                }
                self.qmpQueue.asyncAfter(deadline: .now() + 10) { [weak self] in
                    if let timedOut = self?.qmpPending.removeValue(forKey: id) {
                        timedOut.resume(throwing: QEMUBackendError.qmpCommandFailed("timeout la \(execute)"))
                    }
                }
            }
        }
    }

    /// Called only on qmpQueue.
    private func writeQMPObjectOnQueue(_ object: [String: Any]) -> Bool {
        guard qmpFD >= 0,
              var data = try? JSONSerialization.data(withJSONObject: object) else {
            return false
        }
        data.append(0x0A)
        return data.withUnsafeBytes { raw -> Bool in
            guard let base = raw.baseAddress else {
                return false
            }
            var offset = 0
            while offset < data.count {
                let written = write(qmpFD, base.advanced(by: offset), data.count - offset)
                if written <= 0 {
                    if written < 0 && errno == EINTR {
                        continue
                    }
                    return false
                }
                offset += written
            }
            return true
        }
    }

    /// Called only on qmpQueue.
    private func closeQMPOnQueue(failPending: Bool) {
        if failPending {
            let pending = qmpPending
            qmpPending.removeAll()
            for (_, continuation) in pending {
                continuation.resume(throwing: QEMUBackendError.qmpUnavailable)
            }
        }
        qmpSource?.cancel()
        qmpSource = nil
        qmpFD = -1
        qmpReady = false
        qmpBuffer.removeAll()
    }

    // MARK: - Diagnostic persistent (qemu.log + breadcrumb)

    private static func resetDiagnostics(config: VMConfiguration) {
        try? FileManager.default.removeItem(atPath: config.qemuBreadcrumbPath)
        try? FileManager.default.removeItem(atPath: config.qemuLogPath)
    }

    /// Synchronous append: the line is on disk before we move on, so
    /// ramane citibila si daca urmatorul apel omoara procesul.
    /// Internal (not private): VMController writes lifecycle marks
    /// (background/foreground) into the same breadcrumb, for the previous-session
    /// report — otherwise a background kill looks like a causeless "crash".
    static func appendBreadcrumb(config: VMConfiguration, _ text: String) {
        let fd = open(config.qemuBreadcrumbPath, O_WRONLY | O_CREAT | O_APPEND, 0o644)
        guard fd >= 0 else {
            return
        }
        let line = text + "\n"
        line.withCString { pointer in
            _ = write(fd, pointer, strlen(pointer))
        }
        fsync(fd)
        close(fd)
    }

    /// The previous session report, if it ended abruptly (no stage 16
    /// "clean exit" and no handled 99 error). Shown at relaunch.
    /// The previous session passed `qemu_init` and entered the loop
    /// main loop (stage 13)? Then it did not "die at launch", it ran and
    /// was closed without a clean stop (usually force-close) — not an
    /// error to report as a failure at the next start.
    static func previousLaunchReachedMainLoop() -> Bool {
        let path = VMConfiguration.guestDirectory.appendingPathComponent("qemu-breadcrumb.txt").path
        return readTail(path: path, maxBytes: 8000).contains("13 bridge:")
    }

    static func previousLaunchReport() -> String? {
        let directory = VMConfiguration.guestDirectory
        let breadcrumb = readTail(path: directory.appendingPathComponent("qemu-breadcrumb.txt").path, maxBytes: 8000)
        guard !breadcrumb.isEmpty,
              !breadcrumb.contains("16 backend:"),
              !breadcrumb.contains("99 backend:") else {
            return nil
        }
        var report = breadcrumb.contains("13 bridge:")
            ? "[DroidNative] The previous QEMU session ran (entered the main loop) and was closed without a clean stop. Its stages, for reference:\n"
            : "[DroidNative] The previous QEMU session ended abruptly BEFORE the main loop. Last recorded stages:\n"
        report += breadcrumb
        if !report.hasSuffix("\n") {
            report += "\n"
        }
        let logTail = readTail(path: directory.appendingPathComponent("qemu.log").path, maxBytes: 12000)
        if logTail.isEmpty {
            report += "--- qemu.log: empty (QEMU did not get to write anything) ---\n"
        } else {
            report += "--- qemu.log (ultimele linii) ---\n" + logTail + "\n"
        }
        return report
    }

    private static func readTail(path: String, maxBytes: Int) -> String {
        let fd = open(path, O_RDONLY)
        guard fd >= 0 else {
            return ""
        }
        var info = stat()
        guard fstat(fd, &info) == 0 else {
            close(fd)
            return ""
        }
        let size = Int(info.st_size)
        let toRead = min(size, maxBytes)
        if toRead < size {
            lseek(fd, off_t(size - toRead), SEEK_SET)
        }
        var data = Data()
        var buffer = [UInt8](repeating: 0, count: 4096)
        while data.count < toRead {
            let count = read(fd, &buffer, min(buffer.count, toRead - data.count))
            if count <= 0 {
                break
            }
            data.append(contentsOf: buffer.prefix(count))
        }
        close(fd)
        return String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) ?? ""
    }

    /// Directioneaza stdout/stderr ale procesului in Guest/qemu.log pe durata
    /// of the QEMU run: its error messages (including those before an
    /// exit() in qemu_init) stay in the file, not lost with the process.
    private func installQEMULog(config: VMConfiguration, arguments: [String]) {
        let fd = open(config.qemuLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        guard fd >= 0 else {
            return
        }
        let header = "=== DroidNative QEMU log ===\n" + arguments.joined(separator: " \\\n  ") + "\n=== stdout/stderr QEMU ===\n"
        header.withCString { pointer in
            _ = write(fd, pointer, strlen(pointer))
        }
        fsync(fd)

        savedStdoutFD = dup(STDOUT_FILENO)
        savedStderrFD = dup(STDERR_FILENO)
        dup2(fd, STDOUT_FILENO)
        dup2(fd, STDERR_FILENO)
        if fd > STDERR_FILENO {
            close(fd)
        }
        logRedirectInstalled = true
    }

    private func restoreQEMULog() {
        guard logRedirectInstalled else {
            return
        }
        if savedStdoutFD >= 0 {
            dup2(savedStdoutFD, STDOUT_FILENO)
            close(savedStdoutFD)
            savedStdoutFD = -1
        }
        if savedStderrFD >= 0 {
            dup2(savedStderrFD, STDERR_FILENO)
            close(savedStderrFD)
            savedStderrFD = -1
        }
        logRedirectInstalled = false
    }
}

enum QEMUBackendError: LocalizedError {
    case alreadyStarted
    case libraryMissing(String)
    case libraryLoadFailed(String)
    case socketPairFailed(String)
    case diskPreparationFailed(String)
    case jitRequiredForTCGEngine
    case invalidParallelTrialConfiguration
    case qmpUnavailable
    case qmpCommandFailed(String)
    case snapshotUnsupported

    var errorDescription: String? {
        switch self {
        case .alreadyStarted:
            return "QEMU already ran in this process and cannot be re-initialized. Close the app completely and open it again."
        case .libraryMissing(let path):
            return "The QEMU engine is not bundled in the app (missing \(path)). On the Mac run scripts/stage-qemu-frameworks.sh, then Build from Xcode."
        case .libraryLoadFailed(let message):
            return "The QEMU engine did not load: \(message)"
        case .socketPairFailed(let message):
            return "Nu am putut crea canalele de comunicare cu QEMU: \(message)"
        case .diskPreparationFailed(let message):
            return "Android disk preparation failed before QEMU start: \(message)"
        case .jitRequiredForTCGEngine:
            return "The bundled engine is TCG with JIT and cannot start without JIT acquisition (there is no interpreter in this build). Enable the built-in method in Capabilities and start again."
        case .invalidParallelTrialConfiguration:
            return "Real-time MTTCG refused: Android needs the APFS GPU+parallel CPU clone matching 2–4 vCPUs, no snapshot and bundled TCG-JIT. Working disks unchanged."
        case .qmpUnavailable:
            return "The QMP control channel is not available."
        case .qmpCommandFailed(let message):
            return "The QMP command failed: \(message)"
        case .snapshotUnsupported:
            return "VM snapshots are not yet wired in the QEMU backend (later stage)."
        }
    }
}
