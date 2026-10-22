#pragma once
#include <windows.h>
#include <string>
#include <vector>

// A monitor as the rug sees it.
struct Screen {
    HMONITOR handle = nullptr;
    std::wstring device;  // \\.\DISPLAY1, stays the same while the monitor is connected
    std::wstring name;    // "DELL U2720Q", "Built-in display"
    RECT rect{};          // physical pixels
    float scale = 1;      // physical pixels per point
    double refresh = 60;  // Hz
    bool primary = false;
};

// Primary first, then left to right.
std::vector<Screen> allScreens();
// The screen with this device name, the primary one if it's gone.
Screen findScreen(const std::wstring& device);
// The screen under a point (physical pixels), or the nearest one.
Screen screenAt(POINT p);
