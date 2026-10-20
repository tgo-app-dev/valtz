import AppKit
import AVKit
import Carbon.HIToolbox
import ImageIO
import ScreenCaptureKit
import SwiftUI
import UniformTypeIdentifiers

@main
struct ValtzApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model = AppModel()

    /// The selection's drawings, for the hook's "dsel": the one selected
    /// on the raster (its box), the lifted ones (theirs), the core's
    /// drawing at a point ("x,y").
    @MainActor
    static func drawingSummary(_ m: AppModel, _ arg: String) -> String {
        let mk = m.markup
        func box(_ r: CGRect?) -> String {
            guard let r else { return "-" }
            let x = Int(r.minX), y = Int(r.minY)
            let w = Int(r.width), h = Int(r.height)
            return "\(x),\(y),\(w)x\(h)"
        }
        let lifted: [String] = mk.selection.filter { $0.kind == .drawing }
            .map { box(MarkupRender.box($0)) }
        var at = "-"
        let n: [Double] = arg.split(separator: ",").compactMap { Double($0) }
        let layer: String? = mk.selectionLayer ?? m.markupTargetPreview?.id
        if n.count == 2, let layer, let core = m.core,
           let pid = m.projectId, let pic = m.markupAsset {
            let r = core.layerOp(project: pid,
                                 asset: m.composedTarget(pic.id),
                                 "drawing-at", layer: layer,
                                 extra: ["x": n[0], "y": n[1], "reach": 4])
            let v: [Int] = ["x", "y", "w", "h"].map {
                (r[$0] as? NSNumber)?.intValue ?? -1
            }
            at = "\(v[0]),\(v[1]),\(v[2])x\(v[3])"
        }
        let drawing: String = mk.drawing ? box(mk.drawingBox) : "-"
        let liftedText: String = lifted.isEmpty ? "-"
            : lifted.joined(separator: "+")
        let undo: String = m.document.undo ?? "-"
        let redo: String = m.document.redo ?? "-"
        return "drawing=\(drawing) lifted=\(liftedText) objects=\(mk.selection.count) at=\(at) undo=\(undo) redo=\(redo)"
    }

    /// A suggestion applied, for the hooks: the box's prompt -- enhanced
    /// or not -- the words before it kept (its upstream), the prompts in
    /// Assets, the project's undo.
    @MainActor
    static func appliedSummary(_ m: AppModel) -> String {
        let id: String = m.promptAssetId ?? "-"
        let a: AssetDTO? = m.assets.first { $0.id == id }
        let up: AssetDTO? = a.flatMap { m.promptUpstream(of: $0) }
        let enhanced: Bool = a?.isEnhancedPrompt ?? false
        let before: String = String((up?.text ?? "-").prefix(60))
        let prompts: Int = m.assets.filter(\.isPrompt).count
        let undo: String = m.document.undo ?? "-"
        return "prompt=\(id) enhanced=\(enhanced) upstream=\(up?.id ?? "-") before=\(before) prompts=\(prompts) undo=\(undo)"
    }

    /// One VALTZ_SNAPSHOT_MARKUP_OPS step, as the toolbar and the
    /// stage's pointer would do it.
    @MainActor
    static func markupStep(_ m: AppModel, _ step: String) async {
        let kv = step.split(separator: "=", maxSplits: 1).map(String.init)
        let arg = kv.count > 1 ? kv[1] : ""
        let mk = m.markup
        func points(_ s: String) -> [CGPoint] {
            s.split(separator: ";").compactMap { p in
                let xy = p.split(separator: ",").compactMap {
                    Double($0.trimmingCharacters(in: .whitespaces))
                }
                return xy.count == 2 ? CGPoint(x: xy[0], y: xy[1]) : nil
            }
        }
        func rgba(_ s: String) -> RGBA? {
            let h = s.hasPrefix("#") ? String(s.dropFirst()) : s
            guard h.count == 8, let v = UInt32(h, radix: 16) else { return nil }
            return RGBA(r: Double(v >> 24 & 0xff) / 255,
                        g: Double(v >> 16 & 0xff) / 255,
                        b: Double(v >> 8 & 0xff) / 255,
                        a: Double(v & 0xff) / 255)
        }
        func pointer(_ phase: MarkupPointer.Phase, _ p: CGPoint,
                     clicks: Int = 1, shift: Bool = false,
                     option: Bool = false) {
            m.markupPointer(MarkupPointer(phase: phase, point: p,
                                          shift: shift, option: option,
                                          clickCount: clicks,
                                          pixelsPerPoint: 1))
        }
        // A colour as the hooks print it: #rrggbbaa.
        func hex(_ c: RGBA?) -> String {
            guard let c else { return "-" }
            return "#" + [c.r, c.g, c.b, c.a].map {
                String(format: "%02x", Int(($0 * 255).rounded()))
            }.joined()
        }
        switch kv[0] {
        // The PATH selected -- or the one the pen is drawing: its points
        // (canvas pixels, `~` smooth), open or closed, and the undo
        // step: "path".
        case "path":
            let o = mk.selection.first { $0.kind == .polygon }
            let pts = o.map(MarkupRender.polygonPoints) ?? mk.pen
            let pointsText = pts.enumerated().map { i, p in
                let smooth = o.map { $0.isSmooth(i) }
                    ?? (i < mk.penSmooth.count && mk.penSmooth[i])
                return (smooth ? "~" : "")
                    + String(format: "%.0f,%.0f", p.x, p.y)
            }.joined(separator: ";")
            let b = o.map { MarkupRender.bounds($0) }.map {
                String(format: "%.0f,%.0f,%.0fx%.0f", $0.minX, $0.minY,
                       $0.width, $0.height)
            } ?? "-"
            print("snapshot: path drawing=\(mk.pen.count) selected=\(o?.id ?? "-") open=\(o?.open ?? true) points=\(pointsText.isEmpty ? "-" : pointsText) bounds=\(b) fill=\(hex(o?.fill)) tool=\(mk.tool.rawValue) undo=\(m.document.undo ?? "-")")
        // The pointer leaving the picture -- a real exited event handed to
        // the stage (or a clip's markup view), as its tracking area sends
        // one: "exit". (Its click count read once aborted Valtz.)
        case "exit":
            let target: NSView? = m.markupOnClip
                ? ClipMarkupNSView.shown : CompareCanvas.shown
            if let v = target, let w = v.window,
               let e = NSEvent.enterExitEvent(
                   with: .mouseExited, location: .zero, modifierFlags: [],
                   timestamp: 0, windowNumber: w.windowNumber, context: nil,
                   eventNumber: 0, trackingNumber: 0, userData: nil) {
                v.mouseExited(with: e)
            }
            print("snapshot: exit view=\(target.map { "\(type(of: $0))" } ?? "-") sample=\(mk.sampleAt == nil ? "-" : "shown")")
        // The colours: the toolbar's Color and Fill, the selection's, the
        // eyedropper's sample where it is: "colors".
        case "colors":
            let sel = mk.selection.map {
                "\($0.id):\(hex($0.stroke))/\(hex($0.fill))"
            }.joined(separator: ",")
            print("snapshot: colors edge=\(hex(mk.edge)) fill=\(hex(mk.fill)) selection=\(sel.isEmpty ? "-" : sel) sample=\(hex(mk.sampleColor)) at=\(mk.sampleAt.map { String(format: "%.0f,%.0f", $0.x, $0.y) } ?? "-") tool=\(mk.tool.rawValue)")
        // The magnet (Snap to Guides): "magnet=on|off".
        case "magnet":
            m.snapsToGuides = arg != "off"
            print("snapshot: magnet on=\(m.snapsToGuides)")
        // The selection's DRAWINGS: "dsel" prints the drawing selected on
        // the raster (its box), the lifted ones among the objects (their
        // boxes), and the core's drawing at a point ("dsel=200,400").
        case "dsel":
            print("snapshot: dsel \(arg) " + drawingSummary(m, arg))
        // Information's position and size typed: "frame=x,y,w,h" (the
        // stage's pixels); "where" prints the selection's frame.
        case "frame", "where":
            let n = arg.split(separator: ",").compactMap { Double($0) }
            if kv[0] == "frame", n.count == 4 {
                m.setSelectionFrame(CGRect(x: n[0], y: n[1], width: n[2],
                                           height: n[3]))
            }
            let f = m.selectionFrame.map {
                String(format: "%.1f,%.1f,%.1fx%.1f", $0.minX, $0.minY,
                       $0.width, $0.height)
            } ?? "-"
            print("snapshot: \(kv[0]) \(arg) selection=\(f) objects=\(mk.selection.count) undo=\(m.document.undo ?? "-")")
        case "tool":
            if let t = MarkupTool(rawValue: arg) {
                mk.tool = t
                m.markupToolChanged()
            }
        case "size": mk.radius = Double(arg) ?? mk.radius
        case "soft": mk.softness = Double(arg) ?? mk.softness
        case "width":
            let w = Double(arg) ?? mk.width
            mk.width = w
            m.editSelection { if $0.kind != .text { $0.width = w } }
        case "color":
            if let c = rgba(arg) {
                mk.edge = c
                m.editSelection { $0.stroke = c }
            }
        case "fill":
            if let c = rgba(arg) {
                mk.fill = c
                m.editSelection {
                    if $0.kind == .rect || $0.kind == .ellipse { $0.fill = c }
                }
            }
        case "font":
            // "Family,48,bold,italic,underline"
            let p = arg.split(separator: ",").map(String.init)
            var f = mk.font
            if let fam = p.first, !fam.isEmpty { f.family = fam }
            if p.count > 1, let s = Double(p[1]) { f.size = s }
            f.bold = p.contains("bold")
            f.italic = p.contains("italic")
            f.underline = p.contains("underline")
            mk.font = f
            m.editSelection { if $0.kind == .text { $0.font = f } }
        // A text box's alignment and line spacing, as the toolbar sets
        // them: "align=center", "spacing=1.5".
        case "align":
            mk.align = arg
            m.editSelection {
                if $0.kind == .text { $0.align = arg == "left" ? nil : arg }
            }
        case "spacing":
            let v = min(3, max(1, Double(arg) ?? 1))
            mk.lineSpacing = v
            m.editSelection {
                if $0.kind == .text { $0.lineSpacing = v == 1 ? nil : v }
            }
        // Effects, as the Effects panel sets them: "shadow=off",
        // "shadow=#rrggbbaa,blur,dx,dy", "glow=off", "glow=#rrggbbaa,radius".
        // The Effects panel opened, as its button opens it.
        case "effects":
            mk.effectsOpen = arg != "close"
        // ARRANGE, as its panel does: "arrange=to-top" (up, down,
        // to-bottom, left, center, right, top, middle, bottom,
        // distribute-h, distribute-v), or "open" / "close" the panel;
        // prints what it could do, the layer's order and where the
        // selected objects are.
        case "arrange":
            if arg == "open" || arg == "close" {
                mk.arrangeOpen = arg == "open"
                return
            }
            guard let how = MarkupArrange(rawValue: arg) else { return }
            let can = MarkupArrange.allCases.filter { m.markupCanArrange($0) }
            m.arrangeSelection(how)
            let layer = mk.selectionLayer
            let objects = m.markupAsset?.layerStack
                .first { $0.id == layer }?.markup?.objects ?? []
            let at = mk.selection.map { o in
                let b = MarkupRender.bounds(o)
                return String(format: "%@@%.0f,%.0f", o.id, b.minX, b.minY)
            }
            print("snapshot: arrange \(arg) can=\(can.map(\.rawValue).joined(separator: ",")) order=\(objects.map(\.id).joined(separator: ",")) selected=\(at.joined(separator: ";")) undo=\(m.undoMenuTitle.replacingOccurrences(of: " ", with: "_"))")
        // A GRADIENT, as its editor sets it: "gradient=linear|radial|none"
        // (or "open" / "close" the editor); "stops=#rrggbbaa@0;#..@1" its
        // colours; "stop=add:0.5", "stop=move:1:0.3", "stop=remove:1",
        // "stop=color:1:#rrggbbaa"; "gradfrom=u,v" / "gradto=u,v" its
        // ends in the box's units (as dragged on the picture).
        case "gradient":
            switch arg {
            case "open": mk.gradientOpen = true
            case "close": mk.gradientOpen = false
            default:
                let kind = MarkupGradient.Kind(rawValue: arg)
                m.setMarkupGradient(kind.map { k in
                    m.markupGradient.map {
                        MarkupGradient(kind: k, stops: $0.stops)
                    } ?? AppModel.newGradient(k, from: m.markupGradientBase)
                })
            }
        case "stops":
            guard var g = m.markupGradient else { return }
            g.stops = arg.split(separator: ";").compactMap { st in
                let p = st.split(separator: "@").map(String.init)
                guard p.count == 2, let c = rgba(p[0]),
                      let at = Double(p[1]) else { return nil }
                return MarkupGradient.Stop(at: at, color: c)
            }
            if g.stops.count >= 2 { m.setMarkupGradient(g) }
        case "stop":
            guard var g = m.markupGradient else { return }
            let p = arg.split(separator: ":").map(String.init)
            let i = p.count > 1 ? Int(p[1]) ?? 0 : 0
            switch p.first ?? "" {
            case "add":
                let t = Double(p.count > 1 ? p[1] : "0.5") ?? 0.5
                g.stops.append(.init(at: t, color: g.color(at: t)))
            case "move" where i < g.stops.count && p.count > 2:
                g.stops[i].at = min(1, max(0, Double(p[2]) ?? 0))
            case "remove" where i < g.stops.count && g.stops.count > 2:
                g.stops.remove(at: i)
            case "color" where i < g.stops.count && p.count > 2:
                if let c = rgba(p[2]) { g.stops[i].color = c }
            default: return
            }
            m.setMarkupGradient(g)
        case "gradfrom", "gradto":
            let v = arg.split(separator: ",").compactMap { Double($0) }
            guard var g = m.markupGradient, v.count == 2 else { return }
            if kv[0] == "gradfrom" {
                g.from = CGPoint(x: v[0], y: v[1])
            } else {
                g.to = CGPoint(x: v[0], y: v[1])
            }
            m.setMarkupGradient(g)
        case "shadow", "glow":
            let kind: MarkupEffect.Kind = kv[0] == "glow" ? .glow : .shadow
            let p = arg.split(separator: ",").map(String.init)
            var fx = (mk.selection.first?.effects ?? mk.effects)
                .filter { $0.kind != kind }
            if arg != "off", let c = p.first.flatMap(rgba) {
                let n = p.dropFirst().compactMap { Double($0) }
                fx.append(kind == .glow
                    ? MarkupEffect(kind: .glow, color: c,
                                   radius: n.first ?? 12)
                    : MarkupEffect(kind: .shadow, color: c,
                                   blur: n.first ?? 8,
                                   dx: n.count > 1 ? n[1] : 4,
                                   dy: n.count > 2 ? n[2] : 4))
            }
            fx = fx.filter { $0.kind == .glow } + fx.filter { $0.kind != .glow }
            mk.effects = fx
            m.editSelection { $0.effects = fx }
        // A long stroke as a hand draws it: "scribble=W,H,N" -- N points
        // zigzagging over W x H canvas pixels, one every 8 ms (120 Hz);
        // prints the overlay's draws, how late the stroke ran, and what
        // its release took until the picture painted with it was shown.
        case "scribble":
            let a = arg.split(separator: ",").compactMap { Double($0) }
            guard a.count == 3 else { return }
            let (w, h, n) = (a[0], a[1], Int(a[2]))
            func at(_ i: Int) -> CGPoint {
                let t = Double(i) / Double(max(1, n - 1))
                let rows = 8.0
                let y = h * 0.1 + h * 0.8 * t
                let x = w * 0.1 + w * 0.8 *
                    abs((t * rows).truncatingRemainder(dividingBy: 2) - 1)
                return CGPoint(x: x, y: y)
            }
            MarkupOverlayView.drawn = (0, 0, 0)
            let start = CFAbsoluteTimeGetCurrent()
            pointer(.down, at(0))
            var late = 0.0
            for i in 1..<n {
                let due = start + Double(i) * 0.008
                let now = CFAbsoluteTimeGetCurrent()
                late = max(late, now - due)
                if due > now {
                    try? await Task.sleep(for: .seconds(due - now))
                }
                pointer(.drag, at(i))
            }
            let drawing = CFAbsoluteTimeGetCurrent() - start
            let d = MarkupOverlayView.drawn
            let up0 = CFAbsoluteTimeGetCurrent()
            pointer(.up, at(n - 1))
            let upSync = CFAbsoluteTimeGetCurrent() - up0
            for _ in 0..<600 where !mk.stroke.isEmpty || !mk.pending.isEmpty {
                try? await Task.sleep(for: .milliseconds(10))
            }
            let shown = CFAbsoluteTimeGetCurrent() - up0
            print(String(format: "snapshot: scribble points=%d took=%.2fs "
                         + "(ideal %.2fs) late=%.0fms draws=%d "
                         + "drawMean=%.1fms drawMax=%.1fms upSync=%.0fms "
                         + "shown=%.0fms", n, drawing,
                         Double(n - 1) * 0.008, late * 1000, d.count,
                         d.count > 0 ? d.total / Double(d.count) * 1000 : 0,
                         d.longest * 1000, upSync * 1000, shown * 1000))
        case "drag", "sdrag":
            let ps = points(arg)
            guard let first = ps.first else { return }
            pointer(.down, first)
            for p in ps.dropFirst() {
                pointer(.drag, p, shift: kv[0] == "sdrag")
            }
            pointer(.up, ps.last!)
            // Refused (a locked layer): the banner says why.
            if let b = m.banner { print("snapshot: drag banner=\(b)") }
        case "click", "dclick", "sclick", "aclick":
            // aclick: with ⌥ (the pen's corner, the eyedropper's Fill).
            if let p = points(arg).first {
                let n = kv[0] == "dclick" ? 2 : 1
                pointer(.down, p, clicks: n, shift: kv[0] == "sclick",
                        option: kv[0] == "aclick")
                pointer(.up, p, clicks: n, shift: kv[0] == "sclick",
                        option: kv[0] == "aclick")
            }
        case "key":
            m.markupKey(arg == "delete" ? .delete
                        : arg == "enter" ? .enter : .escape)
        // The pointer moved with no button down (a polygon's next edge).
        case "hover":
            if let p = points(arg).first { pointer(.hover, p) }
        // A key pressed as the keyboard would -- through the app's event
        // path, its monitors included: "[" "]" "{" (⇧[) "}" (⇧]).
        case "press":
            // "⌘]" (⌘[ ⌘} ⌘{): with ⌘, as the Arrange menu's keys.
            let command = arg.hasPrefix("⌘")
            let key = command ? String(arg.dropFirst()) : arg
            let shift = key == "{" || key == "}"
            // A tool's letter (v b e l r c) at its own key; [ ] { } theirs.
            let tool = MarkupTool.allCases.first {
                $0.key.map { String($0) } == key }
            let code: UInt16 = tool?.keyCode
                ?? (key == "d" ? 2 : key == "x" ? MarkupTool.swapKeyCode
                    : key == "[" || key == "{" ? 33 : 30)
            var flags: NSEvent.ModifierFlags = shift ? [.shift] : []
            if command { flags.insert(.command) }
            var viaMenu = "-"
            // The menu items with that key: their titles, enabled or not.
            func items(_ menu: NSMenu?) -> [NSMenuItem] {
                (menu?.items ?? []).flatMap { [$0] + items($0.submenu) }
            }
            let matching = command ? items(NSApp.mainMenu).filter {
                $0.keyEquivalent == key
                    && $0.keyEquivalentModifierMask.contains(.command)
            }.map { "\($0.title):\($0.isEnabled ? "on" : "off")" }
                .joined(separator: ",") : "-"
            if let w = m.editorWindow,
               let e = NSEvent.keyEvent(
                   with: .keyDown, location: .zero,
                   modifierFlags: flags, timestamp: 0,
                   windowNumber: w.windowNumber, context: nil,
                   characters: key, charactersIgnoringModifiers: key,
                   isARepeat: false, keyCode: code) {
                // ⌘D is posted all the same: the window's key monitor
                // takes it before any menu, as a real key's.
                if command && !NSApp.isActive && key != "d" {
                    // An app not in front has no key window: its menu's
                    // keys are matched as AppKit would match them.
                    viaMenu = "\(NSApp.mainMenu?.performKeyEquivalent(with: e) ?? false)"
                } else {
                    NSApp.postEvent(e, atStart: false)
                }
            }
            try? await Task.sleep(for: .milliseconds(150))
            let fr = m.editorWindow?.firstResponder.map {
                String(describing: type(of: $0))
            } ?? "-"
            let layer = mk.selectionLayer
            let order = (m.markupAsset?.layerStack.first { $0.id == layer }?
                .markup?.objects ?? []).map(\.id).joined(separator: ",")
            print("snapshot: press \(arg) tool=\(mk.tool.rawValue) radius=\(mk.radius) softness=\(mk.softness) focus=\(fr) order=\(order.isEmpty ? "-" : order) active=\(NSApp.isActive) menu=\(viaMenu) items=\(matching.isEmpty ? "-" : matching.replacingOccurrences(of: " ", with: "_")) selected=\(mk.selection.count)\(mk.drawing ? "+drawing" : "")")
        // The project's undo and redo (the text window's are "undo",
        // "redo"): printing what stays selected.
        case "pundo", "predo":
            let before = mk.selection.count
            if kv[0] == "pundo" { m.undoProject() } else { m.redoProject() }
            try? await Task.sleep(for: .milliseconds(300))
            print("snapshot: \(kv[0]) layers=\(m.markupAsset?.layerStack.map { "\($0.id.isEmpty ? "0" : $0.id)\($0.visible ? "" : "-hidden")" }.joined(separator: ",") ?? "-") selected=\(before)>\(mk.selection.count)\(mk.drawing ? "+drawing" : "")")
        // The toolbar's Deselect, beside Select (what ⌘D does).
        case "deselect":
            let can = m.markupCanDeselect
            m.deselectMarkup()
            print("snapshot: deselect can=\(can) selected=\(mk.selection.count)\(mk.drawing ? "+drawing" : "") canNow=\(m.markupCanDeselect)")
        // A layer LOCKED ("lock=1"; "0", "sel" as hide's), as its lock
        // does -- "unlock=1" unlocked: its state, the selection, and what
        // the banner says.
        case "lock", "unlock":
            let id = arg == "sel" ? mk.selectionLayer ?? ""
                : arg == "0" ? "" : arg
            let before: Int = mk.selection.count
            m.setLayerLocked(id, kv[0] == "lock")
            try? await Task.sleep(for: .milliseconds(300))
            let locked: Bool = m.isLayerLocked(id)
            let banner: String = m.banner ?? "-"
            print("snapshot: \(kv[0]) layer=\(arg) locked=\(locked) selected=\(before)>\(mk.selection.count) undo=\(m.document.undo ?? "-") banner=\(banner)")
        // A layer hidden ("hide=1"; layer 0 "0"; "sel" the selection's),
        // as its eye does -- "show=1" shown again.
        case "hide", "show":
            let id = arg == "sel" ? mk.selectionLayer ?? ""
                : arg == "0" ? "" : arg
            let before = mk.selection.count
            m.setLayerVisible(id, kv[0] == "show")
            try? await Task.sleep(for: .milliseconds(300))
            let l = m.markupAsset?.layerStack.first { $0.id == id }
            print("snapshot: \(kv[0]) layer=\(arg) visible=\(l?.visible ?? false) selected=\(before)>\(mk.selection.count)\(mk.drawing ? "+drawing" : "") selectionLayer=\(mk.selectionLayer.map { $0.isEmpty ? "0" : $0 } ?? "-")")
        // The pointer over a canvas pixel, its outline drawn; "zoom=2"
        // the stage zoomed by as much first. Prints the outline's radius
        // in points against the brush's in pixels.
        case "zoom":
            if m.markupOnClip {
                ClipZoomView.shown?.zoom(by: CGFloat(Double(arg) ?? 1))
            } else {
                CompareCanvas.shown?.zoom(by: CGFloat(Double(arg) ?? 1))
            }
        case "hover":
            if let p = points(arg).first, m.markupOnClip,
               let c = ClipMarkupNSView.shown {
                let r = c.hover(atCanvas: p)
                print("snapshot: hover \(arg) brush=\(mk.radius) ring=\(r)")
            } else if let p = points(arg).first, let c = CompareCanvas.shown {
                let r = c.hover(atCanvas: p)
                print("snapshot: hover \(arg) brush=\(mk.radius) ring=\(r)")
            }
        // A clip's player put at a frame: markup is drawn there.
        case "seek":
            m.timelineSeek(Int(arg) ?? 0)
            try? await Task.sleep(for: .milliseconds(500))
            print("snapshot: markup-seek frame=\(m.videoFrame) "
                  + "onClip=\(m.markupOnClip) ready=\(m.markupReady)")
        // The timeline in the prompt's place, or the prompt back
        // ("timeline=open|close"): markup drawn with it open.
        case "timeline":
            m.showTimeline(arg != "close")
            try? await Task.sleep(for: .milliseconds(900))
            print("snapshot: markup-timeline open=\(m.timelineOpen) "
                  + "view=\(ClipMarkupNSView.shown != nil) "
                  + "onClip=\(m.markupOnClip) ready=\(m.markupReady)")
        // A drag posted to the window over a clip's markup view, from
        // canvas pixels through its own mapping ("vdrag=10,10;200,120").
        case "vdrag":
            let ps = points(arg)
            if ps.count >= 2, let v = ClipMarkupNSView.shown,
               let a = v.windowPoint(canvas: ps[0]),
               let b = v.windowPoint(canvas: ps[ps.count - 1]) {
                await AppDelegate.postDrag(from: (a.x, a.y), to: (b.x, b.y))
            }
            print("snapshot: markup-vdrag \(arg) view=\(ClipMarkupNSView.shown != nil) "
                  + "selected=\(mk.selection.map { "\($0.kind.rawValue)@\(Int($0.x0)),\(Int($0.y0))-\(Int($0.x1)),\(Int($0.y1))" })")
        case "text":
            m.editSelection { if $0.kind == .text { $0.text = arg } }
        // Markup objects cut, copied and pasted, as the Edit menu does on
        // the stage.
        case "ocut", "ocopy", "opaste":
            let k: MarkupKey = kv[0] == "ocut" ? .cut
                : kv[0] == "ocopy" ? .copy : .paste
            let can = m.markupCan(k)
            m.markupKey(k)
            print("snapshot: objects \(kv[0]) can=\(can) "
                  + "selected=\(mk.selection.map(\.kind.rawValue)) "
                  + "at=\(mk.selection.map { "\(Int($0.x0)),\(Int($0.y0))" })")
        // The text's edit window: opened, typed into ("\n" a line),
        // a range selected, the Edit menu's commands -- as the menu sends
        // them, to the key window's text, when the panel is key -- and
        // drawn to <snapshot>-text.png.
        case "edit":
            m.editMarkupText()
            // Where it opened: beside the text's box, both on the screen.
            try? await Task.sleep(for: .milliseconds(300))
            let pf = mk.textPanel.window?.frame
            let bf = mk.selectedText.flatMap(MarkupTextPanel.screenBox)
            func box(_ r: CGRect?) -> String {
                guard let r else { return "-" }
                return "\(Int(r.minX)),\(Int(r.minY)),\(Int(r.width))x\(Int(r.height))"
            }
            var side = "-"
            if let p = pf, let b = bf {
                side = p.minX >= b.maxX ? "right" : p.maxX <= b.minX ? "left"
                    : p.maxY <= b.minY ? "below" : "over"
            }
            print("snapshot: text-panel edit open=\(mk.textPanel.isOpen) "
                  + "panel=\(box(pf)) box=\(box(bf)) side=\(side)")
        // A colour well clicked through the app's event queue -- the
        // toolbar's Color or Fill, by place ("colorwell=0") -- and where
        // the colour panel then is beside it.
        case "colorwell":
            guard let w = m.editorWindow, let root = w.contentView else { break }
            func wells(_ v: NSView) -> [NSColorWell] {
                (v as? NSColorWell).map { [$0] } ?? v.subviews.flatMap(wells)
            }
            let all = wells(root).filter { !$0.isHiddenOrHasHiddenAncestor }
                .sorted { $0.convert($0.bounds, to: nil).minX
                          < $1.convert($1.bounds, to: nil).minX }
            guard let i = Int(arg), all.indices.contains(i) else {
                print("snapshot: colorwell \(arg) wells=\(all.count) none")
                break
            }
            let well = all[i]
            let at = well.convert(NSPoint(x: well.bounds.midX,
                                          y: well.bounds.midY), to: nil)
            for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
                if let e = NSEvent.mouseEvent(
                    with: type, location: at, modifierFlags: [],
                    timestamp: ProcessInfo.processInfo.systemUptime,
                    windowNumber: w.windowNumber, context: nil,
                    eventNumber: 0, clickCount: 1, pressure: 1) {
                    NSApp.postEvent(e, atStart: false)
                }
            }
            try? await Task.sleep(for: .milliseconds(600))
            let panel = NSColorPanel.shared
            let wf = w.convertToScreen(well.convert(well.bounds, to: nil))
            let pf = panel.frame
            let gap = pf.maxY <= wf.minY ? wf.minY - pf.maxY
                : pf.minY >= wf.maxY ? pf.minY - wf.maxY : -1
            func box(_ r: CGRect) -> String {
                "\(Int(r.minX)),\(Int(r.minY)),\(Int(r.width))x\(Int(r.height))"
            }
            print("snapshot: colorwell \(i) class=\(type(of: well)) "
                  + "active=\(well.isActive) visible=\(panel.isVisible) "
                  + "well=\(box(wf)) panel=\(box(pf)) gap=\(Int(gap)) "
                  + "placed=\(ColorPanelPlacer.last != nil)")
        case "type", "sel", "cut", "copy", "paste", "undo", "redo",
             "selall", "panel":
            let tp = mk.textPanel
            guard let tv = tp.editor, tp.isOpen else {
                print("snapshot: text-panel closed for \(kv[0]) "
                      + "active=\(NSApp.isActive)")
                return
            }
            let key = NSApp.keyWindow === tp.window
            switch kv[0] {
            case "type":
                tv.insertText(arg.replacingOccurrences(of: "\\n", with: "\n"),
                              replacementRange: tv.selectedRange())
            case "sel":
                let r = arg.split(separator: ",").compactMap { Int($0) }
                if r.count == 2 {
                    tv.setSelectedRange(NSRange(location: r[0], length: r[1]))
                }
            case "selall": tv.selectAll(nil)
            case "cut", "copy", "paste":
                let sel = Selector(kv[0] + ":")
                if !(key && NSApp.sendAction(sel, to: nil, from: nil)) {
                    tv.perform(sel, with: nil)
                }
            case "undo":
                if key { m.undoAny() } else { tv.undoManager?.undo() }
            case "redo":
                if key { m.redoAny() } else { tv.undoManager?.redo() }
            default:
                if let w = tp.window, let v = w.contentView?.superview,
                   let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) {
                    v.cacheDisplay(in: v.bounds, to: rep)
                    let out = ProcessInfo.processInfo
                        .environment["VALTZ_SNAPSHOT"].map {
                            ($0 as NSString).deletingPathExtension + "-text.png"
                        } ?? arg
                    try? rep.representation(using: .png, properties: [:])?
                        .write(to: URL(fileURLWithPath: out))
                }
            }
            // An event through the app: the undo group a step opened ends
            // with it, as with each key typed.
            if let ev = NSEvent.otherEvent(
                with: .applicationDefined, location: .zero, modifierFlags: [],
                timestamp: 0, windowNumber: 0, context: nil, subtype: 0,
                data1: 0, data2: 0) {
                NSApp.postEvent(ev, atStart: false)
            }
            try? await Task.sleep(for: .milliseconds(50))
            let words = m.markup.selectedText?.text ?? "-"
            print("snapshot: text-panel \(kv[0]) key=\(key) "
                  + "undoLevel=\(tv.undoManager?.groupingLevel ?? -1) "
                  + "editor=\(tv.string.debugDescription) "
                  + "object=\(words.debugDescription) "
                  + "selection=\(NSStringFromRange(tv.selectedRange())) "
                  + "pasteboard=\((NSPasteboard.general.string(forType: .string) ?? "-").debugDescription)")
        case "pixels":
            m.materializeSelection()
        case "layers":
            let ids = arg.split(separator: ",", omittingEmptySubsequences: false)
                .map { AppDelegate.layerId(String($0)) }
            if let first = ids.first { m.selectLayer(first) }
            for id in ids.dropFirst() { m.toggleLayerSelection(id) }
        case "merge":
            m.mergeSelectedLayers()
        case "mask", "unmask":
            m.setLayerMask(AppDelegate.layerId(arg), kv[0] == "mask")
        case "add":
            m.addLayer()
        case "close":
            m.markupOpen = false
        default:
            break
        }
    }

    var body: some Scene {
        Window("Valtz", id: "main") {
            RootView(model: model)
                .frame(minWidth: model.windowMinWidth, minHeight: 620)
                .onAppear { delegate.model = model }
        }
        .windowToolbarStyle(.unified(showsTitle: false))
        // The first launch's; after it, the window as it was left -- and
        // a project opened at the size it was last looked at.
        .defaultSize(width: 1024, height: 768)
        .commands {
            CommandGroup(after: .appInfo) {
                if model.updater.canCheck {
                    Button("Check for Updates…") {
                        model.updater.checkForUpdates()
                    }
                }
            }
            // While a closed project's tasks are being stopped, what would
            // change or close a document waits (AppModel.closingTasks).
            CommandGroup(replacing: .newItem) {
                Button("New Project…") { newProject() }
                    .keyboardShortcut("n")
                    .disabled(model.closingTasks != nil)
                // Private and untitled, as a browser's private window (⇧⌘N).
                Button("New Anonymous Project") {
                    model.newAnonymousProject()
                }
                    .keyboardShortcut("n", modifiers: [.command, .shift])
                    .disabled(model.closingTasks != nil)
                Button("Open Project…") { openProject() }
                    .keyboardShortcut("o")
                    .disabled(model.closingTasks != nil)
            }
            // The project as a document (DESIGN §5b): Save writes its
            // working copy to its file; Revert to Saved goes back to it.
            CommandGroup(replacing: .saveItem) {
                Button("Save") { model.saveDocument() }
                    .keyboardShortcut("s")
                    .disabled(model.projectId == nil
                              || model.closingTasks != nil)
                Button("Save As…") { model.saveDocumentAs() }
                    .keyboardShortcut("s", modifiers: [.command, .shift])
                    .disabled(model.projectId == nil
                              || model.closingTasks != nil)
                Button("Revert to Saved") { model.revertDocument() }
                    .disabled(!model.document.dirty || model.document.untitled
                              || model.isAnonymous
                              || model.closingTasks != nil)
            }
            // ⌘Z: the text being typed while it has typing to undo, else
            // the project's history of commands.
            CommandGroup(replacing: .undoRedo) {
                Button(model.undoMenuTitle) { model.undoAny() }
                    .keyboardShortcut("z")
                    .disabled(!model.canUndoAny || model.closingTasks != nil)
                Button(model.redoMenuTitle) { model.redoAny() }
                    .keyboardShortcut("z", modifiers: [.command, .shift])
                    .disabled(!model.canRedoAny || model.closingTasks != nil)
            }
            CommandGroup(after: .importExport) {
                Button("Attach Media…") { attachMedia() }
                    .keyboardShortcut("i", modifiers: [.command, .shift])
                    .disabled(model.closingTasks != nil)
            }
            // The image, video or sound on screen, as the share menu's Save…
            // writes it: a format, its quality.
            CommandGroup(after: .saveItem) {
                Divider()
                Button(model.saveItemTitle) {
                    ShareButton.save(model)
                }
                    .keyboardShortcut("e")
                    .disabled(!model.canExportStage
                              || model.closingTasks != nil)
            }
            // The markup's selection let go, as Photoshop's ⌘D (the
            // markup toolbar's button beside Select).
            CommandGroup(after: .pasteboard) {
                Button("Deselect") { model.deselectMarkup() }
                    .keyboardShortcut("d")
                    .disabled(!model.markupCanDeselect
                              || model.closingTasks != nil)
            }
            CommandGroup(after: .textEditing) {
                Button("Find") { model.findRequest += 1 }
                    .keyboardShortcut("f")
            }
            CommandGroup(after: .toolbar) {
                Divider()
                Button("Zoom In") { model.zoom(.zoomIn) }
                    .keyboardShortcut("+")
                    .disabled(!model.canZoom)
                Button("Zoom Out") { model.zoom(.zoomOut) }
                    .keyboardShortcut("-")
                    .disabled(!model.canZoom)
                Button("Actual Size") { model.zoom(.actualSize) }
                    .keyboardShortcut("0")
                    .disabled(!model.canZoom)
                Button("Zoom to Fit") { model.zoom(.fit) }
                    .keyboardShortcut("9")
                    .disabled(!model.canZoom)
                // Rounded by default; square to see a picture's own
                // corners.
                Toggle("Round Stage Corners", isOn: Binding(
                    get: { model.roundStageCorners },
                    set: { on in
                        withAnimation(.smooth(duration: 0.2)) {
                            model.roundStageCorners = on
                        }
                    }))
                // The guides over what the stage shows -- the thirds
                // until set by hand -- ⌘' as the grid is in Photoshop.
                Toggle("Show Guides", isOn: Binding(
                    get: { model.showsGuides },
                    set: { model.showsGuides = $0 }))
                    .keyboardShortcut("'")
                Button("Reset Guides") { model.resetGuides() }
                    .disabled(!model.guidesSetByHand)
                Divider()
                Button(model.inspectorOpen ? "Hide Inspector" : "Show Inspector") {
                    withAnimation(AppModel.motion) { model.inspectorOpen.toggle() }
                }
                .keyboardShortcut("i", modifiers: [.command, .option])
                Button(model.markupOpen ? "Hide Markup Toolbar"
                                        : "Show Markup Toolbar") {
                    withAnimation(AppModel.motion) { model.markupOpen.toggle() }
                }
                .keyboardShortcut("a", modifiers: [.command, .shift])
                .disabled(model.promptImmersive)
                // The prompt the window's height, several open as tabs
                // (PromptStudio).
                Toggle("Prompt Editor", isOn: Binding(
                    get: { model.promptImmersive },
                    set: { on in
                        guard on != model.promptImmersive else { return }
                        withAnimation(AppModel.motion) {
                            model.toggleImmersivePrompt()
                        }
                    }))
                .keyboardShortcut("e", modifiers: [.command, .shift])
                // The timeline in the prompt's place, while the stage
                // works on a timeline or a still with pages.
                Toggle("Timeline", isOn: Binding(
                    get: { model.timelineOpen },
                    set: { model.showTimeline($0) }))
                .disabled(!model.timelineAvailable)
                Toggle("Markdown as It Reads", isOn: Binding(
                    get: { model.promptMarkdown },
                    set: { model.promptMarkdown = $0 }))
                // The machine's load and thermal state along the bottom:
                // ⌘/, as Finder's status bar.
                Button(model.showsStatusBar ? "Hide Status Bar"
                                            : "Show Status Bar") {
                    withAnimation(AppModel.motion) {
                        model.showsStatusBar.toggle()
                    }
                }
                .keyboardShortcut("/")
            }
            // The selected markup objects' order, by Photoshop's keys (the
            // toolbar's Arrange panel has the rest).
            CommandMenu("Arrange") {
                ForEach(MarkupArrange.order) { how in
                    if let key = how.shortcut {
                        Button(how.label) { model.arrangeSelection(how) }
                            .keyboardShortcut(key)
                            .disabled(!model.markupCanArrange(how)
                                      || model.closingTasks != nil)
                    }
                }
            }
            // Help: the buttons' keys, as holding ⌘? shows them -- from
            // here until the next key or click. ⌘? itself is the window's
            // key monitor's (ShortcutHints): held, never this item's.
            CommandGroup(replacing: .help) {
                Button("Keyboard Shortcuts") {
                    ShortcutHints.shared.showLatched()
                }
                .keyboardShortcut("?", modifiers: .command)
            }
            CommandGroup(before: .sidebar) {
                Button(model.navOpen ? "Hide Navigation" : "Show Navigation") {
                    model.toggleNav()
                }
                .keyboardShortcut("n", modifiers: [.command, .control])
                Divider()
            }
        }
        Settings {
            SettingsView(model: model)
        }
    }

    /// Dev aid: a mouse click, sent to `window`, on the first control in
    /// `root` that takes one -- and whether a menu began tracking.
    @MainActor
    static func clickFirstControl(in root: NSView, window: NSWindow) {
        func find(_ v: NSView) -> NSView? {
            if v is NSPopUpButton || v is NSButton
                || String(describing: type(of: v)) == "_FocusRingView" {
                return v
            }
            for s in v.subviews { if let hit = find(s) { return hit } }
            return nil
        }
        func tree(_ v: NSView, _ d: Int) -> String {
            String(repeating: " ", count: d) + String(describing: type(of: v))
                + " \(v.frame)\n" + v.subviews.map { tree($0, d + 1) }.joined()
        }
        FileHandle.standardError.write(Data("snapshot: accessory\n\(tree(root, 1))".utf8))
        // Where to click: a control found, else the accessory's left
        // third, where the menu is.
        let target = find(root)
        let local = target.map { NSPoint(x: $0.bounds.midX, y: $0.bounds.midY) }
            ?? NSPoint(x: root.bounds.width * 0.55, y: root.bounds.maxY - 22)
        let inWindow = (target ?? root).convert(local, to: nil)
        final class Seen: @unchecked Sendable { var menu: NSMenu? }
        let seen = Seen()
        let obs = NotificationCenter.default.addObserver(
            forName: NSMenu.didBeginTrackingNotification, object: nil,
            queue: nil) { note in
            seen.menu = note.object as? NSMenu
            // Closed again a moment later, from the run loop.
            let t = Timer(timeInterval: 0.5, repeats: false) { _ in
                seen.menu?.cancelTracking()
            }
            RunLoop.main.add(t, forMode: .common)
        }
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            if let e = NSEvent.mouseEvent(
                with: type, location: inWindow, modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime,
                windowNumber: window.windowNumber, context: nil,
                eventNumber: 0, clickCount: 1, pressure: 1) {
                window.sendEvent(e)
            }
        }
        NotificationCenter.default.removeObserver(obs)
        FileHandle.standardError.write(Data(
            "snapshot: clicked \(target.map { String(describing: type(of: $0)) } ?? "the accessory") at \(inWindow): menu \(seen.menu != nil ? "opened" : "did NOT open")\n".utf8))
    }

    /// `body` run by the main run loop itself, not from a task: a modal
    /// panel opened from a task holds the main queue until it closes.
    static func fromRunLoop(_ body: @escaping @MainActor () -> Void) {
        CFRunLoopPerformBlock(CFRunLoopGetMain(),
                              CFRunLoopMode.commonModes.rawValue) {
            MainActor.assumeIsolated { body() }
        }
        CFRunLoopWakeUp(CFRunLoopGetMain())
    }

    /// The new project chosen first, then the one open closed -- asked
    /// about, its tasks stopped (AppModel.closeProject): none is stopped
    /// for a panel that is then cancelled.
    private func newProject() {
        let panel = NSSavePanel()
        panel.title = String(localized: "New Valtz Project")
        panel.nameFieldStringValue = "Untitled.valtz"
        panel.allowedContentTypes = [.valtzProject]
        panel.directoryURL = model.panelFolder(.project)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        model.rememberPanel(.project, chose: url)
        model.closeProject(.newProject) { [model] in
            model.createProject(at: url)
        }
    }

    private func openProject() {
        let panel = NSOpenPanel()
        panel.title = String(localized: "Open Valtz Project")
        panel.canChooseDirectories = true
        panel.treatsFilePackagesAsDirectories = false
        panel.allowedContentTypes = [.valtzProject]
        panel.directoryURL = model.panelFolder(.project)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        model.rememberPanel(.project, chose: url)
        model.closeProject(.open) { [model] in
            model.openProject(path: url.path)
        }
    }

    /// Pictures and clips into the prompt, as its paperclip puts them.
    private func attachMedia() {
        let panel = NSOpenPanel()
        panel.title = String(localized: "Attach Media")
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [.image, .movie]
        panel.directoryURL = model.panelFolder(.attach)
        if panel.runModal() == .OK {
            model.rememberPanel(.attach, chose: panel.urls.first)
            model.addReferences(panel.urls)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor var model: AppModel? {
        didSet { openPending() }
    }
    /// Projects Finder asked Valtz to open -- a double-click, a drop on
    /// its Dock icon -- held until the window's model is there.
    @MainActor private var pending: [URL] = []

    func application(_ application: NSApplication, open urls: [URL]) {
        pending += urls.filter { $0.pathExtension.lowercased() == "valtz" }
        openPending()
    }

    /// One window, one project: the last one asked for, once the one open
    /// has been saved or let go.
    @MainActor
    private func openPending() {
        guard let model, let url = pending.last else { return }
        pending = []
        model.closeProject(.open) { model.openProject(path: url.path) }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // A bare executable (not launched through LaunchServices) starts
        // as a background process; make it a regular foreground app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate()
        ColorPanelPlacer.install()
        scheduleSnapshotIfRequested()
    }

    /// A layer as a hook names it: "0" is layer 0, whose id is "" ("bg"
    /// too, as it was once called).
    nonisolated static func layerId(_ s: String) -> String {
        s == "0" || s == "bg" ? "" : s
    }

    /// A closed project's tasks being stopped, as a task step prints it:
    /// `closing=<reason>:<left>/<total> project=<name>`.
    @MainActor
    static func closingSummary(_ m: AppModel) -> String {
        let closing = m.closingTasks.map {
            "\($0.reason):\($0.left.count)/\($0.total)"
        } ?? "-"
        let name = (m.projectName ?? "-")
            .replacingOccurrences(of: " ", with: "_")
        return "closing=\(closing) project=\(name)"
    }

    /// What the landings did, as a task step prints it:
    /// `landing=open:<timeline open>,auto:<n>,flash:<n>,results:<n>,
    /// assets:<inspector on Assets>` -- the timeline opened by itself, the
    /// switch flashed, Assets shown for a second result (AppModel.landed).
    @MainActor
    static func landingSummary(_ m: AppModel) -> String {
        let c = m.landingCounts
        let assets = m.inspectorOpen && (m.inspectorStacked
            ? !m.inspectorFolded.contains(.assets)
            : m.inspectorTab == .assets)
        return "open:\(m.timelineOpen),auto:\(c.timeline),"
            + "flash:\(c.flash),results:\(c.results),assets:\(assets)"
    }

    /// The main window drawn by AppKit (cacheDisplay) to a PNG: a second
    /// picture in one snapshot run.
    @MainActor
    static func drawWindow(to path: String) {
        guard let win = NSApp.windows.first(where: {
                  $0.isVisible && !$0.className.contains("Popover")
                      && !($0 is ShortcutHintWindow) }),
              let content = win.contentView else { return }
        let v = content.superview ?? content
        guard let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)
        else { return }
        v.cacheDisplay(in: v.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?
            .write(to: URL(fileURLWithPath: path))
    }

    /// The main window as the screen shows it (ScreenCaptureKit, glass
    /// and AppKit's own animations included) to a PNG, now: TASKS'
    /// `shot:<file>`, several in a run -- a bar's look over time.
    @MainActor
    static func shootWindow(to path: String) async {
        do {
            let number = NSApp.windows.first(where: {
                $0.isVisible && !$0.className.contains("Popover")
                    && !($0 is ShortcutHintWindow) })?.windowNumber
            let content = try await SCShareableContent.currentProcess
            guard let win = content.windows.first(where: {
                $0.isOnScreen && $0.windowLayer == 0
                    && (number == nil || $0.windowID == CGWindowID(number!))
            }) else { throw CocoaError(.fileNoSuchFile) }
            let filter = SCContentFilter(desktopIndependentWindow: win)
            let cfg = SCStreamConfiguration()
            let scale = CGFloat(filter.pointPixelScale)
            cfg.width = Int(filter.contentRect.width * scale)
            cfg.height = Int(filter.contentRect.height * scale)
            cfg.showsCursor = false
            let img = try await SCScreenshotManager.captureImage(
                contentFilter: filter, configuration: cfg)
            guard let dst = CGImageDestinationCreateWithURL(
                URL(fileURLWithPath: path) as CFURL,
                UTType.png.identifier as CFString, 1, nil) else { return }
            CGImageDestinationAddImage(dst, img, nil)
            CGImageDestinationFinalize(dst)
        } catch {
            print("snapshot: shot \(path) failed: \(error.localizedDescription)")
        }
    }

    /// Dev aid: VALTZ_SNAPSHOT=<file.png> captures the main window after
    /// VALTZ_SNAPSHOT_DELAY seconds (default 4), prints a one-line summary
    /// of the UI state to stderr, then quits -- for UI checks from a
    /// terminal or CI. ScreenCaptureKit (current-process content) gives a
    /// true composite but needs a properly signed build; ad-hoc dev builds
    /// fall back to cacheDisplay, which renders the Core Animation viewer
    /// faithfully but omits vibrancy-backed controls.
    @MainActor
    private func scheduleSnapshotIfRequested() {
        let env = ProcessInfo.processInfo.environment
        guard let path = env["VALTZ_SNAPSHOT"] else { return }
        let delay = Double(env["VALTZ_SNAPSHOT_DELAY"] ?? "") ?? 4
        // The script is a method of its own, not this Task's closure: a
        // closure is type-checked with the expression it sits in, and this
        // one -- thousands of lines -- held every build of this file ~10
        // minutes on one core (565 s of its 571 s type-check).
        Task { @MainActor in
            await self.runSnapshotScript(env: env, path: path, delay: delay)
        }
    }

    /// The snapshot run itself (scheduleSnapshotIfRequested): the hooks in
    /// turn, the capture, the summary line, the quit.
    @MainActor
    private func runSnapshotScript(env: [String: String], path: String,
                                   delay: Double) async {
        // Optional scripting for UI checks: a prompt, the drawer or
        // the navigation open, and Start pressed.
        try? await Task.sleep(for: .seconds(1))
        if let m = model {
            // The window's size, "1600x900" (points).
            if let sz = env["VALTZ_SNAPSHOT_SIZE"],
               let win = NSApp.windows.first(where: { $0.isVisible }) {
                let wh = sz.split(separator: "x").compactMap { Double($0) }
                if wh.count == 2 {
                    var f = win.frame
                    f.origin.y += f.height - wh[1]
                    f.size = CGSize(width: wh[0], height: wh[1])
                    win.setFrame(f, display: true)
                }
            }
            if let p = env["VALTZ_SNAPSHOT_PROMPT"] { m.setPrompt(p) }
            // A captured prompt into the box: its id, or "newest".
            if let v = env["VALTZ_SNAPSHOT_USE_PROMPT"],
               let a = v == "newest"
                   ? m.assets.last(where: \.isPrompt)
                   : m.assets.first(where: { $0.id == v }) {
                m.usePrompt(a)
            }
            // Settings › Experimental's switches, for the run:
            // "agent" (the Agent tab; VALTZ_SNAPSHOT_AGENT too).
            if env["VALTZ_SNAPSHOT_EXPERIMENTAL"] == "agent"
                || env["VALTZ_SNAPSHOT_AGENT"] != nil {
                m.setAgentEnabled(true)
            }
            // Settings › Agentic Helper › Show Speed, for the run.
            if env["VALTZ_SNAPSHOT_AGENT_SPEED"] == "1" {
                m.setAgentShowsSpeed(true)
            }
            // The generation card's Create row, then its model field
            // (a model id; unset is Auto).
            if let v = env["VALTZ_SNAPSHOT_MODALITY"],
               let v = Modality(rawValue: v) { m.setModality(v) }
            if let id = env["VALTZ_SNAPSHOT_MODEL"] { m.modelChoice = id }
            // Favor ("speed", ..., "custom": its panel opens), and
            // Custom's values ("sol_attn=true,steps=8"), as the panel
            // sets them.
            if let v = env["VALTZ_SNAPSHOT_FAVOR"],
               let p = Preference(rawValue: v) {
                // Favor lives in the generation card: open, as a
                // person choosing it has it.
                m.openPanel = .generate
                try? await Task.sleep(for: .milliseconds(400))
                m.choosePreference(p)
            }
            // Weights (LoRAs, DiT / VAE checkpoints), as if dropped
            // on Custom's panel: each filed by what it is.
            if let spec = env["VALTZ_SNAPSHOT_WEIGHTS"] {
                m.addWeights(spec.split(separator: ",").map {
                    URL(fileURLWithPath: String($0))
                })
            }
            if let spec = env["VALTZ_SNAPSHOT_TUNE"] {
                for part in spec.split(separator: ",") {
                    let kv = part.split(separator: "=").map(String.init)
                    guard kv.count == 2 else { continue }
                    // "turbo=off": the Turbo LoRA unchecked in the list.
                    if kv[0] == "turbo" {
                        if let p = m.tuningInfo?.turboLoRA, !p.isEmpty {
                            m.setLoRA(p, on: !["off", "false", "none"]
                                          .contains(kv[1]))
                        }
                        continue
                    }
                    // "fast=<catalog id>" (or none): the Acceleration
                    // pick.
                    if kv[0] == "fast" {
                        m.setFastLoRA(kv[1] == "none" ? nil : kv[1])
                        continue
                    }
                    switch kv[1] {
                    case "true", "on": m.setTuning(kv[0], flag: true)
                    case "false", "off": m.setTuning(kv[0], flag: false)
                    default:
                        // A number, else a choice's value
                        // ("i8_gemm_tier=token").
                        if let d = Double(kv[1]) {
                            m.setTuning(kv[0], number: d)
                        } else {
                            m.setTuning(kv[0], choice: kv[1])
                        }
                    }
                }
            }
            // Custom's panel drawn by AppKit beside the snapshot (a
            // popover is a window of its own): <snapshot>-custom.png.
            if env["VALTZ_SNAPSHOT_FAVOR"] == "custom",
               let shot = env["VALTZ_SNAPSHOT"] {
                try? await Task.sleep(for: .milliseconds(800))
                let pops = NSApp.windows.filter {
                    $0.isVisible && $0.className.contains("Popover")
                }
                if let v = pops.first?.contentView,
                   let rep = v.bitmapImageRepForCachingDisplay(
                       in: v.bounds) {
                    v.cacheDisplay(in: v.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?
                        .write(to: URL(fileURLWithPath: shot
                            .replacingOccurrences(of: ".png",
                                                  with: "-custom.png")))
                }
                FileHandle.standardError.write(Data(
                    "snapshot: custom panel=\(pops.isEmpty ? "-" : "drawn")\n".utf8))
            }
            // Favor's Custom clicked again, `n` times ("2"): clicks
            // posted to the window just under the tip of the panel's
            // arrow, which points at the Custom segment -- each one
            // prints whether the panel is open after it.
            if let n = Int(env["VALTZ_SNAPSHOT_TUNE_CLICKS"] ?? ""),
               let win = NSApp.windows.first(where: {
                   $0.isVisible && !$0.className.contains("Popover") }) {
                var at: NSPoint?
                for i in 0..<n {
                    if let pop = NSApp.windows.first(where: {
                        $0.isVisible && $0.className.contains("Popover")
                    }) {
                        // The panel opens above the segment (there is
                        // no room below the card), its arrow's tip at
                        // the panel's foot: the click goes just under
                        // it, on the segment.
                        at = NSPoint(x: pop.frame.midX - win.frame.minX,
                                     y: win.frame.maxY - pop.frame.minY
                                        + 10)
                    }
                    guard let p = at else { break }
                    Self.postClick(x: p.x, y: p.y)
                    try? await Task.sleep(for: .milliseconds(900))
                    FileHandle.standardError.write(Data(
                        "snapshot: tune-click \(i + 1) at \(Int(p.x)),\(Int(p.y)) open=\(m.showsTuning)\n".utf8))
                }
            }
            // A song's plan ("full", "melody", "off") and the most it
            // may run, in seconds ("auto": as long as the model makes
            // it).
            if let v = env["VALTZ_SNAPSHOT_SONG_PLAN"],
               let p = SongPlan(rawValue: v) { m.songPlan = p }
            if let v = env["VALTZ_SNAPSHOT_SONG_LENGTH"] {
                m.songSeconds = Int(v)
            }
            // A clip's length: a preset's seconds ("5"), or what the
            // Length field takes ("100 frames", "7.5 s").
            if let v = env["VALTZ_SNAPSHOT_LENGTH"] {
                if let n = Int(v), AppModel.clipLengths.contains(n) {
                    m.clipSeconds = n
                } else {
                    m.setClipLength(v)
                }
            }
            if env["VALTZ_SNAPSHOT_WIPE_LINE"] == "0" {
                m.stage.wipeLine = false
            }
            if env["VALTZ_SNAPSHOT_COMPACT"] != nil {
                m.promptCompact = true
            }
            if let a = env["VALTZ_SNAPSHOT_APPEARANCE"] {
                m.setAppearance(dark: a == "dark")
            }
            if env["VALTZ_SNAPSHOT_INSPECTOR"] != nil {
                m.inspectorOpen = true
            }
            if env["VALTZ_SNAPSHOT_STATUS_BAR"] != nil {
                m.showsStatusBar = true
            }
            // A member's offer, asked ("Ask Each Time"), answered as
            // its alert's button would, two seconds after it comes
            // (VALTZ_SNAPSHOT_FLEET_ANSWER=accept|decline).
            if let answer = env["VALTZ_SNAPSHOT_FLEET_ANSWER"] {
                Task { @MainActor in
                    var done = Set<String>()
                    while true {
                        try? await Task.sleep(for: .milliseconds(500))
                        guard let a = m.fleetAsks.first,
                              !done.contains(a.job) else { continue }
                        done.insert(a.job)
                        try? await Task.sleep(for: .seconds(2))
                        print("snapshot: fleet-ask from=\(a.from) "
                              + "title=\(a.title.replacingOccurrences(of: " ", with: "_")) "
                              + "answer=\(answer)")
                        m.answerFleet(a, accept: answer == "accept")
                    }
                }
            }
            if env["VALTZ_SNAPSHOT_MARKUP"] != nil { m.markupOpen = true }
            if let n = env["VALTZ_SNAPSHOT_RENAME"] { m.renameSession(n) }
            if env["VALTZ_SNAPSHOT_TITLE_EDIT"] != nil {
                // After the prompt has taken the focus, as a click on
                // the name would come.
                try? await Task.sleep(for: .milliseconds(500))
                m.renamingTitle = true
                // Clicks posted to the window ("x,y" in points from
                // its top left; ";" between), as a pointer's would
                // come (the pointer itself does not move).
                if let spec = env["VALTZ_SNAPSHOT_TITLE_CLICK"] {
                    try? await Task.sleep(for: .milliseconds(700))
                    for c in spec.split(separator: ";") {
                        let xy = c.split(separator: ",")
                            .compactMap { Double($0) }
                        guard xy.count == 2 else { continue }
                        Self.postClick(x: xy[0], y: xy[1])
                        try? await Task.sleep(for: .milliseconds(500))
                        FileHandle.standardError.write(Data(
                            "snapshot: title-click \(Int(xy[0])),\(Int(xy[1])) renaming=\(m.renamingTitle)\n".utf8))
                    }
                }
            }
            if env["VALTZ_SNAPSHOT_TITLEBAR_HOVER"] != nil {
                m.titlebarHover = true
            }
            // A picture attached as the paperclip would, then time
            // for its import to land (Start needs the asset).
            // A picture dropped on the stage: the base to edit.
            if let f = env["VALTZ_SNAPSHOT_BASE"] {
                let url = URL(fileURLWithPath: f)
                m.setBase(from: url)
                for _ in 0..<100 where m.asset(forFile: url) == nil
                    || m.baseAttachment == nil {
                    try? await Task.sleep(for: .milliseconds(100))
                }
            }
            // Comma-separated: each staged in turn.
            if let f = env["VALTZ_SNAPSHOT_ATTACH"] {
                let urls = f.split(separator: ",").map {
                    URL(fileURLWithPath: String($0))
                }
                m.addReferences(urls)
                for url in urls {
                    for _ in 0..<100 where m.asset(forFile: url) == nil {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                }
            }
            // The prompt row's media, by place (0 first): "focus=1"
            // (a click on it), "mention=0" (dragged into the text, at
            // its end), "move=2:0" (along the row), "remove=1",
            // "base=1" (its pencil badge).
            if let spec = env["VALTZ_SNAPSHOT_REFS"] {
                // A thumbnail's drag, its pointer scripted: `drag=n`
                // begun on the n-th, `over=n` onto another, `out` off
                // the row, `in` back, `drop` let go on the row,
                // `dropout` off it -- each printing the row's order.
                ReferenceRow.followsPointer = false
                for step in spec.split(separator: ";") {
                    try? await Task.sleep(for: .milliseconds(500))
                    let kv = step.split(separator: "=").map(String.init)
                    let at = kv.count > 1 ? Int(kv[1]) : nil
                    let ids = m.promptAttachments.map(\.id)
                    var dragged = true
                    switch kv[0] {
                    case "drag" where at.map(ids.indices.contains)
                                        == true:
                        m.beginRowDrag(ids[at!])
                    case "over" where at.map(ids.indices.contains)
                                        == true:
                        m.rowDragEntered(ids[at!])
                    case "out": m.rowDragMoved(inRow: false)
                    case "in": m.rowDragMoved(inRow: true)
                    case "drop": m.endRowDrag(inRow: true)
                    case "dropout": m.endRowDrag(inRow: false)
                    default: dragged = false
                    }
                    if dragged {
                        try? await Task.sleep(for: .milliseconds(100))
                        FileHandle.standardError.write(Data(
                            "snapshot: refs \(step) order=\(m.promptAttachments.map(\.url.lastPathComponent).joined(separator: ",")) dragging=\(m.rowDrag.map { d in m.promptAttachments.first { $0.id == d.id }?.url.lastPathComponent ?? "?" } ?? "-") outside=\(m.rowDrag?.outside == true)\n".utf8))
                        continue
                    }
                    let args = (kv.count > 1 ? kv[1] : "")
                        .split(separator: ":").compactMap { Int($0) }
                    let row = m.promptAttachments
                    guard let n = args.first, row.indices.contains(n)
                    else { continue }
                    let id = row[n].id
                    switch kv[0] {
                    case "focus": m.toggleReferenceFocus(id)
                    case "remove": m.removeReference(id)
                    case "base": m.toggleBase(id)
                    case "move" where args.count > 1:
                        m.moveReference(id, to: args[1])
                    case "mention":
                        if let win = NSApp.windows.first(where: {
                               $0.isVisible }),
                           let root = win.contentView?.superview,
                           let tv = Self.find(PromptTextView.self,
                                              in: root) {
                            tv.insertMentions([id],
                                              at: tv.string.utf16.count)
                        }
                    default: break
                    }
                }
                try? await Task.sleep(for: .milliseconds(500))
            }
            // Text typed at the end of the prompt, a character at a time
            // (a tag typed whole becomes a tag).
            if let typed = env["VALTZ_SNAPSHOT_TYPE_TEXT"],
               let win = NSApp.windows.first(where: { $0.isVisible }),
               let root = win.contentView?.superview,
               let tv = Self.find(PromptTextView.self, in: root) {
                // The scripted prompt in the view first.
                try? await Task.sleep(for: .milliseconds(300))
                win.makeFirstResponder(tv)
                tv.setSelectedRange(NSRange(location: tv.string.utf16.count,
                                            length: 0))
                for ch in typed {
                    tv.insertText(String(ch),
                                  replacementRange: tv.selectedRange())
                }
                try? await Task.sleep(for: .milliseconds(300))
            }
            // An input method's composition, as one drives the text
            // view: the caret put somewhere, unconfirmed text marked
            // there and grown, then committed. Each step prints where
            // the marked text is and where its panel would go.
            if let script = env["VALTZ_SNAPSHOT_IME"],
               let win = NSApp.windows.first(where: { $0.isVisible }),
               let root = win.contentView?.superview,
               let tv = Self.find(PromptTextView.self, in: root) {
                try? await Task.sleep(for: .milliseconds(300))
                win.makeFirstResponder(tv)
                let none = NSRange(location: NSNotFound, length: 0)
                for step in script.split(separator: ";") {
                    let parts = step.split(separator: ":", maxSplits: 1)
                    let arg = parts.count > 1 ? String(parts[1]) : ""
                    switch parts.first ?? "" {
                    case "caret":
                        tv.setSelectedRange(NSRange(
                            location: min(Int(arg) ?? 0,
                                          tv.string.utf16.count),
                            length: 0))
                    case "mark":
                        tv.setMarkedText(
                            arg,
                            selectedRange: NSRange(
                                location: arg.utf16.count, length: 0),
                            replacementRange: none)
                    case "commit":
                        tv.insertText(arg, replacementRange: none)
                    default:
                        continue
                    }
                    // As between keystrokes: the views update.
                    try? await Task.sleep(for: .milliseconds(150))
                    let m = tv.markedRange()
                    let probe = m.location == NSNotFound
                        ? tv.selectedRange() : m
                    let r = tv.firstRect(forCharacterRange: probe,
                                         actualRange: nil)
                    let inWin = win.convertFromScreen(r)
                    print("snapshot: ime \(step) sel=\(tv.selectedRange().location),\(tv.selectedRange().length) marked=\(m.location == NSNotFound ? "-" : "\(m.location),\(m.length)") panelX=\(Int(inWin.minX)) panelY=\(Int(win.frame.height - inWin.maxY)) text=\(tv.string.replacingOccurrences(of: "\n", with: "|"))")
                }
                try? await Task.sleep(for: .milliseconds(300))
            }
            // The same through a real input method: Pinyin selected,
            // key events posted to the window as typing sends them (the
            // input method composes in its own process), each step
            // printed; the input source put back after.
            if let script = env["VALTZ_SNAPSHOT_IME_KEYS"],
               let win = NSApp.windows.first(where: { $0.isVisible }),
               let root = win.contentView?.superview,
               let tv = Self.find(PromptTextView.self, in: root) {
                try? await Task.sleep(for: .milliseconds(300))
                win.makeFirstResponder(tv)
                let before = TISCopyCurrentKeyboardInputSource()
                    .takeRetainedValue()
                let filter = [kTISPropertyInputSourceID as String:
                                "com.apple.inputmethod.SCIM.ITABC"]
                    as CFDictionary
                NSApp.activate(ignoringOtherApps: true)
                win.makeKeyAndOrderFront(nil)
                if let list = TISCreateInputSourceList(filter, false)?
                    .takeRetainedValue() as? [TISInputSource],
                   let pinyin = list.first {
                    let st = TISSelectInputSource(pinyin)
                    print("snapshot: ime-key select=\(st)")
                }
                try? await Task.sleep(for: .milliseconds(500))
                let now = TISCopyCurrentKeyboardInputSource()
                    .takeRetainedValue()
                if let id = TISGetInputSourceProperty(
                    now, kTISPropertyInputSourceID) {
                    let s = Unmanaged<CFString>.fromOpaque(id)
                        .takeUnretainedValue() as String
                    print("snapshot: ime-key source=\(s) active=\(NSApp.isActive) context=\(tv.inputContext?.selectedKeyboardInputSource ?? "-")")
                }
                let codes: [Character: Int] = [
                    "a": kVK_ANSI_A, "b": kVK_ANSI_B, "c": kVK_ANSI_C,
                    "d": kVK_ANSI_D, "e": kVK_ANSI_E, "f": kVK_ANSI_F,
                    "g": kVK_ANSI_G, "h": kVK_ANSI_H, "i": kVK_ANSI_I,
                    "j": kVK_ANSI_J, "k": kVK_ANSI_K, "l": kVK_ANSI_L,
                    "m": kVK_ANSI_M, "n": kVK_ANSI_N, "o": kVK_ANSI_O,
                    "p": kVK_ANSI_P, "q": kVK_ANSI_Q, "r": kVK_ANSI_R,
                    "s": kVK_ANSI_S, "t": kVK_ANSI_T, "u": kVK_ANSI_U,
                    "v": kVK_ANSI_V, "w": kVK_ANSI_W, "x": kVK_ANSI_X,
                    "y": kVK_ANSI_Y, "z": kVK_ANSI_Z, " ": kVK_Space,
                    "^": kVK_Escape,
                ]
                func report(_ step: String) {
                    let m = tv.markedRange()
                    let marking = tv.hasMarkedText()
                    let r = tv.firstRect(
                        forCharacterRange: marking ? m : tv.selectedRange(),
                        actualRange: nil)
                    let inWin = win.convertFromScreen(r)
                    var glyph = ""
                    if marking, let lm = tv.layoutManager,
                       let tc = tv.textContainer {
                        let g = lm.glyphIndexForCharacter(at: m.location)
                        let frag = lm.lineFragmentRect(forGlyphAt: g,
                                                       effectiveRange: nil)
                        let loc = lm.location(forGlyphAt: g)
                        let box = lm.boundingRect(
                            forGlyphRange: lm.glyphRange(
                                forCharacterRange: m,
                                actualCharacterRange: nil),
                            in: tc)
                        let font = tv.textStorage?.attribute(
                            .font, at: m.location, effectiveRange: nil)
                            as? NSFont
                        glyph = " glyph=\(g) fragX=\(Int(frag.minX)) locX=\(Int(loc.x)) boxX=\(Int(box.minX)) font=\(font?.fontName ?? "-")"
                    }
                    print("snapshot: ime-key \(step)\(glyph) sel=\(tv.selectedRange().location),\(tv.selectedRange().length) marked=\(marking ? "\(m.location),\(m.length)" : "-") panelX=\(Int(inWin.minX)) panelY=\(Int(win.frame.height - inWin.maxY)) text=\(tv.string.replacingOccurrences(of: "\n", with: "|"))")
                }
                for step in script.split(separator: ";") {
                    let parts = step.split(separator: ":", maxSplits: 1)
                    let arg = parts.count > 1 ? String(parts[1]) : ""
                    switch parts.first ?? "" {
                    case "caret":
                        tv.setSelectedRange(NSRange(
                            location: min(Int(arg) ?? 0,
                                          tv.string.utf16.count),
                            length: 0))
                        try? await Task.sleep(for: .milliseconds(150))
                        report(String(step))
                    case "keys":
                        for ch in arg.replacingOccurrences(
                            of: "_", with: " ") {
                            guard let code = codes[ch] else { continue }
                            // Backed by a CGEvent, as the window
                            // server's are: the input method reads it.
                            let src = CGEventSource(
                                stateID: .hidSystemState)
                            for down in [true, false] {
                                if let cg = CGEvent(
                                    keyboardEventSource: src,
                                    virtualKey: CGKeyCode(code),
                                    keyDown: down),
                                   let e = NSEvent(cgEvent: cg) {
                                    win.sendEvent(e)
                                }
                            }
                            try? await Task.sleep(for: .milliseconds(250))
                            report("key \(ch == " " ? "space" : ch == "^" ? "esc" : String(ch))")
                        }
                    default:
                        continue
                    }
                }
                TISSelectInputSource(before)
                try? await Task.sleep(for: .milliseconds(300))
            }
            // A positional tag in the text selected (its editor opens)
            // -- the n-th -- and digits typed into it.
            if let v = env["VALTZ_SNAPSHOT_SELECT_TAG"], let n = Int(v),
               let win = NSApp.windows.first(where: { $0.isVisible }),
               let root = win.contentView?.superview,
               let tv = Self.find(PromptTextView.self, in: root),
               let storage = tv.textStorage {
                var at: [Int] = []
                storage.enumerateAttribute(
                    .attachment,
                    in: NSRange(location: 0, length: storage.length)
                ) { value, range, _ in
                    if value is RefTagAttachment { at.append(range.location) }
                }
                if at.indices.contains(n) {
                    win.makeFirstResponder(tv)
                    tv.setSelectedRange(NSRange(location: at[n], length: 1))
                    let right = tv.selectedRange()
                    try? await Task.sleep(for: .milliseconds(400))
                    FileHandle.standardError.write(Data(
                        "snapshot: tag at=\(at[n]) selection=\(right.location),\(right.length) later=\(tv.selectedRange().location),\(tv.selectedRange().length) tag=\(tv.selectedTag != nil)\n".utf8))
                    if let digits = env["VALTZ_SNAPSHOT_TAG_TYPE"] {
                        for d in digits {
                            tv.insertText(String(d),
                                          replacementRange: tv.selectedRange())
                            try? await Task.sleep(for: .milliseconds(200))
                        }
                    }
                    try? await Task.sleep(for: .milliseconds(300))
                    let pops = NSApp.windows.filter {
                        $0.isVisible && $0.className.contains("Popover")
                    }
                    // The editor drawn by AppKit beside the snapshot
                    // (a popover is a window of its own).
                    if let shot = env["VALTZ_SNAPSHOT"],
                       let v = pops.first?.contentView,
                       let rep = v.bitmapImageRepForCachingDisplay(
                           in: v.bounds) {
                        v.cacheDisplay(in: v.bounds, to: rep)
                        try? rep.representation(using: .png,
                                                properties: [:])?
                            .write(to: URL(fileURLWithPath: shot
                                .replacingOccurrences(of: ".png",
                                                      with: "-editor.png")))
                    }
                    FileHandle.standardError.write(Data(
                        "snapshot: tag selected=\(tv.selectedTag.map { $0.tag.tag } ?? "-") editor=\(pops.map { "#\($0.windowNumber)" }.joined(separator: ","))\n".utf8))
                }
            }
            // Immersive prompt editing (PromptStudio): Markdown drawn
            // or not; the mode entered; prompts opened as tabs
            // ("new:<text>" -- "\n" a line break -- "select:<n>",
            // "close:<n>", from 0); Markdown on the text ("sel:<at>,<n>",
            // "caret:<at>", "bold", "italic", "underline"); and back.
            if let v = env["VALTZ_SNAPSHOT_MARKDOWN"] {
                m.promptMarkdown = v != "0"
            }
            // The stage's corners: "square" or "round" (View › Round
            // Stage Corners).
            if let v = env["VALTZ_SNAPSHOT_STAGE_CORNERS"] {
                m.roundStageCorners = v != "square"
            }
            if env["VALTZ_SNAPSHOT_IMMERSIVE"] != nil {
                withAnimation(AppModel.motion) { m.enterImmersivePrompt() }
                try? await Task.sleep(for: .milliseconds(700))
            }
            // The Prompt Editor's settings popover ("1"; "custom": with
            // Favor's Custom, Tune's options inside it): how many
            // popovers are open, and the popover drawn to
            // <snapshot>-settings.png.
            if let v = env["VALTZ_SNAPSHOT_EDITOR_SETTINGS"],
               m.promptImmersive {
                m.showsGenerationSettings = true
                try? await Task.sleep(for: .milliseconds(700))
                if v == "custom" {
                    m.choosePreference(.custom)
                    if !m.showsTuning { m.toggleTuning() }
                    try? await Task.sleep(for: .milliseconds(900))
                }
                let pops = NSApp.windows.filter {
                    $0.isVisible && $0.className.contains("Popover")
                }
                if let shot = env["VALTZ_SNAPSHOT"],
                   let v = pops.first?.contentView,
                   let rep = v.bitmapImageRepForCachingDisplay(
                       in: v.bounds) {
                    v.cacheDisplay(in: v.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?
                        .write(to: URL(fileURLWithPath: shot
                            .replacingOccurrences(of: ".png",
                                                  with: "-settings.png")))
                }
                FileHandle.standardError.write(Data(
                    "snapshot: editor-settings popovers=\(pops.count) tuning=\(m.showsTuning) size=\(pops.first.map { "\(Int($0.frame.width))x\(Int($0.frame.height))" } ?? "-")\n".utf8))
            }
            // The file panels' folders (FileHistory): "attach:<file>",
            // "export:<file>", "project:<file>" as a panel chose it,
            // "startover", "anonymous" (File › New Anonymous Project),
            // "open:<project>", "print" -- each printing where the
            // three panels would open.
            if let spec = env["VALTZ_SNAPSHOT_FILE_HISTORY"] {
                for step in spec.split(separator: ";").map(String.init) {
                    let kv = step.split(separator: ":", maxSplits: 1)
                        .map(String.init)
                    let arg = kv.count > 1 ? kv[1] : ""
                    switch kv[0] {
                    case "attach", "export", "project":
                        if let k = FileHistory(rawValue: kv[0]) {
                            m.rememberPanel(k, chose: URL(fileURLWithPath: arg))
                        }
                    case "startover": m.startOver()
                    case "anonymous": m.newAnonymousProject()
                    case "open": m.openProject(path: arg)
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(500))
                    let at = FileHistory.allCases.map {
                        "\($0.rawValue)=\(m.panelFolder($0).path)"
                    }.joined(separator: " ")
                    print("snapshot: files \(step) anonymous=\(m.isAnonymous) \(at)")
                }
            }
            // Valtz left for another app (as a person going to the
            // Finder for a LoRA) that many seconds: whether Tune's
            // panel, and the popovers, stay open.
            // "<wait>:<seconds>": a pause first, for Valtz to be brought
            // to the front from outside (a run from a terminal never
            // is, and macOS lets no app take the front itself) --
            // without it, nothing is left.
            if let v = env["VALTZ_SNAPSHOT_LEAVE"],
               let secs = Double(v.split(separator: ":").last ?? "") {
                if v.contains(":"),
                   let pause = Double(v.split(separator: ":")[0]) {
                    print("snapshot: leave-pause pid=\(ProcessInfo.processInfo.processIdentifier)")
                    try? await Task.sleep(for: .seconds(pause))
                }
                let wasActive = NSApp.isActive
                let before = NSApp.windows.filter {
                    $0.isVisible && $0.className.contains("Popover")
                }.count
                // The Finder brought to the front, as a click there
                // would.
                NSRunningApplication.runningApplications(
                    withBundleIdentifier: "com.apple.finder")
                    .first?.activate()
                try? await Task.sleep(for: .seconds(secs))
                let after = NSApp.windows.filter {
                    $0.isVisible && $0.className.contains("Popover")
                }.count
                print("snapshot: left wasActive=\(wasActive) active=\(NSApp.isActive) popovers=\(before)>\(after) tuning=\(m.showsTuning) settings=\(m.showsGenerationSettings)")
            }
            if let spec = env["VALTZ_SNAPSHOT_PROMPT_TABS"] {
                for op in spec.split(separator: ";") {
                    let kv = op.split(separator: ":", maxSplits: 1)
                        .map(String.init)
                    let arg = kv.count > 1 ? kv[1] : ""
                    switch kv[0] {
                    case "new":
                        m.newPromptTab()
                        if !arg.isEmpty {
                            m.setPrompt(arg.replacingOccurrences(
                                of: "\\n", with: "\n"))
                        }
                    case "select":
                        if let n = Int(arg) { m.selectPromptTab(n) }
                    case "close":
                        if let n = Int(arg) { m.requestClosePromptTab(n) }
                    // "open:<asset id>|newest": a text asset selected
                    // in Assets; "type:<text>" typed over the open
                    // tab's words; "retreat" another kind selected;
                    // "shrink" the tab's way back to the box.
                    case "open":
                        let a = arg == "newest"
                            ? m.assets.filter(\.isPrompt)
                                .max { $0.created < $1.created }
                            : m.assets.first { $0.id == arg }
                        if let a { m.openTextAsset(a) }
                    case "type":
                        m.setPrompt(arg.replacingOccurrences(
                            of: "\\n", with: "\n"))
                    case "retreat": m.retreatFromEditor()
                    case "shrink":
                        if m.promptImmersive && !m.activeTabDoNotApply {
                            m.toggleImmersivePrompt()
                        }
                    // A tab's name: "edit:<i>" its header a field (as
                    // a double-click makes it), "keys:<text>" typed
                    // into what has the keyboard, "enter" / "esc";
                    // "rename:<i>:<name>" the model's.
                    case "edit":
                        if let n = Int(arg),
                           m.promptTabs.indices.contains(n) {
                            m.selectPromptTab(n)
                            m.renamingPromptTab = m.promptTabs[n].id
                        }
                    case "keys", "enter", "esc":
                        let chars = kv[0] == "enter" ? "\r"
                            : kv[0] == "esc" ? "\u{1b}" : arg
                        for ch in chars {
                            let code: UInt16 = ch == "\r" ? 36
                                : ch == "\u{1b}" ? 53 : 0
                            if let w = m.editorWindow,
                               let e = NSEvent.keyEvent(
                                   with: .keyDown, location: .zero,
                                   modifierFlags: [], timestamp: 0,
                                   windowNumber: w.windowNumber,
                                   context: nil,
                                   characters: String(ch),
                                   charactersIgnoringModifiers:
                                       String(ch),
                                   isARepeat: false, keyCode: code) {
                                w.sendEvent(e)
                            }
                            try? await Task.sleep(for: .milliseconds(40))
                        }
                    // Typed into its text, as the keyboard types:
                    // the editor being edited.
                    case "typing":
                        if let win = m.editorWindow,
                           let root = win.contentView?.superview,
                           let tv = Self.find(PromptTextView.self,
                                              in: root) {
                            win.makeFirstResponder(tv)
                            tv.setSelectedRange(NSRange(
                                location: tv.string.utf16.count,
                                length: 0))
                            for ch in arg {
                                tv.insertText(String(ch),
                                              replacementRange:
                                                  tv.selectedRange())
                            }
                        }
                    // A locked tab's Duplicate and Edit.
                    case "dup":
                        if let n = Int(arg) { m.duplicatePromptTab(n) }
                    case "rename":
                        let parts = arg.split(separator: ":",
                                              maxSplits: 1,
                                              omittingEmptySubsequences:
                                                  false)
                        if let n = Int(parts.first ?? "") {
                            m.renamePromptTab(
                                n, parts.count > 1 ? String(parts[1]) : "")
                        }
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                    FileHandle.standardError.write(Data(
                        "snapshot: tabs \(op) immersive=\(m.promptImmersive) active=\(m.activePromptTab) tabs=\(m.promptTabs.indices.map { i in "\(i):\(m.tabDoNotApply(i) ? "look" : "edit")\(m.promptTabs[i].readOnly ? "-ro" : "")" }.joined(separator: ",")) shrink=\(m.promptImmersive && !m.activeTabDoNotApply) prompt=\(String(m.prompt.prefix(24)).replacingOccurrences(of: " ", with: "_")) prompts=\(m.assets.filter(\.isPrompt).count) titles=\(m.promptTabs.indices.map { m.promptTabTitle($0) }.joined(separator: "|").replacingOccurrences(of: " ", with: "_")) names=\(m.promptTabs.indices.map { m.promptTabName($0) ?? "-" }.joined(separator: "|").replacingOccurrences(of: " ", with: "_")) editing=\(m.promptTabs.firstIndex { $0.id == m.renamingPromptTab }.map(String.init) ?? "-") promptNames=\(m.assets.filter(\.isPrompt).map { ($0.isNamed ? "*" : "") + $0.name }.joined(separator: "|").replacingOccurrences(of: " ", with: "_")) focus=\(m.editorWindow?.firstResponder.map { String(describing: type(of: $0)) } ?? "-")\n".utf8))
                }
            }
            if let spec = env["VALTZ_SNAPSHOT_MD"],
               let tv = m.promptTextView {
                tv.window?.makeFirstResponder(tv)
                for op in spec.split(separator: ";") {
                    let kv = op.split(separator: ":", maxSplits: 1)
                        .map(String.init)
                    let nums = (kv.count > 1 ? kv[1] : "")
                        .split(separator: ",").compactMap { Int($0) }
                    switch kv[0] {
                    case "sel" where nums.count == 2:
                        tv.setSelectedRange(NSRange(location: nums[0],
                                                    length: nums[1]))
                    case "caret" where nums.count == 1:
                        tv.setSelectedRange(NSRange(location: nums[0],
                                                    length: 0))
                    case "bold": tv.toggleMarkdown(.bold)
                    case "italic": tv.toggleMarkdown(.italic)
                    case "underline": tv.toggleMarkdown(.underline)
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(200))
                }
                FileHandle.standardError.write(Data(
                    "snapshot: md text=\(tv.string.replacingOccurrences(of: "\n", with: "|")) selection=\(tv.selectedRange().location),\(tv.selectedRange().length)\n".utf8))
            }
            if env["VALTZ_SNAPSHOT_EXIT_IMMERSIVE"] != nil {
                withAnimation(AppModel.motion) { m.exitImmersivePrompt() }
                try? await Task.sleep(for: .milliseconds(700))
            }
            // The output size: "portrait", "16:9", "large", or a
            // typed "1200x800" (after a base has brought its own).
            if let o = env["VALTZ_SNAPSHOT_ORIENTATION"],
               let o = Orientation(rawValue: o) { m.setOrientation(o) }
            if let r = env["VALTZ_SNAPSHOT_RATIO"],
               let r = AspectRatio(rawValue: r) { m.aspectRatio = r }
            if let c = env["VALTZ_SNAPSHOT_SIZE_CLASS"],
               let c = SizeClass.allCases.first(where: {
                   "\($0)" == c
               }) { m.sizeChoice = c }
            if let t = env["VALTZ_SNAPSHOT_OUTPUT_SIZE"] {
                try? await Task.sleep(for: .milliseconds(300))
                m.setCustomSize(t)
            }
            // Clicks on the pencil badges of the prompt row's pictures
            // ("0,1": the first, then the second).
            if let list = env["VALTZ_SNAPSHOT_CLICK_BADGE"] {
                for n in list.split(separator: ",").compactMap({ Int($0) }) {
                    try? await Task.sleep(for: .milliseconds(400))
                    m.toggleBase(picture: n)
                }
                try? await Task.sleep(for: .milliseconds(400))
            }
            // The assistant's button with the box empty: the model's
            // prompt template, written for the row ("snapshot: outline
            // model= enabled= lines= text=", its line breaks as ⏎).
            if env["VALTZ_SNAPSHOT_OUTLINE"] != nil {
                try? await Task.sleep(for: .milliseconds(400))
                let enabled = m.assistantEnabled
                m.assistantPressed()
                try? await Task.sleep(for: .milliseconds(400))
                let lines = m.prompt.split(separator: "\n",
                                           omittingEmptySubsequences: false)
                FileHandle.standardError.write(Data(
                    "snapshot: outline model=\(m.outlineModel?.id ?? "-") enabled=\(enabled) lines=\(lines.count) text=\(m.prompt.replacingOccurrences(of: "\n", with: "⏎"))\n".utf8))
            }
            // The assistant's rewrite: "show" waits for the
            // suggestion; "accept" also takes it.
            if let e = env["VALTZ_SNAPSHOT_ENHANCE"] {
                try? await Task.sleep(for: .milliseconds(400))
                m.enhance()
                // "stream": the suggestion caught while it is being
                // written (160 characters in) -- <snapshot>-stream.png,
                // drawn by AppKit -- then waited for as "show" is;
                // "stream-reject": Reject pressed right then.
                if e.hasPrefix("stream") {
                    for _ in 0..<Int(delay * 20)
                    where m.enhanceDraft.count < 160 && m.enhanced == nil {
                        try? await Task.sleep(for: .milliseconds(50))
                    }
                    let n = m.enhanceDraft.count
                    let writing = m.suggestionWriting
                    Self.drawWindow(to: path.replacingOccurrences(
                        of: ".png", with: "-stream.png"))
                    FileHandle.standardError.write(Data(
                        "snapshot: stream draft=\(n) writing=\(writing)\n".utf8))
                    if e == "stream-reject" {
                        m.dismissEnhanced()
                        try? await Task.sleep(for: .seconds(2))
                        FileHandle.standardError.write(Data(
                            "snapshot: rejected job=\(m.enhanceJob ?? "-") suggesting=\(m.suggesting) jobs=\(m.jobs.values.map(\.state).sorted())\n".utf8))
                    }
                }
                for _ in 0..<Int(delay * 4) where m.enhanced == nil {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                // Its speed, the line under it while Show the Helper's
                // Speed is on (VALTZ_SNAPSHOT_AGENT_SPEED=1).
                FileHandle.standardError.write(Data(
                    "snapshot: suggestion-speed shown=\(m.agentShowsSpeed) line=\(m.enhanced?.stats?.line ?? "-")\n".utf8))
                // The box's card: how tall its words stand, how tall
                // they are, its scroll bar ("snapshot: suggestion-card
                // text= doc= bar= cap=").
                try? await Task.sleep(for: .milliseconds(600))
                if let root = NSApp.windows.first(where: \.isVisible)?
                    .contentView?.superview,
                   let sv = Self.find(SuggestionScrollView.self,
                                      in: root) {
                    FileHandle.standardError.write(Data(
                        "snapshot: suggestion-card text=\(Int(sv.frame.height)) doc=\(Int(sv.documentView?.frame.height ?? 0)) bar=\(sv.scrollerStyle == .legacy ? "legacy" : "overlay") cap=\(Int(SuggestionCard.textCap(window: m.editorRoom))) pictureRoom=\(Int(m.pictureRoom))/\(Int(m.editorRoom))\n".utf8))
                }
                if e == "accept" {
                    m.acceptEnhanced()
                    try? await Task.sleep(for: .milliseconds(600))
                    print("snapshot: applied " + ValtzApp.appliedSummary(m))
                }
                // "review": the card's Prompt Editor button.
                if e == "review" {
                    withAnimation(AppModel.motion) {
                        m.enterImmersivePrompt()
                    }
                    try? await Task.sleep(for: .milliseconds(800))
                    FileHandle.standardError.write(Data(
                        "snapshot: suggestion-review immersive=\(m.promptImmersive) suggesting=\(m.suggesting) pane=\(Self.find(SuggestionTextView.self, in: NSApp.windows.first(where: \.isVisible)?.contentView?.superview ?? NSView()).map { $0.string.count } ?? -1)\n".utf8))
                }
            }
            // The suggestion beside the prompt, in the Prompt Editor:
            // words typed at its end ("type:<text>"), then "apply" or
            // "reject" -- its half closes.
            if let spec = env["VALTZ_SNAPSHOT_SUGGESTION"],
               let win = NSApp.windows.first(where: { $0.isVisible }),
               let root = win.contentView?.superview {
                try? await Task.sleep(for: .milliseconds(800))
                for op in spec.split(separator: ";") {
                    let kv = op.split(separator: ":", maxSplits: 1)
                        .map(String.init)
                    switch kv[0] {
                    case "type":
                        if let tv = Self.find(SuggestionTextView.self,
                                              in: root) {
                            win.makeFirstResponder(tv)
                            tv.setSelectedRange(NSRange(
                                location: tv.string.utf16.count,
                                length: 0))
                            tv.insertText(kv.count > 1 ? kv[1] : "",
                                          replacementRange:
                                              tv.selectedRange())
                        }
                    case "apply":
                        m.acceptEnhanced()
                        print("snapshot: applied "
                              + ValtzApp.appliedSummary(m))
                    case "reject": m.dismissEnhanced()
                    default: break
                    }
                    // An event through the app: the undo group the
                    // typing opened ends with it, as with each key.
                    if let ev = NSEvent.otherEvent(
                        with: .applicationDefined, location: .zero,
                        modifierFlags: [], timestamp: 0,
                        windowNumber: 0, context: nil, subtype: 0,
                        data1: 0, data2: 0) {
                        NSApp.postEvent(ev, atStart: false)
                    }
                    try? await Task.sleep(for: .milliseconds(600))
                }
            }
            if env["VALTZ_SNAPSHOT_DRAWER"] != nil {
                withAnimation(AppModel.motion) { m.openPanel = .generate }
            }
            // An asset on the simple stage as if just made: its id, or
            // "newest" / "newest-video" / "newest-image" /
            // "newest-audio".
            // The project set up from the inspector: "still:1024x768",
            // "timeline:832x480", "sound"; its output after a third
            // field, "timeline:832x480:rec2020,30,1,44100,prores4444"
            // (colour, fps, channels, sample rate, intermediate).
            if let spec = env["VALTZ_SNAPSHOT_NEW_PROJECT"] {
                let p = spec.split(separator: ":").map(String.init)
                let wh = (p.count > 1 ? p[1] : "").split(separator: "x")
                    .compactMap { Int($0) }
                var o = OutputSettingsDTO()
                if p.count > 2 {
                    let f = p[2].split(separator: ",").map(String.init)
                    if !f.isEmpty { o.color = f[0] }
                    if f.count > 1, let n = Int64(f[1]) { o.fps = [n, 1] }
                    if f.count > 2, let c = Int(f[2]) { o.channels = c }
                    if f.count > 3, let r = Int(f[3]) { o.sampleRate = r }
                    if f.count > 4 { o.intermediate = f[4] }
                }
                m.setUpProject(p[0] == "still" ? .still
                               : p[0] == "sound" ? .sound : .timeline,
                               width: wh.first ?? 1024,
                               height: wh.last ?? 1024, output: o)
                try? await Task.sleep(for: .milliseconds(600))
            }
            if let which = env["VALTZ_SNAPSHOT_STAGE_ASSET"] {
                let made = m.assets.filter { $0.head > 0 && !$0.isDrawn }
                let a: AssetDTO? = switch which {
                case "newest": made.last
                case "newest-video": made.last { $0.kind == "video" }
                case "newest-image": made.last { $0.kind == "image" }
                case "newest-audio": made.last { $0.kind == "audio" }
                case "project": m.projectViews.first
                default: m.assets.first { $0.id == which }
                }
                if let a { m.putOnStage(a) }
                try? await Task.sleep(for: .milliseconds(500))
                // A frame grabbed where the player is (after a seek):
                // its Grab Frame under the stage.
                if env["VALTZ_SNAPSHOT_GRAB"] != nil {
                    if let f = env["VALTZ_SNAPSHOT_SEEK"].flatMap({
                        Int($0) }) {
                        m.seekVideo(to: f)
                        try? await Task.sleep(for: .milliseconds(600))
                    }
                    m.grabFrame()
                    print("snapshot: grab frame=\(m.videoFrame) refs=\(m.promptAttachments.count)")
                    try? await Task.sleep(for: .milliseconds(600))
                }
                // Continue's menu, opened under its button: its items.
                if env["VALTZ_SNAPSHOT_CONTINUE_MENU"] != nil {
                    m.continueMenuRequest += 1
                    try? await Task.sleep(for: .milliseconds(800))
                    let items = ClipBarMenu.opened?.items.map {
                        $0.title + ($0.subtitle.map { " [\($0)]" } ?? "")
                    } ?? []
                    print("snapshot: continue-menu \(items.joined(separator: " | "))")
                    ClipBarMenu.opened?.cancelTracking()
                }
                // The clip carried on: its Continue under the stage --
                // from its last N seconds (a guide), or "1" as it is.
                if let c = env["VALTZ_SNAPSHOT_CONTINUE"] {
                    if let s = Int(c), s >= 2 {
                        m.continueClip(seconds: s)
                    } else {
                        m.continueClip()
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                }
                // ...and dragged from the stage into the prompt, as the
                // picture to edit.
                // What the stage shows, captured, as the drag does.
                if env["VALTZ_SNAPSHOT_EDIT_STAGE"] != nil {
                    m.addStageToPrompt()
                    try? await Task.sleep(for: .milliseconds(800))
                }
            }
            // The prompt's Clear button ("1"), or steps in turn:
            // "clear;undo;redo;type:<text>;set:<text>" -- undo and
            // redo as the Edit menu's ⌘Z / ⇧⌘Z (undoAny), type as
            // keys at the text's end, set the words put in from
            // outside (as Use in Prompt); the project changed under
            // the box -- "open:<pkg>" another opened (as a drop opens
            // one), "new" a new anonymous one -- "timeline:open|close"
            // View › Timeline (the box out of the window, and back),
            // "focus" the keyboard to the box -- each printing
            // `snapshot: clear-prompt <step> prompt= refs= mentions=
            // undo= redo= focus= project=`.
            if let spec = env["VALTZ_SNAPSHOT_CLEAR_PROMPT"] {
                let steps = spec == "1" ? ["clear"]
                    : spec.split(separator: ";").map(String.init)
                for step in steps {
                    try? await Task.sleep(for: .milliseconds(600))
                    switch step {
                    case "undo": m.undoAny()
                    case "redo": m.redoAny()
                    case "new": m.newAnonymousProject()
                    // The box's own text (promptTextView may be the
                    // suggestion's, last focused).
                    case "focus":
                        if let root = m.editorWindow?.contentView?
                            .superview,
                           let tv = Self.find(PromptTextView.self,
                                              in: root) {
                            tv.window?.makeFirstResponder(tv)
                        }
                    case let s where s.hasPrefix("open:"):
                        m.openDropped(URL(fileURLWithPath:
                            String(s.dropFirst(5))))
                        try? await Task.sleep(for: .milliseconds(1500))
                    case let s where s.hasPrefix("timeline:"):
                        m.showTimeline(s.hasSuffix(":open"))
                    case let s where s.hasPrefix("set:"):
                        m.setPrompt(String(s.dropFirst(4)))
                    case let s where s.hasPrefix("type:"):
                        if let tv = m.promptTextView {
                            tv.window?.makeFirstResponder(tv)
                            tv.setSelectedRange(NSRange(
                                location: tv.string.utf16.count,
                                length: 0))
                            for ch in s.dropFirst(5) {
                                tv.insertText(String(ch),
                                    replacementRange: tv.selectedRange())
                                try? await Task.sleep(
                                    for: .milliseconds(30))
                            }
                        }
                    default: m.clearPrompt()
                    }
                    // An event through the app: the undo group the
                    // step opened ends with it, as with a click or a
                    // key (AppKit ends groups after each event).
                    if let ev = NSEvent.otherEvent(
                        with: .applicationDefined, location: .zero,
                        modifierFlags: [], timestamp: 0,
                        windowNumber: 0, context: nil, subtype: 0,
                        data1: 0, data2: 0) {
                        NSApp.postEvent(ev, atStart: false)
                    }
                    try? await Task.sleep(for: .milliseconds(600))
                    m.refreshTextUndo()
                    let w = m.promptTextView?.window
                    let w2 = w ?? m.editorWindow
                    let focus = w2?.firstResponder is PromptTextView
                        ? "prompt"
                        : w2?.firstResponder is SuggestionTextView
                            ? "suggestion"
                            : (w2?.firstResponder is NSTextView ? "text"
                               : "none")
                    FileHandle.standardError.write(Data(
                        "snapshot: clear-prompt \(step) prompt=\(m.prompt.debugDescription) refs=\(m.promptAttachments.count) mentions=\(m.promptMentions.count) undo=\(m.undoMenuTitle.replacingOccurrences(of: " ", with: "_")) redo=\(m.redoMenuTitle.replacingOccurrences(of: " ", with: "_")) focus=\(focus) project=\(m.windowTitle.replacingOccurrences(of: " ", with: "_")):\(m.assets.count)\n".utf8))
                }
            }
            // The inspector stacked ("1") -- before the asset list's
            // actions, so they play in it -- its sections folded
            // ("info,assets"; neither saved by a snapshot run).
            if let m = model, env["VALTZ_SNAPSHOT_INSPECTOR_STACKED"] == "1" {
                m.inspectorStacked = true
                m.inspectorFolded = Set(
                    (env["VALTZ_SNAPSHOT_INSPECTOR_FOLD"] ?? "")
                        .split(separator: ",")
                        .compactMap { InspectorTab(rawValue: String($0)) })
                // Their shares ("layers:2,assets:1"; none: even).
                m.inspectorWeights = Dictionary(uniqueKeysWithValues:
                    (env["VALTZ_SNAPSHOT_INSPECTOR_WEIGHTS"] ?? "")
                        .split(separator: ",").compactMap { kv in
                            let p = kv.split(separator: ":")
                            guard p.count == 2,
                                  let t = InspectorTab(rawValue: String(p[0])),
                                  let w = Double(p[1]) else { return nil }
                            return (t, w)
                        })
            }
            // The asset list's actions, in turn: "prompt:<id>" (into
            // the prompt's row), "stage:<id>" (dropped on the stage),
            // "capture:all|selected", "folder:<name>" (a new one),
            // "move:<id>:<folder name>", "remove:<id>", "remove-stale".
            if let spec = env["VALTZ_SNAPSHOT_ASSETS"] {
                for step in spec.split(separator: ";") {
                    try? await Task.sleep(for: .milliseconds(600))
                    let p = step.split(separator: ":").map(String.init)
                    var arg = p.count > 1 ? p[1] : ""
                    // "newest-capture": the last capture made.
                    if arg == "newest-capture",
                       let c = m.assets.filter(\.isCapture)
                           .max(by: { $0.created < $1.created }) {
                        arg = c.id
                    }
                    switch p[0] {
                    case "prompt": m.addAssetReferences([arg])
                    // An enhanced prompt's (the newest's) words before
                    // it, put back in the box: Use Original in Prompt.
                    case "original":
                        let e: AssetDTO? = arg == "newest"
                            ? m.assets.last(where: \.isEnhancedPrompt)
                            : m.assets.first { $0.id == arg }
                        if let e, let up = m.promptUpstream(of: e) {
                            m.usePrompt(up)
                        }
                        print("snapshot: original box=\(m.prompt.prefix(60)) "
                              + ValtzApp.appliedSummary(m))
                    case "active":
                        if let a = m.assets.first(where: { $0.id == arg }) {
                            m.setActive(a)
                        }
                    case "stage": m.dropAssetOnStage(arg)
                    // An ASSET OPERATION from the row's menu (DESIGN
                    // §4j): "transcribe:<id>", "summarize:<id>",
                    // "separate:<id>" -- the request, then, its task
                    // done, what it made: a transcript's or a
                    // summary's words; a separation's sounds.
                    case "transcribe", "summarize", "separate":
                        guard let a = m.assets.first(where: { $0.id == arg }),
                              let op = m.operation(p[0]) else {
                            print("snapshot: \(p[0]) offered=\(m.operation(p[0]) != nil) asset=\(arg) -")
                            break
                        }
                        let ready = op.take(a.kind)?.ready == true
                        let items = m.operations(for: a).map(\.name)
                            .joined(separator: ",")
                        let made = m.perform(op, on: a)
                        print("snapshot: operation \(op.name) on=\(a.kind) ready=\(ready) menu=\(items) made=\(made.count) tasks=\(made.map { m.task(of: $0)?.badge ?? "-" }.joined(separator: ","))")
                        let limit = p[0] == "transcribe" ? 1200 : 3000
                        for _ in 0..<limit where !made.isEmpty {
                            try? await Task.sleep(for: .milliseconds(100))
                            m.reloadAssets()
                            let done = made.compactMap { id in
                                m.assets.first { $0.id == id && $0.head > 0 }
                            }
                            guard done.count == made.count else { continue }
                            if op.name == "separate" {
                                let list = done.map { d in
                                    "\(d.id):\(d.name):\(String(format: "%.2f", d.info?.seconds ?? 0))s"
                                }.joined(separator: ",")
                                print("snapshot: separated from=\(a.id) made=\(list) folder=\(done.map { $0.folder ?? "" }.joined(separator: ","))")
                                break
                            }
                            let t = done[0]
                            let words = (t.text ?? "")
                                .replacingOccurrences(of: "\n", with: " | ")
                            if op.name == "transcribe" {
                                print("snapshot: transcribed asset=\(t.id) name=\(t.name) text=\(words)")
                            } else {
                                let from = t.recipe?.inputs?.first {
                                    $0.role == "source" }?.asset ?? "-"
                                print("snapshot: summarized asset=\(t.id) name=\(t.name) from=\(from) text=\(words)")
                            }
                            break
                        }
                    // A single click on a row: the asset VIEWED on
                    // the stage ("view:<id>"); "unview" the list
                    // losing focus -- the active one back.
                    case "view":
                        if let a = m.assets.first(where: { $0.id == arg }) {
                            m.viewAsset(a)
                        }
                    case "unview": m.endViewing()
                    // A blank asset of the New menu ("new:timeline",
                    // "new:still", "new:sound"), made active.
                    case "new":
                        m.newBlankAsset(arg == "still" ? .still
                                        : arg == "sound" ? .sound
                                        : .timeline)
                    // Sound recorded into the blank sound on the stage
                    // ("record:system", "record:mic" -- the first
                    // microphone -- or a source's id), "stop".
                    case "record":
                        m.refreshCaptureSources()
                        let want = p.dropFirst().joined(separator: ":")
                        if let s = m.captureSources.first(where: {
                            $0.id == want
                                || (want == "mic" && $0.kind == "microphone")
                        }) {
                            m.captureSource = s.id
                        }
                        m.startCapture()
                        FileHandle.standardError.write(Data(
                            "snapshot: record source=\(m.captureSource)\n".utf8))
                    case "wait":
                        try? await Task.sleep(
                            for: .seconds(Double(arg) ?? 1))
                        FileHandle.standardError.write(Data(
                            "snapshot: capture recording=\(m.capture.recording) seconds=\(String(format: "%.1f", m.capture.seconds)) level=\(String(format: "%.2f", m.capture.level))\n".utf8))
                    case "stop": m.stopCapture()
                    case "capture":
                        m.captureStage(selectedOnly: arg == "selected")
                    case "folder":
                        if let id = m.createFolder() {
                            m.renameFolder(id, arg)
                        }
                    case "move" where p.count > 2:
                        let f = m.assetFolders.first { $0.name == p[2] }
                        m.moveAsset(arg, to: f?.id ?? "")
                    // "rename:<id>:<name>"; "renaming:<id>" its field
                    // open, as the row's Rename opens it.
                    case "rename" where p.count > 2:
                        m.renameAsset(arg, p[2...].joined(separator: ":"))
                    case "renaming": m.renamingAsset = arg
                    // "fold:<folder name>" -- folded, or open again.
                    case "fold":
                        if let f = m.assetFolders.first(where: {
                            $0.name == arg }) {
                            m.toggleFolder(f.id)
                        }
                    case "remove": m.removeAsset(arg)
                    case "remove-stale": m.removeStaleAssets()
                    // "filter:<words>" the list's filter field (none:
                    // cleared), printing the rows it keeps -- and the
                    // folders, `+` held open by it.
                    case "filter":
                        m.assetFilter = p.count > 1
                            ? p[1...].joined(separator: ":") : ""
                        try? await Task.sleep(for: .milliseconds(400))
                        let w = AppModel.filterWords(m.assetFilter)
                        let kept = m.listedAssets.filter {
                            w.isEmpty || m.passesAssetFilter($0, words: w)
                        }
                        let folders = m.assetFolders.filter { f in
                            kept.contains { $0.folder == f.id }
                        }.map { $0.name + (m.closedFolders.contains($0.id)
                                           && !w.isEmpty ? "+" : "") }
                        FileHandle.standardError.write(Data(
                            "snapshot: assets-filter query=\(m.assetFilter.replacingOccurrences(of: " ", with: "_")) kept=\(kept.count)/\(m.listedAssets.count) rows=\(kept.map(\.name).joined(separator: "|").replacingOccurrences(of: " ", with: "_")) folders=\(folders.joined(separator: "|").replacingOccurrences(of: " ", with: "_"))\n".utf8))
                    default: break
                    }
                }
                try? await Task.sleep(for: .milliseconds(800))
            }
            // The inspector's section ("info", "layers"), and layer work on
            // the picture on the stage: "add;source=<asset>;hide=<layer>;
            // select=<layer>;move=<layer>:<by>" ("0" is layer 0).
            if let m = model, let t = env["VALTZ_SNAPSHOT_INSPECTOR_TAB"],
               let tab = InspectorTab(rawValue: t == "history" ? "assets" : t) {
                m.revealInspector(tab)
            }
            // Canvas Size applied ("1200x700:tl"; the anchor tl, t,
            // tr, l, c, r, bl, b, br; "own": Own Size) and a clip's
            // Length (frames; 0 its own) -- as the popover and the
            // field set them.
            if let m = model, let spec = env["VALTZ_SNAPSHOT_CANVAS"],
               let a = m.stageStack {
                let p = spec.split(separator: ":").map(String.init)
                let wh = p[0].split(separator: "x").compactMap { Int($0) }
                let anchors = ["tl": (0.0, 0.0), "t": (0.5, 0.0),
                               "tr": (1.0, 0.0), "l": (0.0, 0.5),
                               "c": (0.5, 0.5), "r": (1.0, 0.5),
                               "bl": (0.0, 1.0), "b": (0.5, 1.0),
                               "br": (1.0, 1.0)]
                let (ax, ay) = anchors[p.count > 1 ? p[1] : "c"]
                    ?? (0.5, 0.5)
                if spec == "own" {
                    m.resetCanvas(a)
                } else if wh.count == 2 {
                    m.setCanvas(a, width: wh[0], height: wh[1],
                                anchorX: ax, anchorY: ay)
                }
                try? await Task.sleep(for: .seconds(1.5))
            }
            if let m = model, let f = env["VALTZ_SNAPSHOT_TIMELINE"]
                .flatMap({ Int($0) }), let a = m.stageStack {
                m.setTimeline(a, frames: f)
                try? await Task.sleep(for: .seconds(1.5))
            }
            // Information › Canvas › Change…, open.
            if let m = model, env["VALTZ_SNAPSHOT_CANVAS_SIZE"] != nil {
                m.revealInspector(.info)
                try? await Task.sleep(for: .milliseconds(500))
                m.showsCanvasSize = true
            }
            if let m = model, let spec = env["VALTZ_SNAPSHOT_LAYER_OPS"] {
                for step in spec.split(separator: ";") {
                    let kv = step.split(separator: "=", maxSplits: 1)
                        .map(String.init)
                    let arg = kv.count > 1 ? kv[1] : ""
                    switch kv[0] {
                    case "add": m.addLayer()
                    case "source":
                        if let core = m.core, let pid = m.projectId,
                           let pic = m.stageStack {
                            _ = core.layerOp(project: pid, asset: pic.id,
                                             "source", layer: m.activeLayer,
                                             extra: ["source": arg])
                            m.layersChanged()
                        }
                    // A file dropped on the selected layer's row.
                    case "drop":
                        m.setLayerSource(
                            m.activeLayer,
                            from: URL(fileURLWithPath: arg))
                        try? await Task.sleep(for: .seconds(2))
                    // The minus button on that layer ("0": layer 0).
                    case "remove": m.removeLayer(Self.layerId(arg))
                    case "hide": m.setLayerVisible(Self.layerId(arg), false)
                    case "show": m.setLayerVisible(Self.layerId(arg), true)
                    case "select": m.selectLayer(Self.layerId(arg))
                    case "move":
                        let p = arg.split(separator: ":").map(String.init)
                        if p.count == 2, let by = Int(p[1]) {
                            m.moveLayer(Self.layerId(p[0]), by: by)
                        }
                    // Its layers in its place (a layer showing a
                    // composition).
                    case "decompose":
                        m.decomposeLayer(Self.layerId(arg))
                    // Its row's menu's Duplicate Layer.
                    case "dup":
                        m.duplicateLayer(Self.layerId(arg))
                    // The section's find: "find" its magnifier (opened;
                    // "find=<words>" those put in), "ftype=<words>" typed
                    // into it, "fkey=down|up|enter|esc" a key posted to
                    // it, "fgo=<n>" the n-th match clicked -- each printing
                    // what it lists, the mark, the layer selected and
                    // whether its row is in sight in the list.
                    // "sleep=<s>": held so long (for screencapture -l).
                    case "sleep":
                        try? await Task.sleep(
                            for: .seconds(Double(arg) ?? 1))
                    // "fold=<folder name>": a layer folder folded or
                    // opened, as its chevron does.
                    case "fold":
                        if let f = m.stageLayerFolders.first(where: {
                            $0.name == arg }) {
                            m.toggleFolderFolded(f.id)
                        }
                    case "find", "ftype", "fkey", "fgo":
                        let pop = {
                            NSApp.windows.first {
                                $0.isVisible && String(describing: type(of: $0))
                                    .contains("Popover")
                            }
                        }
                        func post(_ ch: String, _ code: UInt16) {
                            guard let w = pop(), let e = NSEvent.keyEvent(
                                with: .keyDown, location: .zero,
                                modifierFlags: [], timestamp: 0,
                                windowNumber: w.windowNumber, context: nil,
                                characters: ch,
                                charactersIgnoringModifiers: ch,
                                isARepeat: false, keyCode: code) else {
                                return
                            }
                            w.sendEvent(e)
                        }
                        switch kv[0] {
                        case "find":
                            m.layerFind = ""
                            m.layerFindOpen = true
                            try? await Task.sleep(for: .milliseconds(500))
                            if !arg.isEmpty { m.layerFind = arg }
                        case "ftype":
                            for ch in arg {
                                post(String(ch), 0)
                                try? await Task.sleep(for: .milliseconds(40))
                            }
                        case "fkey":
                            let k: (String, UInt16) = switch arg {
                            case "down": ("\u{F701}", 125)
                            case "up": ("\u{F700}", 126)
                            case "esc": ("\u{1b}", 53)
                            default: ("\r", 36)
                            }
                            post(k.0, k.1)
                        default:
                            let found = m.layerFindMatches
                            if let n = Int(arg), found.indices.contains(n) {
                                m.goToLayer(found[n].id)
                            }
                        }
                        try? await Task.sleep(for: .milliseconds(900))
                        let list = m.layerListFrame
                        let row = m.activeLayerRowFrame
                        func r(_ f: CGRect) -> String {
                            "\(Int(f.minX)),\(Int(f.minY)) \(Int(f.width))x\(Int(f.height))"
                        }
                        FileHandle.standardError.write(Data(
                            "snapshot: layer-find \(step) open=\(m.layerFindOpen) popover=\(pop() != nil) query=\(m.layerFind.replacingOccurrences(of: " ", with: "_")) matches=\(m.layerFindMatches.map { ($0.id.isEmpty ? "0" : $0.id) + ":" + $0.title.replacingOccurrences(of: " ", with: "_") }.joined(separator: ",")) mark=\(m.layerFindMark) selected=\(m.activeLayer.isEmpty ? "0" : m.activeLayer) inSight=\(list.insetBy(dx: -1, dy: -1).contains(row)) row=\(r(row)) list=\(r(list))\n".utf8))
                    // A still's pages (DESIGN §6a): one added after
                    // the page shown, that page removed, the stage at
                    // a page ("page=2", from 1), the selected layer's
                    // pages ("pages=this|from|every").
                    case "page-add": m.addPage()
                    case "page-remove": m.removePage()
                    case "page":
                        if let n = Int(arg) { m.goToPage(n - 1) }
                    case "pages":
                        m.setLayerPages(m.activeLayer,
                                        arg == "every" ? .every
                                        : arg == "from" ? .fromHere
                                        : .thisPage)
                    // A key toggled on the selected layer's track at
                    // the page or frame shown ("key=place").
                    case "key":
                        m.toggleKey(arg == "adjust" ? .adjust
                                    : arg == "turn" ? .turn : .place)
                    // The Crop panel's values ("crop=offset_x:0.3").
                    case "crop":
                        let p = arg.split(separator: ":")
                        if p.count == 2, let v = Double(p[1]) {
                            m.setCropValues([String(p[0]): v])
                        }
                    case "adjust":
                        let p = arg.split(separator: ":")
                        if p.count == 2, let v = Double(p[1]),
                           let k = ImageAdjustments.Key(
                               rawValue: String(p[0])) {
                            m.setAdjustment(k, v)
                        }
                    // An asset of the list put on the stage: a new
                    // layer ("newest-image", or an id).
                    case "place":
                        let id = arg == "newest-image"
                            ? m.assets.filter {
                                $0.kind == "image" && !$0.isDrawn
                            }.max { $0.created < $1.created }?.id
                            : arg
                        if let id { m.dropAssetOnStage(id) }
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                }
            }
            // Clicks posted to the window ("x,y" in points from its top
            // left; ";" between), as the pointer's would come: each
            // prints what has the focus after it.
            // The status bar's context menu, opened by a right-click
            // posted on the bar: its items printed (+ checked), then
            // the one named picked ("-": only looked at).
            if let m = model,
               let spec = env["VALTZ_SNAPSHOT_STATUS_MENU"] {
                try? await Task.sleep(for: .seconds(1))
                for pick in spec.split(separator: ";").map(String.init) {
                    let items = await Self.statusMenu(
                        pick: pick == "-" ? nil : pick)
                    try? await Task.sleep(for: .milliseconds(400))
                    FileHandle.standardError.write(Data(
                        "snapshot: status-menu pick=\(pick) items=\(items) shown=\(Self.statusItems(m))\n".utf8))
                }
            }
            if let m = model, let spec = env["VALTZ_SNAPSHOT_CLICKS"] {
                for c in spec.split(separator: ";") {
                    // "hint": the edit-base suggestion waited for.
                    if c == "hint" {
                        for _ in 0..<50 where m.baseHint == nil {
                            try? await Task.sleep(for: .milliseconds(100))
                        }
                        try? await Task.sleep(for: .milliseconds(400))
                        FileHandle.standardError.write(Data(
                            "snapshot: hint shown=\(m.baseHint != nil)\n".utf8))
                        continue
                    }
                    let xy = c.split(separator: ",").compactMap {
                        Double($0)
                    }
                    guard xy.count == 2 else { continue }
                    Self.postClick(x: xy[0], y: xy[1])
                    try? await Task.sleep(for: .milliseconds(500))
                    m.refreshTextUndo()
                    let fr = NSApp.windows.first { $0.isVisible }?
                        .firstResponder
                    FileHandle.standardError.write(Data(
                        "snapshot: click \(Int(xy[0])),\(Int(xy[1])) focus=\(fr.map { String(describing: type(of: $0)) } ?? "-") undo=\(m.undoMenuTitle.replacingOccurrences(of: " ", with: "_"))\n".utf8))
                }
            }
            // Drags posted to the window ("x,y>x2,y2" in points from
            // its top left; ";" between), as the pointer's would come:
            // each prints the stacked inspector's shares after it.
            if let m = model, let spec = env["VALTZ_SNAPSHOT_DRAGS"] {
                for d in spec.split(separator: ";") {
                    // "x,y*2": a double-click there.
                    if d.hasSuffix("*2") {
                        let xy = d.dropLast(2).split(separator: ",")
                            .compactMap { Double($0) }
                        if xy.count == 2 {
                            await Self.postDoubleClick(x: xy[0], y: xy[1])
                            try? await Task.sleep(for: .milliseconds(600))
                            FileHandle.standardError.write(Data(
                                "snapshot: double-click \(d)\n".utf8))
                        }
                        continue
                    }
                    let ends = d.split(separator: ">").map {
                        $0.split(separator: ",").compactMap { Double($0) }
                    }
                    guard ends.count == 2, ends[0].count == 2,
                          ends[1].count == 2 else { continue }
                    await Self.postDrag(from: (ends[0][0], ends[0][1]),
                                        to: (ends[1][0], ends[1][1]))
                    try? await Task.sleep(for: .milliseconds(500))
                    FileHandle.standardError.write(Data(
                        "snapshot: drag \(d) panels=\(Self.panelSummary(m))\n".utf8))
                }
            }
            // The project as a document (DESIGN §5b), in turn: "undo",
            // "redo", "save", "saveas:<path>", "revert" -- each prints
            // `snapshot: edit <step> dirty= undo= redo=`.
            if let m = model, let spec = env["VALTZ_SNAPSHOT_EDIT"] {
                for step in spec.split(separator: ";").map(String.init) {
                    try? await Task.sleep(for: .milliseconds(600))
                    if step == "undo" {
                        m.undoProject()
                    } else if step == "redo" {
                        m.redoProject()
                    } else if step == "save" {
                        // Untitled, Save is Save As: its panel would
                        // wait for a person.
                        if !m.document.untitled && !m.isAnonymous {
                            m.saveDocument()
                        }
                    } else if step.hasPrefix("saveas:") {
                        m.save(as: URL(fileURLWithPath: String(
                            step.dropFirst("saveas:".count))))
                    } else if step == "revert" {
                        m.revertDocument(confirm: false)
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                    FileHandle.standardError.write(Data(
                        "snapshot: edit \(step) dirty=\(m.document.dirty) undo=\(m.document.undo ?? "-") redo=\(m.document.redo ?? "-") title=\(m.windowTitle.replacingOccurrences(of: " ", with: "_"))\n".utf8))
                }
            }
            // A file dropped on the stage: instantiated at the
            // selected layer.
            if let m = model, let f = env["VALTZ_SNAPSHOT_STAGE_DROP"] {
                _ = m.dropFilesOnStage([URL(fileURLWithPath: f)])
                try? await Task.sleep(for: .seconds(2))
            }
            // The GUIDES (their marks beside the stage): "show" (View
            // › Show Guides), "add:h@120" / "add:v@300" (the "+"'s,
            // at a pixel), "move:h1@200", "remove:v0", "reset" (View ›
            // Reset Guides), "edit:h0" (its number typed over),
            // "click:h0" (its number clicked through the window),
            // "type:150" / "enter" / "esc" (keys to what has the
            // keyboard), "key:up|down|sup|sdown" (an arrow, ⇧),
            // "minus:h0" (its "−" clicked), "tdrag:h0:40" (its
            // triangle dragged 40 points down -- right for a
            // vertical one's), "zoom:in|out|fit", "wait:1" --
            // each printing `snapshot: guides <step> set= byHand=
            // canvas= rect= marks= editing= dirty=`.
            if let m = model, let spec = env["VALTZ_SNAPSHOT_GUIDE_OPS"] {
                for step in spec.split(separator: ";").map(String.init) {
                    let kv = step.split(separator: ":", maxSplits: 1)
                        .map(String.init)
                    let arg = kv.count > 1 ? kv[1] : ""
                    func ref(_ s: String) -> GuideRef? {
                        guard let k = s.first, k == "h" || k == "v",
                              let i = Int(s.dropFirst()) else {
                            return nil
                        }
                        return GuideRef(horizontal: k == "h", index: i)
                    }
                    let at = arg.split(separator: "@").map(String.init)
                    var wait = 0.6
                    switch kv[0] {
                    case "show": m.showsGuides = arg != "off"
                    case "add":
                        if at.count == 2, let px = Double(at[1]) {
                            m.addGuide(horizontal: at[0] == "h", at: px)
                        }
                    case "move":
                        if at.count == 2, let r = ref(at[0]),
                           let px = Double(at[1]) {
                            m.moveGuide(r, to: px)
                        }
                    case "remove":
                        if let r = ref(arg) { m.removeGuide(r) }
                    case "reset": m.resetGuides()
                    case "edit": m.guideEditing = ref(arg)
                    // The stage zoomed as the title bar's buttons
                    // zoom it: "zoom:in", "zoom:out", "zoom:fit".
                    case "zoom":
                        m.zoom(arg == "in" ? .zoomIn
                               : arg == "out" ? .zoomOut : .fit)
                    case "click", "minus":
                        if let r = ref(arg),
                           let p = Self.guideMarkPoint(m, r,
                                                       part: kv[0]) {
                            await Self.postDrag(from: (p.x, p.y),
                                                to: (p.x, p.y))
                        }
                    // Its triangle dragged so many points along its
                    // edge ("tdrag:h0:40", down or right), through
                    // the window.
                    case "tdrag":
                        let p = arg.split(separator: ":")
                        if p.count == 2, let r = ref(String(p[0])),
                           let d = Double(p[1]),
                           let a = Self.guideMarkPoint(m, r,
                                                       part: "triangle") {
                            let (x, y) = (Double(a.x), Double(a.y))
                            let b: (Double, Double) = r.horizontal
                                ? (x, y + d) : (x + d, y)
                            await Self.postDrag(from: (x, y), to: b)
                        }
                    // An arrow to what has the keyboard: "key:up",
                    // "down", "sup" / "sdown" with ⇧.
                    case "key":
                        let up = arg.hasSuffix("up")
                        if let w = m.editorWindow,
                           let ev = NSEvent.keyEvent(
                               with: .keyDown, location: .zero,
                               modifierFlags: arg.hasPrefix("s")
                                   ? [.numericPad, .function, .shift]
                                   : [.numericPad, .function],
                               timestamp: 0,
                               windowNumber: w.windowNumber,
                               context: nil,
                               characters: up ? "\u{F700}" : "\u{F701}",
                               charactersIgnoringModifiers:
                                   up ? "\u{F700}" : "\u{F701}",
                               isARepeat: false,
                               keyCode: up ? 126 : 125) {
                            NSApp.postEvent(ev, atStart: false)
                        }
                    case "type": Self.sendKeys(m, arg)
                    case "enter": Self.sendKeys(m, "\r")
                    case "esc": Self.sendKeys(m, "\u{1b}")
                    case "wait": wait = Double(arg) ?? 1
                    default: break
                    }
                    try? await Task.sleep(for: .seconds(wait))
                    print("snapshot: guides \(step) \(Self.guideSummary(m))")
                }
            }
            // Markup, through the pointer as the stage sends it
            // (canvas pixels): "tool=rect|color=#ff0000ff|drag=10,10;
            // 200,120|click=50,50|key=delete|text=Hi|pixels|..."
            if let m = model, let spec = env["VALTZ_SNAPSHOT_MARKUP_OPS"] {
                m.markupOpen = true
                try? await Task.sleep(for: .milliseconds(300))
                for step in spec.split(separator: "|") {
                    await ValtzApp.markupStep(m, String(step))
                    try? await Task.sleep(for: .milliseconds(450))
                }
            }
            if let panel = env["VALTZ_SNAPSHOT_PANEL"] {
                withAnimation(AppModel.motion) {
                    m.openPanel = panel == "adjust" ? .adjust
                        : panel == "crop" ? .crop
                        : panel == "trim" ? .trim : .generate
                }
            }
            if env["VALTZ_SNAPSHOT_NAV"] != nil { m.toggleNav() }
            // Adjustments on the picture to edit, before Start
            // ("exposure=0.4,vibrance=0.3"): the model gets them.
            if let spec = env["VALTZ_SNAPSHOT_BASE_ADJUST"] {
                for _ in 0..<40 where !m.adjustsBase {
                    try? await Task.sleep(for: .milliseconds(100))
                }
                m.adjustments = Self.adjustments(spec)
                withAnimation(AppModel.motion) { m.openPanel = .adjust }
                try? await Task.sleep(for: .seconds(1))
            }
            // The Crop panel: values on the picture on the stage
            // ("scale=0.7,rotate=10,pad_a=0"), editing, the guides; a
            // clip's marks ("12,35") and where its player is.
            if let spec = env["VALTZ_SNAPSHOT_CROP"] {
                for _ in 0..<40 where !m.canAdjust {
                    try? await Task.sleep(for: .milliseconds(100))
                }
                var kv: [String: Double] = [:]
                for (k, v) in Self.adjustments(spec).json { kv[k] = v }
                for part in spec.split(separator: ",") {
                    let p = part.split(separator: "=")
                    if p.count == 2, let v = Double(p[1]) {
                        kv[String(p[0])] = v
                    }
                }
                m.setCropValues(kv)
            }
            // The Crop card's wheels turned, as a drag of so many
            // points does ("offx:40;zoomx:50;lock:off;zoomy:-30"),
            // each printing the placement.
            if let spec = env["VALTZ_SNAPSHOT_CROP_JOG"] {
                for step in spec.split(separator: ";") {
                    let kv = step.split(separator: ":")
                    guard kv.count == 2 else { continue }
                    if kv[0] == "lock" {
                        m.cropZoomLocked = kv[1] != "off"
                    } else if let pts = Double(kv[1]) {
                        let f: CropField = switch kv[0] {
                        case "offx": .offsetX
                        case "offy": .offsetY
                        case "zoomy": .scaleY
                        default: .scaleX
                        }
                        m.nudgeCropPlacement(f, by: pts * f.jogPerPoint)
                    }
                    try? await Task.sleep(for: .milliseconds(300))
                    let c = m.crop
                    let v = [c.offsetX, c.offsetY, c.scaleX, c.scaleY]
                        .map { String(format: "%.3f", $0) }
                        .joined(separator: ",")
                    print("snapshot: crop-jog \(step) crop=\(v) locked=\(m.cropZoomLocked)")
                }
            }
            if let g = env["VALTZ_SNAPSHOT_GUIDES"] {
                m.showsGuides = g != "0"
            }
            // A sound's marks typed as times ("1.25,0:03.5"), as its
            // Trim panel's fields take them.
            if let t = env["VALTZ_SNAPSHOT_TRIM_TIMES"] {
                let parts = t.split(separator: ",").map(String.init)
                if parts.count == 2 {
                    m.setMark(in: true, typed: parts[0])
                    m.setMark(in: false, typed: parts[1])
                }
            }
            // The selected layer's start on the timeline, a frame.
            if let t = env["VALTZ_SNAPSHOT_TRIM_START"], let f = Int(t) {
                m.trimOffset = f
                m.persistTrim()
                try? await Task.sleep(for: .milliseconds(600))
            }
            // ... or typed as its field takes it ("00:00:01:12", "#36",
            // "1.5").
            if let t = env["VALTZ_SNAPSHOT_TRIM_START_TEXT"] {
                let took = m.setStart(typed: t)
                m.persistTrim()
                FileHandle.standardError.write(Data(
                    "snapshot: start typed=\(t) took=\(took) offset=\(m.trimOffset)\n".utf8))
                try? await Task.sleep(for: .milliseconds(600))
            }
            // Its speed and sound, as the Trim panel's rows set them:
            // "2", "50%"; "volume=0.5,pitch=-2,follow=1".
            if let t = env["VALTZ_SNAPSHOT_SPEED"],
               let v = SpeedRow.parse(t) {
                m.setLayerSpeed(v)
                m.persistTrim()
                try? await Task.sleep(for: .milliseconds(600))
            }
            if let spec = env["VALTZ_SNAPSHOT_SOUND"] {
                for part in spec.split(separator: ",") {
                    let p = part.split(separator: "=")
                    guard p.count == 2, let v = Double(p[1]) else {
                        continue
                    }
                    switch p[0] {
                    case "volume": m.setLayerVolume(v)
                    case "pitch": m.setLayerPitch(v)
                    case "follow": m.pitchFollowsSpeed = v != 0
                    default: break
                    }
                }
                m.persistTrim()
                try? await Task.sleep(for: .milliseconds(600))
            }
            if let t = env["VALTZ_SNAPSHOT_TRIM"] {
                let f = t.split(separator: ",").compactMap { Int($0) }
                if f.count == 2 {
                    m.seekVideo(to: f[0])
                    try? await Task.sleep(for: .milliseconds(600))
                    m.videoFrame = f[0]
                    m.setMark(in: true)
                    // The mark-in moved the clip on the timeline.
                    try? await Task.sleep(for: .milliseconds(600))
                    let out = m.timelineFrame(ofSource: f[1])
                    m.seekVideo(to: out)
                    try? await Task.sleep(for: .milliseconds(600))
                    m.videoFrame = out
                    m.setMark(in: false)
                }
            }
            // The Trim panel's marks set where the player is, and its
            // transport: "seek:2500;in;play;wait:1;pause;out", each
            // printing where the player is on the timeline and in
            // the clip.
            if let spec = env["VALTZ_SNAPSHOT_MARKS"] {
                for step in spec.split(separator: ";").map(String.init) {
                    let kv = step.split(separator: ":").map(String.init)
                    AudioScrubber.lastFrames = 0
                    switch kv[0] {
                    case "seek":
                        m.seekVideo(to: Int(kv.last ?? "") ?? 0)
                    case "step":
                        m.stepVideo(Int(kv.last ?? "") ?? 1)
                    case "begin", "end":
                        m.seekClipEdge(end: kv[0] == "end")
                    // An arrow key as the keyboard sends it (⇧ with
                    // "sleft" / "sright").
                    case "key" where kv.last == "space":
                        Self.postSpace(m)
                    case "key":
                        let a = kv.last ?? ""
                        let left = a.hasSuffix("left")
                        if let w = m.editorWindow,
                           let ev = NSEvent.keyEvent(
                               with: .keyDown, location: .zero,
                               modifierFlags: a.hasPrefix("s")
                                   ? [.shift, .numericPad, .function]
                                   : [.numericPad, .function],
                               timestamp: 0,
                               windowNumber: w.windowNumber,
                               context: nil,
                               characters: left ? "\u{F702}" : "\u{F703}",
                               charactersIgnoringModifiers:
                                   left ? "\u{F702}" : "\u{F703}",
                               isARepeat: false,
                               keyCode: left ? 123 : 124) {
                            NSApp.postEvent(ev, atStart: false)
                        }
                    case "in", "out":
                        m.setMark(in: kv[0] == "in")
                    case "play": m.playVideo(rate: 1)
                    case "pause": m.playVideo(rate: 0)
                    default: break
                    }
                    let wait = kv[0] == "wait"
                        ? Double(kv.last ?? "") ?? 1 : 0.8
                    try? await Task.sleep(for: .seconds(wait))
                    // One interpolation: a chain of + took the type
                    // checker past its time limit.
                    let markIn = m.trim.markIn ?? -1
                    let markOut = m.trim.markOut ?? -1
                    let stage = m.currentClip?.name ?? "-"
                    let heard = "\(AudioScrubber.lastFrames)@\(String(format: "%.3f", AudioScrubber.lastPeak))"
                    print("snapshot: mark \(step) frame=\(m.videoFrame) clip=\(m.sourceFrameAtPlayhead) in=\(markIn) out=\(markOut) rate=\(m.videoRate) heard=\(heard) stage=\(stage)")
                }
            }
            // The TIMELINE in the prompt's place, in turn: "open",
            // "close", "rows=+1", "zoom=in|out|fit|4", "seek=48",
            // "next", "prev", "step=-1", "key=left", "play", "pause",
            // "scissors=on", "cut=1@60" (layer 0 is "0"),
            // "slide=1:24", "drop=newest-audio@1:30" (a row's layer, or
            // "top"; "newest-composition" New's last), and the pointer
            // through the view itself --
            // "click=60@0" (a frame on a row, from the top),
            // "drag=0:12>40" (a row's clip by its frame 12, to 40),
            // "undo", "redo", "panel=trim" (a card under it; "none"),
            // "continue=2" (the clip's Continue from its last 2 s),
            // "editor" (the Prompt Editor in or out),
            // "eye=1" (a row's eye clicked), "fold=0" (a folder's
            // fold), "hdrag=0>2" (a header dragged to the gap above
            // row 2; "hdrag=3>1.5" onto row 1's middle),
            // TRANSITIONS: "cmd=down|up" (⌘ through the app's event
            // path: the selected layer's overlaps highlighted),
            // "cmdclick=96@1" (a click with ⌘ held), "tmenu=0" (a
            // transition's menu, its items printed; "tmenu=0:dip-black"
            // one picked, "remove" its last), "thit=0" (what a click on
            // its arrow's badge finds),
            // KEYS: "kdrag=0:30>40" (a key's dot dragged from its frame
            // to another, through the view; "khold=" the same left
            // held, for the capture), "ktip=0:30" (its bar's tooltip,
            // zoomed out),
            // "wait=2" -- each printing the timeline as it is.
            if let spec = env["VALTZ_SNAPSHOT_TIMELINE_VIEW"] {
                func layerArg(_ s: Substring) -> String {
                    s == "0" ? "" : String(s)
                }
                for step in spec.split(separator: ";").map(String.init) {
                    let kv = step.split(separator: "=", maxSplits: 1)
                    let key = kv.first.map(String.init) ?? ""
                    let arg = kv.count > 1 ? String(kv[1]) : ""
                    var wait = 0.8
                    switch key {
                    case "open", "close":
                        m.showTimeline(key == "open")
                    case "rows":
                        if arg.hasPrefix("+") || arg.hasPrefix("-") {
                            m.timelineRows(by: Int(arg) ?? 0)
                        } else if let n = Int(arg) {
                            m.timelineRows(by: n - m.timelineRows)
                        }
                    case "zoom":
                        m.zoomTimeline(to: arg == "in"
                            ? m.timelineZoom * 1.6
                            : arg == "out" ? m.timelineZoom / 1.6
                            : arg == "fit" ? 1 : Double(arg) ?? 1)
                    case "seek": m.timelineSeek(Int(arg) ?? 0)
                    case "next", "prev": m.timelineEvent(next: key == "next")
                    case "step": m.timelineStep(Int(arg) ?? 1)
                    case "play": m.playVideo(rate: 1)
                    case "pause": m.playVideo(rate: 0)
                    case "scissors": m.timelineCutting = arg != "off"
                    case "key" where arg == "space":
                        Self.postSpace(m)
                    case "key":
                        // "cleft" / "cright": with ⌘ (the previous / next
                        // event); "sleft" / "sright" with ⇧ (ten).
                        let left = arg.hasSuffix("left")
                        var mods: NSEvent.ModifierFlags = [.numericPad,
                                                           .function]
                        if arg.hasPrefix("c") { mods.insert(.command) }
                        if arg.hasPrefix("s") { mods.insert(.shift) }
                        if let w = m.editorWindow,
                           let ev = NSEvent.keyEvent(
                               with: .keyDown, location: .zero,
                               modifierFlags: mods,
                               timestamp: 0,
                               windowNumber: w.windowNumber,
                               context: nil,
                               characters: left ? "\u{F702}" : "\u{F703}",
                               charactersIgnoringModifiers:
                                   left ? "\u{F702}" : "\u{F703}",
                               isARepeat: false,
                               keyCode: left ? 123 : 124) {
                            NSApp.postEvent(ev, atStart: false)
                        }
                    case "cut", "slide":
                        let p = arg.split(separator: key == "cut" ? "@"
                                                                  : ":")
                        if p.count == 2, let f = Int(p[1]) {
                            if key == "cut" {
                                m.cutTimelineLayer(layerArg(p[0]), at: f)
                            } else {
                                m.slideTimelineLayer(layerArg(p[0]),
                                                     to: f)
                            }
                        }
                        wait = 1.5
                    case "drop":
                        let p = arg.split(separator: "@")
                        let made = m.assets.filter {
                            $0.head > 0 && !$0.isDrawn
                        }
                        let a: AssetDTO? = switch p.first ?? "" {
                        case "newest-video":
                            made.last { $0.kind == "video" }
                        case "newest-image":
                            made.last { $0.kind == "image" }
                        case "newest-audio":
                            made.last { $0.kind == "audio" }
                        // The composition made last (New's).
                        case "newest-composition":
                            m.assets.filter(\.isComposition)
                                .max { $0.created < $1.created }
                        default:
                            m.assets.first { $0.id == p.first ?? "" }
                        }
                        let at = p.count > 1
                            ? p[1].split(separator: ":") : []
                        if let a, at.count == 2, let f = Int(at[1]) {
                            m.dropOnTimeline(
                                asset: a.id,
                                above: at[0] == "top" ? nil
                                    : layerArg(at[0]),
                                at: f)
                        }
                        wait = 1.5
                    // A row's eye clicked ("eye=1"), its header
                    // dragged to the gap above another ("hdrag=0>2";
                    // rows from the top, the gap under the last is
                    // their count).
                    // A header's name: "hname=<row>" double-clicked
                    // through the view, "type=<text>" typed into what
                    // has the keyboard, "enter" / "esc".
                    case "hname":
                        if let r = Int(arg),
                           let pt = TimelineNSView.shown?
                               .windowPoint(header: r) {
                            await Self.postDoubleClick(x: pt.x, y: pt.y)
                        }
                    case "type", "enter", "esc":
                        Self.sendKeys(m, key == "enter" ? "\r"
                                      : key == "esc" ? "\u{1b}" : arg)
                    case "eye", "hdrag", "fold", "lock":
                        let v = TimelineNSView.shown
                        let p = arg.split(separator: ">")
                            .compactMap { Int($0) }
                        if key == "lock", let r = p.first,
                           let pt = v?.windowPoint(header: r, lock: true) {
                            await Self.postDrag(from: (pt.x, pt.y),
                                                to: (pt.x, pt.y))
                        } else if key == "fold", let r = p.first,
                           let pt = v?.windowPoint(header: r, fold: true) {
                            await Self.postDrag(from: (pt.x, pt.y),
                                                to: (pt.x, pt.y))
                        } else if key == "eye", let r = p.first,
                           let pt = v?.windowPoint(header: r, eye: true) {
                            await Self.postDrag(from: (pt.x, pt.y),
                                                to: (pt.x, pt.y))
                        } else if let i = p.first,
                                  let g = arg.split(separator: ">")
                                      .last.flatMap({ Double($0) }),
                                  let a = v?.windowPoint(header: i),
                                  let b = v?.windowPoint(header: i,
                                                         gap: g) {
                            await Self.postDrag(from: (a.x, a.y),
                                                to: (b.x, b.y))
                        }
                        wait = 1.5
                    case "cmd":
                        if let w = m.editorWindow,
                           let ev = NSEvent.keyEvent(
                               with: .flagsChanged, location: .zero,
                               modifierFlags: arg == "up" ? []
                                   : [.command],
                               timestamp: ProcessInfo.processInfo
                                   .systemUptime,
                               windowNumber: w.windowNumber,
                               context: nil, characters: "",
                               charactersIgnoringModifiers: "",
                               isARepeat: false, keyCode: 55) {
                            NSApp.postEvent(ev, atStart: false)
                        }
                    case "cmdclick":
                        let p = arg.split(separator: "@")
                        if p.count == 2, let f = Int(p[0]),
                           let r = Int(p[1]),
                           let pt = TimelineNSView.shown?.windowPoint(
                               frame: f, row: r) {
                            await Self.postDrag(from: (pt.x, pt.y),
                                                to: (pt.x, pt.y),
                                                flags: [.command])
                        }
                        wait = 1.5
                    case "tmenu":
                        let p = arg.split(separator: ":")
                        if let n = Int(p.first ?? ""),
                           let menu = TimelineNSView.shown?
                               .transitionMenu(n) {
                            let items = menu.items.filter {
                                !$0.isSeparatorItem
                            }.map {
                                ($0.identifier?.rawValue ?? "?")
                                    + ($0.state == .on ? "+" : "")
                            }
                            print("snapshot: timeline-tmenu \(n) items=\(items.joined(separator: ","))")
                            if p.count == 2, let at = menu.items
                                .firstIndex(where: {
                                    $0.identifier?.rawValue == String(p[1])
                                }) {
                                menu.performActionForItem(at: at)
                            }
                        }
                        wait = 1.5
                    case "thit":
                        let n = Int(arg) ?? 0
                        let v = TimelineNSView.shown
                        let at = v?.windowPoint(transition: n)
                        print("snapshot: timeline-thit \(n) at=\(at.map { "\(Int($0.x)),\(Int($0.y))" } ?? "-") found=\(v?.transitionHit(n).map(String.init) ?? "-")")
                    case "click", "drag":
                        // Through the view's own handling: points
                        // where it draws that frame on that row.
                        let v = TimelineNSView.shown
                        if key == "click" {
                            let p = arg.split(separator: "@")
                            if p.count == 2, let f = Int(p[0]),
                               let r = Int(p[1]),
                               let pt = v?.windowPoint(frame: f, row: r) {
                                await Self.postDrag(
                                    from: (pt.x, pt.y), to: (pt.x, pt.y))
                            }
                        } else {
                            let p = arg.split(separator: ":")
                            let fs = p.count == 2
                                ? p[1].split(separator: ">")
                                    .compactMap { Int($0) } : []
                            if let r = Int(p.first ?? ""), fs.count == 2,
                               let a = v?.windowPoint(frame: fs[0], row: r),
                               let b = v?.windowPoint(frame: fs[1], row: r) {
                                await Self.postDrag(from: (a.x, a.y),
                                                    to: (b.x, b.y))
                            }
                        }
                        wait = 1.5
                    // A still's block's end dragged to a frame
                    // ("stretch=2>60", rows from the top), through
                    // the view; "length=<layer>:<frames>" the model's.
                    case "stretch":
                        let v = TimelineNSView.shown
                        let p = arg.split(separator: ">")
                            .compactMap { Int($0) }
                        if p.count == 2,
                           let a = v?.windowPoint(endOf: p[0]),
                           let b = v?.windowPoint(frame: p[1], row: p[0]) {
                            await Self.postDrag(from: (a.x, a.y),
                                                to: (b.x, b.y))
                        }
                        wait = 1.5
                    // A key's dot dragged through the view from its
                    // frame to another ("kdrag=1:30>36", rows from the
                    // top, timeline frames); "ktip=1:30" the tooltip
                    // its bar has, zoomed out ("-" none).
                    case "kdrag", "khold":
                        let v = TimelineNSView.shown
                        let p = arg.split(separator: ":")
                        let fs = p.count == 2
                            ? p[1].split(separator: ">")
                                .compactMap { Int($0) } : []
                        if let r = Int(p.first ?? ""), fs.count == 2,
                           let a = v?.windowPoint(key: fs[0], row: r),
                           let b = v?.windowPoint(key: fs[1], row: r) {
                            await Self.postDrag(from: (a.x, a.y),
                                                to: (b.x, b.y),
                                                release: key == "kdrag")
                        }
                        wait = 1.5
                    case "ktip":
                        let p = arg.split(separator: ":")
                            .compactMap { Int($0) }
                        if p.count == 2,
                           let t = TimelineNSView.shown?
                               .keyTip(frame: p[1], row: p[0]) {
                            print("snapshot: timeline-ktip \(arg) bars=\(t.bars) tip=\(t.tip)")
                        }
                    case "length":
                        let p = arg.split(separator: ":")
                        if p.count == 2, let n = Int(p[1]) {
                            m.stretchTimelineLayer(layerArg(p[0]),
                                                   length: n)
                        }
                        wait = 1.5
                    case "undo": m.undoProject()
                    case "redo": m.redoProject()
                    // File › Save, where the player is: "save", then
                    // "seek=20" -- the project stays clean.
                    case "save":
                        m.saveDocument()
                        print("snapshot: timeline-save dirty=\(m.document.dirty) frame=\(m.videoFrame)")
                    // Into the Prompt Editor, or back out of it.
                    case "editor":
                        withAnimation(AppModel.motion) {
                            m.toggleImmersivePrompt()
                        }
                    // A card under it opened: "panel=trim" (none: shut).
                    case "panel":
                        withAnimation(AppModel.motion) {
                            m.openPanel = arg == "adjust" ? .adjust
                                : arg == "crop" ? .crop
                                : arg == "trim" ? .trim
                                : arg == "generate" ? .generate : nil
                        }
                    // The clip carried on from its last N seconds
                    // ("continue=1": as it is) -- the prompt back.
                    case "continue":
                        print("snapshot: timeline-continue can=\(m.canContinueClip)")
                        if let s = Int(arg), s >= 2 {
                            m.continueClip(seconds: s)
                        } else {
                            m.continueClip()
                        }
                    // Grab Frame where the player is, the timeline open
                    // or not: the row's count before and after.
                    case "grab":
                        let before = m.promptAttachments.count
                        m.grabFrame()
                        print("snapshot: timeline-grab open=\(m.timelineOpen) refs=\(before)>\(m.promptAttachments.count) assets=\(m.assets.count)")
                    case "wait": wait = Double(arg) ?? 1
                    default: break
                    }
                    try? await Task.sleep(for: .seconds(wait))
                    print("snapshot: timeline \(step) \(Self.timelineSummary(m)) naming=\(TimelineNSView.shown?.isNaming ?? false) titles=\(m.timelineModel.rows.map(\.title).joined(separator: "|").replacingOccurrences(of: " ", with: "_"))")
                }
            }
            // A clip's keys: "adjust:55:exposure=1.5;crop:55:scale=0.6;
            // rotate:55:rotate=20".
            if let spec = env["VALTZ_SNAPSHOT_KEYS"] {
                for part in spec.split(separator: ";") {
                    let f = part.split(separator: ":", maxSplits: 2)
                    guard f.count == 3, let frame = Int(f[1]) else {
                        continue
                    }
                    var kv: [String: Double] = [:]
                    for item in f[2].split(separator: ",") {
                        let p = item.split(separator: "=")
                        if p.count == 2, let v = Double(p[1]) {
                            kv[String(p[0])] = v
                        }
                    }
                    m.setKey(f[0] == "crop" ? .place
                                 : f[0] == "rotate" ? .turn : .adjust,
                             at: frame, kv)
                }
            }
            // How a key goes on, as the key bar's pop-up sets it:
            // "adjust@0:spline;crop@10:hold" (a key's frame, from the
            // layer's start; none: where the player is). Prints the
            // keys and the track's curve as the panels have it --
            // exposure, scale X, degrees -- every few frames.
            if let spec = env["VALTZ_SNAPSHOT_KEY_INTERP"] {
                for part in spec.split(separator: ";") {
                    let f = part.split(separator: ":")
                    let what = f.first?.split(separator: "@") ?? []
                    guard f.count == 2, let name = what.first,
                          let how = KeyInterp(rawValue: String(f[1]))
                    else { continue }
                    let track: KeyTrack = name == "crop" ? .place
                        : name == "rotate" ? .turn : .adjust
                    m.setKeyInterp(track, how,
                                   at: what.count > 1 ? Int(what[1]) : nil)
                    func curve<T>(_ k: Keyframes<T>,
                                  _ v: (T) -> Double) -> String {
                        guard let a = k.keys.first?.frame,
                              let b = k.keys.last?.frame, b > a else {
                            return "-"
                        }
                        let step = max(1, (b - a) / 12)
                        return stride(from: a, through: b, by: step)
                            .map { f in
                                String(format: "%d:%.3f", f,
                                       k.value(at: f).map(v) ?? 0)
                            }.joined(separator: ",")
                    }
                    func keys<T>(_ k: Keyframes<T>) -> String {
                        k.keys.map { "\($0.frame):\($0.interp.rawValue)" }
                            .joined(separator: ",")
                    }
                    let (ks, c) = switch track {
                    case .adjust:
                        (keys(m.clipAdjustKeys),
                         curve(m.clipAdjustKeys) { $0.exposure })
                    case .place:
                        (keys(m.clipCropKeys.place),
                         curve(m.clipCropKeys.place) { $0.scaleX })
                    case .turn:
                        (keys(m.clipCropKeys.turn),
                         curve(m.clipCropKeys.turn) { $0.degrees })
                    }
                    let shown = m.keyInterp(track).map {
                        "\($0.frame):\($0.interp.rawValue)" } ?? "-"
                    print("snapshot: key-interp \(part) keys=\(ks) popup=\(shown) curve=\(c)")
                }
                try? await Task.sleep(for: .milliseconds(600))
            }
            // Render Upscaled (the Crop panel's), once the stage's layer
            // is scaled up: whether it is offered, its size, the job.
            let upscale = env["VALTZ_SNAPSHOT_UPSCALE"]
            if upscale == "1" || upscale == "wait" {
                try? await Task.sleep(for: .milliseconds(400))
                // A composition: Flatten First, then upscale.
                if m.upscaleNeedsFlatten {
                    m.flattenLayerForUpscale()
                    for _ in 0..<600 where m.flatteningLayer {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                    print("snapshot: upscale flattened source=\(m.activeLayerSourceName)")
                }
                let can = m.canUpscaleLayer
                let size = m.upscaleTarget.map { "\($0.width)x\($0.height)" }
                    ?? "-"
                if can { m.upscaleLayer() }
                print("snapshot: upscale can=\(can) size=\(size) job=\(m.upscaleJob ?? "-")")
                // "wait": until it has landed, then the layer's look as
                // the project has it and as the panel does -- the hooks
                // after (an export) see it done.
                if upscale == "wait" {
                    for _ in 0..<6000 where m.upscaleJob != nil {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                    try? await Task.sleep(for: .milliseconds(800))
                    let rec = m.currentClip?.cropKeys(layer: m.activeLayer)
                    print("snapshot: upscale landed source=\(m.activeLayerSourceName) "
                          + "recordCrop=\(rec.map { $0.isEmpty || $0.isIdentity ? "identity" : "keyed" } ?? "-") "
                          + "panelIdentity=\(m.clipCropKeys.isIdentity)")
                }
            }
            if let s = env["VALTZ_SNAPSHOT_SEEK"], let n = Int(s) {
                m.seekVideo(to: n)
                try? await Task.sleep(for: .milliseconds(400))
            }
            // The Trim panel's transport: a rate ("-1", "2"; 0 is
            // pause), or a step ("-1", "3").
            if let s = env["VALTZ_SNAPSHOT_PLAY"], let r = Float(s) {
                m.playVideo(rate: r)
            }
            if let s = env["VALTZ_SNAPSHOT_STEP"], let n = Int(s) {
                m.stepVideo(n)
            }
            // The player's floating play button: the player played
            // directly, as the button does (not the Trim panel's
            // transport); where it is 0.4 s later is printed.
            if env["VALTZ_SNAPSHOT_PLAYER_PLAY"] != nil,
               let win = NSApp.windows.first(where: { $0.isVisible }),
               let root = win.contentView?.superview,
               let pv = Self.find(AVPlayerView.self, in: root) {
                let from = m.videoFrame
                pv.player?.play()
                try? await Task.sleep(for: .milliseconds(400))
                FileHandle.standardError.write(Data(
                    "snapshot: player-play from=\(from) now=\(m.videoFrame) rate=\(m.videoRate)\n".utf8))
            }
            // Frame N of the clip as its player draws it, with its
            // look: "30:/tmp/frame.png".
            if let spec = env["VALTZ_SNAPSHOT_LOOK_FRAME"],
               let colon = spec.firstIndex(of: ":"),
               let n = Int(spec[..<colon]), let core = m.core {
                let out = URL(fileURLWithPath:
                    String(spec[spec.index(after: colon)...]))
                // A composition's frame as its plan draws it; a clip's
                // through its look.
                if let plan = m.stackPlayback, !plan.soundOnly,
                   let img = core.stackStill(plan: plan.plan, frame: n) {
                    _ = AppModel.writePNG(img, to: out)
                } else if let url = m.stage.video,
                          let look = m.stageClipLook,
                          let img = await VideoPlayerView.frame(
                              url, n, look: look, core: core) {
                    _ = AppModel.writePNG(img, to: out)
                }
            }
            // The attach menu's captures, in turn: "mic" / "system"
            // (a sound recorded), "camera", "card:sound|camera" (the
            // card shown alone), "wait:<s>", "stop" (the
            // sound attached), "snap", "record", "stop-video",
            // "cancel" -- each printing `snapshot: attach <step>
            // capture= recording= seconds= frames= refs=`.
            if let spec = env["VALTZ_SNAPSHOT_ATTACH_CAPTURE"] {
                for step in spec.split(separator: ";").map(String.init) {
                    let kv = step.split(separator: ":").map(String.init)
                    switch kv[0] {
                    case "mic", "system":
                        m.attachSound(system: kv[0] == "system")
                    case "camera": m.attachCamera()
                    // "card:sound" / "card:camera": the card alone, as
                    // a recording or the camera shows it (no device:
                    // what it looks like).
                    case "card":
                        if kv.count > 1 && kv[1] == "camera" {
                            m.camera = CameraStateDTO(
                                on: true, name: "MacBook Pro Camera",
                                width: 1920, height: 1080)
                            m.attachCapture = .camera
                        } else {
                            m.capture = CaptureStateDTO(
                                recording: true, name: "System Audio",
                                seconds: 7.4, level: 0.6)
                            m.attachCapture = .sound
                        }
                    case "wait":
                        try? await Task.sleep(for: .seconds(
                            Double(kv.count > 1 ? kv[1] : "1") ?? 1))
                    // "land:<asset id>": that asset as if just
                    // captured -- the card turning into its thumbnail.
                    case "land" where kv.count > 1:
                        m.capture = CaptureStateDTO()
                        m.camera = CameraStateDTO()
                        m.landCapture(kv[1])
                    case "stop": m.finishAttachSound()
                    case "snap": m.cameraSnap()
                    case "record": m.cameraRecord()
                    case "stop-video": m.cameraStopRecording()
                    case "cancel":
                        if m.attachCapture == .camera {
                            m.closeCamera()
                        } else {
                            m.cancelAttachSound()
                        }
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(1200))
                    FileHandle.standardError.write(Data(
                        "snapshot: attach \(step) capture=\(m.attachCapture.map { "\($0)" } ?? "-") recording=\(m.capture.recording || m.camera.recording) seconds=\(String(format: "%.1f", max(m.capture.seconds, m.camera.seconds))) frames=\(m.camera.frames) refs=\(m.promptAttachments.map { $0.kind + ":" + $0.url.lastPathComponent }.joined(separator: ",").replacingOccurrences(of: " ", with: "_"))\n".utf8))
                }
            }
            // File › New Anonymous Project.
            if env["VALTZ_SNAPSHOT_NEW_ANONYMOUS"] != nil {
                m.newAnonymousProject()
                try? await Task.sleep(for: .milliseconds(800))
                FileHandle.standardError.write(Data(
                    "snapshot: new-anonymous anonymous=\(m.isAnonymous) project=\(m.projectName.replacingOccurrences(of: " ", with: "_")) assets=\(m.assets.count)\n".utf8))
            }
            // A model downloaded at once (DESIGN §3a): it runs beside
            // the work; the task steps print how far it is.
            if let id = env["VALTZ_SNAPSHOT_DOWNLOAD"] {
                m.download(model: id)
                try? await Task.sleep(for: .milliseconds(1500))
                FileHandle.standardError.write(Data(
                    "snapshot: download \(id) downloads=\(Self.downloadSummary(m)) options=\(m.modelOptions(for: m.activeModality).map(ModelPicker.label).joined(separator: "|").replacingOccurrences(of: " ", with: "_"))\n".utf8))
            }
            if env["VALTZ_SNAPSHOT_START"] != nil { m.start() }
            // The TASK QUEUE (DESIGN §3a), in turn: "prompt:<text>",
            // "start", "wait:<s>", "watch:<n>" (the task at T<n>, as a
            // click on its row), "view:<asset id>|newest-image",
            // "active:project", "cancel:<n>", "undo", "compare" (its
            // button), "export:<format>:<file>", "idle:<s>" (until
            // no task is left, at most s), "model:<id>" (the model
            // field; empty: Auto) -- each printing `snapshot: task
            // <step> tasks= watched= stage= downloads=`.
            if let spec = env["VALTZ_SNAPSHOT_TASKS"] {
                for step in spec.split(separator: ";").map(String.init) {
                    let kv = step.split(separator: ":", maxSplits: 1)
                        .map(String.init)
                    let arg = kv.count > 1 ? kv[1] : ""
                    switch kv[0] {
                    case "prompt": m.setPrompt(arg)
                    case "start": m.start()
                    // The project closed while tasks run -- asked
                    // (VALTZ_SNAPSHOT_STOP_TASKS), stopped, waited
                    // for: "close:open:<pkg>" another opened (as a
                    // drop opens one), "close:new" a new anonymous
                    // one.
                    case "close":
                        if arg.hasPrefix("open:") {
                            m.openDropped(URL(fileURLWithPath:
                                String(arg.dropFirst(5))))
                        } else if arg == "new" {
                            m.newAnonymousProject()
                        }
                    // The window drawn now -- the veil while it waits:
                    // "draw:<file.png>".
                    case "draw":
                        Self.drawWindow(to: arg)
                    // The window as the screen shows it, now.
                    case "shot":
                        await Self.shootWindow(to: arg)
                    case "model": m.modelChoice = arg
                    // The generation card's rows, as the env hooks
                    // set them before Start: "size:1920x1088",
                    // "length:10" (or "100 frames"), "modality:video".
                    case "size": m.setCustomSize(arg)
                    case "length":
                        if let n = Int(arg),
                           AppModel.clipLengths.contains(n) {
                            m.clipSeconds = n
                        } else {
                            m.setClipLength(arg)
                        }
                    case "modality":
                        if let v = Modality(rawValue: arg) {
                            m.setModality(v)
                        }
                    case "wait":
                        try? await Task.sleep(
                            for: .seconds(Double(arg) ?? 1))
                    case "watch":
                        if let t = m.tasks.first(where: {
                            $0.position == Int(arg) }) {
                            m.watchTask(t.job)
                        }
                    case "view":
                        let a = arg == "newest-image"
                            ? m.assets.filter { $0.kind == "image"
                                && $0.head > 0 && $0.op != "project" }
                                .max { $0.created < $1.created }
                            : m.assets.first { $0.id == arg }
                        if let a { m.viewAsset(a) }
                    case "active":
                        if let a = m.projectViews.first { m.setActive(a) }
                    case "cancel":
                        if let t = m.tasks.first(where: {
                            $0.position == Int(arg) }) {
                            m.cancel(job: t.job)
                        }
                    case "undo": m.undoProject()
                    // "click:x,y": a click posted to the window, points
                    // from its top left (a card's button, say).
                    case "click":
                        let xy = arg.split(separator: ",").compactMap {
                            Double($0)
                        }
                        if xy.count == 2 {
                            Self.postClick(x: xy[0], y: xy[1])
                        }
                    // Valtz hidden (Hide Valtz, ⌘H) and shown again,
                    // the keyboard left where it is: a task landing
                    // while its window is out of sight.
                    case "hide": NSApp.hide(nil)
                    case "unhide": NSApp.unhideWithoutActivation()
                    // "timeline:open|close": View › Timeline, between
                    // the other steps.
                    case "timeline": m.showTimeline(arg == "open")
                    // "land:<asset id>|newest-video|newest-image": that
                    // asset landing as a result does, not watched -- a
                    // new layer of the active composition, then what
                    // a landing does (AppModel.landed; each step's
                    // `landing=`).
                    case "land":
                        let a = arg.hasPrefix("newest-")
                            ? m.assets.filter {
                                $0.kind == arg.dropFirst(7)
                                    && $0.head > 0 && !$0.isComposition
                              }.max { $0.created < $1.created }?.id
                            : arg
                        if let a { m.snapshotLand(a) }
                    // The AppKit views SwiftUI hosts in the window --
                    // a player, a preview, the canvas -- each with its
                    // frame and alpha: one left behind shows here.
                    case "hosts":
                        if let w = NSApp.windows.first(where: {
                            $0.isVisible }),
                           let root = w.contentView {
                            var out: [String] = []
                            var queue = [root]
                            while !queue.isEmpty {
                                let v = queue.removeFirst()
                                let t = String(describing: type(of: v))
                                if t.hasPrefix("AppKitPlatformViewHost"),
                                   let inner = v.subviews.first {
                                    let f = v.convert(v.bounds, to: nil)
                                    // Seen: the alphas and layer
                                    // opacities above it, multiplied;
                                    // "h" hidden itself or above.
                                    var a = 1.0
                                    var up: NSView? = v
                                    while let u = up {
                                        a *= Double(u.alphaValue)
                                        a *= Double(u.layer?.opacity ?? 1)
                                        up = u.superview
                                    }
                                    out.append("\(type(of: inner))@\(Int(f.minX)),\(Int(w.frame.height - f.maxY)),\(Int(f.width))x\(Int(f.height))a\(String(format: "%.2f", a))\(v.isHiddenOrHasHiddenAncestor ? "h" : "")")
                                }
                                queue += v.subviews
                            }
                            FileHandle.standardError.write(Data(
                                "snapshot: hosts \(out.joined(separator: " "))\n".utf8))
                        }
                    // Every progress bar in the windows -- `snapshot:
                    // bars <kind>:<indeterminate|value>@x,y,w ...` and
                    // the stage's caption: one bouncing while the
                    // caption counts shows here.
                    case "bars":
                        var out: [String] = []
                        for w in NSApp.windows where w.isVisible {
                            guard let root = w.contentView else { continue }
                            var queue: [NSView] = [root]
                            while !queue.isEmpty {
                                let v = queue.removeFirst()
                                if let p = v as? NSProgressIndicator,
                                   !p.isHiddenOrHasHiddenAncestor {
                                    let f = p.convert(p.bounds, to: nil)
                                    let k = p.style == .bar ? "bar" : "spin"
                                    let s = p.isIndeterminate ? "indeterminate"
                                        : String(format: "%.3f", p.doubleValue
                                                 / max(p.maxValue, 1e-9))
                                    // Seen: the alphas and opacities
                                    // above it, multiplied.
                                    var a = 1.0
                                    var up: NSView? = p
                                    while let u = up {
                                        a *= Double(u.alphaValue)
                                        a *= Double(u.layer?.opacity ?? 1)
                                        up = u.superview
                                    }
                                    out.append("\(k):\(s)@\(Int(f.minX)),\(Int(w.frame.height - f.maxY)),\(Int(f.width))a\(String(format: "%.2f", a))")
                                }
                                queue += v.subviews
                            }
                        }
                        FileHandle.standardError.write(Data(
                            "snapshot: bars \(out.isEmpty ? "-" : out.joined(separator: " ")) fraction=\(m.generationPhase?.fraction.map { String(format: "%.3f", $0) } ?? "-") caption=\(m.generationCaption)\n".utf8))
                    // The stage player's own views, as AVKit has them:
                    // each one's class, frame (points from the window's
                    // top left), alpha and hidden -- `snapshot: avtree`.
                    // `avhover` first hands its tracking areas' owners an
                    // enter and moves at the player's middle, the events
                    // a pointer over it brings (posted ones never reach
                    // a tracking area): what a hover shows, without one.
                    case "avtree", "avhover":
                        guard let w = NSApp.windows.first(where: {
                            $0.isVisible }),
                              let root = w.contentView,
                              let pv = Self.find(AVPlayerView.self, in: root)
                        else {
                            FileHandle.standardError.write(Data(
                                "snapshot: \(kv[0]) player=-\n".utf8))
                            break
                        }
                        if kv[0] == "avhover" {
                            let mid = pv.convert(
                                NSPoint(x: pv.bounds.midX,
                                        y: pv.bounds.midY), to: nil)
                            var views: [NSView] = [pv]
                            var all: [NSView] = []
                            while let v = views.popLast() {
                                all.append(v)
                                views += v.subviews
                            }
                            for v in all {
                                for area in v.trackingAreas {
                                    guard let owner = area.owner
                                        as? NSResponder else { continue }
                                    if let e = NSEvent.enterExitEvent(
                                        with: .mouseEntered, location: mid,
                                        modifierFlags: [],
                                        timestamp: ProcessInfo.processInfo
                                            .systemUptime,
                                        windowNumber: w.windowNumber,
                                        context: nil, eventNumber: 0,
                                        trackingNumber: 0, userData: nil) {
                                        owner.mouseEntered(with: e)
                                    }
                                    for dx in [0.0, 6, 12] {
                                        if let e = NSEvent.mouseEvent(
                                            with: .mouseMoved,
                                            location: NSPoint(
                                                x: mid.x + dx, y: mid.y),
                                            modifierFlags: [],
                                            timestamp: ProcessInfo
                                                .processInfo.systemUptime,
                                            windowNumber: w.windowNumber,
                                            context: nil, eventNumber: 0,
                                            clickCount: 0, pressure: 0) {
                                            owner.mouseMoved(with: e)
                                        }
                                    }
                                }
                            }
                            try? await Task.sleep(for: .seconds(1))
                        }
                        var out: [String] = []
                        var stack: [(NSView, Int)] = [(pv, 0)]
                        while let (v, depth) = stack.popLast() {
                            let f = v.convert(v.bounds, to: nil)
                            if f.width > 0 && f.height > 0 {
                                out.append("\(depth):\(type(of: v))@\(Int(f.minX)),\(Int(w.frame.height - f.maxY)),\(Int(f.width))x\(Int(f.height))a\(String(format: "%.2f", v.alphaValue * CGFloat(v.layer?.opacity ?? 1)))\(v.isHidden ? "h" : "")")
                            }
                            stack += v.subviews.reversed().map {
                                ($0, depth + 1)
                            }
                        }
                        FileHandle.standardError.write(Data(
                            "snapshot: \(kv[0]) style=\(pv.controlsStyle.rawValue) \(out.joined(separator: " "))\n".utf8))
                    // "hit:x,y": the AppKit view a click there goes
                    // to, and its ancestors (points from the window's
                    // top left) -- printed as `snapshot: hit`.
                    case "hit":
                        let xy = arg.split(separator: ",").compactMap {
                            Double($0)
                        }
                        if xy.count == 2 {
                            FileHandle.standardError.write(Data(
                                "snapshot: hit \(arg) dragReady=\(m.stageDragReady) chain=\(Self.hitChain(x: xy[0], y: xy[1]))\n".utf8))
                        }
                    // "move:x,y>x,y>...": the pointer moved through
                    // the window, a mouse-moved event at each point
                    // (from its top left), 80 ms apart, through the
                    // app's event queue -- its monitors see it; the
                    // tracking areas do not, the window server's --
                    // or, "hmove:", handed to the window's SwiftUI as
                    // over its own views: the drag band armed and let
                    // go, the cursor, where a click would go -- each
                    // printed as `snapshot: move`.
                    case "move", "hmove":
                        for pt in arg.split(separator: ">") {
                            let xy = pt.split(separator: ",")
                                .compactMap { Double($0) }
                            guard xy.count == 2 else { continue }
                            Self.postMove(x: xy[0], y: xy[1],
                                          toHost: kv[0] == "hmove")
                            try? await Task.sleep(for: .milliseconds(80))
                            let c = m.stageCardFrame
                            FileHandle.standardError.write(Data(
                                "snapshot: \(kv[0]) \(pt) dragReady=\(m.stageDragReady) canDrag=\(m.canDragStage) cursor=\(Self.cursorName(NSCursor.current)) card=\(Int(c.minX)),\(Int(c.minY)),\(Int(c.width))x\(Int(c.height)) chain=\(Self.hitChain(x: xy[0], y: xy[1]))\n".utf8))
                        }
                    // The title bar's compare button.
                    case "compare": m.toggleCompare()
                    // "zoom:in|out|fit|actual|unfit": the title bar's
                    // zoom group, mid-script (a clip's too), printed as
                    // `snapshot: zoom`.
                    case "zoom":
                        switch arg {
                        case "in": m.zoom(.zoomIn)
                        case "out": m.zoom(.zoomOut)
                        case "fit": m.zoom(.fit)
                        case "actual": m.zoom(.actualSize)
                        case "unfit": m.toggleFit()
                        default: break
                        }
                        try? await Task.sleep(for: .milliseconds(300))
                        let v = m.currentViewer
                        let r = m.stageVideoRect.map {
                            "\(Int($0.minX)),\(Int($0.minY)),"
                                + "\(Int($0.width))x\(Int($0.height))"
                        } ?? "-"
                        FileHandle.standardError.write(Data(
                            "snapshot: zoom \(arg) zoom=\(v.zoomText) fit=\(v.fitting) canZoom=\(m.canZoom) videoShown=\(r)\n".utf8))
                    // "export:<format>:<file>": what the stage shows,
                    // saved as Save… would.
                    case "export":
                        let fp = arg.split(separator: ":", maxSplits: 1)
                            .map(String.init)
                        if fp.count == 2,
                           let c = ExportChoice(rawValue: fp[0]) {
                            m.save(to: URL(fileURLWithPath: fp[1]), as: c)
                        }
                    case "idle":
                        for _ in 0..<Int((Double(arg) ?? 60) * 4)
                        where !m.tasks.isEmpty {
                            try? await Task.sleep(for: .milliseconds(250))
                        }
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                    FileHandle.standardError.write(Data(
                        "snapshot: task \(step.replacingOccurrences(of: " ", with: "_")) tasks=\(Self.taskSummary(m)) compareOn=\(m.compareOn) canCompare=\(m.canToggleCompare) activeComp=\(m.activeComposition.map { "\($0.name.replacingOccurrences(of: " ", with: "_")):\($0.layerStack.count)layers" } ?? "-") watched=\(m.tasks.first { $0.job == m.generationJob }?.badge ?? (m.generationJob == nil ? "-" : "done")) stage=\(m.stageAssetId.flatMap { id in m.assets.first { $0.id == id }?.name }?.replacingOccurrences(of: " ", with: "_") ?? "-") caption=\(m.generationCaption.replacingOccurrences(of: " ", with: "_")) downloads=\(Self.downloadSummary(m)) generating=\(m.isGenerating) memoryFailure=\(m.memoryFailure != nil) landing=\(Self.landingSummary(m)) \(Self.closingSummary(m))\n".utf8))
                }
            }
            // THE AGENT (DESIGN §9b), driven as its card drives it:
            // "ask:<words>" (the Agent tab, the words, Start),
            // "say:<words>", "wait:<state>@<s>" (until the session is
            // in that state, at most <s> seconds), "approve",
            // "skip:<note>", "all", "pause", "resume", "stop",
            // "close", "sleep:<s>", "editor" (the Prompt Editor).
            if let spec = env["VALTZ_SNAPSHOT_AGENT"] {
                for step in spec.split(separator: ";").map(String.init) {
                    let kv = step.split(separator: ":", maxSplits: 1)
                        .map(String.init)
                    let arg = kv.count > 1 ? kv[1] : ""
                    let proposed = m.agent?.proposed
                    switch kv[0] {
                    case "ask":
                        m.setModality(.agent)
                        m.setPrompt(arg)
                        m.start()
                    case "say":
                        m.setPrompt(arg)
                        m.start()
                    // Words typed into the box as the keyboard types
                    // them, then Start pressed: the text view holds
                    // them, as a person's do.
                    case "type":
                        if let win = NSApp.windows.first(where: { $0.isVisible }),
                           let root = win.contentView?.superview,
                           let tv = Self.find(PromptTextView.self, in: root) {
                            win.makeFirstResponder(tv)
                            tv.setSelectedRange(NSRange(
                                location: tv.string.utf16.count, length: 0))
                            for ch in arg {
                                tv.insertText(String(ch),
                                              replacementRange: tv.selectedRange())
                            }
                        }
                    case "start": m.start()
                    case "wait":
                        let w = arg.split(separator: "@").map(String.init)
                        let want = w.first ?? "done"
                        let limit = Double(w.count > 1 ? w[1] : "") ?? 120
                        for _ in 0..<Int(limit * 4) {
                            let st = m.agent?.state ?? ""
                            if st == want || st == "stopped"
                                || (st == "failed" && want != "failed") {
                                break
                            }
                            try? await Task.sleep(for: .milliseconds(250))
                        }
                    case "approve": if let p = proposed { m.agentApprove(p) }
                    case "skip":
                        if let p = proposed {
                            m.agentNote = arg
                            m.agentSkip(p)
                        }
                    case "all": if let p = proposed { m.agentApproveAll(p) }
                    case "pause", "resume", "stop": m.agentControl(kv[0])
                    case "close": m.agentDismiss()
                    case "editor": m.enterImmersivePrompt()
                    case "sleep":
                        try? await Task.sleep(for: .seconds(Double(arg) ?? 1))
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                    let a = m.agent
                    // What the box's text view shows, and whether the
                    // empty box's hint is drawn over it.
                    let box = NSApp.windows.first(where: { $0.isVisible })?
                        .contentView?.superview
                        .flatMap { Self.find(PromptTextView.self, in: $0) }?
                        .string.count ?? -1
                    let hint = m.prompt.isEmpty && m.promptAttachments.isEmpty
                    FileHandle.standardError.write(Data(
                        "snapshot: agent-box box=\(box) prompt=\(m.prompt.count) hint=\(hint) immersive=\(m.promptImmersive) boxRect=\(Self.promptBoxRect())\n".utf8))
                    FileHandle.standardError.write(Data(
                        "snapshot: agent \(step.replacingOccurrences(of: " ", with: "_").prefix(40)) state=\(a?.state ?? "-") steps=\(a?.steps.count ?? 0) proposed=\(a?.proposed?.tool ?? "-") last=\(a?.steps.last.map { "\($0.tool):\($0.state)" } ?? "-") review=\(a?.review ?? false) backend=\(a?.backend?.kind ?? "-") composition=\(m.activeComposition?.name.replacingOccurrences(of: " ", with: "_") ?? "-") pages=\(m.activeComposition?.pages ?? 0) writing=\(m.agentWriting.count) error=\(a?.errorText?.replacingOccurrences(of: " ", with: "_").prefix(60) ?? "-") speed=\(a?.stats.flatMap { $0.isEmpty ? nil : $0.line.replacingOccurrences(of: " ", with: "_") } ?? "-")\n".utf8))
                }
            }
            // Stop the generation after some seconds, as the Stop
            // button would; then, with VALTZ_SNAPSHOT_RESTART, Start
            // again once it has stopped.
            if let s = env["VALTZ_SNAPSHOT_STOP_AFTER"].flatMap(Double.init) {
                try? await Task.sleep(for: .seconds(s))
                m.stop()
                for _ in 0..<80 where m.isGenerating {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                if env["VALTZ_SNAPSHOT_RESTART"] != nil {
                    try? await Task.sleep(for: .milliseconds(500))
                    m.start()
                    FileHandle.standardError.write(Data(
                        "snapshot: restart generating=\(m.isGenerating) banner=\(m.banner ?? "-") base=\(m.baseAttachment?.url.lastPathComponent ?? "-")\n".utf8))
                }
            }
            // Document steps once the generation started above has
            // landed (VALTZ_SNAPSHOT_EDIT's, "undo;redo"): it is
            // undone as one command, placed result and all.
            if let spec = env["VALTZ_SNAPSHOT_EDIT_AFTER"] {
                for _ in 0..<Int(delay * 4) where m.isGenerating {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                for step in spec.split(separator: ";").map(String.init) {
                    try? await Task.sleep(for: .milliseconds(800))
                    if step == "undo" { m.undoProject() }
                    if step == "redo" { m.redoProject() }
                    try? await Task.sleep(for: .milliseconds(400))
                    FileHandle.standardError.write(Data(
                        "snapshot: edit-after \(step) dirty=\(m.document.dirty) undo=\(m.document.undo ?? "-") redo=\(m.document.redo ?? "-") stage=\(m.stageAssetId ?? "-") layers=\(m.stageStack?.layerStack.map { $0.id.isEmpty ? "0" : $0.id }.joined(separator: ",") ?? "-") assets=\(m.assets.count)\n".utf8))
                }
            }
            // Adjustments ("exposure=0.4,saturation=0.3"), once the
            // generation started above has a result to adjust.
            if let spec = env["VALTZ_SNAPSHOT_ADJUST"] {
                for _ in 0..<Int(delay * 4) where !m.canAdjust {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                // As the panel sets them (a clip's go to its keys).
                let a = Self.adjustments(spec)
                for key in ImageAdjustments.Key.allCases where a[key] != 0 {
                    m.setAdjustment(key, a[key])
                }
                withAnimation(AppModel.motion) { m.openPanel = .adjust }
                try? await Task.sleep(for: .seconds(1.5))
            }
            // The key button, here: "adjust" or "crop".
            if let t = env["VALTZ_SNAPSHOT_KEY_TOGGLE"] {
                m.toggleKey(t == "crop" ? .place
                                : t == "rotate" ? .turn : .adjust)
            }
        }
        // Start Over, straight through (no confirmation).
        // Bypass held ("adjust", "place", "turn"; comma-separated), as
        // its button does while pressed.
        if let m = model, let spec = env["VALTZ_SNAPSHOT_BYPASS"] {
            for part in spec.split(separator: ",") {
                switch part {
                case "adjust": m.setBypass(.adjust, true)
                case "place": m.setBypass(.place, true)
                case "turn": m.setBypass(.turn, true)
                default: break
                }
            }
            try? await Task.sleep(for: .seconds(1))
        }
        // Focus the field `name` names (its placeholder, in any
        // window -- Custom's panel is a popover) and click the
        // stepper beside it: "Steps:up:2" (events sent to the window,
        // not the pointer). The summary's `tuning=` says what took.
        if let spec = env["VALTZ_SNAPSHOT_STEPPER"] {
            let p = spec.split(separator: ":").map(String.init)
            if p.count >= 2 {
                try? await Task.sleep(for: .milliseconds(600))
                await Self.clickStepper(field: p[0], up: p[1] == "up",
                                        times: p.count > 2
                                            ? Int(p[2]) ?? 1 : 1)
                try? await Task.sleep(for: .milliseconds(600))
            }
        }
        // Tab through the text fields from the one `start` names (its
        // placeholder), as the key does: the order focus takes.
        if let spec = env["VALTZ_SNAPSHOT_TABS"] {
            let parts = spec.split(separator: ",").map(String.init)
            let n = parts.count > 1 ? Int(parts[1]) ?? 8 : 8
            try? await Task.sleep(for: .milliseconds(500))
            let order = await Self.tabOrder(from: parts.first ?? "",
                                            count: n)
            FileHandle.standardError.write(Data(
                "snapshot: tabs=\(order.joined(separator: " > "))\n"
                    .utf8))
        }
        // ⌘? held ("hold:<s>": its key let go after s seconds;
        // "holdcmd:<s>": ⌘ let go; "menu:<s>": Help › Keyboard
        // Shortcuts, then a key) -- posted through the app's event
        // path: "snapshot: shortcuts held shown= windows= hints=", then
        // "released shown=". The labels are a window of their own:
        // capture it by its number (`windows=` parent:labels) while
        // held.
        if let spec = env["VALTZ_SNAPSHOT_SHORTCUTS"],
           let w = model?.editorWindow {
            let p = spec.split(separator: ":").map(String.init)
            let hold = p.count > 1 ? Double(p[1]) ?? 2 : 2
            func post(_ type: NSEvent.EventType,
                      _ flags: NSEvent.ModifierFlags,
                      _ chars: String, _ code: UInt16) {
                if let e = NSEvent.keyEvent(
                    with: type, location: .zero, modifierFlags: flags,
                    timestamp: ProcessInfo.processInfo.systemUptime,
                    windowNumber: w.windowNumber, context: nil,
                    characters: chars,
                    charactersIgnoringModifiers: type == .flagsChanged
                        ? "" : "/",
                    isARepeat: false, keyCode: code) {
                    NSApp.postEvent(e, atStart: false)
                }
            }
            try? await Task.sleep(for: .milliseconds(500))
            if p[0] == "menu" {
                ShortcutHints.shared.showLatched()
            } else {
                post(.keyDown, [.command, .shift], "?", 44)
            }
            try? await Task.sleep(for: .milliseconds(500))
            let overlays = NSApp.windows.filter {
                $0 is ShortcutHintWindow && $0.isVisible
            }
            FileHandle.standardError.write(Data(
                "snapshot: shortcuts held shown=\(ShortcutHints.shared.shown) windows=\(overlays.map { "\($0.parent?.windowNumber ?? 0):\($0.windowNumber)" }.joined(separator: ",")) hints=\(ShortcutHints.shared.summary.replacingOccurrences(of: " ", with: "|"))\n".utf8))
            try? await Task.sleep(for: .seconds(hold))
            switch p[0] {
            case "holdcmd": post(.flagsChanged, [.shift], "", 55)
            case "menu": post(.keyDown, [], "a", 0)
            default:
                // As fingers let go: the key, then ⇧ and ⌘.
                post(.keyUp, [.command, .shift], "?", 44)
                post(.flagsChanged, [.command], "", 56)
            }
            try? await Task.sleep(for: .milliseconds(400))
            FileHandle.standardError.write(Data(
                "snapshot: shortcuts released shown=\(ShortcutHints.shared.shown)\n".utf8))
        }
        // busy= the button greyed; went= a new session begun;
        // downloads= what runs on through it.
        if let m = model, env["VALTZ_SNAPSHOT_START_OVER"] != nil {
            let old = m.projectPath
            let busy = m.isBusy
            m.startOver()
            let gone = !FileManager.default.fileExists(atPath: old)
            FileHandle.standardError.write(Data(
                "snapshot: startOver busy=\(busy) went=\(m.projectPath != old) oldPackageGone=\(gone) downloads=\(Self.downloadSummary(m))\n".utf8))
        }
        // The editor's window closed as ⌘W closes it: asked about
        // first (VALTZ_SNAPSHOT_CLOSE answers), Cancel keeping it.
        // "cancel" | "discard" | "save:<path>": that close's answer
        // (the run's own quit takes VALTZ_SNAPSHOT_CLOSE's).
        if let m = model, let a = env["VALTZ_SNAPSHOT_CLOSE_WINDOW"],
           let w = m.editorWindow {
            let guarded = w.delegate is WindowCloseGuard
            m.snapshotCloseAnswer = a
            w.performClose(nil)
            m.snapshotCloseAnswer = nil
            try? await Task.sleep(for: .seconds(1))
            FileHandle.standardError.write(Data(
                "snapshot: close-window answer=\(a) guarded=\(guarded) open=\(m.editorWindow?.isVisible == true) confirmed=\(m.closeConfirmed)\n".utf8))
        }
        // The stage's compare mode ("difference"), once a result
        // has landed to compare.
        if let m = model, let name = env["VALTZ_SNAPSHOT_STAGE_MODE"],
           let mode = CompareMode(rawValue: name) {
            for _ in 0..<Int(delay * 4) where !m.canAdjust {
                try? await Task.sleep(for: .milliseconds(250))
            }
            m.setStageMode(mode)
        }
        // Once a result has landed: attach a picture, then click
        // badges ("0,0") -- the base toggle against a result.
        if let m = model, env["VALTZ_SNAPSHOT_THEN_ATTACH"] != nil
            || env["VALTZ_SNAPSHOT_THEN_CLICK_BADGE"] != nil {
            for _ in 0..<Int(delay * 4) where !m.canAdjust {
                try? await Task.sleep(for: .milliseconds(250))
            }
            if let f = env["VALTZ_SNAPSHOT_THEN_ATTACH"] {
                let url = URL(fileURLWithPath: f)
                m.addReferences([url])
                for _ in 0..<100 where m.asset(forFile: url) == nil {
                    try? await Task.sleep(for: .milliseconds(100))
                }
            }
            for n in (env["VALTZ_SNAPSHOT_THEN_CLICK_BADGE"] ?? "")
                .split(separator: ",").compactMap({ Int($0) }) {
                try? await Task.sleep(for: .milliseconds(400))
                m.toggleBase(picture: n)
            }
            try? await Task.sleep(for: .milliseconds(400))
        }
        // Save… as an export ("exr:/tmp/out.exr"), as the save
        // panel's format menu would, of the asset on the stage (the
        // newest picture or video put there when the stage has
        // another kind).
        if let m = model, let spec = env["VALTZ_SNAPSHOT_EXPORT"],
           let colon = spec.firstIndex(of: ":") {
            let choice = ExportChoice(rawValue: String(spec[..<colon]))
                ?? .original
            let dest = URL(fileURLWithPath:
                String(spec[spec.index(after: colon)...]))
            let kind = choice.video ? "video" : "image"
            if m.currentAsset?.kind != kind,
               let last = m.assets.last(where: {
                   $0.kind == kind && $0.head > 0
               }) {
                m.putOnStage(last)
            }
            try? FileManager.default.removeItem(at: dest)
            m.save(to: dest, as: choice)
            // VALTZ_SNAPSHOT_EXPORT_WAIT=<s>: the snapshot that long
            // after it starts, while it runs (a clip's export on the
            // stage), not once it is saved.
            if let wait = env["VALTZ_SNAPSHOT_EXPORT_WAIT"]
                .flatMap(Double.init) {
                try? await Task.sleep(for: .seconds(wait))
                let size = m.exportShow?.preview.map {
                    "\($0.width)x\($0.height)" } ?? "-"
                FileHandle.standardError.write(Data(
                    "snapshot: exporting caption=\(m.exportCaption) preview=\(size)\n".utf8))
                if env["VALTZ_SNAPSHOT_EXPORT_STOP"] != nil {
                    m.stopExport()
                    try? await Task.sleep(for: .seconds(1.5))
                    FileHandle.standardError.write(Data(
                        "snapshot: export-stopped showing=\(m.exportShow != nil) exists=\(FileManager.default.fileExists(atPath: dest.path))\n".utf8))
                }
            }
            for _ in 0..<Int(delay * 4) where
                env["VALTZ_SNAPSHOT_EXPORT_WAIT"] == nil &&
                !FileManager.default.fileExists(atPath: dest.path) {
                try? await Task.sleep(for: .milliseconds(250))
            }
            try? await Task.sleep(for: .milliseconds(500))
            FileHandle.standardError.write(Data(
                "snapshot: export \(choice.rawValue) -> \(dest.path) exists=\(FileManager.default.fileExists(atPath: dest.path)) choices=\(m.exportChoices.map(\.rawValue))\n".utf8))
        }
        // Save… through its panel: "jpeg:60:/tmp/out.jpg" (the
        // format, its quality -- a JPEG's 1...100, PNG's or TIFF's
        // depth 8 or 16 -- and the file), chosen in the panel as a
        // person would, then Save pressed. With VALTZ_SNAPSHOT_SHARE
        // the share menu opens first (captured a moment) and its
        // Save… item runs it.
        let saveSpec = (env["VALTZ_SNAPSHOT_SAVE"] ?? "")
            .split(separator: ":", maxSplits: 2).map(String.init)
        if saveSpec.count == 3 {
            let dest = URL(fileURLWithPath: saveSpec[2])
            try? FileManager.default.removeItem(at: dest)
            try? FileManager.default.createDirectory(
                at: dest.deletingLastPathComponent(),
                withIntermediateDirectories: true)
            ShareButton.scriptedDestination = dest
            ShareButton.script = { panel, options in
                // A click on the Format menu, as a pointer makes it:
                // does its menu open?
                FileHandle.standardError.write(Data(
                    "snapshot: panel sheetParent=\(panel.sheetParent.map { String(describing: type(of: $0)) } ?? "-") visible=\(panel.isVisible) accessoryWindow=\(panel.accessoryView?.window.map { "\(type(of: $0))#\($0.windowNumber)" } ?? "-") panel#\(panel.windowNumber)\n".utf8))
                if env["VALTZ_SNAPSHOT_SAVE_CLICK"] != nil,
                   let acc = panel.accessoryView,
                   let w = acc.window {
                    ValtzApp.clickFirstControl(in: acc, window: w)
                }
                let f = ExportChoice(rawValue: saveSpec[0]) ?? .original
                options.setFormat(f)
                if f.hasDepth { options.setDeep(saveSpec[1] == "16") }
                if let q = Double(saveSpec[1]), f == .jpeg {
                    options.setQuality(q)
                }
                // A movie's encoding, as its rows set it:
                // "rate=bitrate,bitrate=8,peak=12,keyframes=2,bframes=0,
                // profile=main,level=4.1,entropy=cavlc,prores=422lt,
                // hevc=main,quality=60"; the accessory drawn to
                // <snapshot>-save.png.
                if let spec = env["VALTZ_SNAPSHOT_SAVE_VIDEO"],
                   let rows = options.video {
                    var v = rows.settings
                    for part in spec.split(separator: ",") {
                        let kv = part.split(separator: "=").map(String.init)
                        guard kv.count == 2 else { continue }
                        let n = Double(kv[1]) ?? 0
                        switch kv[0] {
                        case "rate":
                            v.rate = VideoEncodingSettings.Rate(
                                rawValue: kv[1]) ?? v.rate
                        case "bitrate": v.bitrate = n
                        case "peak": v.peak = n
                        case "quality": v.quality = n
                        case "keyframes": v.keyframes = n
                        case "bframes": v.bFrames = n != 0
                        case "profile": v.profile = kv[1]
                        case "level": v.level = kv[1]
                        case "entropy": v.entropy = kv[1]
                        case "hevc": v.hevcProfile = kv[1]
                        case "prores":
                            if f == .prores4444 { v.prores4444 = kv[1] }
                            else { v.prores422 = kv[1] }
                        default: break
                        }
                    }
                    rows.set(v)
                    options.apply()
                }
                Task { @MainActor in
                    // Long enough to capture the panel.
                    try? await Task.sleep(for: .seconds(3))
                    if let acc = panel.accessoryView,
                       let snap = env["VALTZ_SNAPSHOT"],
                       let rep = acc.bitmapImageRepForCachingDisplay(
                           in: acc.bounds) {
                        acc.cacheDisplay(in: acc.bounds, to: rep)
                        let out = (snap as NSString).deletingPathExtension
                            + "-save.png"
                        try? rep.representation(using: .png,
                                                properties: [:])?
                            .write(to: URL(fileURLWithPath: out))
                    }
                    let video = options.videoEncoding.map { j in
                        j.keys.sorted().map { k in "\(k)=\(j[k]!)" }
                            .joined(separator: ",")
                    } ?? "-"
                    FileHandle.standardError.write(Data(
                        "snapshot: save video=\(video)\n".utf8))
                    FileHandle.standardError.write(Data(
                        "snapshot: save panel \(type(of: panel)) choice=\(options.choice.rawValue) quality=\(options.jpegQuality.map(String.init) ?? "-") estimate=\(options.estimate.map(String.init) ?? "-") name=\(panel.nameFieldStringValue)\n".utf8))
                    // As Save would: NSSavePanel.ok(_:) throws when
                    // not sent by its button.
                    if let parent = panel.sheetParent {
                        parent.endSheet(panel, returnCode: .OK)
                    } else {
                        NSApp.stopModal(withCode: .OK)
                    }
                }
            }
        }
        if let m = model, env["VALTZ_SNAPSHOT_SHARE"] != nil {
            m.shareRequest += 1
            try? await Task.sleep(for: .milliseconds(2500))
            let menu = ShareButton.openedMenu
            let file = NSApp.mainMenu?.items.dropFirst().first?.submenu
            // As it is about to open: SwiftUI brings its items up to
            // date then, not before.
            if let file { file.delegate?.menuNeedsUpdate?(file) }
            FileHandle.standardError.write(Data(
                "snapshot: share menu \(menu?.items.map(\.title) ?? []) file=\(file?.items.map { $0.title + ($0.keyEquivalent.isEmpty ? "" : "⌘" + $0.keyEquivalent) } ?? [])\n".utf8))
            menu?.cancelTracking()
            // "sheet": its Share… then -- the system's sheet.
            if env["VALTZ_SNAPSHOT_SHARE"] == "sheet",
               let i = menu?.items.lastIndex(where: { !$0.isSeparatorItem }) {
                try? await Task.sleep(for: .milliseconds(300))
                ValtzApp.fromRunLoop { menu?.performActionForItem(at: i) }
            } else if saveSpec.count == 3 {
                try? await Task.sleep(for: .milliseconds(300))
                // From the run loop, as a click is: run from this
                // task, the panel's modal loop would hold the main
                // queue -- and every main-actor task -- until it
                // closed.
                ValtzApp.fromRunLoop { menu?.performActionForItem(at: 0) }
            }
        } else if let m = model, saveSpec.count == 3 {
            ValtzApp.fromRunLoop { ShareButton.save(m) }
        }
        if saveSpec.count == 3 {
            let dest = URL(fileURLWithPath: saveSpec[2])
            for _ in 0..<Int(delay * 4) where
                !FileManager.default.fileExists(atPath: dest.path) {
                try? await Task.sleep(for: .milliseconds(250))
            }
            try? await Task.sleep(for: .milliseconds(500))
            let size = (try? FileManager.default.attributesOfItem(
                atPath: dest.path)[.size] as? Int) ?? -1
            FileHandle.standardError.write(Data(
                "snapshot: saved \(dest.path) bytes=\(size)\n".utf8))
        }
        // Pictures onto the compare's sides, as a drop on A or B makes
        // it (the asset list's rows, and their bases): "b:<asset>"
        // compares an asset as B, "a:base:<asset>" the picture it was
        // made from as A ("newest" the newest generated picture,
        // "newest-edit" the newest with a base); "stage:<asset>" a
        // made clip's or song's Show on Stage ("newest-video",
        // "newest-audio"); "return" puts the current state back in A.
        if let m = model, let spec = env["VALTZ_SNAPSHOT_COMPARE"] {
            let made = m.assets.filter(\.isGeneration)
                .sorted { $0.created > $1.created }
            @MainActor func pick(_ arg: String) -> AssetDTO? {
                switch arg {
                case "newest": made.first { $0.kind == "image" }
                case "newest-video": made.first { $0.kind == "video" }
                case "newest-audio": made.first { $0.kind == "audio" }
                case "newest-edit": made.first { m.baseEntry(of: $0) != nil }
                default: m.assets.first { $0.id == arg }
                }
            }
            for step in spec.split(separator: ";") {
                try? await Task.sleep(for: .milliseconds(600))
                if step == "return" {
                    m.returnToCurrent()
                    continue
                }
                let p = step.split(separator: ":").map(String.init)
                guard p.count >= 2, let a = pick(p.last ?? "") else {
                    continue
                }
                if p[0] == "stage" {
                    m.showMadeOnStage(a)
                    continue
                }
                let slot: StageSlot = p[0] == "a" ? .a : .b
                let took: Bool
                if p.count == 3 && p[1] == "base" {
                    took = m.baseEntry(of: a).map {
                        m.dropOnCompare($0.url, in: slot)
                    } ?? false
                } else {
                    took = m.dropOnCompare(URL(string: AppModel
                        .assetPrefix + a.id)!, in: slot)
                }
                FileHandle.standardError.write(Data(
                    "snapshot: compare \(step) took=\(took)\n".utf8))
            }
            try? await Task.sleep(for: .milliseconds(600))
        }
        // Toggles in turn, animated as their buttons are --
        // "inspector:2,generate:3": the inspector 2 s after this
        // point, then the generation card's drawer 3 s after that
        // (inspector | editor | compare | generate | adjust | crop | trim). Each prints
        // its time, to find it in a filmed run.
        if let m = model, let steps = env["VALTZ_SNAPSHOT_TOGGLES"] {
            for step in steps.split(separator: ",") {
                let p = step.split(separator: ":").map(String.init)
                guard p.count == 2, let w = Double(p[1]) else { continue }
                try? await Task.sleep(for: .seconds(w))
                FileHandle.standardError.write(Data(
                    "snapshot: toggle \(p[0]) at \(Date().timeIntervalSince1970)\n".utf8))
                withAnimation(AppModel.motion) {
                    if p[0] == "inspector" {
                        m.inspectorOpen.toggle()
                    } else if p[0] == "editor" {
                        m.toggleImmersivePrompt()
                    } else if p[0] == "compare" {
                        m.toggleCompare()
                        FileHandle.standardError.write(Data(
                            "snapshot: compare on=\(m.compareOn) b=\(m.stage.b.map { "\($0.width)x\($0.height)" } ?? "-") mode=\(m.stage.mode)\n".utf8))
                    } else if p[0] == "appearance" {
                        // The bulb: light <-> dark, faded.
                        m.setAppearance(dark: !m.appearsDark,
                                        animated: true)
                        // A transition is kept under kCATransition.
                        let fades = NSApp.windows.filter(\.isVisible)
                            .compactMap { w in
                                (w.contentView?.superview?.layer
                                    ?? w.contentView?.layer)?
                                    .animation(forKey: kCATransition)
                                    .map { "\($0.duration)s" }
                            }
                        FileHandle.standardError.write(Data(
                            "snapshot: appearance dark=\(m.appearsDark) fades=\(fades.joined(separator: ","))\n".utf8))
                    } else if let panel = ComposerPanel.allCases.first(
                        where: { "\($0)" == p[0] }) {
                        m.openPanel = m.openPanel == panel ? nil : panel
                    }
                }
            }
        }
        if let m = model, env["VALTZ_SNAPSHOT_DRAG_READY"] != nil {
            m.stageDragReady = true
        }
        if let m = model, let q = env["VALTZ_SNAPSHOT_SEARCH"] {
            m.searchText = q
        }
        // The title bar's zoom group: "in,in,out,fit,actual".
        if let m = model, let z = env["VALTZ_SNAPSHOT_ZOOM"] {
            for step in z.split(separator: ",") {
                switch step {
                case "in": m.zoom(.zoomIn)
                case "out": m.zoom(.zoomOut)
                case "fit": m.zoom(.fit)
                case "actual": m.zoom(.actualSize)
                case "unfit": m.toggleFit()
                default: break
                }
                try? await Task.sleep(for: .milliseconds(200))
            }
        }
        // A pan of the stage ("-4000,2500": points, as a drag or a
        // trackpad moves it -- through the canvas's own pan, in 20
        // steps), then where the picture is in the view. Not mouse
        // events: a test window is never active, and its first click
        // would only activate it.
        if let spec = env["VALTZ_SNAPSHOT_PAN"],
           let win = NSApp.windows.first(where: {
               $0.isVisible && Self.find(CompareCanvas.self,
                                         in: $0.contentView ?? NSView()) != nil
           }),
           let canvas = Self.find(CompareCanvas.self,
                                  in: win.contentView ?? NSView()) {
            let d = spec.split(separator: ",").compactMap { Double($0) }
            if d.count == 2 {
                for _ in 0..<20 {
                    canvas.panBy(dx: d[0] / 20, dy: d[1] / 20)
                }
                try? await Task.sleep(for: .milliseconds(300))
                let shown = canvas.shownRectA
                let seen = shown.intersection(canvas.bounds)
                FileHandle.standardError.write(Data(
                    "snapshot: shown=\(Int(shown.minX)),\(Int(shown.minY)) \(Int(shown.width))x\(Int(shown.height)) in \(Int(canvas.bounds.width))x\(Int(canvas.bounds.height)), visible \(Int(seen.width))x\(Int(seen.height)) pt\n".utf8))
            }
        } else if let spec = env["VALTZ_SNAPSHOT_PAN"],
                  let clip = ClipZoomView.shown, clip.window != nil {
            // A clip on the stage: its zoom view's pan.
            let d = spec.split(separator: ",").compactMap { Double($0) }
            if d.count == 2 {
                for _ in 0..<20 {
                    clip.panBy(dx: d[0] / 20, dy: d[1] / 20)
                }
                try? await Task.sleep(for: .milliseconds(300))
                let shown = clip.frameRect
                let seen = shown.intersection(clip.bounds)
                FileHandle.standardError.write(Data(
                    "snapshot: shown=\(Int(shown.minX)),\(Int(shown.minY)) \(Int(shown.width))x\(Int(shown.height)) in \(Int(clip.bounds.width))x\(Int(clip.bounds.height)), visible \(Int(seen.width))x\(Int(seen.height)) pt clip\n".utf8))
            }
        }
        // A screen ("log"), or Settings on a page ("capabilities",
        // "storage", "general") -- then the snapshot is of that window.
        if let m = model, let s = env["VALTZ_SNAPSHOT_SCREEN"],
           let screen = AppScreen(rawValue: s) {
            m.show(screen)
        }
        let settings = env["VALTZ_SNAPSHOT_SETTINGS"] != nil
        if settings,
           let app = NSApp.mainMenu?.items.first?.submenu,
           let i = app.items.firstIndex(where: {
               $0.keyEquivalent == "," }) {
            // The app menu's Settings…, as a person would choose it.
            app.performActionForItem(at: i)
        }
        // The download prompt's Open Capabilities: Settings on that
        // page, the family opened ("minimax-h3").
        // The app menu's items, as they stand (Check for Updates…
        // there only with Sparkle built in).
        if settings, let app = NSApp.mainMenu?.items.first?.submenu {
            let items = app.items.filter { !$0.isSeparatorItem }
                .map(\.title).joined(separator: "|")
            FileHandle.standardError.write(Data(
                "snapshot: app-menu \(items)\n".utf8))
        }
        if settings, let fam = env["VALTZ_SNAPSHOT_OPEN_CAPS"],
           let m = model {
            try? await Task.sleep(for: .milliseconds(1200))
            m.openCapabilities(family: fam)
            try? await Task.sleep(for: .milliseconds(800))
        }
        // Capabilities' models (DESIGN §8b), in turn (";" between):
        // "link:<id>=<path>" (the link menu's file or folder chosen),
        // "check:<id>" (Check Files), "unlink:<id>", "ask:<id>:<bits>"
        // (a width of the compress menu: its alert open),
        // "answer:compress|compress-keep|cancel" (its buttons -- keep: the
        // original kept beside the copy), "compress:<id>:<bits>"
        // (unasked), "wait:<s>" (until no compress runs, s at most),
        // "use:<id>:compressed|original" (a copy beside its original:
        // which runs), "keep:<id>:compressed|original" (one kept: its
        // alert open, "answer:delete|cancel"),
        // "sleep:<s>" (held, for a capture by window id);
        // DOWNLOADS: "download:<id>", "pause:<id>" (its pause button),
        // "cancel:<id>" (its cancel button: the alert open),
        // "answer:delete|pause|keep" (that alert's buttons),
        // "discard:<id>" (cancelled unasked); "wait:<s>" waits for
        // them too -- each printing `snapshot: models <step>
        // row=state/source/bits/why bytes= check=<notes>
        // compress=<note> job=<phase> stopping= ask=`.
        if settings, let spec = env["VALTZ_SNAPSHOT_MODELS"],
           let m = model {
            try? await Task.sleep(for: .milliseconds(1200))
            var last = ""
            @MainActor
            func member(_ id: String) -> CapabilityTree.Member? {
                m.capabilityTree?.families.flatMap(\.members)
                    .first { $0.model == id }
            }
            @MainActor
            func running() -> JobInfo? {
                m.jobs.values.first {
                    ($0.purpose == "compress" || $0.purpose == "download")
                        && ($0.state == "queued" || $0.state == "running")
                }
            }
            for step in spec.split(separator: ";").map(String.init) {
                let parts = step.split(separator: ":", maxSplits: 1)
                    .map(String.init)
                let verb = parts.first ?? ""
                let arg = parts.count > 1 ? parts[1] : ""
                let idBits = arg.split(separator: ":").map(String.init)
                switch verb {
                case "link":
                    let lp = arg.split(separator: "=", maxSplits: 1)
                        .map(String.init)
                    if lp.count == 2 {
                        last = lp[0]
                        m.linkModel(lp[0], to: URL(fileURLWithPath: lp[1]))
                    }
                case "check":
                    last = arg
                    m.checkModel(arg)
                case "unlink":
                    last = arg
                    m.unlinkModel(arg)
                case "ask":
                    if idBits.count == 2, let mm = member(idBits[0]) {
                        last = idBits[0]
                        m.compressAsk = (mm, Int(idBits[1]) ?? 8)
                    }
                case "answer":
                    if arg.hasPrefix("compress"), let ask = m.compressAsk {
                        last = ask.member.model
                        m.compress(model: ask.member.model,
                                   bits: ask.bits,
                                   keepOriginal: arg == "compress-keep")
                    }
                    m.compressAsk = nil
                    if let ask = m.compressKeepAsk {
                        last = ask.member.model
                        if arg == "delete" {
                            m.keepCompressed(model: ask.member.model,
                                             ask.compressed)
                        }
                        m.compressKeepAsk = nil
                    }
                    // The cancel alert's: as its buttons do.
                    if let ask = m.downloadCancelAsk {
                        last = ask.model
                        if arg == "delete" {
                            m.cancelDownload(model: ask.model)
                        } else if arg == "pause" {
                            m.pauseDownload(model: ask.model)
                        }
                    }
                    m.downloadCancelAsk = nil
                case "download":
                    last = arg
                    m.download(model: arg)
                case "pause":
                    last = arg
                    m.pauseDownload(model: arg)
                case "cancel":
                    last = arg
                    m.downloadCancelAsk = member(arg)
                case "discard":
                    last = arg
                    m.cancelDownload(model: arg)
                case "compress":
                    if idBits.count == 2 {
                        last = idBits[0]
                        m.compress(model: idBits[0],
                                   bits: Int(idBits[1]) ?? 8)
                    }
                case "use":
                    if idBits.count == 2 {
                        last = idBits[0]
                        m.useCompressed(model: idBits[0],
                                        idBits[1] == "compressed")
                    }
                case "keep":
                    if idBits.count == 2, let mm = member(idBits[0]) {
                        last = idBits[0]
                        m.compressKeepAsk = (mm,
                                             idBits[1] == "compressed")
                    }
                case "sleep":
                    try? await Task.sleep(
                        for: .seconds(Double(arg) ?? 1))
                case "wait":
                    let until = Date().addingTimeInterval(
                        Double(arg) ?? 60)
                    try? await Task.sleep(for: .milliseconds(500))
                    while running() != nil, Date() < until {
                        try? await Task.sleep(for: .milliseconds(500))
                    }
                default:
                    break
                }
                try? await Task.sleep(for: .milliseconds(700))
                let r = member(last)
                // A copy's width; beside its original, which runs
                // ("8:copy+orig", "8:orig+copy").
                let row = r.map { mm -> String in
                    let c: String = mm.compressed.map {
                        !$0.besideOriginal ? String($0.bits)
                            : "\($0.bits):"
                              + ($0.copyInUse ? "copy+orig" : "orig+copy")
                    } ?? "-"
                    return "\(mm.state)/\(mm.source)/\(c)/"
                        + "\(mm.compress?.why ?? "none")"
                } ?? "-"
                let check = m.modelChecks[last]?.notes
                    .map { "\($0.level):\($0.text)" }
                    .joined(separator: " | ") ?? "-"
                let note = m.compressNotes[last]
                    .map { "\($0.level):\($0.text)" } ?? "-"
                var job = "-"
                if let j = running() {
                    let what: String = j.phase?.caption(kind: .image)
                        ?? j.state
                    let count: String = j.phase
                        .map { " \($0.done)/\($0.total)" } ?? ""
                    job = what + count
                }
                var stopping = "-"
                if let s = m.downloadStopping[last] {
                    stopping = s ? "cancel" : "pause"
                }
                let bytes: String = r.map { String($0.bytes) } ?? "-"
                let ask = m.compressAsk != nil
                let cancelAsk = m.downloadCancelAsk != nil
                var line = "snapshot: models \(step) row=\(row) "
                line += "path=\(r?.path ?? "-") "
                line += "keepAsk=\(m.compressKeepAsk != nil) "
                line += "bytes=\(bytes) ask=\(ask) "
                line += "cancelAsk=\(cancelAsk) check=\(check) "
                line += "compress=\(note) job=\(job) "
                line += "stopping=\(stopping)\n"
                FileHandle.standardError.write(Data(line.utf8))
            }
        }
        // A gated model's Download (Capabilities): its sheet opened,
        // a token typed if given (never downloaded here), the sheet
        // drawn to <snapshot>-gated.png.
        if settings, let id = env["VALTZ_SNAPSHOT_GATED"], let m = model {
            try? await Task.sleep(for: .milliseconds(1200))
            let member = m.capabilityTree?.families
                .flatMap(\.members).first { $0.model == id }
            m.snapshotGatedToken = env["VALTZ_SNAPSHOT_GATED_TOKEN"]
            m.gatedDownload = member
            try? await Task.sleep(for: .milliseconds(1200))
            let sheet = NSApp.windows.first { $0.isSheet && $0.isVisible }
            if let w = sheet, let v = w.contentView?.superview,
               let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds),
               let snap = env["VALTZ_SNAPSHOT"] {
                v.cacheDisplay(in: v.bounds, to: rep)
                let out = (snap as NSString).deletingPathExtension
                    + "-gated.png"
                try? rep.representation(using: .png, properties: [:])?
                    .write(to: URL(fileURLWithPath: out))
            }
            FileHandle.standardError.write(Data(
                ("snapshot: gated model=\(id) found=\(member != nil) "
                 + "gated=\(member?.isGated ?? false) "
                 + "sheet=\(sheet.map { "\(Int($0.frame.width))x\(Int($0.frame.height))" } ?? "-")\n").utf8))
            // Closed again, as Cancel does: an open sheet holds the
            // quit.
            m.gatedDownload = nil
            try? await Task.sleep(for: .milliseconds(600))
        }
        // Settings' navigation (";" between): "go:<place>" (fleet,
        // fleet-offload, fleet-sharing, ...), "back", "forward",
        // "key:back" / "key:forward" (⌘[ / ⌘] posted to its window),
        // "search:<text>", "clear", "pick:<n>" (the n-th row the
        // sidebar lists, as its click does), "offload:<task|model>=
        // <policy|default>", "share:<task|model>=on|off" -- each
        // printing `snapshot: settings-nav <step> at= history= back=
        // forward= search= rows= title= policies=`.
        if settings, let spec = env["VALTZ_SNAPSHOT_SETTINGS_NAV"],
           let m = model {
            try? await Task.sleep(for: .milliseconds(1500))
            let win = NSApp.windows.first(where: {
                $0.isVisible && $0.identifier?.rawValue
                    .contains("Settings") == true
            })
            @MainActor func rows() -> [SettingsResult] {
                m.settingsSearch.isEmpty
                    ? SettingsPage.allCases.map { SettingsResult(.page($0)) }
                    : SettingsPlace.results(m.settingsSearch, m)
            }
            for step in spec.split(separator: ";").map(String.init) {
                let kv = step.split(separator: ":", maxSplits: 1)
                    .map(String.init)
                let arg = kv.count > 1 ? kv[1] : ""
                switch kv[0] {
                case "go":
                    if let p = SettingsPlace(raw: arg) {
                        m.settingsHistory.go(p)
                    }
                case "back": m.settingsHistory.back()
                case "forward": m.settingsHistory.forward()
                // The buttons' keys shown (⌘? held) over Settings too:
                // "snapshot: settings-hints windows= hints=".
                case "hints":
                    ShortcutHints.shared.show()
                    try? await Task.sleep(for: .milliseconds(300))
                    FileHandle.standardError.write(Data(
                        "snapshot: settings-hints settings=\(win?.windowNumber ?? 0) hints=\(ShortcutHints.shared.summary.replacingOccurrences(of: " ", with: "|"))\n".utf8))
                    try? await Task.sleep(for: .seconds(Double(arg) ?? 1))
                    ShortcutHints.shared.hide()
                case "key":
                    let fwd = arg == "forward"
                    if let w = win, let ev = NSEvent.keyEvent(
                        with: .keyDown, location: .zero,
                        modifierFlags: .command, timestamp: 0,
                        windowNumber: w.windowNumber, context: nil,
                        characters: fwd ? "]" : "[",
                        charactersIgnoringModifiers: fwd ? "]" : "[",
                        isARepeat: false, keyCode: fwd ? 30 : 33) {
                        NSApp.postEvent(ev, atStart: false)
                    }
                case "search": m.settingsSearch = arg
                case "clear": m.settingsSearch = ""
                // Escape posted to Settings (it closes it); the window
                // left elsewhere ("move:x,y", the screen's points); the
                // app menu's Settings… again (centred on the editor).
                case "esc":
                    if let w = NSApp.windows.first(where: {
                           $0.identifier?.rawValue.contains("Settings")
                               == true }),
                       let ev = NSEvent.keyEvent(
                        with: .keyDown, location: .zero, modifierFlags: [],
                        timestamp: 0, windowNumber: w.windowNumber,
                        context: nil, characters: "\u{1b}",
                        charactersIgnoringModifiers: "\u{1b}",
                        isARepeat: false, keyCode: 53) {
                        NSApp.postEvent(ev, atStart: false)
                    }
                case "move":
                    let xy = arg.split(separator: ",").compactMap {
                        Double($0)
                    }
                    if xy.count == 2, let w = NSApp.windows.first(where: {
                        $0.isVisible && $0.identifier?.rawValue
                            .contains("Settings") == true }) {
                        w.setFrameOrigin(NSPoint(x: xy[0], y: xy[1]))
                    }
                case "reopen":
                    if let app = NSApp.mainMenu?.items.first?.submenu,
                       let i = app.items.firstIndex(where: {
                           $0.keyEquivalent == "," }) {
                        app.performActionForItem(at: i)
                    }
                // A listed row clicked: its place -- a setting found on
                // it scrolled to and pulsing.
                case "pick":
                    let r = rows()
                    if let i = Int(arg), r.indices.contains(i) {
                        m.settingsHistory.go(r[i].place)
                        m.settingsReveal = r[i].setting.map {
                            SettingsReveal(name: $0, serial:
                                (m.settingsReveal?.serial ?? 0) + 1)
                        }
                    }
                case "offload", "share":
                    let kv2 = arg.split(separator: "=").map(String.init)
                    guard kv2.count == 2 else { break }
                    let task = FleetTaskLabel.all.contains {
                        $0.op == kv2[0]
                    }
                    if kv[0] == "offload" {
                        let v = kv2[1] == "default" ? "" : kv2[1]
                        if task {
                            m.setFleetOffload(task: kv2[0], v)
                        } else {
                            m.setFleetOffload(model: kv2[0], v)
                        }
                    } else if task {
                        m.setFleetShared(task: kv2[0], kv2[1] == "on")
                    } else {
                        m.setFleetShared(model: kv2[0], kv2[1] == "on")
                    }
                default: break
                }
                try? await Task.sleep(for: .milliseconds(700))
                let h = m.settingsHistory
                let hist = h.places.enumerated().map {
                    $0.offset == h.at ? "*" + $0.element.raw
                                      : $0.element.raw
                }.joined(separator: ",")
                let p = m.fleet?.policies ?? FleetPolicies()
                var pairs: [String] = []
                for (k, v) in p.tasks.sorted(by: { $0.key < $1.key }) {
                    pairs.append("\(k)=\(v)")
                }
                for (k, v) in p.models.sorted(by: { $0.key < $1.key }) {
                    pairs.append("\(k)=\(v)")
                }
                let pol = pairs.isEmpty ? "-"
                    : pairs.joined(separator: ",")
                let heldList: [String] = p.withheldTasks.sorted()
                    + p.withheldModels.sorted()
                let held = heldList.isEmpty ? "-"
                    : heldList.joined(separator: ",")
                let listed = rows().map(\.raw).joined(separator: ",")
                let title = win?.title ?? "-"
                var line = "snapshot: settings-nav \(step)"
                line += " at=\(h.current.raw) history=\(hist)"
                line += " back=\(h.canGoBack) forward=\(h.canGoForward)"
                line += " search=\(m.settingsSearch) rows=\(listed)"
                // The page's settings the search lights, and the one
                // picked from its results.
                let lit = SettingsMarksShown.shared.lit(m.settingsSearch)
                line += " lit=\(lit.isEmpty ? "-" : lit.joined(separator: "|"))"
                line += " picked=\(m.settingsReveal?.name ?? "-")"
                // Where the page's scroll views stand (y of the visible
                // top, of the whole height): a pick's scroll.
                if let content = win?.contentView {
                    var ys: [String] = []
                    func walk(_ v: NSView) {
                        if let sv = v as? NSScrollView,
                           let doc = sv.documentView,
                           doc.frame.height > sv.contentSize.height + 1 {
                            ys.append("\(Int(sv.documentVisibleRect.minY))/\(Int(doc.frame.height))")
                        }
                        v.subviews.forEach(walk)
                    }
                    walk(content)
                    line += " scroll=\(ys.isEmpty ? "-" : ys.joined(separator: ","))"
                }
                line += " title=\(title) policies=\(pol) withheld=\(held)"
                // Where it is: open, its frame and the editor's (the
                // screen's points), and whether it is centred on it.
                let sw = NSApp.windows.first(where: {
                    $0.identifier?.rawValue.contains("Settings") == true
                })
                func box(_ r: CGRect?) -> String {
                    guard let r else { return "-" }
                    return "\(Int(r.minX)),\(Int(r.minY)),\(Int(r.width))x\(Int(r.height))"
                }
                let ef = m.editorWindow?.frame
                let open = sw?.isVisible == true
                let centred = open && ef.map { e in
                    let f = sw!.frame
                    return abs(f.midX - e.midX) < 2 && abs(f.midY - e.midY) < 2
                } == true
                line += " open=\(open) frame=\(box(open ? sw?.frame : nil))"
                line += " editor=\(box(ef)) centred=\(centred)"
                FileHandle.standardError.write(Data((line + "\n").utf8))
            }
        }
        // Clicks posted to the Settings window ("x,y" points from its
        // top left; ";" between) once it is open.
        if settings, let spec = env["VALTZ_SNAPSHOT_SETTINGS_CLICKS"] {
            try? await Task.sleep(for: .milliseconds(1500))
            let win = NSApp.windows.first(where: {
                $0.isVisible && $0.identifier?.rawValue
                    .contains("Settings") == true
            }) ?? NSApp.keyWindow
            for c in spec.split(separator: ";") {
                let xy = c.split(separator: ",").compactMap {
                    Double($0)
                }
                guard xy.count == 2, let win else { continue }
                Self.postClick(x: xy[0], y: xy[1], in: win)
                try? await Task.sleep(for: .milliseconds(800))
                FileHandle.standardError.write(Data(
                    "snapshot: settings-click \(Int(xy[0])),\(Int(xy[1]))\n".utf8))
            }
        }
        try? await Task.sleep(for: .seconds(max(0, delay - 1)))
        // The Settings page scrolled down before it is drawn
        // ("<points>", or "bottom"): what lies below its first screen.
        if settings, let spec = env["VALTZ_SNAPSHOT_SETTINGS_SCROLL"],
           let win = NSApp.windows.first(where: {
               $0.isVisible && $0.identifier?.rawValue
                   .contains("Settings") == true
           }) ?? NSApp.keyWindow {
            func scrolls(_ v: NSView) -> [NSScrollView] {
                (v as? NSScrollView).map { [$0] } ?? []
                    + v.subviews.flatMap(scrolls)
            }
            if let sv = scrolls(win.contentView ?? NSView())
                .max(by: {
                    ($0.documentView?.frame.height ?? 0)
                        < ($1.documentView?.frame.height ?? 0)
                }), let doc = sv.documentView {
                let room = max(0, doc.frame.height
                               - sv.contentView.bounds.height)
                let y = spec == "bottom" ? room
                    : min(room, Double(spec) ?? 0)
                sv.contentView.scroll(to: NSPoint(
                    x: 0, y: doc.isFlipped ? y : room - y))
                sv.reflectScrolledClipView(sv.contentView)
                FileHandle.standardError.write(Data(
                    "snapshot: settings-scroll \(Int(y)) of \(Int(room))\n".utf8))
            }
            try? await Task.sleep(for: .milliseconds(600))
        }
        // A popover open now (a window of its own) drawn beside the
        // snapshot: <snapshot>-popover.png (VALTZ_SNAPSHOT_POPOVER=1).
        if env["VALTZ_SNAPSHOT_POPOVER"] != nil,
           let shot = env["VALTZ_SNAPSHOT"] {
            let pops = NSApp.windows.filter {
                $0.isVisible && $0.className.contains("Popover")
            }
            if let v = pops.first?.contentView,
               let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) {
                v.cacheDisplay(in: v.bounds, to: rep)
                try? rep.representation(using: .png, properties: [:])?
                    .write(to: URL(fileURLWithPath: shot
                        .replacingOccurrences(of: ".png",
                                              with: "-popover.png")))
            }
            FileHandle.standardError.write(Data(
                "snapshot: popover \(pops.isEmpty ? "-" : "drawn") at=\(pops.first.map { "\(Int($0.frame.minX)),\(Int($0.frame.minY))" } ?? "-")\n".utf8))
        }
        // The window to capture: Settings when it was asked for, else
        // the main one.
        @MainActor func target() -> NSWindow? {
            if settings,
               let w = NSApp.windows.first(where: {
                   $0.isVisible && $0.identifier?.rawValue
                       .contains("Settings") == true
               }) ?? NSApp.keyWindow {
                return w
            }
            return NSApp.windows.first(where: { $0.isVisible })
        }
        // VALTZ_SNAPSHOT_NOTICE="error:<words>" (info, warning, error): a
        // notice floated over the composer just before the capture, as
        // AppModel.flash shows one; its frame printed.
        if let spec = env["VALTZ_SNAPSHOT_NOTICE"], let m = model {
            let parts = spec.split(separator: ":", maxSplits: 1)
                .map(String.init)
            let level: Notice.Level = switch parts.first {
            case "error": .error
            case "warning": .warning
            default: .info
            }
            m.flash(parts.count > 1 ? parts[1] : spec, level: level)
            try? await Task.sleep(for: .seconds(3))
            FileHandle.standardError.write(Data(
                "snapshot: notice level=\(parts.first ?? "-") text=\(m.banner ?? "-")\n".utf8))
        }
        // VALTZ_SNAPSHOT_DROP_VIEWS=1: every AppKit view registered
        // for drops, its frame in the window (points from the top
        // left), its types -- beside the Assets list's frame: what
        // AppKit may hand a Finder drag to instead of SwiftUI's view.
        // (=assets: the inspector's Assets shown first.)
        if env["VALTZ_SNAPSHOT_DROP_VIEWS"] == "assets", let m = model {
            m.revealInspector(.assets)
            try? await Task.sleep(for: .seconds(1))
        }
        if env["VALTZ_SNAPSHOT_DROP_VIEWS"] != nil,
           let w = NSApp.windows.first(where: { $0.isVisible }),
           let root = w.contentView?.superview ?? w.contentView {
            let h = root.bounds.height
            func walk(_ v: NSView, _ depth: Int) {
                if !v.registeredDraggedTypes.isEmpty {
                    let r = v.convert(v.bounds, to: nil)
                    let vis = v.convert(v.visibleRect, to: nil)
                    let f = { (r: NSRect) in
                        "\(Int(r.minX)),\(Int(h - r.maxY)) \(Int(r.width))x\(Int(r.height))"
                    }
                    FileHandle.standardError.write(Data(
                        "snapshot: drop-view depth=\(depth) \(type(of: v)) frame=\(f(r)) visible=\(f(vis)) hidden=\(v.isHiddenOrHasHiddenAncestor) types=\(v.registeredDraggedTypes.map(\.rawValue).sorted().joined(separator: ","))\n".utf8))
                }
                for sub in v.subviews { walk(sub, depth + 1) }
            }
            walk(root, 0)
            if let host = w.contentView {
                var names: [String] = []
                var k: AnyClass? = object_getClass(host)
                while let c = k, c != NSView.self {
                    var n: UInt32 = 0
                    if let ms = class_copyMethodList(c, &n) {
                        for j in 0..<Int(n) {
                            let m = NSStringFromSelector(
                                method_getName(ms[j]))
                            let l = m.lowercased()
                            if l.contains("drag") || l.contains("hittest") {
                                names.append(m)
                            }
                        }
                        free(ms)
                    }
                    k = class_getSuperclass(c)
                }
                FileHandle.standardError.write(Data(
                    "snapshot: drop-host \(type(of: host)) methods=\(names.joined(separator: ","))\n".utf8))
            }
            let a = model?.assetListFrame ?? .zero
            FileHandle.standardError.write(Data(
                "snapshot: drop-views assetList=\(Int(a.minX)),\(Int(a.minY)) \(Int(a.width))x\(Int(a.height))\n".utf8))
            // VALTZ_SNAPSHOT_FAKE_DROP="<file>@x,y;...": a Finder-like
            // drag of the file (a file URL on its own pasteboard)
            // handed, as AppKit hands one, to the frontmost view
            // registered for drops at that point -- entered, updated,
            // performed -- printing what it said.
            if let spec = env["VALTZ_SNAPSHOT_FAKE_DROP"] {
                for one in spec.split(separator: ";").map(String.init) {
                    // "hover:<file>@x,y": entered and moved, never
                    // dropped -- the capture shows what a drag over
                    // there shows.
                    // "win:...": handed to the view AppKit's own search
                    // finds (the window's), not the one found here.
                    let viaWindow = one.hasPrefix("win:")
                    let spec = viaWindow ? String(one.dropFirst(4)) : one
                    let hover = spec.hasPrefix("hover:")
                    let parts = (hover ? String(spec.dropFirst(6)) : spec)
                        .split(separator: "@").map(String.init)
                    guard parts.count == 2 else { continue }
                    // "x,y<x0,y0": the drag comes in at x0,y0 first.
                    let at = parts[1].split(separator: "<")
                        .map(String.init)
                    let xy = at[0].split(separator: ",")
                        .compactMap { Double($0) }
                    guard xy.count == 2 else { continue }
                    let p = NSPoint(x: xy[0], y: h - xy[1])
                    let from = at.count > 1
                        ? at[1].split(separator: ",").compactMap {
                            Double($0) } : []
                    var hit: NSView?
                    func front(_ v: NSView) {
                        if !v.registeredDraggedTypes.isEmpty,
                           !v.isHiddenOrHasHiddenAncestor,
                           v.convert(v.bounds, to: nil).contains(p) {
                            hit = v
                        }
                        for sub in v.subviews { front(sub) }
                    }
                    front(root)
                    // As AppKit's own search goes: into a view only
                    // where the point is inside it.
                    var inside: NSView?
                    func within(_ v: NSView) {
                        guard !v.isHidden,
                              v.convert(v.bounds, to: nil).contains(p)
                        else { return }
                        if !v.registeredDraggedTypes.isEmpty {
                            inside = v
                        }
                        for sub in v.subviews { within(sub) }
                    }
                    within(root)
                    let before = model?.assets.count ?? 0
                    // What this drop says, alone: `banner=`.
                    model?.banner = nil
                    // "row:<asset name>": a row of the list dragged,
                    // as its drag carries it (text, "valtz-asset:id").
                    let row = parts[0].hasPrefix("row:")
                        ? model?.assets.first(where: {
                            $0.name == String(parts[0].dropFirst(4))
                        }) : nil
                    // "ref:<n>": the prompt row's n-th medium, as its
                    // thumbnail's drag carries it.
                    let ref = parts[0].hasPrefix("ref:")
                        ? Int(parts[0].dropFirst(4)).flatMap { n in
                            model?.promptAttachments.indices
                                .contains(n) == true
                                ? model?.promptAttachments[n].id : nil
                        } : nil
                    let info = ref.map {
                        FakeDrag(text: PromptTextView.referencePrefix
                                     + $0.uuidString,
                                 at: p, window: w)
                    } ?? row.map {
                        FakeDrag(text: AppModel.assetPrefix + $0.id,
                                 at: p, window: w)
                    } ?? FakeDrag(
                        file: URL(fileURLWithPath: parts[0]),
                        at: p, window: w)
                    // Where AppKit itself would hand this drag.
                    let find = NSSelectorFromString("_findDragTargetFrom:")
                    // Coming in elsewhere first, as a real drag crosses
                    // the window to where it is let go: entered there,
                    // moved, and gone on (its exit) -- `crossed=`.
                    var crossed = "-"
                    if from.count == 2, w.responds(to: find) {
                        info.location = NSPoint(x: from[0], y: h - from[1])
                        if let first = w.perform(find, with: info)?
                            .takeUnretainedValue() as? NSView {
                            let d = first as NSDraggingDestination
                            let e = d.draggingEntered?(info) ?? []
                            let u = d.draggingUpdated?(info) ?? []
                            try? await Task.sleep(for: .milliseconds(300))
                            info.location = p
                            d.draggingExited?(info)
                            crossed = "\(type(of: first)):\(e.rawValue)/\(u.rawValue)"
                        }
                        info.location = p
                    }
                    let appkit = w.responds(to: find)
                        ? w.perform(find, with: info)?
                            .takeUnretainedValue() as? NSView
                        : nil
                    if viaWindow { hit = appkit }
                    let entered = hit.map {
                        ($0 as NSDraggingDestination)
                            .draggingEntered?(info) ?? []
                    } ?? []
                    let updated = hit.map {
                        ($0 as NSDraggingDestination)
                            .draggingUpdated?(info) ?? []
                    } ?? []
                    let prepared = !hover && (hit.map {
                        ($0 as NSDraggingDestination)
                            .prepareForDragOperation?(info) ?? true
                    } ?? false)
                    let performed = prepared && (hit.map {
                        ($0 as NSDraggingDestination)
                            .performDragOperation?(info) ?? false
                    } ?? false)
                    if let hit, !hover {
                        (hit as NSDraggingDestination)
                            .concludeDragOperation?(info)
                    }
                    try? await Task.sleep(for: .seconds(3))
                    var rowFolder = "-"
                    if let r = row, let m = model,
                       let now = m.assets.first(where: { $0.id == r.id }) {
                        let fid = now.folder ?? ""
                        rowFolder = fid.isEmpty ? "top"
                            : (m.assetFolders.first { $0.id == fid }?
                                .name ?? fid)
                    }
                    FileHandle.standardError.write(Data(
                        "snapshot: fake-drop at=\(parts[1]) crossed=\(crossed) view=\(hit.map { "\(type(of: $0))@\(Int($0.convert($0.bounds, to: nil).minX)),\(Int(h - $0.convert($0.bounds, to: nil).maxY))" } ?? "-") entered=\(entered.rawValue) updated=\(updated.rawValue) performed=\(performed) assets=\(before)>\(model?.assets.count ?? 0) rowFolder=\(rowFolder) inside=\(inside.map { "\(type(of: $0))" } ?? "-") appkit=\(appkit.map { "\(type(of: $0))@\(Int($0.convert($0.bounds, to: nil).minX)),\(Int(h - $0.convert($0.bounds, to: nil).maxY))" } ?? "-") mentions=\(model?.promptMentions.count ?? 0) banner=\(model?.banner ?? "-")\n".utf8))
                }
            }
        }
        do {
            // VALTZ_SNAPSHOT_APPKIT=1: skip ScreenCaptureKit, draw the
            // whole window frame (title bar included) with AppKit.
            if env["VALTZ_SNAPSHOT_APPKIT"] != nil {
                throw CocoaError(.featureUnsupported)
            }
            let content = try await SCShareableContent.currentProcess
            let number = target().map { CGWindowID($0.windowNumber) }
            guard let win = content.windows.first(where: {
                $0.isOnScreen && $0.windowLayer == 0
                    && (number == nil || $0.windowID == number)
            }) else { throw CocoaError(.fileNoSuchFile) }
            let filter = SCContentFilter(desktopIndependentWindow: win)
            let cfg = SCStreamConfiguration()
            let scale = CGFloat(filter.pointPixelScale)
            cfg.width = Int(filter.contentRect.width * scale)
            cfg.height = Int(filter.contentRect.height * scale)
            cfg.showsCursor = false
            let img = try await SCScreenshotManager.captureImage(
                contentFilter: filter, configuration: cfg)
            let url = URL(fileURLWithPath: path) as CFURL
            if let dst = CGImageDestinationCreateWithURL(
                url, UTType.png.identifier as CFString, 1, nil) {
                CGImageDestinationAddImage(dst, img, nil)
                CGImageDestinationFinalize(dst)
            }
        } catch {
            FileHandle.standardError.write(Data(
                "snapshot: ScreenCaptureKit unavailable (\(error.localizedDescription)); using cacheDisplay\n".utf8))
            if let content = target()?.contentView,
                let view = content.superview ?? Optional(content),
                let rep = view.bitmapImageRepForCachingDisplay(
                    in: view.bounds) {
                view.cacheDisplay(in: view.bounds, to: rep)
                try? rep.representation(using: .png, properties: [:])?
                    .write(to: URL(fileURLWithPath: path))
            }
        }
        if let m = model {
            // The focus's undo as it is now (no event may have come
            // since the last scripted step).
            m.refreshTextUndo()
            let ready = m.capabilities
                .filter { $0.availability == "ready" }
                .map(\.capability)
            let models = m.installedImageModels.map(\.model)
            // The tray's words, a clip's keyframes, Favor's hint and
            // Custom's values (the model's own).
            let trayLead = ComposerStack.summaryLead(m)
                .replacingOccurrences(of: " ", with: "_")
            var keyframes = "-"
            if let k = m.keyframes {
                keyframes = k.first.url.lastPathComponent
                if let l = k.last {
                    keyframes += ">" + l.url.lastPathComponent
                }
            }
            let preset = m.preferenceHint
                .replacingOccurrences(of: " ", with: "_")
            var tuneValues = "-"
            if let t = m.tuningInfo?.tuning {
                let nums: [String] = ["steps", "video_shift",
                                      "audio_shift"].compactMap { k in
                    t.numbers[k].map { "\(k):\(String(format: "%g", $0))" }
                }
                let loras: [String] = t.loras.filter(\.on).map {
                    URL(fileURLWithPath: $0.path).lastPathComponent
                }
                let picked: [String] = t.choices.keys.sorted().map {
                    "\($0):\(t.choices[$0] ?? "")"
                }
                tuneValues = (nums + picked).joined(separator: ",") + ";"
                    + loras.joined(separator: "+")
            }
            // A zoomed clip's frame on the card.
            let videoShown: String = m.stageVideoRect.map { r in
                "\(Int(r.minX)),\(Int(r.minY)),\(Int(r.width))x\(Int(r.height))"
            } ?? "-"
            // What a stack's frames are drawn at (they follow the zoom).
            let videoRender: String = (ClipZoomView.shown?.player?
                .currentItem?.videoComposition?.renderSize).map {
                    "\(Int($0.width))x\(Int($0.height))"
                } ?? "-"
            let line = "snapshot: project=\(m.projectName) "
                + "assets=\(m.assets.count) imageModel=\(m.imageModel) "
                + "editModel=\(m.editModel) willEdit=\(m.willEdit) "
                + "modality=\(m.modality.rawValue) "
                + "modelChoice=\(m.modelChoice.isEmpty ? "auto" : m.modelChoice) "
                + "panel=\(String(describing: m.openPanel)) "
                + "compact=\(m.promptCompact) "
                + "title=\(m.windowTitle) inspector=\(m.inspectorOpen) "
                + "titlebarHover=\(m.titlebarHover) "
                + "anonymous=\(m.isAnonymous) projectPath=\(m.projectPath) "
                + "base=\(m.baseAttachment?.url.lastPathComponent ?? "-") refs=\(m.promptAttachments.filter { !$0.isBase }.count) "
                + "dragReady=\(m.stageDragReady && m.canDragStage) attachments=\(m.promptAttachments.count) "
                + "hits=\(m.searchHits.sorted()) "
                + "window=\(NSApp.windows.first { $0.isVisible }.map { "\(Int($0.frame.width))x\(Int($0.frame.height))" } ?? "-") "
                + "screen=\(NSScreen.main.map { "\(Int($0.visibleFrame.width))x\(Int($0.visibleFrame.height))" } ?? "-") "
                + "installed=\(models) ready=\(ready) "
                + "zoom=\(m.currentViewer.zoomText) fit=\(m.currentViewer.fitting) actual=\(m.currentViewer.isActualSize) canZoom=\(m.canZoom) videoShown=\(videoShown) videoRender=\(videoRender) "
                + "output=\(m.dimensions.width)x\(m.dimensions.height) "
                + "sizes=\(m.sizeClasses.map { "\($0)" }.joined(separator: ",")) "
                + "composer=\(Int(m.composerWidth))/\(Int(m.composerColumn)) "
                + "drawers=\(m.drawerWidths.sorted { $0.key < $1.key }.map { "\($0.key):\(Int($0.value))" }.joined(separator: ",")) "
                + "sizeShown=\(m.sizeChoice.map { "\($0)" } ?? "custom") "
                + "custom=\(m.customSize != nil) orientation=\(m.shownOrientation.rawValue) "
                + "ratioShown=\(m.showsAspectRatio) "
                + "marked=\(m.promptMarked.replacingOccurrences(of: "\u{FFFC}", with: "[*]").replacingOccurrences(of: " ", with: "_")) "
                + "languages=\(L10n.available.joined(separator: ",")) "
                + "baseHint=\(m.baseHint.flatMap { h in m.promptAttachments.first { $0.id == h }?.url.lastPathComponent } ?? "-") "
                + "refs=\(m.promptAttachments.map { a in (a.isBase ? "*" : "") + (a.ownCopy ? "+" : "") + a.url.lastPathComponent + (m.referenceNumber(a).map { "#\($0)" } ?? "") }.joined(separator: ",")) "
                + "active=\((m.assets.first { $0.id == m.stageAssetId }?.name ?? "-").replacingOccurrences(of: " ", with: "_")) "
                + "assets=\(m.assets.count) ops=\(Dictionary(grouping: m.assets, by: { $0.op ?? "import" }).map { "\($0.key):\($0.value.count)" }.sorted().joined(separator: ",")) stale=\(m.assets.filter { m.isStale($0, used: m.assetsInUse) }.count) folders=\(m.assetFolders.map { f in "\(f.name)\(m.closedFolders.contains(f.id) ? "-folded" : ""):\(m.assets.filter { $0.folder == f.id }.count)" }.joined(separator: "|").replacingOccurrences(of: " ", with: "_")) renaming=\(m.renamingAsset ?? "-") "
                + "mentions=\(m.promptMentions.count) focus=\(m.promptAttachments.firstIndex { $0.id == m.focusedReference }.map(String.init) ?? "-") "
                + "stack=\(m.stackPlayback.map { "\($0.width)x\($0.height)/\($0.frames)f/\($0.clips.count)clips" } ?? "-") "
                + "look=\(m.stageStack?.layerStack.map { l in let k = m.layerLook(l.id); return "\(l.id.isEmpty ? "0" : l.id):\(k.adjust ? "a" : "")\(k.place ? "p" : "")\(k.turn ? "t" : "")\(k.keyed ? "k" : "")" }.joined(separator: ",") ?? "-") "
                + "layers=\(m.stageStack?.layerStack.map { "\($0.id.isEmpty ? "0" : $0.id)\($0.visible ? "" : "-hidden")\($0.isEmpty ? "-empty" : "")\($0.isMarkup ? "-markup\($0.markup?.raster.isEmpty == false ? "+px" : "")\($0.markup.map { "+\($0.objects.count)obj" } ?? "")" : "")\($0.mask ? "-mask" : "")" }.joined(separator: ",") ?? "-") selectedLayer=\(m.activeLayer.isEmpty ? "0" : m.activeLayer) selectedLayers=\(m.selectedLayers.map { $0.isEmpty ? "0" : $0 }.sorted()) inspector=\(m.inspectorStacked ? "stacked" : "one"):\(InspectorTab.allCases.filter { !m.inspectorFolded.contains($0) }.map(\.rawValue).joined(separator: "+")) panels=\(Self.panelSummary(m)) tasks=\(Self.taskSummary(m)) projectOutput=\(m.projectOutput.color),\(m.projectOutput.fps.map(String.init).joined(separator: "/")),\(m.projectOutput.channels),\(m.projectOutput.sampleRate),\(m.projectOutput.intermediate) options=\(m.modelOptions(for: m.activeModality).map(\.name).joined(separator: "|").replacingOccurrences(of: " ", with: "_")) runningModel=\(m.runningModel) hint=\(String(m.promptHint.prefix(40)).replacingOccurrences(of: " ", with: "_")) promptCompact=\(m.promptCompact) pictureRoom=\(Int(m.pictureRoom))/\(Int(m.editorRoom)) framed=\(m.stageStack?.canvas.map { c in c.framed ? "\(c.fw ?? 0)x\(c.fh ?? 0)" + (c.resized ? "@\(c.w)x\(c.h)" : "") : "-" } ?? "-") "
                + "markup=\(m.markupOpen ? m.markup.tool.rawValue : "off"):sel[\(m.markup.selection.map(\.kind.rawValue).joined(separator: ","))\(m.markup.drawing ? "+drawing" : "")]@\(m.markup.selectionLayer ?? "-") "
                // The first selected object's style: a text's
                // alignment and spacing, its effects.
                + "markupStyle=\(m.markup.selection.first.map { o in [o.kind == .text ? (o.align ?? "left") : nil, o.lineSpacing.map { String(format: "x%.1f", $0) }, o.effects.isEmpty ? nil : o.effects.map(\.kind.rawValue).joined(separator: "+"), o.gradient.map { "\($0.kind.rawValue)\($0.stops.count)" }, o.kind == .polygon ? "corners\(o.points.count)" : nil].compactMap { $0 }.joined(separator: ":") } ?? "-") "
                + "favor=\(m.preference.rawValue) tuning=\(m.preference == .custom ? m.customSummary : "-") tunePanel=\(m.showsTuning) "
                + "tuneValues=\(tuneValues) "
                + "upscale=\(m.upscaleJob != nil ? "running" : m.canUpscaleLayer ? "offered" : m.upscaleNeedsFlatten ? "flatten-first" : "-") "
                + "capture=\(m.capture.recording ? "recording" : m.showsCapture ? "offered" : "-") captureSources=\(m.captureSources.map(\.kind).joined(separator: ",")) "
                + "viewed=\(m.viewedAsset.flatMap { id in m.assets.first { $0.id == id }?.name.replacingOccurrences(of: " ", with: "_") } ?? "-") stageLift=\(String(format: "%.2f", m.stageLift)) "
                + "pages=\(m.stagePages) page=\(m.stagePage + 1) layerPages=\(m.stagePicture.map { p in p.layerStack.map { l in "\(l.id.isEmpty ? "0" : l.id):\(p.pageSpan(of: l).map { "\($0.lowerBound + 1)-\($0.upperBound + 1)" } ?? "none")" }.joined(separator: ",") } ?? "-") keyed=\(m.keyedStage) keys=\(m.keyFrames(.place).map(String.init).joined(separator: ",")) "
                + "memoryFailure=\(m.memoryFailure.map { f in "\(f.step):\(MemoryFailure.size(f.need)):\(f.gates.filter { !$0.ok }.map(\.name).joined(separator: "+")):parts=\(f.parts.map(\.name).joined(separator: "+")):plan=\(MemoryFailure.size(f.planPeak))" } ?? "-") "
                + "suggests=\([m.suggestsShorterClip ? "length" : nil, m.suggestsSmallerSize ? "size" : nil, m.suggestsFewerReferences ? "refs" : nil].compactMap { $0 }.joined(separator: ",")) "
                + "bypassed=\(m.bypassed.map { "\($0)" }.sorted()) "
                + "statusBar=\(m.showsStatusBar) "
                + "statusItems=\(Self.statusItems(m)) "
                + "createTabs=\(m.modalities.map(\.rawValue).joined(separator: ",")) "
                + "agentSpeed=\(m.agentShowsSpeed ? "on" : "off") "
                // The stage player's own controls: floating, or none
                // (the timeline open).
                + "playerControls=\(NSApp.windows.first { $0.isVisible }?.contentView.flatMap { Self.find(AVPlayerView.self, in: $0) }.map { $0.controlsStyle == .none ? "none" : "floating" } ?? "-") "
                // The fleet: in one or not, its members reached, a
                // job served here.
                + "fleet=\(m.fleet?.config.map { $0.joined ? $0.fleet.replacingOccurrences(of: " ", with: "_") : "-" } ?? "off"):\((m.fleet?.members ?? []).filter { $0.state == "connected" }.map { $0.name.replacingOccurrences(of: " ", with: "_") }.joined(separator: ",")) "
                + "fleetServing=\(m.fleetServing.map { "\($0.from)/\($0.op)/\(Int(($0.progress ?? 0) * 100))%" } ?? "-") "
                + "fleetAsks=\(m.fleetAsks.count) "
                + "localNetwork=\(m.fleet?.localNetwork.flatMap { $0.isEmpty ? nil : $0 } ?? "-") "
                + "load=ane:\(m.monitor.load.ane.map { String(format: "%.1f", $0) } ?? "-"),gpu:\(m.monitor.load.gpu.map { String(format: "%.1f", $0) } ?? "-"),ram:\(m.monitor.load.footprint.map { String(format: "%.2fGB", $0 / 1e9) } ?? "-"),sys:\(m.monitor.load.systemUsed.map { String(format: "%.2fGB", $0 / 1e9) } ?? "-") "
                + "flops=\(Self.flopsSummary(m.monitor.load.flops)) "
                + "fleetFlops=\(Self.flopsSummary(m.monitor.load.fleet)) "
                + "thermal=\(m.monitor.thermal?.verdict ?? "-"):\(m.monitor.thermalLabel.replacingOccurrences(of: " ", with: "_")) "
                + "firstResponder=\(NSApp.windows.first { $0.isVisible }?.firstResponder.map { r in ((r as? NSTextView)?.delegate as? NSTextField).map { "field(\($0.placeholderString ?? $0.stringValue))" } ?? String(describing: type(of: r)) } ?? "-") "
                + "stageA=\(m.stage.aLabel.replacingOccurrences(of: " ", with: "_")) stageB=\(m.stage.bLabel.replacingOccurrences(of: " ", with: "_")) "
                + "history=\(m.history.count) current=\(m.currentSlot.map { "\($0)" } ?? "-") takesCompare=\(m.stageTakesCompare) compareOn=\(m.compareOn) compareB=\(m.stage.b.map { "\($0.width)x\($0.height)" } ?? "-")@\(String(format: "%.4f", m.stage.bFit.scale)),\(String(format: "%.1f", m.stage.bFit.origin.x)),\(String(format: "%.1f", m.stage.bFit.origin.y)) compareA=\(m.stage.a.map { "\($0.width)x\($0.height)" } ?? "-") mode=\(m.stage.mode) dirty=\(m.document.dirty) untitled=\(m.document.untitled) undoNext=\(m.document.undo ?? "-") redoNext=\(m.document.redo ?? "-") undoTitle=\(m.undoMenuTitle.replacingOccurrences(of: " ", with: "_")) textUndo=\(m.textUndo.map { "\($0.canUndo)" } ?? "-") compared=\(m.stage.aCompared.map { _ in "a" } ?? "")\(m.stage.bCompared.map { _ in "b" } ?? "") "
                + "stageMode=\(m.stage.mode.rawValue) "
                + "bFit=\(m.stage.bFit.scale)@\(m.stage.bFit.origin.x),\(m.stage.bFit.origin.y) "
                + "b=\(m.stage.b.map { "\($0.width)x\($0.height)" } ?? "-") "
                + "a=\(m.stage.a.map { "\($0.width)x\($0.height)" } ?? "-") "
                + "clip=\(m.stage.clip.map { "\($0.frames.count)f@\($0.fps)" } ?? "-") "
                + "video=\(m.stage.video?.lastPathComponent ?? "-") stageVisible=\(m.stageVisible) stageCorners=\(m.roundStageCorners ? "round" : "square") suggestion=\(m.enhanced != nil) "
                + "clipFrames=\(m.clipFrames) favorHint=\(m.preferenceHint.replacingOccurrences(of: " ", with: "_")) "
                + "wipeLine=\(m.stage.wipeLine) canDragStage=\(m.canDragStage) "
                + "lastError=\(m.log.last { $0.level == "error" }?.text ?? "-") "
                + "banner=\((m.banner ?? "-").replacingOccurrences(of: " ", with: "_")) "
                + "generating=\(m.isGenerating) exporting=\(m.exportShow != nil) "
                + "stackFrames=\(m.stackCounts.map { "\($0.drawn)/\($0.skipped)" } ?? "-") "
                + "promptText=\(m.prompt.replacingOccurrences(of: " ", with: "_").replacingOccurrences(of: "\n", with: "|")) "
                + "immersive=\(m.promptImmersive) editorEdited=\(m.editorEdited) tabs=\(m.promptTabs.count) activeTab=\(m.activePromptTab) tabTitles=\(m.promptTabs.indices.map { m.promptTabTitle($0).replacingOccurrences(of: " ", with: "_") }.joined(separator: "|")) markdown=\(m.promptMarkdown) "
                + "promptAsset=\(m.promptAssetId ?? "-") unbound=\(m.unboundRefTags.joined(separator: ",")) refTags=\(AppModel.refTags(in: m.prompt).count) prompts=\(m.assets.filter(\.isPrompt).count) "
                + "references=\(m.usesReferences) badges=\(m.promptAttachments.map { (m.referenceBadge($0) ?? "-") + ($0.continues ? ">" : "") }.joined(separator: ",")) canContinue=\(m.canContinueClip) guides=\(m.guideLengths.map { "\($0.asked):\($0.frames)" }.joined(separator: ",")) running=\(m.runningModel) "
                + "trayLead=\(trayLead) keyframes=\(keyframes) preset=\(preset) "
                + "songPlan=\(m.songPlan.rawValue) songLength=\(m.songSeconds.map(String.init) ?? "auto") "
                + "lyrics=\(m.songWords.sections)s/\(m.songWords.lines)l style=\(m.songWords.style.replacingOccurrences(of: " ", with: "_")) "
                + "audio=\(m.stageIsAudio) rate=\(m.stageFrameRate.num)/\(m.stageFrameRate.den) timecode=\(m.stageFrameRate.timecode(m.videoFrame)) "
                + "crop=\(m.crop.json.keys.sorted().map { "\($0):\(m.crop.json[$0]!)" }.joined(separator: ",")) "
                + "guides=\(m.showsGuides) "
                + "cropPlaced=\(m.stageCropPlacement.map { "\(Int($0.canvas.width))x\(Int($0.canvas.height))" } ?? "-") "
                + "trim=\(m.trim.markIn.map(String.init) ?? "-")..\(m.trim.markOut.map(String.init) ?? "-") offset=\(m.trimOffset) "
                + "clipTime=\(m.clipEdge?.rawValue ?? m.trimRate.timecode(m.sourceFrameAtPlayhead)) clipEdge=\(m.clipEdge?.rawValue ?? "-") layerOff=\(m.layerOffStage?.rawValue ?? "-") canEditLayer=\(m.canEditLayerHere) layerEnd=\(m.layerClock.map { $0.end.isFinite ? String(format: "%.3f", $0.end) : "inf" } ?? "-") "
                + "speed=\(m.layerSpeed.keys.map { "\($0.frame):\(CropField.display($0.value.rate))" }.joined(separator: ",")) sound=\(m.layerSound.keys.map { "\($0.frame):\(CropField.display($0.value.volume))/\(CropField.display($0.value.pitch))" }.joined(separator: ",")) followSpeed=\(m.pitchFollowsSpeed) resampled=\(m.layerResampled) timedLayer=\(m.layerIsTimed) layerSound=\(m.layerHasSound) "
                + "viewing=\(m.stageViewingName.map { "\"\($0)\"" } ?? "-") project=\(m.projectViews.first.map { p in (p.kind == "audio" ? "sound" : p.isTimeline ? "timeline" : "still") + (p.ownFrame.map { ":\($0.w)x\($0.h)" } ?? "") + ":\(p.layerStack.count)layers" } ?? "-") "
                + "videoFrame=\(m.videoFrame) showsCrop=\(m.showsCrop) "
                + "showsTrim=\(m.showsTrim) showsAdjust=\(m.showsAdjust) "
                + "adjustKeys=\(m.keyFrames(.adjust)) placeKeys=\(m.keyFrames(.place)) turnKeys=\(m.keyFrames(.turn)) "
                + "cropHere=\(CropField.allCases.map { CropField.display($0.value(m.crop)) }.joined(separator: ",")) videoRate=\(m.videoRate) "
                + "exposureHere=\(m.adjustments.exposure) rotateHere=\(m.crop.rotate) "
                + "\n"
            FileHandle.standardError.write(Data(line.utf8))
        }
        NSApp.terminate(nil)
    }

    /// "exposure=0.4,saturation=0.3" as adjustments.
    private static func adjustments(_ spec: String) -> ImageAdjustments {
        var a = ImageAdjustments()
        for pair in spec.split(separator: ",") {
            let kv = pair.split(separator: "=")
            if kv.count == 2,
               let key = ImageAdjustments.Key(rawValue: String(kv[0])),
               let v = Double(kv[1]) { a[key] = v }
        }
        return a
    }

    /// A drag posted to the window, from one point to another (points
    /// from its top left), in eight steps.
    @MainActor
    static func postDrag(from a: (Double, Double),
                                 to b: (Double, Double),
                                 flags: NSEvent.ModifierFlags = [],
                                 release: Bool = true) async {
        guard let win = NSApp.windows.first(where: { $0.isVisible }) else {
            return
        }
        func post(_ type: NSEvent.EventType, _ x: Double, _ y: Double) {
            if let ev = NSEvent.mouseEvent(
                with: type, location: NSPoint(x: x, y: win.frame.height - y),
                modifierFlags: flags, timestamp: ProcessInfo.processInfo
                    .systemUptime,
                windowNumber: win.windowNumber, context: nil,
                eventNumber: 0, clickCount: 1, pressure: 1) {
                win.sendEvent(ev)
            }
        }
        // The pointer arrives first, as a person's would: SwiftUI places
        // its views' AppKit hit areas on the first event it sees.
        post(.mouseMoved, a.0, a.1)
        try? await Task.sleep(for: .milliseconds(100))
        post(.leftMouseDown, a.0, a.1)
        try? await Task.sleep(for: .milliseconds(60))
        for i in 1...8 {
            let t = Double(i) / 8
            post(.leftMouseDragged, a.0 + (b.0 - a.0) * t,
                 a.1 + (b.1 - a.1) * t)
            try? await Task.sleep(for: .milliseconds(30))
        }
        postedClickAt = CACurrentMediaTime()
        // Held: the drag as it is for the capture.
        if release { post(.leftMouseUp, b.0, b.1) }
    }

    /// The guides as they are: "set=h:120,240|v:213.3 byHand=true
    /// canvas=640x360 rect=0,0,800x450 marks=h0@150,v0@266 editing=h0
    /// dirty=true" -- their pixels, the stage's, where what they measure
    /// lies on the card, the marks shown (each at its point on the card's
    /// edge) and the number typed over.
    @MainActor
    static func guideSummary(_ m: AppModel) -> String {
        let g = m.stageGuides
        let set = g.map {
            "h:" + $0.h.map(GuideMarks.label).joined(separator: ",")
                + "|v:" + $0.v.map(GuideMarks.label).joined(separator: ",")
        } ?? "-"
        let card = m.stageCardFrame.size
        let rect = m.stage.video != nil ? CGRect(origin: .zero, size: card)
                                        : m.stageCanvasRect
        var marks: [String] = []
        if let g, let c = m.guideCanvas, let r = rect, m.showsGuides {
            for (i, px) in g.h.enumerated() {
                let y = r.minY + px / c.height * r.height
                if y >= -0.5, y <= card.height + 0.5 {
                    marks.append("h\(i)@\(Int(y.rounded()))")
                }
            }
            for (i, px) in g.v.enumerated() {
                let x = r.minX + px / c.width * r.width
                if x >= -0.5, x <= card.width + 0.5 {
                    marks.append("v\(i)@\(Int(x.rounded()))")
                }
            }
        }
        let c = m.guideCanvas.map { "\(Int($0.width))x\(Int($0.height))" }
            ?? "-"
        let r = rect.map {
            "\(Int($0.minX)),\(Int($0.minY)),\(Int($0.width))x\(Int($0.height))"
        } ?? "-"
        let ed = m.guideEditing.map {
            "\($0.horizontal ? "h" : "v")\($0.index)"
        } ?? "-"
        return "set=\(set) byHand=\(m.guidesSetByHand) canvas=\(c) rect=\(r) "
            + "marks=\(marks.isEmpty ? "-" : marks.joined(separator: ",")) "
            + "editing=\(ed) dirty=\(m.document.dirty)"
    }

    /// Where a guide's number is in the window (points from its top
    /// left) -- or its "−", or its triangle -- as GuideMarks lays them
    /// out.
    @MainActor
    static func guideMarkPoint(_ m: AppModel, _ ref: GuideRef,
                               part: String) -> CGPoint? {
        let minus = part == "minus"
        let f = m.stageCardFrame
        guard let g = m.stageGuides, let c = m.guideCanvas,
              let r = m.stage.video != nil
                  ? CGRect(origin: .zero, size: f.size)
                  : m.stageCanvasRect else { return nil }
        if ref.horizontal, g.h.indices.contains(ref.index) {
            let y = f.minY + r.minY + g.h[ref.index] / c.height * r.height
            if part == "triangle" { return CGPoint(x: f.maxX + 5.5, y: y) }
            return minus ? CGPoint(x: f.maxX + 5.5, y: y + 13)
                         : CGPoint(x: f.maxX + 16, y: y)
        }
        if !ref.horizontal, g.v.indices.contains(ref.index) {
            let x = f.minX + r.minX + g.v[ref.index] / c.width * r.width
            if part == "triangle" { return CGPoint(x: x, y: f.maxY + 5.5) }
            return minus ? CGPoint(x: x + 13, y: f.maxY + 5.5)
                         : CGPoint(x: x, y: f.maxY + 17)
        }
        return nil
    }

    /// The timeline as it is: "open=true rows=3 zoom=1.00 frame=48/120
    /// selected=1 layers=1:30-54:clip:k30,0:12-30:clip events=0,12,30"
    /// -- each row top first, its id ("0" layer 0), start, end, kind,
    /// keys, a waveform, hidden.
    @MainActor
    static func timelineSummary(_ m: AppModel) -> String {
        let t = m.timelineModel
        let rows = t.rows.map { r -> String in
            let id = r.id.isEmpty ? "0" : r.id
            let end = r.end.map(String.init) ?? "on"
            // Between two keys how the values go on: ">" moving, "|"
            // held to the next, "/" nothing changing.
            let keys = r.keys.isEmpty ? ""
                : ":k" + r.keys.enumerated().map { i, k in
                    let way = i + 1 == r.keys.count ? ""
                        : r.ways[k] == .moves ? ">"
                        : r.ways[k] == .jumps ? "|" : "/"
                    return "\(k)\(way)"
                }.joined()
            let wave = r.wave != nil ? ":wave" : ""
            let shown = (r.visible ? "" : ":hidden")
                + (r.locked ? ":locked" : "")
            // A folder's header: its id and its layers' spans together;
            // a layer in one set in by ">".
            if r.isFolder {
                let spans = r.union.map { "\($0.lowerBound)-\($0.upperBound)" }
                    .joined(separator: "+")
                return "[\(r.folder ?? "")\(r.folded ? ":folded" : ""):\(spans)\(shown)]"
            }
            let inset = r.depth > 0 ? ">" : ""
            return "\(inset)\(id):\(r.start)-\(end):\(r.kind)\(keys)\(wave)\(shown)"
        }.joined(separator: ",")
        let sel = t.selected.isEmpty ? "0" : t.selected
        let zoom = String(format: "%.2f", t.zoom)
        let events = t.events.map(String.init).joined(separator: ",")
        // In parts: one chain of + took the type checker past its limit.
        let layers = rows.isEmpty ? "-" : rows
        let panel = m.openPanel.map { "\($0)" } ?? "-"
        let a = "open=\(m.timelineOpen) available=\(m.timelineAvailable)"
        let b = "rows=\(m.timelineRows) zoom=\(zoom)"
        let c = "frame=\(t.playhead)/\(t.frames) cutting=\(t.cutting)"
        let d = "selected=\(sel) layers=\(layers) events=\(events)"
        let e = "panel=\(panel) assets=\(m.assets.count)"
        // The playhead off the selected layer: before or after it.
        let f = "off=\(m.layerOffStage?.rawValue ?? "-")"
        // Its transitions, from>to:kind; what ⌘ highlights.
        let tr = t.transitions.map {
            "\($0.from.isEmpty ? "0" : $0.from)>\($0.to.isEmpty ? "0" : $0.to):\($0.kind)"
        }.joined(separator: ",")
        let g = "transitions=\(tr.isEmpty ? "-" : tr) highlights=\(TimelineNSView.shown?.overlapSummary ?? "-")"
        return [a, b, c, d, e, f, g].joined(separator: " ")
    }

    /// The downloads: "z-image-turbo:running:0.42" -- each one's model,
    /// state and progress.
    /// "12.30+0.00/31.20" (GPU + ANE / the highest), or "-"; a fleet's
    /// with its members after a colon ("Studio=12.30,Mini=-").
    static func flopsSummary(_ f: MachineMonitor.Flops?) -> String {
        guard let f else { return "-" }
        var s = String(format: "%.2f+%.2f/%.2f", f.gpu, f.ane, f.peak)
        if !f.members.isEmpty {
            s += ":" + f.members.map { m in
                m.name.replacingOccurrences(of: " ", with: "_") + "="
                    + (m.gpu.map { String(format: "%.2f", $0 + (m.ane ?? 0)) }
                       ?? "-")
            }.joined(separator: ",")
        }
        return s
    }

    @MainActor
    static func downloadSummary(_ m: AppModel) -> String {
        let d = m.jobs.values.filter { $0.purpose == "download" }
        return d.isEmpty ? "-" : d.map {
            "\($0.model):\($0.state):\(String(format: "%.3f", $0.progress))"
        }.sorted().joined(separator: ",")
    }

    /// The task queue: "T0:running:generate,T1:queued:export" -- one
    /// waiting for a download "D:waiting:generate".
    @MainActor
    static func taskSummary(_ m: AppModel) -> String {
        m.tasks.isEmpty ? "-" : m.tasks.map {
            $0.waits ? "D:waiting:\($0.kind)"
                : "\($0.badge):\($0.state):\($0.kind)"
        }.joined(separator: ",")
    }

    /// The stacked inspector's open sections: "layers:1.00@312" -- each
    /// one's share and height.
    @MainActor
    static func panelSummary(_ m: AppModel) -> String {
        InspectorTab.allCases.filter { !m.inspectorFolded.contains($0) }
            .map { t in
                "\(t.rawValue):\(String(format: "%.2f", m.inspectorWeight(t)))@\(Int(m.inspectorHeights[t] ?? 0))"
            }.joined(separator: ",")
    }

    /// When the last click was posted (a snapshot run): how long a
    /// click takes to act is measured from it.
    @MainActor static var postedClickAt: CFTimeInterval = 0

    /// Characters typed into what has the window's keyboard -- "\r"
    /// Return, "\u{1b}" Escape -- as key events sent to the window.
    @MainActor
    static func sendKeys(_ m: AppModel, _ text: String) {
        guard let w = m.editorWindow else { return }
        for ch in text {
            let code: UInt16 = ch == "\r" ? 36 : ch == "\u{1b}" ? 53 : 0
            if let e = NSEvent.keyEvent(
                with: .keyDown, location: .zero, modifierFlags: [],
                timestamp: 0, windowNumber: w.windowNumber, context: nil,
                characters: String(ch),
                charactersIgnoringModifiers: String(ch),
                isARepeat: false, keyCode: code) {
                w.sendEvent(e)
            }
        }
    }

    /// Space, as the keyboard sends it, through the app's event path (its
    /// monitors included).
    @MainActor
    static func postSpace(_ m: AppModel) {
        guard let w = m.editorWindow,
              let ev = NSEvent.keyEvent(
                  with: .keyDown, location: .zero, modifierFlags: [],
                  timestamp: 0, windowNumber: w.windowNumber, context: nil,
                  characters: " ", charactersIgnoringModifiers: " ",
                  isARepeat: false, keyCode: 49) else { return }
        NSApp.postEvent(ev, atStart: false)
    }

    /// A double-click posted to the window at (x, y) points from its top
    /// left: the pointer there first, then two clicks 180 ms apart.
    @MainActor
    static func postDoubleClick(x: Double, y: Double) async {
        guard let win = NSApp.windows.first(where: { $0.isVisible }) else {
            return
        }
        let p = NSPoint(x: x, y: win.frame.height - y)
        func post(_ type: NSEvent.EventType, _ clicks: Int) {
            if let ev = NSEvent.mouseEvent(
                with: type, location: p, modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime,
                windowNumber: win.windowNumber, context: nil,
                eventNumber: 0, clickCount: clicks, pressure: 1) {
                win.sendEvent(ev)
            }
        }
        // At a hand's pace: the pointer resting first, each press held a
        // moment -- SwiftUI's taps missed the second click of a faster one
        // in a scroll view (the prompt tabs).
        post(.mouseMoved, 0)
        try? await Task.sleep(for: .milliseconds(600))
        post(.leftMouseDown, 1)
        try? await Task.sleep(for: .milliseconds(60))
        post(.leftMouseUp, 1)
        try? await Task.sleep(for: .milliseconds(180))
        post(.leftMouseDown, 2)
        try? await Task.sleep(for: .milliseconds(60))
        post(.leftMouseUp, 2)
    }

    /// The status bar's items shown, "gpu,ram,..." ("-": none).
    @MainActor
    static func statusItems(_ m: AppModel) -> String {
        let shown = StatusBarItem.allCases.filter { m.showsStatusItem($0) }
        return shown.isEmpty ? "-" : shown.map(\.rawValue)
            .joined(separator: ",")
    }

    /// A right-click posted on the status bar, at its left edge: the menu
    /// it opens read as it tracks -- "+GPU,-ANE,...,Hide_Status_Bar" --
    /// then shut, the item titled `pick` performed. "no-menu" when none
    /// opened in three seconds.
    @MainActor
    static func statusMenu(pick: String?) async -> String {
        guard let win = NSApp.windows.first(where: { $0.isVisible }) else {
            return "no-window"
        }
        // What the closures share: the observer, the menu once it opened
        // (AppKit's, used on the main thread alone), whether it is over.
        final class Watch: @unchecked Sendable {
            var token: NSObjectProtocol?
            var menu: NSMenu?
            var done = false
        }
        let watch = Watch()
        return await withCheckedContinuation { cont in
            @MainActor func finish(_ s: String) {
                guard !watch.done else { return }
                watch.done = true
                if let t = watch.token {
                    NotificationCenter.default.removeObserver(t)
                }
                cont.resume(returning: s)
            }
            watch.token = NotificationCenter.default.addObserver(
                forName: NSMenu.didBeginTrackingNotification, object: nil,
                queue: .main) { note in
                watch.menu = note.object as? NSMenu
                MainActor.assumeIsolated {
                    guard let menu = watch.menu else { return }
                    let items = menu.items.map { i in
                        i.isSeparatorItem ? "|" :
                            (i.state == .on ? "+" : i.action == nil ? ""
                                : "-") + i.title
                            .replacingOccurrences(of: " ", with: "_")
                    }.joined(separator: ",")
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                        MainActor.assumeIsolated {
                            guard let menu = watch.menu else { return }
                            menu.cancelTrackingWithoutAnimation()
                            if let pick, let i = menu.items.firstIndex(
                                where: { $0.title == pick }) {
                                menu.performActionForItem(at: i)
                            }
                            finish(items)
                        }
                    }
                }
            }
            // In the bar's left padding: the bar's own, no meter's.
            let p = NSPoint(x: 6, y: StatusBar.height / 2)
            for type in [NSEvent.EventType.rightMouseDown, .rightMouseUp] {
                if let ev = NSEvent.mouseEvent(
                    with: type, location: p, modifierFlags: [],
                    timestamp: 0, windowNumber: win.windowNumber,
                    context: nil, eventNumber: 0, clickCount: 1,
                    pressure: 1) {
                    NSApp.postEvent(ev, atStart: false)
                }
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                MainActor.assumeIsolated { finish("no-menu") }
            }
        }
    }

    /// The pointer moved to (x, y) points from the window's top left:
    /// posted to the app's event queue (its monitors see it), or handed to
    /// the window's SwiftUI itself (`toHost`), as the pointer's moves over
    /// its own views are.
    @MainActor
    private static func postMove(x: Double, y: Double, toHost: Bool) {
        guard let win = NSApp.windows.first(where: { $0.isVisible }),
              let ev = NSEvent.mouseEvent(
                  with: .mouseMoved,
                  location: NSPoint(x: x, y: win.frame.height - y),
                  modifierFlags: [],
                  timestamp: ProcessInfo.processInfo.systemUptime,
                  windowNumber: win.windowNumber, context: nil,
                  eventNumber: 0, clickCount: 0, pressure: 0)
        else { return }
        if toHost {
            win.contentView?.mouseMoved(with: ev)
        } else {
            NSApp.postEvent(ev, atStart: false)
        }
    }

    /// The AppKit view a click at (x, y) points from the window's top
    /// left goes to, then its ancestors: "AVPlayerView<AppKitPlatform...".
    @MainActor
    private static func hitChain(x: Double, y: Double) -> String {
        guard let w = NSApp.windows.first(where: { $0.isVisible }),
              let root = w.contentView?.superview else { return "-" }
        var chain: [String] = []
        var v = root.hitTest(NSPoint(x: x, y: w.frame.height - y))
        while let view = v, chain.count < 8 {
            chain.append(String(describing: type(of: view)))
            v = view.superview
        }
        return chain.joined(separator: "<")
    }

    /// The cursor by name, as far as AppKit's own are told apart.
    @MainActor
    private static func cursorName(_ c: NSCursor) -> String {
        let known: [(NSCursor, String)] = [
            (.arrow, "arrow"), (.openHand, "openHand"),
            (.closedHand, "closedHand"), (.pointingHand, "pointingHand"),
            (.iBeam, "iBeam"), (.crosshair, "crosshair"),
        ]
        return known.first { $0.0 == c }?.1 ?? "other"
    }

    /// A click -- down, then up -- posted to the window at (x, y) points
    /// from its top left, through the app's own event queue.
    @MainActor
    private static func postClick(x: Double, y: Double,
                                  in window: NSWindow? = nil) {
        guard let win = window
                ?? NSApp.windows.first(where: { $0.isVisible }) else {
            return
        }
        postedClickAt = CACurrentMediaTime()
        let p = NSPoint(x: x, y: win.frame.height - y)
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            if let ev = NSEvent.mouseEvent(
                with: type, location: p, modifierFlags: [], timestamp: 0,
                windowNumber: win.windowNumber, context: nil,
                eventNumber: 0, clickCount: 1, pressure: 1) {
                NSApp.postEvent(ev, atStart: false)
            }
        }
    }

    /// The fields focus visits from `start`, pressing Tab `count` times.
    @MainActor
    private static func tabOrder(from start: String,
                                 count: Int) async -> [String] {
        guard let win = NSApp.windows.first(where: { $0.isVisible }),
              let root = win.contentView?.superview,
              let field = fields(in: root).first(where: {
                  $0.placeholderString == start })
        else { return ["no field \(start)"] }
        win.makeKeyAndOrderFront(nil)
        win.makeFirstResponder(field)
        var out = [focusName(win.firstResponder)]
        for _ in 0..<count {
            guard let ev = NSEvent.keyEvent(
                with: .keyDown, location: .zero, modifierFlags: [],
                timestamp: 0, windowNumber: win.windowNumber, context: nil,
                characters: "\t", charactersIgnoringModifiers: "\t",
                isARepeat: false, keyCode: 48) else { break }
            win.sendEvent(ev)
            try? await Task.sleep(for: .milliseconds(150))
            out.append(focusName(win.firstResponder))
        }
        return out
    }

    @MainActor
    private static func clickStepper(field: String, up: Bool,
                                     times: Int) async {
        for win in NSApp.windows where win.isVisible {
            guard let root = win.contentView?.superview,
                  let f = fields(in: root).first(where: {
                      $0.placeholderString == field }) else { continue }
            win.makeFirstResponder(f)
            let fr = f.convert(f.bounds, to: nil)
            // The stepper beside it: the nearest one to its right, on
            // its row.
            let steppers = views(NSStepper.self, in: root).map {
                ($0, $0.convert($0.bounds, to: nil))
            }.filter { abs($0.1.midY - fr.midY) < 8 && $0.1.minX >= fr.maxX }
            guard let (_, sr) = steppers.min(by: { $0.1.minX < $1.1.minX })
            else { return }
            let pt = NSPoint(x: sr.midX,
                             y: up ? sr.maxY - sr.height / 4
                                   : sr.minY + sr.height / 4)
            func event(_ type: NSEvent.EventType) -> NSEvent? {
                NSEvent.mouseEvent(
                    with: type, location: pt, modifierFlags: [],
                    timestamp: ProcessInfo.processInfo.systemUptime,
                    windowNumber: win.windowNumber, context: nil,
                    eventNumber: 0, clickCount: 1, pressure: 1)
            }
            for _ in 0..<times {
                // The stepper tracks the press in its own loop: the
                // release waits in the queue for it.
                guard let down = event(.leftMouseDown),
                      let upEv = event(.leftMouseUp) else { break }
                NSApp.postEvent(upEv, atStart: false)
                win.sendEvent(down)
                try? await Task.sleep(for: .milliseconds(250))
            }
            let still = (win.firstResponder as? NSTextView)?.delegate === f
            FileHandle.standardError.write(Data(
                "snapshot: stepper field=\(field) stillFocused=\(still) text=\(f.stringValue)\n"
                    .utf8))
            return
        }
    }

    @MainActor
    private static func views<V: NSView>(_ type: V.Type,
                                         in v: NSView) -> [V] {
        var out: [V] = []
        if let hit = v as? V { out.append(hit) }
        for sub in v.subviews { out += views(type, in: sub) }
        return out
    }

    @MainActor
    private static func fields(in v: NSView) -> [NSTextField] {
        var out: [NSTextField] = []
        if let f = v as? NSTextField, f.isEditable { out.append(f) }
        for sub in v.subviews { out += fields(in: sub) }
        return out
    }

    @MainActor
    private static func focusName(_ r: NSResponder?) -> String {
        if let tv = r as? NSTextView, let f = tv.delegate as? NSTextField {
            return f.placeholderString ?? f.stringValue
        }
        return r.map { String(describing: type(of: $0)) } ?? "-"
    }

    /// The prompt's text view -- its scroll view -- in the window, for the
    /// hooks: "x,y,wxh" in points from the window's top left; "-" when
    /// none shows.
    @MainActor
    static func promptBoxRect() -> String {
        guard let win = NSApp.windows.first(where: { $0.isVisible }),
              let root = win.contentView?.superview,
              let tv = find(PromptTextView.self, in: root) else { return "-" }
        let v: NSView = tv.enclosingScrollView ?? tv
        let r: NSRect = v.convert(v.bounds, to: nil)
        let h: CGFloat = win.contentView?.frame.height ?? 0
        return "\(Int(r.minX)),\(Int(h - r.maxY)),\(Int(r.width))x\(Int(r.height))"
    }

    @MainActor
    static func find<V: NSView>(_ type: V.Type, in v: NSView) -> V? {
        if let hit = v as? V { return hit }
        for sub in v.subviews {
            if let hit = find(type, in: sub) { return hit }
        }
        return nil
    }

    /// Its window closed, Valtz quits -- the EDITOR's: a window closed
    /// before that one has been there never quits it. Sparkle's update
    /// permission prompt can come up first (its short delay elapses while
    /// launch is busy), and answering it closed the only window: Valtz
    /// quit at once, without a word.
    func applicationShouldTerminateAfterLastWindowClosed(
        _ sender: NSApplication
    ) -> Bool {
        MainActor.assumeIsolated { model?.editorWasShown ?? false }
    }

    /// Unsaved changes: saved, let go, or the quit cancelled. Tasks under
    /// way: asked about, and -- agreed -- stopped before Valtz quits, the
    /// window veiled meanwhile (AppModel.prepareClose): the quit waits.
    @MainActor
    func applicationShouldTerminate(
        _ sender: NSApplication
    ) -> NSApplication.TerminateReply {
        guard let m = model else { return .terminateNow }
        // Asked already, as its window closed.
        if m.closeConfirmed { return .terminateNow }
        switch m.prepareClose(.quit, then: { ok in
            if ok { m.closeForQuit() }
            NSApp.reply(toApplicationShouldTerminate: ok)
        }) {
        case .proceed:
            m.closeForQuit()
            return .terminateNow
        case .cancelled:
            return .terminateCancel
        case .waiting:
            return .terminateLater
        }
    }

    @MainActor
    func applicationWillTerminate(_ notification: Notification) {
        // Stop the engine (finishing or cancelling the running job) and
        // close the project databases before the process exits.
        model?.shutdown()
    }
}

/// A drag as Finder makes one -- a file URL on its own pasteboard -- for
/// VALTZ_SNAPSHOT_FAKE_DROP: handed straight to a drop view, since a run
/// cannot move the pointer for a real one.
@MainActor
final class FakeDrag: NSObject, @preconcurrency NSDraggingInfo {
    let pasteboard: NSPasteboard
    var location: NSPoint
    let window: NSWindow

    /// A file's drag as the Finder makes it: its FILE REFERENCE URL
    /// (file:///.file/id=...), as public.file-url and the Finder's own
    /// node type -- a path URL took drops a real drag did not.
    init(file: URL, at: NSPoint, window: NSWindow) {
        pasteboard = NSPasteboard(name: NSPasteboard.Name(
            "valtz-fake-drag-\(UUID().uuidString)"))
        pasteboard.clearContents()
        // Through Core Foundation: Swift's URL makes a reference a path.
        let s = CFURLCreateFileReferenceURL(nil, file as CFURL, nil)
            .map { CFURLGetString($0.takeRetainedValue()) as String }
            ?? file.absoluteString
        let item = NSPasteboardItem()
        item.setString(s, forType: .fileURL)
        item.setString(s, forType: NSPasteboard.PasteboardType(
            "com.apple.finder.node"))
        pasteboard.writeObjects([item])
        location = at
        self.window = window
    }

    /// A row's drag: its text, as NSItemProvider(object: NSString) has it.
    init(text: String, at: NSPoint, window: NSWindow) {
        pasteboard = NSPasteboard(name: NSPasteboard.Name(
            "valtz-fake-drag-\(UUID().uuidString)"))
        pasteboard.clearContents()
        pasteboard.writeObjects([text as NSString])
        location = at
        self.window = window
    }

    var draggingDestinationWindow: NSWindow? { window }
    var draggingSourceOperationMask: NSDragOperation { [.copy, .link, .generic] }
    var draggingLocation: NSPoint { location }
    var draggedImageLocation: NSPoint { location }
    var draggedImage: NSImage? { nil }
    var draggingPasteboard: NSPasteboard { pasteboard }
    var draggingSource: Any? { nil }
    var draggingSequenceNumber: Int { 4242 }
    func slideDraggedImage(to screenPoint: NSPoint) {}
    var draggingFormation: NSDraggingFormation = .default
    var animatesToDestination = false
    var numberOfValidItemsForDrop = 1
    func enumerateDraggingItems(
        options enumOpts: NSDraggingItemEnumerationOptions = [],
        for view: NSView?,
        classes classArray: [AnyClass],
        searchOptions: [NSPasteboard.ReadingOptionKey: Any] = [:],
        using block: (NSDraggingItem, Int, UnsafeMutablePointer<ObjCBool>)
            -> Void
    ) {
        let objects = pasteboard.readObjects(forClasses: classArray,
                                             options: searchOptions) ?? []
        var stop: ObjCBool = false
        for (i, o) in objects.enumerated() {
            guard let w = o as? NSPasteboardWriting else { continue }
            block(NSDraggingItem(pasteboardWriter: w), i, &stop)
            if stop.boolValue { break }
        }
    }
    var springLoadingHighlight: NSSpringLoadingHighlight { .none }
    func resetSpringLoading() {}
}
