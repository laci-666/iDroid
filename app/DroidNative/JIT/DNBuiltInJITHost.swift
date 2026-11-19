import Foundation
#if canImport(Darwin)
import Darwin
#endif

/// The host-app side of the built-in JIT.
///
/// The model (also documented in StikJIT's `INTEGRATION.md`): StikJIT cannot
/// run in the process it attaches (it would self-block), so the app
/// starts its own `DroidNativeJITHelper.appex` extension, sends it a
/// single JSON request and lets it run StikJIT in the extension process.
///
/// Transport: an `NSExtensionItem` with the JSON payload in `userInfo`, started
/// via the private `NSExtension` API (the same surface extension hosts use
/// extensii ale sistemului o folosesc: request completion/cancellation/
/// interruption on the extension object). The stimulus is used EXCLUSIVELY for
/// extensia proprie, identificata din pachetul instalat.
///
/// For the `enable` operation, the caller does NOT await the response before
/// do its part of the TXM protocol: the request completes only after
/// debugserver-ul scriptat face detach. De aceea API-ul e bazat pe completion,
/// while the JIT coordinator meanwhile interleaves the attach wait and
/// arena preparation.
enum DNBuiltInJITHost {
    static let helperProductName = "DroidNativeJITHelper.appex"

    /// URL-ul helper-ului instalat, daca extensia chiar exista in pachet.
    static var helperBundleURL: URL? { discoveredHelper()?.url }

    static var isHelperInstalled: Bool { helperBundleURL != nil }

    /// LiveContainer cannot create extension processes; the StikJIT guide requires
    /// detectia lui si dezactivarea metodei incorporate acolo.
    static var isRunningInsideLiveContainer: Bool {
        getenv("LC_HOME_PATH") != nil
    }

    /// The extension's REAL bundle ID, read from the installed package.
    /// Sideloader-ele pot redenumi bundle-ID-ul aplicatiei la instalare; un ID
    /// hardcoded would miss the helper after such a rename.
    static var helperBundleIdentifier: String? { discoveredHelper()?.identifier }

    /// Helper discovery: we do not assume a single file name.
    /// Cautam intai produsul nostru in `PlugIns/`, apoi ORICE `.appex` din
    /// the same directory (a sideloader can rename the product too), and we
    /// we read the bundle ID from its own Info.plist. We check both
    /// `builtInPlugInsURL`, cat si calea fizica `<App>.app/PlugIns` — pe unele
    /// installs the first returns nil although the directory exists.
    private static func discoveredHelper() -> (url: URL, identifier: String)? {
        guard !isRunningInsideLiveContainer else { return nil }
        for directory in plugInsCandidateDirectories() {
            let fm = FileManager.default
            let preferred = directory.appendingPathComponent(helperProductName, isDirectory: true)
            if fm.fileExists(atPath: preferred.path), let identifier = bundleIdentifier(of: preferred) {
                return (preferred, identifier)
            }
            let contents = (try? fm.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []
            for candidate in contents where candidate.pathExtension == "appex" {
                if let identifier = bundleIdentifier(of: candidate) {
                    return (candidate, identifier)
                }
            }
        }
        return nil
    }

    private static func plugInsCandidateDirectories() -> [URL] {
        var directories: [URL] = []
        if let plugIns = Bundle.main.builtInPlugInsURL { directories.append(plugIns) }
        let physical = Bundle.main.bundleURL.appendingPathComponent("PlugIns", isDirectory: true)
        if !directories.contains(physical) { directories.append(physical) }
        return directories
    }

    private static func bundleIdentifier(of appexURL: URL) -> String? {
        if let identifier = Bundle(url: appexURL)?.bundleIdentifier {
            return identifier
        }
        // Fallback: ID derived from the host. Good only if the appex really exists
        // physically (the caller checked) — Bundle(url:) can fail on packages
        // re-semnate de sideloader-e cu metadate incomplete.
        guard appexURL.lastPathComponent == helperProductName,
              let hostIdentifier = Bundle.main.bundleIdentifier else { return nil }
        return hostIdentifier + ".JITHelper"
    }

    /// Helper state for UI/diagnostics: if missing, say WHERE we
    /// looked and what we found, so the next screenshot is the answer,
    /// nu o noua ghicitoare.
    static var helperDiagnostic: String {
        if let helper = discoveredHelper() {
            return "present: \(helper.url.lastPathComponent) (\(helper.identifier))"
        }
        if isRunningInsideLiveContainer {
            return "unavailable in LiveContainer (cannot start extensions)"
        }
        for directory in plugInsCandidateDirectories() {
            if let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) {
                return "negasit — PlugIns contine: \(names.isEmpty ? "(gol)" : names.joined(separator: ", "))"
            }
        }
        return "not found — the PlugIns directory is missing from the bundle (the DroidNativeJITHelper target was not embedded at build/signing)"
    }

    /// Sends a request to the helper. The completion is called exactly once,
    /// pe MainActor, inclusiv la timeout/anulare.
    @MainActor
    static func send(
        _ request: DNBuiltInJITRequest,
        timeout: TimeInterval = 180,
        completion: @escaping @MainActor (DNBuiltInJITResponse) -> Void
    ) {
        guard let identifier = helperBundleIdentifier else {
            completion(.failure("The DroidNativeJITHelper helper is not available in this install (\(helperDiagnostic)). Reinstall from Xcode with the helper target signed and embedded."))
            return
        }
        let box = DNExtensionRequestBox(completion: completion)
        DNExtensionRequestBox.register(box)
        box.start(identifier: identifier, request: request, timeout: timeout)
    }
}

// MARK: - Suprafata privata NSExtension folosita

/// `+[NSExtension extensionWithIdentifier:error:]` e API privat pe iOS;
/// pe macOS e varianta publica a aceluiasi mecanism.
@objc private protocol DNExtensionFactory {
    @objc(extensionWithIdentifier:error:)
    func dn_makeExtension(withIdentifier identifier: String) throws -> AnyObject
}

/// The extension host's private signatures: the request starts with
/// `beginExtensionRequestWithInputItems:completion:` (the completion receives
/// the request's UUID identifier), and results arrive via
/// blocurile de completare/anulare/intrerupere setate pe acelasi obiect.
@objc private protocol DNExtensionHost {
    @objc(beginExtensionRequestWithInputItems:completion:)
    func dn_beginRequest(withInputItems items: [Any], completion: @escaping (NSUUID?) -> Void)

    @objc(pidForRequestIdentifier:)
    func dn_pid(forRequestIdentifier identifier: NSUUID) -> Int32

    @objc(setRequestCompletionBlock:)
    func dn_setRequestCompletionBlock(_ block: @escaping (NSUUID?, [Any]?) -> Void)

    @objc(setRequestCancellationBlock:)
    func dn_setRequestCancellationBlock(_ block: @escaping (NSUUID?, NSError?) -> Void)

    @objc(setRequestInterruptionBlock:)
    func dn_setRequestInterruptionBlock(_ block: @escaping (NSUUID?) -> Void)
}

@MainActor
private final class DNExtensionRequestBox {
    private static var running: [DNExtensionRequestBox] = []

    static func register(_ box: DNExtensionRequestBox) {
        running.append(box)
    }

    private let completion: @MainActor (DNBuiltInJITResponse) -> Void
    /// The NSExtension object must be kept alive for the whole request.
    private var extensionObject: AnyObject?
    private var timeoutItem: DispatchWorkItem?
    private var finished = false

    init(completion: @escaping @MainActor (DNBuiltInJITResponse) -> Void) {
        self.completion = completion
    }

    func start(identifier: String, request: DNBuiltInJITRequest, timeout: TimeInterval) {
        ensureExtensionFoundationLoaded()

        guard let extensionClass = NSClassFromString("NSExtension") else {
            finish(.failure("NSExtension is not available in this process; the helper cannot be started."))
            return
        }

        let payload: Data
        do {
            payload = try request.encoded()
        } catch {
            finish(.failure("The helper request could not be encoded: \(error.localizedDescription)"))
            return
        }

        let extensionObject: AnyObject
        do {
            // Obiectul-clasa e receptorul: +extensionWithIdentifier:error: e metoda
            // class type in ObjC, so we treat it as an instance of the shim protocol,
            // nu ca metatip (pe metatip Swift ar cere o cerinta `static`).
            let factory = unsafeBitCast(extensionClass as AnyObject, to: DNExtensionFactory.self)
            extensionObject = try factory.dn_makeExtension(withIdentifier: identifier)
        } catch {
            finish(.failure("The extension \(identifier) could not be created: \(error.localizedDescription)"))
            return
        }
        self.extensionObject = extensionObject
        let host = unsafeBitCast(extensionObject, to: DNExtensionHost.self)

        let timeoutItem = DispatchWorkItem { [weak self] in
            Task { @MainActor in
                self?.finish(.failure("The JIT helper did not respond within \(Int(timeout)) s (timeout)."))
            }
        }
        self.timeoutItem = timeoutItem
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout, execute: timeoutItem)

        // Blocurile pot sosi pe orice coada; verdictul se ia pe MainActor.
        host.dn_setRequestCompletionBlock { [weak self] _, items in
            let payload = ((items?.first as? NSExtensionItem)?.userInfo?[DNBuiltInJITTransport.responseKey] as? Data)
            Task { @MainActor in
                self?.handleResponseData(payload)
            }
        }
        host.dn_setRequestCancellationBlock { [weak self] _, error in
            let reason = error?.localizedDescription ?? "no reason reported"
            Task { @MainActor in
                self?.finish(.failure("The helper request was cancelled: \(reason)"))
            }
        }
        host.dn_setRequestInterruptionBlock { [weak self] _ in
            Task { @MainActor in
                self?.finish(.failure("The connection to the JIT helper was interrupted (the extension exited or was killed)."))
            }
        }

        let item = NSExtensionItem()
        item.userInfo = [DNBuiltInJITTransport.requestKey: payload]
        host.dn_beginRequest(withInputItems: [item]) { [weak self] requestIdentifier in
            Task { @MainActor in
                guard let self, !self.finished else { return }
                if requestIdentifier == nil {
                    self.finish(.failure("The JIT helper could not be started. If you installed via a sideloader, check it did not strip the app extensions."))
                }
            }
        }
    }

    private func handleResponseData(_ payload: Data?) {
        guard !finished else { return }
        guard let payload,
              let response = try? JSONDecoder().decode(DNBuiltInJITResponse.self, from: payload) else {
            finish(.failure("The helper closed the request without a decodable result."))
            return
        }
        finish(response)
    }

    private func finish(_ response: DNBuiltInJITResponse) {
        guard !finished else { return }
        finished = true
        timeoutItem?.cancel()
        timeoutItem = nil
        Self.running.removeAll { $0 === self }
        completion(response)
    }

    private func ensureExtensionFoundationLoaded() {
        guard NSClassFromString("NSExtension") == nil else { return }
        _ = dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW)
    }
}
