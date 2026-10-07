import CoreGraphics
import Foundation

/// An sRGB colour, straight alpha, 0...1 -- as the core keeps markup's
/// (core media/markup.h).
struct RGBA: Codable, Equatable, Hashable, Sendable {
    var r: Double
    var g: Double
    var b: Double
    var a: Double

    static let clear = RGBA(r: 0, g: 0, b: 0, a: 0)
    static let red = RGBA(r: 0.95, g: 0.18, b: 0.16, a: 1)

    init(r: Double, g: Double, b: Double, a: Double) {
        (self.r, self.g, self.b, self.a) = (r, g, b, a)
    }

    /// From the core's [r, g, b, a]; nil for anything else.
    init?(_ any: Any?) {
        guard let v = any as? [Double], v.count == 4 else { return nil }
        self.init(r: v[0], g: v[1], b: v[2], a: v[3])
    }

    init(cgColor: CGColor) {
        let srgb = CGColorSpace(name: CGColorSpace.sRGB)!
        let c = cgColor.converted(to: srgb, intent: .defaultIntent,
                                  options: nil)?.components ?? [0, 0, 0, 1]
        self.init(r: Double(c[0]), g: Double(c.count > 2 ? c[1] : c[0]),
                  b: Double(c.count > 2 ? c[2] : c[0]),
                  a: Double(c.last ?? 1))
    }

    var cgColor: CGColor {
        CGColor(colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                components: [r, g, b, a].map { CGFloat($0) })!
    }

    var json: [Double] { [r, g, b, a] }

    init(from d: Decoder) throws {
        let v = try d.singleValueContainer().decode([Double].self)
        guard v.count == 4 else {
            throw DecodingError.dataCorrupted(.init(codingPath: d.codingPath,
                debugDescription: "a colour is [r, g, b, a]"))
        }
        self.init(r: v[0], g: v[1], b: v[2], a: v[3])
    }

    func encode(to e: Encoder) throws {
        var c = e.singleValueContainer()
        try c.encode(json)
    }
}

/// A text object's font (core media/markup.h): a family at a size in
/// canvas pixels, bold and italic where the family has them.
struct MarkupFont: Codable, Equatable, Hashable, Sendable {
    var family = "Helvetica Neue"
    var size: Double = 48
    var bold = false
    var italic = false
    var underline = false

    init() {}

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        family = (try? c.decodeIfPresent(String.self, forKey: .family))
            .flatMap { $0 } ?? family
        size = (try? c.decodeIfPresent(Double.self, forKey: .size))
            .flatMap { $0 } ?? size
        bold = (try? c.decodeIfPresent(Bool.self, forKey: .bold))
            .flatMap { $0 } ?? false
        italic = (try? c.decodeIfPresent(Bool.self, forKey: .italic))
            .flatMap { $0 } ?? false
        underline = (try? c.decodeIfPresent(Bool.self, forKey: .underline))
            .flatMap { $0 } ?? false
    }

    var json: [String: Any] {
        ["family": family, "size": size, "bold": bold, "italic": italic,
         "underline": underline]
    }
}

/// A vector object on a markup layer (core media/markup.h): canvas
/// pixels, top-left origin. A line runs (x0, y0) -> (x1, y1); a rectangle
/// or ellipse fills that box; a text starts at (x0, y0), its top-left.
struct MarkupObject: Codable, Equatable, Hashable, Sendable, Identifiable {
    enum Kind: String, Codable, Sendable {
        case line, rect, ellipse, text
    }
    var id: String
    var kind: Kind
    var x0: Double
    var y0: Double
    var x1: Double
    var y1: Double
    var stroke: RGBA = .red
    var fill: RGBA = .clear
    var width: Double = 4
    var text = ""
    var font = MarkupFont()
    /// A text BOX, (x0, y0) to (x1, y1): its words wrapped within its
    /// width, the lines that fit whole in its height shown (core
    /// media/markup.h). A text from before has none: unwrapped.
    var box = false

    init(id: String, kind: Kind, x0: Double, y0: Double, x1: Double,
         y1: Double) {
        (self.id, self.kind) = (id, kind)
        (self.x0, self.y0, self.x1, self.y1) = (x0, y0, x1, y1)
    }

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        kind = try c.decode(Kind.self, forKey: .kind)
        func num(_ k: CodingKeys) -> Double {
            (try? c.decodeIfPresent(Double.self, forKey: k)).flatMap { $0 } ?? 0
        }
        (x0, y0, x1, y1) = (num(.x0), num(.y0), num(.x1), num(.y1))
        stroke = (try? c.decodeIfPresent(RGBA.self, forKey: .stroke))
            .flatMap { $0 } ?? .red
        fill = (try? c.decodeIfPresent(RGBA.self, forKey: .fill))
            .flatMap { $0 } ?? .clear
        width = (try? c.decodeIfPresent(Double.self, forKey: .width))
            .flatMap { $0 } ?? 4
        text = (try? c.decodeIfPresent(String.self, forKey: .text))
            .flatMap { $0 } ?? ""
        font = (try? c.decodeIfPresent(MarkupFont.self, forKey: .font))
            .flatMap { $0 } ?? MarkupFont()
        box = (try? c.decodeIfPresent(Bool.self, forKey: .box))
            .flatMap { $0 } ?? false
    }

    /// As the core takes it.
    var json: [String: Any] {
        var j: [String: Any] = [
            "id": id, "kind": kind.rawValue, "x0": x0, "y0": y0,
            "x1": x1, "y1": y1, "stroke": stroke.json, "fill": fill.json,
            "width": width,
        ]
        if kind == .text {
            j["text"] = text
            j["font"] = font.json
            j["box"] = box
        }
        return j
    }
}

/// A markup layer's content (core project::Markup): whether anything is
/// painted on it, and its objects.
struct MarkupDTO: Decodable, Sendable, Hashable {
    /// The painted raster's content hash; "" with nothing painted.
    var raster: String
    var objects: [MarkupObject]
    /// Its size (a markup asset's); 0: the frame it is drawn on.
    var w = 0
    var h = 0

    private enum K: String, CodingKey { case raster, objects, w, h }
    private struct Blob: Decodable { var hash: String? }

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: K.self)
        raster = (try? c.decodeIfPresent(Blob.self, forKey: .raster))
            .flatMap { $0?.hash } ?? ""
        // One object the app cannot read is dropped, not the list.
        var list: [MarkupObject] = []
        if var arr = try? c.nestedUnkeyedContainer(forKey: .objects) {
            while !arr.isAtEnd {
                if let o = try? arr.decode(MarkupObject.self) {
                    list.append(o)
                } else {
                    _ = try? arr.decode(Ignored.self)
                }
            }
        }
        objects = list
        w = (try? c.decodeIfPresent(Int.self, forKey: .w)).flatMap { $0 }
            ?? 0
        h = (try? c.decodeIfPresent(Int.self, forKey: .h)).flatMap { $0 }
            ?? 0
    }

    private struct Ignored: Decodable {}
}
