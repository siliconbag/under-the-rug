#include "Settings.h"
#include <dwmapi.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <algorithm>
#include "App.h"
#include "Common.h"
#include "Floors.h"
#include "IconPile.h"
#include "RugController.h"
#include "Strings.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

enum {
    ID_RUGS = 100,
    ID_FLOORS,
    ID_SIZE,
    ID_RESTORE_WALL,
    ID_SWEEP,
    ID_PUTBACK,
    ID_COVER,
    ID_DROP,
    ID_CENTER,
    ID_QUIT,
    ID_AUTOSTART,
    ID_SCREEN = 200,
};

// tile geometry in points
static const float kTileW = 100, kImgH = 68, kCapGap = 5, kCapH = 17, kGap = 12, kPad = 3;

// MARK: theme

// The apps theme from Settings > Personalization > Colors. RUG_THEME=dark or light overrides it.
static bool systemDark() {
    wchar_t v[8] = L"";
    if (GetEnvironmentVariableW(L"RUG_THEME", v, 8) > 0) return v[0] == L'd';
    DWORD light = 1, n = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &n);
    return light == 0;
}

// The user's accent colour, in the shade WinUI uses for each theme (Dark1 on light, Light2 on dark).
static COLORREF accentColor(bool dark) {
    BYTE p[32];
    DWORD n = sizeof(p);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, p, &n) == ERROR_SUCCESS &&
        n >= 32) {
        int i = dark ? 1 : 4;
        return RGB(p[i * 4], p[i * 4 + 1], p[i * 4 + 2]);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

static Palette makePalette(bool dark) {
    Palette p;
    p.dark = dark;
    p.accent = accentColor(dark);
    if (dark) {
        p.bg = RGB(32, 32, 32);
        p.text = RGB(255, 255, 255);
        p.secondary = RGB(170, 170, 170);
        p.warning = RGB(255, 170, 80);
        p.rail = RGB(120, 120, 120);
        p.thumbFill = RGB(69, 69, 69);
        p.thumbEdge = RGB(95, 95, 95);
        p.tileEdge = 0x40FFFFFF;
        p.dashed = 0xFF8A8A8A;
        p.plus = 0xFFD0D0D0;
    } else {
        p.bg = GetSysColor(COLOR_WINDOW);
        p.text = GetSysColor(COLOR_WINDOWTEXT);
        p.secondary = GetSysColor(COLOR_GRAYTEXT);
        p.warning = RGB(204, 102, 0);
        p.rail = RGB(196, 196, 196);
        p.thumbFill = RGB(255, 255, 255);
        p.thumbEdge = RGB(210, 210, 210);
        p.tileEdge = 0x26000000;
        p.dashed = 0xFF8C8C8C;
        p.plus = 0xFF5A5A5A;
    }
    return p;
}

static Color gdip(COLORREF c) { return Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

static void roundRect(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

// Thumbnails come from the shell thumbnail cache, so even 5000 px textures open instantly.
static std::unique_ptr<Bitmap> loadThumb(const std::wstring& path) {
    ComPtr<IShellItemImageFactory> f;
    HBITMAP hb = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) ||
        FAILED(f->GetImage(SIZE{360, 360}, SIIGBF_RESIZETOFIT, &hb)))
        return nullptr;
    std::unique_ptr<Bitmap> b(Bitmap::FromHBITMAP(hb, nullptr));
    DeleteObject(hb);
    return b && b->GetLastStatus() == Ok ? std::move(b) : nullptr;
}

// MARK: tiles

TileStrip::TileStrip(HWND parent, int id, std::map<std::wstring, std::unique_ptr<Bitmap>>* thumbs_, const Palette* pal_)
    : thumbs(thumbs_), pal(pal_) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = L"RugTiles";
        RegisterClassExW(&wc);
        registered = true;
    }
    hwnd = CreateWindowExW(0, L"RugTiles", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), this);
}

int TileStrip::columns(int width) const { return std::max(1, int((width + kGap * sc) / ((kTileW + kGap) * sc))); }

RECT TileStrip::tileRect(int i, int width) const {
    int c = columns(width), col = i % c, row = i / c;
    int x = int(col * (kTileW + kGap) * sc);
    int y = int((kPad + row * (kImgH + kCapGap + kCapH + kGap)) * sc);
    return {x, y, x + int(kTileW * sc), y + int((kImgH + kCapGap + kCapH) * sc)};
}

// The × of a removable tile, in the top right corner of its picture.
RECT TileStrip::closeRect(int i, int width) const {
    RECT r = tileRect(i, width);
    int d = int(20 * sc), m = int(5 * sc);
    return {r.right - m - d, r.top + m, r.right - m, r.top + m + d};
}

int TileStrip::tileAt(POINT p) const {
    RECT rc;
    GetClientRect(hwnd, &rc);
    for (int i = 0; i < (int)tiles.size(); i++) {
        RECT r = tileRect(i, rc.right);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

int TileStrip::heightFor(int width) const {
    int c = columns(width), rows = std::max(1, (int(tiles.size()) + c - 1) / c);
    return int((2 * kPad + rows * (kImgH + kCapGap + kCapH) + (rows - 1) * kGap) * sc);
}

void TileStrip::paint(HDC dc, int w, int h) {
    RECT all = {0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(pal->bg);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        for (int i = 0; i < (int)tiles.size(); i++) {
            const Tile& t = tiles[i];
            RECT r = tileRect(i, w);
            RectF img(float(r.left), float(r.top), kTileW * sc, kImgH * sc);
            float rad = 8 * sc;
            if (t.path.empty()) {
                // the dashed "your image" tile
                float lw = 1.5f * sc;
                Pen pen(Color(pal->dashed), lw);
                REAL dash[] = {4 / 1.5f, 4 / 1.5f};
                pen.SetDashPattern(dash, 2);
                GraphicsPath p;
                roundRect(p, RectF(img.X + lw / 2, img.Y + lw / 2, img.Width - lw, img.Height - lw), rad);
                g.DrawPath(&pen, &p);
                Pen plus(Color(pal->plus), 1.6f * sc);
                float cx = img.X + img.Width / 2, cy = img.Y + img.Height * 0.36f, len = 7 * sc;
                g.DrawLine(&plus, cx - len, cy, cx + len, cy);
                g.DrawLine(&plus, cx, cy - len, cx, cy + len);
                continue;
            }
            auto it = thumbs->find(t.path);
            if (it == thumbs->end()) it = thumbs->emplace(t.path, loadThumb(t.path)).first;
            Bitmap* b = it->second.get();
            GraphicsPath clip;
            roundRect(clip, img, rad);
            if (b) {
                // aspect fill
                float bw = float(b->GetWidth()), bh = float(b->GetHeight());
                float k = std::max(img.Width / bw, img.Height / bh);
                float sw = img.Width / k, sh = img.Height / k;
                g.SetClip(&clip);
                g.DrawImage(b, img, (bw - sw) / 2, (bh - sh) / 2, sw, sh, UnitPixel);
                g.ResetClip();
            } else {
                SolidBrush gray(Color(255, 128, 128, 128));
                g.FillPath(&gray, &clip);
            }
            float lw = t.selected ? 3 * sc : 1 * sc;
            Pen border(t.selected ? gdip(pal->accent) : Color(pal->tileEdge), lw);
            GraphicsPath edge;
            roundRect(edge, RectF(img.X + lw / 2, img.Y + lw / 2, img.Width - lw, img.Height - lw), rad - lw / 2);
            g.DrawPath(&border, &edge);
            if (t.removable && i == hover) {
                RECT c = closeRect(i, w);
                float d = float(c.right - c.left), x = float(c.left), y = float(c.top), k = d * 0.27f;
                SolidBrush disc(Color(170, 0, 0, 0));
                Pen cross(Color(255, 255, 255, 255), 1.6f * sc);
                g.FillEllipse(&disc, x, y, d, d);
                g.DrawLine(&cross, x + d / 2 - k, y + d / 2 - k, x + d / 2 + k, y + d / 2 + k);
                g.DrawLine(&cross, x + d / 2 - k, y + d / 2 + k, x + d / 2 + k, y + d / 2 - k);
            }
        }
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font);
    SetTextColor(dc, pal->text);
    for (int i = 0; i < (int)tiles.size(); i++) {
        RECT r = tileRect(i, w);
        RECT cap = {r.left, r.top + int((kImgH + kCapGap) * sc), r.right, r.bottom};
        if (tiles[i].path.empty()) cap = {r.left, r.top + int(kImgH * 0.36f * sc + 12 * sc), r.right, r.top + int(kImgH * sc)};
        DrawTextW(dc, tiles[i].caption.c_str(), -1, &cap, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

LRESULT CALLBACK TileStrip::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (TileStrip*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        self->paint(mem, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int i = self->tileAt({(short)LOWORD(lp), (short)HIWORD(lp)});
        if (!self->tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            self->tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (i != self->hover) {
            self->hover = i;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        self->tracking = false;
        self->hover = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        int i = self->tileAt(p);
        if (i < 0) return 0;
        RECT rc, c;
        GetClientRect(h, &rc);
        c = self->closeRect(i, rc.right);
        // the × only counts while it's showing, a click on a bare corner picks the rug
        bool remove = self->tiles[i].removable && i == self->hover && PtInRect(&c, p);
        self->hover = -1;
        if (remove) {
            if (self->onRemove) self->onRemove(i);
        } else if (self->onClick) {
            self->onClick(i);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        int i = self->tileAt(p);
        if (i < 0 || !self->tiles[i].removable) return 0;
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, tr(L"Убрать", L"Remove"));
        ClientToScreen(h, &p);
        UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, h, nullptr);
        DestroyMenu(m);
        self->hover = -1;
        if (cmd == 1 && self->onRemove) self->onRemove(i);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// MARK: window

SettingsWindow::SettingsWindow(App* app_) : app(app_) {}

SettingsWindow::~SettingsWindow() {
    if (hwnd) DestroyWindow(hwnd);
    for (HFONT f : {bodyFont, headFont, captionFont})
        if (f) DeleteObject(f);
    if (bgBrush) DeleteObject(bgBrush);
}

HWND SettingsWindow::makeButton(int id, const wchar_t* text) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
                             (HMENU)(INT_PTR)id, app->inst, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)bodyFont, FALSE);
    themeControl(b);
    return b;
}

void SettingsWindow::create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = app->inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = app->icon(true);
        wc.hIconSm = app->icon(false);
        wc.lpszClassName = L"RugSettings";
        RegisterClassExW(&wc);
        registered = true;
    }
    pal = makePalette(systemDark());
    hwnd = CreateWindowExW(0, L"RugSettings", tr(L"Ковер", L"Rug"), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 600, nullptr, nullptr, app->inst, this);
    sc = GetDpiForWindow(hwnd) / 96.0f;
    makeFonts();

    rugStrip = std::make_unique<TileStrip>(hwnd, ID_RUGS, &thumbs, &pal);
    rugStrip->onClick = [this](int i) {
        const Tile& t = rugStrip->tiles[i];
        if (t.path.empty()) app->openRug(hwnd);
        else app->selectRug(t.path);
    };
    rugStrip->onRemove = [this](int i) {
        std::wstring path = rugStrip->tiles[i].path;
        thumbs.erase(path);
        app->removeRug(path);
    };
    floorStrip = std::make_unique<TileStrip>(hwnd, ID_FLOORS, &thumbs, &pal);
    floorStrip->onClick = [this](int i) {
        Floors::apply(floorStrip->tiles[i].path, app->rugScreen());
        refresh();
    };

    // a fixed thumb length makes the thumb wide enough for a round knob (it's half as wide as long)
    sizeTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH, 0, 0, 10,
                                10, hwnd, (HMENU)ID_SIZE, app->inst, nullptr);
    SendMessageW(sizeTrack, TBM_SETRANGE, TRUE, MAKELPARAM(25, 75));
    SendMessageW(sizeTrack, TBM_SETPAGESIZE, 0, 5);
    btnRestoreWall = makeButton(ID_RESTORE_WALL, tr(L"Вернуть прежние обои", L"Restore previous wallpaper"));
    btnSweep = makeButton(ID_SWEEP, tr(L"Замести под ковер", L"Sweep under the rug"));
    btnPutBack = makeButton(ID_PUTBACK, tr(L"Разложить обратно", L"Put them back"));
    btnCover = makeButton(ID_COVER, tr(L"Накрыть иконки ковром", L"Cover icons with the rug"));
    btnDrop = makeButton(ID_DROP, tr(L"Уронить заново", L"Drop again"));
    btnCenter = makeButton(ID_CENTER, tr(L"В центр", L"Center"));
    btnQuit = makeButton(ID_QUIT, tr(L"Выйти из Ковра", L"Quit Rug"));
    // the label is drawn by the window, a themed checkbox would keep black text in dark mode
    chkAutostart = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                   hwnd, (HMENU)ID_AUTOSTART, app->inst, nullptr);
    applyTheme();
}

void SettingsWindow::themeControl(HWND c) { SetWindowTheme(c, pal.dark ? L"DarkMode_Explorer" : nullptr, nullptr); }

void SettingsWindow::applyTheme() {
    pal = makePalette(systemDark());
    if (bgBrush) DeleteObject(bgBrush);
    bgBrush = CreateSolidBrush(pal.bg);
    BOOL dark = pal.dark;
    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE, 19 before Windows 10 20H1
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32];
        GetClassNameW(c, cls, 32);
        if (_wcsicmp(cls, L"Button") == 0) themeControl(c);
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

void SettingsWindow::makeFonts() {
    for (HFONT f : {bodyFont, headFont, captionFont})
        if (f) DeleteObject(f);
    UINT dpi = GetDpiForWindow(hwnd);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    bodyFont = CreateFontIndirectW(&lf);
    LOGFONTW hf = lf;
    hf.lfHeight = -MulDiv(10, dpi, 72);
    hf.lfWeight = FW_SEMIBOLD;
    headFont = CreateFontIndirectW(&hf);
    LOGFONTW cf = lf;
    cf.lfHeight = -MulDiv(8, dpi, 72);
    captionFont = CreateFontIndirectW(&cf);
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageW(c, WM_SETFONT, (WPARAM)bodyFont, TRUE);
}

// One button per screen when there are several; the one the rug is on is ticked and disabled.
void SettingsWindow::syncScreens() {
    std::vector<Screen> now = allScreens();
    if (now.size() < 2) now.clear();
    std::wstring here = app->controller ? app->controller->currentScreen().device : L"";
    bool same = now.size() == screens.size();
    for (size_t i = 0; same && i < now.size(); i++) same = now[i].device == screens[i].device && now[i].name == screens[i].name;
    if (!same) {
        for (HWND b : screenButtons) DestroyWindow(b);
        screenButtons.clear();
        screens = now;
        for (size_t i = 0; i < screens.size(); i++) screenButtons.push_back(makeButton(ID_SCREEN + int(i), L""));
    }
    for (size_t i = 0; i < screens.size(); i++) {
        bool current = screens[i].device == here;
        SetWindowTextW(screenButtons[i], ((current ? L"✓ " : L"") + screens[i].name).c_str());
        EnableWindow(screenButtons[i], !current);
    }
}

static const wchar_t* introText() {
    return tr(L"Тяни ковер мышью за любой край. Прокрутка над ковром поворачивает его, правый клик по ковру открывает "
              L"короткое меню. Окно можно закрыть, ковер останется на столе, а настройки откроются снова из значка в трее "
              L"или при повторном запуске Ковра.",
              L"Grab the rug by any edge and drag. Scroll over the rug to rotate it, right-click it for a quick menu. You "
              L"can close this window, the rug stays on the desktop, and settings open again from the tray icon or when "
              L"you start Rug again.");
}

static const wchar_t* warnText() {
    return tr(L"Сейчас Windows сам раскладывает иконки (включено «Упорядочить значки автоматически» или «Выровнять значки "
              L"по сетке»), поэтому они возвращаются на места. Правый клик по пустому месту рабочего стола, «Вид», снять обе "
              L"галочки. Или просто нажми «Замести под ковер», Ковер предложит сделать это сам.",
              L"Windows is arranging your icons (“Auto arrange icons” or “Align icons to grid” is on), so they snap back. "
              L"Right-click an empty spot on the desktop, open View and uncheck both. Or just press “Sweep under the "
              L"rug”, Rug offers to do it for you.");
}

static const wchar_t* autostartText() { return tr(L"Запускать вместе с Windows", L"Start with Windows"); }

void SettingsWindow::layout() {
    HDC dc = GetDC(hwnd);
    int pad = S(22), W = S(620), cw = W - 2 * pad, btnH = S(28), headH = S(20);
    auto textHeight = [&](HFONT f, const wchar_t* s) {
        RECT r = {0, 0, cw, 0};
        SelectObject(dc, f);
        DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        return int(r.bottom);
    };
    auto textWidth = [&](HFONT f, const wchar_t* s) {
        SIZE sz;
        SelectObject(dc, f);
        GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
        return int(sz.cx);
    };
    auto placeButton = [&](HWND b, int x, int y) {
        wchar_t text[128];
        GetWindowTextW(b, text, 128);
        int w = std::max<int>(S(90), textWidth(bodyFont, text) + S(32));
        MoveWindow(b, x, y, w, btnH, TRUE);
        return w;
    };
    int y = pad;
    introRc = {pad, y, pad + cw, y + textHeight(bodyFont, introText())};
    y = introRc.bottom + S(20);

    headRug = {pad, y, pad + cw, y + headH};
    y += headH + S(8);
    rugStrip->sc = sc;
    rugStrip->font = captionFont;
    int h = rugStrip->heightFor(cw);
    MoveWindow(rugStrip->hwnd, pad, y, cw, h, TRUE);
    y += h + S(20);

    headSize = {pad, y, pad + cw, y + headH};
    y += headH + S(8);
    int rowH = S(44);
    smallGlyph = {pad, y + (rowH - S(9)) / 2, pad + S(12), y + (rowH - S(9)) / 2 + S(9)};
    bigGlyph = {pad + cw - S(22), y + (rowH - S(16)) / 2, pad + cw, y + (rowH - S(16)) / 2 + S(16)};
    SendMessageW(sizeTrack, TBM_SETTHUMBLENGTH, S(40), 0);
    MoveWindow(sizeTrack, pad + S(20), y, cw - S(50), rowH, TRUE);
    y += rowH + S(20);

    y -= S(14);  // the slider row is tall for the knob, the visible part is in its middle
    headScreen = {};
    if (!screens.empty()) {
        headScreen = {pad, y, pad + cw, y + headH};
        y += headH + S(8);
        int x = pad;
        for (HWND b : screenButtons) {
            int w = placeButton(b, 0, 0);
            if (x > pad && x + w > pad + cw) {
                x = pad;
                y += btnH + S(8);
            }
            MoveWindow(b, x, y, w, btnH, TRUE);
            x += w + S(8);
        }
        y += btnH + S(20);
    }

    headFloor = {pad, y, pad + cw, y + headH};
    y += headH + S(8);
    floorStrip->sc = sc;
    floorStrip->font = captionFont;
    h = floorStrip->heightFor(cw);
    MoveWindow(floorStrip->hwnd, pad, y, cw, h, TRUE);
    y += h;
    ShowWindow(btnRestoreWall, hasPrevWallpaper ? SW_SHOW : SW_HIDE);
    if (hasPrevWallpaper) {
        y += S(8);
        placeButton(btnRestoreWall, pad, y);
        y += btnH;
    }
    y += S(20);

    headIcons = {pad, y, pad + cw, y + headH};
    y += headH + S(8);
    int x = pad;
    for (HWND b : {btnSweep, btnPutBack, btnCover}) x += placeButton(b, x, y) + S(8);
    y += btnH;
    warnRc = {};
    if (arranges) {
        y += S(8);
        warnRc = {pad, y, pad + cw, y + textHeight(bodyFont, warnText())};
        y = warnRc.bottom;
    }
    y += S(20);

    // bottom row: Drop again, Center ... [x] Start with Windows, Quit
    x = pad;
    for (HWND b : {btnDrop, btnCenter}) x += placeButton(b, x, y) + S(8);
    int qw = placeButton(btnQuit, 0, y);
    MoveWindow(btnQuit, pad + cw - qw, y, qw, btnH, TRUE);
    int box = S(20), labelW = textWidth(bodyFont, autostartText());
    int bx = pad + cw - qw - S(20) - labelW - S(6) - box;
    MoveWindow(chkAutostart, bx, y + (btnH - box) / 2, box, box, TRUE);
    autostartRc = {bx + box + S(6), y, bx + box + S(6) + labelW, y + btnH};
    y += btnH + pad;
    ReleaseDC(hwnd, dc);

    RECT r = {0, 0, W, y};
    AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, 0, GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, TRUE);
    InvalidateRect(rugStrip->hwnd, nullptr, FALSE);
    InvalidateRect(floorStrip->hwnd, nullptr, FALSE);
}

void SettingsWindow::refresh() {
    if (!hwnd) return;
    std::wstring current = app->currentRug();
    rugStrip->tiles.clear();
    auto addRugs = [&](const std::vector<std::wstring>& list, bool removable) {
        for (auto& p : list)
            rugStrip->tiles.push_back({p, displayName(p), _wcsicmp(p.c_str(), current.c_str()) == 0, removable});
    };
    addRugs(app->builtinRugs(), false);
    addRugs(app->userRugs(), true);
    rugStrip->tiles.push_back({L"", tr(L"Своя картинка", L"Your image"), false});

    std::wstring floor = Floors::currentName(app->rugScreen());
    floorStrip->tiles.clear();
    for (auto& p : Floors::all()) floorStrip->tiles.push_back({p, displayName(p), _wcsicmp(fileName(p).c_str(), floor.c_str()) == 0});

    float scale = app->controller ? app->controller->scale() : float(Prefs::getDouble(L"scale", 0.46));
    SendMessageW(sizeTrack, TBM_SETPOS, TRUE, LPARAM(scale * 100 + 0.5f));
    SendMessageW(chkAutostart, BM_SETCHECK, app->autostart() ? BST_CHECKED : BST_UNCHECKED, 0);
    hasPrevWallpaper = Floors::hasPrevious();
    arranges = IconPile::windowsArranges();
    EnableWindow(btnPutBack, IconPile::hasHome());
    syncScreens();
    layout();
}

void SettingsWindow::show() {
    bool first = !hwnd;
    if (first) create();
    refresh();
    if (first) {
        RECT r;
        GetWindowRect(hwnd, &r);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                     mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

void SettingsWindow::paint(HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, bodyFont);
    SetTextColor(dc, pal.secondary);
    DrawTextW(dc, introText(), -1, &introRc, DT_WORDBREAK | DT_NOPREFIX);
    SetTextColor(dc, pal.text);
    DrawTextW(dc, autostartText(), -1, &autostartRc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, headFont);
    DrawTextW(dc, tr(L"Ковер", L"Rug"), -1, &headRug, DT_SINGLELINE | DT_NOPREFIX);
    DrawTextW(dc, tr(L"Размер", L"Size"), -1, &headSize, DT_SINGLELINE | DT_NOPREFIX);
    if (!screens.empty()) DrawTextW(dc, tr(L"Экран", L"Screen"), -1, &headScreen, DT_SINGLELINE | DT_NOPREFIX);
    DrawTextW(dc, tr(L"Пол на рабочем столе", L"Desktop floor"), -1, &headFloor, DT_SINGLELINE | DT_NOPREFIX);
    DrawTextW(dc, tr(L"Иконки", L"Icons"), -1, &headIcons, DT_SINGLELINE | DT_NOPREFIX);
    if (arranges) {
        SelectObject(dc, bodyFont);
        SetTextColor(dc, pal.warning);
        DrawTextW(dc, warnText(), -1, &warnRc, DT_WORDBREAK | DT_NOPREFIX);
    }
    // small and large rug on both ends of the size slider
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(gdip(pal.secondary), 1.3f * sc);
    for (const RECT& r : {smallGlyph, bigGlyph}) {
        GraphicsPath p;
        roundRect(p, RectF(float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top)), 2 * sc);
        g.DrawPath(&pen, &p);
    }
}

// The size slider drawn like a Windows 11 one: a thin rail filled with the accent up to a round thumb.
LRESULT SettingsWindow::drawTrack(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    RECT thumb;
    SendMessageW(sizeTrack, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    float cy = (thumb.top + thumb.bottom) / 2.f, cx = (thumb.left + thumb.right) / 2.f;
    Graphics g(cd->hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        float h = 4 * sc, l = float(cd->rc.left), r = float(cd->rc.right);
        GraphicsPath rail, done;
        roundRect(rail, RectF(l, cy - h / 2, r - l, h), h / 2);
        roundRect(done, RectF(l, cy - h / 2, std::max(h, cx - l), h), h / 2);
        SolidBrush railBrush(gdip(pal.rail)), doneBrush(gdip(pal.accent));
        g.FillPath(&railBrush, &rail);
        g.FillPath(&doneBrush, &done);
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        float d = float(std::min(thumb.right - thumb.left, thumb.bottom - thumb.top)) - 1;
        SolidBrush fill(gdip(pal.thumbFill)), dot(gdip(pal.accent));
        Pen edge(gdip(pal.thumbEdge), 1);
        g.FillEllipse(&fill, cx - d / 2, cy - d / 2, d, d);
        g.DrawEllipse(&edge, cx - d / 2, cy - d / 2, d, d);
        float k = GetFocus() == sizeTrack ? 0.6f : 0.5f;
        g.FillEllipse(&dot, cx - d * k / 2, cy - d * k / 2, d * k, d * k);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

LRESULT CALLBACK SettingsWindow::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (SettingsWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && self->hwnd == h) return self->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT SettingsWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    RugController* c = app->controller.get();
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, bgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, pal.bg);
        SetTextColor((HDC)wp, pal.text);
        return (LRESULT)bgBrush;
    case WM_NOTIFY: {
        auto* n = (NMHDR*)lp;
        if (n->hwndFrom == sizeTrack && n->code == NM_CUSTOMDRAW) return drawTrack((NMCUSTOMDRAW*)lp);
        break;
    }
    case WM_HSCROLL:
        if ((HWND)lp == sizeTrack && LOWORD(wp) == TB_ENDTRACK && c)
            c->setScale(float(SendMessageW(sizeTrack, TBM_GETPOS, 0, 0)) / 100);
        return 0;
    case WM_LBUTTONUP: {
        // the checkbox label toggles it like a real one
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (PtInRect(&autostartRc, p)) SendMessageW(chkAutostart, BM_CLICK, 0, 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_SCREEN && id < ID_SCREEN + (int)screens.size()) {
            if (c) c->moveTo(screens[id - ID_SCREEN]);
            refresh();
            return 0;
        }
        switch (id) {
        case ID_RESTORE_WALL:
            Floors::restore();
            refresh();
            break;
        case ID_SWEEP:
            app->pileIcons(hwnd);
            refresh();
            break;
        case ID_PUTBACK:
            if (c) c->restoreIcons();
            refresh();
            break;
        case ID_COVER:
            if (c) c->coverIcons();
            break;
        case ID_DROP:
            if (c) c->redrop();
            break;
        case ID_CENTER:
            if (c) c->center();
            break;
        case ID_AUTOSTART:
            app->setAutostart(SendMessageW(chkAutostart, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case ID_QUIT:
            app->quit();
            break;
        case IDCANCEL:
            ShowWindow(hwnd, SW_HIDE);
            break;
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        // light or dark theme switched in Settings > Personalization
        if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) applyTheme();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        applyTheme();
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) refresh();
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DPICHANGED: {
        sc = HIWORD(wp) / 96.0f;
        makeFonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool SettingsWindow::shot(const std::wstring& path) {
    if (!hwnd) create();
    refresh();
    SetWindowPos(hwnd, nullptr, -20000, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC wdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    GdiFlush();
    uint8_t* px = (uint8_t*)bits;
    for (int i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
    bool ok = savePNG(path, px, w, h, w * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, wdc);
    ShowWindow(hwnd, SW_HIDE);
    return ok;
}
