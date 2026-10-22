#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Floor wallpapers. Copied to %APPDATA%\Rug\floors so the wallpaper path doesn't depend on where Rug.exe lives.
// They go on the screen the rug is on (its rectangle in physical pixels).
namespace Floors {
std::vector<std::wstring> all();
void apply(const std::wstring& src, const RECT& screen);
void restore();
bool hasPrevious();
// File name of the current wallpaper on that screen.
std::wstring currentName(const RECT& screen);
}
