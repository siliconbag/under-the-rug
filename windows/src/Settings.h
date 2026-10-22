#pragma once
#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Screens.h"

class App;
namespace Gdiplus { class Bitmap; }

// Colours of the settings window, light or dark like the Windows app theme.
struct Palette {
    bool dark = false;
    COLORREF bg, text, secondary, warning, accent, rail, thumbFill, thumbEdge;
    uint32_t tileEdge, dashed, plus;  // ARGB for GDI+
};

struct Tile {
    std::wstring path;  // empty for the "your image" tile
    std::wstring caption;
    bool selected = false;
    bool removable = false;  // the user's own pictures get a × on hover
};

// A wrapping row of picture tiles (rugs, floors).
class TileStrip {
public:
    TileStrip(HWND parent, int id, std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>>* thumbs, const Palette* pal);
    HWND hwnd = nullptr;
    std::vector<Tile> tiles;
    std::function<void(int)> onClick;
    std::function<void(int)> onRemove;
    float sc = 1;
    HFONT font = nullptr;
    int heightFor(int width) const;

private:
    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>>* thumbs;
    const Palette* pal;
    int hover = -1;
    bool tracking = false;
    int columns(int width) const;
    RECT tileRect(int i, int width) const;
    RECT closeRect(int i, int width) const;
    int tileAt(POINT p) const;
    void paint(HDC dc, int w, int h);
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
};

// The settings window: rug, size, screen, floor, icons. Opens at launch and whenever Rug is started again.
class SettingsWindow {
public:
    explicit SettingsWindow(App* app);
    ~SettingsWindow();
    void show();
    void refresh();
    HWND window() const { return hwnd; }
    // Renders the window to a PNG to check the layout (--settings-shot).
    bool shot(const std::wstring& path);

private:
    App* app;
    HWND hwnd = nullptr;
    float sc = 1;
    Palette pal;
    HBRUSH bgBrush = nullptr;
    HFONT bodyFont = nullptr, headFont = nullptr, captionFont = nullptr;
    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> thumbs;
    std::unique_ptr<TileStrip> rugStrip, floorStrip;
    HWND sizeTrack = nullptr, btnRestoreWall = nullptr, btnSweep = nullptr, btnPutBack = nullptr, btnCover = nullptr,
         btnDrop = nullptr, btnCenter = nullptr, btnQuit = nullptr, chkAutostart = nullptr;
    std::vector<Screen> screens;
    std::vector<HWND> screenButtons;
    RECT introRc{}, headRug{}, headSize{}, headScreen{}, headFloor{}, headIcons{}, warnRc{}, smallGlyph{}, bigGlyph{},
        autostartRc{};
    bool hasPrevWallpaper = false, arranges = false;

    void create();
    HWND makeButton(int id, const wchar_t* text);
    void makeFonts();
    void applyTheme();
    void themeControl(HWND c);
    void syncScreens();
    void layout();
    int S(float v) const { return int(v * sc + 0.5f); }
    void paint(HDC dc);
    LRESULT drawTrack(NMCUSTOMDRAW* cd);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
};
