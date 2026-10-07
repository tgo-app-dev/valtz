// trace-mark -- measures the Valtz mark's edges from the painted artwork
// and writes them as Swift data (app/macos/Sources/Mark/MarkTrace.swift).
//
//   swiftc -O tools/valtz-icon/trace-mark.swift -o /tmp/trace-mark
//   /tmp/trace-mark app/macos/Resources/valtz.png \
//       > app/macos/Sources/Mark/MarkTrace.swift
//
// Why measure: the mark's look is its edges. Hand-placed points wobble;
// these do not.
//   * The SILHOUETTE is the artwork's alpha contour (marching squares at
//     alpha 0.5, sub-pixel), smoothed, and split into the named runs the
//     layers use at a few anchor points.
//   * INNER EDGES -- where one sheet crosses another -- are drawn in the
//     artwork as bright hairlines. Each is given as a rough guide (below)
//     and SNAPPED onto that hairline: every point moves along the curve's
//     normal to the brightest, least saturated pixel within reach, then
//     the curve is smoothed.
//   * The SPROCKET HOLES are the small alpha loops. Their centres are
//     projected onto one line at a constant distance from the strip's
//     measured edge, so the perforations run parallel to it.
// The output is resampled evenly (every ~6 px).

import CoreGraphics
import Foundation
import ImageIO
import simd

typealias P = SIMD2<Double>

// ---- the artwork ------------------------------------------------------------

let args = CommandLine.arguments
guard args.count > 1,
      let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil),
      let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    FileHandle.standardError.write(Data("usage: trace-mark <valtz.png>\n".utf8))
    exit(2)
}
let N = img.width
var rgba = [UInt8](repeating: 0, count: N * N * 4)
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
CGContext(data: &rgba, width: N, height: N, bitsPerComponent: 8, bytesPerRow: N * 4,
          space: cs, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    .draw(img, in: CGRect(x: 0, y: 0, width: N, height: N))
func alpha(_ x: Int, _ y: Int) -> Double {
    guard x >= 0, y >= 0, x < N, y < N else { return 0 }
    return Double(rgba[(y * N + x) * 4 + 3]) / 255
}
/// How much a pixel looks like the artwork's hairlines: bright and pale.
func whiteness(_ p: P) -> Double {
    let x = Int(p.x.rounded()), y = Int(p.y.rounded())
    guard x >= 0, y >= 0, x < N, y < N else { return 0 }
    let i = (y * N + x) * 4
    let a = Double(rgba[i + 3]) / 255
    guard a > 0.5 else { return 0 }
    let r = Double(rgba[i]) / 255 / a, g = Double(rgba[i + 1]) / 255 / a
    let b = Double(rgba[i + 2]) / 255 / a
    let mx = max(r, g, b), mn = min(r, g, b)
    let lum = 0.3 * r + 0.59 * g + 0.11 * b
    return lum * (1 - (mx > 0 ? (mx - mn) / mx : 0))
}

// ---- contours (marching squares at alpha 0.5) ---------------------------------

func contours() -> [[P]] {
    // Edge ids: horizontal (x,y)-(x+1,y) = 2*(y*N+x); vertical = +1.
    func point(_ e: Int) -> P {
        let c = e / 2, x = c % N, y = c / N
        let (a, b): (P, P) = e % 2 == 0 ? (P(Double(x), Double(y)), P(Double(x + 1), Double(y)))
                                         : (P(Double(x), Double(y)), P(Double(x), Double(y + 1)))
        let va = alpha(Int(a.x), Int(a.y)), vb = alpha(Int(b.x), Int(b.y))
        let f = abs(vb - va) > 1e-6 ? (0.5 - va) / (vb - va) : 0.5
        return a + (b - a) * min(max(f, 0), 1)
    }
    var next = [Int: [Int]]()
    func link(_ a: Int, _ b: Int) { next[a, default: []].append(b); next[b, default: []].append(a) }
    for y in 0..<(N - 1) {
        for x in 0..<(N - 1) {
            let tl = alpha(x, y) >= 0.5, tr = alpha(x + 1, y) >= 0.5
            let br = alpha(x + 1, y + 1) >= 0.5, bl = alpha(x, y + 1) >= 0.5
            let top = 2 * (y * N + x), bottom = 2 * ((y + 1) * N + x)
            let left = 2 * (y * N + x) + 1, right = 2 * (y * N + x + 1) + 1
            var crossed = [Int]()
            if tl != tr { crossed.append(top) }
            if tr != br { crossed.append(right) }
            if br != bl { crossed.append(bottom) }
            if bl != tl { crossed.append(left) }
            if crossed.count == 2 { link(crossed[0], crossed[1]) }
            if crossed.count == 4 { link(crossed[0], crossed[1]); link(crossed[2], crossed[3]) }
        }
    }
    var seen = Set<Int>()
    var loops = [[P]]()
    for start in next.keys where !seen.contains(start) {
        var loop = [Int]()
        var prev = -1, cur = start
        while !seen.contains(cur) {
            seen.insert(cur)
            loop.append(cur)
            guard let nb = next[cur]?.first(where: { $0 != prev && !seen.contains($0) }) else { break }
            prev = cur
            cur = nb
        }
        loops.append(loop.map(point))
    }
    return loops
}

func length(_ c: [P]) -> Double {
    zip(c, c.dropFirst()).reduce(0) { $0 + simd_length($1.1 - $1.0) }
}

/// Gaussian smoothing along a polyline (sigma in points); closed or open
/// (an open curve keeps its ends).
func smooth(_ c: [P], sigma: Double, closed: Bool) -> [P] {
    let r = Int(ceil(3 * sigma))
    let w = (-r...r).map { exp(-Double($0 * $0) / (2 * sigma * sigma)) }
    // An open curve is extended past its ends by point reflection, so its
    // ends are smoothed too (an unsmoothed end point kinks the curve).
    func at(_ j: Int) -> P {
        if closed { return c[(j % c.count + c.count) % c.count] }
        if j < 0 { return 2 * c[0] - c[min(-j, c.count - 1)] }
        if j >= c.count { return 2 * c[c.count - 1] - c[max(2 * (c.count - 1) - j, 0)] }
        return c[j]
    }
    return c.indices.map { i in
        var acc = P(0, 0), tot = 0.0
        for k in -r...r { acc += at(i + k) * w[k + r]; tot += w[k + r] }
        return acc / tot
    }
}

/// Points every `step` px along `c`.
func resample(_ c: [P], step: Double) -> [P] {
    var out = [c[0]]
    var carry = 0.0
    for (a, b) in zip(c, c.dropFirst()) {
        let d = simd_length(b - a)
        var s = step - carry
        while s <= d { out.append(a + (b - a) * (s / d)); s += step }
        carry = d - (s - step)
    }
    if simd_length(out.last! - c.last!) > step * 0.3 { out.append(c.last!) }
    return out
}

func nearest(_ c: [P], _ p: P) -> Int {
    c.indices.min(by: { simd_length(c[$0] - p) < simd_length(c[$1] - p) })!
}

/// The closed contour's run from `a` to `b` that passes nearest `via`.
func run(_ c: [P], from a: P, to b: P, via: P) -> [P] {
    let i = nearest(c, a), j = nearest(c, b), k = nearest(c, via)
    func walk(_ step: Int) -> [P] {
        var out = [P](), x = i
        while true { out.append(c[x]); if x == j { break }; x = (x + step + c.count) % c.count }
        return out
    }
    let fwd = walk(1), back = walk(-1)
    return fwd.contains(where: { simd_length($0 - c[k]) < 1 }) ? fwd : back
}

// ---- inner edges: snapped onto the artwork's hairlines ------------------------

func catmull(_ pts: [P], perSegment: Int = 12) -> [P] {
    var out = [P]()
    for i in 0..<(pts.count - 1) {
        let p0 = pts[max(i - 1, 0)], p1 = pts[i], p2 = pts[i + 1], p3 = pts[min(i + 2, pts.count - 1)]
        for s in 0..<perSegment {
            let f = Double(s) / Double(perSegment), f2 = f * f, f3 = f2 * f
            out.append(0.5 * (2 * p1 + (p2 - p0) * f + (2 * p0 - 5 * p1 + 4 * p2 - p3) * f2
                              + (3 * p1 - p0 - 3 * p2 + p3) * f3))
        }
    }
    out.append(pts.last!)
    return out
}

func snap(_ guide: [P], reach: Double, sigma: Double = 8) -> [P] {
    let c = resample(catmull(guide), step: 3)
    let snapped: [P] = c.indices.map { i in
        let a = c[max(i - 2, 0)], b = c[min(i + 2, c.count - 1)]
        let t = simd_normalize(b - a), n = P(-t.y, t.x)
        var best = c[i], bestScore = whiteness(c[i])
        var s = -reach
        while s <= reach {
            let q = c[i] + n * s
            let w = whiteness(q)
            if w > bestScore + 0.02 { best = q; bestScore = w }
            s += 0.5
        }
        return bestScore > 0.72 ? best : c[i]
    }
    // The ends stay where the guide put them -- they meet other edges --
    // but eased in over ~25 px, so the curve does not kink to reach them.
    var out = smooth(snapped, sigma: sigma, closed: false)
    let d0 = guide.first! - out[0], d1 = guide.last! - out[out.count - 1]
    for i in out.indices {
        let a = min(1.0, Double(i) * 3 / 40), b = min(1.0, Double(out.count - 1 - i) * 3 / 40)
        let ea = 1 - a * a * (3 - 2 * a), eb = 1 - b * b * (3 - 2 * b)
        out[i] += d0 * ea + d1 * eb
    }
    return resample(out, step: 6)
}

// ---- the mark -------------------------------------------------------------------

let loops = contours()
guard let outer = loops.max(by: { length($0) < length($1) }) else { exit(1) }
let silhouette = smooth(resample(outer, step: 1), sigma: 2.5, closed: true)

// Anchors on the silhouette (the artwork's 1024 px, y down). The crotch
// -- the V's inner vertex, where three sheets meet -- is MEASURED: the
// notch's lowest point, so every edge meeting there meets exactly.
let crotch: P = {
    let near = silhouette.filter { simd_length($0 - P(465, 630)) < 30 }
    return near.max(by: { $0.y < $1.y }) ?? P(465, 630)
}()
let tip = P(54, 297), crestEnd = P(990, 205)
let tailJoin = P(655, 675), base = P(495, 852)

let lipTop = resample(run(silhouette, from: tip, to: crotch, via: P(250, 207)), step: 6)
let stripOuter = resample(run(silhouette, from: crotch, to: crestEnd, via: P(880, 136)), step: 6)
let tailOuter = resample(run(silhouette, from: crestEnd, to: tailJoin, via: P(820, 380)), step: 6)
let stripLowerRight = resample(run(silhouette, from: tailJoin, to: base, via: P(580, 770)), step: 6)
let leftSilhouette = resample(run(silhouette, from: tip, to: base, via: P(237, 550)), step: 6)

// Inner edges: guides (hand-placed, rough) snapped onto the hairlines.

/// Lay `c` onto the outline by `weight(i)` (0 = where it is, 1 = on the
/// nearest outline point): for an edge that, in the artwork, BECOMES the
/// outline for a stretch. Weights ramp smoothly, so it joins tangentially.
func onto(_ c: [P], weight: (Int) -> Double) -> [P] {
    c.indices.map { i in
        let w = weight(i)
        guard w > 0 else { return c[i] }
        return c[i] + (project(onto: silhouette, c[i]) - c[i]) * w
    }
}

/// The closest point ON the closed polyline `c` (not merely its nearest
/// vertex, which would add half a pixel of jitter).
func project(onto c: [P], _ p: P) -> P {
    let k = nearest(c, p)
    var best = c[k], bd = simd_length(c[k] - p)
    for j in [k - 1, k] {
        let a = c[(j + c.count) % c.count], b = c[(j + 1) % c.count]
        let ab = b - a
        let t = min(max(simd_dot(p - a, ab) / max(simd_length_squared(ab), 1e-9), 0), 1)
        let q = a + ab * t
        if simd_length(q - p) < bd { best = q; bd = simd_length(q - p) }
    }
    return best
}

/// A penalised smoothing spline (P-spline): a cubic B-spline with control
/// points every `spacing` px, fitted to `c` by least squares with a
/// penalty on the control points' second differences -- `alpha` at each
/// point of `c`, so a curve can be stiff along a long sweep (no wobble)
/// and supple through a tight bend (not pulled inwards). The ends are
/// held.
func pspline(_ c: [P], spacing: Double, alpha: (P) -> Double) -> [P] {
    var s = [0.0]
    for (a, b) in zip(c, c.dropFirst()) { s.append(s.last! + simd_length(b - a)) }
    let L = s.last!
    let K = max(4, Int(L / spacing) + 3)
    let span = Double(K - 3)
    func basis(_ si: Double) -> (Int, [Double]) {
        let u = min(max(si / L * span, 0), span - 1e-9)
        let j = min(Int(u), K - 4), t = u - Double(j)
        let t2 = t * t, t3 = t2 * t
        return (j, [(1 - t) * (1 - t) * (1 - t) / 6, (3 * t3 - 6 * t2 + 4) / 6,
                    (-3 * t3 + 3 * t2 + 3 * t + 1) / 6, t3 / 6])
    }
    // Normal equations: (B^T W B + alpha D^T D) x = B^T W p.
    var M = [[Double]](repeating: [Double](repeating: 0, count: K), count: K)
    var rx = [Double](repeating: 0, count: K), ry = rx
    for i in c.indices {
        let w = (i == 0 || i == c.count - 1) ? 1e4 : 1.0
        let (j, b) = basis(s[i])
        for a in 0..<4 {
            rx[j + a] += w * b[a] * c[i].x; ry[j + a] += w * b[a] * c[i].y
            for d in 0..<4 { M[j + a][j + d] += w * b[a] * b[d] }
        }
    }
    for r in 0..<(K - 2) {                       // second differences
        // The stiffness where this control point acts on the curve.
        let si = min(max(Double(r) / span * L, 0), L)
        let k = min(s.firstIndex { $0 >= si } ?? (c.count - 1), c.count - 1)
        let al = alpha(c[k])
        let d = [1.0, -2.0, 1.0]
        for a in 0..<3 { for e in 0..<3 { M[r + a][r + e] += al * d[a] * d[e] } }
    }
    func solve(_ rhs: [Double]) -> [Double] {
        var A = M, x = rhs
        for col in 0..<K {                       // SPD: no pivoting needed
            for row in (col + 1)..<K where A[row][col] != 0 {
                let f = A[row][col] / A[col][col]
                for k in col..<K { A[row][k] -= f * A[col][k] }
                x[row] -= f * x[col]
            }
        }
        for row in stride(from: K - 1, through: 0, by: -1) {
            var v = x[row]
            for k in (row + 1)..<K { v -= A[row][k] * x[k] }
            x[row] = v / A[row][row]
        }
        return x
    }
    let cx = solve(rx), cy = solve(ry)
    return s.map { si in
        let (j, b) = basis(si)
        var p = P(0, 0)
        for a in 0..<4 { p += P(cx[j + a], cy[j + a]) * b[a] }
        return p
    }
}
/// Taubin smoothing: alternating shrink (lambda) and inflate (mu) steps
/// -- a low-pass filter on the curve's wiggle that, unlike a plain
/// Gaussian, does not pull tight bends inwards. The end points stay.
func taubin(_ c: [P], iterations: Int, lambda: Double = 0.5, mu: Double = -0.53) -> [P] {
    var p = c
    for _ in 0..<iterations {
        for k in [lambda, mu] {
            var q = p
            for i in 1..<(p.count - 1) {
                q[i] = p[i] + ((p[i - 1] + p[i + 1]) / 2 - p[i]) * k
            }
            p = q
        }
    }
    return p
}

func ramp(_ x: Double, _ a: Double, _ b: Double) -> Double {
    let t = min(max((x - a) / (b - a), 0), 1)
    return t * t * (3 - 2 * t)
}

// The S-curve, the back sheet's left edge. In the artwork that sheet IS
// the outline down the middle of the left stroke (y ~440-590), and at its
// end the curve does not stop at the base: past the point where the
// fold's right edge merges into it, it curls LEFT and runs into the
// bottom outline tangentially. Both stretches are laid onto the outline.
let sCurveSnapped = snap([P(190, 295), P(200, 335), P(210, 370), P(216, 400), P(222, 430),
                          P(228, 465), P(236, 500), P(245, 540), P(258, 580), P(272, 604),
                          P(296, 632), P(330, 660), P(370, 690), P(400, 705), P(430, 720),
                          P(455, 737), P(475, 755), P(490, 775), P(500, 795), P(503, 815),
                          P(500, 835), P(488, 850), P(470, 860), P(452, 864)],
                         reach: 6, sigma: 5)
let sCurve: [P] = {
    // The target: the snapped hairline, laid onto the outline over y
    // 445-560 on the stroke (lifting off gently, as the artwork does, from
    // ~560 over 70 px) and over its last ~30 px into the bottom outline.
    let h = resample(sCurveSnapped, step: 3)
    let n = h.count
    let target = onto(h) { i in
        let p = h[i]
        let stroke = p.x < 330 ? ramp(p.y, 410, 450) * (1 - ramp(p.y, 555, 625)) : 0
        let tail = ramp(Double(i), Double(n - 11), Double(n - 1))
        return max(stroke, tail)
    }
    // Then ONE smooth curve through all of it. The hairline wobbles by a
    // pixel or two over tens of pixels (the painting's texture) and the
    // outline by less. A P-spline with a control point every 32 px, stiff
    // along the long sweep and supple through the tight bend at the
    // bottom, keeps the S and its bends and nothing finer.
    var c = pspline(target, spacing: 32) { p in
        let bend = p.y > 700 && p.x > 420 ? ramp(p.y, 740, 790) : 0
        return 6 * (1 - bend) + 0.25 * bend
    }
    c[c.count - 1] = project(onto: silhouette, c[c.count - 1])
    return resample(c, step: 6)
}()
/// Where the fold's right edge merges into the S-curve: the fold's tip,
/// inside the U.
let foldTipIndex = nearest(sCurve, P(501, 832))
let foldTip = sCurve[foldTipIndex]
/// Where the S-curve's tail meets the bottom outline.
let bottomJoin = sCurve.last!

// The front sheet's one long edge -- under the lip, down the diagonal,
// down the fold to its tip -- is snapped as ONE curve and only then split
// where other sheets meet it, so its direction is continuous through
// those junctions.
let longEdgeSnapped = snap([P(54, 297), P(70, 275), P(90, 270), P(115, 267), P(140, 269),
                     P(165, 276), P(190, 290), P(240, 340), P(300, 400), P(340, 445),
                     P(370, 490), P(400, 530), P(425, 570), P(445, 600), crotch,
                     P(475, 655), P(490, 685), P(500, 710), P(507, 740), P(510, 770),
                     P(509, 795), P(505, 815), foldTip], reach: 8)
// It must pass EXACTLY through the crotch, where the stroke's outline and
// the strip's meet it; pull it there, eased over +-30 px.
let longEdge: [P] = {
    var e = longEdgeSnapped
    let k = nearest(e, crotch), delta = crotch - e[k]
    for i in e.indices {
        let d = min(1, Double(abs(i - k)) * 6 / 30)
        e[i] += delta * (1 - d * d * (3 - 2 * d))
    }
    return e
}()
let lipJoin = nearest(longEdge, P(190, 290)), crotchJoin = nearest(longEdge, crotch)
let lipInner = Array(longEdge[...lipJoin])
let diagonal = Array(longEdge[lipJoin...crotchJoin])
let foldRight = Array(longEdge[crotchJoin...])
let sCurveHead = Array(sCurve[...foldTipIndex])
let sCurveTail = Array(sCurve[foldTipIndex...])
let joinIndex = nearest(leftSilhouette, bottomJoin)
let leftToJoin = Array(leftSilhouette[...joinIndex]) + [bottomJoin]
let bottomRun = [bottomJoin] + Array(leftSilhouette[(joinIndex + 1)...])
let stripEdge = snap([P(655, 675), P(685, 630), P(700, 590), P(715, 555), P(735, 510),
                      P(760, 455), P(785, 400), P(800, 370), P(820, 320), P(840, 275),
                      P(860, 235), P(885, 200), P(910, 185), P(940, 180), P(970, 185),
                      P(990, 205)], reach: 12)

// Holes: the small loops; centres onto a line parallel to the strip edge.
let holeLoops = loops.filter { let l = length($0); return l > 50 && l < 200 }
    .filter { l in let c = l.reduce(P(0, 0), +) / Double(l.count); return c.x > 600 && c.y < 600 }
let centres = holeLoops.map { $0.reduce(P(0, 0), +) / Double($0.count) }
    .sorted { $0.y < $1.y }
let edgeDense = resample(stripEdge, step: 1)
func edgeFrame(_ p: P) -> (P, P, Double) {       // point, inward normal, distance
    let i = nearest(edgeDense, p)
    let a = edgeDense[max(i - 4, 0)], b = edgeDense[min(i + 4, edgeDense.count - 1)]
    let t = simd_normalize(b - a)
    var n = P(-t.y, t.x)
    if simd_dot(n, p - edgeDense[i]) < 0 { n = -n }
    return (edgeDense[i], n, simd_length(p - edgeDense[i]))
}
let meanDist = centres.map { edgeFrame($0).2 }.reduce(0, +) / Double(max(centres.count, 1))
let holes: [(P, P)] = centres.map { c in
    let (e, n, _) = edgeFrame(c)
    let i = nearest(edgeDense, e)
    let a = edgeDense[max(i - 6, 0)], b = edgeDense[min(i + 6, edgeDense.count - 1)]
    return (e + n * meanDist, simd_normalize(b - a))
}
// Shape: each hole is a rounded parallelogram -- two sides along the
// strip's edge, the other two along the hole's own cross direction (near
// horizontal low down, tilting where the strip turns towards the crest).
// The cross direction is the dominant edge angle of the hole's outline
// away from the edge direction; the size is measured in that frame.
let byY = holeLoops.sorted { a, b in
    (a.reduce(P(0, 0), +) / Double(a.count)).y < (b.reduce(P(0, 0), +) / Double(b.count)).y }
var holeShapes = [(P, P, P, P)]()          // centre, along d, across e, half (a, b)
for (l, h) in zip(byY, holes) {
    let c = l.reduce(P(0, 0), +) / Double(l.count), d = h.1
    let dAngle = atan2(d.y, d.x)
    var bins = [Double](repeating: 0, count: 180)
    let loop = smooth(resample(l + [l[0]], step: 1), sigma: 1.5, closed: true)
    for (a, b) in zip(loop, loop.dropFirst()) {
        let v = b - a, len = simd_length(v)
        guard len > 1e-6 else { continue }
        var ang = atan2(v.y, v.x)
        // Angle between this segment and the edge direction, mod pi.
        var rel = (ang - dAngle).truncatingRemainder(dividingBy: .pi)
        if rel < 0 { rel += .pi }
        if rel < 0.35 || rel > .pi - 0.35 { continue }     // an along-side
        ang = ang.truncatingRemainder(dividingBy: .pi); if ang < 0 { ang += .pi }
        bins[min(179, Int(ang / .pi * 180))] += len
    }
    // Peak of the length-weighted angle histogram, lightly smoothed.
    var best = 0, bestW = -1.0
    for i in 0..<180 {
        let w = bins[(i + 179) % 180] + 2 * bins[i] + bins[(i + 1) % 180]
        if w > bestW { bestW = w; best = i }
    }
    let ea = (Double(best) + 0.5) / 180 * .pi
    let e = P(cos(ea), sin(ea))
    // Extents in the (e, d) frame: q = a e + b d.
    let det = e.x * d.y - e.y * d.x
    var amin = 1e9, amax = -1e9, bmin = 1e9, bmax = -1e9
    for q0 in l {
        let q = q0 - c
        let a = (q.x * d.y - q.y * d.x) / det
        let b = (e.x * q.y - e.y * q.x) / det
        amin = min(amin, a); amax = max(amax, a); bmin = min(bmin, b); bmax = max(bmax, b)
    }
    holeShapes.append((h.0, d, e, P((amax - amin) / 2, (bmax - bmin) / 2)))
}

// ---- output --------------------------------------------------------------------

func emit<C: Collection>(_ name: String, _ doc: String, _ c: C) where C.Element == P {
    print("    /// \(doc)")
    print("    static let \(name): [SIMD2<Float>] = [")
    var line = "       "
    for p in c {
        let item = String(format: " [%.1f, %.1f],", p.x, p.y)
        if line.count + item.count > 78 { print(line); line = "       " }
        line += item
    }
    print(line)
    print("    ]")
}
print("""
// GENERATED by tools/valtz-icon/trace-mark.swift from the painted
// artwork (app/macos/Resources/valtz.png) -- do not edit; re-run it.
//
// The Valtz mark's edges in the artwork's 1024 px space (y down): the
// silhouette runs measured from its alpha contour, the inner edges
// snapped onto its hairlines, and the sprocket holes on a line parallel
// to the strip's edge. MarkGeometry.swift composes the layers from them.

enum MarkTrace {
""")
emit("silhouette", "The whole outline, closed (a backplate under the sheets).",
     resample(silhouette + [silhouette[0]], step: 6).dropLast())
emit("lipTop", "Lip tip, over the lip, down the stroke's right side to the crotch.", lipTop)
emit("leftSilhouette", "Lip tip, under the curl, down the left side and round the base.", leftSilhouette)
emit("stripOuter", "Crotch, up the strip's outer edge and over its crest.", stripOuter)
emit("tailOuter", "Crest, down the curled end's outer edge to where it meets the strip.", tailOuter)
emit("stripLowerRight", "Down the strip's lower right edge into the base.", stripLowerRight)
emit("lipInner", "The underside of the lip (a hairline).", lipInner)
emit("diagonal", "The front sheet's edge down the stroke (a hairline).", diagonal)
emit("foldRight", "The fold's right edge, crotch to the fold's tip (a hairline).", foldRight)
emit("sCurve", "The back sheet's left edge, down the S-curve to the fold's tip.", sCurveHead)
emit("sCurveTail", "The S-curve past the fold's tip, curling into the bottom outline.", sCurveTail)
emit("leftToJoin", "The left outline, lip tip to where the S-curve's tail meets it.", leftToJoin)
emit("bottomRun", "The bottom outline from that point to the base.", bottomRun)
emit("stripEdge", "The strip's right edge against its curled end (a hairline).", stripEdge)
print("    /// Sprocket holes, top to bottom: centre xy and the strip edge's")
print("    /// direction xy (the along-sides are parallel to it) ...")
print("    static let holes: [SIMD4<Float>] = [")
for h in holeShapes {
    print(String(format: "        [%.1f, %.1f, %.4f, %.4f],", h.0.x, h.0.y, h.1.x, h.1.y))
}
print("    ]")
print("    /// ... and each hole's cross direction xy and half size along it")
print("    /// and along the edge.")
print("    static let holeShapes: [SIMD4<Float>] = [")
for h in holeShapes {
    print(String(format: "        [%.4f, %.4f, %.1f, %.1f],", h.2.x, h.2.y, h.3.x, h.3.y))
}
print("    ]")
print("}")
FileHandle.standardError.write(Data(String(format:
    "silhouette %.0f px; crotch (%.1f, %.1f); fold tip (%.1f, %.1f); bottom join (%.1f, %.1f); %d holes at %.1f px from the edge\n",
    length(silhouette), crotch.x, crotch.y, foldTip.x, foldTip.y, bottomJoin.x, bottomJoin.y,
    holes.count, meanDist).utf8))
