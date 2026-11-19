import Foundation
import Compression

/// Streaming ZIP extractor (STORE and DEFLATE methods), no external deps —
/// iOS offers no public unzip API, and the Lineage archive is ~1.1 GB
/// (so nothing is loaded whole into memory). Supports ZIP64 fields
/// on individual entries (the extracted qcow2 disk exceeds 2 GB).
enum ZipExtractor {

    struct Entry {
        let name: String
        let method: UInt16
        let compressedSize: UInt64
        let uncompressedSize: UInt64
        let localHeaderOffset: UInt64
    }

    /// Extracts the whole zip into the given directory; returns the extracted files.
    /// `progress` receives the fraction of total uncompressed bytes.
    static func extract(zipURL: URL, to destination: URL,
                          progress: @escaping (Double) -> Void) throws -> [URL] {
        let fh = try FileHandle(forReadingFrom: zipURL)
        defer { try? fh.close() }
        let entries = try readCentralDirectory(fh)
        let total = entries.reduce(UInt64(0)) { $0 + $1.uncompressedSize }
        var done: UInt64 = 0
        var extracted: [URL] = []

        for entry in entries where !entry.name.hasSuffix("/") {
            let safeURL = try sanitizedDestination(entry.name, in: destination)
            try FileManager.default.createDirectory(
                at: safeURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            FileManager.default.createFile(atPath: safeURL.path, contents: nil)
            let out = try FileHandle(forWritingTo: safeURL)
            try extractEntry(entry, from: fh, to: out) { bytesWritten in
                done += bytesWritten
                if total > 0 { progress(Double(done) / Double(total)) }
            }
            try out.close()
            // Hard check: an extracted file shorter than in the directory
            // central ar instala imagini corupte tacut (caz real: sablonul
            // qcow2 gol trunchiat inainte de blocul refcount -> QEMU il vedea
            // empty and marked the image "corrupt" at first allocation).
            let actualSize = (try FileManager.default.attributesOfItem(atPath: safeURL.path)[.size] as? NSNumber)?.uint64Value
            guard actualSize == entry.uncompressedSize else {
                try? FileManager.default.removeItem(at: safeURL)
                throw SetupError.extraction("incomplete extracted file: \(entry.name) (\(actualSize ?? 0)/\(entry.uncompressedSize) bytes)")
            }
            extracted.append(safeURL)
        }
        progress(1)
        return extracted
    }

    // MARK: - Central directory

    private static func readCentralDirectory(_ fh: FileHandle) throws -> [Entry] {
        let fileSize = try fh.seekToEnd()
        let tailLen = min(fileSize, 65_557 + 22)
        try fh.seek(toOffset: fileSize - tailLen)
        let tail = try fh.read(upToCount: Int(tailLen)) ?? Data()

        // EOCD: search signature 0x06054b50 from the tail toward the head.
        guard let eocd = findEOCD(in: tail) else {
            throw SetupError.extraction("nu gasesc sfarsitul directorului central ZIP")
        }
        let count = tail.u16(at: eocd + 10)
        let cdSize = tail.u32(at: eocd + 12)
        let cdOffset = tail.u32(at: eocd + 16)
        guard count != 0xFFFF, cdOffset != 0xFFFFFFFF else {
            throw SetupError.extraction("archive-level ZIP64 not supported (the expected package does not use it)")
        }
        try fh.seek(toOffset: UInt64(cdOffset))
        let cd = try fh.read(upToCount: Int(cdSize)) ?? Data()

        var entries: [Entry] = []
        var pos = 0
        for _ in 0..<count {
            guard pos + 46 <= cd.count, cd.u32(at: pos) == 0x02014b50 else {
                throw SetupError.extraction("director central corupt")
            }
            let method = cd.u16(at: pos + 10)
            var comp = UInt64(cd.u32(at: pos + 20))
            var uncomp = UInt64(cd.u32(at: pos + 24))
            let nameLen = Int(cd.u16(at: pos + 28))
            let extraLen = Int(cd.u16(at: pos + 30))
            let commentLen = Int(cd.u16(at: pos + 32))
            var localOff = UInt64(cd.u32(at: pos + 42))
            let name = String(decoding: cd[(pos + 46)..<(pos + 46 + nameLen)], as: UTF8.self)

            // Campul extra ZIP64 (0x0001): valori U64, in ordinea uncomp/comp/offset,
            // present only for fields saturated at 0xFFFFFFFF.
            var extraPos = pos + 46 + nameLen
            let extraEnd = extraPos + extraLen
            while extraPos + 4 <= extraEnd {
                let tag = cd.u16(at: extraPos)
                let len = Int(cd.u16(at: extraPos + 2))
                if tag == 0x0001 {
                    var p = extraPos + 4
                    if uncomp == 0xFFFFFFFF { uncomp = cd.u64(at: p); p += 8 }
                    if comp == 0xFFFFFFFF { comp = cd.u64(at: p); p += 8 }
                    if localOff == 0xFFFFFFFF { localOff = cd.u64(at: p) }
                }
                extraPos += 4 + len
            }
            entries.append(Entry(name: name, method: method, compressedSize: comp,
                                 uncompressedSize: uncomp, localHeaderOffset: localOff))
            pos += 46 + nameLen + extraLen + commentLen
        }
        return entries
    }

    private static func findEOCD(in data: Data) -> Int? {
        guard data.count >= 22 else { return nil }
        var i = data.count - 22
        while i >= 0 {
            if data.u32(at: i) == 0x06054b50 { return i }
            i -= 1
        }
        return nil
    }

    // MARK: - Extragerea unei intrari

    private static func extractEntry(_ entry: Entry, from fh: FileHandle, to out: FileHandle,
                                     written: (UInt64) -> Void) throws {
        try fh.seek(toOffset: entry.localHeaderOffset)
        let header = try fh.read(upToCount: 30) ?? Data()
        guard header.count == 30, header.u32(at: 0) == 0x04034b50 else {
            throw SetupError.extraction("invalid local header for \(entry.name)")
        }
        let nameLen = Int(header.u16(at: 26))
        let extraLen = Int(header.u16(at: 28))
        try fh.seek(toOffset: entry.localHeaderOffset + 30 + UInt64(nameLen + extraLen))

        var remaining = entry.compressedSize
        let chunkSize = 256 * 1024
        switch entry.method {
        case 0: // STORE
            while remaining > 0 {
                let n = min(UInt64(chunkSize), remaining)
                let data = try fh.read(upToCount: Int(n)) ?? Data()
                if data.isEmpty { throw SetupError.extraction("truncated archive") }
                try out.write(contentsOf: data)
                remaining -= UInt64(data.count)
                written(UInt64(data.count))
            }
        case 8: // DEFLATE
            let decoder = try DeflateDecoder()
            while remaining > 0 {
                let n = min(UInt64(chunkSize), remaining)
                let data = try fh.read(upToCount: Int(n)) ?? Data()
                if data.isEmpty { throw SetupError.extraction("truncated archive") }
                remaining -= UInt64(data.count)
                try decoder.decompress(data, isFinal: remaining == 0) { outData in
                    try out.write(contentsOf: outData)
                    written(UInt64(outData.count))
                }
            }
        default:
            throw SetupError.extraction("metoda ZIP nesuportata: \(entry.method)")
        }
    }

    /// Zip-Slip protection: no file escapes the destination directory.
    private static func sanitizedDestination(_ name: String, in base: URL) throws -> URL {
        var parts: [String] = []
        for part in name.split(separator: "/") {
            let p = String(part)
            if p == ".." { throw SetupError.extraction("suspicious path in archive: \(name)") }
            if p != "." { parts.append(p) }
        }
        var url = base
        for p in parts { url.appendPathComponent(p) }
        return url
    }
}

/// DEFLATE (RFC 1951) peste framework-ul Compression — exact metoda 8 din ZIP.
/// In the current SDK, `dst_ptr`/`src_ptr` in `compression_stream` are pointers
/// NON-optional (nu accept nil), iar structura nu are init() gol in Swift.
/// So we keep it behind an explicitly allocated pointer: we give it real
/// buffers from the start, the memberwise constructor is complete, and C calls get
/// the pointer directly (no `&` on a property, which gives odd init errors).
private final class DeflateDecoder {
    private static let outCapacity = 256 * 1024
    private let streamPtr: UnsafeMutablePointer<compression_stream>
    private let outPointer: UnsafeMutablePointer<UInt8>

    init() throws {
        outPointer = UnsafeMutablePointer<UInt8>.allocate(capacity: Self.outCapacity)
        streamPtr = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        // Membri in ordinea din compression.h (dst_ptr, dst_size, src_ptr,
        // src_size, state); real buffers, size 0 until the first chunk.
        streamPtr.initialize(to: compression_stream(dst_ptr: outPointer, dst_size: 0,
                                                    src_ptr: UnsafePointer(outPointer), src_size: 0,
                                                    state: nil))
        guard compression_stream_init(streamPtr, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            streamPtr.deinitialize(count: 1)
            streamPtr.deallocate()
            outPointer.deallocate()
            throw SetupError.extraction("DEFLATE init failed")
        }
    }

    deinit {
        compression_stream_destroy(streamPtr)
        streamPtr.deinitialize(count: 1)
        streamPtr.deallocate()
        outPointer.deallocate()
    }

    func decompress(_ input: Data, isFinal: Bool, write: (Data) throws -> Void) throws {
        try input.withUnsafeBytes { raw in
            streamPtr.pointee.src_ptr = raw.bindMemory(to: UInt8.self).baseAddress!
            streamPtr.pointee.src_size = input.count
            while true {
                streamPtr.pointee.dst_ptr = outPointer
                streamPtr.pointee.dst_size = Self.outCapacity
                let flags: Int32 = isFinal ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0
                let status = compression_stream_process(streamPtr, flags)
                let produced = Self.outCapacity - streamPtr.pointee.dst_size
                if produced > 0 {
                    try write(Data(bytes: UnsafeRawPointer(outPointer), count: produced))
                }
                switch status {
                case COMPRESSION_STATUS_OK:
                    if streamPtr.pointee.src_size == 0 {
                        // At the final chunk we do NOT stop when input ran out:
                        // the decoder may still hold output in its internal buffer.
                        // Real bug: highly compressible files (the template
                        // qcow2 gol: 388 -> 196864 bytes) ramaneau trunchiate
                        // at 131072 because we exited before draining/END.
                        if !isFinal { return }
                        if produced == 0 {
                            throw SetupError.extraction("DEFLATE finished without END")
                        }
                    }
                case COMPRESSION_STATUS_END:
                    return
                default:
                    throw SetupError.extraction("DEFLATE error")
                }
            }
        }
    }
}

// MARK: - Citiri little-endian din Data

private extension Data {
    func u16(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | UInt16(self[offset + 1]) << 8
    }
    func u32(at offset: Int) -> UInt32 {
        UInt32(u16(at: offset)) | UInt32(u16(at: offset + 2)) << 16
    }
    func u64(at offset: Int) -> UInt64 {
        UInt64(u32(at: offset)) | UInt64(u32(at: offset + 4)) << 32
    }
}
