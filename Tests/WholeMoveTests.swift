import Foundation
import simd

@main
struct WholeMoveTests {
    static func expect(_ value: Bool, _ message: String) { precondition(value, message) }
    static func angleDistance(_ a: Float, _ b: Float) -> Float { abs(atan2(sin(a-b), cos(a-b))) }

    static func main() {
        let c = Cloth(width: 400, height: 280, columns: 12)
        let cam = Camera(size: SIMD2(1512, 982))
        var editor = WholeRugEditor()
        c.layout(center: cam.center, angle: 0.31, drop: 0)
        // Compress and wrinkle all three axes before selecting.
        for i in 0..<c.count {
            c.pos[i].x = cam.center.x + (c.pos[i].x - cam.center.x) * 0.6
            c.pos[i].y = cam.center.y + (c.pos[i].y - cam.center.y) * 0.7
            c.pos[i].z = Float(i % 7) * 3
        }
        c.updateDerived(cam)
        let oldPose = c.pose()
        expect(editor.mouseDown(cam.center, cloth: c, camera: cam), "Selection missed the rug")
        expect(c.isMovingWhole && !c.isGrabbing, "Selection failed")
        expect(simd_distance(c.pose().center, oldPose.center) < 0.001, "Unfolding moved center")
        expect(angleDistance(c.pose().angle, oldPose.angle) < 0.001, "Unfolding changed orientation")
        for i in 0..<c.count {
            expect(abs(c.pos[i].z - 120) < 0.001, "Wrinkle remained")
            expect(c.vel[i] == .zero && c.prev[i] == c.pos[i], "Stale velocity")
        }
        expect(abs(simd_distance(c.pos[0], c.pos[c.nx-1]) - c.width) < 0.001, "Width not fully unfolded")
        expect(abs(simd_distance(c.pos[0], c.pos[c.idx(0, c.ny-1)]) - c.height) < 0.001, "Height not fully unfolded")
        editor.mouseUp()
        expect(c.isMovingWhole, "Mouse-up deselected rug")
        let held = (0..<c.count).map { c.pos[$0] }
        for _ in 0..<30 { c.step(dt: 1/60) }
        editor.mouseDragged(cam.center + SIMD2(60, 80), cloth: c, camera: cam)
        expect((0..<c.count).allSatisfy { c.pos[$0] == held[$0] }, "Idle selection moved")

        let geometry = editor.geometry(cloth: c, camera: cam)!
        let ids = RugEditGeometry.cornerIDs(c)
        let anchor = c.wholeMoveCenter!
        let diagonal = c.pos[ids[0]] - anchor
        expect(editor.handle(at: geometry.center, cloth: c, camera: cam) == nil, "Center must stay free for moving")
        expect(editor.handle(at: geometry.corners[0] + SIMD2(10, 0), cloth: c, camera: cam) == .corner(0), "Corner target too small")
        expect(editor.mouseDown(geometry.corners[0], cloth: c, camera: cam), "Corner missed")
        expect(editor.isTransforming, "Corner gesture not active")
        let beforeRotation = c.pose().angle
        // Complete turns in both directions, crossing atan2's discontinuity.
        for direction: Float in [1, -1] {
            for step in 1...96 {
                let angle = direction * Float(step) * .pi / 16
                editor.mouseDragged(c.presentationCamera(cam).project(anchor + rotate(diagonal, by: angle)), cloth: c, camera: cam)
                expect(angleDistance(c.pose().angle, beforeRotation + angle) < 0.0001, "Corner rotation jumped or clamped")
                expect(simd_distance((c.pos[ids[0]] + c.pos[ids[2]]) / 2, anchor) < 0.002, "Rotation moved center")
                expect(abs(c.width - 400) < 0.002, "Circular drag changed size")
            }
        }
        expect((0..<c.count).allSatisfy { simd_distance(c.pos[$0], held[$0]) < 0.003 }, "Full turns stretched rug")
        let beforeDeadZone = (0..<c.count).map { c.pos[$0] }
        for offset in [SIMD2<Float>.zero, SIMD2<Float>(1, -1)] {
            editor.mouseDragged(c.presentationCamera(cam).project(anchor) + offset, cloth: c, camera: cam)
            expect((0..<c.count).allSatisfy { c.pos[$0] == beforeDeadZone[$0] }, "Center dead zone jumped")
        }
        editor.mouseDragged(c.presentationCamera(cam).project(anchor + rotate(diagonal, by: -.pi / 2)), cloth: c, camera: cam)
        expect(angleDistance(c.pose().angle, beforeRotation - .pi / 2) < 0.0001, "Rotation did not resume after dead zone")
        editor.mouseUp()
        expect(c.isMovingWhole && !editor.isTransforming, "Corner mouse-up lost selection or kept dragging")

        // Regrab after rotating; a movement must not overwrite the new angle.
        let centerScreen = c.presentationCamera(cam).project(c.wholeMoveCenter!)
        let pickup = centerScreen + SIMD2<Float>(8, -8)
        expect(editor.mouseDown(pickup, cloth: c, camera: cam), "Regrab failed")
        let rotated = c.pose().angle
        let targetScreen = pickup + SIMD2<Float>(180, -95)
        editor.mouseDragged(targetScreen, cloth: c, camera: cam)
        expect(simd_distance(c.presentationCamera(cam).project(c.wholeMoveCenter!), centerScreen + SIMD2(180, -95)) < 0.001, "Cursor anchor drifted")
        expect(angleDistance(c.pose().angle, rotated) < 0.0001, "Moving reset rotation")
        editor.mouseDragged(pickup, cloth: c, camera: cam)
        expect(simd_distance(c.presentationCamera(cam).project(c.wholeMoveCenter!), centerScreen) < 0.001, "Return path drifted")
        editor.mouseUp()
        editor.finish(cloth: c)
        expect(!c.isMovingWhole, "Option release failed")
        let releaseZ = c.pos[0].z
        c.step(dt: 1/60)
        expect(c.pos[0].z < releaseZ, "Gravity did not resume")
        expect((0..<c.count).allSatisfy { c.pos[$0].x.isFinite && c.pos[$0].z.isFinite }, "Unstable release")
        c.beginGrab(vertex: c.count/2, target: c.pos[c.count/2] + SIMD3(0,0,100))
        expect(c.isGrabbing && !c.isMovingWhole, "Ordinary grab broken")
        c.beginWholeMove(height: 120)
        expect((0..<c.count).allSatisfy { c.invMass[$0] == 1 }, "Old corner pins stayed locked")
        c.layout(center: cam.center, angle: 0, drop: 0)
        expect(!c.isMovingWhole && !c.isGrabbing, "Layout did not cancel interaction")
        transformTests(camera: cam)
        projectionTests()
        print("PASS: unfold, latched selection, held stability, multi-turn/reverse rotation, dead zone, movement after rotation, release gravity, ordinary grab, cleanup, combined corner scaling and rotation, proportional resizing, fixed center, bounds, post-transform physics, true-size preview and continuous landing")
    }

    static func rotate(_ vector: SIMD3<Float>, by angle: Float) -> SIMD3<Float> {
        SIMD3(vector.x * cos(angle) - vector.y * sin(angle),
              vector.x * sin(angle) + vector.y * cos(angle), vector.z)
    }

    static func transformTests(camera cam: Camera) {
        for angle: Float in [0, 0.7, 1.57, -2.7] {
            for corner in 0..<4 {
                let c = Cloth(width: 600, height: 400, columns: 18)
                c.layout(center: cam.center, angle: angle, drop: 0)
                c.beginWholeMove(height: 120)
                c.updateDerived(cam)
                var editor = WholeRugEditor()
                let ids = RugEditGeometry.cornerIDs(c)
                let anchor = c.wholeMoveCenter!
                let diagonal = c.pos[ids[corner]] - anchor
                let ratio = c.height / c.width
                let g = editor.geometry(cloth: c, camera: cam)!
                // Start near, rather than exactly on, the tiny visible control point.
                let offset = SIMD2<Float>(5, -4)
                expect(editor.mouseDown(g.corners[corner] + offset, cloth: c, camera: cam), "Corner hit area too small")
                for factor: Float in [0.7, 1.4, 0.9, 1] {
                    editor.mouseDragged(c.presentationCamera(cam).project(anchor + diagonal * factor) + offset, cloth: c, camera: cam)
                    expect(abs(c.width - 600 * factor) < 0.002, "Corner resizing has wrong scale")
                    expect(abs(c.height / c.width - ratio) < 0.00001, "Aspect ratio changed")
                    expect(simd_distance((c.pos[ids[0]] + c.pos[ids[2]]) / 2, anchor) < 0.002, "Resize moved center")
                    expect(angleDistance(c.pose().angle, angle) < 0.0001, "Resize changed rotation")
                }
                // Simultaneous scale and angle changes must keep the grabbed
                // corner under the cursor (including a near-handle pickup offset).
                for (factor, delta): (Float, Float) in [(0.7, 0.4), (1.4, -1.2), (0.9, 2.8), (1, .pi)] {
                    let target = anchor + rotate(diagonal * factor, by: delta)
                    editor.mouseDragged(c.presentationCamera(cam).project(target) + offset, cloth: c, camera: cam)
                    expect(abs(c.width - 600 * factor) < 0.002, "Combined gesture has wrong size")
                    expect(angleDistance(c.pose().angle, angle + delta) < 0.0001, "Combined gesture has wrong angle")
                    expect(abs(c.height / c.width - ratio) < 0.00001, "Combined gesture distorted rug")
                    expect(simd_distance((c.pos[ids[0]] + c.pos[ids[2]]) / 2, anchor) < 0.002, "Combined gesture moved center")
                    expect(simd_distance(c.presentationCamera(cam).project(c.pos[ids[corner]]) + offset, c.presentationCamera(cam).project(target) + offset) < 0.003, "Grabbed corner drifted from cursor")
                    expect(editor.readout(cloth: c, camera: cam)!.text.contains("°"), "Combined readout missing angle")
                }
                for (factor, bound): (Float, Float) in [(0.1, WholeRugEditor.sizeRange.lowerBound), (10, WholeRugEditor.sizeRange.upperBound)] {
                    for delta: Float in [0.8, -2, .pi] {
                        editor.mouseDragged(c.presentationCamera(cam).project(anchor + rotate(diagonal * factor, by: delta)) + offset, cloth: c, camera: cam)
                        expect(abs(c.width - cam.size.x * bound) < 0.002, "Size limit ignored")
                        expect(angleDistance(c.pose().angle, angle + delta) < 0.0001, "Size limit blocked rotation")
                        expect(simd_distance((c.pos[ids[0]] + c.pos[ids[2]]) / 2, anchor) < 0.002, "Size limit moved center")
                    }
                }
                editor.mouseDragged(c.presentationCamera(cam).project(anchor + rotate(diagonal * 0.7, by: 0.6)) + offset, cloth: c, camera: cam)
                editor.mouseUp()
                expect(editor.readout(cloth: c, camera: cam) == nil, "Readout stayed after mouse-up")
                expect(c.isMovingWhole, "Resize mouse-up deselected rug")
                editor.finish(cloth: c)
                let width = c.width
                // With no ground contact, uniform gravity should not undo the size.
                for _ in 0..<3 { c.step(dt: 1/60) }
                expect(abs(simd_distance(c.pos[0], c.pos[c.nx - 1]) - width) < 0.05, "Released rug sprung back to old size")
                expect((0..<c.count).allSatisfy { c.pos[$0].x.isFinite && c.pos[$0].y.isFinite && c.pos[$0].z.isFinite }, "Resized physics became unstable")
                c.beginWholeMove(height: 120)
                expect(abs(c.width - width) < 0.001, "Reselecting lost size")
            }
        }
    }

    static func projectionTests() {
        for size in [SIMD2<Float>(1512, 982), SIMD2<Float>(982, 1512)] {
            let cam = Camera(size: size)
            for center in [cam.center, size * SIMD2<Float>(0.65, 0.4)] {
                let c = Cloth(width: size.x * 0.75, height: size.x * 0.5, columns: 18)
                c.layout(center: center, angle: 0.37, drop: 0)
                c.updateDerived(cam)
                let ground = (0..<c.count).map { c.proj[$0] }
                let width = c.width
                c.beginWholeMove(height: 120)
                c.updateDerived(cam)
                for i in 0..<c.count {
                    expect(simd_distance(c.proj[i], ground[i]) < 0.002, "Lifting changed the visible footprint")
                    expect(c.pos[i].z == 120, "True-size preview removed physical lift")
                }
                let g = RugEditGeometry(cloth: c, camera: cam)
                for (corner, id) in RugEditGeometry.cornerIDs(c).enumerated() {
                    expect(simd_distance(g.corners[corner], c.proj[id]) < 0.001, "Overlay and visible rug disagree")
                    expect(c.pick(g.corners[corner] * 0.99 + g.center * 0.01, needVertex: false) != nil, "True-size preview hit test missed")
                }
                c.endWholeMove()
                c.updateDerived(cam)
                expect((0..<c.count).allSatisfy { simd_distance(c.proj[$0], ground[$0]) < 0.002 }, "Release caused a perspective jump")
                var previousZ = c.pos[0].z
                for _ in 0..<60 {
                    c.step(dt: 1/60)
                    c.updateDerived(cam)
                    expect(c.pos[0].z <= previousZ + 0.001, "Drop did not descend")
                    previousZ = c.pos[0].z
                    expect((0..<c.count).allSatisfy { simd_distance(c.proj[$0], ground[$0]) < 0.05 }, "Landing changed visible size or position")
                }
                expect(c.pos[0].z < 0.001 && c.projectionLift == 0, "Drop never reached the floor or restored perspective")
                expect(c.width == width, "Preview compensation changed saved size")
                c.beginGrab(vertex: c.count / 2, target: c.pos[c.count / 2] + SIMD3(0, 0, 100))
                let raised = SIMD3(center.x + 100, center.y, Float(100))
                expect(c.presentationCamera(cam).project(raised) == cam.project(raised), "Ordinary grab lost normal perspective")
                c.beginWholeMove(height: 120)
                c.layout(center: center, angle: 0, drop: 90)
                expect(c.projectionLift == 0, "Redrop retained editing projection")
            }
        }
    }

}
