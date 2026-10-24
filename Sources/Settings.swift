import SwiftUI
import AppKit
import ImageIO
import ApplicationServices

func displayName(_ u: URL) -> String {
    var n = u.deletingPathExtension().lastPathComponent
    // «1 persian» -> «persian», номер нужен только для порядка
    if let sp = n.firstIndex(of: " "), n[..<sp].allSatisfy(\.isNumber) { n = String(n[n.index(after: sp)...]) }
    if let b = builtinNames[n] { return L(b.ru, b.en) }
    return n
}

final class SettingsModel: ObservableObject {
    @Published var rugs: [URL] = []
    @Published var floors: [URL] = []
    @Published var currentRug: URL?
    @Published var currentFloor: String?
    @Published var scale: Double = 0.46
    @Published var axTrusted = true
    @Published var finderArranges = false
    @Published var hasPrevWallpaper = false
    @Published var hasIconHome = false
    weak var app: AppDelegate?
    private var thumbs: [URL: NSImage] = [:]

    func invalidateThumbnail(_ url: URL) { thumbs.removeValue(forKey: url) }

    func thumb(_ u: URL) -> NSImage {
        if let t = thumbs[u] { return t }
        var img = NSImage(size: NSSize(width: 1, height: 1))
        let opts = [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 360] as CFDictionary
        if let src = CGImageSourceCreateWithURL(u as CFURL, nil), let cg = CGImageSourceCreateThumbnailAtIndex(src, 0, opts) {
            img = NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
        }
        thumbs[u] = img
        return img
    }

    func refresh() {
        guard let app else { return }
        rugs = app.builtinRugs + app.userRugs
        floors = Floors.all
        currentRug = app.currentRug.standardizedFileURL
        currentFloor = NSScreen.screens.first.flatMap { NSWorkspace.shared.desktopImageURL(for: $0) }?.lastPathComponent
        scale = Double(app.controller?.scale ?? 0.46)
        axTrusted = AXIsProcessTrusted()
        finderArranges = IconPile.finderArranges
        hasPrevWallpaper = UserDefaults.standard.string(forKey: "rug.prevWallpaper") != nil
        hasIconHome = IconPile.hasHome
    }
}

struct SettingsView: View {
    @ObservedObject var m: SettingsModel

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text(L("Тяни ковер мышью за любой край. Прокрутка над ковром поворачивает его, правый клик по ковру открывает короткое меню. Окно можно закрыть, ковер останется на столе, а настройки откроются снова из «Программ» или из значка в строке меню.", "Grab the rug by any edge and drag. Scroll over the rug to rotate it, right-click it for a quick menu. You can close this window, the rug stays on the desktop, and settings open again from Applications or the menu bar icon."))
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            section(L("Ковер", "Rug")) {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 12) {
                        ForEach(m.rugs, id: \.self) { u in
                            tile(u, selected: u.standardizedFileURL == m.currentRug) { m.app?.selectRug(u) }
                                .contextMenu {
                                    if m.app?.isUserRug(u) == true {
                                        Button(role: .destructive) { m.app?.deleteUserRug(u) } label: {
                                            Label(L("Переместить в Корзину", "Move to Trash"), systemImage: "trash")
                                        }
                                    }
                                }
                        }
                        Button { m.app?.openRug() } label: {
                            VStack(spacing: 4) {
                                Image(systemName: "plus").font(.title3)
                                Text(L("Своя картинка", "Your image")).font(.caption)
                            }
                            .frame(width: 100, height: 68)
                            .background(RoundedRectangle(cornerRadius: 8).strokeBorder(.secondary, style: StrokeStyle(lineWidth: 1.5, dash: [4])))
                        }
                        .buttonStyle(.plain)
                    }
                    .padding(.vertical, 3)
                }
                Text(L("Чтобы удалить свою картинку, нажми на нее правой кнопкой и выбери «Переместить в Корзину».", "Right-click a custom image and choose Move to Trash to remove it."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            section(L("Размер", "Size")) {
                HStack {
                    Image(systemName: "rectangle").font(.caption)
                    Slider(value: $m.scale, in: 0.25...0.75, onEditingChanged: { editing in
                        if !editing { m.app?.controller?.setScale(Float(m.scale)) }
                    })
                    Image(systemName: "rectangle").font(.title2)
                }
            }

            section(L("Пол на рабочем столе", "Desktop floor")) {
                HStack(spacing: 12) {
                    ForEach(m.floors, id: \.self) { u in
                        tile(u, selected: u.lastPathComponent == m.currentFloor) {
                            Floors.apply(u)
                            m.refresh()
                        }
                    }
                }
                if m.hasPrevWallpaper {
                    Button(L("Вернуть прежние обои", "Restore previous wallpaper")) {
                        Floors.restore()
                        m.refresh()
                    }
                }
            }

            section(L("Иконки", "Icons")) {
                HStack {
                    Button(L("Замести под ковер", "Sweep under the rug")) {
                        m.app?.controller?.pileIcons()
                        m.refresh()
                    }
                    Button(L("Разложить обратно", "Put them back")) {
                        m.app?.controller?.restoreIcons()
                        m.refresh()
                    }
                    .disabled(!m.hasIconHome)
                    Button(L("Накрыть иконки ковром", "Cover icons with the rug")) { m.app?.controller?.coverIcons() }
                }
                if m.finderArranges {
                    Text(L("Сейчас Finder сам раскладывает иконки по стопкам или сортирует их, поэтому они возвращаются на места. Правый клик по пустому месту рабочего стола, снять «Использовать стопки», в «Сортировать по» выбрать «Нет».", "Finder is grouping your icons into stacks or sorting them, so they snap back. Right-click an empty spot on the desktop, uncheck “Use Stacks” and set “Sort By” to “None”."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if !m.axTrusted {
                    Button(L("Разрешить видеть иконки", "Allow access to icons")) { m.app?.askAccessibility() }
                }
            }

            HStack {
                Button(L("Уронить заново", "Drop again")) { m.app?.controller?.redrop() }
                Button(L("В центр", "Center")) { m.app?.controller?.center() }
                Spacer()
                Button(L("Выйти из Ковра", "Quit Rug")) { NSApp.terminate(nil) }
                    .keyboardShortcut("q")
            }
        }
        .padding(22)
        .frame(width: 620)
    }

    private func section<C: View>(_ title: String, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline)
            content()
        }
    }

    private func tile(_ u: URL, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 5) {
                Image(nsImage: m.thumb(u))
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: 100, height: 68)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(selected ? Color.accentColor : Color.black.opacity(0.15), lineWidth: selected ? 3 : 1))
                Text(displayName(u)).font(.caption).lineLimit(1)
            }
            .frame(width: 100)
        }
        .buttonStyle(.plain)
    }
}
