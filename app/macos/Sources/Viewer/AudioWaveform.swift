import AVFoundation
import CoreGraphics

/// A SOUND's picture: its waveform, read with AVFoundation (its samples,
/// mixed to one channel, the loudest of each column) and drawn as bars --
/// the stage's poster while it plays, and its thumbnail in the lists.
enum AudioWaveform {
    /// `width` x `height` pixels; nil when it has no sound to read.
    nonisolated static func image(_ url: URL, width: Int,
                                  height: Int) async -> CGImage? {
        guard let peaks = await peaks(url, count: max(8, width / 6)) else {
            return nil
        }
        return draw(peaks, width: width, height: height)
    }

    /// The loudest sample of each of `count` stretches, 0...1 -- kept
    /// as the samples stream by: an hour's sound is 115 MB of samples at
    /// 8 kHz, and never more than a buffer of it is held.
    nonisolated private static func peaks(_ url: URL,
                                          count: Int) async -> [Float]? {
        let asset = AVURLAsset(url: url)
        guard let track = try? await asset.loadTracks(withMediaType: .audio)
                  .first,
              let duration = try? await asset.load(.duration),
              duration.seconds > 0,
              let reader = try? AVAssetReader(asset: asset) else {
            return nil
        }
        let rate = 8000.0
        let out = AVAssetReaderTrackOutput(track: track, outputSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVLinearPCMBitDepthKey: 32,
            AVLinearPCMIsFloatKey: true,
            AVLinearPCMIsBigEndianKey: false,
            AVLinearPCMIsNonInterleaved: false,
            AVNumberOfChannelsKey: 1,
            AVSampleRateKey: rate,
        ])
        guard reader.canAdd(out) else { return nil }
        reader.add(out)
        guard reader.startReading() else { return nil }
        // Each stretch as long as the sound's length says; the last takes
        // what is left over.
        let per = max(1, Int(duration.seconds * rate) / count)
        var peaks = [Float](repeating: 0, count: count)
        var at = 0
        var chunk: [Float] = []
        while let buf = out.copyNextSampleBuffer(),
              let block = CMSampleBufferGetDataBuffer(buf) {
            let n = CMBlockBufferGetDataLength(block)
            if chunk.count != n / 4 {
                chunk = [Float](repeating: 0, count: n / 4)
            }
            chunk.withUnsafeMutableBytes { raw in
                _ = CMBlockBufferCopyDataBytes(block, atOffset: 0,
                                               dataLength: n,
                                               destination: raw.baseAddress!)
            }
            for s in chunk {
                let i = min(count - 1, at / per)
                peaks[i] = max(peaks[i], abs(s))
                at += 1
            }
        }
        guard at > 0 else { return nil }
        // A sound shorter than its stated length: only what it has.
        let used = min(count, (at + per - 1) / per)
        let top = max(peaks[..<used].max() ?? 1, 0.001)
        return peaks[..<used].map { $0 / top }
    }

    nonisolated private static func draw(_ peaks: [Float], width: Int,
                                         height: Int) -> CGImage? {
        guard let ctx = CGContext(
            data: nil, width: width, height: height, bitsPerComponent: 8,
            bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        // A dark plate, so it reads as a sound wherever it is shown.
        ctx.setFillColor(CGColor(srgbRed: 0.11, green: 0.11, blue: 0.12,
                                 alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let step = CGFloat(width) / CGFloat(peaks.count)
        let bar = max(1, step * 0.6)
        let mid = CGFloat(height) / 2
        ctx.setFillColor(CGColor(srgbRed: 0.98, green: 0.62, blue: 0.24,
                                 alpha: 1))
        for (i, p) in peaks.enumerated() {
            let h = max(1, CGFloat(p) * CGFloat(height) * 0.8)
            let r = CGRect(x: CGFloat(i) * step + (step - bar) / 2,
                           y: mid - h / 2, width: bar, height: h)
            ctx.addPath(CGPath(roundedRect: r, cornerWidth: bar / 2,
                               cornerHeight: min(bar / 2, h / 2),
                               transform: nil))
        }
        ctx.fillPath()
        return ctx.makeImage()
    }
}
