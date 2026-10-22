#include "IconPile.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <sstream>
#include "Common.h"
#include "Desktop.h"
#include "Strings.h"

#pragma comment(lib, "comctl32.lib")

namespace IconPile {

static const DWORD kArrangeFlags = FWF_AUTOARRANGE | FWF_SNAPTOGRID;

bool windowsArranges() { return (DesktopView::shared().folderFlags() & kArrangeFlags) != 0; }

bool hasHome() { return Prefs::has(L"iconHome"); }

bool askToStopArranging(HWND owner) {
    TASKDIALOG_BUTTON buttons[] = {{100, tr(L"Выключить и замести", L"Switch off and sweep")}};
    TASKDIALOGCONFIG c = {sizeof(c)};
    c.hwndParent = owner;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = tr(L"Ковер", L"Rug");
    c.pszMainInstruction = tr(L"Windows сам раскладывает иконки", L"Windows is arranging your icons");
    c.pszContent = tr(L"Сейчас включено «Упорядочить значки автоматически» или «Выровнять значки по сетке», поэтому "
                      L"Windows возвращает иконки на места. Ковер может выключить оба пункта сейчас и включить их "
                      L"обратно, когда иконки вернутся на места.",
                      L"“Auto arrange icons” or “Align icons to grid” is on, so Windows snaps icons back into place. "
                      L"Rug can switch both off now and switch them back on when you put the icons back.");
    c.cButtons = ARRAYSIZE(buttons);
    c.pButtons = buttons;
    c.nDefaultButton = 100;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr)) || pressed != 100) return false;
    DesktopView& d = DesktopView::shared();
    if (!Prefs::has(L"savedFolderFlags")) Prefs::setDouble(L"savedFolderFlags", d.folderFlags() & kArrangeFlags);
    d.setFolderFlags(kArrangeFlags, 0);
    return !windowsArranges();
}

void pile(POINT center, float dipScale) {
    std::vector<DesktopItem> items;
    if (!DesktopView::shared().items(items) || items.empty()) return;
    std::sort(items.begin(), items.end(), [](const DesktopItem& a, const DesktopItem& b) { return a.name < b.name; });
    if (!hasHome()) {
        // one line per icon: x,y,name
        std::wstring s;
        for (auto& it : items) s += fmt(L"%ld,%ld,", it.pos.x, it.pos.y) + it.name + L"\n";
        Prefs::setString(L"iconHome", s);
    }
    POINT o = DesktopView::shared().listOrigin();
    int half = DesktopView::shared().iconPixels() / 2;
    double cx = center.x - o.x - half, cy = center.y - o.y - half;
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(0, 8);
    std::vector<const DesktopItem*> which;
    std::vector<POINT> to;
    for (size_t k = 0; k < items.size(); k++) {
        double a = double(k) * 2.39996;
        double r = (13 * std::sqrt(double(k)) + jitter(rng)) * dipScale;
        which.push_back(&items[k]);
        to.push_back({LONG(cx + r * std::cos(a)), LONG(cy + r * std::sin(a) * 0.8)});
    }
    DesktopView::shared().move(which, to);
}

void restore() {
    std::map<std::wstring, POINT> home;
    std::wistringstream lines(Prefs::getString(L"iconHome"));
    std::wstring line;
    while (std::getline(lines, line)) {
        size_t a = line.find(L','), b = a == std::wstring::npos ? a : line.find(L',', a + 1);
        if (b == std::wstring::npos) continue;
        home[line.substr(b + 1)] = {_wtol(line.substr(0, a).c_str()), _wtol(line.substr(a + 1, b - a - 1).c_str())};
    }
    std::vector<DesktopItem> items;
    if (DesktopView::shared().items(items)) {
        std::vector<const DesktopItem*> which;
        std::vector<POINT> to;
        for (auto& it : items) {
            auto h = home.find(it.name);
            if (h == home.end()) continue;
            which.push_back(&it);
            to.push_back(h->second);
        }
        DesktopView::shared().move(which, to);
    }
    Prefs::remove(L"iconHome");
    if (Prefs::has(L"savedFolderFlags")) {
        DesktopView::shared().setFolderFlags(kArrangeFlags, (DWORD)Prefs::getDouble(L"savedFolderFlags", 0));
        Prefs::remove(L"savedFolderFlags");
    }
}

}  // namespace IconPile
