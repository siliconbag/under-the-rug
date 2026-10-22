#include "Common.h"
#include <knownfolders.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdarg>
#include <cwchar>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

static const wchar_t* kRegKey = L"Software\\Rug";

bool rugDebugOn() {
    static int on = -1;
    if (on < 0) on = (GetEnvironmentVariableW(L"RUG_DEBUG", nullptr, 0) > 0 || Prefs::getDouble(L"debug", 0) != 0) ? 1 : 0;
    return on == 1;
}

void rugLog(const std::wstring& s) {
    std::wstring dir = knownFolder(FOLDERID_LocalAppData) + L"\\Rug";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW((dir + L"\\Rug.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string line = wideToUtf8(fmt(L"%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                                      t.wSecond) + s + L"\r\n");
    DWORD n;
    WriteFile(h, line.data(), (DWORD)line.size(), &n, nullptr);
    CloseHandle(h);
}

// MARK: prefs

namespace Prefs {
bool has(const wchar_t* key) {
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, key, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

std::wstring getString(const wchar_t* key, const std::wstring& def) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, key, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return def;
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, key, RRF_RT_REG_SZ, nullptr, s.data(), &size) != ERROR_SUCCESS) return def;
    s.resize(wcslen(s.c_str()));
    return s;
}

void setString(const wchar_t* key, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, key, REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

double getDouble(const wchar_t* key, double def) {
    std::wstring s = getString(key);
    return s.empty() ? def : _wtof(s.c_str());
}

void setDouble(const wchar_t* key, double v) { setString(key, fmt(L"%.6f", v)); }

void remove(const wchar_t* key) { RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegKey, key); }
}  // namespace Prefs

// MARK: paths

std::wstring exePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring exeDir() {
    std::wstring p = exePath();
    return p.substr(0, p.find_last_of(L"\\/"));
}

std::wstring resourcesDir() {
    std::wstring d = exeDir() + L"\\Resources";
    if (dirExists(d + L"\\rugs")) return d;
    // running straight from windows\build\Rug during development: the repo's own Resources
    for (const wchar_t* up : {L"\\..\\..\\..\\Resources", L"\\..\\..\\Resources"}) {
        std::wstring dev = exeDir() + up;
        if (dirExists(dev + L"\\rugs")) return dev;
    }
    return d;
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring supportDir() {
    std::wstring d = knownFolder(FOLDERID_RoamingAppData) + L"\\Rug";
    CreateDirectoryW(d.c_str(), nullptr);
    CreateDirectoryW((d + L"\\rugs").c_str(), nullptr);
    return d;
}

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring fileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring extensionLower(const std::wstring& p) {
    std::wstring n = fileName(p);
    size_t i = n.find_last_of(L'.');
    if (i == std::wstring::npos) return L"";
    std::wstring e = n.substr(i + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    return e;
}

std::vector<std::wstring> listFiles(const std::wstring& dir, const std::vector<std::wstring>& exts) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (std::find(exts.begin(), exts.end(), extensionLower(p)) != exts.end()) out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return fileName(a) < fileName(b); });
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), u.data(), n, nullptr, nullptr);
    return u;
}

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[2048];
    va_list a;
    va_start(a, f);
    _vsnwprintf_s(buf, _TRUNCATE, f, a);
    va_end(a);
    return buf;
}

double nowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

bool savePNG(const std::wstring& path, const uint8_t* bgra, int w, int h, int stride) {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    frame->SetSize(w, h);
    frame->SetPixelFormat(&pf);
    if (FAILED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgra)))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}
