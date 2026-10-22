#include "Screens.h"
#include <shellscalingapi.h>
#include <algorithm>
#include <map>
#include "Common.h"
#include "Strings.h"

#pragma comment(lib, "shcore.lib")

struct DisplayInfo {
    std::wstring name;
    double refresh = 0;
};

// Monitor names and exact refresh rates from the display configuration, by GDI device name.
static std::map<std::wstring, DisplayInfo> displayInfo() {
    std::map<std::wstring, DisplayInfo> out;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return out;
    for (UINT32 i = 0; i < np; i++) {
        const DISPLAYCONFIG_PATH_INFO& p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = p.targetInfo.adapterId;
        tgt.header.id = p.targetInfo.id;
        DisplayInfo d;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) {
            auto t = tgt.outputTechnology;
            bool builtin = t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                           t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                           t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
            d.name = builtin ? tr(L"Встроенный экран", L"Built-in display") : tgt.monitorFriendlyDeviceName;
        }
        DISPLAYCONFIG_RATIONAL r = p.targetInfo.refreshRate;
        if (r.Denominator) d.refresh = double(r.Numerator) / r.Denominator;
        out[src.viewGdiDeviceName] = d;
    }
    return out;
}

std::vector<Screen> allScreens() {
    std::vector<Screen> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(m, &mi)) return TRUE;
            Screen s;
            s.handle = m;
            s.device = mi.szDevice;
            s.rect = mi.rcMonitor;
            s.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            UINT dx = 96, dy = 96;
            GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy);
            s.scale = dx / 96.0f;
            ((std::vector<Screen>*)p)->push_back(s);
            return TRUE;
        },
        (LPARAM)&out);
    std::sort(out.begin(), out.end(), [](const Screen& a, const Screen& b) {
        if (a.primary != b.primary) return a.primary;
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    });
    auto info = displayInfo();
    for (size_t i = 0; i < out.size(); i++) {
        Screen& s = out[i];
        auto it = info.find(s.device);
        if (it != info.end()) {
            s.name = it->second.name;
            s.refresh = it->second.refresh;
        }
        if (s.refresh < 1) {
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            s.refresh = EnumDisplaySettingsW(s.device.c_str(), ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
                            ? dm.dmDisplayFrequency
                            : 60;
        }
        if (s.name.empty()) s.name = fmt(tr(L"Экран %d", L"Screen %d"), int(i + 1));
    }
    return out;
}

Screen findScreen(const std::wstring& device) {
    auto all = allScreens();
    for (auto& s : all)
        if (s.device == device) return s;
    return all.empty() ? Screen() : all.front();
}

Screen screenAt(POINT p) {
    HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    auto all = allScreens();
    for (auto& s : all)
        if (s.handle == m) return s;
    return all.empty() ? Screen() : all.front();
}
