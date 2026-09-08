/*
 * The presenter: the driver's own Metal path from the game's frame to the
 * screen.
 *
 * Copyright 2026 kageroumado
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

import Foundation
import Metal
import ObjectiveC
import QuartzCore

// MARK: - The C surface the driver calls

/// Brings the presenter up for this process.
///
/// - Parameters:
///   - upscaler: the `Upscaler` option: `off`, `lanczos`, `metalfx`, or a
///     shader package name.
///   - filter: the `FinalFilter` option: `nearest`, `bilinear` or `lanczos`.
///   - shaderDirectories: the colon-separated package search path
///     (`SEVO_SHADER_DIR`), or NULL for the default.
///   - trace: non-zero to trace frames to stderr (`PresenterLog`).
///   - debug: the `PresenterDebug` option; `clear` paints the drawable red
///     instead of the frame.
/// - Returns: 1 when the presenter has a Metal device and its shaders, 0
///   when the driver should behave as if the option were off.
@_cdecl("sevo_presenter_init")
public func sevoPresenterInit(
    _ upscaler: UnsafePointer<CChar>?, _ filter: UnsafePointer<CChar>?,
    _ shaderDirectories: UnsafePointer<CChar>?, _ trace: Int32, _ debug: UnsafePointer<CChar>?
) -> Int32 {
    let config = PresenterConfig(
        upscaler: upscaler.map { String(cString: $0) } ?? "off",
        filter: FinalFilter(option: filter.map { String(cString: $0) } ?? ""),
        shaderDirectories: shaderDirectories.map { String(cString: $0) } ?? "",
        tracing: trace != 0,
        debugClear: debug.map { String(cString: $0).lowercased() == "clear" } ?? false
    )
    return Presenter.shared.start(config) ? 1 : 0
}

/// Takes over presentation for one Metal view. The renderer keeps drawing
/// into `rendererLayer`, which is never on screen; the layer this returns
/// through `sevo_presenter_onscreen_layer` is the view's backing layer.
///
/// - Returns: a retained handle, released by `sevo_presenter_detach`, or
///   NULL when the presenter is not running.
@_cdecl("sevo_presenter_attach")
public func sevoPresenterAttach(_ rendererLayer: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer? {
    let layer = Unmanaged<CAMetalLayer>.fromOpaque(rendererLayer).takeUnretainedValue()
    guard let presenter = MetalViewPresenter(rendererLayer: layer) else { return nil }
    return Unmanaged<ViewPresenter>.passRetained(presenter).toOpaque()
}

/// Takes over one GDI window surface: `bits` is the DIB the program draws
/// into, BGRA top-down, `bytesPerRow` apart, `width`×`height` pixels, in an
/// allocation of `size` bytes. The DIB stays the program's; the presenter
/// reads it only inside `sevo_presenter_surface_flush`, under the caller's
/// surface lock, and calls `release(context)` once the handle is gone.
///
/// - Returns: a retained handle, released by `sevo_presenter_detach`, or
///   NULL when the presenter is not running, in which case `release` is
///   never called.
@_cdecl("sevo_presenter_attach_surface")
public func sevoPresenterAttachSurface(
    _ bits: UnsafeMutableRawPointer, _ size: Int, _ bytesPerRow: Int32, _ width: Int32, _ height: Int32,
    _ release: @escaping @convention(c) (UnsafeMutableRawPointer?) -> Void, _ context: UnsafeMutableRawPointer?
) -> UnsafeMutableRawPointer? {
    guard let presenter = SurfacePresenter(
        bits: bits, size: size, bytesPerRow: Int(bytesPerRow), width: Int(width), height: Int(height),
        deallocator: { _, _ in release(context) })
    else { return nil }
    return Unmanaged<ViewPresenter>.passRetained(presenter).toOpaque()
}

/// Presents the last frame again. A layer whose content the system dropped
/// while the window was hidden or covered gets its picture back this way; a
/// program that has finished drawing does not flush again.
@_cdecl("sevo_presenter_surface_refresh")
public func sevoPresenterSurfaceRefresh(_ handle: UnsafeMutableRawPointer) {
    guard let presenter = Unmanaged<ViewPresenter>.fromOpaque(handle).takeUnretainedValue() as? SurfacePresenter
    else { return }
    presenter.refresh()
}

/// The program finished drawing `left, top, right, bottom` of a surface, in
/// DIB pixels. Called with win32u's surface lock held, which is what makes
/// the DIB readable here; the rectangle is copied out before this returns.
///
/// - Returns: 1 when the rectangle was taken, 0 when every staging slot is
///   in flight and the caller keeps the rectangle dirty for its next flush.
@_cdecl("sevo_presenter_surface_flush")
public func sevoPresenterSurfaceFlush(
    _ handle: UnsafeMutableRawPointer, _ left: Int32, _ top: Int32, _ right: Int32, _ bottom: Int32
) -> Int32 {
    guard let presenter = Unmanaged<ViewPresenter>.fromOpaque(handle).takeUnretainedValue() as? SurfacePresenter
    else { return 1 }
    return presenter.flush(left: Int(left), top: Int(top), right: Int(right), bottom: Int(bottom)) ? 1 : 0
}

/// The on-screen layer of a view's presenter, unretained.
@_cdecl("sevo_presenter_onscreen_layer")
public func sevoPresenterOnscreenLayer(_ handle: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer {
    let presenter = Unmanaged<ViewPresenter>.fromOpaque(handle).takeUnretainedValue()
    return Unmanaged.passUnretained(presenter.onscreen).toOpaque()
}

/// The view's geometry changed. Main thread.
///
/// - Parameters:
///   - deviceScale: device pixels per point of the view, the presentation
///     scale of a resized window included.
///   - rendererScale: the source's pixels per point, which is Wine's Retina
///     scale: the renderer layer's `contentsScale`, or the DIB's density.
///   - width, height: the view's bounds in points.
@_cdecl("sevo_presenter_layout")
public func sevoPresenterLayout(
    _ handle: UnsafeMutableRawPointer, _ deviceScale: Double, _ rendererScale: Double,
    _ width: Double, _ height: Double
) {
    let presenter = Unmanaged<ViewPresenter>.fromOpaque(handle).takeUnretainedValue()
    presenter.layout(deviceScale: deviceScale, rendererScale: rendererScale, size: CGSize(width: width, height: height))
}

/// A drawable for the renderer to draw the next frame into, autoreleased the
/// way `-[CAMetalLayer nextDrawable]` returns its own, or NULL when no frame
/// can be taken right now.
@_cdecl("sevo_presenter_next_drawable")
public func sevoPresenterNextDrawable(_ handle: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer? {
    guard let presenter = Unmanaged<ViewPresenter>.fromOpaque(handle).takeUnretainedValue() as? MetalViewPresenter,
          let drawable = presenter.nextDrawable()
    else { return nil }
    return Unmanaged.passRetained(drawable).autorelease().toOpaque()
}

@_cdecl("sevo_presenter_detach")
public func sevoPresenterDetach(_ handle: UnsafeMutableRawPointer) {
    let presenter = Unmanaged<ViewPresenter>.fromOpaque(handle)
    presenter.takeUnretainedValue().willDetach()
    presenter.release()
}

// MARK: - Configuration

/// The resampling of the last pass into the window.
enum FinalFilter {
    case nearest
    case bilinear
    case lanczos

    init(option: String) {
        switch option.lowercased().first {
        case "n": self = .nearest
        case "b": self = .bilinear
        default: self = .lanczos
        }
    }
}

struct PresenterConfig {
    let upscaler: String
    let filter: FinalFilter
    let shaderDirectories: String
    /// Frames and layouts are traced to stderr.
    let tracing: Bool
    /// The final pass paints red instead of the frame.
    let debugClear: Bool

    var upscalerName: String { upscaler.lowercased() }
}

// MARK: - The process-wide half

/// One per process: the device, the compiled final pass, the command-buffer
/// hook, and the configuration every view's presenter is built from.
final class Presenter: @unchecked Sendable {
    static let shared = Presenter()

    private(set) var device: MTLDevice?
    private(set) var config = PresenterConfig(
        upscaler: "off", filter: .lanczos, shaderDirectories: "", tracing: false, debugClear: false)
    private(set) var finalPass: FinalPass?
    private var hooked = false

    var tracing: Bool { config.tracing }
    var debugClear: Bool { config.debugClear }

    func start(_ config: PresenterConfig) -> Bool {
        self.config = config
        guard let device = MTLCreateSystemDefaultDevice() else {
            log("no Metal device; presenter off")
            return false
        }
        guard let finalPass = FinalPass(device: device) else {
            log("final pass shaders did not compile; presenter off")
            return false
        }
        self.device = device
        self.finalPass = finalPass
        hookCommandBuffers(device: device)
        log("on: upscaler \(config.upscalerName) filter \(config.filter) device \(device.name)")
        return true
    }

    /// The scaler the configuration asks for, built fresh for one view.
    /// A choice this engine cannot honor becomes plain resampling, said once.
    func makeScaler() -> Scaler? {
        guard let device else { return nil }
        switch config.upscalerName {
        case "off", "lanczos", "passthrough":
            return nil
        case "metalfx", "metalfx-spatial":
            if let scaler = SpatialScaler(device: device) { return scaler }
            log("MetalFX Spatial is not available on \(device.name); resampling only")
            return nil
        case let name:
            guard let directory = MPVHookScaler.locate(name, in: config.shaderDirectories) else {
                log("shader package '\(name)' not found (search path \(config.shaderDirectories.isEmpty ? "the default" : config.shaderDirectories)); resampling only")
                return nil
            }
            guard let finalPass, let scaler = MPVHookScaler(device: device, directory: directory, vertexFunction: finalPass.vertexFunction) else {
                log("shader package '\(name)' did not load; resampling only")
                return nil
            }
            return scaler
        }
    }

    // MARK: The command-buffer hook

    /// Presents reach the driver through `-[MTLCommandBuffer presentDrawable:]`
    /// and its two timed variants on the concrete class the device hands out,
    /// which is the class the renderer's buffers have too. A buffer presenting
    /// one of the presenter's own drawables gets the final pass encoded into
    /// it, then presents the real drawable; any other drawable passes through.
    private func hookCommandBuffers(device: MTLDevice) {
        guard !hooked else { return }
        hooked = true
        guard let queue = device.makeCommandQueue(), let buffer = queue.makeCommandBuffer(),
              let cls = object_getClass(buffer)
        else {
            log("could not learn the command buffer class; presents are not hooked")
            return
        }
        if let method = class_getInstanceMethod(cls, #selector(MTLCommandBuffer.present(_:))) {
            CommandBufferHook.present = unsafeBitCast(method_getImplementation(method), to: CommandBufferHook.PresentIMP.self)
            let replacement: CommandBufferHook.PresentIMP = { buffer, selector, drawable in
                if let proxy = drawable as? SevoDrawable, let commandBuffer = buffer as? MTLCommandBuffer {
                    proxy.presenter.present(proxy, in: commandBuffer) { real in CommandBufferHook.present?(buffer, selector, real) }
                } else {
                    CommandBufferHook.present?(buffer, selector, drawable)
                }
            }
            method_setImplementation(method, unsafeBitCast(replacement, to: IMP.self))
        }
        if let method = class_getInstanceMethod(cls, #selector(MTLCommandBuffer.present(_:atTime:))) {
            CommandBufferHook.presentAtTime = unsafeBitCast(method_getImplementation(method), to: CommandBufferHook.PresentTimedIMP.self)
            let replacement: CommandBufferHook.PresentTimedIMP = { buffer, selector, drawable, time in
                if let proxy = drawable as? SevoDrawable, let commandBuffer = buffer as? MTLCommandBuffer {
                    proxy.presenter.present(proxy, in: commandBuffer) { real in CommandBufferHook.presentAtTime?(buffer, selector, real, time) }
                } else {
                    CommandBufferHook.presentAtTime?(buffer, selector, drawable, time)
                }
            }
            method_setImplementation(method, unsafeBitCast(replacement, to: IMP.self))
        }
        if let method = class_getInstanceMethod(cls, #selector(MTLCommandBuffer.present(_:afterMinimumDuration:))) {
            CommandBufferHook.presentAfterDuration = unsafeBitCast(method_getImplementation(method), to: CommandBufferHook.PresentTimedIMP.self)
            let replacement: CommandBufferHook.PresentTimedIMP = { buffer, selector, drawable, duration in
                if let proxy = drawable as? SevoDrawable, let commandBuffer = buffer as? MTLCommandBuffer {
                    proxy.presenter.present(proxy, in: commandBuffer) { real in CommandBufferHook.presentAfterDuration?(buffer, selector, real, duration) }
                } else {
                    CommandBufferHook.presentAfterDuration?(buffer, selector, drawable, duration)
                }
            }
            method_setImplementation(method, unsafeBitCast(replacement, to: IMP.self))
        }
        buffer.commit()
        log("presents hooked on \(String(cString: class_getName(cls)))")
    }
}

/// The implementations the hook replaced, kept where a C function pointer
/// can reach them: a replacement captures nothing.
enum CommandBufferHook {
    typealias PresentIMP = @convention(c) (AnyObject, Selector, AnyObject) -> Void
    typealias PresentTimedIMP = @convention(c) (AnyObject, Selector, AnyObject, CFTimeInterval) -> Void
    nonisolated(unsafe) static var present: PresentIMP?
    nonisolated(unsafe) static var presentAtTime: PresentTimedIMP?
    nonisolated(unsafe) static var presentAfterDuration: PresentTimedIMP?
}

/// One line on stderr, prefixed so it can be picked out of the wine log.
func log(_ message: String) {
    fputs("sevo:presenter \(message)\n", stderr)
}

// MARK: - One view's presenter

/// What every presented view shares: the on-screen layer at the device
/// pixels the view covers, the scaler chain, and the final pass that draws
/// a frame into one of the layer's drawables. A subclass supplies the
/// frames: a Metal renderer's, or a GDI surface's.
class ViewPresenter: NSObject {
    let device: MTLDevice
    let finalPass: FinalPass
    let filter: FinalFilter
    let scaler: Scaler?
    private let renderPass = MTLRenderPassDescriptor()

    private let pixelFormat: MTLPixelFormat
    private let contentsScale: CGFloat
    private var onscreenLayer: CAMetalLayer?

    /// The final pass's Lanczos intermediates, one per on-screen drawable
    /// the layer can have in flight, indexed by the acquisition that took
    /// the drawable: drawable N+3 is handed out only once N is on screen,
    /// after N's command buffer completed, so the intermediate N drew into
    /// is free by then whatever queue the frames were encoded on.
    private var intermediates: [MTLTexture?] = []
    private var acquisitions = 0

    private let statsLock = NSLock()
    private var presented = 0
    private var dropped = 0
    private var lastSummary = CFAbsoluteTimeGetCurrent()

    /// The view's own layer, at the device pixels the view covers, made on
    /// the main thread when the driver first asks for it. Never create it on
    /// another thread: such a `CAMetalLayer` joins the layer tree and draws
    /// its background, but nothing presented into it reaches the screen.
    var onscreen: CAMetalLayer {
        if let onscreenLayer { return onscreenLayer }
        dispatchPrecondition(condition: .onQueue(.main))
        let layer = CAMetalLayer()
        layer.device = device
        layer.framebufferOnly = true
        layer.isOpaque = true
        layer.backgroundColor = CGColor(gray: 0, alpha: 1)
        layer.magnificationFilter = .nearest
        layer.minificationFilter = .nearest
        layer.maximumDrawableCount = 3
        layer.allowsNextDrawableTimeout = true
        layer.pixelFormat = pixelFormat
        layer.contentsScale = contentsScale
        configure(layer)
        onscreenLayer = layer
        return layer
    }

    /// What a subclass sets on the fresh layer.
    func configure(_ layer: CAMetalLayer) {}

    /// The driver is about to release its handle, on the driver's thread;
    /// what a subclass must finish before its last reference can go.
    func willDetach() {}

    init?(pixelFormat: MTLPixelFormat, contentsScale: CGFloat) {
        let shared = Presenter.shared
        guard let device = shared.device, let finalPass = shared.finalPass else { return nil }
        self.device = device
        self.finalPass = finalPass
        self.filter = shared.config.filter
        self.scaler = shared.makeScaler()
        self.pixelFormat = pixelFormat
        self.contentsScale = contentsScale
        super.init()
    }

    // MARK: Geometry (main thread)

    func layout(deviceScale: Double, rendererScale: Double, size: CGSize) {
        let scale = deviceScale > 0 ? deviceScale : 1
        let pixels = CGSize(width: (size.width * scale).rounded(), height: (size.height * scale).rounded())
        if onscreen.contentsScale != scale { onscreen.contentsScale = scale }
        if onscreen.drawableSize != pixels, pixels.width >= 1, pixels.height >= 1 {
            onscreen.drawableSize = pixels
        }
        layoutChanged(rendererScale: rendererScale, size: size)
        if Presenter.shared.tracing {
            log("layout view \(Int(size.width))x\(Int(size.height)) scale \(scale) -> onscreen \(Int(pixels.width))x\(Int(pixels.height)) source scale \(rendererScale)")
        }
    }

    /// The source's side of a layout: the view's bounds in points and the
    /// source's pixels per point.
    func layoutChanged(rendererScale: Double, size: CGSize) {}

    // MARK: The frame

    /// Encodes the scaler chain and the final pass from `source` into one of
    /// the on-screen layer's drawables and returns that drawable for the
    /// caller to present on the same command buffer, or `nil` when the layer
    /// had none to give (the frame is dropped). One caller at a time per
    /// presenter: the renderer's thread, or the surface's display link.
    func encodeFrame(source: MTLTexture, in commandBuffer: MTLCommandBuffer) -> CAMetalDrawable? {
        guard let real = onscreen.nextDrawable() else {
            statsLock.lock()
            dropped += 1
            statsLock.unlock()
            summarize()
            return nil
        }
        if intermediates.isEmpty {
            intermediates = Array(repeating: nil, count: max(1, onscreen.maximumDrawableCount))
        }
        acquisitions += 1
        let slot = acquisitions % intermediates.count
        let target = real.texture
        let scaled = scaler?.encode(source: source, target: MTLSize(width: target.width, height: target.height, depth: 1), in: commandBuffer) ?? source
        finalPass.encode(
            source: scaled, into: target, filter: filter, pass: renderPass,
            intermediate: { width, height in self.intermediate(slot, width: width, height: height) },
            in: commandBuffer)
        statsLock.lock()
        presented += 1
        statsLock.unlock()
        summarize()
        return real
    }

    /// The intermediate of one slot at the size the final pass asks for,
    /// remade when that changes; the texture a command buffer still reads
    /// lives on until it completes.
    private func intermediate(_ slot: Int, width: Int, height: Int) -> MTLTexture? {
        if let texture = intermediates[slot], texture.width == width, texture.height == height { return texture }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: FinalPass.intermediateFormat, width: width, height: height, mipmapped: false)
        descriptor.usage = [.shaderRead, .renderTarget]
        descriptor.storageMode = .private
        guard let texture = device.makeTexture(descriptor: descriptor) else {
            log("could not allocate a \(width)x\(height) final pass intermediate")
            return nil
        }
        texture.label = "sevo final intermediate \(slot)"
        intermediates[slot] = texture
        return texture
    }

    /// The source's part of the trace summary.
    var sourceDescription: String { "" }

    private func summarize() {
        guard Presenter.shared.tracing else { return }
        let now = CFAbsoluteTimeGetCurrent()
        statsLock.lock()
        let due = now - lastSummary >= 5
        if due { lastSummary = now }
        let counts = (presented, dropped)
        statsLock.unlock()
        guard due else { return }
        log("presented \(counts.0) dropped \(counts.1) \(sourceDescription) onscreen \(Int(onscreen.drawableSize.width))x\(Int(onscreen.drawableSize.height))")
    }
}

// MARK: - A Metal view's presenter

/// The presenter of one Metal view: the renderer's layer it answers
/// drawables for, and the ring of source textures those drawables wrap.
/// The renderer's present of one of them becomes a present of the
/// on-screen layer's real drawable, on the renderer's own command buffer.
///
/// A source texture is leased: the proxy drawable holds it while the
/// renderer has it, and each command buffer that presents it holds it until
/// that buffer completes. The lease ends when both have let go, and only
/// then can the texture back another drawable. A caller past the ring's
/// depth waits for a lease to end, the way `-[CAMetalLayer nextDrawable]`
/// waits for a drawable, honoring the renderer layer's
/// `allowsNextDrawableTimeout`: one second and then `nil` when it is set,
/// indefinitely when the renderer turned it off.
///
/// A drawable the renderer drops without presenting is the one case the
/// present hook cannot see through: work encoded against its texture may
/// still be in flight on a command buffer the presenter never met. Such a
/// slot is held back until the presenter's next two on-screen presents
/// have completed, which orders it after anything the renderer committed
/// before those presents. That is the supported renderer's behavior
/// (D3DMetal presents every drawable it draws into), stated here as the
/// rule the pool relies on.
final class MetalViewPresenter: ViewPresenter {
    /// The layer the renderer configures and draws into, outside any layer
    /// tree. The source textures take their shape from the `drawableSize` and
    /// `pixelFormat` the renderer sets on it.
    let rendererLayer: CAMetalLayer

    /// Source textures the ring holds. Two more than the on-screen layer
    /// keeps in flight, so a renderer drawing a frame ahead of the one being
    /// presented and a dropped drawable held back both have a texture to use.
    static let ringDepth = 5

    /// Guards every count and list below. Completion handlers and drawable
    /// deinits take it and never wait on anything while holding it.
    private let lock = NSLock()
    private var slots: [SourceSlot] = []
    private var free: [SourceSlot] = []
    /// Slots dropped without a present, each waiting for two on-screen
    /// presents submitted after the drop to complete.
    private var deferred: [SourceSlot] = []
    /// Counts free slots; its value is `free.count` less the tokens held by
    /// callers between their wait and their take. The wait happens outside
    /// the lock, so a caller blocked on a full ring stops nothing else.
    private let available = DispatchSemaphore(value: MetalViewPresenter.ringDepth)
    /// Bumped when the ring is rebuilt; a slot from an earlier ring goes
    /// away with its last holder instead of returning to the free list.
    private var ring = 0
    private var ringShape = (width: 0, height: 0, format: MTLPixelFormat.invalid)
    /// On-screen presents encoded so far; the sequence number of each.
    private var presentsSubmitted = 0
    private var nextDrawableID = 0
    private var fallbackQueue: MTLCommandQueue?

    init?(rendererLayer: CAMetalLayer) {
        self.rendererLayer = rendererLayer
        super.init(pixelFormat: rendererLayer.pixelFormat, contentsScale: rendererLayer.contentsScale)
    }

    /// The semaphore's value is the free list's length, and libdispatch traps
    /// on disposing of a semaphore below the value it was made with: the
    /// slots out of the free list at the end (deferred after a drop with no
    /// present, or the one whose last drawable is letting go of this
    /// presenter) are signaled back first.
    deinit {
        if slots.isEmpty { return }
        for _ in free.count..<MetalViewPresenter.ringDepth { available.signal() }
    }

    override func layoutChanged(rendererScale: Double, size: CGSize) {
        // The renderer's layer keeps the shape it had as the view's own
        // backing layer: bounds in the view's points at Wine's scale, so a
        // renderer sizing its swapchain from the layer gets Wine's answer.
        let frame = CGRect(origin: .zero, size: size)
        if rendererLayer.frame != frame { rendererLayer.frame = frame }
        if rendererLayer.contentsScale != rendererScale { rendererLayer.contentsScale = rendererScale }
    }

    override var sourceDescription: String { "source \(ringShape.width)x\(ringShape.height)" }

    // MARK: The renderer's side

    /// A drawable wrapping a free source texture, waited for when every
    /// texture is leased; `nil` after a second's wait with the renderer
    /// layer's timeout on, or when no texture could be allocated.
    func nextDrawable() -> SevoDrawable? {
        var size = rendererLayer.drawableSize
        if size.width < 1 || size.height < 1 {
            let bounds = rendererLayer.bounds.size
            size = CGSize(width: bounds.width * rendererLayer.contentsScale, height: bounds.height * rendererLayer.contentsScale)
        }
        let width = max(1, Int(size.width.rounded()))
        let height = max(1, Int(size.height.rounded()))
        var format = rendererLayer.pixelFormat
        if format == .invalid { format = .bgra8Unorm }
        // A new shape gets its ring before the wait: the rebuild is what
        // fills the free list when every slot of the old shape is out.
        lock.lock()
        if ringShape.width != width || ringShape.height != height || ringShape.format != format {
            guard rebuildRing(width: width, height: height, format: format) else {
                lock.unlock()
                return nil
            }
        }
        lock.unlock()
        if rendererLayer.allowsNextDrawableTimeout {
            guard available.wait(timeout: .now() + .seconds(1)) == .success else { return nil }
        } else {
            available.wait()
        }
        lock.lock()
        defer { lock.unlock() }
        // The semaphore's value is the free list's length, so a token
        // always finds a slot.
        let slot = free.removeLast()
        slot.uses = 1
        slot.presented = false
        nextDrawableID += 1
        return SevoDrawable(slot: slot, presenter: self, drawableID: nextDrawableID)
    }

    /// Replaces the ring with textures of the new shape. Leased and deferred
    /// slots of the old ring keep their textures until their last holder
    /// lets go; the free list starts full, and the semaphore is raised by
    /// the slots that were out, so its value follows the free list. The
    /// ring changes hands only once every texture exists: a failed
    /// allocation leaves the old ring current, its slots still recycling.
    /// Called with the lock held.
    private func rebuildRing(width: Int, height: Int, format: MTLPixelFormat) -> Bool {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .private
        let generation = ring + 1
        var fresh: [SourceSlot] = []
        for index in 0..<MetalViewPresenter.ringDepth {
            guard let texture = device.makeTexture(descriptor: descriptor) else {
                log("could not allocate a \(width)x\(height) source texture; frames are not taken")
                return false
            }
            texture.label = "sevo source \(index)"
            fresh.append(SourceSlot(texture: texture, ring: generation))
        }
        ring = generation
        let out = slots.count - free.count
        slots = fresh
        free = fresh
        deferred = []
        ringShape = (width, height, format)
        for _ in 0..<out { available.signal() }
        log("source \(width)x\(height) \(format.rawValue) x\(MetalViewPresenter.ringDepth)")
        return true
    }

    /// The proxy drawable let go of its slot.
    func drop(_ slot: SourceSlot) {
        lock.lock()
        slot.uses -= 1
        if slot.uses == 0 { settle(slot) }
        lock.unlock()
    }

    /// A slot no holder is left on. Called with the lock held.
    private func settle(_ slot: SourceSlot) {
        guard slot.ring == ring else { return }
        if slot.presented {
            free.append(slot)
            available.signal()
        } else {
            slot.droppedAfter = presentsSubmitted
            slot.presentsSince = 0
            deferred.append(slot)
        }
    }

    /// A command buffer that presented `slot` completed; `sequence` is the
    /// on-screen present it carried, 0 when the frame was dropped.
    private func presentCompleted(_ slot: SourceSlot, sequence: Int) {
        lock.lock()
        for held in deferred where sequence > held.droppedAfter { held.presentsSince += 1 }
        deferred.removeAll { held in
            guard held.presentsSince >= 2 else { return false }
            free.append(held)
            available.signal()
            return true
        }
        slot.uses -= 1
        if slot.uses == 0 { settle(slot) }
        lock.unlock()
    }

    // MARK: The present

    /// Encodes the scaler chain and the final pass into the renderer's own
    /// command buffer, then presents the real drawable through `presentReal`
    /// (the original `presentDrawable:` the hook replaced). Runs on the
    /// renderer's thread, before the buffer is committed. The command buffer
    /// holds the slot until it completes whether or not a real drawable was
    /// had: the renderer's work on the texture is in it either way.
    func present(_ proxy: SevoDrawable, in commandBuffer: MTLCommandBuffer, presentReal: (CAMetalDrawable) -> Void) {
        mirrorLayerProperties()
        let slot = proxy.slot
        lock.lock()
        slot.uses += 1
        slot.presented = true
        lock.unlock()
        var sequence = 0
        if let real = encodeFrame(source: slot.texture, in: commandBuffer) {
            proxy.forwardHandlers(to: real)
            presentReal(real)
            lock.lock()
            presentsSubmitted += 1
            sequence = presentsSubmitted
            lock.unlock()
        }
        commandBuffer.addCompletedHandler { [self] _ in presentCompleted(slot, sequence: sequence) }
    }

    /// A present called on the drawable itself rather than on a command
    /// buffer: the chain is encoded into a buffer of the presenter's own,
    /// which nothing orders after the renderer's work. This is the fallback
    /// for renderers the hook does not see.
    func presentDirectly(_ proxy: SevoDrawable, presentReal: (CAMetalDrawable) -> Void) {
        guard let queue = fallbackQueue ?? device.makeCommandQueue(), let commandBuffer = queue.makeCommandBuffer() else { return }
        fallbackQueue = queue
        present(proxy, in: commandBuffer, presentReal: presentReal)
        commandBuffer.commit()
    }

    /// What the renderer set on its layer that the on-screen one must match
    /// to show the same picture: the pixel format, the color space, EDR, and
    /// whether presents wait for the display.
    private func mirrorLayerProperties() {
        let format = rendererLayer.pixelFormat
        if format != .invalid, onscreen.pixelFormat != format { onscreen.pixelFormat = format }
        if onscreen.displaySyncEnabled != rendererLayer.displaySyncEnabled {
            onscreen.displaySyncEnabled = rendererLayer.displaySyncEnabled
        }
        if onscreen.wantsExtendedDynamicRangeContent != rendererLayer.wantsExtendedDynamicRangeContent {
            onscreen.wantsExtendedDynamicRangeContent = rendererLayer.wantsExtendedDynamicRangeContent
        }
        let colorspace = rendererLayer.colorspace
        if onscreen.colorspace != colorspace { onscreen.colorspace = colorspace }
    }
}

/// One source texture of a Metal view's ring and the state of its lease.
/// Every field but the texture and ring is the presenter's, under its lock.
final class SourceSlot {
    let texture: MTLTexture
    /// The ring this slot was made for.
    let ring: Int
    /// Holders: the proxy drawable while it lives, plus one per command
    /// buffer presenting it until that buffer completes.
    var uses = 0
    /// Whether a present hook has seen this lease; a lease dropped without
    /// one is held back instead of freed.
    var presented = false
    /// For a held-back slot: the present count when it was dropped, and the
    /// on-screen presents submitted after that which have completed.
    var droppedAfter = 0
    var presentsSince = 0

    init(texture: MTLTexture, ring: Int) {
        self.texture = texture
        self.ring = ring
    }
}

// MARK: - The drawable the renderer gets

/// What `nextDrawable` hands the renderer: a leased texture from the
/// presenter's ring wearing the drawable protocol. The renderer draws into
/// it and presents it; the presenter turns that present into one of the
/// on-screen layer's real drawables. Going away lets the lease's proxy
/// half go.
final class SevoDrawable: NSObject, CAMetalDrawable {
    let slot: SourceSlot
    let presenter: MetalViewPresenter
    let drawableID: Int
    private var handlers: [MTLDrawablePresentedHandler] = []
    private var real: CAMetalDrawable?

    init(slot: SourceSlot, presenter: MetalViewPresenter, drawableID: Int) {
        self.slot = slot
        self.presenter = presenter
        self.drawableID = drawableID
    }

    deinit {
        presenter.drop(slot)
    }

    var texture: MTLTexture { slot.texture }

    var layer: CAMetalLayer { presenter.rendererLayer }

    var presentedTime: CFTimeInterval { real?.presentedTime ?? 0 }

    func addPresentedHandler(_ block: @escaping MTLDrawablePresentedHandler) {
        if let real { real.addPresentedHandler(block) } else { handlers.append(block) }
    }

    func forwardHandlers(to real: CAMetalDrawable) {
        self.real = real
        for handler in handlers { real.addPresentedHandler(handler) }
        handlers.removeAll()
    }

    func present() {
        presenter.presentDirectly(self) { $0.present() }
    }

    func present(at presentationTime: CFTimeInterval) {
        presenter.presentDirectly(self) { $0.present(at: presentationTime) }
    }

    func present(afterMinimumDuration duration: CFTimeInterval) {
        presenter.presentDirectly(self) { $0.present(afterMinimumDuration: duration) }
    }
}
