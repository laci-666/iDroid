import SwiftUI
import UniformTypeIdentifiers

/// The Settings tab: automatic download, manual package import and
/// reset. Android is the only supported guest; the old development
/// test-Linux selector was removed in iDroid 0.4.
struct SetupView: View {
    @EnvironmentObject private var setup: GuestSetupManager
    @EnvironmentObject private var controller: VMController

    @State private var showingImporter = false
    @State private var confirmFactoryReset = false
    @State private var confirmDeleteAll = false

    var body: some View {
        List {
            statusSection
            androidSection
            dangerSection
            versionSection
        }
        .navigationTitle("Setup")
        .fileImporter(isPresented: $showingImporter, allowedContentTypes: [.zip]) { result in
            if case .success(let url) = result {
                setup.importPackage(from: url, probe: controller.probe)
            }
        }
        .confirmationDialog("Reset to factory settings?",
                            isPresented: $confirmFactoryReset) {
            Button("Yes, reset (keep the base image)", role: .destructive) {
                setup.factoryReset(probe: controller.probe)
            }
        } message: {
            Text("Android data and apps are lost; the base image remains, so you re-download nothing.")
        }
        .confirmationDialog("Delete Android completely from this phone?",
                            isPresented: $confirmDeleteAll) {
            Button("Yes, delete everything", role: .destructive) { setup.deleteEverything() }
        } message: {
            Text("Including the downloaded image (~3 GB). The next setup starts from zero.")
        }
    }

    // MARK: - Sections

    private var statusSection: some View {
        Section("Status") {
            LabeledContent("Android (LineageOS)",
                           value: setup.androidInstalled ? "installed \(setup.state.lineageReleaseTag ?? "")" : "not installed")
            LabeledContent("Free space",
                           value: ByteCountFormatter.string(fromByteCount: Int64(setup.freeDiskBytes), countStyle: .file))
            if !setup.statusLine.isEmpty {
                Text(setup.statusLine).font(.callout).foregroundStyle(.secondary)
            }
            phaseView
        }
    }

    @ViewBuilder
    private var phaseView: some View {
        switch setup.phase {
        case .downloading(let f), .extracting(let f):
            ProgressView(value: f) {
                Text(setup.phase == .downloading(f) ? "Downloading" : "Extracting")
            } currentValueLabel: {
                Text("\(Int(f * 100))%")
            }
            Button("Cancel (can resume)", role: .destructive) { setup.cancel() }
        case .resolving, .verifying, .preparing, .writingConfig:
            HStack { ProgressView(); Text("Working…") }
        case .failed(let message):
            Label(message, systemImage: "exclamationmark.triangle")
                .foregroundStyle(.orange)
        case .idle, .finished:
            EmptyView()
        }
    }

    private var androidSection: some View {
        Section {
            Button {
                controller.refreshProbe()
                setup.startAutomaticSetup(probe: controller.probe)
            } label: {
                Label(setup.androidInstalled ? "Re-download and re-set up Android" : "Automatic setup — download Android",
                      systemImage: "arrow.down.circle.fill")
            }
            .disabled(setup.phase.isBusy)

            Button {
                controller.refreshProbe()
                setup.startAutomaticSetup(probe: controller.probe, releaseOffset: 1)
            } label: {
                Label("Install the previous version (if the latest does not start)", systemImage: "arrow.uturn.backward.circle")
            }
            .disabled(setup.phase.isBusy)

            Button {
                showingImporter = true
            } label: {
                Label("Import package from Files (.zip)", systemImage: "folder")
            }
            .disabled(setup.phase.isBusy)
        } header: {
            Text("Full Android")
        } footer: {
            Text("Downloads the latest LineageOS for QEMU (virtio ARM64, ~1.1 GB), verifies it (SHA256), prepares the disks and firmware, then configures RAM/cores for your phone by itself. Recommended on Wi-Fi, with the app open.")
        }
    }

    private var dangerSection: some View {
        Section {
            Button("Factory reset", role: .destructive) { confirmFactoryReset = true }
            Button("Delete the whole guest", role: .destructive) { confirmDeleteAll = true }
        } header: {
            Text("Danger zone")
        }
    }

    /// Jos de tot: numarul buildului instalat (MARKETING_VERSION), ca sa
    /// we always know which version is actually running on the phone.
    private var versionSection: some View {
        Section {
            LabeledContent("Version", value: "iDroid \(VMController.appVersionDisplay)")
        }
    }
}
