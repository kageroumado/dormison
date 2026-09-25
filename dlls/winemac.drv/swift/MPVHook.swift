/*
 * The presenter's shader package runner: mpv user-shader pass graphs
 * compiled ahead of time into a metallib.
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

/// Runs a shader package: a directory holding `shaders.metallib`, one
/// function per pass, and `graph.json`, `{"format": 2, "name": "...", "passes":
/// [...]}` with the passes in run order. Each pass carries its mpv hook
/// directives as fields: `function` (its name in the metallib), `desc`,
/// `hook` (the texture names it hooks), `textures` (what it binds, in binding
/// order), `save`, `width`, `height` and `when` (size expressions as token
/// arrays, see ``evaluate(_:sizes:)``), `components`, and for a compute pass
/// `compute` (`[bw, bh, tw, th]`). A fragment pass draws a full-screen
/// triangle into a texture of its own size; a compute pass writes that texture
/// as `out_image`, one threadgroup per block of output pixels. Both sample the
/// textures they bind (`MAIN` is the frame, or the last pass that saved as
/// `MAIN`) with the sizes handed over in a uniform buffer, as mpv runs them.
/// The scaled frame is whatever `MAIN` names at the end.
final class MPVHookScaler: Scaler {
    private struct Graph: Decodable {
        let format: Int
        let name: String
        let passes: [PassSpec]
    }

    private struct PassSpec: Decodable {
        let function: String
        let desc: String
        let hook: [String]
        /// The bound textures in binding order: texture and sampler index.
        let textures: [String]
        let save: String
        let width: [String]?
        let height: [String]?
        let when: [String]?
        let components: Int
        /// Block width and height in output pixels, then threads per group
        /// on each axis; absent for a fragment pass.
        let compute: [Int]?
    }

    private enum Pipeline {
        case render(MTLRenderPipelineState)
        case compute(MTLComputePipelineState, block: MTLSize, threads: MTLSize)
    }

    private struct Pass {
        let spec: PassSpec
        let pipeline: Pipeline
    }

    /// The graph format this runner reads. In format 2 a pass's Metal
    /// indices are its GLSL bindings: the uniforms at buffer 0, the k-th
    /// bound texture and its sampler at k+1, a compute pass's `out_image` at
    /// the index after the last bound texture.
    static let format = 2

    private let device: MTLDevice
    private let name: String
    private let passes: [Pass]
    private let sampler: MTLSamplerState
    private let renderPass = MTLRenderPassDescriptor()
    private var outputs: [MTLTexture?]
    private var uniforms: [SIMD2<Float>] = []
    private var lastShapeLog = ""
    private var loggedTooLarge = false

    static let intermediateFormat = MTLPixelFormat.rgba16Float
    /// The largest 2D texture every Apple GPU takes.
    static let maxTextureSize = 16384

    /// Finds `<name>` under the colon-separated search path, or under
    /// `~/Library/Application Support/Sevoflurane/Shaders` when it is empty.
    static func locate(_ name: String, in searchPath: String) -> URL? {
        var directories = searchPath.split(separator: ":").map(String.init)
        if directories.isEmpty {
            directories = [NSHomeDirectory() + "/Library/Application Support/Sevoflurane/Shaders"]
        }
        for directory in directories {
            let url = URL(fileURLWithPath: directory).appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: url.appendingPathComponent("graph.json").path) {
                return url
            }
        }
        return nil
    }

    init?(device: MTLDevice, directory: URL, vertexFunction: MTLFunction) {
        self.device = device
        let graph: Graph
        let library: MTLLibrary
        do {
            graph = try JSONDecoder().decode(Graph.self, from: Data(contentsOf: directory.appendingPathComponent("graph.json")))
            library = try device.makeLibrary(URL: directory.appendingPathComponent("shaders.metallib"))
        } catch {
            log("package \(directory.lastPathComponent): \(error)")
            return nil
        }
        guard graph.format == MPVHookScaler.format else {
            log("package \(graph.name) is format \(graph.format); this engine reads format \(MPVHookScaler.format)")
            return nil
        }
        var passes: [Pass] = []
        for spec in graph.passes {
            guard let function = library.makeFunction(name: spec.function) else {
                log("package \(graph.name): \(spec.function) is not in shaders.metallib")
                return nil
            }
            do {
                if let sizes = spec.compute {
                    guard sizes.count == 4, sizes.allSatisfy({ $0 >= 1 }) else {
                        log("package \(graph.name) pass \(spec.function): compute sizes \(sizes) are not 'bw bh tw th'")
                        return nil
                    }
                    let state = try device.makeComputePipelineState(function: function)
                    let threads = MTLSize(width: sizes[2], height: sizes[3], depth: 1)
                    let total = threads.width.multipliedReportingOverflow(by: threads.height)
                    guard !total.overflow, total.partialValue <= state.maxTotalThreadsPerThreadgroup else {
                        log("package \(graph.name) pass \(spec.function): \(threads.width)x\(threads.height) threads exceed \(state.maxTotalThreadsPerThreadgroup)")
                        return nil
                    }
                    let block = MTLSize(width: sizes[0], height: sizes[1], depth: 1)
                    passes.append(Pass(spec: spec, pipeline: .compute(state, block: block, threads: threads)))
                } else {
                    let descriptor = MTLRenderPipelineDescriptor()
                    descriptor.label = "\(graph.name) \(spec.function)"
                    descriptor.vertexFunction = vertexFunction
                    descriptor.fragmentFunction = function
                    descriptor.colorAttachments[0].pixelFormat = MPVHookScaler.intermediateFormat
                    passes.append(Pass(spec: spec, pipeline: .render(try device.makeRenderPipelineState(descriptor: descriptor))))
                }
            } catch {
                log("package \(graph.name) pass \(spec.function): \(error)")
                return nil
            }
        }
        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        samplerDescriptor.sAddressMode = .clampToEdge
        samplerDescriptor.tAddressMode = .clampToEdge
        samplerDescriptor.normalizedCoordinates = true
        guard let sampler = device.makeSamplerState(descriptor: samplerDescriptor) else { return nil }
        self.sampler = sampler
        self.name = graph.name
        self.passes = passes
        self.outputs = Array(repeating: nil, count: passes.count)
        log("package \(graph.name): \(passes.count) passes from \(directory.path)")
    }

    func encode(source: MTLTexture, target: MTLSize, in commandBuffer: MTLCommandBuffer) -> MTLTexture? {
        var named: [String: MTLTexture] = ["MAIN": source, "NATIVE": source]
        var sizes: [String: SIMD2<Float>] = [
            "MAIN": SIMD2(Float(source.width), Float(source.height)),
            "NATIVE": SIMD2(Float(source.width), Float(source.height)),
            "OUTPUT": SIMD2(Float(target.width), Float(target.height)),
        ]
        var ran = 0
        for (index, pass) in passes.enumerated() {
            let spec = pass.spec
            guard let hooked = spec.hook.first, named[hooked] != nil else { continue }
            sizes["HOOKED"] = sizes[hooked]
            if let when = spec.when, evaluate(when, sizes: sizes) == 0 { continue }
            let hookedSize = sizes[hooked]!
            let width = spec.width.map { evaluate($0, sizes: sizes) } ?? hookedSize.x
            let height = spec.height.map { evaluate($0, sizes: sizes) } ?? hookedSize.y
            let outputWidth = Self.textureSide(width, otherwise: hookedSize.x)
            let outputHeight = Self.textureSide(height, otherwise: hookedSize.y)
            guard let output = output(at: index, width: outputWidth, height: outputHeight) else { return nil }

            uniforms.removeAll(keepingCapacity: true)
            uniforms.append(SIMD2(Float(outputWidth), Float(outputHeight)))
            var bound: [MTLTexture] = []
            for textureName in spec.textures {
                guard let texture = named[textureName], let size = sizes[textureName] else {
                    log("package \(name) pass \(spec.function) binds \(textureName), which nothing produced this frame")
                    return nil
                }
                bound.append(texture)
                uniforms.append(size)
                uniforms.append(SIMD2(1 / size.x, 1 / size.y))
            }

            switch pass.pipeline {
            case let .render(pipeline):
                let attachment = renderPass.colorAttachments[0]!
                attachment.texture = output
                attachment.loadAction = .dontCare
                attachment.storeAction = .store
                guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: renderPass) else { return nil }
                encoder.label = "\(name) \(spec.function)"
                encoder.setRenderPipelineState(pipeline)
                for (slot, texture) in bound.enumerated() {
                    encoder.setFragmentTexture(texture, index: slot + 1)
                    encoder.setFragmentSamplerState(sampler, index: slot + 1)
                }
                uniforms.withUnsafeBytes { bytes in
                    encoder.setFragmentBytes(bytes.baseAddress!, length: bytes.count, index: 0)
                }
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                encoder.endEncoding()
                attachment.texture = nil
            case let .compute(pipeline, block, threads):
                guard let encoder = commandBuffer.makeComputeCommandEncoder() else { return nil }
                encoder.label = "\(name) \(spec.function)"
                encoder.setComputePipelineState(pipeline)
                for (slot, texture) in bound.enumerated() {
                    encoder.setTexture(texture, index: slot + 1)
                    encoder.setSamplerState(sampler, index: slot + 1)
                }
                encoder.setTexture(output, index: bound.count + 1)
                uniforms.withUnsafeBytes { bytes in
                    encoder.setBytes(bytes.baseAddress!, length: bytes.count, index: 0)
                }
                let groups = MTLSize(
                    width: Self.blocks(covering: outputWidth, of: block.width),
                    height: Self.blocks(covering: outputHeight, of: block.height), depth: 1)
                encoder.dispatchThreadgroups(groups, threadsPerThreadgroup: threads)
                encoder.endEncoding()
            }

            named[spec.save] = output
            sizes[spec.save] = SIMD2(Float(outputWidth), Float(outputHeight))
            ran += 1
        }
        if Presenter.shared.tracing {
            let shape = "\(source.width)x\(source.height) -> \(named["MAIN"]!.width)x\(named["MAIN"]!.height) (\(ran) passes)"
            if shape != lastShapeLog {
                lastShapeLog = shape
                log("package \(name): \(shape)")
            }
        }
        let result = named["MAIN"]!
        return result === source ? nil : result
    }

    private func output(at index: Int, width: Int, height: Int) -> MTLTexture? {
        if let texture = outputs[index], texture.width == width, texture.height == height { return texture }
        // A package lays feature maps side by side, so a pass can ask for a
        // texture many frames wide, and Metal traps on a texture past its
        // limit rather than refusing it.
        guard width <= MPVHookScaler.maxTextureSize, height <= MPVHookScaler.maxTextureSize else {
            if !loggedTooLarge {
                loggedTooLarge = true
                log("package \(name): pass \(index) wants \(width)x\(height), past Metal's \(MPVHookScaler.maxTextureSize); resampling only")
            }
            return nil
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MPVHookScaler.intermediateFormat, width: width, height: height, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead, .shaderWrite]
        descriptor.storageMode = .private
        guard let texture = device.makeTexture(descriptor: descriptor) else {
            log("package \(name): could not allocate a \(width)x\(height) pass texture")
            return nil
        }
        texture.label = "\(name) pass \(index)"
        outputs[index] = texture
        return texture
    }

    /// The largest texture side Metal makes on every Apple GPU.
    private static let largestTextureSide: Float = 16384

    /// A pass's output side from its size expression. A package's expression can come out
    /// infinite, not a number or past what Metal makes, and converting any of those to an
    /// integer traps: those take the hooked texture's side instead.
    private static func textureSide(_ value: Float, otherwise fallback: Float) -> Int {
        let side = value.isFinite && value >= 1 && value <= largestTextureSide ? value : fallback
        return Int(min(largestTextureSide, max(1, side.isFinite ? side : 1)).rounded())
    }

    /// Blocks of `block` pixels that cover `side` pixels. Divides before it adds, so a
    /// package's block size as large as `Int.max` still answers 1.
    private static func blocks(covering side: Int, of block: Int) -> Int {
        side / block + (side % block == 0 ? 0 : 1)
    }

    /// mpv's size expressions: reverse Polish over numbers and `NAME.w` /
    /// `NAME.h`, with `+ - * /` and the comparisons `> < =` answering 1 or
    /// 0, `!` negating, and `*` doubling as "and" over those answers.
    private func evaluate(_ tokens: [String], sizes: [String: SIMD2<Float>]) -> Float {
        var stack: [Float] = []
        func pop() -> Float { stack.popLast() ?? 0 }
        for token in tokens {
            switch token {
            case "+": let b = pop(), a = pop(); stack.append(a + b)
            case "-": let b = pop(), a = pop(); stack.append(a - b)
            case "*": let b = pop(), a = pop(); stack.append(a * b)
            case "/": let b = pop(), a = pop(); stack.append(b == 0 ? 0 : a / b)
            case ">": let b = pop(), a = pop(); stack.append(a > b ? 1 : 0)
            case "<": let b = pop(), a = pop(); stack.append(a < b ? 1 : 0)
            case "=": let b = pop(), a = pop(); stack.append(a == b ? 1 : 0)
            case "!": let a = pop(); stack.append(a == 0 ? 1 : 0)
            default:
                if let number = Float(token) {
                    stack.append(number)
                } else if token.hasSuffix(".w"), let size = sizes[String(token.dropLast(2))] {
                    stack.append(size.x)
                } else if token.hasSuffix(".h"), let size = sizes[String(token.dropLast(2))] {
                    stack.append(size.y)
                } else {
                    stack.append(0)
                }
            }
        }
        return stack.last ?? 0
    }
}
