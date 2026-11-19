import Darwin
import Foundation

/// Container location for converted libraries used by the own
/// mini-linker test (docs/13 step 2.5). The Mach-O files are loaded by
/// dyld; the ELF inside them is bound by DNNativeLinker.c.
func dnNativeLibraryURL(_ filename: String) -> URL {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
        ?? FileManager.default.temporaryDirectory
    return base.appendingPathComponent("DroidNative/native-libraries/\(filename)")
}

/// On-disk progress markers for the complete-native fixture run.  The
/// harness returns its report lines only at the end, so a hard crash
/// inside guest code would otherwise leave no trace of which step was
/// running.  Before every risky boundary (link, each guest export,
/// each activity transition) the harness records the step name in the
/// breadcrumb and appends it, synchronously, to the progress log.
/// Both files are included in the console diagnostic export, so after
/// a crash the user does not have to start the same run again just to
/// learn where it stopped.  A finished run records "complete".
private func dnNativeFullBreadcrumbURL() -> URL {
    return dnNativeLibraryURL("libdnfull.dylib")
        .deletingLastPathComponent()
        .appendingPathComponent("native-full-breadcrumb.txt")
}

private func dnNativeFullProgressURL() -> URL {
    return dnNativeLibraryURL("libdnfull.dylib")
        .deletingLastPathComponent()
        .appendingPathComponent("native-full-progress.txt")
}

private func dnNativeFullEnsureDiagnosticDirectory() {
    try? FileManager.default.createDirectory(
        at: dnNativeFullProgressURL().deletingLastPathComponent(),
        withIntermediateDirectories: true)
}

private func dnNativeFullAppendProgress(_ line: String) {
    dnNativeFullEnsureDiagnosticDirectory()
    let url = dnNativeFullProgressURL()
    let data = Data((line + "\n").utf8)
    if let handle = try? FileHandle(forWritingTo: url) {
        defer { try? handle.close() }
        _ = try? handle.seekToEnd()
        try? handle.write(contentsOf: data)
        try? handle.synchronize()
    } else {
        try? data.write(to: url, options: [.atomic])
    }
}

private func dnNativeFullResetProgress(previousBreadcrumb: String?) {
    dnNativeFullEnsureDiagnosticDirectory()
    var header = "complete native fixture run started\n"
    if let previousBreadcrumb, previousBreadcrumb != "complete" {
        header += "previous complete-native run stopped at: \(previousBreadcrumb)\n"
    }
    try? Data(header.utf8).write(to: dnNativeFullProgressURL(), options: [.atomic])
}

private func dnNativeFullSetBreadcrumb(_ value: String) {
    dnNativeFullEnsureDiagnosticDirectory()
    try? Data(value.utf8).write(to: dnNativeFullBreadcrumbURL(), options: [.atomic])
    dnNativeFullAppendProgress("stage: \(value)")
}

func dnNativeFullReadBreadcrumb() -> String? {
    guard let text = try? String(contentsOf: dnNativeFullBreadcrumbURL(), encoding: .utf8) else {
        return nil
    }
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    return trimmed.isEmpty ? nil : trimmed
}

func dnNativeFullBreadcrumbText() -> String {
    guard let breadcrumb = dnNativeFullReadBreadcrumb() else {
        return "(no complete-native breadcrumb file)\n"
    }
    return breadcrumb + "\n"
}

func dnNativeFullProgressText() -> String {
    guard let text = try? String(contentsOf: dnNativeFullProgressURL(), encoding: .utf8),
          !text.isEmpty else {
        return "(no complete-native progress file)\n"
    }
    return text.hasSuffix("\n") ? text : text + "\n"
}

private func dnNativeReportLines(_ image: OpaquePointer?) -> [String] {
    guard let image, let report = dn_native_image_report(image) else { return [] }
    return String(cString: report)
        .split(separator: "\n", omittingEmptySubsequences: true)
        .map(String.init)
}

/// Step 2.5 gate: two dependent converted libraries are linked by
/// DroidNative's own ELF mini-linker (dependencies, RELA/RELR,
/// imports, constructors/destructors), then called directly. This is
/// the first runtime block that real NativeActivity libraries need;
/// it intentionally does not use the step-2 one-off symbol resolver.
func dnNativeLinkRunTest() -> [String] {
    let baseURL = dnNativeLibraryURL("libdnbase.dylib")
    let linkURL = dnNativeLibraryURL("libdnlink.dylib")
    let basePath = baseURL.path
    let linkPath = linkURL.path

    var out: [String] = []
    let missing = [baseURL, linkURL]
        .filter { !FileManager.default.fileExists(atPath: $0.path) }
        .map(\.lastPathComponent)
    if !missing.isEmpty {
        return ["missing converted libraries: \(missing.joined(separator: ", ")) — import both first"]
    }

    var ok = true
    func expect(_ condition: Bool, _ label: String) {
        out.append("\(label): \(condition ? "OK" : "MISMATCH")")
        ok = ok && condition
    }

    // ---- dependency first -------------------------------------------------
    _ = dlerror()
    guard let baseHandle = dlopen(basePath, RTLD_NOW | RTLD_LOCAL) else {
        return ["dlopen libdnbase.dylib FAILED: \(dlerror().map { String(cString: $0) } ?? "unknown error")"]
    }
    out.append("dlopen libdnbase.dylib OK")

    var baseImage: OpaquePointer?
    let baseRC = dn_native_link_image(basePath, baseHandle, &baseImage)
    out += dnNativeReportLines(baseImage)
    guard baseRC == 0, let linkedBase = baseImage else {
        out.append("dn-link libdnbase FAILED (rc=\(baseRC))")
        if let baseImage { dn_native_image_dispose(baseImage) }
        dlclose(baseHandle)
        out.append("FAIL — dependency did not link")
        return out
    }

    func symbol(_ image: OpaquePointer, _ name: String) -> UnsafeMutableRawPointer? {
        dn_native_image_symbol(image, name)
    }

    if let p = symbol(linkedBase, "dn_base_add") {
        let f = unsafeBitCast(p, to: (@convention(c) (Int32, Int32) -> Int32).self)
        let v = f(20, 3)
        out.append("dn_base_add(20, 3) = \(v) (expect 42 after base constructor)")
        expect(v == 42, "base cross-library function")
    } else {
        out.append("resolve dn_base_add FAILED")
        ok = false
    }
    if let p = symbol(linkedBase, "dn_base_ctor_calls") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> Int32).self)
        let v = f()
        out.append("dn_base_ctor_calls() = \(v) (expect 1)")
        expect(v == 1, "base DT_INIT_ARRAY ran once")
    } else {
        out.append("resolve dn_base_ctor_calls FAILED")
        ok = false
    }

    // ---- main library -----------------------------------------------------
    _ = dlerror()
    guard let linkHandle = dlopen(linkPath, RTLD_NOW | RTLD_LOCAL) else {
        out.append("dlopen libdnlink.dylib FAILED: \(dlerror().map { String(cString: $0) } ?? "unknown error")")
        _ = dn_native_image_run_finalizers(linkedBase)
        dn_native_image_dispose(linkedBase)
        dlclose(baseHandle)
        out.append("FAIL — main library did not dlopen")
        return out
    }
    out.append("dlopen libdnlink.dylib OK")

    var linkImage: OpaquePointer?
    let linkRC = dn_native_link_image(linkPath, linkHandle, &linkImage)
    out += dnNativeReportLines(linkImage)
    guard linkRC == 0, let linkedMain = linkImage else {
        out.append("dn-link libdnlink FAILED (rc=\(linkRC))")
        if let linkImage { dn_native_image_dispose(linkImage) }
        dlclose(linkHandle)
        _ = dn_native_image_run_finalizers(linkedBase)
        dn_native_image_dispose(linkedBase)
        dlclose(baseHandle)
        out.append("FAIL — main library did not link")
        return out
    }

    if let p = symbol(linkedMain, "dn_link_compute") {
        let f = unsafeBitCast(p, to: (@convention(c) (Int32) -> Int32).self)
        let v = f(10)
        out.append("dn_link_compute(10) = \(v) (expect 63: local GOT table + base import + DN shim)")
        expect(v == 63, "main computation")
    } else {
        out.append("resolve dn_link_compute FAILED")
        ok = false
    }
    if let p = symbol(linkedMain, "dn_link_state") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> Int32).self)
        let v = f()
        out.append("dn_link_state() = \(v) (expect 33: constructor used DT_NEEDED value + host strlen)")
        expect(v == 33, "main constructor state")
    } else {
        out.append("resolve dn_link_state FAILED")
        ok = false
    }
    if let p = symbol(linkedMain, "dn_link_ctor_calls") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> Int32).self)
        let v = f()
        out.append("dn_link_ctor_calls() = \(v) (expect 1)")
        expect(v == 1, "main DT_INIT_ARRAY ran once")
    } else {
        out.append("resolve dn_link_ctor_calls FAILED")
        ok = false
    }
    if let p = symbol(linkedMain, "dn_link_pointer_check") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> Int32).self)
        let v = f()
        out.append("dn_link_pointer_check() = \(v) (expect 42: function/data pointers relocated)")
        expect(v == 42, "relocated pointer tables")
    } else {
        out.append("resolve dn_link_pointer_check FAILED")
        ok = false
    }
    if let p = symbol(linkedMain, "dn_link_text") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> UnsafePointer<CChar>?).self)
        let value = f().map { String(cString: $0) }
        out.append("dn_link_text() = \(value.map { "\"\($0)\"" } ?? "NULL")")
        expect(value == "dn-link-native", "main text pointer")
    } else {
        out.append("resolve dn_link_text FAILED")
        ok = false
    }

    // Finalizers run while both images are still mapped; observe the
    // destructor side effect before disposing/dlclosing in reverse
    // dependency order.
    let finalizedPtr = symbol(linkedMain, "dn_link_finalized")
    let finiRC = dn_native_image_run_finalizers(linkedMain)
    out.append("dn_native_image_run_finalizers(libdnlink) rc=\(finiRC)")
    expect(finiRC == 0, "main finalizers")
    if let finalizedPtr {
        let finalizedFn = unsafeBitCast(finalizedPtr, to: (@convention(c) () -> Int32).self)
        let v = finalizedFn()
        out.append("dn_link_finalized() after finalizers = \(v) (expect 1)")
        expect(v == 1, "destructor side effect")
    } else {
        out.append("resolve dn_link_finalized FAILED")
        ok = false
    }

    dn_native_image_dispose(linkedMain)
    dlclose(linkHandle)
    _ = dn_native_image_run_finalizers(linkedBase)
    dn_native_image_dispose(linkedBase)
    dlclose(baseHandle)

    out.append(ok
        ? "PASS — own mini-linker bound two dependent native libraries and executed them directly (docs/13 step 2.5)"
        : "FAIL — see mismatches above")
    return out
}

/// iDroid 0.3 host-side GPU gate: run the Vulkan-on-Metal driver that
/// is linked into the app process, without needing an imported guest
/// library. This validates the Swift/Metal backend on the real iPhone
/// GPU before translated code enters the picture.
func dnNativeVulkanRunHostSelfTest() -> [String] {
    var buffer = [CChar](repeating: 0, count: 8192)
    let rc = dn_vulkan_run_selftest(&buffer, 8192)
    let text = buffer.withUnsafeBufferPointer { pointer -> String in
        guard let base = pointer.baseAddress else { return "" }
        return String(cString: base)
    }
    var lines = text
        .split(separator: "\n", omittingEmptySubsequences: true)
        .map(String.init)
    lines.append(rc == 0
        ? "PASS — Vulkan-on-Metal host driver self-test completed in-process"
        : "FAIL — Vulkan-on-Metal host driver self-test rc=\(rc)")
    return lines
}

/// iDroid 0.3 guest-side GPU gate: a converted ARM64 ELF fixture
/// (libdnvk.dylib) is linked by DroidNative's own mini-linker; its
/// vk* imports bind to the DN Vulkan driver, then the fixture issues
/// buffer/image commands and verifies readback bytes itself.
func dnNativeVulkanFixtureRunTest() -> [String] {
    let url = dnNativeLibraryURL("libdnvk.dylib")
    let path = url.path
    var out: [String] = []
    guard FileManager.default.fileExists(atPath: path) else {
        return ["missing converted library: libdnvk.dylib — import it first"]
    }

    var ok = true
    func expect(_ condition: Bool, _ label: String) {
        out.append("\(label): \(condition ? "OK" : "MISMATCH")")
        ok = ok && condition
    }

    _ = dlerror()
    guard let handle = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
        return ["dlopen libdnvk.dylib FAILED: \(dlerror().map { String(cString: $0) } ?? "unknown error")"]
    }
    out.append("dlopen libdnvk.dylib OK")

    var image: OpaquePointer?
    let linkRC = dn_native_link_image(path, handle, &image)
    out += dnNativeReportLines(image)
    guard linkRC == 0, let linked = image else {
        out.append("dn-link libdnvk FAILED (rc=\(linkRC))")
        if let image { dn_native_image_dispose(image) }
        dlclose(handle)
        out.append("FAIL — Vulkan fixture did not link")
        return out
    }

    func symbol(_ name: String) -> UnsafeMutableRawPointer? {
        dn_native_image_symbol(linked, name)
    }

    var runResult: Int32 = -999
    if let p = symbol("dn_vk_run_test") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> Int32).self)
        runResult = f()
        out.append("dn_vk_run_test() = \(runResult) (expect 0; negative values are fixture steps)")
        expect(runResult == 0, "guest Vulkan sequence")
    } else {
        out.append("resolve dn_vk_run_test FAILED")
        ok = false
    }

    if let p = symbol("dn_vk_last_step") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> Int32).self)
        let step = f()
        out.append("dn_vk_last_step() = \(step) (expect 10 on full completion)")
        expect(step == 10 || runResult != 0, "guest fixture completion step")
    } else {
        out.append("resolve dn_vk_last_step FAILED")
        ok = false
    }

    if let p = symbol("dn_vk_checksum") {
        let f = unsafeBitCast(p, to: (@convention(c) () -> UInt32).self)
        let checksum = f()
        let hex = String(format: "0x%08x", checksum)
        out.append("dn_vk_checksum() = \(hex) (expect 0x1de4992d: BGRA clear readback)")
        expect(checksum == 0x1de4992d, "BGRA clear checksum")
    } else {
        out.append("resolve dn_vk_checksum FAILED")
        ok = false
    }

    let finiRC = dn_native_image_run_finalizers(linked)
    out.append("dn_native_image_run_finalizers(libdnvk) rc=\(finiRC)")
    expect(finiRC == 0, "Vulkan fixture finalizers")

    dn_native_image_dispose(linked)
    dlclose(handle)

    out.append(ok
        ? "PASS — translated native code issued Vulkan commands and the DN Vulkan-on-Metal driver read back byte-exact results (iDroid 0.3)"
        : "FAIL — see mismatches above")
    return out
}

/// Configure the DN native runtime's on-device roots before any DN
/// self-test or converted library runs. The C setters copy the paths;
/// this also creates the deterministic asset read by libdnfull.
private func dnNativeFullPrepareRuntime() -> String? {
    let fm = FileManager.default
    guard let appSupport = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
        return "Application Support directory is unavailable"
    }
    let root = appSupport.appendingPathComponent("DroidNative", isDirectory: true)
    let files = root.appendingPathComponent("native-files", isDirectory: true)
    let libraries = root.appendingPathComponent("native-libraries", isDirectory: true)
    let assets = root.appendingPathComponent("native-assets", isDirectory: true)

    do {
        for directory in [root, files, libraries, assets] {
            try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        try Data("DN-ASSET-42".utf8).write(
            to: assets.appendingPathComponent("dn-full-asset.txt"),
            options: [.atomic])
    } catch {
        return error.localizedDescription
    }

    dn_native_set_files_root(files.path)
    dn_native_set_library_directory(libraries.path)
    dn_native_set_asset_directory(assets.path)
    dn_native_set_package_name("ro.droidnative.app")
    return nil
}

private func dnNativeFullSelfTestLines(
    _ label: String,
    _ run: (UnsafeMutablePointer<CChar>, Int) -> Int32
) -> (lines: [String], passed: Bool) {
    var buffer = [CChar](repeating: 0, count: 16384)
    let rc = buffer.withUnsafeMutableBufferPointer { pointer -> Int32 in
        guard let base = pointer.baseAddress else { return -1 }
        return run(base, pointer.count)
    }
    let text = buffer.withUnsafeBufferPointer { pointer -> String in
        guard let base = pointer.baseAddress else { return "" }
        return String(cString: base)
    }
    var lines = ["--- \(label) ---"]
    lines += text
        .split(separator: "\n", omittingEmptySubsequences: true)
        .map(String.init)
    let passed = rc == 0 &&
        (text.contains("host verdict: PASS") ||
         text.contains("PASS - DroidNative Vulkan-on-Metal subset self-test"))
    lines.append("\(label) self-test rc=\(rc): \(passed ? "OK" : "MISMATCH")")
    return (lines, passed)
}

/// iDroid 0.4 host-side complete-native gate. This runs every DN
/// runtime layer in-process before converted guest code is involved:
/// patch/TLS/VFS, bionic, libandroid/NativeActivity support and the
/// Vulkan-on-Metal driver.
func dnNativeFullRunHostSelfTest() -> [String] {
    var out: [String] = []
    if let error = dnNativeFullPrepareRuntime() {
        return [
            "runtime path setup FAILED: \(error)",
            "overall: FAIL - DN native runtime paths could not be configured"
        ]
    }
    out.append("runtime paths configured (files, libraries, assets): OK")

    var ok = true
    let patch = dnNativeFullSelfTestLines("patch/TLS/VFS") { dn_native_run_selftest($0, $1) }
    out += patch.lines
    ok = ok && patch.passed

    let bionic = dnNativeFullSelfTestLines("bionic") { dn_native_bionic_run_selftest($0, $1) }
    out += bionic.lines
    ok = ok && bionic.passed

    let android = dnNativeFullSelfTestLines("libandroid") { dn_native_android_run_selftest($0, $1) }
    out += android.lines
    ok = ok && android.passed

    let vulkan = dnNativeFullSelfTestLines("Vulkan-on-Metal") { dn_vulkan_run_selftest($0, $1) }
    out += vulkan.lines
    ok = ok && vulkan.passed

    out.append(ok
        ? "overall: PASS - DN complete native runtime host self-tests passed"
        : "overall: FAIL - one or more DN native runtime host self-tests failed")
    return out
}

/// iDroid 0.4 guest-side complete-native gate. libdnfull.dylib is a
/// converted ARM64 ELF image. DN's linker arms its patch island and
/// ELF TLS, binds bionic/libandroid/Vulkan imports, runs constructors,
/// then this harness exercises direct svc/tpidr code, threads, files,
/// dlopen, assets, local JNI objects, NativeActivity lifecycle, direct
/// touch input, window pixels, Vulkan commands and finalizers.
func dnNativeFullFixtureRunTest() -> [String] {
    let url = dnNativeLibraryURL("libdnfull.dylib")
    let path = url.path
    var out: [String] = []
    let previousBreadcrumb = dnNativeFullReadBreadcrumb()
    dnNativeFullResetProgress(previousBreadcrumb: previousBreadcrumb)
    dnNativeFullSetBreadcrumb("prepare runtime paths")

    if let error = dnNativeFullPrepareRuntime() {
        dnNativeFullSetBreadcrumb("complete")
        return [
            "runtime path setup FAILED: \(error)",
            "overall: FAIL - DN native runtime paths could not be configured"
        ]
    }
    out.append("runtime paths configured (files, libraries, assets): OK")
    if let previousBreadcrumb, previousBreadcrumb != "complete" {
        out.append("previous complete-native run stopped at: \(previousBreadcrumb)")
    }

    guard FileManager.default.fileExists(atPath: path) else {
        dnNativeFullSetBreadcrumb("complete")
        return out + [
            "missing converted library: libdnfull.dylib - import it first",
            "overall: FAIL - complete native fixture is missing"
        ]
    }

    var ok = true
    func expect(_ condition: Bool, _ label: String) {
        out.append("\(label): \(condition ? "OK" : "MISMATCH")")
        ok = ok && condition
    }

    dnNativeFullSetBreadcrumb("dlopen libdnfull.dylib")
    _ = dlerror()
    guard let handle = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
        dnNativeFullSetBreadcrumb("complete")
        return out + [
            "dlopen libdnfull.dylib FAILED: \(dlerror().map { String(cString: $0) } ?? "unknown error")",
            "overall: FAIL - complete native fixture did not dlopen"
        ]
    }
    out.append("dlopen libdnfull.dylib OK")

    dnNativeFullSetBreadcrumb("dn-link libdnfull (parse, patch island, TLS, relocations, initializers)")
    dn_native_set_link_progress_path(dnNativeFullProgressURL().path)
    var image: OpaquePointer?
    let linkRC = dn_native_link_image(path, handle, &image)
    dn_native_set_link_progress_path(nil)
    out += dnNativeReportLines(image)
    guard linkRC == 0, let linked = image else {
        out.append("dn-link libdnfull FAILED (rc=\(linkRC))")
        if let image { dn_native_image_dispose(image) }
        dlclose(handle)
        out.append("overall: FAIL - complete native fixture did not link")
        dnNativeFullSetBreadcrumb("complete")
        return out
    }

    func symbol(_ name: String) -> UnsafeMutableRawPointer? {
        dn_native_image_symbol(linked, name)
    }
    func callInt(_ name: String) -> Int32? {
        guard let pointer = symbol(name) else {
            out.append("resolve \(name) FAILED")
            ok = false
            return nil
        }
        let function = unsafeBitCast(pointer, to: (@convention(c) () -> Int32).self)
        dnNativeFullSetBreadcrumb("guest call: \(name)")
        return function()
    }
    func callUInt(_ name: String) -> UInt32? {
        guard let pointer = symbol(name) else {
            out.append("resolve \(name) FAILED")
            ok = false
            return nil
        }
        let function = unsafeBitCast(pointer, to: (@convention(c) () -> UInt32).self)
        dnNativeFullSetBreadcrumb("guest call: \(name)")
        return function()
    }

    if let value = callInt("dn_full_ctor_calls") {
        out.append("dn_full_ctor_calls() = \(value) (expect 1)")
        expect(value == 1, "complete fixture DT_INIT_ARRAY ran once")
    }

    if let value = callInt("dn_full_run_test") {
        out.append("dn_full_run_test() = \(value) (expect 0; negative values are failed steps)")
        expect(value == 0, "guest svc/tpidr/TLS/pthread/file/dlopen sequence")
    }
    if let value = callInt("dn_full_last_step") {
        out.append("dn_full_last_step() after core = \(value) (expect 9)")
        expect(value == 9, "guest core completion step")
    }
    if let value = callUInt("dn_full_checksum") {
        out.append(String(format: "dn_full_checksum() = 0x%08x", value))
        expect(value != 0, "guest core checksum")
    }

    if let value = callInt("dn_vk_run_test") {
        out.append("dn_vk_run_test() = \(value) (expect 0 from the same converted image)")
        expect(value == 0, "guest Vulkan sequence in complete fixture")
    }
    if let value = callInt("dn_vk_last_step") {
        out.append("dn_vk_last_step() = \(value) (expect 10)")
        expect(value == 10, "guest Vulkan completion step")
    }
    if let value = callUInt("dn_vk_checksum") {
        out.append(String(format: "dn_vk_checksum() = 0x%08x (expect 0x1de4992d)", value))
        expect(value == 0x1de4992d, "guest Vulkan BGRA checksum")
    }

    dnNativeFullSetBreadcrumb("NativeActivity host create + guest onCreate")
    if let onCreate = symbol("ANativeActivity_onCreate"),
       let activityHost = dn_native_activity_host_create(64, 48) {
        let createRC = dn_native_activity_host_dispatch_create(activityHost, onCreate)
        out.append("dn_native_activity_host_dispatch_create rc=\(createRC)")
        expect(createRC == 0, "NativeActivity onCreate dispatch")

        dnNativeFullSetBreadcrumb("NativeActivity start + resume (guest window/input callbacks)")
        let startRC = dn_native_activity_host_start(activityHost)
        let resumeRC = dn_native_activity_host_resume(activityHost)
        out.append("NativeActivity start rc=\(startRC), resume rc=\(resumeRC)")
        expect(startRC == 0 && resumeRC == 0, "NativeActivity start/resume")

        if let state = callInt("dn_full_activity_state") {
            out.append("dn_full_activity_state() after resume = \(state) (expect 63)")
            expect(state == 63, "NativeActivity created/started/resumed/window/input/focus callbacks")
        }

        let frameCount = dn_native_activity_host_frame_count(activityHost)
        out.append("NativeActivity frame count after window creation = \(frameCount) (expect 1)")
        expect(frameCount == 1, "guest posted one software window frame")

        let hostChecksum = dn_native_activity_host_window_checksum(activityHost)
        if let guestChecksum = callUInt("dn_full_activity_checksum") {
            out.append(String(
                format: "window checksum host=0x%08x guest=0x%08x",
                hostChecksum, guestChecksum))
            expect(hostChecksum != 0 && hostChecksum == guestChecksum,
                   "guest window pixels match DN host checksum")
        }

        if let value = callInt("dn_full_run_activity_test") {
            out.append("dn_full_run_activity_test() = \(value) (expect 0)")
            expect(value == 0, "guest assets/configuration/local-JNI/property sequence")
        }
        if let value = callInt("dn_full_last_step") {
            out.append("dn_full_last_step() after activity = \(value) (expect 12)")
            expect(value == 12, "guest complete completion step")
        }

        dnNativeFullSetBreadcrumb("NativeActivity direct touch + guest input pump")
        dn_native_activity_host_touch(activityHost, 0, 7, 17.5, 29.25)
        if let value = callInt("dn_full_pump_input") {
            out.append("dn_full_pump_input() = \(value) (expect 42 for direct touch id 7 at 17.5,29.25)")
            expect(value == 42, "guest consumed direct touch event")
        }

        dnNativeFullSetBreadcrumb("NativeActivity resize (guest redraw)")
        dn_native_activity_host_resize(activityHost, 32, 24)
        let resizedFrameCount = dn_native_activity_host_frame_count(activityHost)
        out.append("NativeActivity frame count after resize = \(resizedFrameCount) (expect 2)")
        expect(resizedFrameCount == 2, "guest redrew after window resize")
        let resizedHostChecksum = dn_native_activity_host_window_checksum(activityHost)
        if let guestChecksum = callUInt("dn_full_activity_checksum") {
            out.append(String(
                format: "resized window checksum host=0x%08x guest=0x%08x",
                resizedHostChecksum, guestChecksum))
            expect(resizedHostChecksum != 0 && resizedHostChecksum == guestChecksum,
                   "guest resized window pixels match DN host checksum")
        }

        dnNativeFullSetBreadcrumb("NativeActivity pause + stop + destroy (guest callbacks)")
        let pauseRC = dn_native_activity_host_pause(activityHost)
        let stopRC = dn_native_activity_host_stop(activityHost)
        let destroyRC = dn_native_activity_host_destroy_callbacks(activityHost)
        out.append("NativeActivity pause rc=\(pauseRC), stop rc=\(stopRC), destroy rc=\(destroyRC)")
        expect(pauseRC == 0 && stopRC == 0 && destroyRC == 0,
               "NativeActivity pause/stop/destroy")

        if let state = callInt("dn_full_activity_state") {
            out.append("dn_full_activity_state() after destroy = \(state) (expect 4095)")
            expect(state == 4095, "NativeActivity full lifecycle callback mask")
        }

        dn_native_activity_host_destroy(activityHost)
    } else {
        out.append("NativeActivity host setup FAILED (missing ANativeActivity_onCreate or host allocation)")
        ok = false
    }

    let finalizedPointer = symbol("dn_full_finalized")
    if finalizedPointer == nil {
        out.append("resolve dn_full_finalized FAILED")
        ok = false
    }
    dnNativeFullSetBreadcrumb("guest finalizers")
    let finiRC = dn_native_image_run_finalizers(linked)
    out.append("dn_native_image_run_finalizers(libdnfull) rc=\(finiRC)")
    expect(finiRC == 0, "complete fixture finalizers")
    if let finalizedPointer {
        let function = unsafeBitCast(finalizedPointer, to: (@convention(c) () -> Int32).self)
        let value = function()
        out.append("dn_full_finalized() after finalizers = \(value) (expect 1)")
        expect(value == 1, "complete fixture destructor side effect")
    }

    dnNativeFullSetBreadcrumb("dispose image + dlclose")
    dn_native_image_dispose(linked)
    dlclose(handle)
    dnNativeFullSetBreadcrumb("complete")

    out.append(ok
        ? "overall: PASS - translated ARM64 code ran through DN patch/TLS/bionic/libandroid/NativeActivity/Vulkan successfully"
        : "overall: FAIL - see mismatches above")
    return out
}
