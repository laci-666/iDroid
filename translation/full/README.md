# Complete native fixture — iDroid 0.4

> **Status (2026-10-26):** implemented for iDroid 0.4.0; 0.4.1 fixed the
> iPhoneOS 27.1 Swift spelling of the Metal fill call; 0.4.2 fixes the
> two defects from the first 0.4.1 device run. On that run, the host
> patch/TLS and bionic self-tests passed on iPhone; libandroid failed
> only `AAssetManager file assets` because the probe was written under
> the files root instead of the sibling assets root (fixed, with the
> host harness now mirroring the device roots); and the fixture closed
> the app instantly at "Link and run". 0.4.2 refreshes/rebuilds the
> converter on every fixture build, requires the `__TEXT,__henge`
> patch-island section in the final dylib, makes the mini-linker refuse
> any image that still contains raw `svc`/`tpidr` sites, and records a
> persistent per-step breadcrumb. The exported 0.4.2 console for the
> repeated "Link and run" crash contained no `[native-full]` lines:
> the breadcrumb was on disk but not in the export, so the exact
> stage was not visible. 0.4.3 puts the breadcrumb and a synchronous
> progress log (including linker sub-stages and each initializer
> call) into the diagnostic export and shows the last stage after
> relaunch; it also validates writable island slots and exported
> function pointers, and the Mac build now rejects guest code that
> uses Darwin's reserved `x18` register. **0.4.5 reproduces a real
> AArch64 SIGSEGV PC=0 in the old fixture:** the guest used a 16-byte
> buffer for `__system_property_get` (contract: 92 bytes) and the
> shim zero-padded 92 bytes, overwriting saved LR. The guest now uses
> 92 bytes and the shim copies only the value + NUL. A new test
> executes the converted Mach-O all the way through the DN patch
> island/assembly, guest code, Vulkan (test backend), NativeActivity,
> touch and finalizers under qemu-aarch64 Linux in **both** emutls and
> genuine PT_TLS/TLSDESC modes. Do not call this an
> **on-device** PASS until the `[native-full]` console lines below
> report overall PASS from a home-screen launch with a regenerated
> fixture; dyld, Apple signing, Darwin TSD fast path and Metal real
> are not exercised by QEMU-user.

`dn_full.c` is the guest-side gate for DroidNative's direct
code-translation runtime. One converted ARM64 ELF image contains all
of the following:

- raw Linux `svc #0` and `mrs tpidr_el0` patch sites;
- ELF TLS template/BSS state, exercised on the main thread and a DN
  bionic worker thread;
- constructors/finalizers and relocated imports;
- DN bionic pthread mutex/key/create/join, stdio over the Android
  VFS, Linux errno storage and `dlopen`/`dlsym` for the DN system
  library surface;
- libandroid assets, configuration, system properties and Android
  log calls;
- local JNI object/string calls through the standard JNI tables
  (this is a local object bridge, not a Java VM);
- `ANativeActivity_onCreate`, lifecycle callbacks, software
  `ANativeWindow` drawing, resize redraw and direct-touch input;
- the complete 0.3 `dn_vk.c` Vulkan sequence compiled into the same
  image, so its `vk*` imports reach DN Vulkan-on-Metal.

Build/convert/sign the fixture on the Mac with:

```bash
bash scripts/make-full-native.sh
```

For a delivery, this command is chained on the same physical
copy-paste line as `scripts/update-build.sh`; do not run a separate
manual conversion flow. Delete any old `libdnfull.dylib` in iOS Files,
AirDrop the new artifact, then in iDroid use **Capabilities →
Complete native translation runtime (iDroid 0.4)**:

1. **Run native runtime self-test**.
2. **Import libdnfull.dylib…**.
3. **Link and run complete native fixture** from a home-screen
   launch.

The Mac script must print `patch island present (__TEXT,__henge)`
and `x18 check: 0 instructions`; if it instead errors on the Mac, do
not import that dylib — fix the converter/toolchain first. If a
device run ever stops before printing its report, just relaunch
iDroid: Capabilities shows `Previous complete-native run stopped
at: <step>`, the Console repeats it, and the diagnostic export
contains `native-full-breadcrumb.txt` plus the synchronous
`native-full-progress.txt`. Do not tap **Link and run** again merely
to reveal the stage; export first.

Expected exported evidence includes:

```text
[native-full] patch/TLS/VFS self-test rc=0: OK
[native-full] bionic self-test rc=0: OK
[native-full] libandroid self-test rc=0: OK
[native-full] Vulkan-on-Metal self-test rc=0: OK
[native-full] overall: PASS - DN complete native runtime host self-tests passed
[native-full] dn_full_run_test() = 0
[native-full] dn_full_last_step() after core = 9
[native-full] dn_vk_run_test() = 0
[native-full] dn_vk_checksum() = 0x1de4992d
[native-full] dn_full_last_step() after activity = 12
[native-full] dn_full_pump_input() = 42
[native-full] dn_full_activity_state() after destroy = 4095
[native-full] overall: PASS - translated ARM64 code ran through DN patch/TLS/bionic/libandroid/NativeActivity/Vulkan successfully
```

The sandbox gate is `scripts/test-native-full.sh`: it links every
NativeRuntime C unit in one process, runs the four runtime
self-tests, then runs the synthetic linker suite covering
DT_NEEDED/GLOB_DAT/JUMP_SLOT/RELATIVE/RELR, ELF TLS and TLSDESC,
Android APS2 packed relocations, and patch-island slot filling. A
real Zig-built AArch64 ELF of this fixture also links through the DN
mini-linker on the host with initializers suppressed.

**Stronger 0.4.5 gate (Linux sandbox, external tools needed):**
`scripts/test-native-full-macho-exec.sh` builds the Android-target
AArch64 fixture **twice** (compiler-emulated TLS and genuine
PT_TLS/TLSDESC), links RELR with `ld.lld`, converts each Mach-O with
the external `henge-convert`, and runs the actual converted code via
`qemu-aarch64` against the DN runtime also compiled as AArch64. It
maps the Mach-O segments with RX/RW rights, arms the patch island,
runs the constructor and every export, and checks exact values,
window pixels and VFS files. Only the test copy's Darwin TSD
fast-path **loads** are bypassed under Linux (one for emutls, two
for PT_TLS); it still executes DN's real register-preserving svc,
TLSDESC and TP slow-entry assembly. The converter's
ad-hoc signature here is **test-only**: never import that artifact on
the phone. From a Linux environment with the tools on PATH:

```bash
bash scripts/test-native-full-macho-exec.sh
```

`DN_ZIG`, `DN_LLD`, `DN_HENGE_CONVERT` and `DN_QEMU_AARCH64` can
point to existing local binaries. The host C self-test also checks a
92-byte property buffer and a canary: the previous zero-padding
implementation fails deterministically. 0.4.5 was tested with this
AArch64 gate, the full host suite and ASan/UBSan (leak detection off
for the existing JNI/ALooper test lifetime allocations).

Honest boundary: this proves DN's native runtime contract against a
hard fixture. It is not yet a complete Android userspace, a Java VM,
a conformant Vulkan driver, or an on-device ELF→Mach-O converter.
Shader pipelines/descriptors/draw/presentation and full Android app
compatibility remain separate driver work and must fail closed rather
than being reported as success.
