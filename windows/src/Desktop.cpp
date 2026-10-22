#include "Desktop.h"
#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include "Common.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

DesktopView& DesktopView::shared() {
    static DesktopView v;
    return v;
}

void DesktopView::reset() {
    view.Reset();
    folder.Reset();
    shellFolder.Reset();
    list = nullptr;
}

bool DesktopView::ready() {
    if (folder && IsWindow(list)) {
        DWORD f;
        if (SUCCEEDED(folder->GetCurrentFolderFlags(&f))) return true;
    }
    reset();
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return false;
    VARIANT loc;
    VariantInit(&loc);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    VARIANT empty;
    VariantInit(&empty);
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    if (windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) return false;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    if (FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) return false;
    if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folder))) {
        reset();
        return false;
    }
    folder->GetFolder(IID_PPV_ARGS(&shellFolder));
    HWND defView = nullptr;
    view->GetWindow(&defView);
    list = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    if (!list) {
        reset();
        return false;
    }
    return true;
}

POINT DesktopView::listOrigin() {
    POINT o = {0, 0};
    if (ready()) ClientToScreen(list, &o);
    return o;
}

// The view reports the icon size for 96 dpi, positions are in physical pixels.
int DesktopView::iconPixels() {
    FOLDERVIEWMODE mode;
    int size = 48;
    if (ready()) folder->GetViewModeAndIconSize(&mode, &size);
    return MulDiv(size, ready() ? GetDpiForWindow(list) : 96, 96);
}

DWORD DesktopView::folderFlags() {
    DWORD f = 0;
    if (ready()) folder->GetCurrentFolderFlags(&f);
    return f;
}

void DesktopView::setFolderFlags(DWORD mask, DWORD flags) {
    if (ready()) folder->SetCurrentFolderFlags(mask, flags);
}

bool DesktopView::items(std::vector<DesktopItem>& out) {
    out.clear();
    if (!ready()) return false;
    ComPtr<IEnumIDList> en;
    if (FAILED(folder->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&en))) || !en) return false;
    PITEMID_CHILD pidl;
    while (en->Next(1, &pidl, nullptr) == S_OK) {
        DesktopItem it;
        it.pidl.reset((ITEMID_CHILD*)pidl);
        if (FAILED(folder->GetItemPosition(pidl, &it.pos))) continue;
        STRRET s;
        PWSTR name = nullptr;
        if (shellFolder && SUCCEEDED(shellFolder->GetDisplayNameOf(pidl, SHGDN_INFOLDER | SHGDN_FORPARSING, &s)) &&
            SUCCEEDED(StrRetToStrW(&s, pidl, &name))) {
            it.name = name;
            CoTaskMemFree(name);
        }
        out.push_back(std::move(it));
    }
    return true;
}

bool DesktopView::icons(std::vector<DesktopIcon>& out, int* iconPx) {
    out.clear();
    std::vector<DesktopItem> all;
    if (!items(all)) return false;
    // "Show desktop icons" is off: nothing under the rug
    if (!IsWindowVisible(list)) return true;
    POINT o = listOrigin();
    int px = iconPixels();
    if (iconPx) *iconPx = px;
    for (auto& it : all) {
        RECT r = {o.x + it.pos.x, o.y + it.pos.y, o.x + it.pos.x + px, o.y + it.pos.y + px};
        out.push_back({r, it.name});
    }
    return true;
}

bool DesktopView::move(const std::vector<const DesktopItem*>& which, const std::vector<POINT>& to) {
    if (which.empty() || !ready()) return false;
    std::vector<PCITEMID_CHILD> pidls;
    for (auto* it : which) pidls.push_back(it->pidl.get());
    HRESULT hr = folder->SelectAndPositionItems((UINT)pidls.size(), pidls.data(), const_cast<POINT*>(to.data()),
                                                SVSI_POSITIONITEM);
    if (FAILED(hr)) rugLog(fmt(L"move icons error 0x%08lx", hr));
    return SUCCEEDED(hr);
}
