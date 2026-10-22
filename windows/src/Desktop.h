#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <vector>

// A desktop icon: the icon picture in physical screen pixels.
struct DesktopIcon {
    RECT rect;
    std::wstring name;
};

struct PidlDeleter {
    void operator()(ITEMID_CHILD* p) const { CoTaskMemFree(p); }
};

// An item of the desktop folder with its position in list view coordinates (top left of the icon picture).
struct DesktopItem {
    std::wstring name;  // parsing name inside the folder, unique ("report.pdf", "::{645FF040-...}")
    POINT pos;
    std::unique_ptr<ITEMID_CHILD, PidlDeleter> pidl;
};

// Explorer's desktop view, found through IShellWindows the way Raymond Chen shows it.
// Gives the exact places of the icons (the stand-in for Finder's Accessibility tree) and lets us move them.
// Picked up again when Explorer restarts.
class DesktopView {
public:
    static DesktopView& shared();

    // false when there's no desktop to read (Explorer restarting, icons hidden)
    bool icons(std::vector<DesktopIcon>& out, int* iconPx);
    bool items(std::vector<DesktopItem>& out);
    bool move(const std::vector<const DesktopItem*>& which, const std::vector<POINT>& to);
    // screen position of the list view's client origin, icon positions are relative to it
    POINT listOrigin();
    int iconPixels();
    DWORD folderFlags();
    void setFolderFlags(DWORD mask, DWORD flags);

private:
    Microsoft::WRL::ComPtr<IShellView> view;
    Microsoft::WRL::ComPtr<IFolderView2> folder;
    Microsoft::WRL::ComPtr<IShellFolder> shellFolder;
    HWND list = nullptr;
    bool ready();
    void reset();
};
