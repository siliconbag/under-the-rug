import AppKit
import MetalKit
import simd

// Окно над иконками рабочего стола, но под всеми остальными окнами.
final class RugPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

final class RugView: MTKView {
    weak var controller: RugController?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    private func pt(_ e: NSEvent) -> SIMD2<Float> {
        let p = convert(e.locationInWindow, from: nil)
        return SIMD2(Float(p.x), Float(p.y))
    }

    override func mouseDown(with e: NSEvent) { controller?.mouseDown(pt(e), wholeRug: e.modifierFlags.contains(.option)) }
    override func mouseDragged(with e: NSEvent) { controller?.mouseDragged(pt(e)) }
    override func mouseUp(with e: NSEvent) { controller?.mouseUp() }
    override func flagsChanged(with e: NSEvent) { controller?.modifiersChanged(e.modifierFlags) }
    override func scrollWheel(with e: NSEvent) { controller?.scroll(e) }
    override func rightMouseDown(with e: NSEvent) { controller?.contextMenu(e) }
}

final class RugController: NSObject, MTKViewDelegate {
    let screen: NSScreen
    let window: RugPanel
    let view: RugView
    private(set) var renderer: Renderer!
    private(set) var cloth: Cloth!
    private(set) var camera: Camera
    private let field: HeightField
    private let scanner = DesktopScanner()
    private var lastBumps: [Bump] = []
    private var hoverTimer: Timer?
    private var scanTimer: Timer?
    private var desktopWatch: DispatchSourceFileSystemObject?
    private var dragging = false
    private var wholeEditor = WholeRugEditor()
    private let editOverlay = RugEditOverlay(frame: .zero)
    private var calmFrames = 0
    private var stepTime: Double = 0
    private var frames = 0
    var menuProvider: (() -> NSMenu)?

    let liftHeight: Float = 120
    var scale: Float {
        get { let v = UserDefaults.standard.float(forKey: "rug.scale"); return v > 0 ? v : 0.46 }
        set { UserDefaults.standard.set(newValue, forKey: "rug.scale") }
    }

    private var awake = true {
        didSet { if awake != oldValue { view.isPaused = !awake } }
    }

    init(screen: NSScreen, textureURL: URL) {
        self.screen = screen
        let frame = screen.frame
        camera = Camera(size: SIMD2(Float(frame.width), Float(frame.height)))
        field = HeightField(size: camera.size)

        window = RugPanel(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        window.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.desktopIconWindow)) + 1)
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        window.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle, .fullScreenNone]
        window.ignoresMouseEvents = true
        window.isReleasedWhenClosed = false
        window.hidesOnDeactivate = false

        view = RugView(frame: NSRect(origin: .zero, size: frame.size), device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .bgra8Unorm
        view.depthStencilPixelFormat = .depth32Float
        view.sampleCount = 4
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        view.preferredFramesPerSecond = 60
        view.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        view.layer?.isOpaque = false
        super.init()

        view.controller = self
        view.delegate = self
        window.contentView = view
        editOverlay.frame = view.bounds
        editOverlay.autoresizingMask = [.width, .height]
        editOverlay.wantsLayer = true
        editOverlay.isHidden = true
        view.addSubview(editOverlay)

        do {
            renderer = try Renderer(device: view.device!, colorFormat: view.colorPixelFormat,
                                    depthFormat: view.depthStencilPixelFormat, sampleCount: view.sampleCount, textureURL: textureURL)
        } catch {
            rugLog("renderer error \(error)")
            fatalError("\(error)")
        }
        buildCloth(drop: 90, keepPose: false)

        window.setFrame(frame, display: true)
        window.orderFrontRegardless()

        hoverTimer = Timer(timeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.pollHover() }
        RunLoop.main.add(hoverTimer!, forMode: .common)
        scanTimer = Timer(timeInterval: 4, repeats: true) { [weak self] _ in self?.rescan() }
        RunLoop.main.add(scanTimer!, forMode: .common)
        watchDesktop()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.rescan() }
    }

    // MARK: ковер

    private func savedPose() -> (SIMD2<Float>, Float)? {
        guard let a = UserDefaults.standard.array(forKey: "rug.pose") as? [Double], a.count == 3 else { return nil }
        return (SIMD2(Float(a[0]), Float(a[1])) * camera.size, Float(a[2]))
    }

    func savePose() {
        guard let cloth else { return }
        scale = cloth.width / camera.size.x
        let p = cloth.pose()
        let c = p.center / camera.size
        UserDefaults.standard.set([Double(c.x), Double(c.y), Double(p.angle)], forKey: "rug.pose")
    }

    func buildCloth(drop: Float, keepPose: Bool) {
        finishWholeEdit()
        var center = camera.center
        var angle: Float = 0
        if keepPose, let cloth {
            (center, angle) = cloth.pose()
        } else if let (c, a) = savedPose() {
            center = c
            angle = a
        }
        let tex = renderer.rugTexture
        let aspect = Float(tex.height) / Float(tex.width)
        let w = camera.size.x * scale
        let c = Cloth(width: w, height: w * aspect, columns: ClothParams.tuned.columns)
        c.ground = field
        c.layout(center: center, angle: angle, drop: drop)
        c.updateDerived(camera)
        cloth = c
        renderer.setMesh(c)
        wake()
    }

    func redrop() { buildCloth(drop: 110, keepPose: true) }

    func center() {
        finishWholeEdit()
        UserDefaults.standard.removeObject(forKey: "rug.pose")
        cloth.layout(center: camera.center, angle: 0, drop: 60)
        wake()
    }

    func setScale(_ s: Float) {
        finishWholeEdit()
        scale = s
        buildCloth(drop: 60, keepPose: true)
    }

    func setTexture(_ url: URL) {
        do {
            try renderer.setTexture(url)
            buildCloth(drop: 110, keepPose: true)
        } catch {
            rugLog("texture error \(error)")
            NSSound.beep()
        }
    }

    private func wake() {
        calmFrames = 0
        awake = true
    }

    // MARK: кадр

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        let dt: Float = 1.0 / Float(max(30, view.preferredFramesPerSecond))
        let fieldMoving = field.animate(dt: dt)
        let t0 = CACurrentMediaTime()
        cloth.step(dt: dt)
        cloth.updateDerived(camera)
        stepTime += CACurrentMediaTime() - t0
        frames += 1
        if frames == 120 {
            rugDebug(String(format: "sim %.2f ms/frame, maxSpeed %.1f", stepTime / 120 * 1000, cloth.maxSpeed))
            frames = 0
            stepTime = 0
        }
        renderer.draw(cloth: cloth, camera: camera, in: view)

        if !cloth.isGrabbing && !cloth.isMovingWhole && !fieldMoving && cloth.maxSpeed < 3 {
            calmFrames += 1
        } else {
            calmFrames = 0
        }
        if calmFrames > 45 {
            awake = false
            savePose()
        }
    }

    // MARK: мышь

    private func pollHover() {
        // This desktop panel never becomes key. Polling reads modifier state
        // even when flagsChanged goes to Finder, without an event tap or AX access.
        modifiersChanged(NSEvent.modifierFlags)
        if dragging { return }
        let m = NSEvent.mouseLocation
        let s = SIMD2(Float(m.x - window.frame.minX), Float(m.y - window.frame.minY))
        // во время чужого перетаскивания (файл из Finder) ковер пропускает мышь насквозь
        let busy = NSEvent.pressedMouseButtons != 0 && window.ignoresMouseEvents
        let hit = !busy && (cloth.pick(s, needVertex: false) != nil || wholeEditor.handle(at: s, cloth: cloth, camera: camera) != nil)
        if window.ignoresMouseEvents == hit { window.ignoresMouseEvents = !hit }
    }

    func mouseDown(_ s: SIMD2<Float>, wholeRug: Bool = false) {
        if wholeRug {
            dragging = wholeEditor.mouseDown(s, cloth: cloth, camera: camera)
            refreshEditOverlay()
            wake()
            return
        }
        finishWholeEdit()
        guard let (v, z) = cloth.pick(s, needVertex: true) else { return }
        dragging = true
        cloth.beginGrab(vertex: v, target: cloth.presentationCamera(camera).unproject(s, z: max(z, 0) + liftHeight))
        wake()
    }

    func mouseDragged(_ s: SIMD2<Float>) {
        guard dragging else { return }
        if cloth.isMovingWhole {
            wholeEditor.mouseDragged(s, cloth: cloth, camera: camera)
            refreshEditOverlay()
        } else {
            cloth.grabTarget = cloth.presentationCamera(camera).unproject(s, z: cloth.grabTarget.z)
        }
        wake()
    }

    func mouseUp() {
        guard dragging else { return }
        dragging = false
        wholeEditor.mouseUp()
        if cloth.isMovingWhole { scale = cloth.width / camera.size.x }
        refreshEditOverlay()
        if !cloth.isMovingWhole { cloth.endGrab() }
        wake()
    }

    func modifiersChanged(_ flags: NSEvent.ModifierFlags) {
        if cloth.isMovingWhole && !flags.contains(.option) { finishWholeEdit() }
    }

    private func finishWholeEdit() {
        guard let cloth, cloth.isMovingWhole else { return }
        scale = cloth.width / camera.size.x
        wholeEditor.finish(cloth: cloth)
        dragging = false
        editOverlay.isHidden = true
        wake()
    }

    private func refreshEditOverlay() {
        editOverlay.isHidden = !cloth.isMovingWhole
        guard cloth.isMovingWhole else { return }
        editOverlay.geometry = wholeEditor.geometry(cloth: cloth, camera: camera)
        editOverlay.readout = wholeEditor.readout(cloth: cloth, camera: camera)
        editOverlay.needsDisplay = true
    }

    func scroll(_ e: NSEvent) {
        guard !wholeEditor.isTransforming else { return }
        let d = Float(e.scrollingDeltaY) * (e.hasPreciseScrollingDeltas ? 0.004 : 0.05)
        if d == 0 { return }
        cloth.rotate(by: d)
        cloth.updateDerived(camera)
        refreshEditOverlay()
        wake()
    }

    func contextMenu(_ e: NSEvent) {
        finishWholeEdit()
        guard let menu = menuProvider?() else { return }
        NSMenu.popUpContextMenu(menu, with: e, for: view)
    }

    // MARK: иконки

    private func watchDesktop() {
        let path = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Desktop").path
        let fd = open(path, O_EVTONLY)
        guard fd >= 0 else { return }
        let src = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete], queue: .main)
        src.setEventHandler { [weak self] in
            // Finder раскладывает новую иконку не сразу
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { self?.rescan() }
        }
        src.setCancelHandler { close(fd) }
        src.resume()
        desktopWatch = src
    }

    func rescan() {
        if dragging || cloth.isMovingWhole { return }
        let iconSize = CGFloat(UserDefaults(suiteName: "com.apple.finder")?
            .dictionary(forKey: "DesktopViewSettings")
            .flatMap { ($0["IconViewSettings"] as? [String: Any])?["iconSize"] as? NSNumber }?.doubleValue ?? 64)
        guard let icons = scanner.scan(iconSize: iconSize) else { return }
        let files = (try? FileManager.default.contentsOfDirectory(atPath: NSHomeDirectory() + "/Desktop"))?
            .filter { !$0.hasPrefix(".") } ?? []
        let h = Float(screen.frame.height)
        let bumps = icons.map { ic -> Bump in
            let side = Float(min(ic.rect.width, ic.rect.height))
            var r = max(18, min(side, Float(iconSize)) * 0.47)
            // у AX рамка это сама картинка, подпись ниже нее
            let cy = Float(ic.rect.minY) + Float(ic.rect.width) / 2
            var height: Float = 22
            let n = stackCount(ic.name, files: files)
            if n > 1 {
                height = min(50, 24 + 6 * pow(Float(n), 0.7))
                r *= 1.12
            }
            return Bump(center: SIMD2(Float(ic.rect.midX), h - cy), radius: SIMD2(r, r), height: height)
        }
        if bumps != lastBumps {
            rugDebug("icons via \(scanner.source): " + icons.prefix(30).map { "\($0.name)@\(Int($0.rect.minX)),\(Int($0.rect.minY)) \(Int($0.rect.width))x\(Int($0.rect.height))" }.joined(separator: "; "))
            lastBumps = bumps
            field.setBumps(bumps)
            wake()
        }
    }

    // Сколько файлов в стопке. Отдельный файл дает 1, стопку узнаем по названию вида.
    private func stackCount(_ title: String, files: [String]) -> Int {
        if files.contains(where: { $0 == title || ($0 as NSString).deletingPathExtension == title }) { return 1 }
        let t = title.lowercased()
        func ext(_ list: [String]) -> Int { files.filter { list.contains(($0 as NSString).pathExtension.lowercased()) }.count }
        let shots = files.filter { $0.hasPrefix("Снимок экрана") || $0.hasPrefix("Screenshot") }.count
        let recs = files.filter { $0.hasPrefix("Запись экрана") || $0.hasPrefix("Screen Recording") }.count
        var n = 3
        if t.contains("pdf") { n = ext(["pdf"]) }
        else if t.contains("снимки") || t.contains("screenshot") { n = shots }
        else if t.contains("записи") || t.contains("recording") { n = recs }
        else if t.contains("фильм") || t.contains("видео") || t.contains("movie") { n = ext(["mov", "mp4", "m4v", "avi", "mkv"]) - recs }
        else if t.contains("изображ") || t.contains("картин") || t.contains("image") { n = ext(["png", "jpg", "jpeg", "heic", "gif", "webp", "tiff"]) - shots }
        else if t.contains("документ") || t.contains("document") { n = ext(["md", "txt", "html", "htm", "doc", "docx", "rtf", "pages", "odt"]) }
        else if t.contains("папк") || t.contains("folder") {
            n = files.filter { var d: ObjCBool = false; return FileManager.default.fileExists(atPath: NSHomeDirectory() + "/Desktop/" + $0, isDirectory: &d) && d.boolValue }.count
        }
        return max(2, n)
    }

    // Сваливает иконки в кучу под ковром и роняет на нее ковер сверху.
    func pileIcons() {
        finishWholeEdit()
        if IconPile.finderArranges {
            IconPile.explainFinder()
            return
        }
        let p = cloth.pose()
        let h = CGFloat(screen.frame.height)
        IconPile.pile(at: CGPoint(x: CGFloat(p.center.x), y: h - CGFloat(p.center.y)))
        cloth.layout(center: p.center, angle: p.angle, drop: 130)
        wake()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in self?.rescan() }
    }

    func restoreIcons() {
        IconPile.restore()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in self?.rescan() }
    }

    // Бросает ковер так, чтобы он накрыл как можно больше иконок.
    func coverIcons() {
        finishWholeEdit()
        guard !lastBumps.isEmpty else { redrop(); return }
        var lo = SIMD2<Float>(repeating: .greatestFiniteMagnitude), hi = -lo
        for b in lastBumps { lo = simd_min(lo, b.center); hi = simd_max(hi, b.center) }
        var c = (lo + hi) * 0.5
        let half = SIMD2(cloth.width, cloth.height) * 0.5
        c = simd_clamp(c, half, camera.size - half)
        cloth.layout(center: c, angle: 0, drop: 110)
        wake()
    }
}
