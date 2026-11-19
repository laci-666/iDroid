import Foundation

/// Ce booteaza VM-ul. Bring-up-ul in trepte (lectie S5LBox, docs/05 L5):
/// first a minimal Linux proving the engine + virtio end-to-end,
/// abia apoi Android-ul complet.
enum GuestKind: String, Codable {
    case testLinux   // minimal Image + initramfs: serial shell, evtest, test model
    case android     // LineageOS virtio_arm64only via UEFI
}

/// A virtual machine configuration. The app does not know "Android" — it knows a
/// guest image (firmware/disks/cmdline). See docs/03, decision 6.
///
/// Storage model: installed golden QCOW2 is read-only. Guest writes go
/// into a separate, standalone QCOW2 work disk cloned at first boot (no
/// backing-file relationship). "Factory reset" deletes the work disk.
/// The GPU experiment uses yet another APFS clone of the work disk.
struct VMConfiguration: Codable {
    var name: String = "Android (virtio_arm64only)"
    var guestKind: GuestKind = .android
    var accelerator: AcceleratorChoice = .automatic
    var cpuCount: Int = 4                       // A18 Pro: 2P+4E; lasam gazdei margine
    var memoryMegabytes: Int = 3072             // suprascris dinamic la boot (CapabilityProbe)
    var jitArenaMegabytes: Int = 384            // arena TCG unica, prealocata (protocol TXM)
    var displayWidth: Int = 720                 // modest guest scanout at bring-up: rendering is software
    var displayHeight: Int = 1280
    /// Guest display density (EDID physical size -> Android UI scale).
    var guestDPI: Int = 560               // (llvmpipe pe TCG); 1080x2400 infometa boot-ul (2026-10-20)
    var guestRefreshRateHz: Int = 75      // 0.4.9: proven legacy EDID default; 55 remains an optional A/B test
    var gpuAcceleration: Bool = false           // virtio-gpu-gl + virgl; opt-in only on isolated GPU trial
    var isGPUTrial: Bool = false                 // APFS-cloned work disks; never load/save software g0 snapshot
    var isParallelTrial: Bool = false            // a SECOND APFS clone of GPUTrial; only this Android profile may disable icount
    var enableIOSurfaceFastPath: Bool = true     // only isolated parallel-2 EAGL; guarded -> PBO
    /// Diluted virtual time (QEMU -icount): the guest clock advances with
    /// executed instructions, not the wall. Without it, on slow TCG,
    /// The Android watchdog (60 s guest) kills system_server before it
    /// finish starting -> zygote loop (the 2026-10-20 logs).
    var useIcount: Bool = true
    /// Instant-resume: at start, restore the internal `dn-ready` snapshot
    /// (saved with the Snapshot button) instead of booting Android from zero.
    /// Toggleable in Capabilities ("dnLoadSnapshot"), off by default until
    /// the first manually saved snapshot.
    var loadSnapshot: Bool = false
    var adbHostPort: Int = 5555                 // slirp hostfwd -> guest:5555

    /// Cail in containerul aplicatiei (completate de `default(probe:)`).
    var firmwarePath: String = ""               // EDK II CODE (pflash unit 0, read-only) — doar .android
    var firmwareVarsPath: String = ""           // EDK II VARS (pflash unit 1, scriibil; din pachetul UTM)
    var goldenSystemDiskPath: String = ""       // imported Lineage image, READ-ONLY
    var systemOverlayPath: String = ""          // standalone QCOW2 work clone; reset = delete
    var userDiskPath: String = ""               // userdata qcow2 (work disk)
    var goldenUserDiskPath: String = ""         // sablon userdata din pachet (vdb), READ-ONLY; reset = re-clonare
    var testKernelPath: String = ""             // doar .testLinux: Image (kernel ARM64)
    var testInitrdPath: String = ""             // doar .testLinux: initramfs.cpio.gz
    var snapshotDirectory: String = ""

    enum AcceleratorChoice: String, Codable {
        case automatic, hvf, tcgJIT, tcgTCTI
    }

    /// The VM data directory: Documents/Guest/
    static var guestDirectory: URL {
        let base = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("Guest", isDirectory: true)
    }

    static func `default`(probe: CapabilityProbe, kind: GuestKind = .android) -> VMConfiguration {
        var cfg = VMConfiguration()
        cfg.guestKind = kind
        if kind == .testLinux {
            cfg.cpuCount = 2
            cfg.memoryMegabytes = 512           // testul minimal nu are nevoie de mai mult
        } else {
            // Guest RAM is a supported Flags value ("dnGuestRAM" MB;
            // 0/absent = automatic from the device probe). RAM remains
            // part of the snapshot shape.
            //
            // Proven platform values are now fixed in code, not settings:
            // software rendering and icount ON. The old QEMU GL and
            // icount switches were removed in iDroid 0.4; stale defaults
            // from older builds are deliberately ignored here.
            cfg.gpuAcceleration = false
            let explicitRAM = UserDefaults.standard.integer(forKey: "dnGuestRAM")
            if explicitRAM > 0 {
                cfg.memoryMegabytes = explicitRAM
            } else {
                cfg.memoryMegabytes = Int(probe.plannedGuestMemoryBytes / (1024 * 1024))
            }
            cfg.useIcount = true
            // Default ON only for the isolated 2-vCPU EAGL/MTTCG clone.
            // Unsupported EGL/IOSurface/Metal paths fall back to the PBO;
            // Flags can explicitly turn it off without touching guest data.
            cfg.enableIOSurfaceFastPath = UserDefaults.standard.object(forKey: "dnIOSurfaceFastPath") as? Bool ?? true
            // icount executes all vCPUs round-robin on one host thread;
            // two vCPUs are the measured/supported configuration.
            cfg.cpuCount = 2
            // Guest display scale (Capabilities, "dnDisplayScale"):
            // - "redusa" 540x960: 56% din pixelii de 720x1280 — compozitia
            //   SurfaceFlinger pe CPU emulat costa ~liniar cu pixelii;
            // - "nativa" 1320x2868 (0.2.19): the physical resolution of the
            //   iPhone 16 Pro Max screen — Android fills the whole screen, no
            //   letterbox; dar ~4,1x pixelii de 720x1280 (compunere +
            //   readback + texture upload much heavier);
            // - default ("normala") 720x1280.
            switch UserDefaults.standard.string(forKey: "dnDisplayScale") {
            case "redusa":
                cfg.displayWidth = 540
                cfg.displayHeight = 960
            case "nativa":
                cfg.displayWidth = 1320
                cfg.displayHeight = 2868
            default:
                break
            }
            cfg.loadSnapshot = UserDefaults.standard.bool(forKey: "dnLoadSnapshot")
            // JIT arena / tb-size (Capabilities, 0.2.20): 0 = default
            // 384 MB (device-proven). Large values are safe
            // to try: the TXM probe in VMController automatically falls
            // la cea mai mare marime acceptata si logheaza decizia.
            let arenaMB = UserDefaults.standard.integer(forKey: "dnJITArenaMB")
            if arenaMB > 0 { cfg.jitArenaMegabytes = arenaMB }
            // Guest DPI ("dnGuestDPI", 0.2.25): EDID physical size ->
            // Android density. 0/unset = 560 (readable at native res).
            let storedDPI = UserDefaults.standard.integer(forKey: "dnGuestDPI")
            cfg.guestDPI = (storedDPI >= 240 && storedDPI <= 800) ? storedDPI : 560
            // 0.4.9: return to the proven legacy 75 Hz EDID by default.
            // 55 Hz did not improve UI responsiveness and would cap a
            // hypothetical 60-FPS guest. Keep it for explicit A/B only;
            // persisted user choices are not silently overridden.
            let hz = UserDefaults.standard.integer(forKey: "dnGuestRefreshRateHz")
            cfg.guestRefreshRateHz = hz == 55 ? 55 : 75
        }
        let dir = guestDirectory
        cfg.firmwarePath = dir.appendingPathComponent("AAVMF_CODE.fd").path
        cfg.firmwareVarsPath = dir.appendingPathComponent("efi_vars.fd").path
        cfg.goldenSystemDiskPath = dir.appendingPathComponent("system-golden.qcow2").path
        cfg.systemOverlayPath = dir.appendingPathComponent("system-overlay.qcow2").path
        cfg.userDiskPath = dir.appendingPathComponent("userdata.qcow2").path
        cfg.goldenUserDiskPath = dir.appendingPathComponent("userdata-golden.qcow2").path
        cfg.testKernelPath = dir.appendingPathComponent("test-Image").path
        cfg.testInitrdPath = dir.appendingPathComponent("test-initramfs.cpio.gz").path
        cfg.snapshotDirectory = dir.appendingPathComponent("snapshots").path
        return cfg
    }

    /// One-shot, opt-in guest GPU bring-up. The official LineageOS UTM guide
    /// reports a black Android UI with ANGLE Metal and recommends ANGLE OpenGL.
    /// We can test its iOS/EAGL counterpart WITHOUT altering the working VM:
    /// three APFS copy-on-write clones (system work, userdata, EFI variables),
    /// a shared READ-ONLY golden system, and no g0 snapshot load/save. A clone
    /// failure does NOT fall back to a full copy or use the original work disk.
    /// Once prepared, the GPU trial persists separately for repeated tests;
    /// it is not silently synced back into the software VM's user data.
    func isolatedGPUTrial() throws -> VMConfiguration {
        guard guestKind == .android, !isGPUTrial, !gpuAcceleration,
              !goldenSystemDiskPath.isEmpty else {
            throw GPUTrialStorageError.invalidSource
        }
        let fm = FileManager.default
        let guest = Self.guestDirectory
        let trial = guest.appendingPathComponent("GPUTrial", isDirectory: true)
        let marker = trial.appendingPathComponent("apfs-clone-v1.ready")
        let pairs: [(source: String, name: String)] = [
            (systemOverlayPath, "system-overlay.qcow2"),
            (userDiskPath, "userdata.qcow2"),
            (firmwareVarsPath, "efi_vars.fd"),
        ]

        func validRegular(_ path: String) throws -> UInt64 {
            let values = try URL(fileURLWithPath: path).resourceValues(
                forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
            let attributes = try fm.attributesOfItem(atPath: path)
            guard values.isRegularFile == true, values.isSymbolicLink != true,
                  attributes[.type] as? FileAttributeType == FileAttributeType.typeRegular,
                  let size = attributes[.size] as? NSNumber, size.uint64Value > 0 else {
                throw GPUTrialStorageError.invalidFile(path)
            }
            return size.uint64Value
        }
        func validateSelfContainedQCOW2(_ path: String) throws {
            // This product's work disks are standalone QCOW2 files. If a
            // future image has a relative backing file, moving a clone into
            // GPUTrial would break resolution or risk original-disk writes.
            let handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: path))
            defer { try? handle.close() }
            let header = try handle.read(upToCount: 16) ?? Data()
            guard header.count == 16,
                  header.prefix(4).elementsEqual([0x51, 0x46, 0x49, 0xfb]),
                  header[8..<16].allSatisfy({ $0 == 0 }) else {
                throw GPUTrialStorageError.backedDisk(path)
            }
        }
        func configuredTrial() -> VMConfiguration {
            var cfg = self
            cfg.isGPUTrial = true
            cfg.gpuAcceleration = true
            cfg.loadSnapshot = false
            cfg.systemOverlayPath = trial.appendingPathComponent("system-overlay.qcow2").path
            cfg.userDiskPath = trial.appendingPathComponent("userdata.qcow2").path
            cfg.firmwareVarsPath = trial.appendingPathComponent("efi_vars.fd").path
            cfg.snapshotDirectory = trial.appendingPathComponent("snapshots").path
            return cfg
        }

        if fm.fileExists(atPath: marker.path) {
            let folder = try trial.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard folder.isDirectory == true, folder.isSymbolicLink != true else {
                throw GPUTrialStorageError.incompleteTrial(trial.path)
            }
            _ = try validRegular(marker.path)
            let flag = try String(contentsOf: marker, encoding: .utf8)
            guard flag == "DN_GPU_TRIAL_V1\n" else {
                throw GPUTrialStorageError.invalidFile(marker.path)
            }
            for item in pairs {
                let path = trial.appendingPathComponent(item.name).path
                _ = try validRegular(path)
                if item.name.hasSuffix(".qcow2") { try validateSelfContainedQCOW2(path) }
            }
            return configuredTrial()
        }
        guard !fm.fileExists(atPath: trial.path) else {
            // An interrupted older preparation is NOT a valid disk image.
            // Do not delete it automatically: retain evidence and user data.
            throw GPUTrialStorageError.incompleteTrial(trial.path)
        }
        let sources = try pairs.map { item -> UInt64 in
            let size = try validRegular(item.source)
            if item.name.hasSuffix(".qcow2") {
                try validateSelfContainedQCOW2(item.source)
            }
            return size
        }
        _ = try validRegular(goldenSystemDiskPath)
        let fs = try fm.attributesOfFileSystem(forPath: guest.path)
        guard let free = fs[.systemFreeSize] as? NSNumber,
              free.uint64Value >= 3 * 1024 * 1024 * 1024 else {
            throw GPUTrialStorageError.lowSpace
        }
        let staging = guest.appendingPathComponent("GPUTrial.pending-\(UUID().uuidString)", isDirectory: true)
        try fm.createDirectory(at: staging, withIntermediateDirectories: false)
        do {
            for (index, item) in pairs.enumerated() {
                let destination = staging.appendingPathComponent(item.name).path
                let result = item.source.withCString { src in
                    destination.withCString { dst in dn_gpu_trial_clone_file(src, dst) }
                }
                guard result == 0 else {
                    throw GPUTrialStorageError.cloneFailed(item.name, Int(result))
                }
                guard try validRegular(destination) == sources[index] else {
                    throw GPUTrialStorageError.invalidFile(destination)
                }
                if item.name.hasSuffix(".qcow2") {
                    try validateSelfContainedQCOW2(destination)
                }
            }
            try Data("DN_GPU_TRIAL_V1\n".utf8).write(
                to: staging.appendingPathComponent("apfs-clone-v1.ready"), options: .atomic)
            // Move a COMPLETE staging directory into place in one filesystem
            // operation. Never remove or rename the normal Guest work disks.
            try fm.moveItem(at: staging, to: trial)
        } catch {
            try? fm.removeItem(at: staging)
            throw error
        }
        return configuredTrial()
    }

    /// Explicit CPU-performance experiment, not a change to the working GL
    /// profile. First validate an existing GPUTrial (the user should have
    /// booted it before this experiment); then APFS-clone its *work*
    /// disks into independent ParallelGPUTrial (2 vCPU, existing 0.4.9 data),
    /// ParallelGPUTrial3 or ParallelGPUTrial4. A CPU-count experiment never
    /// changes the hardware shape of an existing trial or edits its disks.
    /// No original, GPUTrial or g0 disk/snapshot is opened for writing by
    /// this configuration. Real-time Android watchdogs and host contention
    /// can regress at 3/4 vCPUs: opt-in only, NOT a speed guarantee.
    func isolatedParallelGPUTrial(cpuCount: Int = 2) throws -> VMConfiguration {
        // Validate before constructing any paths from a user preference.
        guard (2...4).contains(cpuCount), guestKind == .android,
              !isGPUTrial, !gpuAcceleration, useIcount,
              !goldenSystemDiskPath.isEmpty else {
            throw GPUTrialStorageError.invalidSource
        }
        let ready = Self.guestDirectory.appendingPathComponent("GPUTrial/apfs-clone-v1.ready")
        guard FileManager.default.fileExists(atPath: ready.path) else {
            throw GPUTrialStorageError.requiresExistingGPUTrial
        }
        let stableGPU = try isolatedGPUTrial() // validates marker, standalone QCOW2 and EFI
        return try stableGPU.prepareParallelGPUTrialClone(cpuCount: cpuCount)
    }

    private func prepareParallelGPUTrialClone(cpuCount: Int) throws -> VMConfiguration {
        guard (2...4).contains(cpuCount), guestKind == .android,
              isGPUTrial, gpuAcceleration, !isParallelTrial, useIcount else {
            throw GPUTrialStorageError.invalidSource
        }
        let fm = FileManager.default
        let guest = Self.guestDirectory
        // The 2-vCPU folder AND marker are byte-compatible with 0.4.9.
        // The other CPU shapes start from GPUTrial, NOT the 2-vCPU clone.
        let folderName = cpuCount == 2 ? "ParallelGPUTrial" : "ParallelGPUTrial\(cpuCount)"
        let markerName = cpuCount == 2 ? "apfs-parallel-v1.ready" : "apfs-parallel-c\(cpuCount)-v1.ready"
        let markerData = cpuCount == 2 ? "DN_PARALLEL_GPU_TRIAL_V1\n" : "DN_PARALLEL_GPU_TRIAL_C\(cpuCount)_V1\n"
        let trial = guest.appendingPathComponent(folderName, isDirectory: true)
        let marker = trial.appendingPathComponent(markerName)
        let pairs = [
            (name: "system-overlay.qcow2", source: systemOverlayPath),
            (name: "userdata.qcow2", source: userDiskPath),
            (name: "efi_vars.fd", source: firmwareVarsPath),
        ]
        func validRegular(_ path: String) throws -> UInt64 {
            let values = try URL(fileURLWithPath: path).resourceValues(
                forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
            let attributes = try fm.attributesOfItem(atPath: path)
            guard values.isRegularFile == true, values.isSymbolicLink != true,
                  attributes[.type] as? FileAttributeType == FileAttributeType.typeRegular,
                  let size = attributes[.size] as? NSNumber, size.uint64Value > 0 else {
                throw GPUTrialStorageError.invalidFile(path)
            }
            return size.uint64Value
        }
        func validateStandaloneQCOW2(_ path: String) throws {
            let handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: path))
            defer { try? handle.close() }
            let header = try handle.read(upToCount: 16) ?? Data()
            guard header.count == 16,
                  header.prefix(4).elementsEqual([0x51, 0x46, 0x49, 0xfb]),
                  header[8..<16].allSatisfy({ $0 == 0 }) else {
                throw GPUTrialStorageError.backedDisk(path)
            }
        }
        func configuredTrial() -> VMConfiguration {
            var cfg = self
            cfg.isParallelTrial = true
            cfg.useIcount = false // QEMU thread=multi: only in this separate trial
            cfg.cpuCount = cpuCount // 2 stays the measured 0.4.9 reference
            cfg.loadSnapshot = false
            cfg.systemOverlayPath = trial.appendingPathComponent("system-overlay.qcow2").path
            cfg.userDiskPath = trial.appendingPathComponent("userdata.qcow2").path
            cfg.firmwareVarsPath = trial.appendingPathComponent("efi_vars.fd").path
            cfg.snapshotDirectory = trial.appendingPathComponent("snapshots").path
            return cfg
        }

        if fm.fileExists(atPath: marker.path) {
            let folder = try trial.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard folder.isDirectory == true, folder.isSymbolicLink != true else {
                throw GPUTrialStorageError.incompleteTrial(trial.path)
            }
            _ = try validRegular(marker.path)
            let flag = try String(contentsOf: marker, encoding: .utf8)
            guard flag == markerData else {
                throw GPUTrialStorageError.invalidFile(marker.path)
            }
            for item in pairs {
                let path = trial.appendingPathComponent(item.name).path
                _ = try validRegular(path)
                if item.name.hasSuffix(".qcow2") { try validateStandaloneQCOW2(path) }
            }
            return configuredTrial()
        }
        guard !fm.fileExists(atPath: trial.path) else {
            // Never overwrite or delete a partial trial; preserve its data.
            throw GPUTrialStorageError.incompleteTrial(trial.path)
        }
        let sizes = try pairs.map { item -> UInt64 in
            let size = try validRegular(item.source)
            if item.name.hasSuffix(".qcow2") { try validateStandaloneQCOW2(item.source) }
            return size
        }
        _ = try validRegular(goldenSystemDiskPath)
        let fs = try fm.attributesOfFileSystem(forPath: guest.path)
        guard let free = fs[.systemFreeSize] as? NSNumber,
              free.uint64Value >= 3 * 1024 * 1024 * 1024 else {
            throw GPUTrialStorageError.lowSpace
        }
        let staging = guest.appendingPathComponent("\(folderName).pending-\(UUID().uuidString)",
                                                  isDirectory: true)
        try fm.createDirectory(at: staging, withIntermediateDirectories: false)
        do {
            for (index, item) in pairs.enumerated() {
                let destination = staging.appendingPathComponent(item.name).path
                let result = item.source.withCString { src in
                    destination.withCString { dst in dn_gpu_trial_clone_file(src, dst) }
                }
                guard result == 0 else {
                    throw GPUTrialStorageError.cloneFailed(item.name, Int(result))
                }
                guard try validRegular(destination) == sizes[index] else {
                    throw GPUTrialStorageError.invalidFile(destination)
                }
                if item.name.hasSuffix(".qcow2") { try validateStandaloneQCOW2(destination) }
            }
            try Data(markerData.utf8).write(
                to: staging.appendingPathComponent(markerName), options: .atomic)
            try fm.moveItem(at: staging, to: trial)
        } catch {
            try? fm.removeItem(at: staging) // unique, uncommitted staging only
            throw error
        }
        return configuredTrial()
    }

    /// Base of the QEMU serial console FIFO (`-chardev pipe`): QEMU creates
    /// `<baza>.in` (intrare) si `<baza>.out` (iesire, citita de QEMUBackend).
    /// Used only as fallback; the main path is socketpair via fd.
    var serialPipeBasePath: String {
        Self.guestDirectory.appendingPathComponent("serial0").path
    }

    /// Logul brut QEMU (stdout+stderr redirectionate la boot). Supravietuieste
    /// of a process crash/exit and is shown at relaunch in the Console.
    var qemuLogPath: String {
        Self.guestDirectory.appendingPathComponent("qemu.log").path
    }

    /// Last QEMU launch stage, written synchronously (breadcrumb). If the process
    /// dies in qemu_init, we know exactly where it stopped, no guessing.
    var qemuBreadcrumbPath: String {
        Self.guestDirectory.appendingPathComponent("qemu-breadcrumb.txt").path
    }

    /// The guest is imported by the user (see guest/README.md).
    var guestImageInstalled: Bool {
        let fm = FileManager.default
        switch guestKind {
        case .android:
            return fm.fileExists(atPath: goldenSystemDiskPath)
                && fm.fileExists(atPath: firmwarePath)
                && fm.fileExists(atPath: firmwareVarsPath)
        case .testLinux:
            return fm.fileExists(atPath: testKernelPath) && fm.fileExists(atPath: testInitrdPath)
        }
    }

    /// Prepares the directory structure. The work disk is created by
    /// the QEMU backend at the first Android boot, by cloning the
    /// golden file (temp + rename) — nobody writes into the golden image.
    func prepareStorage() {
        let fm = FileManager.default
        try? fm.createDirectory(atPath: Self.guestDirectory.path, withIntermediateDirectories: true)
        try? fm.createDirectory(atPath: snapshotDirectory, withIntermediateDirectories: true)
    }

    /// Factory reset: the overlay disappears, the golden stays.
    func factoryReset() {
        try? FileManager.default.removeItem(atPath: systemOverlayPath)
        try? FileManager.default.removeItem(atPath: userDiskPath)
    }
}

private enum GPUTrialStorageError: LocalizedError {
    case invalidSource
    case requiresExistingGPUTrial
    case invalidFile(String)
    case backedDisk(String)
    case incompleteTrial(String)
    case lowSpace
    case cloneFailed(String, Int)

    var errorDescription: String? {
        switch self {
        case .invalidSource:
            return "GPU trial requires the installed Android software profile."
        case .requiresExistingGPUTrial:
            return "Parallel CPU trial needs an existing GPU trial first. Boot the isolated GPU trial successfully before testing real-time MTTCG. Original disks and snapshot unchanged."
        case .invalidFile(let path):
            return "GPU trial source or clone is missing/invalid: \(path). Start the software VM first; no original disk was modified."
        case .backedDisk(let path):
            return "GPU trial refused: QCOW2 image at \(path) is invalid or has a backing file. The trial cannot relocate a backed work disk; original VM unchanged."
        case .incompleteTrial(let path):
            return "An incomplete isolated trial already exists at \(path). The working VM is unchanged; refusing to overwrite that copy."
        case .lowSpace:
            return "An isolated trial needs at least 3 GiB free on the iPhone for copy-on-write growth. Original VM and snapshot unchanged."
        case .cloneFailed(let name, let code):
            return "APFS copy-on-write clone of \(name) failed (errno \(code)). No full-copy fallback and no original disk was modified."
        }
    }
}
