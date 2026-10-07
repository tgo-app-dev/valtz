// The Valtz mark, rendered from geometry with Metal.
//
// The geometry is MarkGeometry: the painted mark's ribbon layers, traced
// from the artwork and triangulated, drawn back to front with colour
// baked per vertex; the scene shader adds the luminous rims and cuts the
// film strip's sprocket holes, and a bloom pass makes it glow
// (MarkShaders.swift). Because it is geometry, the mark is sharp at
// every size, simplifies itself when small (no sprocket holes below
// ~64 px), and can move: while a job loads, a slow wave travels through
// the ribbon (`motion`, `phase`, applied in the vertex shader). The app
// draws it (MarkView); the build renders the app icon with the same code
// (tools/valtz-icon).

import CoreGraphics
import Foundation
import Metal
import simd

@MainActor
final class MarkRenderer {
    enum Style {
        case mark      // the mark alone, framed as the painted artwork was
        case markFit   // the mark alone, framed by its own extent (any aspect)
        case appIcon   // on the charcoal icon body, macOS icon grid
    }

    /// What the ribbon is made of. Metal: opaque, coloured reflections.
    /// Glass: translucent -- denser and more saturated towards its edges
    /// and at grazing angles, clearer in the middle, with untinted
    /// mirror highlights and a Fresnel rim -- so whatever is behind the
    /// mark shows through it.
    enum Material { case metal, glass }

    /// 0 = still; 1 = the full waltz. `phase` advances with time.
    var motion: Float = 0
    var phase: Float = 0
    var material: Material = .metal
    /// The light sweep's progress, 0 -> 1 (negative = no sweep): a bright
    /// light passing from the right of the window to the left, fading in
    /// and out, that the glass reflects and refracts.
    var sweep: Float = -1
    /// The mark as a flat white silhouette -- no shading, no glow: what
    /// reads at 16 x 16, where the ribbon's detail would be mush.
    var flat = false
    /// The mark sits on a light ground (the light appearance): its glass
    /// is then denser and more saturated, so white behind it does not
    /// wash it out.
    var lightGround = false
    /// A soft drop shadow under the mark (`.mark` / `.markFit`): its own
    /// silhouette, blurred and offset down. Drawn here rather than by the
    /// UI, so the live view (a Metal layer) and the still look the same.
    /// With a shadow, `.markFit` leaves it room around the mark.
    var shadow = false

    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let scenePipeline: MTLRenderPipelineState
    private let glassPipeline: MTLRenderPipelineState
    private let downPipeline: MTLRenderPipelineState
    private let blurPipeline: MTLRenderPipelineState
    private let compositePipeline: MTLRenderPipelineState
    private let outputFormat: MTLPixelFormat
    private var targets: Targets?
    private let vertices: MTLBuffer
    private let indices: MTLBuffer
    private let indexCount: Int
    private let plateIndexCount: Int
    private let holes: [SIMD4<Float>]
    private let holeShapes: [SIMD4<Float>]

    static let samples = 4

    init?(device: MTLDevice? = MTLCreateSystemDefaultDevice(),
          outputFormat: MTLPixelFormat = .bgra8Unorm) {
        guard let device, let queue = device.makeCommandQueue() else {
            return nil
        }
        self.device = device
        self.queue = queue
        self.outputFormat = outputFormat
        do {
            let lib = try device.makeLibrary(source: MarkShaders.source,
                                             options: nil)
            func fn(_ name: String) -> MTLFunction? { lib.makeFunction(name: name) }

            let vd = MTLVertexDescriptor()
            for i in 0..<6 {
                vd.attributes[i].format = .float4
                vd.attributes[i].offset = 16 * i
                vd.attributes[i].bufferIndex = 0
            }
            vd.layouts[0].stride = MemoryLayout<MarkGeometry.Vertex>.stride

            let scene = MTLRenderPipelineDescriptor()
            scene.vertexFunction = fn("scene_vertex")
            scene.fragmentFunction = fn("scene_fragment")
            scene.vertexDescriptor = vd
            scene.colorAttachments[0].pixelFormat = .rgba16Float
            scene.colorAttachments[1].pixelFormat = .rgba16Float
            scene.rasterSampleCount = Self.samples
            scene.isAlphaToCoverageEnabled = true
            scenePipeline = try device.makeRenderPipelineState(descriptor: scene)
            // Glass writes premultiplied colour with its own opacity. Still
            // painter's order with no blending: only the TOP sheet is kept,
            // so the construction under it never shows through.
            scene.isAlphaToCoverageEnabled = false
            glassPipeline = try device.makeRenderPipelineState(descriptor: scene)

            func quad(_ frag: String, _ format: MTLPixelFormat) throws
                -> MTLRenderPipelineState {
                let d = MTLRenderPipelineDescriptor()
                d.vertexFunction = fn("quad_vertex")
                d.fragmentFunction = fn(frag)
                d.colorAttachments[0].pixelFormat = format
                return try device.makeRenderPipelineState(descriptor: d)
            }
            downPipeline = try quad("down_fragment", .rgba16Float)
            blurPipeline = try quad("blur_fragment", .rgba16Float)
            compositePipeline = try quad("composite_fragment", outputFormat)
        } catch {
            NSLog("Valtz mark: Metal setup failed: \(error)")
            return nil
        }
        // Static: the mark's motion is applied in the vertex shader.
        guard let (vb, ib, n, plate) = MarkGeometry.build(device: device) else {
            return nil
        }
        vertices = vb
        indices = ib
        indexCount = n
        plateIndexCount = plate
        holes = MarkGeometry.holes
        holeShapes = MarkGeometry.holeShapes
    }

    // MARK: - Drawing

    /// Encode the mark into `output` (a render target of `outputFormat`).
    /// `pointSize`: the size it is SEEN at, which picks the level of
    /// detail (a 16 px icon gets no sprocket holes).
    func encode(_ cmd: MTLCommandBuffer, output: MTLTexture, style: Style,
                pointSize: Float) {
        let w = output.width
        let h = output.height
        guard w > 0, h > 0,
              let t = ensureTargets(width: w, height: h) else { return }

        // Design space is the artwork's 1024 px, y down. `.mark` frames it
        // exactly as the painted artwork was (its 1024 canvas fills the
        // view); `.markFit` fits the mark's own extent, with room for its
        // glow; on the icon the extent fills 74% of the body's grid.
        let side = Float(min(w, h))
        let lo = MarkGeometry.bounds.min, hi = MarkGeometry.bounds.max
        let extent = max(hi.x - lo.x, hi.y - lo.y)
        let centre: SIMD2<Float>
        let k: Float                                        // output px per design px
        switch style {
        case .mark:
            centre = [512, 512]
            k = side / 1024
        case .markFit:
            centre = (lo + hi) / 2
            let mx: Float = shadow ? 1.20 : 1.06, my: Float = shadow ? 1.32 : 1.08
            k = min(Float(w) / ((hi.x - lo.x) * mx), Float(h) / ((hi.y - lo.y) * my))
        case .appIcon:
            centre = (lo + hi) / 2
            // The mark's extent fills 60% of the icon (the body is 80%),
            // leaving a clear margin inside the rounded rectangle.
            k = 0.60 * side / extent
        }
        let sx = 2 * k / Float(w), sy = 2 * k / Float(h)
        let sway = motion * 0.035 * sin(phase * .pi)
        // Rotate by the sway in design space, then scale, flipping y up.
        let proj = simd_float4x4(columns: (
            SIMD4(sx * cos(sway), -sy * sin(sway), 0, 0),
            SIMD4(-sx * sin(sway), -sy * cos(sway), 0, 0),
            SIMD4(0, 0, 1, 0),
            SIMD4(0, 0, 0, 1)))
        let mvp = proj * simd_float4x4(translation: SIMD3(-centre.x, -centre.y, 0))
        var su = SceneUniforms(
            mvp: mvp,
            anim: SIMD4(motion, phase,
                        pointSize >= 96 ? 1 : pointSize >= 48
                            ? (pointSize - 48) / 48 : 0,
                        k),
            hole: SIMD4(material == .glass ? 1 : 0, flat ? 1 : 0, MarkGeometry.holeRadius,
                        Float(min(holes.count, 8))),
            holes: (holes[0], holes[1], holes[2], holes[3],
                    holes[4], holes[5], holes[6], holes[7]),
            holeShapes: (holeShapes[0], holeShapes[1], holeShapes[2], holeShapes[3],
                         holeShapes[4], holeShapes[5], holeShapes[6], holeShapes[7]),
            sweep: sweepLight(),
            ground: SIMD4(lightGround && material == .glass ? 1 : 0, 0, 0, 0))

        // 1. The layers, back to front (painter's order, no depth), MSAA,
        //    into the HDR color and glow targets.
        let sp = MTLRenderPassDescriptor()
        sp.colorAttachments[0].texture = t.msaaColor
        sp.colorAttachments[0].resolveTexture = t.color
        sp.colorAttachments[1].texture = t.msaaGlow
        sp.colorAttachments[1].resolveTexture = t.glow
        for i in 0..<2 {
            sp.colorAttachments[i].loadAction = .clear
            sp.colorAttachments[i].clearColor = MTLClearColor(red: 0, green: 0,
                                                              blue: 0, alpha: 0)
            sp.colorAttachments[i].storeAction = .multisampleResolve
        }
        if let e = cmd.makeRenderCommandEncoder(descriptor: sp) {
            e.setRenderPipelineState(material == .glass ? glassPipeline : scenePipeline)
            e.setCullMode(.none)
            e.setVertexBuffer(vertices, offset: 0, index: 0)
            e.setVertexBytes(&su, length: MemoryLayout<SceneUniforms>.stride,
                             index: 1)
            e.setFragmentBytes(&su, length: MemoryLayout<SceneUniforms>.stride,
                               index: 1)
            e.drawIndexedPrimitives(type: .triangle,
                                    indexCount: flat ? plateIndexCount : indexCount,
                                    indexType: .uint32, indexBuffer: indices,
                                    indexBufferOffset: 0)
            e.endEncoding()
        }

        // 2. Bloom: a near halo at half resolution, a wide one at a quarter.
        quadPass(cmd, downPipeline, into: t.halfA, sources: [t.glow])
        blur(cmd, t.halfA, t.halfB, sigma: side * 0.010 / 2)
        quadPass(cmd, downPipeline, into: t.quarterA, sources: [t.halfA])
        blur(cmd, t.quarterA, t.quarterB, sigma: side * 0.028 / 4)

        // The shadow: the mark's coverage at a quarter resolution, blurred.
        let wantShadow = shadow && style != .appIcon && !flat
        if wantShadow {
            quadPass(cmd, downPipeline, into: t.shadowHalf, sources: [t.color])
            quadPass(cmd, downPipeline, into: t.shadowA, sources: [t.shadowHalf])
            blur(cmd, t.shadowA, t.shadowB, sigma: side * 0.040 / 4)
        }

        // 3. Composite into the output.
        var cu = CompositeUniforms(
            body: SIMD4(412, 5.2, side / 1024, style == .appIcon ? 1 : 0),
            bloom: SIMD4(1.0, 0.35, flat ? 0 : style == .appIcon ? 0.45
                                    : material == .glass
                                        ? 0.12 + 0.5 * sweepLight().w : 0.30, 0),
            shadow: SIMD4(wantShadow ? 0.20 : 0, 0.030 * side / Float(h), 0, 0))
        quadPass(cmd, compositePipeline, into: output,
                 sources: [t.color, t.halfA, t.quarterA, t.shadowA],
                 bytes: &cu, length: MemoryLayout<CompositeUniforms>.stride)
    }

    /// One renderer for still frames (the app's idle mark), created on
    /// first use.
    static let shared = MarkRenderer()

    /// A finished image: rendered at `size` x `supersample` and scaled
    /// down, which beats MSAA alone for the fine lines of small sizes.
    /// `pointSize` picks the level of detail (default: `size`).
    func image(size: Int, style: Style, supersample: Int = 1,
               pointSize: Float? = nil) -> CGImage? {
        image(width: size, height: size, style: style, supersample: supersample,
              pointSize: pointSize)
    }

    func image(width: Int, height: Int, style: Style, supersample: Int = 1,
               pointSize: Float? = nil) -> CGImage? {
        let ss = max(1, supersample)
        let nw = width * ss, nh = height * ss
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: outputFormat, width: nw, height: nh, mipmapped: false)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .shared
        guard let tex = device.makeTexture(descriptor: d),
              let cmd = queue.makeCommandBuffer() else { return nil }
        encode(cmd, output: tex, style: style,
               pointSize: pointSize ?? Float(min(width, height)))
        cmd.commit()
        cmd.waitUntilCompleted()
        var bytes = [UInt8](repeating: 0, count: nw * nh * 4)
        tex.getBytes(&bytes, bytesPerRow: nw * 4,
                     from: MTLRegionMake2D(0, 0, nw, nh), mipmapLevel: 0)
        let cs = CGColorSpace(name: CGColorSpace.sRGB)!
        let info = CGBitmapInfo.byteOrder32Little.rawValue
            | CGImageAlphaInfo.premultipliedFirst.rawValue
        guard let full = bytes.withUnsafeMutableBytes({ p -> CGImage? in
            CGContext(data: p.baseAddress, width: nw, height: nh,
                      bitsPerComponent: 8, bytesPerRow: nw * 4, space: cs,
                      bitmapInfo: info)?.makeImage()
        }) else { return nil }
        if ss == 1 { return full }
        guard let ctx = CGContext(data: nil, width: width, height: height,
                                  bitsPerComponent: 8, bytesPerRow: 0,
                                  space: cs, bitmapInfo: info) else {
            return nil
        }
        ctx.interpolationQuality = .high
        ctx.draw(full, in: CGRect(x: 0, y: 0, width: width, height: height))
        return ctx.makeImage()
    }

    var commandQueue: MTLCommandQueue { queue }

    /// The passing light: its position (design px; z in front of the
    /// mark) and strength. It starts beyond the window's right, well off
    /// the mark, crosses in front of it and ends beyond the left, eased at
    /// both ends; its strength rises and falls with it, so nothing is lit
    /// before or after.
    private func sweepLight() -> SIMD4<Float> {
        guard sweep >= 0, sweep <= 1 else { return .zero }
        let e = sweep * sweep * (3 - 2 * sweep)
        let lo = MarkGeometry.bounds.min, hi = MarkGeometry.bounds.max
        let span = hi.x - lo.x
        let x = (hi.x + 0.7 * span) - e * (span * 2.4)
        let strength = pow(sin(.pi * sweep), 1.2)
        return SIMD4(x, lo.y - 60, 520, strength)
    }

    // MARK: - Passes

    private func quadPass(_ cmd: MTLCommandBuffer, _ pipe: MTLRenderPipelineState,
                          into target: MTLTexture, sources: [MTLTexture],
                          bytes: UnsafeRawPointer? = nil, length: Int = 0) {
        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = target
        rp.colorAttachments[0].loadAction = .dontCare
        rp.colorAttachments[0].storeAction = .store
        guard let e = cmd.makeRenderCommandEncoder(descriptor: rp) else { return }
        e.setRenderPipelineState(pipe)
        for (i, s) in sources.enumerated() {
            e.setFragmentTexture(s, index: i)
        }
        if let bytes {
            e.setFragmentBytes(bytes, length: length, index: 0)
        }
        e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        e.endEncoding()
    }

    /// Separable gaussian, `a` -> `b` -> `a`.
    private func blur(_ cmd: MTLCommandBuffer, _ a: MTLTexture, _ b: MTLTexture,
                      sigma: Float) {
        var h = BlurUniforms(direction: SIMD2(1 / Float(a.width), 0),
                             sigma: max(sigma, 0.5), pad: 0)
        quadPass(cmd, blurPipeline, into: b, sources: [a], bytes: &h,
                 length: MemoryLayout<BlurUniforms>.stride)
        var v = BlurUniforms(direction: SIMD2(0, 1 / Float(a.height)),
                             sigma: max(sigma, 0.5), pad: 0)
        quadPass(cmd, blurPipeline, into: a, sources: [b], bytes: &v,
                 length: MemoryLayout<BlurUniforms>.stride)
    }

    private struct Targets {
        let width: Int, height: Int
        let msaaColor, msaaGlow: MTLTexture
        let color, glow: MTLTexture
        let halfA, halfB, quarterA, quarterB: MTLTexture
        let shadowHalf, shadowA, shadowB: MTLTexture
    }

    private func ensureTargets(width: Int, height: Int) -> Targets? {
        if let t = targets, t.width == width, t.height == height { return t }
        func tex(_ f: MTLPixelFormat, _ w: Int, _ h: Int, msaa: Bool = false,
                 memoryless: Bool = false) -> MTLTexture? {
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: f, width: max(w, 1), height: max(h, 1),
                mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]
            if msaa {
                d.textureType = .type2DMultisample
                d.sampleCount = Self.samples
                d.usage = [.renderTarget]
            }
            d.storageMode = memoryless ? .memoryless : .private
            return device.makeTexture(descriptor: d)
        }
        guard let mc = tex(.rgba16Float, width, height, msaa: true, memoryless: true),
              let mg = tex(.rgba16Float, width, height, msaa: true, memoryless: true),
              let c = tex(.rgba16Float, width, height),
              let g = tex(.rgba16Float, width, height),
              let ha = tex(.rgba16Float, width / 2, height / 2),
              let hb = tex(.rgba16Float, width / 2, height / 2),
              let qa = tex(.rgba16Float, width / 4, height / 4),
              let qb = tex(.rgba16Float, width / 4, height / 4),
              let sh = tex(.rgba16Float, width / 2, height / 2),
              let sa = tex(.rgba16Float, width / 4, height / 4),
              let sb = tex(.rgba16Float, width / 4, height / 4) else {
            return nil
        }
        let t = Targets(width: width, height: height, msaaColor: mc,
                        msaaGlow: mg, color: c, glow: g,
                        halfA: ha, halfB: hb, quarterA: qa, quarterB: qb,
                        shadowHalf: sh, shadowA: sa, shadowB: sb)
        targets = t
        return t
    }
}

// MARK: - Uniforms (layouts match MarkShaders.swift)

private struct SceneUniforms {
    var mvp: simd_float4x4
    var anim: SIMD4<Float>     // motion, phase, detail, px per design px
    var hole: SIMD4<Float>     // glass (0/1), -, corner radius, count
    var holes: (SIMD4<Float>, SIMD4<Float>, SIMD4<Float>, SIMD4<Float>,
                SIMD4<Float>, SIMD4<Float>, SIMD4<Float>, SIMD4<Float>)
    var holeShapes: (SIMD4<Float>, SIMD4<Float>, SIMD4<Float>, SIMD4<Float>,
                     SIMD4<Float>, SIMD4<Float>, SIMD4<Float>, SIMD4<Float>)
    var sweep: SIMD4<Float>    // toward the passing light, strength
    var ground: SIMD4<Float>   // x: light ground (0/1)
}

private struct BlurUniforms {
    var direction: SIMD2<Float>
    var sigma: Float
    var pad: Float
}

private struct CompositeUniforms {
    var body: SIMD4<Float>
    var bloom: SIMD4<Float>
    var shadow: SIMD4<Float>   // strength, offset down (uv), -, -
}

// MARK: - Matrices

extension simd_float4x4 {
    init(translation t: SIMD3<Float>) {
        self.init(columns: (SIMD4(1, 0, 0, 0),
                            SIMD4(0, 1, 0, 0),
                            SIMD4(0, 0, 1, 0),
                            SIMD4(t.x, t.y, t.z, 1)))
    }
}
