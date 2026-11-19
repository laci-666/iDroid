# DroidNative

DroidNative runs a LineageOS/Android ARM64 guest on iPhone using QEMU TCG-JIT. It is an experimental iOS system-VM project; no jailbreak is required. A separate native-code translation path is also present, but it is experimental and does not provide general Android app compatibility.

## Status

Source version: **0.4.13**.

- Linux/offline validation of the source and release archive: **374 passed, 0 failed, 1 skipped**. The IOSurface and asynchronous-touch C paths passed sanitizer checks.
- The latest recorded iPhone GPU-trial boot is **0.4.9**, using the 2-vCPU parallel profile. Android booted and was reported more responsive; no Android FPS measurement was made.
- A Mac/Xcode build and iPhone run of **0.4.13** have not been verified. The new IOSurface presenter has not been tested on-device.

The target device is an iPhone 16 Pro Max running iOS 27.2. The project declares iOS 26 as its deployment target; other devices and versions are unverified.

## Android graphics

The default profile uses software guest rendering, 2 vCPUs, `icount` on, and supports `g0` snapshots. The optional GPU trial uses Android's `virtio-gpu-gl`/Mesa VirGL path with ANGLE OpenGL/EAGL on iOS. SurfaceFlinger reported Mesa VirGL in an earlier device trial.

GPU trials use separate APFS copy-on-write disk clones and do not support snapshots. The parallel CPU/GPU trial requires an existing `GPUTrial` clone. Its 2-vCPU profile has been tested on-device; 3- and 4-vCPU profiles are experimental.

The optional IOSurface presenter is limited to the 2-vCPU parallel trial at resolutions up to 720×1280. It falls back to the PBO path; turn it off to force PBO. This 0.4.13 path is not verified on iPhone. The usual display settings are 720×1280 and a 75 Hz EDID request; neither setting establishes Android FPS. **60/120 FPS have not been demonstrated.**

## Build on macOS

Building the iOS app and QEMU engine requires macOS, Xcode, Homebrew, network access, and several gigabytes of free space. The first QEMU build can take 30–90 minutes. Install the prerequisites listed then run from the repository root:

```sh
DN_ENGINE=tcg ./scripts/build-qemu-ios.sh
DN_ENGINE=tcg ./scripts/stage-qemu-frameworks.sh
cd app
xcodegen generate
open DroidNative.xcodeproj
```

Sign the app with your own Apple development account and provisioning profile. JIT also requires device-side setup; see [`docs/08-BCD-JIT-INCORPORAT.md`](docs/08-BCD-JIT-INCORPORAT.md) and [`docs/09-JIT-TCG.md`](docs/09-JIT-TCG.md). The app's **Setup → Automatic Setup** flow downloads and prepares the Android guest. Guest images and Google Play services are not included in this repository.

## Tests

Run the offline regression suite from the repository root:

```sh
bash scripts/verify-p0.sh
```

These checks cover source invariants, packaging and C fixtures. They do not replace an Xcode build or an on-device test.

## Repository layout

- `app/DroidNative/` — iOS app, VM configuration, display, touch and JIT integration.
- `app/QEMUExtra/` — QEMU display, touch and snapshot bridge.
- `scripts/`, `patches/`, `tests/` — build tools, QEMU integration and test fixtures.
- `guest/` — Android guest configuration.
- `translation/` — experimental native AArch64 translation/runtime.
- `docs/` — architecture, build and status documentation.

## License and security

No project-level license has been declared. Third-party components retain their respective licenses; review their notices before redistribution. Add a project license before granting reuse rights.

Do not commit signing identities, provisioning profiles, private keys, pairing data, VM disks or unredacted diagnostics. `.gitignore` excludes common generated and signing files, but does not detect secrets written into source or documentation.
