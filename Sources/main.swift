import AppKit
import SwiftUI
import UniformTypeIdentifiers
import ApplicationServices

// Обои-полы. Копируются в Application Support, чтобы путь у обоев не зависел от места приложения.
enum Floors {
    static var all: [URL] {
        let dir = Bundle.main.resourceURL!.appendingPathComponent("floors")
        return ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension.lowercased() == "jpg" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    static var dir: URL {
        let u = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Rug/floors")
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    static func apply(_ src: URL) {
        guard let screen = NSScreen.screens.first else { return }
        let dst = dir.appendingPathComponent(src.lastPathComponent)
        try? FileManager.default.removeItem(at: dst)
        try? FileManager.default.copyItem(at: src, to: dst)
        if UserDefaults.standard.string(forKey: "rug.prevWallpaper") == nil,
           let prev = NSWorkspace.shared.desktopImageURL(for: screen), !prev.path.hasPrefix(dir.path) {
            UserDefaults.standard.set(prev.path, forKey: "rug.prevWallpaper")
        }
        try? NSWorkspace.shared.setDesktopImageURL(dst, for: screen, options: [
            .imageScaling: NSImageScaling.scaleProportionallyUpOrDown.rawValue,
            .allowClipping: true,
        ])
    }

    static func restore() {
        guard let screen = NSScreen.screens.first, let p = UserDefaults.standard.string(forKey: "rug.prevWallpaper") else { return }
        try? NSWorkspace.shared.setDesktopImageURL(URL(fileURLWithPath: p), for: screen, options: [:])
        UserDefaults.standard.removeObject(forKey: "rug.prevWallpaper")
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate, NSWindowDelegate {
    var controller: RugController?
    var statusItem: NSStatusItem?
    private var settingsWindow: NSWindow?
    private let model = SettingsModel()
    let support: URL = {
        let u = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Rug")
        try? FileManager.default.createDirectory(at: u.appendingPathComponent("rugs"), withIntermediateDirectories: true)
        return u
    }()

    var builtinRugs: [URL] {
        let dir = Bundle.main.resourceURL!.appendingPathComponent("rugs")
        return ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
            .filter { ["jpg", "jpeg", "png"].contains($0.pathExtension.lowercased()) }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    var userRugs: [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: support.appendingPathComponent("rugs"), includingPropertiesForKeys: nil)) ?? [])
            .filter { ["jpg", "jpeg", "png", "heic", "webp"].contains($0.pathExtension.lowercased()) }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    func isUserRug(_ url: URL) -> Bool {
        url.standardizedFileURL.deletingLastPathComponent() == support.appendingPathComponent("rugs").standardizedFileURL
    }

    func deleteUserRug(_ url: URL) {
        guard isUserRug(url) else { return }
        let wasSelected = currentRug.standardizedFileURL == url.standardizedFileURL
        do {
            try FileManager.default.trashItem(at: url, resultingItemURL: nil)
            model.invalidateThumbnail(url)
            if wasSelected, let fallback = builtinRugs.first {
                selectRug(fallback)
            } else {
                model.refresh()
            }
        } catch {
            NSApp.presentError(error)
        }
    }

    var currentRug: URL {
        if let p = UserDefaults.standard.string(forKey: "rug.texture"), FileManager.default.fileExists(atPath: p) {
            return URL(fileURLWithPath: p)
        }
        return builtinRugs.first!
    }

    func applicationDidFinishLaunching(_ n: Notification) {
        guard let screen = NSScreen.screens.first else { return }
        let c = RugController(screen: screen, textureURL: currentRug)
        c.menuProvider = { [weak self] in self?.makeMenu() ?? NSMenu() }
        controller = c

        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = NSImage(systemSymbolName: "rectangle.inset.filled", accessibilityDescription: L("Ковер", "Rug"))
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        statusItem = item

        setupMainMenu()
        showSettings()

        // точные места иконок и стопок macOS отдает только через Accessibility
        if !AXIsProcessTrusted() && !UserDefaults.standard.bool(forKey: "rug.askedAX") {
            UserDefaults.standard.set(true, forKey: "rug.askedAX")
            askAccessibility()
        }
    }

    // Повторный запуск из «Программ», Spotlight или Launchpad открывает настройки.
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showSettings()
        return true
    }

    // Пока открыто окно настроек, у Ковра есть иконка в Dock и обычное меню с ⌘Q.
    @objc func showSettings() {
        model.app = self
        model.refresh()
        if settingsWindow == nil {
            let host = NSHostingView(rootView: SettingsView(m: model))
            let w = NSWindow(contentRect: NSRect(origin: .zero, size: host.fittingSize),
                             styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
            w.title = L("Ковер", "Rug")
            w.contentView = host
            w.isReleasedWhenClosed = false
            w.delegate = self
            w.center()
            settingsWindow = w
        }
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        settingsWindow?.makeKeyAndOrderFront(nil)
    }

    func windowWillClose(_ n: Notification) {
        if (n.object as? NSWindow) === settingsWindow { NSApp.setActivationPolicy(.accessory) }
    }

    func windowDidBecomeKey(_ n: Notification) { model.refresh() }

    private func setupMainMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        main.addItem(appItem)
        let am = NSMenu()
        let s = NSMenuItem(title: L("Настройки…", "Settings…"), action: #selector(showSettings), keyEquivalent: ",")
        s.target = self
        am.addItem(s)
        am.addItem(.separator())
        am.addItem(NSMenuItem(title: L("Закрыть окно", "Close Window"), action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w"))
        am.addItem(NSMenuItem(title: L("Выйти из Ковра", "Quit Rug"), action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        appItem.submenu = am
        NSApp.mainMenu = main
    }

    func selectRug(_ u: URL) {
        UserDefaults.standard.set(u.path, forKey: "rug.texture")
        controller?.setTexture(u)
        model.refresh()
    }

    @objc func askAccessibility() {
        let opt = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        _ = AXIsProcessTrustedWithOptions([opt: true] as CFDictionary)
    }

    func applicationWillTerminate(_ n: Notification) { controller?.savePose() }

    private func add(_ m: NSMenu, _ title: String, _ sel: Selector, state: Bool = false, obj: Any? = nil) {
        let it = NSMenuItem(title: title, action: sel, keyEquivalent: "")
        it.target = self
        it.state = state ? .on : .off
        it.representedObject = obj
        m.addItem(it)
    }

    func makeMenu() -> NSMenu {
        let m = NSMenu()
        fill(m)
        return m
    }

    // меню собирается заново при каждом открытии, чтобы галочки и пункты были актуальны
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        fill(menu)
    }

    private func fill(_ m: NSMenu) {
        add(m, L("Настройки…", "Settings…"), #selector(showSettings))
        m.addItem(.separator())
        add(m, L("Замести иконки под ковер", "Sweep icons under the rug"), #selector(pileIcons))
        if IconPile.hasHome { add(m, L("Разложить иконки обратно", "Put icons back"), #selector(restoreIcons)) }
        add(m, L("Уронить заново", "Drop again"), #selector(redrop))
        add(m, L("Накрыть иконки", "Cover icons"), #selector(coverIcons))
        add(m, L("Положить в центр", "Center"), #selector(centerRug))

        let sizes = NSMenu()
        let cur = controller?.scale ?? 0.46
        for (t, v) in [(L("Маленький", "Small"), 0.32), (L("Средний", "Medium"), 0.46), (L("Большой", "Large"), 0.62)] {
            add(sizes, t, #selector(setSize(_:)), state: abs(cur - Float(v)) < 0.01, obj: v)
        }
        let si = NSMenuItem(title: L("Размер", "Size"), action: nil, keyEquivalent: "")
        si.submenu = sizes
        m.addItem(si)

        let rugs = NSMenu()
        let current = currentRug.standardizedFileURL
        for u in builtinRugs + userRugs {
            add(rugs, displayName(u), #selector(pickRug(_:)), state: u.standardizedFileURL == current, obj: u)
        }
        rugs.addItem(.separator())
        add(rugs, L("Свой ковер из картинки…", "Rug from your image…"), #selector(openRug))
        let ri = NSMenuItem(title: L("Ковер", "Rug"), action: nil, keyEquivalent: "")
        ri.submenu = rugs
        m.addItem(ri)

        m.addItem(.separator())
        if !AXIsProcessTrusted() {
            add(m, L("Разрешить видеть иконки…", "Allow access to icons…"), #selector(askAccessibility))
        }
        let floors = NSMenu()
        let wall = NSScreen.screens.first.flatMap { NSWorkspace.shared.desktopImageURL(for: $0) }?.lastPathComponent
        for u in Floors.all {
            add(floors, displayName(u), #selector(pickFloor(_:)), state: u.lastPathComponent == wall, obj: u)
        }
        if UserDefaults.standard.string(forKey: "rug.prevWallpaper") != nil {
            floors.addItem(.separator())
            add(floors, L("Вернуть прежние обои", "Restore previous wallpaper"), #selector(restoreWallpaper))
        }
        let fi = NSMenuItem(title: L("Пол на рабочем столе", "Desktop floor"), action: nil, keyEquivalent: "")
        fi.submenu = floors
        m.addItem(fi)
        m.addItem(.separator())
        add(m, L("Выйти из Ковра", "Quit Rug"), #selector(quit))
    }

    private func refreshMenu() {}

    @objc func redrop() { controller?.redrop() }
    @objc func centerRug() { controller?.center() }
    @objc func coverIcons() { controller?.coverIcons() }
    @objc func pileIcons() { controller?.pileIcons() }
    @objc func restoreIcons() { controller?.restoreIcons() }

    @objc func setSize(_ sender: NSMenuItem) {
        guard let v = sender.representedObject as? Double else { return }
        controller?.setScale(Float(v))
        refreshMenu()
    }

    @objc func pickRug(_ sender: NSMenuItem) {
        guard let u = sender.representedObject as? URL else { return }
        selectRug(u)
    }

    @objc func openRug() {
        NSApp.activate(ignoringOtherApps: true)
        let p = NSOpenPanel()
        p.allowedContentTypes = [.image]
        p.message = L("Картинка ковра, вид сверху, ковер во весь кадр", "A top-down picture of a rug that fills the whole frame")
        guard p.runModal() == .OK, let src = p.url else { return }
        let dst = support.appendingPathComponent("rugs").appendingPathComponent(src.lastPathComponent)
        try? FileManager.default.removeItem(at: dst)
        do {
            try FileManager.default.copyItem(at: src, to: dst)
        } catch {
            rugLog("copy error \(error)")
            return
        }
        selectRug(dst)
    }

    @objc func pickFloor(_ sender: NSMenuItem) {
        guard let u = sender.representedObject as? URL else { return }
        Floors.apply(u)
    }

    @objc func restoreWallpaper() { Floors.restore() }

    @objc func quit() { NSApp.terminate(nil) }
}

if let i = CommandLine.arguments.firstIndex(of: "--demo") {
    let args = CommandLine.arguments
    Demo.run(outDir: i + 1 < args.count ? args[i + 1] : "demo", rugPath: i + 2 < args.count ? args[i + 2] : nil)
    exit(0)
}

if let i = CommandLine.arguments.firstIndex(of: "--settings-shot"), i + 1 < CommandLine.arguments.count {
    // снимок окна настроек для проверки верстки
    _ = NSApplication.shared
    let d = AppDelegate()
    let m = SettingsModel()
    m.app = d
    m.refresh()
    let host = NSHostingView(rootView: SettingsView(m: m))
    let w = NSWindow(contentRect: NSRect(origin: .zero, size: host.fittingSize), styleMask: [.titled], backing: .buffered, defer: false)
    w.contentView = host
    host.layoutSubtreeIfNeeded()
    if let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
        host.cacheDisplay(in: host.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: CommandLine.arguments[i + 1]))
    }
    exit(0)
}

if let i = CommandLine.arguments.firstIndex(of: "--floor"), i + 1 < CommandLine.arguments.count {
    let q = CommandLine.arguments[i + 1]
    if q == "restore" { Floors.restore() } else if let u = Floors.all.first(where: { $0.lastPathComponent.contains(q) }) { Floors.apply(u) }
    exit(0)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
