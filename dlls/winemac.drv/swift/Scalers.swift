/*
 * The presenter's scalers: what runs between the game's frame and the final
 * pass.
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
import MetalFX

/// A scaler writes a texture it owns; the final pass draws that texture into
/// the drawable. It encodes into the renderer's command buffer and never
/// commits, waits, or opens a queue of its own.
protocol Scaler: AnyObject {
    /// The scaled frame, or `nil` when the source is to be drawn as it is.
    func encode(source: MTLTexture, target: MTLSize, in commandBuffer: MTLCommandBuffer) -> MTLTexture?
}

/// MetalFX Spatial: one scaler built for one source and output shape. A
/// resize drag changes the target every frame, so a new target is honored
/// once it has held still for `settleFrames` frames; before that the final
/// pass resamples the output built for the old target. A source change (the
/// game picked another resolution) rebuilds at once.
final class SpatialScaler: Scaler {
    private let device: MTLDevice
    private var scaler: MTLFXSpatialScaler?
    private var output: MTLTexture?
    private var built: Shape?
    private var pending: Shape?
    private var pendingFrames = 0
    private let settleFrames = 12

    private struct Shape: Equatable {
        let width: Int
        let height: Int
        let format: MTLPixelFormat
        let outputWidth: Int
        let outputHeight: Int
    }

    init?(device: MTLDevice) {
        guard MTLFXSpatialScalerDescriptor.supportsDevice(device) else { return nil }
        self.device = device
    }

    func encode(source: MTLTexture, target: MTLSize, in commandBuffer: MTLCommandBuffer) -> MTLTexture? {
        // A window no larger than the frame has nothing to upscale.
        guard target.width > source.width || target.height > source.height else { return nil }
        let wanted = Shape(
            width: source.width, height: source.height, format: source.pixelFormat,
            outputWidth: max(target.width, source.width), outputHeight: max(target.height, source.height))
        if wanted != built {
            let sourceChanged = built.map { $0.width != wanted.width || $0.height != wanted.height || $0.format != wanted.format } ?? true
            if wanted == pending { pendingFrames += 1 } else { pending = wanted; pendingFrames = 0 }
            if sourceChanged || pendingFrames >= settleFrames { rebuild(wanted) }
        }
        guard let scaler, let output else { return nil }
        scaler.colorTexture = source
        scaler.outputTexture = output
        scaler.inputContentWidth = source.width
        scaler.inputContentHeight = source.height
        scaler.encode(commandBuffer: commandBuffer)
        return output
    }

    private func rebuild(_ shape: Shape) {
        let descriptor = MTLFXSpatialScalerDescriptor()
        descriptor.inputWidth = shape.width
        descriptor.inputHeight = shape.height
        descriptor.outputWidth = shape.outputWidth
        descriptor.outputHeight = shape.outputHeight
        descriptor.colorTextureFormat = shape.format
        descriptor.outputTextureFormat = shape.format
        descriptor.colorProcessingMode = SpatialScaler.isFloat(shape.format) ? .linear : .perceptual
        guard let scaler = descriptor.makeSpatialScaler(device: device) else {
            log("MetalFX Spatial refused \(shape.width)x\(shape.height) -> \(shape.outputWidth)x\(shape.outputHeight); resampling only")
            self.scaler = nil
            output = nil
            built = shape
            return
        }
        let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: shape.format, width: shape.outputWidth, height: shape.outputHeight, mipmapped: false)
        textureDescriptor.usage = [.shaderRead, .renderTarget]
        textureDescriptor.storageMode = .private
        guard let output = device.makeTexture(descriptor: textureDescriptor) else {
            log("could not allocate the MetalFX output; resampling only")
            self.scaler = nil
            self.output = nil
            built = shape
            return
        }
        output.label = "sevo metalfx output"
        self.scaler = scaler
        self.output = output
        built = shape
        log("MetalFX Spatial \(shape.width)x\(shape.height) -> \(shape.outputWidth)x\(shape.outputHeight)")
    }

    private static func isFloat(_ format: MTLPixelFormat) -> Bool {
        switch format {
        case .rgba16Float, .rgba32Float, .rg11b10Float, .rgb9e5Float, .r16Float, .r32Float, .rg16Float, .rg32Float:
            true
        default:
            false
        }
    }
}
