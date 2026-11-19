import SwiftUI
import UIKit
import UniformTypeIdentifiers
import MachO // _dyld_image_* (step-2 ELF symbol resolver)

struct ContentView: View {
    @EnvironmentObject private var controller: VMController

    var body: some View {
        TabView {
            NavigationStack { SetupView() }
                .tabItem { Label("Settings", systemImage: "gear.badge") }
            NavigationStack { AndroidTab() }
                .tabItem { Label("Android", systemImage: "smartphone") }
            NavigationStack { CapabilitiesTab() }
                .tabItem { Label("Capabilities", systemImage: "cpu") }
            NavigationStack { ConsoleTab() }
                .tabItem { Label("Console", systemImage: "terminal") }
        }
    }
}

private struct AndroidTab: View {
    @EnvironmentObject private var controller: VMController
    /// Fullscreen on the guest screen: no status bar, no tab bar,
    /// no navigation bar and no Home indicator. Exit with a
    /// with 3 fingers (or re-enter with it / with the X button) — then all return
    /// controalele gazdei: taburi jos, X, Snapshot/Oprire.
    @State private var guestFullscreen = true
    @State private var keyboardText = ""

    private func sendKeyboard() {
        guard !keyboardText.isEmpty else { return }
        controller.sendKeyboardText(keyboardText)
        keyboardText = ""
    }

    private var isDisplayState: Bool {
        switch controller.state {
        case .booting, .running, .snapshotting: return true
        default: return false
        }
    }
    private var chromeHidden: Bool { isDisplayState && guestFullscreen }

    var body: some View {
        ZStack {
            switch controller.state {
            case .stopped, .failed(_):
                VStack(spacing: 16) {
                    Image(systemName: "play.rectangle.on.rectangle")
                        .font(.system(size: 52)).foregroundStyle(.secondary)
                    Text(controller.stateDescription).multilineTextAlignment(.center)
                    Button("Start Android") {
                        // The button says Android — starts Android, regardless
                        // by the picker in Settings (real trap: picker
                        // left on test + "Start Android" button).
                        UserDefaults.standard.set(GuestKind.android.rawValue, forKey: "guestKind")
                        Task { await controller.boot() }
                    }
                        .buttonStyle(.borderedProminent)
                    Text("Accelerator selected automatically: HVF \u{2192} TCG-JIT (iOS 26/27) \u{2192} TCTI")
                        .font(.caption).foregroundStyle(.secondary)
                }
                .padding()
            case .waitingForJIT:
                VStack(spacing: 16) {
                    ProgressView()
                    Text(controller.stateDescription).multilineTextAlignment(.center)
                }
                .padding()
            case .booting, .running, .snapshotting:
                // Imagine de la boot (animatia Android), nu abia la "running":
                // view-ul trage cadrele din QEMU de indata ce exista scanout.
                GuestDisplayRepresentable(onThreeFingerTap: { guestFullscreen.toggle() })
                    .ignoresSafeArea()
                if !guestFullscreen {
                    VStack {
                        HStack {
                            Spacer()
                            Button { guestFullscreen = true } label: {
                                Image(systemName: "xmark.circle.fill")
                                    .font(.system(size: 30))
                                    .symbolRenderingMode(.hierarchical)
                                    .foregroundStyle(.white, .black.opacity(0.45))
                            }
                            .accessibilityLabel("Re-enter full screen")
                            .padding(.top, 8).padding(.trailing, 10)
                        }
                        Spacer()
                        VStack(spacing: 6) {
                            if controller.state == .booting {
                                Text("Android is starting… (detailed progress in the Console)")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            HStack {
                                Button("Snapshot + Pause") { Task { await controller.snapshotAndPause() } }
                                Button("Resume") { controller.resumeVM() }
                                Button("Stop", role: .destructive) { controller.requestStop() }
                            }
                            // Real keyboard into Android (virtio-keyboard,
                            // 0.2.19): type here, tap "Type" and the text
                            // arrives as key presses in the focused
                            // Android field. "Enter" sends ⏎.
                            HStack(spacing: 6) {
                                TextField("Type into Android…", text: $keyboardText)
                                    .textFieldStyle(.roundedBorder)
                                    .autocorrectionDisabled()
                                    .onSubmit { sendKeyboard() }
                                Button("Type") { sendKeyboard() }
                                Button("Enter") { controller.sendKeyboardText("\n") }
                            }
                            Text("Tap anywhere with 3 fingers for fullscreen")
                                .font(.caption2).foregroundStyle(.secondary)
                        }
                        .buttonStyle(.bordered).padding(8)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                        .padding(.bottom, 10)
                    }
                }
            }
        }
        .navigationTitle("iDroid")
        .statusBarHidden(chromeHidden)
        .persistentSystemOverlays(chromeHidden ? .hidden : .automatic)
        .toolbar(chromeHidden ? .hidden : .visible, for: .tabBar)
        .toolbar(chromeHidden ? .hidden : .visible, for: .navigationBar)
        .animation(.easeInOut(duration: 0.2), value: guestFullscreen)
        .onChange(of: controller.state) { _, newState in
            if newState == .stopped { guestFullscreen = true }
        }
    }
}

private struct CapabilitiesTab: View {
    @EnvironmentObject private var controller: VMController
    // Flags: the supported daily profile stays software + g0. A NEW,
    // explicitly isolated one-shot ANGLE OpenGL trial is opt-in; the old
    // persistent Metal/GL switch remains removed. Stale UserDefaults from
    // older builds cannot silently enable GPU acceleration.
    @AppStorage("dnGPUTrialNextBoot") private var dnGPUTrialNextBoot = false
    @AppStorage("dnParallelGPUTrialNextBoot") private var dnParallelGPUTrialNextBoot = false
    @AppStorage("dnParallelCPUCount") private var dnParallelCPUCount = 2
    @AppStorage("dnIOSurfaceFastPath") private var dnIOSurfaceFastPath = true
    @AppStorage("dnLoadSnapshot") private var dnLoadSnapshot = false
    @AppStorage("dnDisplayScale") private var dnDisplayScale = "normala"
    @AppStorage("dnGuestRAM") private var dnGuestRAM = 0
    @AppStorage("dnJITArenaMB") private var dnJITArenaMB = 0
    @AppStorage("dnGuestDPI") private var dnGuestDPI = 560
    @AppStorage("dnGuestRefreshRateHz") private var dnGuestRefreshRateHz = 75
    @State private var importingPairing = false
    @State private var importingDylib = false
    @State private var helloInfo = ""
    @State private var helloStatus = ""
    @State private var importingNativeLinkDylibs = false
    @State private var nativeLinkInfo = ""
    @State private var nativeLinkStatus = ""
    @State private var importingNativeVulkanDylib = false
    @State private var nativeVulkanInfo = ""
    @State private var nativeVulkanStatus = ""
    @State private var importingNativeFullDylib = false
    @State private var nativeFullInfo = ""
    @State private var nativeFullStatus = ""
    @State private var nativeFullBreadcrumb = ""

    /// Step 2 (docs/13): every result line lands both in this view and
    /// in the persisted console (same diag channel as pointer/census),
    /// so the verdict is visible in the next console export.
    private func helloLog(_ line: String) {
        helloStatus = line
        NotificationCenter.default.post(name: .dnPointerDiag, object: "[native-hello] " + line)
    }

    private func importHelloDylib(from url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            let dst = dnHelloDylibURL()
            try FileManager.default.createDirectory(at: dst.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: dst.path) {
                try FileManager.default.removeItem(at: dst)
            }
            try data.write(to: dst, options: [.atomic])
            helloInfo = dnMachOPeek(data)
            helloLog("imported \(data.count) bytes from \(url.lastPathComponent). " + helloInfo)
        } catch {
            helloLog("import failed: \(error.localizedDescription)")
        }
    }

    private func runHelloTest() {
        for line in dnHelloRunTest(dylibPath: dnHelloDylibURL().path) {
            helloLog(line)
        }
    }

    private func nativeLinkLog(_ line: String) {
        nativeLinkStatus = line
        NotificationCenter.default.post(name: .dnPointerDiag, object: "[native-link] " + line)
    }

    private func importNativeLinkDylibs(from urls: [URL]) {
        let accepted: Set<String> = ["libdnbase.dylib", "libdnlink.dylib"]
        var infos: [String] = []
        for url in urls {
            guard accepted.contains(url.lastPathComponent) else {
                nativeLinkLog("ignored \(url.lastPathComponent) — select libdnbase.dylib and libdnlink.dylib")
                continue
            }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do {
                let data = try Data(contentsOf: url)
                let dst = dnNativeLibraryURL(url.lastPathComponent)
                try FileManager.default.createDirectory(at: dst.deletingLastPathComponent(),
                                                        withIntermediateDirectories: true)
                if FileManager.default.fileExists(atPath: dst.path) {
                    try FileManager.default.removeItem(at: dst)
                }
                try data.write(to: dst, options: [.atomic])
                let info = dnMachOPeek(data)
                infos.append("\(url.lastPathComponent): \(info)")
                nativeLinkLog("imported \(data.count) bytes from \(url.lastPathComponent). " + info)
            } catch {
                nativeLinkLog("import \(url.lastPathComponent) failed: \(error.localizedDescription)")
            }
        }
        if !infos.isEmpty {
            nativeLinkInfo = infos.joined(separator: "; ")
        }
    }

    private func runNativeLinkTest() {
        for line in dnNativeLinkRunTest() {
            nativeLinkLog(line)
        }
    }

    private func nativeVulkanLog(_ line: String) {
        nativeVulkanStatus = line
        NotificationCenter.default.post(name: .dnPointerDiag, object: "[native-vulkan] " + line)
    }

    private func importNativeVulkanDylib(from url: URL) {
        guard url.lastPathComponent == "libdnvk.dylib" else {
            nativeVulkanLog("ignored \(url.lastPathComponent) — select libdnvk.dylib")
            return
        }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            let dst = dnNativeLibraryURL(url.lastPathComponent)
            try FileManager.default.createDirectory(at: dst.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: dst.path) {
                try FileManager.default.removeItem(at: dst)
            }
            try data.write(to: dst, options: [.atomic])
            nativeVulkanInfo = dnMachOPeek(data)
            nativeVulkanLog("imported \(data.count) bytes from \(url.lastPathComponent). " + nativeVulkanInfo)
        } catch {
            nativeVulkanLog("import failed: \(error.localizedDescription)")
        }
    }

    private func runNativeVulkanHostSelfTest() {
        for line in dnNativeVulkanRunHostSelfTest() {
            nativeVulkanLog(line)
        }
    }

    private func runNativeVulkanFixtureTest() {
        for line in dnNativeVulkanFixtureRunTest() {
            nativeVulkanLog(line)
        }
    }

    private func nativeFullLog(_ line: String) {
        nativeFullStatus = line
        NotificationCenter.default.post(name: .dnPointerDiag, object: "[native-full] " + line)
    }

    private func importNativeFullDylib(from url: URL) {
        guard url.lastPathComponent == "libdnfull.dylib" else {
            nativeFullLog("ignored \(url.lastPathComponent) — select libdnfull.dylib")
            return
        }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            let dst = dnNativeLibraryURL(url.lastPathComponent)
            try FileManager.default.createDirectory(at: dst.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: dst.path) {
                try FileManager.default.removeItem(at: dst)
            }
            try data.write(to: dst, options: [.atomic])
            nativeFullInfo = dnMachOPeek(data)
            nativeFullLog("imported \(data.count) bytes from \(url.lastPathComponent). " + nativeFullInfo)
        } catch {
            nativeFullLog("import failed: \(error.localizedDescription)")
        }
    }

    private func runNativeFullHostSelfTest() {
        for line in dnNativeFullRunHostSelfTest() {
            nativeFullLog(line)
        }
    }

    private func runNativeFullFixtureTest() {
        if let previous = dnNativeFullReadBreadcrumb(), previous != "complete" {
            controller.appendPersistentDiagnosticLine(
                "[native-full] previous complete-native run stopped at: \(previous)")
        }
        controller.appendPersistentDiagnosticLine(
            "[native-full] starting complete native fixture run")
        for line in dnNativeFullFixtureRunTest() {
            nativeFullLog(line)
        }
        nativeFullBreadcrumb = dnNativeFullReadBreadcrumb() ?? ""
    }

    var body: some View {
        List {
            Section("Flags") {
                Toggle("Resume from snapshot at start", isOn: $dnLoadSnapshot)
                Toggle("Isolated GPU trial (next start only)", isOn: $dnGPUTrialNextBoot)
                Text("ANGLE OpenGL/EAGL with guest Mesa VirGL verified by SurfaceFlinger. Uses separate APFS work-disk copies; main Android data and software snapshot stay untouched. No snapshot/instant resume in the GPU trial. The trial copy is kept separately, not synced back. One shot; next start defaults to software. Smooth post-boot UI is NOT verified.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Parallel CPU + GPU trial (next start only)", isOn: $dnParallelGPUTrialNextBoot)
                Picker("Parallel trial vCPUs", selection: $dnParallelCPUCount) {
                    Text("2 vCPU (device-tested)").tag(2)
                    Text("3 vCPU (experimental)").tag(3)
                    Text("4 vCPU (experimental)").tag(4)
                }
                Text("Requires an existing GPU trial. Every CPU count has a SEPARATE APFS clone of its work disks: 2 reuses your current ParallelGPUTrial, while 3/4 start new clones from GPUTrial. Runs MTTCG (icount off) with EAGL; the real-time Android watchdog may restart system_server or prevent boot. More vCPUs can also make UI slower through host contention or heat. Clones grow separately on disk; none changes your software VM, GPUTrial, other parallel clones or g0 snapshot. This picker affects only the one-shot parallel trial; 2 remains the tested default. If both trial switches are on, the parallel trial takes priority. 60/120 FPS is not measured or promised.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("GPU IOSurface fast path (2 vCPU only)", isOn: $dnIOSurfaceFastPath)
                Text("Uses the existing EAGL/Mesa VirGL GPU clone. If supported, a fenced four-slot IOSurface pool replaces full-frame GPU readback and CPU-to-Metal uploads; it never changes the Android driver or disks. Configuration, EGL and Metal failures revert to the proven PBO path. Disable here to force PBO. No iPhone FPS or screen-correctness validation was performed for this new path.")
                    .font(.caption).foregroundStyle(.secondary)
                Picker("Guest resolution", selection: $dnDisplayScale) {
                    Text("1320×2868 native (full screen)").tag("nativa")
                    Text("720×1280").tag("normala")
                    Text("540×960 (low resolution)").tag("redusa")
                }
                Picker("Android text size (guest DPI)", selection: $dnGuestDPI) {
                    Text("420 (smaller)").tag(420)
                    Text("480").tag(480)
                    Text("560 (default)").tag(560)
                    Text("640 (largest)").tag(640)
                }
                Picker("Guest refresh (EDID)", selection: $dnGuestRefreshRateHz) {
                    Text("75 Hz (default, legacy timing)").tag(75)
                    Text("55 Hz (slower test, did not help)").tag(55)
                }
                Picker("Guest RAM", selection: $dnGuestRAM) {
                    Text("Automatic (4096 MB)").tag(0)
                    Text("2048 MB (economical)").tag(2048)
                    Text("3072 MB").tag(3072)
                    Text("4096 MB").tag(4096)
                }
                Picker("JIT arena (translation cache)", selection: $dnJITArenaMB) {
                    Text("384 MB (default)").tag(0)
                    Text("768 MB").tag(768)
                    Text("1024 MB").tag(1024)
                }
                LabeledContent("Virtual time", value: "icount on (fixed); parallel trial opt-in")
                LabeledContent("Android graphics", value: "software default; isolated GPU trial optional")
                LabeledContent("QEMU JIT acquisition", value: "built-in helper only")
                Text("These settings apply on the next Android start. The 55 Hz EDID mode did not improve responsiveness on the device; use 75 Hz for the previous timing. Changing only refresh keeps a matching software snapshot; changing RAM or resolution requires a full boot. Neither refresh option enables Android GPU acceleration. The separate 2-vCPU parallel GPU trial felt more responsive on iPhone, but Android FPS and long-term watchdog stability remain unmeasured.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Device / iOS") {
                LabeledContent("Model", value: controller.probe.deviceModel)
                LabeledContent("Physical RAM", value: controller.probe.physicalMemoryText)
                LabeledContent("RAM available to the process", value: controller.probe.availableMemoryText)
                LabeledContent("Planned guest RAM", value: controller.probe.plannedGuestMemoryText)
            }
            Section("Acceleration") {
                LabeledContent("Hypervisor (EL2)", value: controller.probe.hypervisorText)
                LabeledContent("TXM/SPTM detected", value: controller.probe.txmPresent ? "Yes (A15+)" : "No")
                LabeledContent("get-task-allow (signing)", value: controller.probe.hasGetTaskAllow ? "Yes — JIT can be attached" : "No — JIT impossible under this signing")
                LabeledContent("Debugged flag (CS_DEBUGGED)", value: controller.probe.isDebugged ? "Yes (can linger after detach)" : "No")
                LabeledContent("Live debugger now (P_TRACED)", value: controller.probe.isTraced ? "Yes" : "No")
                LabeledContent("Code-mapping probe (automatic, safe)", value: controller.probe.classicJITMapText)
                LabeledContent("Chosen accelerator", value: controller.probe.chosenAccelerator.rawValue)
                LabeledContent("Bundled QEMU engine", value: controller.qemuEngineText)
            }
            Section("Built-in JIT") {
                LabeledContent("Method", value: "Built-in helper (fixed)")
                LabeledContent("Built-in helper", value: controller.builtInJITDiagnostic)
                LabeledContent("LocalDevVPN", value: controller.localDevVPNStatus)
                Button("Check LocalDevVPN") { Task { await controller.checkLocalDevVPN() } }
                LabeledContent("Pairing (Keychain)", value: controller.hasStoredPairing ? "saved" : "not imported")
                Button(controller.hasStoredPairing ? "Replace pairing file…" : "Import pairing file…") {
                    importingPairing = true
                }
                if controller.hasStoredPairing {
                    Button("Delete pairing", role: .destructive) { controller.deletePairingFile() }
                }
                Button("Prepare built-in JIT (DDI)") { Task { await controller.prepareBuiltInJIT() } }
                    .disabled(!controller.builtInJITAvailable || !controller.hasStoredPairing)
                Button("Reset DDI (recovery)") { Task { await controller.resetBuiltInDDI() } }
                    .disabled(!controller.builtInJITAvailable)
                Text(controller.builtInJITStatus)
                    .font(.caption).foregroundStyle(.secondary)
                if controller.builtInJITAvailable {
                    Text("JIT is acquired strictly from iDroid's own helper. Platform conditions still apply: LocalDevVPN connected, pairing for this iPhone, and development signing with get-task-allow.")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    Text("The helper is not in the installed bundle: in Xcode, the DroidNativeJITHelper target must be signed with the same Personal Team and embedded in the app.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Native translation test (docs/13 step 2)") {
                Button("Import libdnhello.dylib…") { importingDylib = true }
                Button("Load and call (dlopen)") { runHelloTest() }
                if !helloInfo.isEmpty {
                    Text(helloInfo).font(.caption).foregroundStyle(.secondary)
                }
                if !helloStatus.isEmpty {
                    Text(helloStatus).font(.caption).foregroundStyle(.secondary)
                }
                Text("The step 2 spike: a tiny ARM64 ELF library converted to Mach-O and signed with your team identity on the Mac (scripts/make-hello-native.sh) must load from this app's container and run with no JIT and no debugger. Import the .dylib, load it, and read the verdict here and in the console export ([native-hello] lines). Also try it after launching iDroid from the home screen — that is the platform question this test answers.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Native runtime linker (docs/13 step 2.5)") {
                Button("Import libdnbase + libdnlink…") { importingNativeLinkDylibs = true }
                Button("Link and call (own mini-linker)") { runNativeLinkTest() }
                if !nativeLinkInfo.isEmpty {
                    Text(nativeLinkInfo).font(.caption).foregroundStyle(.secondary)
                }
                if !nativeLinkStatus.isEmpty {
                    Text(nativeLinkStatus).font(.caption).foregroundStyle(.secondary)
                }
                Text("DroidNative's own ELF mini-linker links two dependent converted libraries: DT_NEEDED, imported functions/data, RELA/RELR relocations, constructors and destructors, then direct native calls. Build the pair with scripts/make-link-native.sh. Results are persisted as [native-link] console lines.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Native Vulkan GPU driver (iDroid 0.3)") {
                Button("Run Vulkan GPU driver self-test") { runNativeVulkanHostSelfTest() }
                Button("Import libdnvk.dylib…") { importingNativeVulkanDylib = true }
                Button("Link and run Vulkan fixture") { runNativeVulkanFixtureTest() }
                if !nativeVulkanInfo.isEmpty {
                    Text(nativeVulkanInfo).font(.caption).foregroundStyle(.secondary)
                }
                if !nativeVulkanStatus.isEmpty {
                    Text(nativeVulkanStatus).font(.caption).foregroundStyle(.secondary)
                }
                Text("DroidNative's own Vulkan-on-Metal driver for translated native code: device/queue, unified memory, buffers, 2D images, command buffers, fences and GPU copy/fill/clear commands. Build the guest fixture with scripts/make-vulkan-native.sh. Results are persisted as [native-vulkan] console lines. Shader pipelines and presentation are not silently faked: unsupported vk* imports fail closed until their driver layers land.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Complete native translation runtime (iDroid 0.4)") {
                Button("Run native runtime self-test") { runNativeFullHostSelfTest() }
                Button("Import libdnfull.dylib…") { importingNativeFullDylib = true }
                Button("Link and run complete native fixture") { runNativeFullFixtureTest() }
                if !nativeFullInfo.isEmpty {
                    Text(nativeFullInfo).font(.caption).foregroundStyle(.secondary)
                }
                if !nativeFullStatus.isEmpty {
                    Text(nativeFullStatus).font(.caption).foregroundStyle(.secondary)
                }
                if !nativeFullBreadcrumb.isEmpty, nativeFullBreadcrumb != "complete" {
                    Text("Previous complete-native run stopped at: \(nativeFullBreadcrumb)")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text("The complete direct-translation path: converted ARM64 code, DN's own mini-linker, patch-island svc/tpidr handling, ELF TLS, bionic threads/files, libandroid assets/input/window, NativeActivity lifecycle and Vulkan-on-Metal. Build the fixture with scripts/make-full-native.sh. Results are persisted as [native-full] console lines, and the last completed stage is kept in the diagnostic export even if guest code crashes the process; overall PASS is the device acceptance gate.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Thermal state") {
                LabeledContent("Level", value: controller.probe.thermalText)
            }
        }
        .navigationTitle("Capabilities")
        .refreshable { controller.refreshProbe() }
        .task {
            controller.refreshProbe()
            if let data = try? Data(contentsOf: dnHelloDylibURL()) {
                helloInfo = dnMachOPeek(data)
            }
            let nativeInfos = ["libdnbase.dylib", "libdnlink.dylib"].compactMap { name -> String? in
                guard let data = try? Data(contentsOf: dnNativeLibraryURL(name)) else { return nil }
                return "\(name): \(dnMachOPeek(data))"
            }
            if !nativeInfos.isEmpty {
                nativeLinkInfo = nativeInfos.joined(separator: "; ")
            }
            if let data = try? Data(contentsOf: dnNativeLibraryURL("libdnvk.dylib")) {
                nativeVulkanInfo = dnMachOPeek(data)
            }
            if let data = try? Data(contentsOf: dnNativeLibraryURL("libdnfull.dylib")) {
                nativeFullInfo = dnMachOPeek(data)
            }
            nativeFullBreadcrumb = dnNativeFullReadBreadcrumb() ?? ""
        }
        .fileImporter(isPresented: $importingPairing, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                controller.importPairingFile(from: url)
            }
        }
        .fileImporter(isPresented: $importingDylib, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                importHelloDylib(from: url)
            }
        }
        .fileImporter(isPresented: $importingNativeLinkDylibs, allowedContentTypes: [.data], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result {
                importNativeLinkDylibs(from: urls)
            }
        }
        .fileImporter(isPresented: $importingNativeVulkanDylib, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                importNativeVulkanDylib(from: url)
            }
        }
        .fileImporter(isPresented: $importingNativeFullDylib, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                importNativeFullDylib(from: url)
            }
        }
    }
}

private struct ConsoleTab: View {
    @EnvironmentObject private var controller: VMController
    @State private var followTail = true
    @State private var exportingLog = false
    @State private var exportDocument = ConsoleLogDocument(text: "")
    @State private var exportFilename = "droidnative-console"
    @State private var consoleInput = ""

    var body: some View {
        VStack(spacing: 0) {
            ConsoleTextView(
                text: controller.serialLog.isEmpty ? "(the guest serial console will appear here)" : controller.serialLog,
                followTail: followTail
            )
            // Input to serial: on the current GSI the serial has NO shell
            // (androidboot.console=hvc0; the PL011 is log-only) — that is why
            // typing here has no effect in Android. For typing in
            // Android uses the keyboard field in the Android tab
            // (virtio keyboard, 0.2.19). The field stays for images
            // userdebug images with a serial shell.
            HStack(spacing: 8) {
                TextField("Bytes to serial (current GSI: no shell)…", text: $consoleInput)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.footnote, design: .monospaced))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .onSubmit { sendConsoleLine() }
                Button("Send") { sendConsoleLine() }
                    .buttonStyle(.bordered)
            }
            .padding(8)
        }
        .navigationTitle("Serial console")
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                // Export .txt to Files: console + breadcrumb + qemu.log,
                // ready to send for diagnostics (no screen photos).
                Button {
                    let nameFormatter = DateFormatter()
                    nameFormatter.dateFormat = "yyyyMMdd-HHmmss"
                    exportFilename = "droidnative-console-\(nameFormatter.string(from: Date()))"
                    exportDocument = ConsoleLogDocument(text: controller.makeConsoleExportText())
                    exportingLog = true
                } label: {
                    Label("Export log (.txt)", systemImage: "square.and.arrow.up")
                        .labelStyle(.iconOnly)
                }
                Button {
                    followTail.toggle()
                } label: {
                    Label(followTail ? "Auto-scroll: on" : "Auto-scroll: off",
                          systemImage: followTail ? "arrow.down.to.line.circle.fill" : "arrow.down.to.line.circle")
                        .labelStyle(.iconOnly)
                }
            }
        }
        .fileExporter(isPresented: $exportingLog,
                      document: exportDocument,
                      contentType: .plainText,
                      defaultFilename: exportFilename) { _ in }
    }

    private func sendConsoleLine() {
        controller.sendConsoleInput(consoleInput)
        consoleInput = ""
        followTail = true   // the guest response lands at the tail
    }
}

/// UITextView console: incremental TextKit layout + delta-only append.
/// The old single SwiftUI `Text` re-laid-out the whole log (up to
/// 256 KB) la fiecare publicare — cauza sacadarii intregii interfete la boot.
private struct ConsoleTextView: UIViewRepresentable {
    let text: String
    let followTail: Bool

    private static let consoleFont = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)

    func makeUIView(context: Context) -> UITextView {
        let textView = UITextView()
        textView.isEditable = false
        textView.isSelectable = true
        textView.alwaysBounceVertical = true
        textView.backgroundColor = .systemBackground
        textView.textContainerInset = UIEdgeInsets(top: 8, left: 6, bottom: 8, right: 6)
        textView.font = Self.consoleFont
        textView.textColor = .label
        textView.autocorrectionType = .no
        textView.autocapitalizationType = .none
        textView.dataDetectorTypes = []
        // Lazy layout: TextKit computes only the needed region, not the whole log.
        textView.layoutManager.allowsNonContiguousLayout = true
        textView.text = text
        return textView
    }

    func updateUIView(_ textView: UITextView, context: Context) {
        let newText = text as NSString
        let oldText = (textView.text ?? "") as NSString
        if newText.length >= oldText.length,
           newText.substring(to: oldText.length) == (oldText as String) {
            // Normal case: just append the new tail, no global relayout.
            let delta = newText.substring(from: oldText.length)
            if !delta.isEmpty {
                let attrs: [NSAttributedString.Key: Any] = [
                    .font: Self.consoleFont,
                    .foregroundColor: UIColor.label,
                ]
                textView.textStorage.append(NSAttributedString(string: delta, attributes: attrs))
            }
        } else if newText != oldText {
            // Init sau taierea ferestrei vizibile (prefixul s-a schimbat).
            textView.text = text
        }
        if followTail, newText.length > 0 {
            textView.scrollRangeToVisible(NSRange(location: newText.length, length: 0))
        }
    }
}

private struct ConsoleLogDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.plainText] }

    var text: String

    init(text: String) {
        self.text = text
    }

    init(configuration: ReadConfiguration) throws {
        if let data = configuration.file.regularFileContents {
            text = String(decoding: data, as: UTF8.self)
        } else {
            text = ""
        }
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data(text.utf8))
    }
}


// MARK: - Native translation test (docs/13 step 2, "hello nativ")

/// Where the imported test library lives inside the app container.
/// dyld loads same-team-signed code from here without any debugger
/// (that is exactly what step 2 measures).
private func dnHelloDylibURL() -> URL {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
        ?? FileManager.default.temporaryDirectory
    return base.appendingPathComponent("DroidNative/native-hello/libdnhello.dylib")
}

/// One-line Mach-O description: architecture, file type, code
/// signature presence, install name. Pure byte math over the 64-bit
/// header + load commands (logic validated against a real arm64
/// dylib before mirroring it here).
private func dnMachOPeek(_ data: Data) -> String {
    func u32(_ off: Int) -> UInt32? {
        guard off >= 0, off + 4 <= data.count else { return nil }
        return UInt32(data[off]) | (UInt32(data[off + 1]) << 8)
            | (UInt32(data[off + 2]) << 16) | (UInt32(data[off + 3]) << 24)
    }
    guard let magic = u32(0) else { return "file too small (\(data.count) bytes)" }
    guard magic == 0xFEED_FACF else {
        return String(format: "NOT a Mach-O 64-bit file (magic 0x%08x, %d bytes) — was it converted?",
                      magic, data.count)
    }
    let cpu = u32(4) ?? 0
    let cpuName = cpu == 0x0100_000C ? "arm64" : String(format: "cpu 0x%x", cpu)
    let ftype = u32(12) ?? 0
    let typeName = ftype == 6 ? "dylib" : "filetype \(ftype)"
    let ncmds = Int(u32(16) ?? 0)
    var sig = false
    var dyid: String?
    var off = 32
    for _ in 0 ..< ncmds {
        guard let cmd = u32(off), let size = u32(off + 4), size >= 8 else { break }
        if cmd == 0x1D { sig = true } // LC_CODE_SIGNATURE
        if cmd == 0x0D, let noff = u32(off + 8) { // LC_ID_DYLIB
            let start = off + Int(noff)
            if start >= off, start < data.count {
                var end = start
                while end < data.count, data[end] != 0 { end += 1 }
                dyid = String(bytes: data[start ..< end], encoding: .utf8)
            }
        }
        off += Int(size)
    }
    return "Mach-O \(cpuName) \(typeName), \(ncmds) load commands, code signature \(sig ? "present" : "MISSING")"
        + (dyid.map { ", id \($0)" } ?? "") + " (\(data.count) bytes)"
}

/// henge-converted libraries expose no symbols to dyld: the Mach-O
/// carries a single marker symbol, while the real exports live in the
/// dynamic symbol table of the ELF image embedded in the
/// `__TEXT,__elf` section. The converter's layout contract is that
/// every ELF virtual address V lives at Mach-O vmaddr V + H, where H
/// is the vmaddr of that section. This resolver takes H from the
/// file, adds the ASLR slide, then reads the embedded ELF's
/// PT_DYNAMIC/.dynsym straight from the mapped image and returns
/// live addresses. The step-2 fixture is dependency-free (no
/// imports, no relocations, no initializers), so direct calls are
/// valid; a general mini linker is later native-mode work.
private func dnHelloELFSymbols(dylibPath: String) -> [String: UnsafeMutableRawPointer] {
    guard let data = try? Data(contentsOf: URL(fileURLWithPath: dylibPath)) else { return [:] }
    func fu32(_ off: Int) -> UInt32? {
        guard off >= 0, off + 4 <= data.count else { return nil }
        return UInt32(data[off]) | (UInt32(data[off + 1]) << 8)
            | (UInt32(data[off + 2]) << 16) | (UInt32(data[off + 3]) << 24)
    }
    func fu64(_ off: Int) -> UInt64? {
        guard let lo = fu32(off), let hi = fu32(off + 4) else { return nil }
        return UInt64(lo) | (UInt64(hi) << 32)
    }
    func fname(_ off: Int) -> String {
        var bytes: [UInt8] = []
        var i = off
        while i < off + 16, i < data.count, data[i] != 0 { bytes.append(data[i]); i += 1 }
        return String(bytes: bytes, encoding: .utf8) ?? ""
    }
    guard fu32(0) == 0xFEED_FACF, let ncmds = fu32(16) else { return [:] }
    var hAddr: UInt64 = 0
    var off = 32
    for _ in 0 ..< ncmds {
        guard let cmd = fu32(off), let size = fu32(off + 4), size >= 8 else { break }
        if cmd == 0x19 { // LC_SEGMENT_64
            let segname = fname(off + 8)
            let nsects = Int(fu32(off + 64) ?? 0)
            var so = off + 72
            for _ in 0 ..< nsects {
                if segname == "__TEXT" && fname(so) == "__elf" {
                    hAddr = fu64(so + 32) ?? 0 // section_64.addr
                }
                so += 80
            }
        }
        off += Int(size)
    }
    guard hAddr != 0 else { return [:] }
    var slide = 0
    for i in 0 ..< _dyld_image_count() {
        if let n = _dyld_get_image_name(i), String(cString: n).hasSuffix("/libdnhello.dylib") {
            slide = _dyld_get_image_vmaddr_slide(i)
            break
        }
    }
    guard let base = UnsafeRawPointer(bitPattern: UInt(bitPattern: slide) &+ UInt(hAddr)) else { return [:] }
    func m16(_ o: Int) -> UInt16 { base.loadUnaligned(fromByteOffset: o, as: UInt16.self) }
    func m32(_ o: Int) -> UInt32 { base.loadUnaligned(fromByteOffset: o, as: UInt32.self) }
    func m64(_ o: Int) -> UInt64 { base.loadUnaligned(fromByteOffset: o, as: UInt64.self) }
    guard m32(0) == 0x464C_457F else { return [:] } // 0x7F + "ELF"
    let phoff = Int(m64(0x20))
    let phnum = Int(m16(0x38))
    var symtab: UInt64 = 0, strtab: UInt64 = 0, hash: UInt64 = 0, gnuHash: UInt64 = 0
    for i in 0 ..< phnum {
        let p = phoff + i * 56
        guard m32(p) == 2 else { continue } // PT_DYNAMIC
        var d = Int(m64(p + 16)) // p_vaddr
        for _ in 0 ..< 512 {
            let tag = Int64(bitPattern: m64(d))
            let val = m64(d + 8)
            if tag == 0 { break }
            switch tag {
            case 5: strtab = val // DT_STRTAB
            case 6: symtab = val // DT_SYMTAB
            case 4: hash = val // DT_HASH
            case 0x6FFF_FEF5: gnuHash = val // DT_GNU_HASH
            default: break
            }
            d += 16
        }
    }
    guard symtab != 0, strtab != 0 else { return [:] }
    var count = 0
    if hash != 0 {
        count = Int(m32(Int(hash) + 4)) // DT_HASH: [nbucket, nchain, ...]
    } else if gnuHash != 0 {
        let g = Int(gnuHash)
        let nbuckets = Int(m32(g))
        let symoffset = Int(m32(g + 4))
        let bloomSize = Int(m32(g + 8))
        let buckets = g + 16 + bloomSize * 8
        let chain = buckets + nbuckets * 4
        for b in 0 ..< nbuckets {
            var idx = Int(m32(buckets + b * 4))
            if idx == 0 { continue }
            while m32(chain + (idx - symoffset) * 4) & 1 == 0 { idx += 1 }
            count = max(count, idx + 1)
        }
    }
    guard count > 0 else { return [:] }
    var result: [String: UnsafeMutableRawPointer] = [:]
    for j in 0 ..< count {
        let s = Int(symtab) + j * 24
        let stName = Int(m32(s))
        let stValue = m64(s + 8)
        guard stName > 0, m16(s + 6) != 0, stValue != 0 else { continue }
        let name = String(cString: base.advanced(by: Int(strtab) + stName).assumingMemoryBound(to: CChar.self))
        if !name.isEmpty {
            result[name] = UnsafeMutableRawPointer(mutating: base.advanced(by: Int(stValue)))
        }
    }
    return result
}

/// The step 2 gate itself: dlopen the converted library from the
/// container and call its functions directly. Expected values are
/// from translation/hello/dn_hello.c.
private func dnHelloRunTest(dylibPath: String) -> [String] {
    var out: [String] = []
    guard FileManager.default.fileExists(atPath: dylibPath) else {
        return ["nothing imported yet — use Import first"]
    }
    out.append("dlopen \(dylibPath)")
    _ = dlerror()
    guard let handle = dlopen(dylibPath, RTLD_NOW | RTLD_LOCAL) else {
        out.append("dlopen FAILED: \(dlerror().map { String(cString: $0) } ?? "unknown error")")
        return out
    }
    out.append("dlopen OK")
    var elfSyms: [String: UnsafeMutableRawPointer]? = nil
    func sym(_ name: String) -> UnsafeMutableRawPointer? {
        if let p = dlsym(handle, name) { return p }
        if elfSyms == nil {
            elfSyms = dnHelloELFSymbols(dylibPath: dylibPath)
            if let found = elfSyms, !found.isEmpty {
                out.append("no Mach-O exports (henge layout) - resolved \(found.count) symbols from the embedded ELF .dynsym")
            }
        }
        return elfSyms?[name]
    }
    var ok = true
    if let p = sym("dn_hello_answer") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> Int32).self)
        let v = f()
        out.append("dn_hello_answer() = \(v) (expect 42)")
        ok = ok && v == 42
    } else { out.append("resolve dn_hello_answer FAILED"); ok = false }
    if let p = sym("dn_hello_add") {
        let f = unsafeBitCast(p, to: (@convention(c) (Int32, Int32) -> Int32).self)
        let v = f(19, 23)
        out.append("dn_hello_add(19, 23) = \(v) (expect 42)")
        ok = ok && v == 42
    } else { out.append("resolve dn_hello_add FAILED"); ok = false }
    if let p = sym("dn_hello_fib") {
        let f = unsafeBitCast(p, to: (@convention(c) (Int32) -> Int64).self)
        let v = f(20)
        out.append("dn_hello_fib(20) = \(v) (expect 6765)")
        ok = ok && v == 6765
    } else { out.append("resolve dn_hello_fib FAILED"); ok = false }
    if let p = sym("dn_hello_text") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> UnsafePointer<CChar>?).self)
        if let cs = f() {
            out.append("dn_hello_text() = \"\(String(cString: cs))\"")
        } else {
            out.append("dn_hello_text() returned NULL")
        }
    } else { out.append("resolve dn_hello_text FAILED (bonus symbol)") }
    dlclose(handle)
    out.append(ok ? "PASS — translated library loaded and executed natively in-process (docs/13 step 2)"
                  : "FAIL — see lines above")
    return out
}
