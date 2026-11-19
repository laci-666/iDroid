import Foundation

/// Guest download sources — live-verified in Oct 2026.
/// The app does NOT redistribute Android: at the user's request it downloads
/// public packages (LineageOS for QEMU, Alpine, EDK2), exactly like S5LBox with the IPSW.
enum GuestCatalog {

    // LineageOS for the QEMU `virt` machine, ARM64-only build, packaged as a UTM VM.
    // We always resolve the latest release via the API (release cadence is monthly):
    static let lineageRepoAPI = URL(string: "https://api.github.com/repos/jqssun/android-lineage-qemu/releases/latest")!
    static func isLineageUTMAsset(_ name: String) -> Bool {
        name.hasPrefix("UTM-VM-lineage") && name.contains("arm64only") && name.hasSuffix(".zip")
    }

    // Test guest for gates 1a-1c: official Alpine kernel + initramfs (aarch64).
    static let alpineKernelURL = URL(string: "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64/netboot/vmlinuz-lts")!
    static let alpineInitrdURL = URL(string: "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64/netboot/initramfs-lts")!

    // UEFI firmware (EDK2 for QEMU aarch64, RELEASE build). The UTM-VM package
    // contine efi_vars.fd, dar CODUL firmware e al aplicatiei gazda (ca la UTM).
    static let firmwareCodeURL = URL(string: "https://raw.githubusercontent.com/retrage/edk2-nightly/master/bin/RELEASEAARCH64_QEMU_EFI.fd")!

    /// The QEMU `virt` machine expects 64 MiB pflash images (UTM does the same at import).
    static let pflashSizeBytes: UInt64 = 64 * 1024 * 1024

    /// Minimum space required for the full Android setup:
    /// arhiva (~1.1 GB) + disc extras (~2.2 GB) + marja de crestere overlay/userdata.
    static let recommendedFreeBytes: UInt64 = 6 * 1024 * 1024 * 1024
}

/// Minimal metadata from the GitHub API for a release.
struct GitHubRelease: Decodable {
    let tagName: String
    let prerelease: Bool
    let assets: [Asset]
    struct Asset: Decodable {
        let name: String
        let size: UInt64
        let digest: String?            // format "sha256:...." pe release-urile recente
        let browserDownloadURL: URL
        enum CodingKeys: String, CodingKey {
            case name, size, digest
            case browserDownloadURL = "browser_download_url"
        }
    }
    enum CodingKeys: String, CodingKey {
        case tagName = "tag_name"
        case prerelease
        case assets
    }

    /// Cauta asset-ul UTM-VM ARM64-only din release.
    var lineageAsset: Asset? { assets.first { GuestCatalog.isLineageUTMAsset($0.name) } }

    static func fetchLatestLineage() async throws -> GitHubRelease {
        try await fetchLineageRelease(offset: 0)
    }

    /// The `offset`-th stable release (no prerelease) containing the
    /// arm64only UTM-VM asset: 0 = latest, 1 = previous, etc. Useful when
    /// ultimul build upstream e defect (ex. init incompatibil cu libc++).
    static func fetchLineageRelease(offset: Int) async throws -> GitHubRelease {
        if offset <= 0 {
            let data = try await githubGET(GuestCatalog.lineageRepoAPI)
            return try JSONDecoder().decode(GitHubRelease.self, from: data)
        }
        let listURL = URL(string: "https://api.github.com/repos/jqssun/android-lineage-qemu/releases?per_page=10")!
        let data = try await githubGET(listURL)
        let releases = try JSONDecoder().decode([GitHubRelease].self, from: data)
            .filter { !$0.prerelease && $0.lineageAsset != nil }
        guard offset < releases.count else { throw SetupError.assetMissing }
        return releases[offset]
    }

    private static func githubGET(_ url: URL) async throws -> Data {
        var req = URLRequest(url: url)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("DroidNative/0.1 (iOS)", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await URLSession.shared.data(for: req)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw SetupError.network("GitHub API answered \((response as? HTTPURLResponse)?.statusCode ?? -1)")
        }
        return data
    }
}

enum SetupError: Error, LocalizedError {
    case network(String)
    case assetMissing
    case checksumMismatch
    case extraction(String)
    case insufficientStorage(available: UInt64, required: UInt64)
    case cancelled
    case unexpectedPackage(String)

    var errorDescription: String? {
        switch self {
        case .network(let m): return "Retea: \(m)"
        case .assetMissing: return "The Lineage release no longer contains the expected arm64only UTM-VM asset."
        case .checksumMismatch: return "SHA256 checksum mismatch — corrupted download. Retry."
        case .extraction(let m): return "Extragere: \(m)"
        case .insufficientStorage(let a, let r):
            return "Insufficient space: \(ByteCountFormatter.string(fromByteCount: Int64(a), countStyle: .file)) available, \(ByteCountFormatter.string(fromByteCount: Int64(r), countStyle: .file)) recommended."
        case .cancelled: return "Anulat."
        case .unexpectedPackage(let m): return "Unexpected package: \(m)"
        }
    }
}
