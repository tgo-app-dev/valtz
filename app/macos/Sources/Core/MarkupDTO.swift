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

/// An effect drawn with a markup object (core media/markup.h): a drop
/// SHADOW -- its colour, blurred `blur` canvas pixels, `dx` right and
/// `dy` down -- or a GLOW, a halo `radius` pixels out.
struct MarkupEffect: Codable, Equatable, Hashable, Sendable {
    enum Kind: String, Codable, Sendable { case shadow, glow }
    var kind: Kind
    var color: RGBA
    var blur: Double = 0
    var dx: Double = 0
    var dy: Double = 0
    var radius: Double = 0

    init(kind: Kind, color: RGBA, blur: Double = 0, dx: Double = 0,
         dy: Double = 0, radius: Double = 0) {
        (self.kind, self.color) = (kind, color)
        (self.blur, self.dx, self.dy, self.radius) = (blur, dx, dy, radius)
    }

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        kind = try c.decode(Kind.self, forKey: .kind)
        color = (try? c.decodeIfPresent(RGBA.self, forKey: .color))
            .flatMap { $0 } ?? RGBA(r: 0, g: 0, b: 0, a: 0.5)
        func num(_ k: CodingKeys) -> Double {
            (try? c.decodeIfPresent(Double.self, forKey: k)).flatMap { $0 }
                ?? 0
        }
        (blur, dx, dy, radius) = (num(.blur), num(.dx), num(.dy),
                                  num(.radius))
    }

    /// As the core takes it: a kind's own fields.
    var json: [String: Any] {
        switch kind {
        case .shadow:
            ["kind": "shadow", "color": color.json, "blur": blur, "dx": dx,
             "dy": dy]
        case .glow:
            ["kind": "glow", "color": color.json, "radius": radius]
        }
    }
}

/// A GRADIENT (core media/markup.h): a shape's inside, or a text box's
/// words, in place of their flat colour -- LINEAR from `from` to `to`, or
/// RADIAL round `from` out to `to`, both in the object's box's units (0
/// at (x0, y0), 1 at (x1, y1)), so it moves and scales with the object;
/// two or more stops, each a colour at 0...1 along it.
struct MarkupGradient: Codable, Equatable, Hashable, Sendable {
    enum Kind: String, Codable, Sendable { case linear, radial }
    struct Stop: Codable, Equatable, Hashable, Sendable {
        var at: Double
        var color: RGBA
    }
    var kind: Kind
    var stops: [Stop]
    var from: CGPoint
    var to: CGPoint

    init(kind: Kind, stops: [Stop], from: CGPoint? = nil,
         to: CGPoint? = nil) {
        (self.kind, self.stops) = (kind, stops)
        self.from = from ?? (kind == .radial ? CGPoint(x: 0.5, y: 0.5)
                                             : CGPoint(x: 0, y: 0.5))
        // Not on a box's corner: its handle would cover the corner's.
        self.to = to ?? CGPoint(x: 1, y: 0.5)
    }

    private enum CodingKeys: String, CodingKey { case kind, stops, from, to }

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        let k = (try? c.decode(Kind.self, forKey: .kind)) ?? .linear
        let st = (try? c.decode([Stop].self, forKey: .stops)) ?? []
        func point(_ key: CodingKeys) -> CGPoint? {
            guard let v = try? c.decode([Double].self, forKey: key),
                  v.count == 2 else { return nil }
            return CGPoint(x: v[0], y: v[1])
        }
        self.init(kind: k, stops: st.sorted { $0.at < $1.at },
                  from: point(.from), to: point(.to))
    }

    func encode(to e: Encoder) throws {
        var c = e.container(keyedBy: CodingKeys.self)
        try c.encode(kind, forKey: .kind)
        try c.encode(stops, forKey: .stops)
        try c.encode([from.x, from.y], forKey: .from)
        try c.encode([to.x, to.y], forKey: .to)
    }

    /// As the core takes it.
    var json: [String: Any] {
        ["kind": kind.rawValue,
         "stops": stops.map { ["at": $0.at, "color": $0.color.json] },
         "from": [from.x, from.y], "to": [to.x, to.y]]
    }

    /// The colour at `t` along it, as Core Graphics blends between stops.
    func color(at t: Double) -> RGBA {
        let s = stops.sorted { $0.at < $1.at }
        guard let first = s.first, let last = s.last else {
            return .clear
        }
        if t <= first.at { return first.color }
        if t >= last.at { return last.color }
        for (a, b) in zip(s, s.dropFirst()) where t <= b.at {
            let f = b.at > a.at ? (t - a.at) / (b.at - a.at) : 0
            return RGBA(r: a.color.r + (b.color.r - a.color.r) * f,
                        g: a.color.g + (b.color.g - a.color.g) * f,
                        b: a.color.b + (b.color.b - a.color.b) * f,
                        a: a.color.a + (b.color.a - a.color.a) * f)
        }
        return last.color
    }
}

/// A CONNECTOR (core media/markup.h): a point on an object's outline
/// that another line's or path's points anchor to -- on segment `seg`
/// (a line's one, a rectangle's four from (x0, y0) clockwise, a path's
/// pieces; an ellipse's one) at `t` along it (an ellipse's a fraction of
/// a turn from its right, clockwise).
struct MarkupConnector: Codable, Equatable, Hashable, Sendable {
    var id: String
    var seg: Int
    var t: Double

    var json: [String: Any] { ["id": id, "seg": seg, "t": t] }
}

/// An ANCHORED point (core media/markup.h): a line's end -- 0 its start,
/// 1 its end -- or a path's point `point`, lying on connector `connector`
/// of object `to`, another of its markup's: where that one goes, it goes.
struct MarkupAnchor: Codable, Equatable, Hashable, Sendable {
    var point: Int
    var to: String
    var connector: String

    var json: [String: Any] {
        ["point": point, "to": to, "connector": connector]
    }
}

/// A vector object on a markup layer (core media/markup.h): canvas
/// pixels, top-left origin. A line runs (x0, y0) -> (x1, y1); a rectangle
/// or ellipse fills that box; a text starts at (x0, y0), its top-left.
struct MarkupObject: Codable, Equatable, Hashable, Sendable, Identifiable {
    enum Kind: String, Codable, Sendable {
        case line, rect, ellipse, polygon, text
        /// A DRAWING lifted off the raster (core media/markup.h): its
        /// pixels a picture in its box, moved as objects are, laid back
        /// into the raster when the selection lets it go.
        case drawing
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
    /// A text box's PARAGRAPH style, kept as the core has it (nil: none,
    /// as before): "left" | "center" | "right" | "justified"; its lines
    /// this many of the font's line apart; pixels after each paragraph;
    /// its last line stretched too (a paragraph running on in another
    /// box).
    var align: String?
    var lineSpacing: Double?
    var paragraphSpacing: Double?
    var justifyLast = false
    /// Its effects, in order (the first innermost); a kind this Valtz
    /// does not know is not read.
    var effects: [MarkupEffect] = []
    /// A POLYGON's corners, closed, in its box's units (0 at (x0, y0), 1
    /// at (x1, y1)): they move and scale with the box.
    var points: [CGPoint] = []
    /// A PATH (core media/markup.h): a polygon whose points marked here
    /// are SMOOTH -- the curve passes through them without a turn, the
    /// curvature pen's -- the rest corners (empty: all); and whether it
    /// is OPEN, a curve with no inside.
    var smooth: [Bool] = []
    var open = false
    /// A line's or outline's DASHES, "dashed" or "dotted" (nil: solid),
    /// and a line's ARROWHEADS, "start", "end" or "both" (nil: none) --
    /// core media/markup.h.
    var dash: String?
    var arrows: String?
    /// Its TURN, degrees (+ clockwise), and FLIPS, about its box's centre
    /// -- drawn in its own pixels, flipped, then turned (core
    /// media/markup.h "rotate", "flip_x", "flip_y"; Information's
    /// rotation and its negative width and height).
    var rotate = 0.0
    var flipX = false
    var flipY = false
    /// A gradient in place of a shape's fill or a text's colour.
    var gradient: MarkupGradient?
    /// A lifted drawing's picture: its blob, as the core keeps it.
    var image: DrawingImage?
    /// Its CONNECTORS (a line, a rectangle, an ellipse, a path) and its
    /// points ANCHORED to others' (a line, a path) -- core media/markup.h.
    var connectors: [MarkupConnector] = []
    var anchors: [MarkupAnchor] = []
    /// A shape's WORDS' colour (its `text`, `font`, `align` and
    /// `lineSpacing` as a text box's): nil, black.
    var textColor: RGBA?

    /// Shapes with an inside: a fill or a gradient goes there (an open
    /// path has none).
    var fillable: Bool {
        kind == .rect || kind == .ellipse || (kind == .polygon && !open)
    }

    /// It holds WORDS: a text, or a shape with an inside (core
    /// media/markup.h: a rectangle's, an ellipse's, a closed path's).
    var takesWords: Bool { kind == .text || fillable }

    /// A shape holding words now.
    var shapeWithWords: Bool { fillable && !text.isEmpty }

    /// It takes connectors; its points take anchors.
    var takesConnectors: Bool {
        kind == .line || kind == .rect || kind == .ellipse
            || kind == .polygon
    }
    var takesAnchors: Bool { kind == .line || kind == .polygon }

    /// A line or an outline: it takes dashes (a text, a drawing none).
    var dashable: Bool {
        kind == .line || kind == .rect || kind == .ellipse
            || kind == .polygon
    }

    /// Point `i` of a path is smooth.
    func isSmooth(_ i: Int) -> Bool { i < smooth.count && smooth[i] }

    // Plain names: the core's snake_case keys ("line_spacing") reach them
    // through the decoder's convertFromSnakeCase (Core/DTO.swift) -- a
    // key spelled "line_spacing" here matched nothing, and every edit in
    // the app wrote a flowed text's spacing away.
    private enum CodingKeys: String, CodingKey {
        case id, kind, x0, y0, x1, y1, stroke, fill, width, text, font, box
        case align, effects, lineSpacing, paragraphSpacing, justifyLast
        case points, smooth, open, gradient, image, dash, arrows
        case rotate, flipX, flipY, connectors, anchors, textColor
    }

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
        align = (try? c.decodeIfPresent(String.self, forKey: .align))
            .flatMap { $0 }
        lineSpacing = (try? c.decodeIfPresent(Double.self,
                                              forKey: .lineSpacing))
            .flatMap { $0 }
        paragraphSpacing = (try? c.decodeIfPresent(
            Double.self, forKey: .paragraphSpacing)).flatMap { $0 }
        justifyLast = (try? c.decodeIfPresent(Bool.self,
                                              forKey: .justifyLast))
            .flatMap { $0 } ?? false
        points = ((try? c.decodeIfPresent([[Double]].self,
                                          forKey: .points)) ?? nil)?
            .compactMap { $0.count == 2 ? CGPoint(x: $0[0], y: $0[1]) : nil }
            ?? []
        smooth = ((try? c.decodeIfPresent([Bool].self, forKey: .smooth))
            ?? nil) ?? []
        open = ((try? c.decodeIfPresent(Bool.self, forKey: .open)) ?? nil)
            ?? false
        gradient = (try? c.decodeIfPresent(MarkupGradient.self,
                                           forKey: .gradient)) ?? nil
        image = (try? c.decodeIfPresent(DrawingImage.self, forKey: .image))
            ?? nil
        dash = ((try? c.decodeIfPresent(String.self, forKey: .dash)) ?? nil)
            .flatMap { $0 == "dashed" || $0 == "dotted" ? $0 : nil }
        arrows = ((try? c.decodeIfPresent(String.self, forKey: .arrows))
            ?? nil).flatMap {
                ["start", "end", "both"].contains($0) ? $0 : nil
            }
        rotate = num(.rotate)
        if !rotate.isFinite { rotate = 0 }
        flipX = ((try? c.decodeIfPresent(Bool.self, forKey: .flipX)) ?? nil)
            ?? false
        flipY = ((try? c.decodeIfPresent(Bool.self, forKey: .flipY)) ?? nil)
            ?? false
        if gradient.map({ $0.stops.count < 2 }) ?? false { gradient = nil }
        connectors = ((try? c.decodeIfPresent([MarkupConnector].self,
                                              forKey: .connectors))
            ?? nil) ?? []
        anchors = ((try? c.decodeIfPresent([MarkupAnchor].self,
                                           forKey: .anchors)) ?? nil) ?? []
        textColor = (try? c.decodeIfPresent(RGBA.self, forKey: .textColor))
            ?? nil
        if var arr = try? c.nestedUnkeyedContainer(forKey: .effects) {
            while !arr.isAtEnd {
                if let e = try? arr.decode(MarkupEffect.self) {
                    effects.append(e)
                } else {
                    _ = try? arr.decode(Skipped.self)
                }
            }
        }
    }

    private struct Skipped: Decodable {}

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
            if let align { j["align"] = align }
            if let lineSpacing { j["line_spacing"] = lineSpacing }
            if let paragraphSpacing {
                j["paragraph_spacing"] = paragraphSpacing
            }
            if justifyLast { j["justify_last"] = true }
        } else if fillable && !text.isEmpty {
            // A shape's words, set in it.
            j["text"] = text
            j["font"] = font.json
            if let align { j["align"] = align }
            if let lineSpacing { j["line_spacing"] = lineSpacing }
            if let textColor { j["text_color"] = textColor.json }
        }
        if takesConnectors && !connectors.isEmpty {
            j["connectors"] = connectors.map(\.json)
        }
        if takesAnchors && !anchors.isEmpty {
            j["anchors"] = anchors.map(\.json)
        }
        if !effects.isEmpty { j["effects"] = effects.map(\.json) }
        if kind == .polygon {
            j["points"] = points.map { [$0.x, $0.y] }
            if smooth.contains(true) {
                j["smooth"] = points.indices.map { isSmooth($0) }
            }
            if open { j["open"] = true }
        }
        if let gradient, kind != .line && kind != .drawing {
            j["gradient"] = gradient.json
        }
        if kind == .drawing, let image { j["image"] = image.json }
        if let dash, kind != .text && kind != .drawing { j["dash"] = dash }
        if let arrows, kind == .line { j["arrows"] = arrows }
        if rotate != 0 { j["rotate"] = rotate }
        if flipX { j["flip_x"] = true }
        if flipY { j["flip_y"] = true }
        return j
    }
}

/// A lifted drawing's picture, as the core names its blob (project::
/// BlobRef): kept as it came, sent back whole -- an object without it is
/// dropped by the core.
struct DrawingImage: Codable, Hashable, Sendable {
    var hash: String
    var size: Int
    var ext: String

    var json: [String: Any] { ["hash": hash, "size": size, "ext": ext] }
}

/// A markup layer's content (core project::Markup): whether anything is
/// painted on it, and its objects -- with the asset list only up to a few
/// hundred (the bridge's assets_json): past that `stamp` and `count`
/// alone, the objects the app's own copy of them (AppModel.markupCopies).
struct MarkupDTO: Decodable, Sendable, Hashable {
    /// The painted raster's content hash; "" with nothing painted.
    var raster: String
    var objects: [MarkupObject]
    /// Its size (a markup asset's); 0: the frame it is drawn on.
    var w = 0
    var h = 0
    /// Its objects' stamp (the core's objects_stamp) and how many; and
    /// whether they came with it.
    var stamp = ""
    var count = 0
    var inline = true

    private enum K: String, CodingKey {
        case raster, objects, w, h, stamp, count
    }
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
        inline = c.contains(.objects)
        stamp = (try? c.decodeIfPresent(String.self, forKey: .stamp))
            .flatMap { $0 } ?? ""
        count = (try? c.decodeIfPresent(Int.self, forKey: .count))
            .flatMap { $0 } ?? list.count
        w = (try? c.decodeIfPresent(Int.self, forKey: .w)).flatMap { $0 }
            ?? 0
        h = (try? c.decodeIfPresent(Int.self, forKey: .h)).flatMap { $0 }
            ?? 0
    }

    private struct Ignored: Decodable {}
}
