#include "Floors.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include "Common.h"

using Microsoft::WRL::ComPtr;

namespace Floors {

std::vector<std::wstring> all() { return listFiles(resourcesDir() + L"\\floors", {L"jpg"}); }

static std::wstring dir() {
    std::wstring d = supportDir() + L"\\floors";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static ComPtr<IDesktopWallpaper> wallpaper() {
    ComPtr<IDesktopWallpaper> w;
    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&w));
    return w;
}

// Wallpaper monitor id of the screen with this rectangle.
static std::wstring monitorId(IDesktopWallpaper* w, const RECT& screen) {
    UINT n = 0;
    w->GetMonitorDevicePathCount(&n);
    for (UINT i = 0; i < n; i++) {
        LPWSTR id = nullptr;
        RECT r;
        if (FAILED(w->GetMonitorDevicePathAt(i, &id))) continue;
        std::wstring s = id;
        CoTaskMemFree(id);
        if (SUCCEEDED(w->GetMonitorRECT(s.c_str(), &r)) && EqualRect(&r, &screen)) return s;
    }
    return L"";
}

static std::wstring current(IDesktopWallpaper* w, const std::wstring& mon) {
    LPWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(w->GetWallpaper(mon.empty() ? nullptr : mon.c_str(), &p)) && p) s = p;
    CoTaskMemFree(p);
    return s;
}

void apply(const std::wstring& src, const RECT& screen) {
    ComPtr<IDesktopWallpaper> w = wallpaper();
    if (!w) return;
    std::wstring dst = dir() + L"\\" + fileName(src);
    CopyFileW(src.c_str(), dst.c_str(), FALSE);
    std::wstring mon = monitorId(w.Get(), screen);
    std::wstring prev = current(w.Get(), mon);
    if (!Prefs::has(L"prevWallpaper") && !prev.empty() && _wcsnicmp(prev.c_str(), dir().c_str(), dir().size()) != 0) {
        DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
        w->GetPosition(&pos);
        Prefs::setString(L"prevWallpaper", prev);
        Prefs::setString(L"prevWallpaperMonitor", mon);
        Prefs::setDouble(L"prevWallpaperPosition", pos);
    }
    HRESULT hr = w->SetWallpaper(mon.empty() ? nullptr : mon.c_str(), dst.c_str());
    if (FAILED(hr)) rugLog(fmt(L"wallpaper error 0x%08lx", hr));
    w->SetPosition(DWPOS_FILL);
}

void restore() {
    ComPtr<IDesktopWallpaper> w = wallpaper();
    std::wstring p = Prefs::getString(L"prevWallpaper");
    if (!w || p.empty()) return;
    std::wstring mon = Prefs::getString(L"prevWallpaperMonitor");
    w->SetWallpaper(mon.empty() ? nullptr : mon.c_str(), p.c_str());
    w->SetPosition((DESKTOP_WALLPAPER_POSITION)(int)Prefs::getDouble(L"prevWallpaperPosition", DWPOS_FILL));
    Prefs::remove(L"prevWallpaper");
    Prefs::remove(L"prevWallpaperMonitor");
    Prefs::remove(L"prevWallpaperPosition");
}

bool hasPrevious() { return Prefs::has(L"prevWallpaper"); }

std::wstring currentName(const RECT& screen) {
    ComPtr<IDesktopWallpaper> w = wallpaper();
    if (!w) return L"";
    return fileName(current(w.Get(), monitorId(w.Get(), screen)));
}

}  // namespace Floors
