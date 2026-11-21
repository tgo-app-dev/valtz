// valtz-mark -- the Valtz mark as media (core media/valtz-mark.h): the V
// drawn with Metal by MarkRenderer (the code the app draws it with),
// beside the word "valtz" in Bookman Old Style Bold Italic, light grey
// with a dark grey glow around it -- one lockup, centred in the picture.
//
//   valtz-mark --width W --height H --out mark.png            (a still)
//   valtz-mark --width W --height H --frames N --fps-num 24 --fps-den 1
//              --out mark.mov                                  (a clip)
//
// A clip plays the light sweep once, over its first second, then holds
// the mark settled; it is ProRes 4444 with alpha, silent (the core joins
// the sonic logo). A still is the mark settled, a PNG with alpha. The
// core starts it for each size it needs; it exits 0 when the file is
// written.

import AVFoundation
import CoreGraphics
import CoreText
import CoreVideo
import Foundation
import ImageIO
import UniformTypeIdentifiers

func fail(_ s: String) -> Never {
    FileHandle.standardError.write(Data("valtz-mark: \(s)\n".utf8))
    exit(1)
}

var width = 0, height = 0, frames = 0
var fpsNum: Int32 = 24, fpsDen: Int32 = 1
var out = ""
var args = CommandLine.arguments.dropFirst().makeIterator()
while let a = args.next() {
    switch a {
    case "--width": width = Int(args.next() ?? "") ?? 0
    case "--height": height = Int(args.next() ?? "") ?? 0
    case "--frames": frames = Int(args.next() ?? "") ?? 0
    case "--fps-num": fpsNum = Int32(args.next() ?? "") ?? 24
    case "--fps-den": fpsDen = Int32(args.next() ?? "") ?? 1
    case "--out": out = args.next() ?? ""
    default: fail("unknown \(a)")
    }
}
guard width >= 16, height >= 16, !out.isEmpty, fpsNum > 0, fpsDen > 0 else {
    fail("usage: valtz-mark --width W --height H [--frames N] --out file")
}

/// The word's font: Bookman Old Style Bold Italic where the Mac has it
/// (Microsoft Office installs it), else its free twins, else the nearest
/// Apple ships.
func wordFont(_ size: CGFloat) -> CTFont {
    for name in ["BookmanOldStyle-BoldItalic", "TeXGyreBonum-BoldItalic",
                 "URWBookman-DemiItalic", "BookmanURW-DemItal",
                 "ITCBookman-DemiItalic", "Georgia-BoldItalic"] {
        let f = CTFontCreateWithName(name as CFString, size, nil)
        if (CTFontCopyPostScriptName(f) as String) == name { return f }
    }
    return CTFontCreateWithName("Georgia-BoldItalic" as CFString, size, nil)
}

let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!
let bitmapInfo = CGBitmapInfo.byteOrder32Little.rawValue
    | CGImageAlphaInfo.premultipliedFirst.rawValue

/// The lockup's layout in a W x H picture: the mark's box, the word's
/// line and where it goes. The mark is 70% of the height; the word's
/// size follows it; both shrink together where they would not fit 92% of
/// the width.
struct Layout {
    var markRect: CGRect
    var line: CTLine
    var origin: CGPoint
    var glow: CGFloat
}

func layout(width w: Int, height h: Int) -> Layout {
    let W = CGFloat(w), H = CGFloat(h)
    let lo = MarkGeometry.bounds.min, hi = MarkGeometry.bounds.max
    // `.markFit` leaves 6% / 8% around the mark's extent for its glow.
    let aspect = CGFloat((hi.x - lo.x) * 1.06 / ((hi.y - lo.y) * 1.08))
    func measure(_ s: CGFloat) -> (CGFloat, CGFloat, CTLine, CGRect) {
        let markH = 0.70 * H * s
        let markW = markH * aspect
        let font = wordFont(0.40 * H * s)
        let attr = NSAttributedString(string: "valtz", attributes: [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String):
                CGColor(srgbRed: 0.84, green: 0.84, blue: 0.85, alpha: 1),
        ])
        let line = CTLineCreateWithAttributedString(attr)
        let ink = CTLineGetImageBounds(line, nil)
        return (markW, markH, line, ink)
    }
    var s: CGFloat = 1
    var (markW, markH, line, ink) = measure(s)
    let gapOf = { (mh: CGFloat) in 0.06 * mh }
    let total = markW + gapOf(markH) + ink.width
    if total > 0.92 * W {
        s = 0.92 * W / total
        (markW, markH, line, ink) = measure(s)
    }
    let gap = gapOf(markH)
    let left = (W - (markW + gap + ink.width)) / 2
    let markRect = CGRect(x: left, y: (H - markH) / 2, width: markW,
                          height: markH)
    // The word's ink centred on the picture's middle, beside the mark.
    let origin = CGPoint(x: left + markW + gap - ink.minX,
                         y: H / 2 - ink.midY)
    return Layout(markRect: markRect, line: line, origin: origin,
                  glow: 0.40 * H * s * 0.16)
}

/// The word, drawn: its glow -- a dark grey halo, twice for its weight --
/// under light grey letters.
func drawWord(_ ctx: CGContext, _ l: Layout) {
    let halo = CGColor(srgbRed: 0.22, green: 0.22, blue: 0.23, alpha: 1)
    for blur in [l.glow, l.glow * 0.45] {
        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: blur, color: halo)
        ctx.textPosition = l.origin
        CTLineDraw(l.line, ctx)
        ctx.restoreGState()
    }
    ctx.textPosition = l.origin
    CTLineDraw(l.line, ctx)
}

@MainActor
func frame(_ r: MarkRenderer, _ l: Layout, sweep: Float,
           into ctx: CGContext) {
    ctx.clear(CGRect(x: 0, y: 0, width: width, height: height))
    r.sweep = sweep
    let mw = max(1, Int(l.markRect.width.rounded()))
    let mh = max(1, Int(l.markRect.height.rounded()))
    let ss = min(mw, mh) <= 256 ? 4 : 2
    if let img = r.image(width: mw, height: mh, style: .markFit,
                         supersample: ss) {
        ctx.interpolationQuality = .high
        ctx.draw(img, in: CGRect(x: l.markRect.minX.rounded(),
                                 y: l.markRect.minY.rounded(),
                                 width: CGFloat(mw), height: CGFloat(mh)))
    }
    drawWord(ctx, l)
}

MainActor.assumeIsolated {
    guard let r = MarkRenderer() else { fail("no Metal device") }
    r.material = .glass
    let l = layout(width: width, height: height)
    let url = URL(fileURLWithPath: out)
    try? FileManager.default.removeItem(at: url)

    if frames <= 0 {
        guard let ctx = CGContext(data: nil, width: width, height: height,
                                  bitsPerComponent: 8, bytesPerRow: 0,
                                  space: sRGB, bitmapInfo: bitmapInfo)
        else { fail("no bitmap") }
        frame(r, l, sweep: -1, into: ctx)
        guard let img = ctx.makeImage(),
              let dst = CGImageDestinationCreateWithURL(
                  url as CFURL, UTType.png.identifier as CFString, 1, nil)
        else { fail("cannot write \(out)") }
        CGImageDestinationAddImage(dst, img, nil)
        if !CGImageDestinationFinalize(dst) { fail("cannot write \(out)") }
        exit(0)
    }

    guard let writer = try? AVAssetWriter(outputURL: url, fileType: .mov)
    else { fail("cannot write \(out)") }
    let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
        AVVideoCodecKey: AVVideoCodecType.proRes4444,
        AVVideoWidthKey: width,
        AVVideoHeightKey: height,
        AVVideoColorPropertiesKey: [
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
        ],
    ])
    input.expectsMediaDataInRealTime = false
    let adaptor = AVAssetWriterInputPixelBufferAdaptor(
        assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
        ])
    writer.add(input)
    guard writer.startWriting() else {
        fail("cannot write \(out): \(writer.error?.localizedDescription ?? "")")
    }
    writer.startSession(atSourceTime: .zero)
    let fps = Double(fpsNum) / Double(fpsDen)
    for i in 0..<frames {
        while !input.isReadyForMoreMediaData { usleep(1000) }
        guard let pool = adaptor.pixelBufferPool else { fail("no buffer pool") }
        var pb: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb)
        guard let pb else { fail("no pixel buffer") }
        CVPixelBufferLockBaseAddress(pb, [])
        guard let ctx = CGContext(
            data: CVPixelBufferGetBaseAddress(pb), width: width,
            height: height, bitsPerComponent: 8,
            bytesPerRow: CVPixelBufferGetBytesPerRow(pb), space: sRGB,
            bitmapInfo: bitmapInfo)
        else { fail("no bitmap") }
        // The sweep over the first second, then the mark settled.
        let t = Double(i) / fps
        frame(r, l, sweep: t < 1 ? Float(t) : -1, into: ctx)
        CVPixelBufferUnlockBaseAddress(pb, [])
        CVBufferSetAttachment(pb, kCVImageBufferAlphaChannelModeKey,
                              kCVImageBufferAlphaChannelMode_PremultipliedAlpha,
                              .shouldPropagate)
        let at = CMTime(value: CMTimeValue(i) * CMTimeValue(fpsDen),
                        timescale: fpsNum)
        if !adaptor.append(pb, withPresentationTime: at) {
            fail("cannot write frame \(i): "
                 + (writer.error?.localizedDescription ?? ""))
        }
    }
    input.markAsFinished()
    let done = DispatchSemaphore(value: 0)
    writer.finishWriting { done.signal() }
    // finishWriting calls back off the main thread.
    while done.wait(timeout: .now() + 0.01) == .timedOut {
        RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.01))
    }
    if writer.status != .completed {
        fail("cannot write \(out): \(writer.error?.localizedDescription ?? "")")
    }
    exit(0)
}
