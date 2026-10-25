import Foundation
import simd

struct RugEditGeometry {
    let corners: [SIMD2<Float>] // bottom-left, bottom-right, top-right, top-left
    let center: SIMD2<Float>

    static func cornerIDs(_ cloth: Cloth) -> [Int] {
        [cloth.idx(0, 0), cloth.idx(cloth.nx - 1, 0),
         cloth.idx(cloth.nx - 1, cloth.ny - 1), cloth.idx(0, cloth.ny - 1)]
    }

    init(cloth: Cloth, camera: Camera) {
        let camera = cloth.presentationCamera(camera)
        corners = Self.cornerIDs(cloth).map { camera.project(cloth.pos[$0]) }
        center = camera.project(cloth.wholeMoveCenter!)
    }
}

// Mouse-up ends a drag, but only releasing Option ends the editing session.
struct WholeRugEditor {
    static let sizeRange: ClosedRange<Float> = 0.25...0.75
    enum Handle: Equatable { case corner(Int) }
    private enum Drag {
        case move(offset: SIMD2<Float>)
        case transform(corner: Int, pivot: SIMD3<Float>, radius: SIMD2<Float>, width: Float, angle: Float, offset: SIMD2<Float>)
    }
    private var drag: Drag?

    var isTransforming: Bool {
        if case .transform = drag { return true }
        return false
    }

    func geometry(cloth: Cloth, camera: Camera) -> RugEditGeometry? {
        guard cloth.isMovingWhole else { return nil }
        return RugEditGeometry(cloth: cloth, camera: camera)
    }

    func handle(at point: SIMD2<Float>, cloth: Cloth, camera: Camera) -> Handle? {
        guard let g = geometry(cloth: cloth, camera: camera) else { return nil }
        if let i = g.corners.indices.min(by: { simd_distance(point, g.corners[$0]) < simd_distance(point, g.corners[$1]) }),
           simd_distance(point, g.corners[i]) <= 12 { return .corner(i) }
        return nil
    }

    func readout(cloth: Cloth, camera: Camera) -> (text: String, point: SIMD2<Float>)? {
        guard let g = geometry(cloth: cloth, camera: camera) else { return nil }
        switch drag {
        case .transform(let corner, _, _, _, _, _):
            let degrees = (Int((cloth.pose().angle * 180 / .pi).rounded()) % 360 + 360) % 360
            return ("\(Int((cloth.width / camera.size.x * 100).rounded()))% · \(degrees)°",
                    g.corners[corner] + simd_normalize(g.corners[corner] - g.center) * 26)
        default: return nil
        }
    }

    @discardableResult
    mutating func mouseDown(_ point: SIMD2<Float>, cloth: Cloth, camera: Camera) -> Bool {
        let selectedHandle = handle(at: point, cloth: cloth, camera: camera)
        guard selectedHandle != nil || cloth.pick(point, needVertex: false) != nil else { return false }
        if !cloth.isMovingWhole {
            cloth.beginWholeMove(height: 120)
            cloth.updateDerived(camera)
        }
        guard let center = cloth.wholeMoveCenter else { return false }
        let p = cloth.presentationCamera(camera).unproject(point, z: center.z)
        switch selectedHandle {
        case .corner(let i):
            let ids = RugEditGeometry.cornerIDs(cloth)
            let corner = cloth.pos[ids[i]]
            drag = .transform(corner: i, pivot: center, radius: SIMD2(corner.x - center.x, corner.y - center.y),
                              width: cloth.width, angle: cloth.pose().angle,
                              offset: SIMD2(p.x - corner.x, p.y - corner.y))
        case nil:
            drag = .move(offset: SIMD2(center.x - p.x, center.y - p.y))
        }
        return true
    }

    mutating func mouseDragged(_ point: SIMD2<Float>, cloth: Cloth, camera: Camera) {
        guard let center = cloth.wholeMoveCenter, let drag else { return }
        let camera = cloth.presentationCamera(camera)
        switch drag {
        case .move(let offset):
            let p = camera.unproject(point, z: center.z)
            cloth.moveWhole(to: p + SIMD3(offset.x, offset.y, 0))
        case .transform(_, let pivot, let radius, let width, let angle, let offset):
            let p = camera.unproject(point, z: center.z)
            let d = SIMD2(p.x - pivot.x, p.y - pivot.y) - offset
            let length = simd_length(d)
            // Direction is ambiguous right at the center. Keep the last
            // valid pose inside a small screen-space dead zone.
            guard length / ((camera.D - pivot.z + camera.projectionLift) / camera.D) >= 16 else { return }
            let factor = length / simd_length(radius)
            let newWidth = min(max(width * factor, camera.size.x * Self.sizeRange.lowerBound),
                               camera.size.x * Self.sizeRange.upperBound)
            let delta = atan2(radius.x * d.y - radius.y * d.x, simd_dot(radius, d))
            // Solve from the pickup pose, avoiding accumulated scale/angle error.
            // Even at a size limit the corner can still turn around the center.
            cloth.transformWhole(width: newWidth, center: pivot, angle: angle + delta)
        }
        cloth.updateDerived(camera)
    }

    mutating func mouseUp() { drag = nil }

    mutating func finish(cloth: Cloth) {
        drag = nil
        cloth.endWholeMove()
    }
}
