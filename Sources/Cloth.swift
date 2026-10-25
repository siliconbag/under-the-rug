import Foundation
import simd

// Ткань ковра. XPBD с подшагами (как в Ten Minute Physics), пол с кулоновым трением,
// бугры от иконок через карту высот, самоколлизии через пространственный хеш.
// Координаты в пунктах экрана, x вправо, y вверх, z к зрителю.

struct Con {
    var a: Int32
    var b: Int32
    var rest: Float
    var compliance: Float
}

struct ClothParams {
    var gravity: Float = 5000
    var substeps = 10
    var shearCompliance: Float = 2e-7
    var bendCompliance: Float = 3e-7
    var farBendCompliance: Float = -1   // изгиб через 4 узла, отрицательное значение отключает
    var airDamping: Float = 1.3
    var groundStatic: Float = 2
    var groundKinetic: Float = 1.3
    var selfFriction: Float = 0.3
    var columns = 44

    // для подбора на демо: RUG_G, RUG_BEND, RUG_FAR, RUG_DAMP, RUG_FS, RUG_FK, RUG_COLS
    static var tuned: ClothParams {
        var p = ClothParams()
        let e = ProcessInfo.processInfo.environment
        func f(_ k: String) -> Float? { e[k].flatMap(Float.init) }
        if let v = f("RUG_G") { p.gravity = v }
        if let v = f("RUG_BEND") { p.bendCompliance = v }
        if let v = f("RUG_FAR") { p.farBendCompliance = v }
        if let v = f("RUG_DAMP") { p.airDamping = v }
        if let v = f("RUG_FS") { p.groundStatic = v }
        if let v = f("RUG_FK") { p.groundKinetic = v }
        if let v = e["RUG_COLS"].flatMap(Int.init) { p.columns = v }
        return p
    }
}

struct Camera {
    var size: SIMD2<Float>
    var projectionLift: Float = 0
    var D: Float { max(size.x, size.y) * 1.15 }
    var center: SIMD2<Float> { size * 0.5 }

    func project(_ p: SIMD3<Float>) -> SIMD2<Float> {
        let w = (D - p.z + projectionLift) / D
        return center + (SIMD2(p.x, p.y) - center) / w
    }

    func unproject(_ s: SIMD2<Float>, z: Float) -> SIMD3<Float> {
        let w = (D - z + projectionLift) / D
        let xy = center + (s - center) * w
        return SIMD3(xy.x, xy.y, z)
    }
}

final class Cloth {
    let nx: Int, ny: Int, count: Int
    private(set) var width: Float, height: Float, spacing: Float, thickness: Float
    var params = ClothParams.tuned

    let pos: UnsafeMutablePointer<SIMD3<Float>>
    let prev: UnsafeMutablePointer<SIMD3<Float>>
    let vel: UnsafeMutablePointer<SIMD3<Float>>
    let rest: UnsafeMutablePointer<SIMD3<Float>>
    let nrm: UnsafeMutablePointer<SIMD3<Float>>
    let proj: UnsafeMutablePointer<SIMD2<Float>>
    let invMass: UnsafeMutablePointer<Float>
    private var cons: [Con] = []
    private(set) var indices: [UInt32] = []
    private(set) var uvs: [SIMD2<Float>] = []
    private var hash: SpatialHash
    var ground: HeightField?

    var bboxMin = SIMD2<Float>(repeating: 0)
    var bboxMax = SIMD2<Float>(repeating: 0)
    private(set) var maxSpeed: Float = 0

    private var grabbedIds: [Int] = []
    private var grabOffsets: [SIMD3<Float>] = []
    private var grabPos = SIMD3<Float>(repeating: 0)
    private var tetherRest: [Float] = []
    var grabTarget = SIMD3<Float>(repeating: 0)
    var isGrabbing: Bool { !grabbedIds.isEmpty }
    private(set) var wholeMoveCenter: SIMD3<Float>?
    private var wholeMoveAngle: Float = 0
    // Remove only the common editing lift from perspective, leaving physical
    // height intact for gravity, collision, depth and shadows.
    private(set) var projectionLift: Float = 0
    var isMovingWhole: Bool { wholeMoveCenter != nil }

    func presentationCamera(_ camera: Camera) -> Camera {
        var result = camera
        result.projectionLift = projectionLift
        return result
    }

    init(width: Float, height: Float, columns: Int) {
        nx = columns
        spacing = width / Float(columns - 1)
        ny = max(3, Int((height / spacing).rounded()) + 1)
        count = nx * ny
        self.width = width
        self.height = spacing * Float(ny - 1)
        thickness = spacing * 1.1

        pos = .allocate(capacity: count)
        prev = .allocate(capacity: count)
        vel = .allocate(capacity: count)
        rest = .allocate(capacity: count)
        nrm = .allocate(capacity: count)
        proj = .allocate(capacity: count)
        invMass = .allocate(capacity: count)
        hash = SpatialHash(spacing: thickness, maxObjects: count)

        for j in 0..<ny {
            for i in 0..<nx {
                let k = idx(i, j)
                rest[k] = SIMD3(Float(i) * spacing - self.width / 2, Float(j) * spacing - self.height / 2, 0)
                pos[k] = rest[k]
                prev[k] = rest[k]
                vel[k] = .zero
                nrm[k] = SIMD3(0, 0, 1)
                invMass[k] = 1
                uvs.append(SIMD2(Float(i) / Float(nx - 1), 1 - Float(j) / Float(ny - 1)))
            }
        }
        for j in 0..<(ny - 1) {
            for i in 0..<(nx - 1) {
                let a = UInt32(idx(i, j)), b = UInt32(idx(i + 1, j))
                let c = UInt32(idx(i, j + 1)), d = UInt32(idx(i + 1, j + 1))
                if (i + j) % 2 == 0 {
                    indices += [a, b, d, a, d, c]
                } else {
                    indices += [a, b, c, b, d, c]
                }
            }
        }
        let p = params
        for j in 0..<ny {
            for i in 0..<nx {
                if i + 1 < nx { addCon(idx(i, j), idx(i + 1, j), 0) }
                if j + 1 < ny { addCon(idx(i, j), idx(i, j + 1), 0) }
                if i + 1 < nx && j + 1 < ny {
                    addCon(idx(i, j), idx(i + 1, j + 1), p.shearCompliance)
                    addCon(idx(i + 1, j), idx(i, j + 1), p.shearCompliance)
                }
                if i + 2 < nx { addCon(idx(i, j), idx(i + 2, j), p.bendCompliance) }
                if j + 2 < ny { addCon(idx(i, j), idx(i, j + 2), p.bendCompliance) }
                if p.farBendCompliance >= 0 {
                    if i + 4 < nx { addCon(idx(i, j), idx(i + 4, j), p.farBendCompliance) }
                    if j + 4 < ny { addCon(idx(i, j), idx(i, j + 4), p.farBendCompliance) }
                }
            }
        }
    }

    deinit {
        pos.deallocate(); prev.deallocate(); vel.deallocate(); rest.deallocate()
        nrm.deallocate(); proj.deallocate(); invMass.deallocate()
    }

    @inline(__always) func idx(_ i: Int, _ j: Int) -> Int { j * nx + i }

    private func addCon(_ a: Int, _ b: Int, _ c: Float) {
        cons.append(Con(a: Int32(a), b: Int32(b), rest: simd_length(rest[a] - rest[b]), compliance: c))
    }

    // MARK: раскладка

    func layout(center: SIMD2<Float>, angle: Float, drop: Float) {
        endWholeMove()
        endGrab()
        projectionLift = 0
        let ca = cos(angle), sa = sin(angle)
        for j in 0..<ny {
            for i in 0..<nx {
                let k = idx(i, j)
                let r = rest[k]
                let u = Float(i) / Float(nx - 1)
                let z = drop > 0 ? drop * (0.55 + 0.45 * u) + Float.random(in: -1.5...1.5) : 0
                pos[k] = SIMD3(center.x + r.x * ca - r.y * sa, center.y + r.x * sa + r.y * ca, z)
                prev[k] = pos[k]
                vel[k] = .zero
            }
        }
        maxSpeed = 1000
    }

    func pose() -> (center: SIMD2<Float>, angle: Float) {
        var c = SIMD2<Float>(repeating: 0)
        for i in 0..<count { c += SIMD2(pos[i].x, pos[i].y) }
        c /= Float(count)
        let j = ny / 2
        let a = pos[idx(0, j)], b = pos[idx(nx - 1, j)]
        return (c, atan2(b.y - a.y, b.x - a.x))
    }

    func rotate(by angle: Float) {
        if isMovingWhole {
            wholeMoveAngle = (wholeMoveAngle + angle).truncatingRemainder(dividingBy: 2 * .pi)
            applyWholePose()
            return
        }
        let c = pose().center
        let ca = cos(angle), sa = sin(angle)
        func rot(_ p: SIMD3<Float>) -> SIMD3<Float> {
            let d = SIMD2(p.x, p.y) - c
            return SIMD3(c.x + d.x * ca - d.y * sa, c.y + d.x * sa + d.y * ca, p.z)
        }
        for i in 0..<count {
            pos[i] = rot(pos[i])
            prev[i] = rot(prev[i])
        }
        if isGrabbing {
            grabPos = rot(grabPos)
            grabTarget = rot(grabTarget)
        }
    }

    // MARK: захват

    // Unfold to the rest mesh, keeping the current center, orientation and size.
    // Rebuild from rest on each edit so repeated rotations cannot stretch the rug.
    func beginWholeMove(height: Float) {
        let p = pose()
        endWholeMove()
        endGrab()
        wholeMoveCenter = SIMD3(p.center.x, p.center.y, height)
        wholeMoveAngle = p.angle
        projectionLift = max(0, height)
        applyWholePose()
    }

    func moveWhole(to center: SIMD3<Float>) {
        guard isMovingWhole else { return }
        wholeMoveCenter = center
        applyWholePose()
    }

    func transformWhole(width newWidth: Float, center: SIMD3<Float>, angle: Float) {
        guard isMovingWhole, newWidth.isFinite, newWidth > 0, angle.isFinite else { return }
        width = newWidth
        spacing = width / Float(nx - 1)
        height = spacing * Float(ny - 1)
        thickness = spacing * 1.1
        for j in 0..<ny {
            for i in 0..<nx {
                rest[idx(i, j)] = SIMD3(Float(i) * spacing - width / 2, Float(j) * spacing - height / 2, 0)
            }
        }
        // Update the simulation's rest lengths too; otherwise the resized rug
        // would spring back to its old dimensions as soon as it is released.
        for i in cons.indices {
            cons[i].rest = simd_distance(rest[Int(cons[i].a)], rest[Int(cons[i].b)])
        }
        wholeMoveCenter = center
        wholeMoveAngle = angle.truncatingRemainder(dividingBy: 2 * .pi)
        applyWholePose()
    }

    private func applyWholePose() {
        guard let center = wholeMoveCenter else { return }
        let ca = cos(wholeMoveAngle), sa = sin(wholeMoveAngle)
        for i in 0..<count {
            let r = rest[i]
            pos[i] = center + SIMD3(r.x * ca - r.y * sa, r.x * sa + r.y * ca, 0)
            prev[i] = pos[i]
            vel[i] = .zero
        }
        maxSpeed = 0
    }

    func endWholeMove() {
        guard isMovingWhole else { return }
        wholeMoveCenter = nil
        if hash.spacing != thickness { hash = SpatialHash(spacing: thickness, maxObjects: count) }
        for i in 0..<count { prev[i] = pos[i]; vel[i] = .zero }
        maxSpeed = 1000
    }

    func beginGrab(vertex v: Int, target: SIMD3<Float>) {
        endWholeMove()
        endGrab()
        let vi = v % nx, vj = v / nx
        let center = pos[v]
        for j in max(0, vj - 1)...min(ny - 1, vj + 1) {
            for i in max(0, vi - 1)...min(nx - 1, vi + 1) {
                let k = idx(i, j)
                grabbedIds.append(k)
                var o = pos[k] - center
                o.z = 0
                grabOffsets.append(o)
                invMass[k] = 0
            }
        }
        grabPos = center
        grabTarget = target
        // привязи дальнего действия: ни одна точка не уходит от руки дальше, чем по ткани в покое
        tetherRest = (0..<count).map { simd_length(rest[$0] - rest[v]) * 1.01 }
        maxSpeed = 1000
    }

    func endGrab() {
        let cap: Float = 1400
        for k in grabbedIds {
            invMass[k] = 1
            let s = simd_length(vel[k])
            if s > cap { vel[k] *= cap / s }
        }
        grabbedIds.removeAll()
        grabOffsets.removeAll()
    }

    // MARK: шаг симуляции

    func step(dt frameDt: Float) {
        guard !isMovingWhole else { return }
        let n = params.substeps
        let sdt = frameDt / Float(n)
        let maxVel = 0.2 * thickness / sdt
        let travel = min(max(maxSpeed * frameDt * 1.5, thickness), thickness * 4)
        hash.create(pos, count)
        hash.queryAll(pos, count, maxDist: travel)

        for _ in 0..<n {
            integrate(sdt, maxVel)
            moveGrabbed(sdt)
            solveConstraints(sdt)
            solveTethers()
            solveSelfCollisions()
            solveSelfCollisions()
            solveGround()
            let inv = 1 / sdt
            for i in 0..<count { vel[i] = (pos[i] - prev[i]) * inv }
        }
        var m: Float = 0
        for i in 0..<count where invMass[i] > 0 { m = max(m, simd_length_squared(vel[i])) }
        maxSpeed = m.squareRoot()
        if !maxSpeed.isFinite {
            // на всякий случай: если решатель разошелся, кладем ковер заново
            let c = SIMD2<Float>(prev[count / 2].x, prev[count / 2].y)
            layout(center: c.x.isFinite && c.y.isFinite ? c : SIMD2(width, height), angle: 0, drop: 60)
        }
        if projectionLift > 0 {
            // Keep the release frame identical to the preview. As the rug falls,
            // remove the remaining common height until normal perspective resumes
            // at the floor. Raised folds and icon bumps still retain their depth.
            var lowest = projectionLift
            for i in 0..<count { lowest = min(lowest, pos[i].z) }
            projectionLift = max(0, lowest)
        }
    }

    private func solveTethers() {
        guard isGrabbing, tetherRest.count == count else { return }
        let g = grabPos
        for i in 0..<count {
            if invMass[i] == 0 { continue }
            let d = pos[i] - g
            let l = simd_length(d)
            let m = tetherRest[i]
            if l > m { pos[i] = g + d * (m / l) }
        }
    }

    private func integrate(_ sdt: Float, _ maxVel: Float) {
        let g = params.gravity * sdt
        let damp = max(0, 1 - params.airDamping * sdt)
        let mv2 = maxVel * maxVel
        for i in 0..<count {
            if invMass[i] == 0 { continue }
            var v = vel[i]
            v.z -= g
            v *= damp
            let s2 = simd_length_squared(v)
            if s2 > mv2 { v *= maxVel / s2.squareRoot() }
            vel[i] = v
            prev[i] = pos[i]
            pos[i] += v * sdt
        }
    }

    private func moveGrabbed(_ sdt: Float) {
        guard isGrabbing else { return }
        var d = (grabTarget - grabPos) * min(1, sdt * 30)
        let maxStep = 0.3 * thickness
        let l = simd_length(d)
        if l > maxStep { d *= maxStep / l }
        grabPos += d
        for (n, k) in grabbedIds.enumerated() {
            prev[k] = pos[k]
            pos[k] = grabPos + grabOffsets[n]
        }
    }

    private func solveConstraints(_ sdt: Float) {
        let inv = 1 / (sdt * sdt)
        cons.withUnsafeBufferPointer { cb in
            for c in cb {
                let a = Int(c.a), b = Int(c.b)
                let wa = invMass[a], wb = invMass[b]
                let w = wa + wb
                if w == 0 { continue }
                let d = pos[b] - pos[a]
                let len = simd_length(d)
                if len < 1e-6 { continue }
                let s = -(len - c.rest) / (w + c.compliance * inv)
                let nd = d / len
                pos[a] -= nd * (s * wa)
                pos[b] += nd * (s * wb)
            }
        }
    }

    // Точки, которые держит мышь, тоже сталкиваются: у них бесконечная масса, двигается только сосед.
    private func solveSelfCollisions() {
        let t = thickness, t2 = t * t
        let fr = params.selfFriction
        for i in 0..<count {
            let first = Int(hash.firstAdj[i]), last = Int(hash.firstAdj[i + 1])
            if first == last { continue }
            let wi = invMass[i]
            for k in first..<last {
                let j = Int(hash.adj[k])
                let wj = invMass[j]
                let w = wi + wj
                if w == 0 { continue }
                var d = pos[j] - pos[i]
                let dist2 = simd_length_squared(d)
                if dist2 > t2 || dist2 == 0 { continue }
                let restDist2 = simd_length_squared(rest[i] - rest[j])
                if dist2 > restDist2 { continue }
                var minDist = t
                if restDist2 < t2 { minDist = restDist2.squareRoot() }
                let dist = dist2.squareRoot()
                d *= (minDist - dist) / (dist * w)
                pos[i] -= d * wi
                pos[j] += d * wj
                let v0 = pos[i] - prev[i], v1 = pos[j] - prev[j]
                let avg = (v0 * wj + v1 * wi) / w
                pos[i] += (avg - v0) * (fr * wi / w * 2)
                pos[j] += (avg - v1) * (fr * wj / w * 2)
            }
        }
    }

    private func solveGround() {
        let ms = params.groundStatic, mk = params.groundKinetic
        for i in 0..<count {
            if invMass[i] == 0 { continue }
            var p = pos[i]
            let h = ground?.sample(p.x, p.y) ?? 0
            if p.z >= h { continue }
            let pen = h - p.z
            p.z = h
            let q = prev[i]
            let dx = p.x - q.x, dy = p.y - q.y
            let l = (dx * dx + dy * dy).squareRoot()
            if l < ms * pen {
                p.x = q.x
                p.y = q.y
            } else if l > 0 {
                let k = min(mk * pen / l, 1)
                p.x -= dx * k
                p.y -= dy * k
            }
            pos[i] = p
        }
    }

    // MARK: производные для рендера и мыши

    func updateDerived(_ cam: Camera) {
        let cam = presentationCamera(cam)
        for i in 0..<count { nrm[i] = .zero }
        indices.withUnsafeBufferPointer { ib in
            var t = 0
            while t < ib.count {
                let a = Int(ib[t]), b = Int(ib[t + 1]), c = Int(ib[t + 2])
                t += 3
                let n = simd_cross(pos[b] - pos[a], pos[c] - pos[a])
                nrm[a] += n; nrm[b] += n; nrm[c] += n
            }
        }
        var lo = SIMD2<Float>(repeating: .greatestFiniteMagnitude)
        var hi = SIMD2<Float>(repeating: -.greatestFiniteMagnitude)
        for i in 0..<count {
            let l = simd_length(nrm[i])
            nrm[i] = l > 0 ? nrm[i] / l : SIMD3(0, 0, 1)
            let s = cam.project(pos[i])
            proj[i] = s
            lo = simd_min(lo, s)
            hi = simd_max(hi, s)
        }
        bboxMin = lo
        bboxMax = hi
    }

    // Самый верхний треугольник под точкой экрана. Возвращает ближайшую его вершину и высоту.
    func pick(_ s: SIMD2<Float>, needVertex: Bool) -> (vertex: Int, z: Float)? {
        if s.x < bboxMin.x || s.y < bboxMin.y || s.x > bboxMax.x || s.y > bboxMax.y { return nil }
        var bestZ = -Float.greatestFiniteMagnitude
        var bestV = -1
        indices.withUnsafeBufferPointer { ib in
            var t = 0
            while t < ib.count {
                let a = Int(ib[t]), b = Int(ib[t + 1]), c = Int(ib[t + 2])
                t += 3
                let pa = proj[a], pb = proj[b], pc = proj[c]
                if s.x < min(pa.x, pb.x, pc.x) || s.x > max(pa.x, pb.x, pc.x) { continue }
                if s.y < min(pa.y, pb.y, pc.y) || s.y > max(pa.y, pb.y, pc.y) { continue }
                let v0 = pb - pa, v1 = pc - pa, v2 = s - pa
                let den = v0.x * v1.y - v1.x * v0.y
                if abs(den) < 1e-6 { continue }
                let u = (v2.x * v1.y - v1.x * v2.y) / den
                let v = (v0.x * v2.y - v2.x * v0.y) / den
                if u < 0 || v < 0 || u + v > 1 { continue }
                let w = 1 - u - v
                let z = pos[a].z * w + pos[b].z * u + pos[c].z * v
                if z > bestZ {
                    bestZ = z
                    bestV = (w >= u && w >= v) ? a : (u >= v ? b : c)
                    if !needVertex { return }
                }
            }
        }
        return bestV >= 0 ? (bestV, bestZ) : nil
    }
}
