// valtz-icon -- renders the Valtz mark with Metal (MarkRenderer, the same
// code the app draws it with) to a PNG. The build runs it once per icon
// size to make AppIcon.icns, so every size is rendered natively at its
// own level of detail rather than scaled down from one bitmap.
//
//   valtz-icon --size 1024 --out icon.png [--mark | --fit] [--glass]
//              [--width w --height h] [--supersample n]
//              [--motion 0..1 --phase p] [--sweep 0..1] [--flat]
//              [--light-ground] [--shadow]

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

var size = 1024
var width = 0, height = 0
var material = MarkRenderer.Material.metal
var out = ""
var style = MarkRenderer.Style.appIcon
var supersample = 0
var motion: Float = 0
var phase: Float = 0
var sweep: Float = -1
var flat = false
var lightGround = false
var shadow = false
var args = CommandLine.arguments.dropFirst().makeIterator()
while let a = args.next() {
    switch a {
    case "--size": size = Int(args.next() ?? "") ?? size
    case "--out": out = args.next() ?? ""
    case "--mark": style = .mark
    case "--fit": style = .markFit
    case "--glass": material = .glass
    case "--width": width = Int(args.next() ?? "") ?? 0
    case "--height": height = Int(args.next() ?? "") ?? 0
    case "--supersample": supersample = Int(args.next() ?? "") ?? 0
    case "--motion": motion = Float(args.next() ?? "") ?? 0
    case "--phase": phase = Float(args.next() ?? "") ?? 0
    case "--sweep": sweep = Float(args.next() ?? "") ?? -1
    case "--flat": flat = true
    case "--light-ground": lightGround = true
    case "--shadow": shadow = true
    default:
        FileHandle.standardError.write(Data("valtz-icon: unknown \(a)\n".utf8))
        exit(2)
    }
}
guard !out.isEmpty, size > 0 else {
    FileHandle.standardError.write(Data(
        "usage: valtz-icon --size N --out file.png [--mark]\n".utf8))
    exit(2)
}
// Small sizes render larger and scale down: their lines are finer than
// MSAA alone resolves.
if supersample <= 0 {
    supersample = size <= 64 ? 8 : size <= 256 ? 4 : 2
}

MainActor.assumeIsolated {
    guard let r = MarkRenderer() else {
        FileHandle.standardError.write(Data("valtz-icon: no Metal device\n".utf8))
        exit(1)
    }
    r.motion = motion
    r.phase = phase
    r.material = material
    r.sweep = sweep
    r.flat = flat
    r.lightGround = lightGround
    r.shadow = shadow
    guard let img = r.image(width: width > 0 ? width : size,
                            height: height > 0 ? height : size,
                            style: style, supersample: supersample)
    else {
        FileHandle.standardError.write(Data("valtz-icon: render failed\n".utf8))
        exit(1)
    }
    let url = URL(fileURLWithPath: out)
    guard let dst = CGImageDestinationCreateWithURL(
        url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        exit(1)
    }
    CGImageDestinationAddImage(dst, img, nil)
    if !CGImageDestinationFinalize(dst) {
        FileHandle.standardError.write(Data("valtz-icon: cannot write \(out)\n".utf8))
        exit(1)
    }
}
