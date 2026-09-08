/*
 * The presenter's final pass: the full-screen draws that resample the
 * scaled frame into the on-screen drawable.
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

/// The last pass of every frame: full-screen triangles that sample the
/// scaled frame with the chosen filter and write the on-screen drawable.
/// Nearest and bilinear are one draw. Lanczos is two, horizontal into an
/// intermediate the caller owns and vertical into the drawable, with the
/// kernel weights read from tables built once per geometry; at 1:1 it is
/// the nearest draw, which produces the same bytes. Pipelines and tables
/// are kept for the life of the process. One instance serves every
/// presenter, so nothing here holds per-frame state.
final class FinalPass {
    private let device: MTLDevice
    private let library: MTLLibrary
    /// The full-screen triangle every pass of every scaler draws with.
    let vertexFunction: MTLFunction
    private let lock = NSLock()
    private var pipelines: [PipelineKey: MTLRenderPipelineState] = [:]
    private var tables: [TableKey: MTLBuffer] = [:]

    /// The format of the horizontal pass's intermediate: wide enough to
    /// carry the kernel's negative lobes into the vertical pass.
    static let intermediateFormat = MTLPixelFormat.rgba16Float
    /// Taps per axis at the widest kernel, a 4x reduction: `SevoTaps` in
    /// the shader has this many weights.
    static let maximumTaps = 24
    /// Tables the pass keeps before starting over; a buffer an encoded
    /// command buffer still references lives on until that buffer completes.
    private static let tableLimit = 32

    private enum Stage: String {
        case nearest = "sevo_final_nearest"
        case bilinear = "sevo_final_bilinear"
        case lanczosHorizontal = "sevo_final_lanczos_h"
        case lanczosVertical = "sevo_final_lanczos_v"
    }

    private struct PipelineKey: Hashable {
        let stage: Stage
        let format: MTLPixelFormat
    }

    /// One axis's geometry; the kernel's scale follows from the two sizes.
    private struct TableKey: Hashable {
        let source: Int
        let target: Int
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
    }

    /// Encodes the draws into `commandBuffer`. `pass` is the caller's
    /// descriptor, reused across frames so the hot path allocates nothing.
    /// `intermediate` supplies the Lanczos horizontal pass's target of the
    /// given width and height in ``intermediateFormat``; the caller owns it
    /// and keeps it untouched until the command buffer completes.
    func encode(
        source: MTLTexture, into target: MTLTexture, filter: FinalFilter,
        pass: MTLRenderPassDescriptor, intermediate: (_ width: Int, _ height: Int) -> MTLTexture?,
        in commandBuffer: MTLCommandBuffer
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
            guard let columns = table(source: source.width, target: target.width),
                  let rows = table(source: source.height, target: target.height),
                  let middle = intermediate(target.width, source.height)
            else {
                log("final pass: no tables or intermediate for \(source.width)x\(source.height) -> \(target.width)x\(target.height); frame skipped")
                return
            }
            draw(.lanczosHorizontal, from: source, into: middle, taps: columns, pass: pass, in: commandBuffer)
            draw(.lanczosVertical, from: middle, into: target, taps: rows, pass: pass, in: commandBuffer)
        }
    }

    private func draw(
        _ stage: Stage, from source: MTLTexture, into target: MTLTexture, taps: MTLBuffer? = nil,
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
        if let taps { encoder.setFragmentBuffer(taps, offset: 0, index: 0) }
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

    // MARK: Weight tables

    /// The Lanczos-3 taps of one axis, one `SevoTaps` entry per target
    /// pixel: the first source index, the tap count, and the normalized
    /// weights. The kernel widens by the source pixels per target pixel, at
    /// least one and at most four, so minification averages instead of
    /// aliasing; a tap window is capped at ``maximumTaps``.
    private func table(source: Int, target: Int) -> MTLBuffer? {
        let key = TableKey(source: source, target: target)
        lock.lock()
        defer { lock.unlock() }
        if let table = tables[key] { return table }
        let scale = min(4, max(1, Float(source) / Float(target)))
        let radius = 3 * scale
        let words = 2 + FinalPass.maximumTaps
        var entries = [UInt32](repeating: 0, count: target * words)
        var weights = [Float](repeating: 0, count: FinalPass.maximumTaps)
        for pixel in 0..<target {
            let position = (Float(pixel) + 0.5) / Float(target) * Float(source) - 0.5
            let first = Int32((position - radius).rounded(.up))
            let last = Int32((position + radius).rounded(.down))
            let count = min(Int(last - first + 1), FinalPass.maximumTaps)
            var sum: Float = 0
            for tap in 0..<count {
                weights[tap] = FinalPass.lanczos((Float(first) + Float(tap) - position) / scale)
                sum += weights[tap]
            }
            let base = pixel * words
            entries[base] = UInt32(bitPattern: first)
            entries[base + 1] = UInt32(count)
            for tap in 0..<count {
                entries[base + 2 + tap] = (sum > 0 ? weights[tap] / sum : 0).bitPattern
            }
        }
        guard let table = device.makeBuffer(bytes: entries, length: entries.count * 4, options: .storageModeShared) else {
            log("final pass: no buffer for a \(source) -> \(target) weight table")
            return nil
        }
        table.label = "sevo lanczos \(source) -> \(target)"
        if tables.count >= FinalPass.tableLimit { tables.removeAll() }
        tables[key] = table
        return table
    }

    private static func lanczos(_ x: Float) -> Float {
        let a: Float = 3
        let ax = abs(x)
        if ax < 1e-4 { return 1 }
        if ax >= a { return 0 }
        let px = Float.pi * x
        return a * sin(px) * sin(px / a) / (px * px)
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

    /* Lanczos-3 in two passes. One table entry per target pixel of the
       axis holds the first source index and the normalized weights; the
       taps are read directly, so every sample lands on a pixel center and
       nothing is softened by a half-texel offset. */
    struct SevoTaps {
        int first;
        int count;
        float weight[24];
    };

    fragment float4 sevo_final_lanczos_h(SevoVertex in [[stage_in]], texture2d<float> src [[texture(0)]],
                                         constant SevoTaps* taps [[buffer(0)]])
    {
        uint2 p = uint2(in.position.xy);
        constant SevoTaps& t = taps[p.x];
        int limit = int(src.get_width()) - 1;
        float4 color = float4(0.0);
        for (int i = 0; i < t.count; i++) {
            int x = clamp(t.first + i, 0, limit);
            color += src.read(uint2(x, p.y)) * t.weight[i];
        }
        return color;
    }

    fragment float4 sevo_final_lanczos_v(SevoVertex in [[stage_in]], texture2d<float> src [[texture(0)]],
                                         constant SevoTaps* taps [[buffer(0)]])
    {
        uint2 p = uint2(in.position.xy);
        constant SevoTaps& t = taps[p.y];
        int limit = int(src.get_height()) - 1;
        float4 color = float4(0.0);
        for (int i = 0; i < t.count; i++) {
            int y = clamp(t.first + i, 0, limit);
            color += src.read(uint2(p.x, y)) * t.weight[i];
        }
        return color;
    }
    """
}
