#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

class RugController;
class SettingsWindow;

// What AppDelegate does on the Mac: tray icon and menus, the rug, the settings window.
class App {
public:
    explicit App(HINSTANCE inst);
    ~App();
    // At login (--autostart) the rug comes back quietly, otherwise settings open too.
    bool start(bool showSettingsNow);

    HINSTANCE inst;
    HWND msgWnd = nullptr;
    std::unique_ptr<RugController> controller;
    std::unique_ptr<SettingsWindow> settings;

    std::vector<std::wstring> builtinRugs() const;
    std::vector<std::wstring> userRugs() const;
    std::wstring currentRug() const;
    RECT rugScreen() const;  // the screen the rug is on, physical pixels
    HICON icon(bool big) const;

    // "Start with Windows": a Run entry that Task Manager or Settings > Startup apps hasn't switched off.
    bool autostart() const;
    void setAutostart(bool on);

    void selectRug(const std::wstring& path);
    void openRug(HWND owner);
    // One of the user's own rugs to the Recycle Bin (only Rug's copy, the original picture stays where it was).
    void removeRug(const std::wstring& path);
    bool isUserRug(const std::wstring& path) const;
    void pileIcons(HWND owner);
    void showSettings();
    void showMenu(POINT at);
    void quit();

private:
    int modal = 0;  // menus and dialogs running their own loop, the rug isn't rebuilt under them
    UINT taskbarCreated = 0;
    void addTrayIcon();
    void removeTrayIcon();
    bool createController();
    void runCommand(UINT cmd, const std::vector<std::wstring>& rugs, const std::vector<std::wstring>& floors);
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
};
