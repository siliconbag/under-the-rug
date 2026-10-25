import AppKit
import simd

final class RugEditOverlay: NSView {
    var geometry: RugEditGeometry?
    var readout: (text: String, point: SIMD2<Float>)?

    override var isOpaque: Bool { false }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    private func point(_ p: SIMD2<Float>) -> NSPoint { NSPoint(x: CGFloat(p.x), y: CGFloat(p.y)) }

    override func draw(_ dirtyRect: NSRect) {
        guard let g = geometry else { return }
        let outline = NSBezierPath()
        outline.move(to: point(g.corners[0]))
        for p in g.corners.dropFirst() { outline.line(to: point(p)) }
        outline.close()
        NSColor.black.withAlphaComponent(0.24).setStroke()
        outline.lineWidth = 2.5
        outline.stroke()
        NSColor.white.withAlphaComponent(0.94).setStroke()
        outline.lineWidth = 1
        outline.stroke()

        for p in g.corners {
            let c = point(p)
            let dot = NSBezierPath(roundedRect: NSRect(x: c.x - 3.5, y: c.y - 3.5, width: 7, height: 7), xRadius: 1.5, yRadius: 1.5)
            NSColor.white.setFill()
            dot.fill()
            NSColor.black.withAlphaComponent(0.32).setStroke()
            dot.lineWidth = 0.8
            dot.stroke()
        }

        // Numeric feedback only while resizing/rotating; no permanent instruction bar.
        if let readout {
            let text = readout.text as NSString
            let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .medium),
                                                      .foregroundColor: NSColor.darkGray]
            let size = text.size(withAttributes: attrs)
            let c = point(readout.point)
            let x = max(8, min(c.x - size.width / 2, bounds.width - size.width - 8))
            let y = max(8, min(c.y - size.height / 2, bounds.height - size.height - 8))
            let plate = NSBezierPath(roundedRect: NSRect(x: x - 5, y: y - 3, width: size.width + 10, height: size.height + 6), xRadius: 5, yRadius: 5)
            NSColor.white.withAlphaComponent(0.94).setFill()
            plate.fill()
            text.draw(at: NSPoint(x: x, y: y), withAttributes: attrs)
        }
    }
}
