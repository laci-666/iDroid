# Imaginea guest Android

> **Flux curent (26.10.2026, surse iDroid 0.4.10):** aplicația descarcă
> și configurează singură LineageOS `virtio_arm64only`; copierea manuală
> de mai jos este doar rezervă/depanare. Profilul software/g0 rămâne
> implicit, cu `icount ON`, 2 vCPU, JIT 384 MB, EDID 75 Hz (55 Hz
> disponibil manual) și `-no-reboot`.
> Proba GPU din Flags folosește clone APFS și ANGLE EAGL. 0.4.8 a
> pornit cu UI vizibilă, dar sacadată chiar la 720×1280; sonda guest
> confirmă SurfaceFlinger **Mesa VirGL**, nu FPS fluid.
> 0.4.9 a folosit al doilea clone separat `ParallelGPUTrial` cu 2 vCPU
> MTTCG fără `icount`; pe iPhone a bootat, iar utilizatorul spune că
> Android merge mult mai bine. 0.4.10 păstrează acea clonă implicită
> în proba paralelă și oferă 3/4 vCPU **numai opțional**, fiecare cu
> clona sa din GPUTrial. Watchdogul/termica pot înrăutăți testul;
> datele software/GPUTrial/parallel-2 nu sunt modificate. Metal a
> încărcat tot full-frame în logul 0.4.9, iar PBO continuă readbackul
> full-frame. Nu avem FPS Android sau verdict iPhone pentru 3/4 vCPU
> (docs/12, 15).

## Imaginea folosită: LineageOS `virtio_arm64only` pre-construit
1. Descarcă pachetul **UTM-VM-lineage-\*.zip** (build `arm64only`) din release-urile proiectului `jqssun/android-lineage-qemu`.
2. Din pachet ai nevoie de discurile qcow2 și de firmware-ul EDK II (`AAVMF_CODE.fd` / fișierul `.fd` din configurația UTM).
3. Copiază-le pe dispozitiv în `Fișiere → DroidNative/Guest/` ca:
   - `AAVMF_CODE.fd`
   - `system-golden.qcow2`  ← imaginea de sistem importată; **aplicația nu o modifică niciodată** (modelul golden+overlay, docs/05 L3). Scrierile guest-ului merg în `system-overlay.qcow2`, creat automat la primul boot.
   - `userdata.qcow2`
   (echivalent: `scripts/fetch-guest-image.sh` pe Mac le pregătește într-un folder gata de AirDrop/Finder File Sharing.)

## Randare software (profilul dovedit)
Aplică `android-software-mode.props` (EGL SwiftShader, fără hwcomposer GPU, fără boot animation) înainte de primele boot-uri. Lecția S5LBox (`CA_ENABLE_MBX2D=0`): folosește renderer-ul software pe care guest-ul îl livrează deja; aduci accelerarea virgl/Metal pe un sistem care bootează stabil, nu depanezi GPU-ul virtual și kernelul în același timp. Instrucțiuni în fișier.

## Guest de test pentru bring-up (înainte de Android)
În `Guest/` pot sta și `test-Image` + `test-initramfs.cpio.gz` (kernel ARM64 + initramfs Alpine/BusyBox cu `evtest`). Cu `GuestKind.testLinux`, aplicația bootează direct kernelul, fără UEFI/Android — porțile de validare 1a–1c din plan (consolă, framebuffer, touch).

## Faza 4: build propriu (control total)
- AOSP target `aosp_cf_arm64_only_phone-userdebug` (Cuttlefish) sau Lineage `lineage_virtio_arm64only`, compilat local:
  ```bash
  repo init -u https://android.googlesource.com/platform/manifest -b android-latest-release
  repo sync -j$(nproc)
  source build/envsetup.sh && lunch aosp_cf_arm64_only_phone-trunk_staging-userdebug && m -j$(nproc)
  ```
- 250 GB disc + un post de lucru Linux serios; NU se poate pe telefon.

## Cerințe de kernel guest (GKI / `kernel-config.fragment`)
Drivere virtio in-tree (`VIRTIO_PCI`, `VIRTIO_BLK`, `VIRTIO_NET`, `DRM_VIRTIO_GPU`, `VIRTIO_INPUT`, `SND_VIRTIO`), Binder activ (`ANDROID_BINDER_IPC`, dispozitivele `binder,hwbinder,vndbinder`), pagină 4 KB. Fragmentul de config de plecare e în `kernel-config.fragment`.

## Aplicații în guest
- F-Droid pentru open-source; APK-uri individuale prin funcția „Instalează APK" (Faza 2).
- **Fără Google Play Services** (nu pot fi redistribuite; certificarea Google nu e posibilă pentru un dispozitiv virtual neoficial). microG = opțiune a utilizatorului, fără garanții. Aplicațiile cu Play Integrity tare (bănci, DRM) nu vor porni — e o limită de politică Google, nu un defect al VM-ului.
