import Foundation
import Metal

// Apple GPU backend for the DroidNative Vulkan-on-Metal driver.
//
// DNGpuVulkan.c is the portable Vulkan core. This file implements the
// small C backend surface from DNGpuVulkanBackend.h with @_cdecl
// functions. The heavy lifting is intentionally narrow and synchronous:
// each validated Vulkan transfer/clear command becomes a Metal blit or
// a one-attachment clear render pass, is committed, and waits for
// completion before QueueSubmit returns. That is not the final
// high-throughput scheduler, but it makes fence/semaphore semantics in
// the 0.3 core exact instead of racy.

private final class DNVKMetalBackend {
    let device: MTLDevice
    let queue: MTLCommandQueue
    let lock = NSLock()

    init?() {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else {
            return nil
        }
        self.device = device
        self.queue = queue
    }
}

private final class DNVKMetalBuffer {
    let buffer: MTLBuffer
    let length: Int
    init(buffer: MTLBuffer) {
        self.buffer = buffer
        self.length = buffer.length
    }
}

private final class DNVKMetalImage {
    let texture: MTLTexture
    let width: Int
    let height: Int
    let format: UInt32
    let bytesPerPixel: Int

    init(texture: MTLTexture, width: Int, height: Int, format: UInt32, bytesPerPixel: Int) {
        self.texture = texture
        self.width = width
        self.height = height
        self.format = format
        self.bytesPerPixel = bytesPerPixel
    }
}

private func dnVKBackend(from pointer: UnsafeMutableRawPointer?) -> DNVKMetalBackend? {
    guard let pointer else { return nil }
    return Unmanaged<DNVKMetalBackend>.fromOpaque(pointer).takeUnretainedValue()
}

private func dnVKBuffer(from pointer: UnsafeMutableRawPointer?) -> DNVKMetalBuffer? {
    guard let pointer else { return nil }
    return Unmanaged<DNVKMetalBuffer>.fromOpaque(pointer).takeUnretainedValue()
}

private func dnVKImage(from pointer: UnsafeMutableRawPointer?) -> DNVKMetalImage? {
    guard let pointer else { return nil }
    return Unmanaged<DNVKMetalImage>.fromOpaque(pointer).takeUnretainedValue()
}

private func dnVKInt(_ value: UInt64) -> Int? {
    guard value <= UInt64(Int.max) else { return nil }
    return Int(value)
}

private func dnVKRunBlit(_ backend: DNVKMetalBackend,
                         _ encode: (MTLBlitCommandEncoder) -> Void) -> Bool {
    guard let commandBuffer = backend.queue.makeCommandBuffer(),
          let encoder = commandBuffer.makeBlitCommandEncoder() else {
        return false
    }
    encode(encoder)
    encoder.endEncoding()
    commandBuffer.commit()
    commandBuffer.waitUntilCompleted()
    return commandBuffer.status == .completed && commandBuffer.error == nil
}

private func dnVKRangeOK(length: Int, offset: UInt64, size: UInt64) -> Bool {
    guard let offset = dnVKInt(offset), let size = dnVKInt(size) else { return false }
    return offset >= 0 && size >= 0 && offset <= length && size <= length - offset
}

@_cdecl("dn_gpu_backend_probe")
func dn_gpu_backend_probe() -> Int32 {
    return DNVKMetalBackend() != nil ? 1 : 0
}

@_cdecl("dn_gpu_backend_create")
func dn_gpu_backend_create() -> UnsafeMutableRawPointer? {
    guard let backend = DNVKMetalBackend() else { return nil }
    return Unmanaged.passRetained(backend).toOpaque()
}

@_cdecl("dn_gpu_backend_destroy")
func dn_gpu_backend_destroy(_ backend: UnsafeMutableRawPointer?) {
    guard let backend else { return }
    Unmanaged<DNVKMetalBackend>.fromOpaque(backend).release()
}

@_cdecl("dn_gpu_backend_wait_idle")
func dn_gpu_backend_wait_idle(_ backendPointer: UnsafeMutableRawPointer?) {
    guard let backend = dnVKBackend(from: backendPointer) else { return }
    backend.lock.lock()
    defer { backend.lock.unlock() }
    if let commandBuffer = backend.queue.makeCommandBuffer() {
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
    }
}

@_cdecl("dn_gpu_backend_create_buffer")
func dn_gpu_backend_create_buffer(_ backendPointer: UnsafeMutableRawPointer?,
                                  _ size: UInt64,
                                  _ outCpuPtr: UnsafeMutablePointer<UnsafeMutableRawPointer?>?) -> UnsafeMutableRawPointer? {
    outCpuPtr?.pointee = nil
    let requestedLength: UInt64 = size == 0 ? 1 : size
    guard let backend = dnVKBackend(from: backendPointer),
          let length = dnVKInt(requestedLength) else {
        return nil
    }
    backend.lock.lock()
    defer { backend.lock.unlock() }
    guard let metalBuffer = backend.device.makeBuffer(length: length, options: [.storageModeShared]) else {
        return nil
    }
    let box = DNVKMetalBuffer(buffer: metalBuffer)
    outCpuPtr?.pointee = metalBuffer.contents()
    return Unmanaged.passRetained(box).toOpaque()
}

@_cdecl("dn_gpu_backend_destroy_buffer")
func dn_gpu_backend_destroy_buffer(_ backend: UnsafeMutableRawPointer?, _ buffer: UnsafeMutableRawPointer?) {
    _ = backend
    guard let buffer else { return }
    Unmanaged<DNVKMetalBuffer>.fromOpaque(buffer).release()
}

@_cdecl("dn_gpu_backend_create_image")
func dn_gpu_backend_create_image(_ backendPointer: UnsafeMutableRawPointer?,
                                 _ width: UInt32,
                                 _ height: UInt32,
                                 _ backendFormat: UInt32) -> UnsafeMutableRawPointer? {
    guard let backend = dnVKBackend(from: backendPointer), width > 0, height > 0 else {
        return nil
    }
    let pixelFormat: MTLPixelFormat
    let bytesPerPixel: Int
    switch backendFormat {
    case 1: // DN_GPU_FORMAT_RGBA8
        pixelFormat = .rgba8Unorm
        bytesPerPixel = 4
    case 2: // DN_GPU_FORMAT_BGRA8
        pixelFormat = .bgra8Unorm
        bytesPerPixel = 4
    case 3: // DN_GPU_FORMAT_R8
        pixelFormat = .r8Unorm
        bytesPerPixel = 1
    default:
        return nil
    }

    backend.lock.lock()
    defer { backend.lock.unlock() }
    let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: pixelFormat,
                                                                width: Int(width),
                                                                height: Int(height),
                                                                mipmapped: false)
    descriptor.usage = [.shaderRead, .shaderWrite, .renderTarget]
    descriptor.storageMode = .shared
    guard let texture = backend.device.makeTexture(descriptor: descriptor) else {
        return nil
    }
    let box = DNVKMetalImage(texture: texture,
                             width: Int(width),
                             height: Int(height),
                             format: backendFormat,
                             bytesPerPixel: bytesPerPixel)
    return Unmanaged.passRetained(box).toOpaque()
}

@_cdecl("dn_gpu_backend_destroy_image")
func dn_gpu_backend_destroy_image(_ backend: UnsafeMutableRawPointer?, _ image: UnsafeMutableRawPointer?) {
    _ = backend
    guard let image else { return }
    Unmanaged<DNVKMetalImage>.fromOpaque(image).release()
}

@_cdecl("dn_gpu_backend_fill_buffer")
func dn_gpu_backend_fill_buffer(_ backendPointer: UnsafeMutableRawPointer?,
                                _ bufferPointer: UnsafeMutableRawPointer?,
                                _ offset: UInt64,
                                _ size: UInt64,
                                _ data: UInt32) -> Int32 {
    guard let backend = dnVKBackend(from: backendPointer),
          let box = dnVKBuffer(from: bufferPointer),
          dnVKRangeOK(length: box.length, offset: offset, size: size),
          let offsetI = dnVKInt(offset),
          let sizeI = dnVKInt(size) else {
        return -1
    }
    if sizeI == 0 { return 0 }
    backend.lock.lock()
    defer { backend.lock.unlock() }

    let firstByte = UInt8(data & 0xff)
    let repeatedBytePattern = UInt32(firstByte) * 0x01010101
    if data == repeatedBytePattern {
        let ok = dnVKRunBlit(backend) { encoder in
            // The iPhoneOS 27.1 SDK imports fillBuffer:range:value: into
            // Swift as __fill(_:range:value:); `fill(buffer:...)` is
            // rejected by swiftc even though the Objective-C name reads
            // naturally.
            encoder.__fill(box.buffer,
                           range: NSRange(location: offsetI, length: sizeI),
                           value: firstByte)
        }
        return ok ? 0 : -1
    }

    // Seed the first pattern word in shared memory, then expand it with
    // GPU blit copies (4 -> 8 -> 16 ...). This preserves the 32-bit
    // Vulkan fill pattern while keeping the bulk of the fill on GPU.
    var pattern = data
    let seedLength = min(sizeI, 4)
    withUnsafeBytes(of: &pattern) { raw in
        if let base = raw.baseAddress {
            box.buffer.contents().advanced(by: offsetI).copyMemory(from: base, byteCount: seedLength)
        }
    }
    if sizeI <= 4 { return 0 }

    var filled = 4
    while filled < sizeI {
        let copyLength = min(filled, sizeI - filled)
        let srcOffset = offsetI
        let dstOffset = offsetI + filled
        let ok = dnVKRunBlit(backend) { encoder in
            encoder.copy(from: box.buffer,
                         sourceOffset: srcOffset,
                         to: box.buffer,
                         destinationOffset: dstOffset,
                         size: copyLength)
        }
        if !ok { return -1 }
        filled += copyLength
    }
    return 0
}

@_cdecl("dn_gpu_backend_copy_buffer")
func dn_gpu_backend_copy_buffer(_ backendPointer: UnsafeMutableRawPointer?,
                                _ srcPointer: UnsafeMutableRawPointer?,
                                _ srcOffset: UInt64,
                                _ dstPointer: UnsafeMutableRawPointer?,
                                _ dstOffset: UInt64,
                                _ size: UInt64) -> Int32 {
    guard let backend = dnVKBackend(from: backendPointer),
          let src = dnVKBuffer(from: srcPointer),
          let dst = dnVKBuffer(from: dstPointer),
          dnVKRangeOK(length: src.length, offset: srcOffset, size: size),
          dnVKRangeOK(length: dst.length, offset: dstOffset, size: size),
          let srcOffsetI = dnVKInt(srcOffset),
          let dstOffsetI = dnVKInt(dstOffset),
          let sizeI = dnVKInt(size) else {
        return -1
    }
    if sizeI == 0 { return 0 }
    backend.lock.lock()
    defer { backend.lock.unlock() }
    let ok = dnVKRunBlit(backend) { encoder in
        encoder.copy(from: src.buffer,
                     sourceOffset: srcOffsetI,
                     to: dst.buffer,
                     destinationOffset: dstOffsetI,
                     size: sizeI)
    }
    return ok ? 0 : -1
}

@_cdecl("dn_gpu_backend_clear_color_image")
func dn_gpu_backend_clear_color_image(_ backendPointer: UnsafeMutableRawPointer?,
                                      _ imagePointer: UnsafeMutableRawPointer?,
                                      _ backendFormat: UInt32,
                                      _ red: Float,
                                      _ green: Float,
                                      _ blue: Float,
                                      _ alpha: Float) -> Int32 {
    guard let backend = dnVKBackend(from: backendPointer),
          let image = dnVKImage(from: imagePointer),
          image.format == backendFormat else {
        return -1
    }
    backend.lock.lock()
    defer { backend.lock.unlock() }
    guard let commandBuffer = backend.queue.makeCommandBuffer() else { return -1 }
    let descriptor = MTLRenderPassDescriptor()
    descriptor.colorAttachments[0].texture = image.texture
    descriptor.colorAttachments[0].loadAction = .clear
    descriptor.colorAttachments[0].storeAction = .store
    descriptor.colorAttachments[0].clearColor = MTLClearColor(red: Double(red),
                                                              green: Double(green),
                                                              blue: Double(blue),
                                                              alpha: Double(alpha))
    guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else {
        return -1
    }
    encoder.endEncoding()
    commandBuffer.commit()
    commandBuffer.waitUntilCompleted()
    return commandBuffer.status == .completed && commandBuffer.error == nil ? 0 : -1
}

@_cdecl("dn_gpu_backend_copy_buffer_to_image")
func dn_gpu_backend_copy_buffer_to_image(_ backendPointer: UnsafeMutableRawPointer?,
                                         _ bufferPointer: UnsafeMutableRawPointer?,
                                         _ bufferOffset: UInt64,
                                         _ bufferRowPitchBytes: UInt32,
                                         _ imagePointer: UnsafeMutableRawPointer?,
                                         _ x: UInt32,
                                         _ y: UInt32,
                                         _ width: UInt32,
                                         _ height: UInt32) -> Int32 {
    guard let backend = dnVKBackend(from: backendPointer),
          let buffer = dnVKBuffer(from: bufferPointer),
          let image = dnVKImage(from: imagePointer),
          width > 0, height > 0,
          Int(x) <= image.width, Int(width) <= image.width - Int(x),
          Int(y) <= image.height, Int(height) <= image.height - Int(y),
          let bufferOffsetI = dnVKInt(bufferOffset) else {
        return -1
    }
    let tightRow = Int(width) * image.bytesPerPixel
    let rowPitch = bufferRowPitchBytes == 0 ? tightRow : Int(bufferRowPitchBytes)
    guard rowPitch >= tightRow else { return -1 }
    let needed = UInt64(rowPitch) * UInt64(height - 1) + UInt64(tightRow)
    guard dnVKRangeOK(length: buffer.length, offset: bufferOffset, size: needed) else {
        return -1
    }
    backend.lock.lock()
    defer { backend.lock.unlock() }
    let ok = dnVKRunBlit(backend) { encoder in
        encoder.copy(from: buffer.buffer,
                     sourceOffset: bufferOffsetI,
                     sourceBytesPerRow: rowPitch,
                     sourceBytesPerImage: rowPitch * Int(height),
                     sourceSize: MTLSize(width: Int(width), height: Int(height), depth: 1),
                     to: image.texture,
                     destinationSlice: 0,
                     destinationLevel: 0,
                     destinationOrigin: MTLOrigin(x: Int(x), y: Int(y), z: 0))
    }
    return ok ? 0 : -1
}

@_cdecl("dn_gpu_backend_copy_image_to_buffer")
func dn_gpu_backend_copy_image_to_buffer(_ backendPointer: UnsafeMutableRawPointer?,
                                         _ imagePointer: UnsafeMutableRawPointer?,
                                         _ x: UInt32,
                                         _ y: UInt32,
                                         _ width: UInt32,
                                         _ height: UInt32,
                                         _ bufferPointer: UnsafeMutableRawPointer?,
                                         _ bufferOffset: UInt64,
                                         _ bufferRowPitchBytes: UInt32) -> Int32 {
    guard let backend = dnVKBackend(from: backendPointer),
          let image = dnVKImage(from: imagePointer),
          let buffer = dnVKBuffer(from: bufferPointer),
          width > 0, height > 0,
          Int(x) <= image.width, Int(width) <= image.width - Int(x),
          Int(y) <= image.height, Int(height) <= image.height - Int(y),
          let bufferOffsetI = dnVKInt(bufferOffset) else {
        return -1
    }
    let tightRow = Int(width) * image.bytesPerPixel
    let rowPitch = bufferRowPitchBytes == 0 ? tightRow : Int(bufferRowPitchBytes)
    guard rowPitch >= tightRow else { return -1 }
    let needed = UInt64(rowPitch) * UInt64(height - 1) + UInt64(tightRow)
    guard dnVKRangeOK(length: buffer.length, offset: bufferOffset, size: needed) else {
        return -1
    }
    backend.lock.lock()
    defer { backend.lock.unlock() }
    let ok = dnVKRunBlit(backend) { encoder in
        encoder.copy(from: image.texture,
                     sourceSlice: 0,
                     sourceLevel: 0,
                     sourceOrigin: MTLOrigin(x: Int(x), y: Int(y), z: 0),
                     sourceSize: MTLSize(width: Int(width), height: Int(height), depth: 1),
                     to: buffer.buffer,
                     destinationOffset: bufferOffsetI,
                     destinationBytesPerRow: rowPitch,
                     destinationBytesPerImage: rowPitch * Int(height))
    }
    return ok ? 0 : -1
}

@_cdecl("dn_gpu_backend_copy_image")
func dn_gpu_backend_copy_image(_ backendPointer: UnsafeMutableRawPointer?,
                               _ srcPointer: UnsafeMutableRawPointer?,
                               _ srcX: UInt32,
                               _ srcY: UInt32,
                               _ dstPointer: UnsafeMutableRawPointer?,
                               _ dstX: UInt32,
                               _ dstY: UInt32,
                               _ width: UInt32,
                               _ height: UInt32) -> Int32 {
    guard let backend = dnVKBackend(from: backendPointer),
          let src = dnVKImage(from: srcPointer),
          let dst = dnVKImage(from: dstPointer),
          src.bytesPerPixel == dst.bytesPerPixel,
          width > 0, height > 0,
          Int(srcX) <= src.width, Int(width) <= src.width - Int(srcX),
          Int(srcY) <= src.height, Int(height) <= src.height - Int(srcY),
          Int(dstX) <= dst.width, Int(width) <= dst.width - Int(dstX),
          Int(dstY) <= dst.height, Int(height) <= dst.height - Int(dstY) else {
        return -1
    }
    backend.lock.lock()
    defer { backend.lock.unlock() }
    let ok = dnVKRunBlit(backend) { encoder in
        encoder.copy(from: src.texture,
                     sourceSlice: 0,
                     sourceLevel: 0,
                     sourceOrigin: MTLOrigin(x: Int(srcX), y: Int(srcY), z: 0),
                     sourceSize: MTLSize(width: Int(width), height: Int(height), depth: 1),
                     to: dst.texture,
                     destinationSlice: 0,
                     destinationLevel: 0,
                     destinationOrigin: MTLOrigin(x: Int(dstX), y: Int(dstY), z: 0))
    }
    return ok ? 0 : -1
}
