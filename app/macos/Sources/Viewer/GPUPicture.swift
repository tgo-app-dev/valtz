import CoreGraphics
import CoreImage
import Foundation
import IOSurface

/// A picture Core Image drew on the GPU into an IOSurface -- which the
/// stage's layers show as it is (CALayer.contents), with no copy -- and
/// the same pixels as a CGImage over the surface's memory, for everything
/// that reads them (share, save, a drag, the compare's logic), with no
/// copy either.
///
/// Rendering to a CGImage instead read every frame back from the GPU --
/// lazily, when Core Animation first touched it, on the main thread --
/// and Core Animation then copied it into GPU memory again. MEASURED on a
/// 24 MP picture, M5 Pro: ~27 ms a render that way, ~10 ms into a surface.
struct GPUPicture: @unchecked Sendable {
    let image: CGImage
    let surface: IOSurface

    /// `image` (Core Image's frame, origin bottom left) cropped to
    /// `extent`, drawn into a new 8-bit surface in `colorSpace` -- tagged
    /// with it, so Core Animation colour-matches what it shows. nil if
    /// Metal or Core Image cannot.
    ///
    /// A context for this render alone, released with it: a long-lived
    /// one keeps the Metal memory of its largest render, and the engine
    /// shares this process's GPU budget (core model-input.mm). Making one
    /// costs ~3 ms; Core Image keeps its compiled kernels across them.
    nonisolated static func render(_ image: CIImage, extent: CGRect,
                                   colorSpace: CGColorSpace) -> GPUPicture? {
        let w = Int(extent.width.rounded()), h = Int(extent.height.rounded())
        guard w > 0, h > 0,
              let surface = IOSurface(properties: [
                  .width: w, .height: h, .bytesPerElement: 4,
                  .bytesPerRow: IOSurfacePropertyKey.alignedBytesPerRow(w * 4),
                  .pixelFormat: 0x4247_5241,  // 'BGRA'
              ])
        else { return nil }
        if let tags = colorSpace.copyPropertyList() {
            IOSurfaceSetValue(surface, kIOSurfaceColorSpace, tags)
        }
        let context = CIContext(options: [
            .workingColorSpace: CGColorSpace(
                name: CGColorSpace.extendedLinearSRGB)!,
            .workingFormat: CIFormat.RGBAh,
            .cacheIntermediates: false,
        ])
        let dest = CIRenderDestination(ioSurface: surface)
        dest.colorSpace = colorSpace
        dest.alphaMode = .premultiplied
        // Row 0 at the top, as a CGImage's and the layer's.
        dest.isFlipped = true
        let moved = image.cropped(to: extent).transformed(
            by: CGAffineTransform(translationX: -extent.minX,
                                  y: -extent.minY))
        do {
            let task = try context.startTask(toRender: moved, to: dest)
            _ = try task.waitUntilCompleted()
        } catch {
            return nil
        }
        guard let cg = cgImage(over: surface, colorSpace: colorSpace)
        else { return nil }
        return GPUPicture(image: cg, surface: surface)
    }

    /// A CGImage reading the surface's memory in place: locked for reading
    /// as long as the image lives, the surface kept with it. The surface
    /// holds 8-bit premultiplied BGRA.
    nonisolated static func cgImage(over surface: IOSurface,
                                    colorSpace: CGColorSpace) -> CGImage? {
        guard surface.lock(options: .readOnly, seed: nil) == kIOReturnSuccess
        else { return nil }
        let held = Unmanaged.passRetained(surface)
        let bytes = surface.bytesPerRow * surface.height
        guard let provider = CGDataProvider(
            dataInfo: held.toOpaque(), data: surface.baseAddress,
            size: bytes,
            releaseData: { info, _, _ in
                guard let info else { return }
                let s = Unmanaged<IOSurface>.fromOpaque(info)
                s.takeUnretainedValue().unlock(options: .readOnly, seed: nil)
                s.release()
            })
        else {
            surface.unlock(options: .readOnly, seed: nil)
            held.release()
            return nil
        }
        return CGImage(
            width: surface.width, height: surface.height,
            bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: surface.bytesPerRow, space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue:
                CGImageAlphaInfo.premultipliedFirst.rawValue
                | CGBitmapInfo.byteOrder32Little.rawValue),
            provider: provider, decode: nil, shouldInterpolate: true,
            intent: .defaultIntent)
    }
}

extension IOSurfacePropertyKey {
    /// Row bytes as IOSurface wants them aligned.
    static func alignedBytesPerRow(_ n: Int) -> Int {
        IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, n)
    }
}
