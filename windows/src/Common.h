#pragma once
#include <windows.h>
#include <shtypes.h>
#include <cstdint>
#include <string>
#include <vector>

// Messages shared between windows.
enum : UINT {
    WM_APP_FRAME = WM_APP + 1,       // pacer thread asks the rug for the next frame
    WM_APP_DESKTOP_CHANGED,          // a file appeared or vanished on the desktop
    WM_APP_TRAY,                     // tray icon callback
    WM_APP_SHOW_SETTINGS,            // second launch asks the running copy for settings
    WM_APP_REBUILD,                  // screen or GPU changed, the rug window is recreated
};

// Verbose log (file names, timings) only when debugging: RUG_DEBUG=1 or HKCU\Software\Rug debug=1.
bool rugDebugOn();
void rugLog(const std::wstring& s);
#define rugDebug(expr) do { if (rugDebugOn()) rugLog(expr); } while (0)

// Settings in HKCU\Software\Rug, the stand-in for UserDefaults.
namespace Prefs {
bool has(const wchar_t* key);
std::wstring getString(const wchar_t* key, const std::wstring& def = L"");
void setString(const wchar_t* key, const std::wstring& v);
double getDouble(const wchar_t* key, double def);
void setDouble(const wchar_t* key, double v);
void remove(const wchar_t* key);
}

std::wstring exePath();
std::wstring exeDir();
std::wstring resourcesDir();           // Resources next to Rug.exe
std::wstring supportDir();             // %APPDATA%\Rug
std::wstring knownFolder(REFKNOWNFOLDERID id);
bool fileExists(const std::wstring& p);
bool dirExists(const std::wstring& p);
std::wstring fileName(const std::wstring& p);
std::wstring extensionLower(const std::wstring& p);
// Files in dir with one of the extensions (lowercase, no dot), sorted by name.
std::vector<std::wstring> listFiles(const std::wstring& dir, const std::vector<std::wstring>& exts);
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
std::wstring fmt(const wchar_t* f, ...);

double nowSeconds();

// Writes 32-bit BGRA pixels to a PNG with WIC.
bool savePNG(const std::wstring& path, const uint8_t* bgra, int w, int h, int stride);
