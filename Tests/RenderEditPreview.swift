import AppKit
import MetalKit

// Offscreen visual check using the real renderer and overlay; no desktop window,
// Finder access, preferences, or user images are involved.
@main
struct RenderEditPreview {
    static func main() throws {
        _ = NSApplication.shared
        let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        let cam = Camera(size: SIMD2(1000, 700))
        let renderer = try Renderer(device: MTLCreateSystemDefaultDevice()!, colorFormat: .bgra8Unorm,
                                    depthFormat: .depth32Float, sampleCount: 4,
                                    textureURL: root.appendingPathComponent("Resources/rugs/1 persian.jpg"))
        let floor = try Renderer.loadTexture(device: renderer.device, url: root.appendingPathComponent("Resources/floors/3 oak-planks.jpg"))
        verifyLandingPreview(renderer: renderer, camera: cam)
        let cloth = Cloth(width: 540, height: 360, columns: 44)
        cloth.layout(center: cam.center, angle: 0.12, drop: 0)
        cloth.beginWholeMove(height: 120)
        cloth.updateDerived(cam)
        renderer.setMesh(cloth)
        let editor = WholeRugEditor()
        let rect = NSRect(x: 0, y: 0, width: 1000, height: 700)
        let overlay = RugEditOverlay(frame: rect)
        overlay.geometry = editor.geometry(cloth: cloth, camera: cam)
        guard let frame = renderer.snapshot(cloth: cloth, camera: cam, scale: 1, background: floor) else {
            fatalError("Metal snapshot failed")
        }
        let image = NSImage(size: rect.size)
        image.lockFocus()
        NSImage(cgImage: frame, size: rect.size).draw(in: rect)
        overlay.draw(rect)
        image.unlockFocus()
        let png = NSBitmapImageRep(data: image.tiffRepresentation!)!.representation(using: .png, properties: [:])!
        let output = root.appendingPathComponent("build/tests/edit-preview.png")
        try png.write(to: output)
        print(output.path)
    }

    // Check actual Metal pixels, independently of the CPU projection math.
    static func verifyLandingPreview(renderer: Renderer, camera: Camera) {
        let c = Cloth(width: 620, height: 340, columns: 18)
        c.layout(center: SIMD2(580, 350), angle: 0.15, drop: 0)
        c.updateDerived(camera)
        renderer.setMesh(c)
        func frame() -> CGImage {
            renderer.snapshot(cloth: c, camera: camera, scale: 1, background: nil)!
        }
        func opaqueBounds(_ frame: CGImage) -> [Int] {
            let data = frame.dataProvider!.data!
            let bytes = CFDataGetBytePtr(data)!
            var left = frame.width, top = frame.height, right = 0, bottom = 0
            for y in 0..<frame.height {
                for x in 0..<frame.width where bytes[y * frame.bytesPerRow + x * 4 + 3] == 255 {
                    left = min(left, x); right = max(right, x)
                    top = min(top, y); bottom = max(bottom, y)
                }
            }
            precondition(left < right && top < bottom, "Metal rendered no rug")
            return [left, top, right, bottom]
        }
        let landed = frame()
        let bounds = opaqueBounds(landed)
        func sameFootprint(_ image: CGImage) {
            precondition(zip(bounds, opaqueBounds(image)).allSatisfy { abs($0 - $1) <= 1 },
                         "GPU preview does not match the landing footprint")
        }
        c.beginWholeMove(height: 120)
        c.updateDerived(camera)
        let lifted = frame()
        sameFootprint(lifted)
        precondition((landed.dataProvider!.data! as Data) != (lifted.dataProvider!.data! as Data),
                     "Lifting lost its visible shadow")
        c.endWholeMove()
        c.updateDerived(camera)
        precondition((lifted.dataProvider!.data! as Data) == (frame().dataProvider!.data! as Data),
                     "Release frame jumped")
        for _ in 0..<3 { c.step(dt: 1/60) }
        c.updateDerived(camera)
        sameFootprint(frame())
        precondition(c.pos[0].z > 0 && c.pos[0].z < 120, "Physical fall was removed")
        for _ in 0..<57 { c.step(dt: 1/60) }
        c.updateDerived(camera)
        sameFootprint(frame())
        precondition(c.projectionLift == 0, "Normal perspective did not resume")
        print("PASS: Metal pixel footprint matches before lift, during preview, on release and after landing; lift shadow and physical fall preserved")
    }
}
