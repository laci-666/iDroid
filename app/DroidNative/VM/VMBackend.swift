import Foundation

/// Evenimente din stratul nativ (QEMU) catre UI.
enum VMEvent {
    case serialOutput(String)
    case displayResized(width: Int, height: Int)
    case exited(code: Int32)
}

/// Contractul dintre Swift si motorul de virtualizare.
///
/// The productive implementation (Phase 1, `QEMUBackend`) dynamically loads
/// framework-ul QEMU din bundle si conduce qemu_init/qemu_main_loop pe un
/// dedicated thread (the C bridge), and control (pause/resume/stop) goes via QMP.
/// iOS particularity (from the UTM architecture): QEMU cannot restart cleanly
/// in the same process -> after stop, the app asks for a relaunch.
///
/// This skeleton includes a "stub" backend validating the whole flow of
/// UI/capabilities/JIT before the QEMU build chain (Mac + Xcode) is
/// connected — see scripts/build-qemu-ios.sh.
protocol VMBackend: AnyObject {
    var events: AsyncStream<VMEvent> { get }
    func start(config: VMConfiguration, accelerator: Accelerator, arena: JITArena?) async throws
    func pause() async
    func snapshot(to directory: String) async throws
    func resume() async
    func requestPowerDown() // ACPI powerdown via QMP; the process relaunches afterwards
    /// Writes to the guest serial console (diagnostics/commands if the guest
    /// offers a shell on ttyAMA0). Text arrives without a newline; the caller adds it.
    func sendSerialInput(_ text: String)
}

/// Backend de dezvoltare: simuleaza boot-ul ca sa putem testa UI-ul, cascada de
/// accelerators and JIT acquisition on a real device, before QEMU integration.
final class StubVMBackend: VMBackend {
    private var continuation: AsyncStream<VMEvent>.Continuation?
    private(set) lazy var events: AsyncStream<VMEvent> = AsyncStream { self.continuation = $0 }

    func start(config: VMConfiguration, accelerator: Accelerator, arena: JITArena?) async throws {
        let args = QEMUArgumentsBuilder.build(config: config, accelerator: accelerator)
        continuation?.yield(.serialOutput("[DroidNative] (stub) would start QEMU with arguments:\n" + args.joined(separator: " \\\n  ") + "\n"))
        if let arena {
            continuation?.yield(.serialOutput("[DroidNative] authorized JIT arena: \(arena.size / (1024*1024)) MB (RX \(arena.rx), RW \(arena.rw))\n"))
        }
        if !config.guestImageInstalled {
            continuation?.yield(.serialOutput("[DroidNative] WARNING: the guest image is missing from \(VMConfiguration.guestDirectory.path). Import the Lineage virtio_arm64only package (see guest/README.md).\n"))
        }
        continuation?.yield(.displayResized(width: config.displayWidth, height: config.displayHeight))
    }

    func pause() async {}
    func snapshot(to directory: String) async throws {
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        continuation?.yield(.serialOutput("[DroidNative] (stub) snapshot in \(directory)\n"))
    }
    func resume() async {}
    func requestPowerDown() { continuation?.yield(.exited(code: 0)) }

    func sendSerialInput(_ text: String) {
        continuation?.yield(.serialOutput("[stub] serial input received: \(text)"))
    }
}
