# Native Vulkan fixture — iDroid 0.3

> **Status (2026-10-26):** delivered with iDroid 0.3.0. The portable
> driver suite and a real AArch64 ELF link of this fixture pass on the
> host. The remaining gate is the Mac/Xcode build plus the real iPhone
> Metal run; do not treat this fixture as a completed on-device GPU
> proof until the exported `[native-vulkan]` log reports PASS with
> step 10 and checksum `0x1DE4992D`.

`dn_vk.c` is the first guest-side gate for DroidNative's own
Vulkan-on-Metal translation driver. It is an ARM64 Android ELF library
that imports only the Vulkan 1.0 subset implemented in
`app/DroidNative/NativeRuntime/DNGpuVulkan.c`:

- instance / physical device / device / queue creation;
- unified host-visible memory, buffers and 2D BGRA8 images;
- command pools, fences and synchronous queue submission;
- buffer copy and non-repeated-byte 32-bit fill;
- padded buffer→image upload (`bufferRowLength=6` for a 4-pixel row);
- image→image copy and image→buffer readback;
- layout barriers and a BGRA clear whose final bytes are checked with
  FNV-1a (`0x1DE4992D` over twelve `[255,128,64,255]` pixels).

Build/convert/sign it on the Mac with:

```bash
bash scripts/make-vulkan-native.sh
```

Then delete any old `libdnvk.dylib` in iOS Files, AirDrop the new
artifact, import it from **Capabilities → Native Vulkan GPU driver
(iDroid 0.3)**, run the host driver self-test, and finally use
**Link and run Vulkan fixture** from the home-screen launch. Results
are persisted as `[native-vulkan]` console lines.

In iDroid 0.4.0 this same source is also compiled into `translation/full/dn_full.c`, so the complete fixture exercises Vulkan together with patch/TLS, bionic, libandroid and NativeActivity. The 0.3 subset deliberately fails closed for shaders/SPIR-V pipelines,
descriptor sets, render-pass draw and swapchains. Those are the next
driver layers; they are not silently mapped to fake success here.
