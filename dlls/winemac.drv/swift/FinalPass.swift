/*
 * The presenter's final pass: the resample of the scaled frame into the
 * on-screen drawable.
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
import MetalPerformanceShaders

/// The last pass of every frame: the scaled frame resampled with the chosen
/// filter into the on-screen drawable. Nearest and bilinear are one
/// full-screen draw each. Lanczos is Apple's `MPSImageLanczosScale`, a
/// compute kernel that writes the drawable directly, which is why the
/// presenter's layer is not framebuffer-only; at 1:1 it is the nearest
/// draw, which produces the same bytes. Pipelines are kept for the life of
/// the process. One instance serves every presenter, so nothing here holds
/// per-frame state.
final class FinalPass {
    private let device: MTLDevice
    private let library: MTLLibrary
    /// The full-screen triangle every pass of every scaler draws with.
    let vertexFunction: MTLFunction
    private let lock = NSLock()
    private var pipelines: [PipelineKey: MTLRenderPipelineState] = [:]
    /// Lanczos-3 as MetalPerformanceShaders encodes it. Measured against the
    /// driver's former two-pass shader on real frames at ×2 to 5K: the same
    /// picture to one level inside the frame, at about 0.6 of the GPU time
    /// (`bispectral/presenter/correctness/compare`, 2026-11-01). Encoded
    /// under ``lock``: presenters encode from their own threads and one
    /// kernel object serves them all.
    private let lanczos: MPSImageLanczosScale
    private var warnedUnwritable = false

    private enum Stage: String {
        case nearest = "sevo_final_nearest"
        case bilinear = "sevo_final_bilinear"
    }

    private struct PipelineKey: Hashable {
        let stage: Stage
        let format: MTLPixelFormat
    }

    init?(device: MTLDevice) {
        self.device = device
        do {
            library = try device.makeLibrary(source: FinalPass.source, options: nil)
        } catch {
            log("final pass: \(error)")
            return nil
        }
        guard let vertexFunction = library.makeFunction(name: "sevo_fullscreen") else { return nil }
        self.vertexFunction = vertexFunction
        lanczos = MPSImageLanczosScale(device: device)
        // The kernel's taps past the edge read the edge pixel, as the draws'
        // samplers do; its default reads zero and darkens the outer rows.
        lanczos.edgeMode = .clamp
    }

    /// Encodes the pass into `commandBuffer`. `pass` is the caller's
    /// descriptor, reused across frames so the hot path allocates nothing.
    func encode(
        source: MTLTexture, into target: MTLTexture, filter: FinalFilter,
        pass: MTLRenderPassDescriptor, in commandBuffer: MTLCommandBuffer
    ) {
        if Presenter.shared.debugClear {
            // PresenterDebug=clear: the drawable is cleared to red and nothing
            // is drawn. A window that turns red has a black source; one that
            // stays black has a layer that is not on screen.
            let attachment = pass.colorAttachments[0]!
            attachment.texture = target
            attachment.loadAction = .clear
            attachment.storeAction = .store
            attachment.clearColor = MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)
            commandBuffer.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
            attachment.texture = nil
            return
        }
        let identity = source.width == target.width && source.height == target.height
        switch filter {
        case .nearest:
            draw(.nearest, from: source, into: target, pass: pass, in: commandBuffer)
        case .bilinear:
            draw(.bilinear, from: source, into: target, pass: pass, in: commandBuffer)
        case .lanczos where identity:
            draw(.nearest, from: source, into: target, pass: pass, in: commandBuffer)
        case .lanczos:
            resample(from: source, into: target, pass: pass, in: commandBuffer)
        }
    }

    /// The Lanczos kernel into `target`, which a compute kernel can write
    /// only when the texture allows it; a target that does not (a
    /// framebuffer-only drawable, a render-only texture) gets the bilinear
    /// draw and one line in the log, never a skipped frame.
    private func resample(
        from source: MTLTexture, into target: MTLTexture,
        pass: MTLRenderPassDescriptor, in commandBuffer: MTLCommandBuffer
    ) {
        guard target.usage.contains(.shaderWrite), !target.isFramebufferOnly else {
            lock.lock()
            let warn = !warnedUnwritable
            warnedUnwritable = true
            lock.unlock()
            if warn {
                log("final pass: \(target.label ?? "the target") takes no compute writes (usage \(target.usage.rawValue), framebuffer-only \(target.isFramebufferOnly)); Lanczos draws bilinear")
            }
            draw(.bilinear, from: source, into: target, pass: pass, in: commandBuffer)
            return
        }
        lock.lock()
        lanczos.encode(commandBuffer: commandBuffer, sourceTexture: source, destinationTexture: target)
        lock.unlock()
    }

    private func draw(
        _ stage: Stage, from source: MTLTexture, into target: MTLTexture,
        pass: MTLRenderPassDescriptor, in commandBuffer: MTLCommandBuffer
    ) {
        guard let pipeline = pipeline(for: stage, format: target.pixelFormat) else { return }
        let attachment = pass.colorAttachments[0]!
        attachment.texture = target
        attachment.loadAction = .dontCare
        attachment.storeAction = .store
        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else {
            attachment.texture = nil
            return
        }
        encoder.label = "sevo final \(stage)"
        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(source, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        attachment.texture = nil
    }

    private func pipeline(for stage: Stage, format: MTLPixelFormat) -> MTLRenderPipelineState? {
        let key = PipelineKey(stage: stage, format: format)
        lock.lock()
        defer { lock.unlock() }
        if let pipeline = pipelines[key] { return pipeline }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.label = "sevo final \(stage)"
        descriptor.vertexFunction = vertexFunction
        descriptor.fragmentFunction = library.makeFunction(name: stage.rawValue)
        descriptor.colorAttachments[0].pixelFormat = format
        do {
            let pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
            pipelines[key] = pipeline
            return pipeline
        } catch {
            log("final pass pipeline \(stage) for \(format.rawValue): \(error)")
            return nil
        }
    }

    /// Compiled once per process. Texture coordinates put the source's first
    /// row at the top of the drawable, the way every renderer lays it out.
    static let source = """
    #include <metal_stdlib>
    using namespace metal;

    struct SevoVertex {
        float4 position [[position]];
        float2 uv;
    };

    vertex SevoVertex sevo_fullscreen(uint id [[vertex_id]])
    {
        float2 p = float2((id << 1) & 2, id & 2);
        SevoVertex out;
        out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
        out.uv = float2(p.x, 1.0 - p.y);
        return out;
    }

    fragment float4 sevo_final_nearest(SevoVertex in [[stage_in]], texture2d<float> src [[texture(0)]])
    {
        constexpr sampler s(coord::normalized, filter::nearest, address::clamp_to_edge);
        return src.sample(s, in.uv);
    }

    fragment float4 sevo_final_bilinear(SevoVertex in [[stage_in]], texture2d<float> src [[texture(0)]])
    {
        constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
        return src.sample(s, in.uv);
    }
    """
}
