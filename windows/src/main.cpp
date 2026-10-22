#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <cstdio>
#include "App.h"
#include "Common.h"
#include "Demo.h"
#include "Floors.h"
#include "IconPile.h"
#include "RugController.h"
#include "Screens.h"
#include "Settings.h"
#include "Strings.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

enum {
    CMD_SETTINGS = 1,
    CMD_PILE,
    CMD_RESTORE_ICONS,
    CMD_REDROP,
    CMD_COVER,
    CMD_CENTER,
    CMD_OPEN_RUG,
    CMD_REMOVE_RUG,
    CMD_RESTORE_WALL,
    CMD_AUTOSTART,
    CMD_QUIT,
    CMD_SIZE = 100,
    CMD_RUG = 200,
    CMD_FLOOR = 400,
    CMD_SCREEN = 500,
};
enum { TIMER_REBUILD = 1 };

static const float kSizes[] = {0.32f, 0.46f, 0.62f};
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

static std::wstring autostartCommand() { return L"\"" + exePath() + L"\" --autostart"; }

App::App(HINSTANCE inst_) : inst(inst_) {}

App::~App() {
    settings.reset();
    controller.reset();
    if (msgWnd) DestroyWindow(msgWnd);
}

std::vector<std::wstring> App::builtinRugs() const {
    return listFiles(resourcesDir() + L"\\rugs", {L"jpg", L"jpeg", L"png"});
}

std::vector<std::wstring> App::userRugs() const {
    return listFiles(supportDir() + L"\\rugs", {L"jpg", L"jpeg", L"png", L"heic", L"webp"});
}

std::wstring App::currentRug() const {
    std::wstring p = Prefs::getString(L"texture");
    if (!p.empty() && fileExists(p)) return p;
    auto b = builtinRugs();
    return b.empty() ? L"" : b.front();
}

RECT App::rugScreen() const {
    return controller ? controller->currentScreen().rect : findScreen(Prefs::getString(L"screen")).rect;
}

bool App::autostart() const {
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Rug", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        return false;
    // the first byte is odd when the entry is switched off in Startup apps
    BYTE approved[12];
    DWORD n = sizeof(approved);
    return !(RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, L"Rug", RRF_RT_REG_BINARY, nullptr, approved, &n) ==
                 ERROR_SUCCESS &&
             n > 0 && (approved[0] & 1));
}

void App::setAutostart(bool on) {
    if (on) {
        std::wstring cmd = autostartCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Rug", REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, L"Rug");
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Rug");
    }
}

HICON App::icon(bool big) const {
    HICON h = nullptr;
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_RUG), big ? LIM_LARGE : LIM_SMALL, &h);
    return h;
}

bool App::createController() {
    std::wstring rug = currentRug();
    Screen screen = findScreen(Prefs::getString(L"screen"));
    for (int attempt = 0; attempt < 2; attempt++) {
        try {
            controller = std::make_unique<RugController>(inst, rug, screen);
            break;
        } catch (const std::exception& e) {
            rugLog(L"rug error " + utf8ToWide(e.what()));
            auto b = builtinRugs();
            // a broken custom picture shouldn't keep the rug away, try a built-in one once
            if (attempt == 0 && !b.empty() && rug != b.front()) {
                Prefs::remove(L"texture");
                rug = b.front();
                continue;
            }
            std::wstring msg = tr(L"Ковер не смог запуститься: ", L"Rug couldn't start: ") + utf8ToWide(e.what());
            if (rug.empty())
                msg = tr(L"Не найдена папка Resources рядом с Rug.exe.", L"The Resources folder next to Rug.exe is missing.");
            MessageBoxW(nullptr, msg.c_str(), tr(L"Ковер", L"Rug"), MB_ICONERROR);
            return false;
        }
    }
    controller->menuProvider = [this](POINT p) { showMenu(p); };
    controller->onDeviceLost = [this] { SetTimer(msgWnd, TIMER_REBUILD, 500, nullptr); };
    controller->onMoved = [this] {
        if (settings) settings->refresh();
    };
    return true;
}

bool App::start(bool showSettingsNow) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RugApp";
    RegisterClassExW(&wc);
    msgWnd = CreateWindowExW(0, L"RugApp", L"Rug", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, this);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Rug.exe moved since autostart was switched on: point the Run entry here
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Rug", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        wchar_t cur[MAX_PATH * 2] = L"";
        DWORD size = sizeof(cur);
        RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Rug", RRF_RT_REG_SZ, nullptr, cur, &size);
        std::wstring cmd = autostartCommand();
        if (_wcsicmp(cur, cmd.c_str()) != 0)
            RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Rug", REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    }
    if (!createController()) return false;
    addTrayIcon();
    settings = std::make_unique<SettingsWindow>(this);
    if (showSettingsNow) showSettings();
    return true;
}

void App::addTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon(false);
    wcscpy_s(nid.szTip, tr(L"Ковер", L"Rug"));
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void App::removeTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = msgWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void App::showSettings() {
    if (settings) settings->show();
}

void App::selectRug(const std::wstring& path) {
    if (controller && controller->setTexture(path)) Prefs::setString(L"texture", path);
    if (settings) settings->refresh();
}

void App::openRug(HWND owner) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC spec[] = {{tr(L"Картинки", L"Images"), L"*.jpg;*.jpeg;*.png;*.heic;*.webp"}};
    d->SetFileTypes(1, spec);
    d->SetTitle(tr(L"Картинка ковра, вид сверху, ковер во весь кадр", L"A top-down picture of a rug that fills the whole frame"));
    modal++;
    HRESULT hr = d->Show(owner);
    modal--;
    ComPtr<IShellItem> item;
    PWSTR p = nullptr;
    if (FAILED(hr) || FAILED(d->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return;
    std::wstring src = p;
    CoTaskMemFree(p);
    std::wstring dst = supportDir() + L"\\rugs\\" + fileName(src);
    if (_wcsicmp(src.c_str(), dst.c_str()) != 0 && !CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        rugLog(fmt(L"copy error %lu", GetLastError()));
        return;
    }
    selectRug(dst);
}

bool App::isUserRug(const std::wstring& path) const {
    std::wstring dir = supportDir() + L"\\rugs\\";
    return _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0;
}

void App::removeRug(const std::wstring& path) {
    if (!isUserRug(path) || !fileExists(path)) return;
    // the rug on the floor goes first, back to the first built-in one
    if (_wcsicmp(path.c_str(), currentRug().c_str()) == 0) {
        auto b = builtinRugs();
        if (!b.empty()) selectRug(b.front());
        Prefs::remove(L"texture");
    }
    ComPtr<IFileOperation> op;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
        SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOFX_RECYCLEONDELETE);
        op->DeleteItem(item.Get(), nullptr);
        op->PerformOperations();
    }
    if (fileExists(path)) rugLog(L"couldn't remove a rug");
    if (settings) settings->refresh();
}

void App::pileIcons(HWND owner) {
    if (!controller) return;
    modal++;
    bool ok = !IconPile::windowsArranges() || IconPile::askToStopArranging(owner);
    modal--;
    if (ok && controller) controller->pileIcons();
}

void App::quit() {
    if (controller) controller->savePose();
    removeTrayIcon();
    PostQuitMessage(0);
}

static void add(HMENU m, UINT id, const std::wstring& title, bool checked = false, bool enabled = true) {
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, title.c_str());
}

// The menu is built anew every time, so checkmarks and items are current.
void App::showMenu(POINT at) {
    std::vector<std::wstring> rugs = builtinRugs(), user = userRugs(), floors = Floors::all();
    rugs.insert(rugs.end(), user.begin(), user.end());
    HMENU m = CreatePopupMenu();
    add(m, CMD_SETTINGS, tr(L"Настройки…", L"Settings…"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(m, CMD_PILE, tr(L"Замести иконки под ковер", L"Sweep icons under the rug"));
    if (IconPile::hasHome()) add(m, CMD_RESTORE_ICONS, tr(L"Разложить иконки обратно", L"Put icons back"));
    add(m, CMD_REDROP, tr(L"Уронить заново", L"Drop again"));
    add(m, CMD_COVER, tr(L"Накрыть иконки", L"Cover icons"));
    add(m, CMD_CENTER, tr(L"Положить в центр", L"Center"));

    HMENU sizes = CreatePopupMenu();
    float cur = controller ? controller->scale() : 0.46f;
    const wchar_t* names[] = {tr(L"Маленький", L"Small"), tr(L"Средний", L"Medium"), tr(L"Большой", L"Large")};
    for (int i = 0; i < 3; i++) add(sizes, CMD_SIZE + i, names[i], std::abs(cur - kSizes[i]) < 0.01f);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sizes, tr(L"Размер", L"Size"));

    HMENU rugMenu = CreatePopupMenu();
    std::wstring current = currentRug();
    for (size_t i = 0; i < rugs.size(); i++)
        add(rugMenu, CMD_RUG + UINT(i), displayName(rugs[i]), _wcsicmp(rugs[i].c_str(), current.c_str()) == 0);
    AppendMenuW(rugMenu, MF_SEPARATOR, 0, nullptr);
    add(rugMenu, CMD_OPEN_RUG, tr(L"Свой ковер из картинки…", L"Rug from your image…"));
    if (isUserRug(current)) add(rugMenu, CMD_REMOVE_RUG, tr(L"Убрать этот ковер", L"Remove this rug"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rugMenu, tr(L"Ковер", L"Rug"));

    std::vector<Screen> screens = allScreens();
    if (screens.size() > 1) {
        HMENU screenMenu = CreatePopupMenu();
        std::wstring here = controller ? controller->currentScreen().device : L"";
        for (size_t i = 0; i < screens.size(); i++) add(screenMenu, CMD_SCREEN + UINT(i), screens[i].name, screens[i].device == here);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)screenMenu, tr(L"Экран", L"Screen"));
    }

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU floorMenu = CreatePopupMenu();
    std::wstring wall = Floors::currentName(rugScreen());
    for (size_t i = 0; i < floors.size(); i++)
        add(floorMenu, CMD_FLOOR + UINT(i), displayName(floors[i]), _wcsicmp(fileName(floors[i]).c_str(), wall.c_str()) == 0);
    if (Floors::hasPrevious()) {
        AppendMenuW(floorMenu, MF_SEPARATOR, 0, nullptr);
        add(floorMenu, CMD_RESTORE_WALL, tr(L"Вернуть прежние обои", L"Restore previous wallpaper"));
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)floorMenu, tr(L"Пол на рабочем столе", L"Desktop floor"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(m, CMD_AUTOSTART, tr(L"Запускать вместе с Windows", L"Start with Windows"), autostart());
    add(m, CMD_QUIT, tr(L"Выйти из Ковра", L"Quit Rug"));

    // the menu closes properly only when its owner is in front
    SetForegroundWindow(msgWnd);
    modal++;
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, msgWnd, nullptr);
    modal--;
    PostMessageW(msgWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd >= CMD_SCREEN && cmd < CMD_SCREEN + screens.size()) {
        if (controller) controller->moveTo(screens[cmd - CMD_SCREEN]);
    } else if (cmd) {
        runCommand(cmd, rugs, floors);
    }
}

void App::runCommand(UINT cmd, const std::vector<std::wstring>& rugs, const std::vector<std::wstring>& floors) {
    RugController* c = controller.get();
    if (cmd >= CMD_FLOOR && cmd < CMD_FLOOR + floors.size()) {
        Floors::apply(floors[cmd - CMD_FLOOR], rugScreen());
    } else if (cmd >= CMD_RUG && cmd < CMD_RUG + rugs.size()) {
        selectRug(rugs[cmd - CMD_RUG]);
    } else if (cmd >= CMD_SIZE && cmd < CMD_SIZE + 3) {
        if (c) c->setScale(kSizes[cmd - CMD_SIZE]);
    } else {
        switch (cmd) {
        case CMD_SETTINGS: showSettings(); break;
        case CMD_PILE: pileIcons(nullptr); break;
        case CMD_RESTORE_ICONS: if (c) c->restoreIcons(); break;
        case CMD_REDROP: if (c) c->redrop(); break;
        case CMD_COVER: if (c) c->coverIcons(); break;
        case CMD_CENTER: if (c) c->center(); break;
        case CMD_OPEN_RUG: openRug(nullptr); break;
        case CMD_REMOVE_RUG: removeRug(currentRug()); break;
        case CMD_RESTORE_WALL: Floors::restore(); break;
        case CMD_AUTOSTART: setAutostart(!autostart()); break;
        case CMD_QUIT: quit(); return;
        }
    }
    if (settings) settings->refresh();
}

LRESULT CALLBACK App::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    App* self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    if (msg == self->taskbarCreated && msg != 0) {
        // Explorer restarted
        self->addTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            self->showSettings();
            break;
        case WM_CONTEXTMENU:
            self->showMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            break;
        }
        return 0;
    case WM_APP_SHOW_SETTINGS:
        self->showSettings();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REBUILD) {
            // the GPU was reset or removed: a new rug with a new device, unless a menu is open over the old one
            if (self->modal > 0) return 0;
            KillTimer(h, TIMER_REBUILD);
            if (self->controller) self->controller->savePose();
            self->controller.reset();
            if (!self->createController()) PostQuitMessage(1);
            if (self->settings) self->settings->refresh();
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp && self->controller) self->controller->savePose();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Console output for the command line modes of a GUI program. Output redirected to a file or a pipe
// already works, otherwise it goes to the console Rug was started from.
static void attachConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv, argv + argc);
    LocalFree(argv);
    auto argAt = [&](const wchar_t* flag, int offset) -> std::wstring {
        for (size_t i = 0; i < args.size(); i++)
            if (args[i] == flag) return i + offset < args.size() ? args[i + offset] : L"";
        return L"";
    };
    auto hasArg = [&](const wchar_t* flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    if (hasArg(L"--demo")) {
        attachConsole();
        std::wstring out = argAt(L"--demo", 1);
        return Demo::run(out.empty() ? L"demo" : out, argAt(L"--demo", 2));
    }
    if (hasArg(L"--floor")) {
        std::wstring q = argAt(L"--floor", 1);
        if (q == L"restore") Floors::restore();
        else
            for (auto& f : Floors::all())
                if (!q.empty() && fileName(f).find(q) != std::wstring::npos) {
                    Floors::apply(f, findScreen(Prefs::getString(L"screen")).rect);
                    break;
                }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
    int code = 0;

    if (hasArg(L"--settings-shot")) {
        // picture of the settings window, to check the layout
        App app(inst);
        SettingsWindow s(&app);
        code = s.shot(argAt(L"--settings-shot", 1)) ? 0 : 1;
    } else {
        // One copy at a time. Starting Rug again opens the settings of the running one.
        HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\RugSingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND other = FindWindowW(L"RugApp", nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_APP_SHOW_SETTINGS, 0, 0);
            }
        } else {
            App app(inst);
            if (app.start(!hasArg(L"--autostart"))) {
                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    HWND s = app.settings ? app.settings->window() : nullptr;
                    if (s && IsDialogMessageW(s, &msg)) continue;
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            } else {
                code = 1;
            }
        }
        if (mutex) CloseHandle(mutex);
    }
    Gdiplus::GdiplusShutdown(gdiplus);
    CoUninitialize();
    return code;
}
