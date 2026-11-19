import Foundation
#if !targetEnvironment(simulator)
import StikJIT
#endif

/// Handler-ul extensiei `DroidNativeJITHelper.appex`.
///
/// Extensia e al doilea proces al designului „Built-in StikJIT”: ea vorbeste
/// with the device debugserver, the host app is the attached process. Each
/// request `NSExtension` poarta exact o operatie (`DNBuiltInJITRequest`).
///
/// Reguli respectate aici (din designul StikJIT):
/// - `DDIPaths` locuieste in `Library/StikJIT` AL EXTENSIEI, persistent intre
/// launches — the second time the DDI is not downloaded/mounted from zero.
///   The extension's temp directory is deleted when the process ends, so
///   nu e bun de cache.
/// - StikJIT calls are synchronous and serialize on a single queue.
/// - The pairing received as bytes is written to a unique temp file and deleted
///   right after the operation; the extension keeps no credentials.
/// - The script is `.universal` (the same protocol the host implements
///   already in JIT26Bridge and validated it on device via StikDebug).
@objc(DroidNativeJITHelperRequestHandler)
final class DroidNativeJITHelperRequestHandler: NSObject, NSExtensionRequestHandling {
    private static let workQueue = DispatchQueue(label: "ro.droidnative.jithelper.stikjit")

    func beginRequest(with context: NSExtensionContext) {
        guard let item = context.inputItems.first as? NSExtensionItem,
              let data = item.userInfo?[DNBuiltInJITTransport.requestKey] as? Data,
              let request = try? JSONDecoder().decode(DNBuiltInJITRequest.self, from: data) else {
            complete(context, with: .failure("Request without a decodable payload."))
            return
        }

        // Capture strongly: the request must complete even if the system would
        // release the handler meanwhile (the operation can take minutes at DDI).
        Self.workQueue.async {
            self.complete(context, with: self.perform(request))
        }
    }

    private func perform(_ request: DNBuiltInJITRequest) -> DNBuiltInJITResponse {
        #if targetEnvironment(simulator)
        return .failure("The simulator has no StikJIT; built-in JIT needs real iOS hardware.")
        #else
        let txm = StikJIT.isTXMPresent
        let root = stikJITDirectory()
        let paths = DDIPaths.default(in: root)

        if request.operation == .resetDDI {
            do {
                try StikJIT.resetCachedDDI(at: paths)
                return DNBuiltInJITResponse(ok: true, message: "The DDI cache was cleared; the next preparation re-downloads it.", txmPresent: txm)
            } catch {
                return .failure("DDI reset failed: \(error.localizedDescription)", txmPresent: txm)
            }
        }

        guard let pairingData = request.pairingData, !pairingData.isEmpty else {
            return .failure("Pairing is missing. Import it in the app (Capabilities) before the built-in method.", txmPresent: txm)
        }

        let pairingURL = root.appendingPathComponent("pairing-\(UUID().uuidString).plist")
        do {
            try pairingData.write(to: pairingURL, options: [.atomic])
        } catch {
            return .failure("The temporary pairing could not be written: \(error.localizedDescription)", txmPresent: txm)
        }
        defer { try? FileManager.default.removeItem(at: pairingURL) }

        switch request.operation {
        case .prepare:
            let readiness = StikJIT.prepareDevice(pairingFile: pairingURL, paths: paths) { _ in }
            switch readiness {
            case .ready(let security):
                return DNBuiltInJITResponse(ok: true, message: "Device prepared: DDI mounted, built-in JIT can attach.", txmPresent: security.isTXMPresent ?? txm)
            case .unreachable(let reason), .preparationFailed(let reason):
                return .failure(friendly(reason), txmPresent: txm)
            @unknown default:
                return .failure("Unknown preparation state reported by StikJIT.", txmPresent: txm)
            }

        case .enable:
            guard request.targetPID > 1 else {
                return .failure("Invalid target PID (\(request.targetPID)).", txmPresent: txm)
            }
            do {
                try StikJIT.enableJIT(
                    targetPID: request.targetPID,
                    pairingFile: pairingURL,
                    ddiPaths: paths,
                    // `.universal` = arena aplicatiei; `.legacy` = bufferul TCG
                    // QEMU (brk 0x69 in qemu_init, docs/09). Gazda decide.
                    script: request.scriptKind == .legacy ? StikJIT.Script.legacy : StikJIT.Script.universal,
                    forceScript: request.forceUniversalScript,
                    preparationProgress: { _ in },
                    progress: { _ in }
                )
                return DNBuiltInJITResponse(ok: true, message: "enableJIT finished (debugger detached).", txmPresent: txm)
            } catch {
                return .failure(friendly("\(error)"), txmPresent: txm)
            }

        case .resetDDI:
            return DNBuiltInJITResponse(ok: true, message: "Cache DDI resetat.", txmPresent: txm)
        }
        #endif
    }

    /// The extension's persistent directory for the StikJIT/DDI cache.
    private func stikJITDirectory() -> URL {
        let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library", isDirectory: true)
        let root = library.appendingPathComponent("StikJIT", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }

    /// Raw RSD/lockdown messages most often mean, in practice, one of
    /// putinele motive actionabile de mai jos. Le traducem, pastrand detaliul.
    private func friendly(_ raw: String) -> String {
        let text = raw.lowercased()
        if text.contains("timed out") || text.contains("timeout") ||
           text.contains("couldn't connect") || text.contains("could not connect") ||
           text.contains("connection refused") || text.contains("network is unreachable") ||
           text.contains("no route to host") {
            return "Cannot reach the device through the tunnel — connect LocalDevVPN and wait a few seconds. Detail: \(raw)"
        }
        if text.contains("pairing") || text.contains("invalidhostid") ||
           text.contains("missing value") || text.contains("password protected") {
            return "Pairing rejected or invalid — import a current pairing file for this device. Detail: \(raw)"
        }
        if text.contains("ddi") || text.contains("developer disk") ||
           text.contains("image") && text.contains("mount") {
            return "The DDI did not prepare/mount. Check LocalDevVPN; if it persists, use \"Reset DDI\" and redo the preparation. Detail: \(raw)"
        }
        if text.contains("developer mode") {
            return "Developer Mode seems off: Settings › Privacy & Security › Developer Mode. Detail: \(raw)"
        }
        if text.contains("get-task-allow") || text.contains("task_for_pid") ||
           text.contains("attach") && text.contains("denied") {
            return "The app cannot be attached (most likely not signed with get-task-allow). Detail: \(raw)"
        }
        return raw
    }

    private func complete(_ context: NSExtensionContext, with response: DNBuiltInJITResponse) {
        let item = NSExtensionItem()
        if let data = try? response.encoded() {
            item.userInfo = [DNBuiltInJITTransport.responseKey: data]
        }
        context.completeRequest(returningItems: [item], completionHandler: nil)
    }
}
