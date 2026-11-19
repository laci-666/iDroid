import Foundation
import UIKit

/// Persistent install state (Guest/setup-state.json).
struct SetupState: Codable {
    var lineageReleaseTag: String?
    var lineageAssetName: String?
    var lineageInstalledAt: Date?
    var testGuestInstalledAt: Date?
    var firmwareSource: String?
}

/// The automatic setup assistant (the "Setup" tab).
///
/// The one-tap flow for Android: resolve the latest Lineage release -> check
/// space -> download the UTM-VM archive (with progress and resume) -> verify SHA256
/// -> extract -> map disks onto the golden+overlay model -> download
/// the UEFI firmware and bring pflash to 64 MiB -> write the VM configuration
/// calculata din capabilitatile reale ale telefonului. La final, boot-ul nu
/// mai are nimic de intrebat utilizatorul.
@MainActor
final class GuestSetupManager: ObservableObject {

    enum Phase: Equatable {
        case idle
        case resolving          // API GitHub: ultimul release
        case downloading(Double)
        case verifying
        case extracting(Double)
        case preparing          // discuri, firmware, padding pflash
        case writingConfig
        case finished
        case failed(String)

        var isBusy: Bool {
            switch self {
            case .idle, .finished, .failed(_): return false
            default: return true
            }
        }
    }

    @Published var phase: Phase = .idle
    @Published var statusLine: String = ""
    @Published var state = SetupState()

    private let downloader = DownloadManager()
    private var currentTask: Task<Void, Never>?

    // MARK: - Cai derivate

    private var guestDir: URL { VMConfiguration.guestDirectory }
    private var downloadsDir: URL { guestDir.appendingPathComponent("downloads", isDirectory: true) }
    private var stateURL: URL { guestDir.appendingPathComponent("setup-state.json") }
    private var configURL: URL { guestDir.appendingPathComponent("vm-config.json") }

    init() { loadState() }

    // MARK: - Derived states for the UI

    var androidInstalled: Bool {
        VMConfiguration.default(probe: CapabilityProbe()).guestImageInstalled
    }
    var testGuestInstalled: Bool {
        VMConfiguration.default(probe: CapabilityProbe(), kind: .testLinux).guestImageInstalled
    }
    var freeDiskBytes: UInt64 {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let values = try? docs.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return UInt64(values?.volumeAvailableCapacityForImportantUsage ?? 0)
    }

    // MARK: - Fluxul principal: Android

    func startAutomaticSetup(probe: CapabilityProbe, releaseOffset: Int = 0) {
        guard !phase.isBusy else { return }
        UIApplication.shared.isIdleTimerDisabled = true   // keep the screen on
        currentTask = Task {
            defer { UIApplication.shared.isIdleTimerDisabled = false }
            do {
                try await runAndroidSetup(probe: probe, releaseOffset: releaseOffset)
                phase = .finished
                statusLine = "Android is set up. You can tap \"Start Android\" in the Android tab."
            } catch let e as SetupError {
                phase = .failed(e.localizedDescription)
            } catch is CancellationError {
                phase = .failed("Cancelled (the download can resume where it left off).")
            } catch {
                phase = .failed(error.localizedDescription)
            }
        }
    }

    /// Manual import: the user brings the UTM-VM archive from Files (same flow,
    /// skipping the download). Useful offline or if GitHub is unavailable.
    func importPackage(from sourceURL: URL, probe: CapabilityProbe) {
        guard !phase.isBusy else { return }
        UIApplication.shared.isIdleTimerDisabled = true
        currentTask = Task {
            defer { UIApplication.shared.isIdleTimerDisabled = false }
            do {
                guard sourceURL.startAccessingSecurityScopedResource() else {
                    throw SetupError.network("cannot access the chosen file")
                }
                defer { sourceURL.stopAccessingSecurityScopedResource() }
                try makeDirectories()
                let localZip = downloadsDir.appendingPathComponent(sourceURL.lastPathComponent)
                statusLine = "Copiez pachetul ales…"
                if FileManager.default.fileExists(atPath: localZip.path) {
                    try FileManager.default.removeItem(at: localZip)
                }
                try FileManager.default.copyItem(at: sourceURL, to: localZip)
                try await extractAndPrepare(zipURL: localZip, probe: probe,
                                            releaseTag: "import-manual", assetName: sourceURL.lastPathComponent)
                phase = .finished
                statusLine = "Package imported and set up."
            } catch let e as SetupError {
                phase = .failed(e.localizedDescription)
            } catch {
                phase = .failed(error.localizedDescription)
            }
        }
    }

    /// Guest de test (portile 1a–1c): kernel + initramfs Alpine (~41 MB total).
    func downloadTestGuest() {
        guard !phase.isBusy else { return }
        currentTask = Task {
            do {
                try makeDirectories()
                let cfg = VMConfiguration.default(probe: CapabilityProbe(), kind: .testLinux)
                statusLine = "Downloading the Alpine kernel (aarch64)…"
                try await downloader.download(from: GuestCatalog.alpineKernelURL,
                                              to: URL(fileURLWithPath: cfg.testKernelPath)) { f in
                    Task { @MainActor in self.phase = .downloading(f * 0.35) }
                }
                statusLine = "Downloading the Alpine initramfs…"
                try await downloader.download(from: GuestCatalog.alpineInitrdURL,
                                              to: URL(fileURLWithPath: cfg.testInitrdPath)) { f in
                    Task { @MainActor in self.phase = .downloading(0.35 + f * 0.65) }
                }
                state.testGuestInstalledAt = Date()
                saveState()
                phase = .finished
                statusLine = "Test guest installed (start it from the Android tab, type \"Test Linux\")."
            } catch let e as SetupError {
                phase = .failed(e.localizedDescription)
            } catch {
                phase = .failed(error.localizedDescription)
            }
        }
    }

    func cancel() {
        downloader.cancel()
        currentTask?.cancel()
    }

    /// Factory reset: keeps the golden image and firmware.
    func factoryReset(probe: CapabilityProbe) {
        VMConfiguration.default(probe: probe).factoryReset()
        statusLine = "Reset done: the overlay and data were deleted; the base image and template remain — the work disks rebuild themselves at start."
        objectWillChange.send()
    }

    /// Deletes the whole guest (including golden) — the next setup re-downloads.
    func deleteEverything() {
        try? FileManager.default.removeItem(at: guestDir)
        state = SetupState()
        phase = .idle
        statusLine = "Everything was deleted."
        objectWillChange.send()
    }

    // MARK: - Pasi interni

    private func runAndroidSetup(probe: CapabilityProbe, releaseOffset: Int = 0) async throws {
        try makeDirectories()

        phase = .resolving
        statusLine = releaseOffset == 0
            ? "Looking for the latest LineageOS for QEMU…"
            : "Looking for the previous LineageOS for QEMU…"
        let release = try await GitHubRelease.fetchLineageRelease(offset: releaseOffset)
        guard let asset = release.lineageAsset else { throw SetupError.assetMissing }

        let free = freeDiskBytes
        guard free >= GuestCatalog.recommendedFreeBytes else {
            throw SetupError.insufficientStorage(available: free, required: GuestCatalog.recommendedFreeBytes)
        }

        let zipURL = downloadsDir.appendingPathComponent(asset.name)
        let existingSize = (try? FileManager.default.attributesOfItem(atPath: zipURL.path)[.size] as? NSNumber)?.uint64Value
        let alreadyHave = existingSize == asset.size
        if !alreadyHave {
            statusLine = "Downloading \(asset.name) (\(ByteCountFormatter.string(fromByteCount: Int64(asset.size), countStyle: .file))) — keep the app open."
            try await downloader.download(from: asset.browserDownloadURL, to: zipURL) { f in
                Task { @MainActor in self.phase = .downloading(f) }
            }
        }

        phase = .verifying
        statusLine = "Verifying the checksum…"
        // Pas pe un fir de fundal: 1 GB de hash nu tine pe MainActor.
        let digest = asset.digest
        let zipToHash = zipURL
        if let expected = digest?.replacingOccurrences(of: "sha256:", with: "") {
            let actual = try await Task.detached { try DownloadManager.sha256Hex(of: zipToHash) }.value
            guard actual == expected else { throw SetupError.checksumMismatch }
        }

        try await extractAndPrepare(zipURL: zipURL, probe: probe,
                                    releaseTag: release.tagName, assetName: asset.name)
    }

    private func extractAndPrepare(zipURL: URL, probe: CapabilityProbe,
                                   releaseTag: String, assetName: String) async throws {
        let extractDir = downloadsDir.appendingPathComponent("extracted", isDirectory: true)
        try? FileManager.default.removeItem(at: extractDir)
        try FileManager.default.createDirectory(at: extractDir, withIntermediateDirectories: true)

        statusLine = "Extrag arhiva…"
        let extracted = try await Task.detached { () throws -> [URL] in
            try ZipExtractor.extract(zipURL: zipURL, to: extractDir) { f in
                Task { @MainActor in self.phase = .extracting(f) }
            }
        }.value

        phase = .preparing
        statusLine = "Preparing the disks (base image + data)…"
        let cfg = VMConfiguration.default(probe: probe)
        cfg.prepareStorage()
        let fm = FileManager.default

        // Pachetul UTM-VM: Data/vda.qcow2 = sistemul, Data/vdb.qcow2 = userdata gol,
        // Data/efi_vars.fd = sablonul de variabile EFI.
        guard let vda = extracted.first(where: { $0.lastPathComponent == "vda.qcow2" }) else {
            throw SetupError.unexpectedPackage("nu gasesc vda.qcow2 in \(assetName)")
        }
        try replaceItem(at: URL(fileURLWithPath: cfg.goldenSystemDiskPath), with: vda)
        // Discul de lucru e o clona a golden-ului: la orice reinstalare il
        // aruncam, altfel boot-ul ar continua pe sistemul vechi (posibil
        // corrupted by crashes). On a version change userdata falls too
        // (/data formats are not compatible between different builds).
        try? fm.removeItem(atPath: cfg.systemOverlayPath)
        if let oldTag = state.lineageReleaseTag, oldTag != releaseTag {
            try? fm.removeItem(atPath: cfg.userDiskPath)
        }
        // vdb devine SABLONUL userdata (pristine), nu discul de lucru: la boot,
        // the backend clones the template if the work disk is missing. So
        // "factory reset" (delete the work disk) heals itself, no setup
        // re-run. On old installs the template is born at the first
        // re-setup.
        if let vdb = extracted.first(where: { $0.lastPathComponent == "vdb.qcow2" }) {
            try replaceItem(at: URL(fileURLWithPath: cfg.goldenUserDiskPath), with: vdb)
        }
        if let vars = extracted.first(where: { $0.lastPathComponent == "efi_vars.fd" }),
           !fm.fileExists(atPath: cfg.firmwareVarsPath) {
            try replaceItem(at: URL(fileURLWithPath: cfg.firmwareVarsPath), with: vars)
        }
        try? FileManager.default.removeItem(at: extractDir)

        // Firmware UEFI (codul): il aducem noi, o singura data.
        if !fm.fileExists(atPath: cfg.firmwarePath) {
            statusLine = "Downloading the UEFI firmware…"
            try await downloader.download(from: GuestCatalog.firmwareCodeURL,
                                          to: URL(fileURLWithPath: cfg.firmwarePath)) { _ in }
            state.firmwareSource = GuestCatalog.firmwareCodeURL.absoluteString
        }
        // the `virt` machine pflash images must be exactly 64 MiB.
        try padPflash(URL(fileURLWithPath: cfg.firmwarePath))
        if fm.fileExists(atPath: cfg.firmwareVarsPath) {
            try padPflash(URL(fileURLWithPath: cfg.firmwareVarsPath))
        }

        phase = .writingConfig
        statusLine = "Writing the optimal configuration for your phone…"
        let data = try JSONEncoder.pretty.encode(cfg)
        try data.write(to: configURL)

        state.lineageReleaseTag = releaseTag
        state.lineageAssetName = assetName
        state.lineageInstalledAt = Date()
        saveState()
        objectWillChange.send()
    }

    // MARK: - Utilitare

    private func makeDirectories() throws {
        try FileManager.default.createDirectory(at: guestDir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: downloadsDir, withIntermediateDirectories: true)
    }

    private func replaceItem(at dest: URL, with src: URL) throws {
        if FileManager.default.fileExists(atPath: dest.path) {
            try FileManager.default.removeItem(at: dest)
        }
        try FileManager.default.moveItem(at: src, to: dest)
    }

    private func padPflash(_ url: URL) throws {
        let fh = try FileHandle(forWritingTo: url)
        defer { try? fh.close() }
        let size = try fh.seekToEnd()
        if size < GuestCatalog.pflashSizeBytes {
            try fh.truncate(atOffset: GuestCatalog.pflashSizeBytes)
        }
    }

    private func loadState() {
        if let data = try? Data(contentsOf: stateURL),
           let decoded = try? JSONDecoder.iso.decode(SetupState.self, from: data) {
            state = decoded
        }
    }

    private func saveState() {
        if let data = try? JSONEncoder.iso.encode(state) {
            try? data.write(to: stateURL)
        }
    }
}

private extension JSONEncoder {
    static var pretty: JSONEncoder {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        return e
    }
    static var iso: JSONEncoder {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        return e
    }
}

private extension JSONDecoder {
    static var iso: JSONDecoder {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }
}
