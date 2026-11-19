import Foundation

/// Inter-process messages: host app <-> the `DroidNativeJITHelper` extension.
///
/// The file is compiled into BOTH targets. The protocol is intentionally minimal:
/// exact o operatie pe request de extensie. Pairing-ul calatoreste ca `Data`
/// (JSON encodes it base64) because the host keeps it in the Keychain, not on disk;
/// the extension writes it to a temp file only for the duration of the StikJIT call.
///
/// We send no custom script; we choose between the two official StikJIT
/// scripts (docs/09): `.universal` for our TXM arena (JIT26Bridge) and
/// `.legacy` for the UTM fork QEMU TCG buffer, which asks for its own
/// itself for authorization via `brk #0x69` during `qemu_init`.
/// The keys under which the JSON payloads travel in the `userInfo` of the
/// `NSExtensionItem`-urilor dintre cele doua procese.
enum DNBuiltInJITTransport {
    static let requestKey = "ro.droidnative.jit.request"
    static let responseKey = "ro.droidnative.jit.response"
}

/// The debugserver script for an `enable` operation:
/// - `universal`: protocolul `brk #0xf00d` (x16 = comanda) — arena aplicatiei.
/// - `legacy`: the `brk #0x69` protocol (x0 = address, x1 = length; exactly one
///   region, then detach) — used by the UTM fork's `tcg/region.c` at
///   initializarea bufferului de cod pe iOS 26/TXM.
enum DNJITScriptKind: String, Codable {
    case universal
    case legacy

    /// The file name required by the StikDebug URL scheme (`script-name=`).
    var fileName: String {
        switch self {
        case .universal: return "universal.js"
        case .legacy: return "legacy.js"
        }
    }
}

enum DNBuiltInJITOperation: String, Codable {
    /// Device preparation only (DDI downloaded/mounted) — for the
    /// "Prepare built-in JIT" button in Capabilities, no attach.
    case prepare
    /// Preparation + enableJIT on the host PID (blocking until detach).
    case enable
    /// Clears the extension's DDI cache (recovery after a corrupt DDI).
    case resetDDI
}

struct DNBuiltInJITRequest: Codable {
    var operation: DNBuiltInJITOperation
    /// Host process PID (debugserver target). Negative/zero at resetDDI.
    var targetPID: Int32
    /// Bytes-ii plist-ului de remote pairing. `nil` doar la resetDDI.
    var pairingData: Data?
    /// Force script even on devices where StikJIT would not detect TXM
    /// (e.g. a model newer than its list). Default false: automatic detection.
    var forceUniversalScript: Bool = false
    /// The script used at `enable` (ignored at `prepare`/`resetDDI`).
    var scriptKind: DNJITScriptKind = .universal

    func encoded() throws -> Data {
        try JSONEncoder().encode(self)
    }

    init(operation: DNBuiltInJITOperation, targetPID: Int32, pairingData: Data?, forceUniversalScript: Bool = false, scriptKind: DNJITScriptKind = .universal) {
        self.operation = operation
        self.targetPID = targetPID
        self.pairingData = pairingData
        self.forceUniversalScript = forceUniversalScript
        self.scriptKind = scriptKind
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        operation = try container.decode(DNBuiltInJITOperation.self, forKey: .operation)
        targetPID = try container.decode(Int32.self, forKey: .targetPID)
        pairingData = try container.decodeIfPresent(Data.self, forKey: .pairingData)
        forceUniversalScript = try container.decodeIfPresent(Bool.self, forKey: .forceUniversalScript) ?? false
        scriptKind = try container.decodeIfPresent(DNJITScriptKind.self, forKey: .scriptKind) ?? .universal
    }
}

struct DNBuiltInJITResponse: Codable {
    var ok: Bool
    var message: String
    /// TXM detected by StikJIT in the extension process (informative for the UI).
    var txmPresent: Bool?

    func encoded() throws -> Data {
        try JSONEncoder().encode(self)
    }

    static func failure(_ message: String, txmPresent: Bool? = nil) -> DNBuiltInJITResponse {
        DNBuiltInJITResponse(ok: false, message: message, txmPresent: txmPresent)
    }
}
