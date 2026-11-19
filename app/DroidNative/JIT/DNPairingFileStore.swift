import Foundation
import Security

/// Storage of the remote-pairing file for the built-in JIT.
///
/// Chestii importante:
/// - Pairing is a device credential (private key in a plist),
///   so it lives in the Keychain (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`),
///   not in Documents and not as an exportable file.
/// - Un pairing de StikDebug existent (`.mobiledevicepairing` sau plist exportat)
///   can be imported once from Capabilities; later read from the Keychain.
/// - Stage B3 (docs/08) will generate it on-device (iOS 27, Developer Mode ->
///   "Pair with DroidNative") and write to the same store.
enum DNPairingFileStore {
    static let service = "ro.droidnative.jit-pairing"
    static let account = "rppairing"

    enum PairingError: LocalizedError {
        case invalidPlist
        case keychain(OSStatus)

        var errorDescription: String? {
            switch self {
            case .invalidPlist:
                return "The file is not a valid pairing (remote-pairing keys missing)."
            case .keychain(let status):
                return "Keychain refused the save (OSStatus \(status))."
            }
        }
    }

    static var hasPairing: Bool { pairingData() != nil }

    static func pairingData() -> Data? {
        var query = baseQuery()
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data, !data.isEmpty else { return nil }
        return data
    }

    static func save(_ data: Data) throws {
        guard isValidPairingPlist(data) else { throw PairingError.invalidPlist }
        var attributes = baseQuery()
        attributes[kSecValueData as String] = data
        attributes[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly

        let addStatus = SecItemAdd(attributes as CFDictionary, nil)
        if addStatus == errSecSuccess { return }
        guard addStatus == errSecDuplicateItem else { throw PairingError.keychain(addStatus) }

        let update: [String: Any] = [kSecValueData as String: data]
        let updateStatus = SecItemUpdate(baseQuery() as CFDictionary, update as CFDictionary)
        guard updateStatus == errSecSuccess else { throw PairingError.keychain(updateStatus) }
    }

    static func delete() {
        SecItemDelete(baseQuery() as CFDictionary)
    }

    /// Import from a user-chosen URL (Files/iCloud). Reads
    /// security-scoped, validates and saves; the original URL is not retained.
    static func importPairing(from url: URL) throws {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let data = try Data(contentsOf: url)
        try save(data)
    }

    /// Forma minima a unui RPPairing plist: cheile criptografice de 32 de bytes
    /// and the host identifier. Guards against importing the wrong file by accident.
    static func isValidPairingPlist(_ data: Data) -> Bool {
        guard let plist = try? PropertyListSerialization.propertyList(from: data, format: nil),
              let dict = plist as? [String: Any],
              let publicKey = dict["public_key"] as? Data, publicKey.count == 32,
              let privateKey = dict["private_key"] as? Data, privateKey.count == 32,
              let identifier = dict["identifier"] as? String, !identifier.isEmpty else {
            return false
        }
        return true
    }

    private static func baseQuery() -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecAttrSynchronizable as String: false
        ]
    }
}
