#pragma once
#include <windows.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "Cloth.h"
#include "HeightField.h"
#include "Renderer.h"
#include "Screens.h"

// The rug window: transparent, over the desktop icons but under every other window,
// click-through everywhere except over the rug itself. It lives on one screen and moves between them.
class RugController {
public:
    // Throws std::runtime_error when Direct3D or the window can't be set up.
    RugController(HINSTANCE inst, const std::wstring& texturePath, const Screen& screen);
    ~RugController();

    std::function<void(POINT)> menuProvider;  // context menu at a screen point
    std::function<void()> onDeviceLost;       // the GPU went away, the app rebuilds the rug
    std::function<void()> onMoved;            // the rug went to another screen

    HWND window() const { return hwnd; }
    const Screen& currentScreen() const { return screen; }
    float scale() const;
    void setScale(float s);
    void savePose();
    void redrop();
    void center();
    bool setTexture(const std::wstring& path);
    void pileIcons();
    void restoreIcons();
    void coverIcons();
    // Moves the rug to a screen. With dropAt (physical pixels) it falls there, otherwise it keeps its relative place.
    void moveTo(const Screen& s, const POINT* dropAt = nullptr);

private:
    HWND hwnd = nullptr;
    Screen screen;
    RECT frame;   // the screen, physical pixels
    float sc;     // physical pixels per point
    Camera camera;
    std::unique_ptr<HeightField> field;
    std::unique_ptr<Cloth> cloth;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    std::unique_ptr<Renderer> renderer;

    std::vector<Bump> lastBumps;
    bool dragging = false;
    bool ignoresMouse = true;
    bool awake = false;
    double calmTime = 0;
    const float liftHeight = 120;

    // timings for RUG_DEBUG, logged once a second while the rug moves
    struct Stats {
        int frames = 0, hitches = 0;
        double start = 0, last = 0, maxGap = 0;
        double sim = 0, render = 0, present = 0, maxPresent = 0;
        double gpu = 0, maxGpu = 0;
        int gpuFrames = 0;
        double lag = 0, maxLag = 0;  // grabbed point behind the cursor, points
        int lagFrames = 0;
        UINT presents = 0, refreshes = 0;  // from DXGI frame statistics: on-screen frames vs. screen refreshes
        UINT lastPresent = 0, lastRefresh = 0;
    } stats;
    struct GpuTimer {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    } gpuTimers[4];
    int gpuNext = 0;
    double benchStart = 0;  // RUG_BENCH: a scripted fast drag to measure smoothness
    void logStats(double now);
    void collectGpuTimes();

    HANDLE awakeEvent = nullptr, stopEvent = nullptr;
    std::atomic<bool> framePending{false};
    std::thread pacer, watcher;
    std::mutex paceMutex;                 // guards the two below, the pacer thread reads them
    ComPtr<IDXGIOutput> vblankOutput;     // the screen's output, frames follow its vertical blank
    double framePeriod = 1.0 / 60;

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void applyScreen();
    void buildCloth(float drop, bool keepPose);
    void layoutCloth(float drop, float2 center, float angle);
    bool savedPose(float2& center, float& angle);
    void wake();
    void setAwake(bool a);
    bool trimmed = false;  // render targets dropped after a long sleep
    void trim();
    void warmUp();
    void drawFrame();
    void present();

    float2 toPoints(int x, int y) const;
    float2 cursorPoints() const;
    void setIgnoresMouse(bool v);
    void pollHover();
    void keepAboveDesktop();
    void mouseDown(float2 s);
    void mouseDragged(float2 s);
    void mouseUp();

    void rescan();
    void pacerLoop();
    void watchLoop();
};
