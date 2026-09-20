/*
 * The presenter for a GDI window surface: the program's DIB, copied out at
 * each flush and drawn through the same chain as a rendered frame.
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

import CoreVideo
import Foundation
import Metal
import QuartzCore

/// A GDI surface's presenter. The DIB is read on the program's thread only,
/// inside `flush`, where win32u holds the surface lock that keeps the
/// program from drawing: the dirty rectangle is copied into a staging slot
/// there, and the GPU blits the slot into the frame texture on the
/// presenter's queue. The frame texture is the accumulated truth of what the
/// view shows (win32u rounds a surface up to 128 pixels, and the rest is
/// never drawn); a refresh presents it again, and a resize seeds the new
/// texture from the old one on the GPU. Frames go on screen from a display
/// link, one per refresh while something is dirty, so the program's thread
/// never waits for a drawable and a static screen costs nothing.
///
/// Three generations under the lock say where a frame is: `dirty` counts
/// changes, `submitted` the last one a present was committed for, and
/// `completed` the last one whose present finished. A tick encodes while
/// `dirty` is ahead of `submitted`; a present that fails rolls `submitted`
/// back so the next tick encodes again; the link idles out only once
/// `completed` has caught up.
final class SurfacePresenter: ViewPresenter {
    private let bits: UnsafeMutableRawPointer
    private let size: Int
    private let bytesPerRow: Int
    private let dibWidth: Int
    private let dibHeight: Int
    private let deallocator: (UnsafeMutableRawPointer, Int) -> Void
    private let queue: MTLCommandQueue

    private let lock = NSLock()
    private var frame: MTLTexture?
    /// The frame size the last layout asked for, in DIB pixels.
    private var wanted = (width: 0, height: 0)
    /// The next flush copies the whole frame, whatever it was told is dirty.
    private var needsFull = true
    private var dirty = 0
    private var submitted = 0
    private var completed = 0

    /// Staging slots at rest, each a shared buffer holding one dirty
    /// rectangle row-packed while its blit is in flight. The pool grows on
    /// demand to ``slotLimit``; a flush that finds none free is refused so
    /// the program's thread never waits for the GPU under the surface lock.
    private var freeSlots: [MTLBuffer] = []
    private var slotCount = 0
    static let slotLimit = 8

    private var link: CVDisplayLink?
    /// Whether the link is meant to run; ``applyLinkState()`` makes it so.
    private var linkRunning = false
    /// Set once the driver has let go; nothing starts the link after that.
    private var detached = false
    private var idleTicks = 0
    /// Refreshes with nothing to show before the display link stops.
    private let idleLimit = 120
    /// Starts and stops run here one after another, each applying the intent
    /// current when it runs, so a stop queued by an idle tick and the start
    /// from a flush right behind it cannot cross and leave the link stopped
    /// while it is meant to run.
    private let linkQueue = DispatchQueue(label: "sevo.surface.link", qos: .userInteractive)

    /// `deallocator` runs when the presenter goes away; every read of the
    /// DIB happens inside `flush`, so nothing reads it after that.
    init?(
        bits: UnsafeMutableRawPointer, size: Int, bytesPerRow: Int, width: Int, height: Int,
        deallocator: @escaping (UnsafeMutableRawPointer, Int) -> Void
    ) {
        guard let shared = Presenter.shared.device, let queue = shared.makeCommandQueue() else {
            log("surface \(width)x\(height): no command queue")
            return nil
        }
        queue.label = "sevo surface"
        self.bits = bits
        self.size = size
        self.bytesPerRow = bytesPerRow
        self.dibWidth = width
        self.dibHeight = height
        self.deallocator = deallocator
        self.queue = queue
        super.init(pixelFormat: .bgra8Unorm, contentsScale: 1)
        if Presenter.shared.tracing { log("surface \(width)x\(height) stride \(bytesPerRow) attached") }
    }

    /// A DIB holds sRGB pixels, whatever the display is.
    override func configure(_ layer: CAMetalLayer) {
        layer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
    }

    deinit {
        if let link { CVDisplayLinkStop(link) }
        deallocator(bits, size)
    }

    /// Stops the link, waiting for a tick in progress to return; nothing
    /// starts it again. The stop goes through `linkQueue`, behind any start
    /// already on its way there, so no start lands after it. Never let the
    /// last reference drop inside the link's own callback: `deinit`'s stop
    /// would wait for that callback forever.
    override func willDetach() {
        lock.lock()
        detached = true
        linkRunning = false
        lock.unlock()
        linkQueue.sync { applyLinkState() }
    }

    /// A layout happens on the main thread inside a Core Animation
    /// transaction, so the frame is presented again once that transaction has
    /// reached the window server. A present into a layer the server does not
    /// know yet, or at the old drawable size, does not stay on screen, and a
    /// program that has finished drawing does not flush again to fix it.
    override func layoutChanged(rendererScale: Double, size: CGSize) {
        let scale = rendererScale > 0 ? rendererScale : 1
        lock.lock()
        wanted = (
            width: min(dibWidth, max(0, Int((size.width * scale).rounded()))),
            height: min(dibHeight, max(0, Int((size.height * scale).rounded()))))
        lock.unlock()
        CATransaction.setCompletionBlock { [weak self] in self?.refresh() }
    }

    override var sourceDescription: String { "surface \(wanted.width)x\(wanted.height) of \(dibWidth)x\(dibHeight)" }

    // MARK: The program's side

    /// The program finished drawing this rectangle of the DIB. The dirty part
    /// (or the whole frame, after a layout) is copied into a staging slot
    /// here, under the caller's surface lock, blitted to the frame texture
    /// on the presenter's queue, and the display link is woken.
    ///
    /// - Returns: `false` when every slot is in flight or no frame texture
    ///   could be had; the rectangle stays dirty with the caller, which
    ///   includes it in its next flush. A `true` always means the pixels
    ///   were copied.
    func flush(left: Int, top: Int, right: Int, bottom: Int) -> Bool {
        lock.lock()
        guard let frame = currentFrame() else {
            needsFull = true
            lock.unlock()
            return false
        }
        var x = 0, y = 0, width = frame.width, height = frame.height
        if !needsFull {
            x = max(0, min(left, frame.width))
            y = max(0, min(top, frame.height))
            width = max(0, min(right, frame.width)) - x
            height = max(0, min(bottom, frame.height)) - y
        }
        guard width > 0, height > 0 else {
            lock.unlock()
            return true
        }
        guard copyFromDIB(into: frame, x: x, y: y, width: width, height: height) else {
            lock.unlock()
            return false
        }
        needsFull = false
        let start = wake()
        lock.unlock()
        if start { linkQueue.async { [weak self] in self?.applyLinkState() } }
        return true
    }

    /// Copies a rectangle of the DIB into the frame texture through a staging
    /// slot. `false` when every slot is in flight or Metal gave no command
    /// buffer. Called with the lock held.
    private func copyFromDIB(into frame: MTLTexture, x: Int, y: Int, width: Int, height: Int) -> Bool {
        let rowBytes = width * 4
        guard let slot = takeSlot(bytes: rowBytes * height) else { return false }
        let contents = slot.contents()
        for row in 0..<height {
            memcpy(contents + row * rowBytes, bits + (y + row) * bytesPerRow + x * 4, rowBytes)
        }
        guard let commandBuffer = queue.makeCommandBuffer(), let blit = commandBuffer.makeBlitCommandEncoder() else {
            freeSlots.append(slot)
            return false
        }
        blit.label = "sevo surface copy"
        blit.copy(
            from: slot, sourceOffset: 0, sourceBytesPerRow: rowBytes, sourceBytesPerImage: rowBytes * height,
            sourceSize: MTLSize(width: width, height: height, depth: 1),
            to: frame, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: x, y: y, z: 0))
        blit.endEncoding()
        commandBuffer.addCompletedHandler { [weak self] buffer in self?.copyCompleted(slot, error: buffer.error) }
        commandBuffer.commit()
        return true
    }

    /// A free slot of at least `bytes`, grown when the one at hand is too
    /// small, or `nil` when ``slotLimit`` slots are in flight. Called with
    /// the lock held.
    private func takeSlot(bytes: Int) -> MTLBuffer? {
        let reused = freeSlots.popLast()
        if let reused, reused.length >= bytes { return reused }
        guard reused != nil || slotCount < SurfacePresenter.slotLimit else { return nil }
        guard let slot = device.makeBuffer(length: bytes, options: .storageModeShared) else {
            if let reused { freeSlots.append(reused) }
            log("surface: no staging buffer of \(bytes) bytes")
            return nil
        }
        if reused == nil { slotCount += 1 }
        slot.label = "sevo surface staging"
        return slot
    }

    /// The blit out of `slot` finished; the slot is free again. A blit that
    /// failed left the frame short of that rectangle, so the next flush
    /// copies the whole frame.
    private func copyCompleted(_ slot: MTLBuffer, error: Error?) {
        lock.lock()
        freeSlots.append(slot)
        if let error {
            needsFull = true
            if Presenter.shared.tracing { log("surface copy: \(error)") }
        }
        lock.unlock()
    }

    /// The frame goes on screen again, after a layout. A texture the layout
    /// made new is filled from the DIB first.
    func refresh() {
        lock.lock()
        guard let frame = currentFrame() else {
            lock.unlock()
            return
        }
        // A texture made for this layout holds whatever the GPU last kept in
        // that memory beyond what the old frame seeded. The DIB has the
        // program's picture now, and a program at rest may not flush for
        // seconds, so the texture takes it here rather than at the next flush.
        if needsFull, copyFromDIB(into: frame, x: 0, y: 0, width: min(frame.width, dibWidth), height: min(frame.height, dibHeight)) {
            needsFull = false
        }
        let start = wake()
        lock.unlock()
        if start { linkQueue.async { [weak self] in self?.applyLinkState() } }
    }

    /// Marks the frame for the next refresh. Called with the lock held;
    /// returns whether the caller is to start the display link. The link
    /// starts only once a layout has given the view a size: the on-screen
    /// layer is made by that layout, on the main thread, and a tick must
    /// find it there.
    private func wake() -> Bool {
        dirty += 1
        idleTicks = 0
        let start = !linkRunning && !detached && wanted.width >= 1 && wanted.height >= 1
        if start { linkRunning = true }
        return start
    }

    /// The frame texture at the size the last layout asked for, made when
    /// that changes and seeded from the old one on the GPU. While no layout
    /// has given the view a size it is the whole DIB, so a flush ahead of
    /// the first layout is copied and the layout cuts the picture to the
    /// view; `nil` only when no texture could be allocated. Called with the
    /// lock held.
    private func currentFrame() -> MTLTexture? {
        var width = wanted.width, height = wanted.height
        if width < 1 || height < 1 { (width, height) = (dibWidth, dibHeight) }
        guard width >= 1, height >= 1 else { return nil }
        if let frame, frame.width == width, frame.height == height { return frame }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .private
        guard let texture = device.makeTexture(descriptor: descriptor) else {
            log("surface: could not allocate a \(width)x\(height) frame texture")
            return nil
        }
        texture.label = "sevo surface frame"
        if let frame { seed(texture, from: frame) }
        frame = texture
        needsFull = true
        if Presenter.shared.tracing { log("surface frame \(width)x\(height)") }
        return texture
    }

    /// Blits what the old frame texture and the new one have in common, so
    /// a refresh has the picture before the program's next flush.
    private func seed(_ texture: MTLTexture, from old: MTLTexture) {
        guard let commandBuffer = queue.makeCommandBuffer(), let blit = commandBuffer.makeBlitCommandEncoder() else { return }
        blit.label = "sevo surface seed"
        blit.copy(
            from: old, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
            sourceSize: MTLSize(width: min(old.width, texture.width), height: min(old.height, texture.height), depth: 1),
            to: texture, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        commandBuffer.commit()
    }

    // MARK: The display's side

    /// Brings the link to the state `linkRunning` asks for, making it on the
    /// first start. On `linkQueue`.
    private func applyLinkState() {
        lock.lock()
        if linkRunning, link == nil { link = makeLink() }
        let link = self.link
        let run = linkRunning
        lock.unlock()
        guard let link else { return }
        if run { CVDisplayLinkStart(link) } else { CVDisplayLinkStop(link) }
    }

    private func makeLink() -> CVDisplayLink? {
        var created: CVDisplayLink?
        guard CVDisplayLinkCreateWithActiveCGDisplays(&created) == kCVReturnSuccess, let created else {
            log("surface: no display link; frames are not presented")
            return nil
        }
        CVDisplayLinkSetOutputHandler(created) { [weak self] _, _, _, _, _ in
            self?.tick()
            return kCVReturnSuccess
        }
        return created
    }

    /// One refresh, the display link's callback: the frame goes on screen
    /// when a change is ahead of the last present. A refresh with nothing
    /// to do, or one the layer gave no drawable for, counts toward
    /// `idleLimit`, after which the link is stopped from `linkQueue`. Never
    /// stop it from this callback: the stop waits for the callback to return.
    func tick() {
        lock.lock()
        guard let frame, dirty != submitted else {
            if dirty == completed { idleTicks += 1 }
            let stop = idle()
            lock.unlock()
            if stop { linkQueue.async { [weak self] in self?.applyLinkState() } }
            return
        }
        let generation = dirty
        idleTicks = 0
        lock.unlock()
        guard onscreen.drawableSize.width >= 1, onscreen.drawableSize.height >= 1,
              let commandBuffer = queue.makeCommandBuffer(),
              let real = encodeFrame(source: frame, in: commandBuffer)
        else {
            lock.lock()
            idleTicks += 1
            let stop = idle()
            lock.unlock()
            if stop { linkQueue.async { [weak self] in self?.applyLinkState() } }
            return
        }
        commandBuffer.label = "sevo surface present"
        commandBuffer.present(real)
        if !presentedOnce {
            presentedOnce = true
            if Presenter.shared.tracing {
                log("surface first present: \(real.texture.width)x\(real.texture.height) into a \(Int(onscreen.bounds.width))x\(Int(onscreen.bounds.height))pt layer")
            }
        }
        commandBuffer.addCompletedHandler { [weak self] buffer in self?.presentCompleted(generation, error: buffer.error) }
        // Ahead of the commit, so the completion handler can only ever see
        // the generation it is about to settle.
        lock.lock()
        submitted = generation
        lock.unlock()
        commandBuffer.commit()
    }

    /// Whether the link has idled long enough to stop; marks it stopped.
    /// Called with the lock held.
    private func idle() -> Bool {
        let stop = idleTicks >= idleLimit && linkRunning
        if stop { linkRunning = false }
        return stop
    }

    /// The present of `generation` finished. A failure rolls `submitted`
    /// back to the last generation known to have completed, so the next
    /// tick encodes again; a success never moves `completed` backward past
    /// a newer one that finished first.
    func presentCompleted(_ generation: Int, error: Error?) {
        lock.lock()
        if let error {
            submitted = completed
            idleTicks = 0
            if Presenter.shared.tracing { log("surface present: \(error)") }
        } else {
            completed = max(completed, generation)
        }
        lock.unlock()
    }

    private var presentedOnce = false
}
