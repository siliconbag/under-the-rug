#pragma once
#include <windows.h>

// A pile of icons under the rug. Icons are moved through Explorer's desktop view,
// which only sticks when Windows doesn't arrange them itself (auto arrange and align to grid are off).
namespace IconPile {
bool windowsArranges();
bool hasHome();
// Explains that Windows arranges the icons and offers to switch that off. True when it's off now.
bool askToStopArranging(HWND owner);
// Sweeps every icon into a heap around a screen point (physical pixels) and remembers where they were.
void pile(POINT center, float dipScale);
void restore();
}
