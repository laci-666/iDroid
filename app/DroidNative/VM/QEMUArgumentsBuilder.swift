import Foundation

/// Builds the QEMU argument line (qemu-system-aarch64 run as a library,
/// UTM model: qemu_init/qemu_main_loop on a dedicated thread — iOS allows no child processes).
///
/// The test guest has a separate MINIMAL profile (machine + kernel + serial
/// + QMP only): at bring-up, every extra device is a possible cause of
/// `exit()` in qemu_init. The full profile (virtio-*) is for Android.
///
/// Console and QMP go via socketpair: the backend passes the guest-end fd
/// via `-chardev socket,fd=N`. No FIFO/socket on disk, no limit
/// `sun_path`. fd < 0 (stub/log) cade pe variantele clasice pipe/qmp.sock.
enum QEMUArgumentsBuilder {

    /// The Android CPU for TCG modes. Neoverse-V1 in QEMU 10 has
    /// FEAT_PAuth (ID_AA64ISAR1.APA=3), dar nu are FEAT_BTI
    /// (ID_AA64PFR1.BT=0). This fixes two blockages observed on device:
    /// 1) without BTI, the old "Oops - BTI" panic in init no longer appears;
    /// 2) cu PAC, kernelul Android NU mai activeaza dynamic SCS
    /// (UNWIND_PATCH_PAC_INTO_SCS), al carui parser faulta in
    /// `module_finalize/__pi_scs_handle_fde_frame` la module gen virt_wifi.
    /// `pauth-impdef=on` alege algoritmul QEMU rapid, iar `sve=off` tine
    /// profile close to the former neoverse-n1, no SVE paths under the translator.
    private static let androidTCGCPUMode = "neoverse-v1,pauth=on,pauth-impdef=on,sve=off"

    static func build(config: VMConfiguration,
                      accelerator: Accelerator,
                      serialFD: Int32 = -1,
                      qmpFD: Int32 = -1) -> [String] {
        if config.guestKind == .testLinux {
            return buildTestLinux(config: config, accelerator: accelerator,
                                  serialFD: serialFD, qmpFD: qmpFD)
        }
        return buildAndroid(config: config, accelerator: accelerator,
                            serialFD: serialFD, qmpFD: qmpFD)
    }

    // MARK: - Minimal profile (gate 1a: the engine proves itself on serial)

    private static func buildTestLinux(config: VMConfiguration,
                                       accelerator: Accelerator,
                                       serialFD: Int32,
                                       qmpFD: Int32) -> [String] {
        var args: [String] = ["qemu-system-aarch64"]
        appendDataDirectory(to: &args)

        args += ["-machine", "virt,gic-version=3"]
        args += ["-smp", "1"] // un vCPU la bring-up: mai putine variabile
        args += ["-m", "\(config.memoryMegabytes)"]

        switch accelerator {
        case .hvf:
            args += ["-accel", "hvf"]
            args += ["-cpu", "host"]
        case .tcgJIT:
            args += ["-accel", "tcg,thread=multi,split-wx=on,tb-size=\(config.jitArenaMegabytes)"]
            args += ["-cpu", "cortex-a72"]
        case .tcgTCTI:
            args += ["-accel", "tcg,tb-size=128"]
            args += ["-cpu", "cortex-a72"]
        }

        args += ["-kernel", config.testKernelPath]
        args += ["-initrd", config.testInitrdPath]
        args += ["-append", "console=ttyAMA0 earlycon=pl011,0x9000000 loglevel=8"]

        appendSerialAndControl(to: &args, config: config, serialFD: serialFD, qmpFD: qmpFD)
        args += ["-display", "none"]
        return args
    }

    // MARK: - Profil complet Android (virtio cap-coada)

    private static func buildAndroid(config: VMConfiguration,
                                     accelerator: Accelerator,
                                     serialFD: Int32,
                                     qmpFD: Int32) -> [String] {
        var args: [String] = ["qemu-system-aarch64"]
        appendDataDirectory(to: &args)

        // --- Machine and CPU ---
        args += ["-machine", "virt,gic-version=3,highmem=on"]
        // No bootloop after kernel panic (observed on device: panic ->
        // RESET -> UEFI -> din nou): in bring-up preferam oprire cu diagnostic.
        // To revisit when Android is stable and guest "Restart" must
        // sa insemne reboot, nu iesirea VM-ului.
        args += ["-no-reboot"]
        args += ["-smp", "\(config.cpuCount)"]
        args += ["-m", "\(config.memoryMegabytes)"]

        switch accelerator {
        case .hvf:
            // Executie directa la EL2, zero traducere. Ajungem aici doar daca
            // proba de hypervisor a reusit (entitlement activ).
            args += ["-accel", "hvf"]
            args += ["-cpu", "host"]
        case .tcgJIT:
            // split-wx=on is MANDATORY on TXM (iOS 26+): forces the
            // vmremap + brk #0x69 path in the UTM fork's tcg/region.c, for
            // our whole protocol is built on (legacy script).
            // QEMU's default is split-wx=on ONLY in builds with
            // CONFIG_DEBUG_TCG; otherwise it takes the classic MAP_JIT path,
            // which TXM refuses with EPERM at mmap ("allocate ... jit
            // buffer: Operation not permitted” + exit in qemu_init,
            // observat 21.10.2026, logurile 9-11). Nu-l pune pe ramura
            // .tcgTCTI: interpretorul il respinge („jit split-wx not
            // supported") and anyway generates no code.
            if config.useIcount {
                // Diluted virtual time (-icount, FIXED shift): guest timers
                // advances with executed instructions, so the Android Watchdog
                // Android no longer kills system_server just because the host
                // traduce lent (bucla zygote din logurile 2026-10-20).
                // shift=auto realiniaza ceasul la perete si anula protectia;
                // shift=1 (2 ns/instructiune) garanteaza bugetul de lucru
                // per virtual second. icount requires single-thread TCG.
                args += ["-accel", "tcg,split-wx=on,tb-size=\(config.jitArenaMegabytes)"]
                args += ["-icount", "shift=1,sleep=on"]
            } else {
                // Traducere dinamica ARM64->ARM64, multithread (MTTCG).
                args += ["-accel", "tcg,thread=multi,split-wx=on,tb-size=\(config.jitArenaMegabytes)"]
            }
            args += ["-cpu", androidTCGCPUMode]
        case .tcgTCTI:
            // Threaded interpreter (UTM SE). No thread=multi: MTTCG does not
            // aduce nimic interpretorului, doar risc.
            args += ["-accel", "tcg,tb-size=128"]
            if config.useIcount {
                // Same virtual time as in TCG-JIT (the interpreter is even more
                // exposed to guest timeouts measured in real seconds).
                args += ["-icount", "shift=1,sleep=on"]
            }
            // SAME CPU as in TCG-JIT: the JIT->interpreter fall must not
            // change the hardware Android sees between starts.
            // Important for fallback: this model still exposes no BTI
            // (vechiul „Kernel panic - Oops - BTI" din init era de la
            // `-cpu max` under the interpreter), but exposes PAC so as not to enter
            // kernelul in dynamic SCS.
            args += ["-cpu", androidTCGCPUMode]
        }

        // Firmware UEFI: CODE read-only (unit 0) + VARS scriibil (unit 1).
        args += ["-drive", "if=pflash,format=raw,readonly=on,unit=0,file=\(config.firmwarePath)"]
        args += ["-drive", "if=pflash,format=raw,unit=1,file=\(config.firmwareVarsPath)"]

        // Disks: system from OVERLAY (backing = read-only golden), userdata separate.
        args += ["-drive", "if=none,id=sys,format=qcow2,file=\(config.systemOverlayPath),discard=unmap"]
        args += ["-device", "virtio-blk-pci,drive=sys,bootindex=0"]
        // fixed node-name = target of the internal dn-ready snapshot (app/QEMUExtra):
        // VM state + disk snapshot live ONLY in userdata.qcow2.
        args += ["-drive", "if=none,id=usr,format=qcow2,file=\(config.userDiskPath),discard=unmap,node-name=dnvmstate"]
        args += ["-device", "virtio-blk-pci,drive=usr"]

        // Display + input + network + audio (phases 1b/1c/3 actually wire them).
        let gpu = config.gpuAcceleration ? "virtio-gpu-gl-pci" : "virtio-gpu-pci"
        args += ["-device", "\(gpu),xres=\(config.displayWidth),yres=\(config.displayHeight)"]
        // UN SINGUR pointer absolut (Log 12, 2026-10-21): cu virtio-tablet
        // + usb-tablet in paralel, rutarea de input din QEMU
        // (qemu_input_find_handler takes the first compatible handler; USB HID
        // also "activates" itself at enumeration) sends ALL touches
        // onto usb-tablet, which the virtio_arm64only kernel does not bind —
        // touch perfect mut in guest, desi fiecare veriga parea OK.
        // Only virtio-tablet remains (virtio-input, guest-supported).
        // The keyboard returned as virtio-keyboard-pci in 0.2.19:
        // dnq_display_send_text now sends real key events,
        // and the GSI serial console has no shell
        // (androidboot.console=hvc0, which we do not wire) — the keyboard is
        // the only path for user text to reach
        // Android. Snapshot NOTE: new device = machine shape "d2".
        args += ["-device", "virtio-tablet-pci"]
        args += ["-device", "virtio-keyboard-pci"]
        args += ["-netdev", "user,id=net0,hostfwd=tcp::\(config.adbHostPort)-:5555"]
        args += ["-device", "virtio-net-pci,netdev=net0"]
        // NO virtio-sound-pci: it is non-migratable in QEMU 10 and blocks
        // snapshot-ul intern ("State blocked by non-migratable device
        // .../virtio-sound-device", Log 12). We had no host audio path
        // anyway; Android starts normally without a sound card.
        args += ["-device", "virtio-rng-pci"]
        args += ["-device", "virtio-balloon-pci"]

        appendSerialAndControl(to: &args, config: config, serialFD: serialFD, qmpFD: qmpFD)
        args += ["-display", "none"]
        return args
    }

    // MARK: - Piese comune

    /// Date QEMU (ROM-uri etc.) ambalate in bundle; prefixul compilat din
    /// sysroot (the Mac path) does not exist on device.
    private static func appendDataDirectory(to args: inout [String]) {
        if let dataDirectory = Bundle.main.resourceURL?.appendingPathComponent("qemu-data").path {
            args += ["-L", dataDirectory]
        }
    }

    private static func appendSerialAndControl(to args: inout [String],
                                               config: VMConfiguration,
                                               serialFD: Int32,
                                               qmpFD: Int32) {
        // Serial console: socketpair via fd preferred; FIFO only as fallback.
        if serialFD >= 0 {
            args += ["-chardev", "socket,id=dnser0,fd=\(serialFD)"]
        } else {
            args += ["-chardev", "pipe,id=dnser0,path=\(config.serialPipeBasePath)"]
        }
        args += ["-serial", "chardev:dnser0"]

        // QMP: socketpair via fd, or a relative unix socket (cwd = Guest).
        if qmpFD >= 0 {
            args += ["-chardev", "socket,id=dnqmp0,fd=\(qmpFD)"]
            args += ["-mon", "chardev=dnqmp0,mode=control"]
        } else {
            args += ["-qmp", "unix:qmp.sock,server=on,wait=off"]
            args += ["-monitor", "none"]
        }
    }
}
