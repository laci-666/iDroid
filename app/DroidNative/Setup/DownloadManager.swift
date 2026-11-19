import Foundation
import CryptoKit

/// Download with progress + resume, for large files (Lineage archive ~1.1 GB).
/// A single active download. On failure/cancel it keeps resumeData next to the
/// destination ("<file>.resume") so a relaunch continues where it stopped.
/// The user is asked to keep the app in the foreground (like S5LBox at setup);
/// sesiunea e una normala, nu de fundal — v. docs/06.
final class DownloadManager: NSObject {
    private var session: URLSession!
    private var continuation: CheckedContinuation<Void, Error>?
    private var progressHandler: ((Double) -> Void)?
    private var currentDestination: URL?
    private var activeTask: URLSessionDownloadTask?

    override init() {
        super.init()
        let cfg = URLSessionConfiguration.default
        cfg.waitsForConnectivity = true
        cfg.timeoutIntervalForResource = 60 * 60 * 3   // 3 h pe resursa (Wi-Fi lent)
        cfg.httpMaximumConnectionsPerHost = 1
        session = URLSession(configuration: cfg, delegate: self, delegateQueue: nil)
    }

    func download(from url: URL, to destination: URL,
                  progress: @escaping (Double) -> Void) async throws {
        precondition(activeTask == nil, "one download at a time")
        progressHandler = progress
        currentDestination = destination
        let resumeFile = destination.appendingPathExtension("resume")
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            continuation = cont
            if let data = try? Data(contentsOf: resumeFile), !data.isEmpty {
                activeTask = session.downloadTask(withResumeData: data)
            } else {
                var req = URLRequest(url: url)
                req.setValue("DroidNative/0.1 (iOS)", forHTTPHeaderField: "User-Agent")
                activeTask = session.downloadTask(with: req)
            }
            activeTask?.resume()
        }
    }

    func cancel() {
        // byProducingResumeData: we save the state for a later resume.
        activeTask?.cancel(byProducingResumeData: { [weak self] data in
            guard let self, let data, let dest = self.currentDestination else { return }
            try? data.write(to: dest.appendingPathExtension("resume"))
        })
    }

    private func finish(with result: Result<Void, Error>) {
        let cont = continuation
        continuation = nil
        progressHandler = nil
        activeTask = nil
        currentDestination = nil
        cont?.resume(with: result)
    }

    /// Streaming SHA256 (we do not load 1 GB into memory).
    static func sha256Hex(of file: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var hasher = SHA256()
        while true {
            let chunk = try handle.read(upToCount: 1 << 20) ?? Data()
            if chunk.isEmpty { break }
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }
}

extension DownloadManager: URLSessionDownloadDelegate {
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard totalBytesExpectedToWrite > 0 else { return }
        let frac = Double(totalBytesWritten) / Double(totalBytesExpectedToWrite)
        progressHandler?(frac)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        guard let dest = currentDestination else {
            finish(with: .failure(SetupError.network("destinatie lipsa")))
            return
        }
        do {
            if FileManager.default.fileExists(atPath: dest.path) {
                try FileManager.default.removeItem(at: dest)
            }
            try FileManager.default.moveItem(at: location, to: dest)
            try? FileManager.default.removeItem(at: dest.appendingPathExtension("resume"))
            finish(with: .success(()))
        } catch {
            finish(with: .failure(error))
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error else { return }   // success is handled in didFinishDownloadingTo
        let ns = error as NSError
        if let resume = ns.userInfo[NSURLSessionDownloadTaskResumeData] as? Data,
           let dest = currentDestination {
            try? resume.write(to: dest.appendingPathExtension("resume"))
        }
        if ns.code == NSURLErrorCancelled {
            finish(with: .failure(SetupError.cancelled))
        } else {
            finish(with: .failure(error))
        }
    }
}
