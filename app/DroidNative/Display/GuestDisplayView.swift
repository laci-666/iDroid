import SwiftUI
import MetalKit
import QuartzCore
import IOSurface

/// Guest display surface: an MTKView presenting the framebuffer
/// QEMU si captureaza multitouch-ul.
///
/// Sursa cadrelor (Faza display, docs/11): podul compilat in QEMU
/// (app/QEMUExtra/dn-display.c) exposes the virtio-gpu surface via
/// dn_display_lock_frame(); at each draw we check the sequence and, if there is a
/// new frame, upload it into the BGRA texture and draw it aspectFit. If
/// framework-ul QEMU nu are podul (build vechi), ramane cadrul de test.
///
/// Active touch uses dn_display_send_pointer() via QEMU's main-loop BH
/// (virtio-tablet, absolute guest pixels). TouchTranslator still supplies
/// aspect-fit geometry; its unused Linux-event stream is not built on UIKit.
final class GuestDisplayView: MTKView {
    let touchTranslator = TouchTranslator()
    private var commandQueue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState?
    private var sampler: MTLSamplerState?
    // Software scanout presentation (damage-driven, triple-buffered):
    // the QEMU bridge coalesces every guest gfx_update rectangle; we
    // keep three textures so a new upload never rewrites a texture the
    // GPU may still be sampling, and every slot accumulates the damage
    // it missed while it was not the presented one.  Only the damaged
    // region is uploaded -- at 1320x2868 a full BGRA frame is ~15 MB.
    private var frameTextures: [MTLTexture?] = [nil, nil, nil]
    private var frameSlotDamage: [CGRect?] = [nil, nil, nil]
    private var frameSlotInFlight: [Int] = [0, 0, 0]
    private var pendingSoftwareFrame = false
    private var activeFrameSlot = 0
    private var frameTextureSize: CGSize = .zero
    private var lastSequence: UInt64 = 0
    private var lastPolledSequence: UInt64 = 0
    private var displayLink: CADisplayLink?
    private var damageFrameCount = 0
    private var damageUploadedBytes = 0
    private var damageFullFrameBytes = 0
    // EAGL IOSurface fast path: the four retained BGRA textures are only
    // used while a QEMU acquire holds that slot. The hold is released from
    // the Metal command-buffer completion handler, never at draw return.
    private var iosurfaceTextures: [UInt: MTLTexture] = [:]
    private var lastPresentedIOTicket: UInt64 = 0
    private var iosurfacePresentedFrames = 0
    private var iosurfaceDiagLogged = false
    // 0.4.6: monotonic, low-rate on-device performance telemetry. Guest
    // gfx updates are NOT necessarily visually distinct frames. The two
    // counts (guest updates and host texture uploads) stay separate so we
    // can test whether a lower EDID refresh actually removes guest work.
    private let requestedGuestRefreshHz = UserDefaults.standard.integer(forKey: "dnGuestRefreshRateHz") == 55 ? 55 : 75
    private var perfWindowStart: CFTimeInterval = 0
    private var perfStartSequence: UInt64 = 0
    private var perfStartUploads = 0
    private var perfStartIOFrames = 0
    private var lastGuestUpdateAt: CFTimeInterval = 0
    private var pendingIdleTouch: (sequence: UInt64, started: CFTimeInterval)?
    private var softwareTexture: MTLTexture? { frameTextures[activeFrameSlot] }
    private var metalDiagLogged = false  // 0.2.24: anunt unic cale Metal directa
    // Metal census (0.2.26): content proof of the direct MTLTexture —
    // raw blit copy vs. sampling through the present pipeline.
    private var metalCensusCounter = 0
    private var censusBlitTex: MTLTexture?
    private var censusSampleTex: MTLTexture?
    // 0.2.28: host-side frame latch. SurfaceFlinger reuses/clears the
    // scanout buffers for the next composition while we present at
    // 120 Hz, so sampling the live guest texture kept catching cleared
    // frames (census 0.2.26/0.2.27). The engine publishes a flush
    // sequence right after its glFinish; on the first draw with a new
    // sequence we copy the guest texture into our own texture and
    // present the copy — like a display controller latching the
    // front buffer at page-flip time.
    private var latchedTex: MTLTexture?
    private var lastLatchSeq: UInt64 = 0
    private var latchCounter = 0
    // Touch diagnostics (Log 12): counter for the pointer telemetry in
    // sendBridgePointer (primele evenimente + periodic, in Consola).
    private var pointerDiagCount = 0
    private var pointerDiagGateLogged = false
    private var primaryTouch: UITouch? // stable single direct-finger virtio-tablet pointer
    private var pointerSubmitMaxMs: Double = 0
    private var pointerSubmitSumMs: Double = 0
    private var pointerSubmitSamples = 0

    override init(frame frameRect: CGRect, device: MTLDevice?) {
        super.init(frame: frameRect, device: device ?? MTLCreateSystemDefaultDevice())
        commonInit()
    }

    required init(coder: NSCoder) {
        super.init(coder: coder)
        self.device = MTLCreateSystemDefaultDevice()
        commonInit()
    }

    private func commonInit() {
        isMultipleTouchEnabled = true
        // Event-driven presentation: a lightweight display-link poll
        // asks QEMU for its draw sequence and only then schedules a
        // Metal draw.  A static Android screen therefore costs no
        // fullscreen GPU redraws at 120 Hz; guest animation still gets
        // the full ProMotion rate while frames actually change.
        preferredFramesPerSecond = 120
        enableSetNeedsDisplay = true
        isPaused = true
        framebufferOnly = true
        commandQueue = device?.makeCommandQueue()
        delegate = self
        buildPipeline()
        let link = CADisplayLink(target: self, selector: #selector(pollGuestFrame))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 120, preferred: 120)
        link.add(to: .main, forMode: .common)
        displayLink = link
        // No dnGuestInput observer was connected to QEMU: constructing and
        // posting Linux multitouch event arrays per finger just consumed
        // UIKit time. Keep the translator implementation for a future
        // actual virtio-multitouch device, not on today's tablet hot path.
    }

    deinit {
        displayLink?.invalidate()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        displayLink?.isPaused = (window == nil)
        if window != nil { setNeedsDisplay() }
    }

    func stopPolling() {
        displayLink?.invalidate()
        displayLink = nil
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        setNeedsDisplay()
    }

    @objc private func pollGuestFrame() {
        guard window != nil, dn_display_available() else { return }
        let now = CACurrentMediaTime()
        let sequence = dn_display_sequence()
        if perfWindowStart == 0 {
            perfWindowStart = now
            perfStartSequence = sequence
            perfStartUploads = damageFrameCount
            perfStartIOFrames = iosurfacePresentedFrames
        }
        if sequence != lastPolledSequence {
            lastGuestUpdateAt = now
            if let sample = pendingIdleTouch, sequence > sample.sequence {
                pendingIdleTouch = nil
                let ms = Int((now - sample.started) * 1_000)
                NotificationCenter.default.post(name: .dnPointerDiag,
                    object: "perf: first guest scanout after idle touch = \(ms) ms (not touch-to-photon)")
            }
            lastPolledSequence = sequence
            setNeedsDisplay()
        } else if pendingSoftwareFrame {
            // All texture slots may have been in use when the newest
            // frame arrived. Retry that frame, not stale damage retained
            // for other slots: the latter is used lazily on future
            // frames and must not redraw a static screen three times.
            setNeedsDisplay()
        }
        if let sample = pendingIdleTouch, now - sample.started >= 10 {
            pendingIdleTouch = nil
            NotificationCenter.default.post(name: .dnPointerDiag,
                object: "perf: no guest scanout within 10 s after idle touch")
        }
        let seconds = now - perfWindowStart
        if seconds >= 15, sequence > 0 {
            let updates = sequence >= perfStartSequence ? sequence - perfStartSequence : 0
            let uploads = max(0, damageFrameCount - perfStartUploads)
            let ioFrames = max(0, iosurfacePresentedFrames - perfStartIOFrames)
            let updateRate = Double(updates) / seconds
            let uploadRate = Double(uploads) / seconds
            let ioRate = Double(ioFrames) / seconds
            let age = lastGuestUpdateAt > 0 ? Int((now - lastGuestUpdateAt) * 1_000) : -1
            NotificationCenter.default.post(name: .dnPointerDiag,
                object: "perf: window=\(Int(seconds))s guest-updates/s=\(String(format: "%.1f", updateRate)) host-uploads/s=\(String(format: "%.1f", uploadRate)) iosurface-presents/s=\(String(format: "%.1f", ioRate)) last-frame-age-ms=\(age) (EDID request \(requestedGuestRefreshHz) Hz; not SurfaceFlinger FPS)")
            perfWindowStart = now
            perfStartSequence = sequence
            perfStartUploads = damageFrameCount
            perfStartIOFrames = iosurfacePresentedFrames
        }
    }

    private func buildPipeline() {
        guard let device else { return }
        let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct DNVertexOut { float4 position [[position]]; float2 uv; };
        vertex DNVertexOut dn_vs(uint vid [[vertex_id]]) {
            float2 pos[4] = { float2(-1.0, -1.0), float2(1.0, -1.0),
                              float2(-1.0,  1.0), float2(1.0,  1.0) };
            float2 uv[4]  = { float2(0.0, 1.0), float2(1.0, 1.0),
                              float2(0.0, 0.0), float2(1.0, 0.0) };
            DNVertexOut out;
            out.position = float4(pos[vid], 0.0, 1.0);
            out.uv = uv[vid];
            return out;
        }
        fragment float4 dn_fs(DNVertexOut in [[stage_in]],
                              texture2d<float> tex [[texture(0)]],
                              sampler samp [[sampler(0)]],
                              constant float &flipY [[buffer(0)]]) {
            // The guest framebuffer is opaque by definition (source is pixman
            // x8r8g8b8, unde X se ignora), dar readback-ul GL aduce ce
            // alpha the guest compositor left — after the boot animation,
            // SurfaceFlinger can leave alpha 0 over the whole buffer, and
            // iOS compositing would blend it with black (= black screen with
            // RGB corect dedesubt). Fortam opacitatea aici.
            // Calea Metal directa (0.2.24): MTLTexture-ul de la
            // virgl/ANGLE can require vertical mirroring (y_0_top of the
            // scanout-ului, conventia Husk/UTM tradusa in UV-ul nostru).
            // The classic surface path leaves flipY=0 (proven).
            float2 uv = in.uv;
            if (flipY > 0.5) { uv.y = 1.0 - uv.y; }
            float4 c = tex.sample(samp, uv);
            return float4(c.rgb, 1.0);
        }
        """
        guard let library = try? device.makeLibrary(source: source, options: nil),
              let vs = library.makeFunction(name: "dn_vs"),
              let fs = library.makeFunction(name: "dn_fs") else { return }
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = vs
        desc.fragmentFunction = fs
        desc.colorAttachments[0].pixelFormat = colorPixelFormat
        pipeline = try? device.makeRenderPipelineState(descriptor: desc)

        let sampDesc = MTLSamplerDescriptor()
        sampDesc.minFilter = .linear
        sampDesc.magFilter = .linear
        sampDesc.sAddressMode = .clampToEdge
        sampDesc.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: sampDesc)
    }

    /// Historical contract (alternative frame source via IOSurface).
    func update(surface: IOSurfaceRef) {
        guard let device else { return }
        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: IOSurfaceGetWidth(surface),
            height: IOSurfaceGetHeight(surface),
            mipmapped: false)
        desc.usage = [.shaderRead]
        frameTextures[activeFrameSlot] = device.makeTexture(descriptor: desc, iosurface: surface, plane: 0)
        frameTextureSize = CGSize(width: IOSurfaceGetWidth(surface),
                                   height: IOSurfaceGetHeight(surface))
        frameSlotDamage = [nil, nil, nil]
        touchTranslator.guestSize = frameTextureSize
    }

    private func unionFrameDamage(_ rect: CGRect) {
        for slot in frameSlotDamage.indices {
            frameSlotDamage[slot] = frameSlotDamage[slot]?.union(rect) ?? rect
        }
    }

    private func freeFrameSlot() -> Int? {
        // Prefer another slot over the presented one; fall back to the
        // presented slot only once the GPU has finished reading it.
        for offset in 1...frameTextures.count {
            let slot = (activeFrameSlot + offset) % frameTextures.count
            if frameSlotInFlight[slot] == 0 { return slot }
        }
        return nil
    }

    /// Uploads the latest QEMU frame into a free texture slot, if newer
    /// than what we have.  The bridge's coalesced damage rectangle
    /// bounds the upload; frameworks without the damage bridge fall
    /// back to a full-frame upload into the same triple-buffer path.
    private func uploadLatestFrameIfNeeded() {
        guard let device, dn_display_available() else { return }
        let sequence = dn_display_sequence()
        let isNewFrame = sequence != lastSequence
        guard isNewFrame || pendingSoftwareFrame else { return }

        var bridgeDamage: DNDisplayDamage?
        if isNewFrame {
            var damage = DNDisplayDamage()
            if dn_display_take_damage(&damage) { bridgeDamage = damage }
        }

        var frame = DNDisplayFrame()
        guard dn_display_lock_frame(&frame), let pixels = frame.pixels else { return }
        defer { dn_display_unlock_frame() }
        // If another guest draw landed between taking the damage and
        // locking the surface, its rectangle is still pending in the
        // bridge: keep lastSequence at the damage horizon so the next
        // poll takes and uploads it instead of losing the region.
        lastSequence = bridgeDamage.map { min(frame.sequence, $0.sequence) } ?? frame.sequence
        lastPolledSequence = lastSequence
        let width = Int(frame.width), height = Int(frame.height), stride = Int(frame.stride)
        guard width > 0, height > 0, frame.bpp == 32, stride >= width * 4 else { return }

        let fullRect = CGRect(x: 0, y: 0, width: width, height: height)
        if frameTextureSize != fullRect.size || frameTextures.contains(where: { $0 == nil }) {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
            desc.storageMode = .shared
            desc.usage = [.shaderRead]
            for slot in frameTextures.indices {
                frameTextures[slot] = device.makeTexture(descriptor: desc)
                frameSlotDamage[slot] = fullRect
            }
            frameTextureSize = fullRect.size
            touchTranslator.guestSize = fullRect.size
        }

        if isNewFrame {
            damageFrameCount += 1
            damageFullFrameBytes += width * height * 4
            if let damage = bridgeDamage, damage.generation == frame.generation {
                let rect = CGRect(x: Int(damage.x), y: Int(damage.y),
                                  width: Int(damage.width), height: Int(damage.height))
                    .intersection(fullRect)
                // A malformed/empty rectangle must not leave the new
                // frame pending forever; upload the full surface safely.
                unionFrameDamage(rect.isEmpty ? fullRect : rect)
            } else {
                unionFrameDamage(fullRect)
            }
            pendingSoftwareFrame = true
        }

        guard let slot = freeFrameSlot(),
              let rect = frameSlotDamage[slot],
              let texture = frameTextures[slot] else { return }
        let source = pixels + Int(rect.minY) * stride + Int(rect.minX) * 4
        texture.replace(region: MTLRegionMake2D(Int(rect.minX), Int(rect.minY),
                                                Int(rect.width), Int(rect.height)),
                        mipmapLevel: 0,
                        withBytes: source,
                        bytesPerRow: stride)
        frameSlotDamage[slot] = nil
        activeFrameSlot = slot
        pendingSoftwareFrame = false
        damageUploadedBytes += Int(rect.width) * Int(rect.height) * 4
        if damageFrameCount > 0, damageFrameCount % 600 == 0 {
            let percent = damageFullFrameBytes > 0
                ? Int(100 * damageUploadedBytes / max(1, damageFullFrameBytes)) : 100
            NotificationCenter.default.post(
                name: .dnPointerDiag,
                object: "frame presenter: \(damageFrameCount) frames, damage uploads \(damageUploadedBytes / 1024) KB vs \(damageFullFrameBytes / 1024) KB full-frame (\(percent)%)")
        }
    }

    // MARK: - Direct on-screen touch (core project requirement)

    /// The FIRST active finger is the stable absolute virtio-tablet pointer.
    /// Never select an arbitrary historical/coalesced UITouch from a Set:
    /// that could send an older position after a newer one during a swipe.
    /// Future virtio-multitouch events are kept separate in TouchTranslator.
    private func sendBridgePointer(_ touches: Set<UITouch>, down: Bool) {
        guard dn_display_available() else {
            if !pointerDiagGateLogged {
                pointerDiagGateLogged = true
                NotificationCenter.default.post(
                    name: .dnPointerDiag,
                    object: "touch received, but the touch bridge reports unavailable (dn_display_available=false)")
            }
            return
        }
        guard let touch = primaryTouch, touches.contains(touch) else { return }
        let guest = touchTranslator.guestSize
        guard guest.width > 0, guest.height > 0, bounds.width > 0, bounds.height > 0 else { return }
        let p = touch.preciseLocation(in: self)
        let fitted = TouchTranslator.aspectFitRect(content: guest, into: bounds.size)
        let nx = min(1, max(0, (p.x - fitted.minX) / fitted.width))
        let ny = min(1, max(0, (p.y - fitted.minY) / fitted.height))
        let gx = Int32(nx * Double(guest.width - 1))
        let gy = Int32(ny * Double(guest.height - 1))
        let start = CACurrentMediaTime()
        dn_display_send_pointer(gx, gy, down)
        let submitMs = max(0, (CACurrentMediaTime() - start) * 1_000)
        // New engine: time spent enqueueing to QEMU's main-loop BH (not
        // BQL wait / guest delivery). Old framework: includes the BQL.
        // Neither value is touch-to-photon or Android composition FPS.
        pointerSubmitSamples += 1
        pointerSubmitSumMs += submitMs
        pointerSubmitMaxMs = max(pointerSubmitMaxMs, submitMs)
        pointerDiagCount += 1
        if pointerDiagCount <= 5 || pointerDiagCount % 50 == 0 {
            NotificationCenter.default.post(
                name: .dnPointerDiag,
                object: "pointer #\(pointerDiagCount) guest=(\(gx),\(gy)) down=\(down) (guest \(Int(guest.width))x\(Int(guest.height)); host-queue-ms avg=\(String(format: "%.1f", pointerSubmitSumMs / Double(pointerSubmitSamples))) max=\(String(format: "%.1f", pointerSubmitMaxMs)); new engine nonblocking, legacy may wait; not touch-to-photon)")
            pointerSubmitSamples = 0
            pointerSubmitSumMs = 0
            pointerSubmitMaxMs = 0
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if primaryTouch == nil || primaryTouch?.phase == .ended || primaryTouch?.phase == .cancelled {
            primaryTouch = touches.first
        }
        // Only sample an idle screen: when Android is already animating,
        // its next frame need not have been caused by this finger. Even on
        // an idle screen this measures QEMU scanout, not visible UI latency.
        let now = CACurrentMediaTime()
        if dn_display_available(), lastGuestUpdateAt > 0,
           now - lastGuestUpdateAt >= 0.5, pendingIdleTouch == nil {
            pendingIdleTouch = (sequence: dn_display_sequence(), started: now)
        }
        sendBridgePointer(touches, down: true)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        // UIKit's `touches` contains the CURRENT position. The old code
        // packed all historical coalescedTouches into a Set and picked an
        // arbitrary `.first`: an older position could arrive LAST, making
        // the Android drag jump backwards. Do not replay history to QEMU;
        // no guest virtio-multitouch listener exists for the translation
        // stream yet, so one current sample also avoids waste on UI main.
        sendBridgePointer(touches, down: true)
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        sendBridgePointer(touches, down: false)
        if let primaryTouch, touches.contains(primaryTouch) { self.primaryTouch = nil }
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        sendBridgePointer(touches, down: false)
        if let primaryTouch, touches.contains(primaryTouch) { self.primaryTouch = nil }
    }
}

extension GuestDisplayView: MTKViewDelegate {
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    /// Metal census (0.2.26; latch-timed since 0.2.28): copy the center
    /// 64x64 of the presented (latched) texture two ways — raw blit (storage truth)
    /// and sampled through the same pipeline as the screen — and log
    /// non-black word counts + the texture descriptor. Splits "texture
    /// is black" from "sampling returns black" in one run.
    private func metalCensus(_ tex: MTLTexture, gen: UInt64, guestPtr: UInt, latched: Bool, onLatch: Bool) {
        // 0.2.28: census only on latch draws (timed to the engine's
        // post-glFinish flush sequence), every 240th latch (0.2.29: sparse; the waitUntilCompleted stall is no longer worth paying often now that GL is parked).
        guard onLatch else { return }
        metalCensusCounter += 1
        guard metalCensusCounter % 240 == 1,
              tex.width >= 64, tex.height >= 64,
              let device, let queue = commandQueue,
              let buffer = queue.makeCommandBuffer() else { return }
        if censusBlitTex == nil {
            let d1 = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: 64, height: 64, mipmapped: false)
            d1.storageMode = .shared
            censusBlitTex = device.makeTexture(descriptor: d1)
            let d2 = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: 64, height: 64, mipmapped: false)
            d2.storageMode = .shared
            d2.usage = [.renderTarget, .shaderRead]
            censusSampleTex = device.makeTexture(descriptor: d2)
        }
        guard let blitDst = censusBlitTex, let sampleDst = censusSampleTex,
              let blit = buffer.makeBlitCommandEncoder() else { return }
        let srcX = (tex.width - 64) / 2, srcY = (tex.height - 64) / 2
        blit.copy(from: tex, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: srcX, y: srcY, z: 0),
                  sourceSize: MTLSize(width: 64, height: 64, depth: 1),
                  to: blitDst, destinationSlice: 0, destinationLevel: 0,
                  destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        if let pipeline, let sampler {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = sampleDst
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            if let enc = buffer.makeRenderCommandEncoder(descriptor: pass) {
                enc.setViewport(MTLViewport(originX: 0, originY: 0, width: 64, height: 64, znear: 0, zfar: 1))
                enc.setRenderPipelineState(pipeline)
                enc.setFragmentTexture(tex, index: 0)
                enc.setFragmentSamplerState(sampler, index: 0)
                var flipY: Float = 0
                enc.setFragmentBytes(&flipY, length: MemoryLayout<Float>.size, index: 0)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                enc.endEncoding()
            }
        }
        buffer.commit()
        buffer.waitUntilCompleted()
        func census(_ t: MTLTexture) -> (Int, UInt32) {
            var words = [UInt32](repeating: 0, count: 64 * 64)
            t.getBytes(&words, bytesPerRow: 64 * 4,
                       from: MTLRegion(origin: MTLOrigin(x: 0, y: 0, z: 0),
                                       size: MTLSize(width: 64, height: 64, depth: 1)),
                       mipmapLevel: 0)
            var nz = 0
            for w in words where (w & 0x00FF_FFFF) != 0 { nz += 1 }
            return (nz, words[32 * 64 + 32])
        }
        let (bnz, bc) = census(blitDst)
        let (snz, sc) = census(sampleDst)
        NotificationCenter.default.post(
            name: .dnPointerDiag,
            object: String(format: "👆 metal census #%d seq=%llu src=%@ gptr=0x%lx: blit nz=%d sample nz=%d center blit=0x%08x sample=0x%08x (fmt=%lu mip=%d usage=%lu storage=%lu)",
                            metalCensusCounter, gen, latched ? "latched" : "guest", guestPtr, bnz, snz, bc, sc,
                            UInt(tex.pixelFormat.rawValue), tex.mipmapLevelCount,
                            UInt(tex.usage.rawValue), UInt(tex.storageMode.rawValue)))
    }

    func draw(in view: MTKView) {
        // Calea Metal directa (0.2.24, docs/12): daca podul QEMU ofera
        // MTLTexture-ul scanout-ului (handle nativ virgl/ANGLE), il
        // draw it — the pixels are already on the GPU; the GL name of
        // the 3D textures is not readable across ANGLE contexts (that is why
        // readback-ul dadea negru in GL). Altfel, calea clasica:
        // upload din suprafata QEMU (mod software / firmware).
        var mw: Int32 = 0, mh: Int32 = 0, mgen: UInt64 = 0
        var my0top = false
        var metalTex: MTLTexture?
        let rawPtr = dn_display_available()
            ? dn_display_metal_acquire(&mw, &mh, &my0top, &mgen) : nil
        defer { if let rawPtr { dn_display_metal_release(rawPtr) } }
        if let rawPtr, let device {
            let obj = Unmanaged<AnyObject>.fromOpaque(rawPtr).takeUnretainedValue()
            if let t = obj as? MTLTexture,
               (t.device as AnyObject) === (device as AnyObject) {
                metalTex = t
                if !metalDiagLogged {
                    metalDiagLogged = true
                    NotificationCenter.default.post(
                        name: .dnPointerDiag,
                        object: "direct Metal presentation active (MTLTexture \(mw)x\(mh), y0top=\(my0top))")
                }
                let gs = CGSize(width: Int(mw), height: Int(mh))
                if touchTranslator.guestSize != gs {
                    touchTranslator.guestSize = gs
                }
            }
        }
        // 0.2.28 latch: on the first draw after each engine flush
        // sequence, blit-copy the guest texture into our own texture.
        // Committed on the same queue before the present buffer below,
        // so the copy is ordered before presentation. We then present
        // the copy, immune to the guest clearing its buffer afterwards.
        var didLatch = false
        if let metalTex, let device, let queue = commandQueue, mgen != lastLatchSeq {
            lastLatchSeq = mgen
            latchCounter += 1
            if latchedTex == nil || latchedTex!.width != metalTex.width
                || latchedTex!.height != metalTex.height
                || latchedTex!.pixelFormat != metalTex.pixelFormat {
                let d = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: metalTex.pixelFormat,
                    width: metalTex.width, height: metalTex.height,
                    mipmapped: false)
                d.usage = [.shaderRead]
                latchedTex = device.makeTexture(descriptor: d)
            }
            if let dst = latchedTex, let lb = queue.makeCommandBuffer(),
               let bl = lb.makeBlitCommandEncoder() {
                bl.copy(from: metalTex, to: dst)
                bl.endEncoding()
                lb.commit()
                didLatch = true
            }
        }
        if metalTex == nil { latchedTex = nil }
        if let metalTex {
            metalCensus(latchedTex ?? metalTex, gen: mgen,
                        guestPtr: rawPtr.map { UInt(bitPattern: Int(bitPattern: $0)) } ?? 0,
                        latched: latchedTex != nil, onLatch: didLatch)
        }
        // On EAGL the guest scanout has no native MTLTexture. QEMU instead
        // draws into one of four IOSurfaces and publishes it only after its
        // GLES fence signals. Acquiring pins the slot against GPU writes;
        // release only after THIS Metal presentation command completes.
        var ioWidth: Int32 = 0, ioHeight: Int32 = 0, ioTicket: UInt64 = 0
        var ioFlipY = false
        var ioRaw: UnsafeMutableRawPointer?
        var iosurfaceTex: MTLTexture?
        defer { if let ioRaw { dn_display_iosurface_release(ioRaw) } }
        if metalTex == nil, let device,
           let raw = dn_display_iosurface_acquire(&ioWidth, &ioHeight,
                                                   &ioFlipY, &ioTicket) {
            ioRaw = raw
            // QEMU owns +1 until dn_display_iosurface_release, so this
            // borrowed Swift wrapper must NEVER takeRetainedValue().
            let surface = unsafeBitCast(raw, to: IOSurfaceRef.self)
            let width = Int(ioWidth), height = Int(ioHeight)
            if width > 0, height > 0,
               IOSurfaceGetWidth(surface) == width,
               IOSurfaceGetHeight(surface) == height,
               IOSurfaceGetPixelFormat(surface) == 0x42475241 {
                let key = UInt(bitPattern: raw)
                if let cached = iosurfaceTextures[key] {
                    iosurfaceTex = cached
                } else {
                    let d = MTLTextureDescriptor.texture2DDescriptor(
                        pixelFormat: .bgra8Unorm,
                        width: width, height: height, mipmapped: false)
                    d.storageMode = .shared
                    d.usage = [.shaderRead]
                    iosurfaceTex = device.makeTexture(descriptor: d,
                                                       iosurface: surface, plane: 0)
                    if let iosurfaceTex { iosurfaceTextures[key] = iosurfaceTex }
                }
                if let tex = iosurfaceTex,
                   (tex.device as AnyObject) !== (device as AnyObject) {
                    iosurfaceTex = nil
                }
            }
            if iosurfaceTex == nil {
                dn_display_iosurface_disable() /* fail closed; next guest frame uses PBO */
            } else {
                let guestSize = CGSize(width: width, height: height)
                if touchTranslator.guestSize != guestSize { touchTranslator.guestSize = guestSize }
                if !iosurfaceDiagLogged {
                    iosurfaceDiagLogged = true
                    NotificationCenter.default.post(name: .dnPointerDiag,
                        object: "EAGL IOSurface presenter active (\(width)x\(height), fenced 4-slot GPU path; not Android FPS)")
                }
            }
        }
        if metalTex == nil && iosurfaceTex == nil { uploadLatestFrameIfNeeded() }
        let texToDraw = (latchedTex ?? metalTex) ?? iosurfaceTex ?? softwareTexture
        let softwareSlotDrawn = (metalTex == nil && latchedTex == nil &&
                                 iosurfaceTex == nil && texToDraw != nil)
            ? activeFrameSlot : nil
        guard let drawable = currentDrawable,
              let pass = currentRenderPassDescriptor,
              let queue = commandQueue,
              let buffer = queue.makeCommandBuffer() else { return }
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.02, green: 0.02, blue: 0.03, alpha: 1)
        guard let encoder = buffer.makeRenderCommandEncoder(descriptor: pass) else { return }
        if let tex = texToDraw, let pipeline, let sampler {
            let fitted = TouchTranslator.aspectFitRect(
                content: CGSize(width: tex.width, height: tex.height), into: drawableSize)
            encoder.setViewport(MTLViewport(originX: Double(fitted.minX), originY: Double(fitted.minY),
                                            width: Double(fitted.width), height: Double(fitted.height),
                                            znear: 0, zfar: 1))
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentTexture(tex, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            var flipY: Float = ((metalTex != nil && my0top) ||
                                (iosurfaceTex != nil && ioFlipY)) ? 1 : 0
            encoder.setFragmentBytes(&flipY, length: MemoryLayout<Float>.size, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        encoder.endEncoding()
        if let slot = softwareSlotDrawn {
            frameSlotInFlight[slot] += 1
            buffer.addCompletedHandler { _ in
                DispatchQueue.main.async { [weak self] in
                    guard let self else { return }
                    self.frameSlotInFlight[slot] = max(0, self.frameSlotInFlight[slot] - 1)
                    if self.pendingSoftwareFrame {
                        self.setNeedsDisplay()
                    }
                }
            }
        }
        if let heldIO = ioRaw, iosurfaceTex != nil {
            // The QEMU slot MUST NOT be recycled until the GPU has finished
            // sampling it. Capture an integer address rather than a mutable
            // Swift pointer/view in this cross-thread completion callback.
            let address = UInt(bitPattern: heldIO)
            buffer.addCompletedHandler { _ in
                if let completed = UnsafeMutableRawPointer(bitPattern: address) {
                    dn_display_iosurface_release(completed)
                }
            }
            ioRaw = nil /* completion, not the defer above, now owns +1 */
            if ioTicket != lastPresentedIOTicket {
                lastPresentedIOTicket = ioTicket
                iosurfacePresentedFrames += 1
            }
        }
        buffer.present(drawable)
        buffer.commit()
    }
}

struct GuestDisplayRepresentable: UIViewRepresentable {
    /// 3-finger touch on the guest screen -> toggles host fullscreen
    /// (shows/hides the controls: bottom bar, X button, etc.).
    var onThreeFingerTap: () -> Void = {}

    final class Coordinator: NSObject {
        var onThreeFingerTap: () -> Void = {}
        @objc func handleThreeFingerTap() { onThreeFingerTap() }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> GuestDisplayView {
        let view = GuestDisplayView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        let recognizer = UITapGestureRecognizer(
            target: context.coordinator, action: #selector(Coordinator.handleThreeFingerTap))
        recognizer.numberOfTouchesRequired = 3
        recognizer.numberOfTapsRequired = 1
        // On recognition, touches arrive as "cancelled" in the view -> the guest
        // receives pointer-up, does not keep ghost fingers pressed.
        recognizer.cancelsTouchesInView = true
        view.addGestureRecognizer(recognizer)
        context.coordinator.onThreeFingerTap = onThreeFingerTap
        return view
    }

    func updateUIView(_ uiView: GuestDisplayView, context: Context) {
        context.coordinator.onThreeFingerTap = onThreeFingerTap
    }

    static func dismantleUIView(_ uiView: GuestDisplayView, coordinator: Coordinator) {
        uiView.stopPolling()
    }
}

extension Notification.Name {
    static let dnPointerDiag = Notification.Name("DroidNative.PointerDiag")
}
