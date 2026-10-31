import AppKit
import SwiftUI

/// Valtz's type: SF Pro, through the system's text styles, so every view
/// takes the Mac's own sizes (body 13, subheadline 11, footnote 10,
/// caption 10) and the same role reads the same everywhere. A literal
/// size is for what scales with a zoom (the Atlas), the mark, and AppKit
/// canvases that draw their own text; nothing else.
extension Font {
    /// A section's or a card's title.
    static let vzHeading = Font.headline
    /// Running text: a prompt's words in a list, a note.
    static let vzBody = Font.body
    /// A control's own label where body is too large: a chip, a tab.
    static let vzCallout = Font.callout
    /// A row's or a field's label.
    static let vzLabel = Font.subheadline
    /// A row's second line: when, how big, how long.
    static let vzMeta = Font.footnote
    /// A figure beside a meter, a key's hint.
    static let vzCaption = Font.caption
}

extension NSFont {
    static var vzHeading: NSFont { .preferredFont(forTextStyle: .headline) }
    static var vzBody: NSFont { .preferredFont(forTextStyle: .body) }
    static var vzLabel: NSFont { .preferredFont(forTextStyle: .subheadline) }
    static var vzMeta: NSFont { .preferredFont(forTextStyle: .footnote) }
    static var vzCaption: NSFont { .preferredFont(forTextStyle: .caption1) }
}
