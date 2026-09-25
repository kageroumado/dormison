/*
 * The presenter for an OpenGL window drawable: the program's frames arrive
 * in IOSurfaces the driver's framebuffer objects draw into.
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
import IOSurface
import Metal
import QuartzCore

/// An OpenGL drawable's presenter. The driver owns the OpenGL side: at each
/// swap it takes a free surface of the ring (`acquire`), copies the back
/// buffer into it top row first, flushes, and hands it over (`present`). The
/// frame is encoded and presented there, on the program's thread, so a swap
/// waits for a drawable the way a swap with an interval waits for the
/// display. The surface presented last stays out of the ring until a newer
/// one replaces it: a refresh draws it again.
///
/// A surface OpenGL is about to draw into must be one Metal has finished
/// reading, so a surface returns to the ring only when every command buffer
/// that sampled it has completed.
final class GLViewPresenter: ViewPresenter {
    /// Surfaces in the ring: the one OpenGL draws into, the one on screen,
    /// and two that may still be in flight behind it.
    static let ringDepth = 4

    private final class Slot {
        let surface: IOSurfaceRef
        let texture: MTLTexture
        /// Frames that sample the texture: each is counted from the moment it
        /// chooses the slot, under `lock`, until its command buffer completes
        /// or it gives up before encoding.
        var readers = 0
        var acquired = false
        init(surface: IOSurfaceRef, texture: MTLTexture) {
            self.surface = surface
            self.texture = texture
        }
    }

    private let queue: MTLCommandQueue
    /// Guards the ring and `last`. Completion handlers take it and wait on
    /// nothing while holding it.
    private let lock = NSLock()
    private var slots: [Slot] = []
    private var last: Slot?
    private var shape = (width: 0, height: 0)
    /// One frame is encoded at a time: the program's thread at a swap, or
    /// `refreshQueue` drawing the last frame again.
    private let encoding = NSLock()
    private let refreshQueue = DispatchQueue(label: "sevo.gl.refresh", qos: .userInteractive)
    private var detached = false
    private var presentedOnce = false

    init?() {
        guard let shared = Presenter.shared.device, let queue = shared.makeCommandQueue() else {
            log("gl: no command queue")
            return nil
        }
        queue.label = "sevo gl"
        self.queue = queue
        super.init(pixelFormat: .bgra8Unorm, contentsScale: 1)
        if Presenter.shared.tracing { log("gl drawable attached") }
    }

    /// An OpenGL default framebuffer holds sRGB pixels, whatever the display is.
    override func configure(_ layer: CAMetalLayer) {
        layer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
    }

    override func willDetach() {
        lock.lock()
        detached = true
        lock.unlock()
        refreshQueue.sync {}
    }

    override func layoutChanged(rendererScale: Double, size: CGSize) {
        CATransaction.setCompletionBlock { [weak self] in self?.refresh() }
    }

    override func optionsChanged() { refresh() }

    override var sourceDescription: String { "gl \(shape.width)x\(shape.height)" }

    // MARK: The driver's side

    /// Replaces the ring with surfaces of `width` x `height` BGRA pixels.
    /// Surfaces of the old ring live on until the command buffers reading
    /// them complete; the driver lets go of its textures of them before it
    /// asks for the new ones.
    ///
    /// - Returns: whether every surface and its Metal texture exist.
    func resize(width: Int, height: Int) -> Bool {
        guard width >= 1, height >= 1 else { return false }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .bytesPerElement: 4,
            .pixelFormat: UInt32(0x4247_5241),  // 'BGRA'
        ]
        var fresh: [Slot] = []
        for index in 0..<GLViewPresenter.ringDepth {
            guard let surface = IOSurfaceCreate(properties as CFDictionary),
                  let texture = device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
            else {
                log("gl: could not allocate a \(width)x\(height) surface; frames are not taken")
                return false
            }
            texture.label = "sevo gl source \(index)"
            fresh.append(Slot(surface: surface, texture: texture))
        }
        lock.lock()
        slots = fresh
        shape = (width, height)
        lock.unlock()
        log("gl source \(width)x\(height) x\(GLViewPresenter.ringDepth)")
        return true
    }

    /// The surface of one ring slot, for the driver to wrap in a texture.
    func surface(at index: Int) -> IOSurfaceRef? {
        lock.lock()
        defer { lock.unlock() }
        return slots.indices.contains(index) ? slots[index].surface : nil
    }

    /// A slot nothing reads and nothing shows, for OpenGL to draw the next
    /// frame into; `nil` when every one is in flight, and the frame is dropped.
    func acquire() -> Int? {
        lock.lock()
        defer { lock.unlock() }
        guard let index = slots.firstIndex(where: { !$0.acquired && $0.readers == 0 && $0 !== last }) else { return nil }
        slots[index].acquired = true
        return index
    }

    /// OpenGL drew slot `index` and flushed: it goes on screen, waiting for
    /// the display when `synced`.
    func present(index: Int, synced: Bool) {
        lock.lock()
        guard slots.indices.contains(index) else {
            lock.unlock()
            return
        }
        let slot = slots[index]
        slot.acquired = false
        last = slot
        slot.readers += 1
        lock.unlock()
        // The main thread attaches the view after the drawable exists: a swap before that
        // has no layer to go to, and the refresh after the attach shows the slot kept above.
        guard let layer = madeOnscreen else {
            release(slot)
            return
        }
        if layer.displaySyncEnabled != synced { layer.displaySyncEnabled = synced }
        encode(slot)
    }

    /// The driver acquired a slot and could not draw it.
    func abandon(index: Int) {
        lock.lock()
        if slots.indices.contains(index) { slots[index].acquired = false }
        lock.unlock()
    }

    // MARK: The frame

    /// The last frame goes on screen again: after a layout, a change of
    /// options, or the system emptying the layer behind a covered window.
    override func refresh() {
        refreshQueue.async { [weak self] in
            guard let self else { return }
            lock.lock()
            let slot = detached ? nil : last
            slot?.readers += 1
            lock.unlock()
            if let slot { encode(slot) }
        }
    }

    /// Draws `slot`, whose reader count the caller raised under `lock` when it
    /// chose the slot. The count falls again when the command buffer completes,
    /// or here when there is nothing to encode into.
    private func encode(_ slot: Slot) {
        encoding.lock()
        defer { encoding.unlock() }
        guard let layer = madeOnscreen, layer.drawableSize.width >= 1, layer.drawableSize.height >= 1,
              let commandBuffer = queue.makeCommandBuffer()
        else {
            release(slot)
            return
        }
        commandBuffer.label = "sevo gl present"
        commandBuffer.addCompletedHandler { [self] _ in release(slot) }
        if let real = encodeFrame(source: slot.texture, in: commandBuffer) {
            commandBuffer.present(real)
            if !presentedOnce {
                presentedOnce = true
                if Presenter.shared.tracing {
                    log("gl first present: \(slot.texture.width)x\(slot.texture.height) into \(real.texture.width)x\(real.texture.height)")
                }
            }
        }
        commandBuffer.commit()
    }

    /// One frame that chose `slot` has finished with it.
    private func release(_ slot: Slot) {
        lock.lock()
        slot.readers -= 1
        lock.unlock()
    }
}
