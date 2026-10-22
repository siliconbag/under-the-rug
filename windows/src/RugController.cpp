#include "RugController.h"
#include <knownfolders.h>
#include <psapi.h>
#include <windowsx.h>
#include <cfloat>
#include <stdexcept>
#include "Common.h"
#include "Desktop.h"
#include "IconPile.h"

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

enum { TIMER_HOVER = 1, TIMER_SCAN, TIMER_RESCAN_SOON, TIMER_SCREENS, TIMER_BENCH, TIMER_TRIM };
enum : UINT { WM_APP_MOVE_SCREEN = WM_APP + 50 };

static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[160];
        sprintf_s(buf, "%s failed (0x%08lx)", what, (unsigned long)hr);
        throw std::runtime_error(buf);
    }
}

// The top-level window that holds the desktop icons: Progman, or a WorkerW on some Windows versions.
static HWND desktopHost() {
    static HWND cached = nullptr;
    if (cached && IsWindow(cached) && FindWindowExW(cached, nullptr, L"SHELLDLL_DefView", nullptr)) return cached;
    cached = nullptr;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            if (!FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) return TRUE;
            *(HWND*)p = h;
            return FALSE;
        },
        (LPARAM)&cached);
    return cached;
}

static bool primaryButtonDown() {
    // GetAsyncKeyState looks at physical buttons
    int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The DXGI output that shows this monitor, whichever GPU drives it.
static ComPtr<IDXGIOutput> outputFor(HMONITOR m) {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return nullptr;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == m) return o;
        }
    }
    return nullptr;
}

RugController::RugController(HINSTANCE inst, const std::wstring& texturePath, const Screen& s) : screen(s) {
    applyScreen();
    int pw = frame.right - frame.left, ph = frame.bottom - frame.top;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"RugWindow";
        RegisterClassExW(&wc);
        registered = true;
    }
    // Layered + transparent passes clicks through, the flag is dropped only while the cursor is over the rug.
    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                               WS_EX_TRANSPARENT,
                           L"RugWindow", L"Rug", WS_POPUP, frame.left, frame.top, pw, ph, nullptr, nullptr, inst, this);
    if (!hwnd) throw std::runtime_error("window");
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    check(createDevice(device, ctx), "Direct3D 11");
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    check(device.As(&dxgiDevice), "DXGI device");
    check(dxgiDevice->GetAdapter(&adapter), "DXGI adapter");
    check(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = pw;
    sd.Height = ph;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    check(factory->CreateSwapChainForComposition(device.Get(), &sd, nullptr, &swapchain), "swap chain");
    check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp)), "DirectComposition");
    check(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target), "composition target");
    check(dcomp->CreateVisual(&visual), "composition visual");
    check(visual->SetContent(swapchain.Get()), "composition content");
    check(target->SetRoot(visual.Get()), "composition root");
    check(dcomp->Commit(), "composition commit");

    renderer = std::make_unique<Renderer>(device.Get(), ctx.Get(), pickSampleCount(device.Get()), texturePath);

    awakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    buildCloth(90, false);
    drawFrame();

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    keepAboveDesktop();

    SetTimer(hwnd, TIMER_HOVER, 1000 / 30, nullptr);
    SetTimer(hwnd, TIMER_SCAN, 4000, nullptr);
    SetTimer(hwnd, TIMER_RESCAN_SOON, 300, nullptr);
    // RUG_BENCH=<seconds>: a scripted fast drag that long after launch (2.5 s for 1 or anything shorter)
    wchar_t bench[16] = L"";
    if (GetEnvironmentVariableW(L"RUG_BENCH", bench, 16) > 0) {
        double delay = std::max(2.5, _wtof(bench));
        SetTimer(hwnd, TIMER_BENCH, UINT(delay * 1000), nullptr);
    }
    pacer = std::thread([this] { pacerLoop(); });
    watcher = std::thread([this] { watchLoop(); });
}

RugController::~RugController() {
    SetEvent(stopEvent);
    if (pacer.joinable()) pacer.join();
    if (watcher.joinable()) watcher.join();
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
    CloseHandle(awakeEvent);
    CloseHandle(stopEvent);
}

// Size, scale and refresh rate of the current screen.
void RugController::applyScreen() {
    frame = screen.rect;
    sc = screen.scale;
    camera.size = float2((frame.right - frame.left) / sc, (frame.bottom - frame.top) / sc);
    field = std::make_unique<HeightField>(camera.size);
    ComPtr<IDXGIOutput> out = outputFor(screen.handle);
    std::lock_guard<std::mutex> lock(paceMutex);
    vblankOutput = out;
    framePeriod = 1.0 / std::min(std::max(screen.refresh, 30.0), 240.0);
}

// MARK: rug

float RugController::scale() const {
    double v = Prefs::getDouble(L"scale", 0.46);
    return v > 0 ? float(v) : 0.46f;
}

void RugController::setScale(float s) {
    Prefs::setDouble(L"scale", s);
    buildCloth(60, true);
}

bool RugController::savedPose(float2& center, float& angle) {
    float x, y, a;
    if (swscanf_s(Prefs::getString(L"pose").c_str(), L"%f,%f,%f", &x, &y, &a) != 3) return false;
    center = float2(x, y) * camera.size;
    angle = a;
    return true;
}

void RugController::savePose() {
    if (!cloth) return;
    Cloth::Pose p = cloth->pose();
    float2 c = p.center / camera.size;
    Prefs::setString(L"pose", fmt(L"%f,%f,%f", c.x, c.y, p.angle));
}

void RugController::buildCloth(float drop, bool keepPose) {
    float2 c = camera.center();
    float angle = 0;
    if (keepPose && cloth) {
        Cloth::Pose p = cloth->pose();
        c = p.center;
        angle = p.angle;
    } else {
        savedPose(c, angle);
    }
    layoutCloth(drop, c, angle);
}

void RugController::layoutCloth(float drop, float2 c, float angle) {
    const Texture& tex = renderer->rugTexture();
    float aspect = float(tex.height) / float(tex.width);
    float w = camera.size.x * scale();
    auto next = std::make_unique<Cloth>(w, w * aspect, ClothParams::tuned().columns);
    next->ground = field.get();
    next->layout(c, angle, drop);
    next->updateDerived(camera);
    cloth = std::move(next);
    renderer->setMesh(*cloth);
    wake();
}

void RugController::redrop() { buildCloth(110, true); }

void RugController::center() {
    Prefs::remove(L"pose");
    cloth->layout(camera.center(), 0, 60);
    wake();
}

bool RugController::setTexture(const std::wstring& path) {
    try {
        renderer->setTexture(path);
        buildCloth(110, true);
        return true;
    } catch (const std::exception& e) {
        rugLog(L"texture error " + utf8ToWide(e.what()));
        MessageBeep(MB_ICONWARNING);
        return false;
    }
}

void RugController::moveTo(const Screen& s, const POINT* dropAt) {
    bool same = s.device == screen.device && EqualRect(&s.rect, &screen.rect) && s.scale == screen.scale &&
                s.refresh == screen.refresh;
    if (same && !dropAt) {
        // nothing to rebuild, but the monitor handle may be new after a display change
        screen = s;
        ComPtr<IDXGIOutput> out = outputFor(s.handle);
        std::lock_guard<std::mutex> lock(paceMutex);
        vblankOutput = out;
        return;
    }
    if (dragging) mouseUp();
    Cloth::Pose p = cloth->pose();
    float2 rel = p.center / camera.size;
    screen = s;
    applyScreen();
    // a new cloth right away, the old one stood on the old floor
    float2 c = dropAt ? toPoints(dropAt->x - frame.left, dropAt->y - frame.top) : rel * camera.size;
    layoutCloth(dropAt ? 110.f : 60.f, c, p.angle);
    int pw = frame.right - frame.left, ph = frame.bottom - frame.top;
    // the window picks up the new screen's DPI here, the WM_DPICHANGED that follows finds nothing to do
    SetWindowPos(hwnd, nullptr, frame.left, frame.top, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    renderer->releaseTargets();
    HRESULT hr = swapchain->ResizeBuffers(2, pw, ph, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        rugLog(fmt(L"resize error 0x%08lx", hr));
        setAwake(false);
        if (onDeviceLost) onDeviceLost();
        return;
    }
    lastBumps.clear();
    SetTimer(hwnd, TIMER_RESCAN_SOON, 300, nullptr);
    Prefs::setString(L"screen", screen.device);
    rugDebug(L"rug on " + screen.name + fmt(L" %ldx%ld, %.0f%%, %.2f Hz", pw, ph, sc * 100, screen.refresh));
    if (onMoved) onMoved();
}

void RugController::wake() {
    calmTime = 0;
    setAwake(true);
}

void RugController::setAwake(bool a) {
    if (a == awake) return;
    awake = a;
    if (a) {
        SetEvent(awakeEvent);
        return;
    }
    ResetEvent(awakeEvent);
    // the multisampled targets go only after a while: freeing and making them again stalls the first frame of a drag
    SetTimer(hwnd, TIMER_TRIM, 15000, nullptr);
    rugDebug(L"asleep");
}

// The rug has been still for a while: the last frame stays on screen in the swap chain,
// the multisampled targets aren't needed until the next move.
void RugController::trim() {
    PROCESS_MEMORY_COUNTERS_EX before = {sizeof(before)}, after = {sizeof(after)};
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&before, sizeof(before));
    renderer->releaseTargets();
    ComPtr<IDXGIDevice3> d3;
    if (SUCCEEDED(device.As(&d3))) d3->Trim();
    trimmed = true;
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&after, sizeof(after));
    rugDebug(fmt(L"trimmed, private memory %zu -> %zu MB", before.PrivateUsage >> 20, after.PrivateUsage >> 20));
}

// The cursor is on the sleeping rug, a grab may follow: bring the render targets back now with one still frame,
// so the stall of making them happens while nothing moves.
void RugController::warmUp() {
    if (!trimmed) return;
    trimmed = false;
    present();
}

// MARK: frame

void RugController::drawFrame() {
    float dt;
    double period;
    {
        std::lock_guard<std::mutex> lock(paceMutex);
        period = framePeriod;
        dt = float(period);
    }
    double t0 = nowSeconds();
    if (stats.frames == 0) stats.start = t0;
    else {
        double gap = t0 - stats.last;
        stats.maxGap = std::max(stats.maxGap, gap);
        if (gap > period * 1.5) stats.hitches++;
    }
    stats.last = t0;
    stats.frames++;

    if (benchStart > 0) {
        // a figure eight around the screen centre, fast like a quick mouse drag, timed by the clock
        double t = t0 - benchStart;
        float2 p = camera.center() + float2(float(std::sin(t * 4.5)) * 320, float(std::sin(t * 9)) * 140);
        cloth->grabTarget = camera.unproject(p, cloth->grabTarget.z);
        if (t > 6) {
            benchStart = 0;
            cloth->endGrab();
            rugLog(L"bench done");
        }
    }
    // the cursor is followed here too, so dragging keeps going over other windows
    if (dragging) {
        if (primaryButtonDown()) mouseDragged(cursorPoints());
        else mouseUp();
    }
    bool fieldMoving = field->animate(dt);
    cloth->step(dt);
    cloth->updateDerived(camera);
    double t1 = nowSeconds();
    stats.sim += t1 - t0;
    if (cloth->isGrabbing()) {
        stats.lag += cloth->handLag();
        stats.maxLag = std::max(stats.maxLag, double(cloth->handLag()));
        stats.lagFrames++;
    }
    present();
    if (t1 - stats.start >= 1 && rugDebugOn()) logStats(nowSeconds());

    bool settled = cloth->settled(dt);
    if (!cloth->isGrabbing() && !fieldMoving && cloth->maxSpeed < 3) calmTime += dt;
    else calmTime = 0;
    if (!fieldMoving && (calmTime > 0.75 || settled)) {
        setAwake(false);
        savePose();
    }
}

void RugController::logStats(double now) {
    collectGpuTimes();
    double span = now - stats.start;
    int n = stats.frames;
    rugLog(fmt(L"%.1f fps on %.0f Hz, %d hitches, worst gap %.1f ms | cpu sim %.2f render %.2f present %.2f (max %.1f) ms "
               L"| gpu %.2f (max %.2f) ms | maxSpeed %.0f",
               n / span, screen.refresh, stats.hitches, stats.maxGap * 1000, stats.sim / n * 1000, stats.render / n * 1000,
               stats.present / n * 1000, stats.maxPresent * 1000, stats.gpuFrames ? stats.gpu / stats.gpuFrames : 0.0,
               stats.maxGpu, cloth->maxSpeed));
    if (stats.refreshes)
        rugLog(fmt(L"  on screen: %u new frames over %u refreshes", stats.presents, stats.refreshes));
    if (stats.lagFrames)
        rugLog(fmt(L"  hand behind the cursor: %.0f pt on average, %.0f at most", stats.lag / stats.lagFrames, stats.maxLag));
    UINT lp = stats.lastPresent, lr = stats.lastRefresh;
    stats = Stats();
    stats.lastPresent = lp;
    stats.lastRefresh = lr;
}

// GPU time of a frame from timestamp queries, read a few frames later without waiting.
void RugController::collectGpuTimes() {
    for (GpuTimer& q : gpuTimers) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (ctx->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 a = 0, b = 0;
        ctx->GetData(q.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        ctx->GetData(q.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        q.pending = false;
        if (dj.Disjoint || b < a) continue;
        double ms = double(b - a) * 1000 / double(dj.Frequency);
        stats.gpu += ms;
        stats.maxGpu = std::max(stats.maxGpu, ms);
        stats.gpuFrames++;
    }
}

void RugController::present() {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    GpuTimer* timer = nullptr;
    if (rugDebugOn()) {
        collectGpuTimes();
        GpuTimer& q = gpuTimers[gpuNext];
        if (!q.disjoint) {
            D3D11_QUERY_DESC dd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, td = {D3D11_QUERY_TIMESTAMP, 0};
            device->CreateQuery(&dd, &q.disjoint);
            device->CreateQuery(&td, &q.begin);
            device->CreateQuery(&td, &q.end);
        }
        if (!q.pending && q.disjoint && q.begin && q.end) {
            timer = &q;
            gpuNext = (gpuNext + 1) % 4;
            ctx->Begin(q.disjoint.Get());
            ctx->End(q.begin.Get());
        }
    }
    double t0 = nowSeconds();
    renderer->render(*cloth, camera, frame.right - frame.left, frame.bottom - frame.top, back.Get(), nullptr);
    if (timer) {
        ctx->End(timer->end.Get());
        ctx->End(timer->disjoint.Get());
        timer->pending = true;
    }
    double t1 = nowSeconds();
    HRESULT hr = swapchain->Present(0, 0);
    double t2 = nowSeconds();
    stats.render += t1 - t0;
    stats.present += t2 - t1;
    stats.maxPresent = std::max(stats.maxPresent, t2 - t1);
    DXGI_FRAME_STATISTICS fs;
    if (rugDebugOn() && SUCCEEDED(swapchain->GetFrameStatistics(&fs))) {
        if (stats.lastPresent && fs.PresentCount > stats.lastPresent) {
            stats.presents += fs.PresentCount - stats.lastPresent;
            stats.refreshes += fs.SyncRefreshCount - stats.lastRefresh;
        }
        stats.lastPresent = fs.PresentCount;
        stats.lastRefresh = fs.SyncRefreshCount;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        rugLog(fmt(L"device lost 0x%08lx", hr));
        setAwake(false);
        if (onDeviceLost) onDeviceLost();
    }
}

// Paces frames at the screen's refresh rate while the rug moves, in step with its vertical blank
// (a timer when that isn't available). Posting keeps the simulation on the UI thread,
// and posted messages also arrive while a menu or dialog runs its own loop.
void RugController::pacerLoop() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    double next = nowSeconds();
    for (;;) {
        if (WaitForSingleObject(awakeEvent, 0) != WAIT_OBJECT_0) {
            HANDLE hs[2] = {awakeEvent, stopEvent};
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) break;
            next = nowSeconds();
        }
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        ComPtr<IDXGIOutput> out;
        double period;
        {
            std::lock_guard<std::mutex> lock(paceMutex);
            out = vblankOutput;
            period = framePeriod;
        }
        double t0 = nowSeconds();
        // a blank that comes back at once means the display is off or gone, then the timer takes over
        if (out && SUCCEEDED(out->WaitForVBlank()) && nowSeconds() - t0 > period * 0.25) {
            next = nowSeconds();
        } else {
            next += period;
            double delay = next - nowSeconds();
            if (delay < -0.1) next = nowSeconds();  // the machine was busy, don't catch up in a burst
            if (delay > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(delay * 1e7);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE hs[2] = {timer, stopEvent};
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) break;
            }
        }
        if (!framePending.exchange(true)) PostMessageW(hwnd, WM_APP_FRAME, 0, 0);
    }
    CloseHandle(timer);
}

// MARK: mouse

float2 RugController::toPoints(int x, int y) const { return float2(x / sc, camera.size.y - y / sc); }

float2 RugController::cursorPoints() const {
    POINT m;
    GetCursorPos(&m);
    return toPoints(m.x - frame.left, m.y - frame.top);
}

void RugController::setIgnoresMouse(bool v) {
    ignoresMouse = v;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, v ? ex | WS_EX_TRANSPARENT : ex & ~WS_EX_TRANSPARENT);
}

void RugController::pollHover() {
    keepAboveDesktop();
    if (dragging) return;
    // while someone else drags (a file from Explorer) the rug lets the mouse through
    bool busy = (primaryButtonDown() || (GetAsyncKeyState(VK_MBUTTON) & 0x8000)) && ignoresMouse;
    bool hit = !busy && cloth->pick(cursorPoints(), false);
    if (ignoresMouse == hit) setIgnoresMouse(!hit);
    if (hit && !awake) warmUp();
}

// Directly above the window that holds the icons: over the icons, under every app window.
// When "Show desktop" brings the desktop forward, the rug follows it.
void RugController::keepAboveDesktop() {
    HWND d = desktopHost();
    if (!d) return;
    HWND below = GetWindow(hwnd, GW_HWNDNEXT);
    while (below && below != d && !IsWindowVisible(below)) below = GetWindow(below, GW_HWNDNEXT);
    if (below == d) return;
    HWND above = GetWindow(d, GW_HWNDPREV);
    HWND after = above && above != hwnd && !(GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) ? above : HWND_TOP;
    SetWindowPos(hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void RugController::mouseDown(float2 s) {
    int v;
    float z;
    if (!cloth->pick(s, true, &v, &z)) {
        setIgnoresMouse(true);
        return;
    }
    dragging = true;
    SetCapture(hwnd);
    cloth->beginGrab(v, camera.unproject(s, std::max(z, 0.f) + liftHeight));
    wake();
}

void RugController::mouseDragged(float2 s) {
    if (!dragging) return;
    cloth->grabTarget = camera.unproject(s, cloth->grabTarget.z);
    wake();
}

void RugController::mouseUp() {
    if (!dragging) return;
    dragging = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    cloth->endGrab();
    wake();
    // let go over another screen: the rug lands there
    POINT m;
    GetCursorPos(&m);
    HMONITOR mon = MonitorFromPoint(m, MONITOR_DEFAULTTONULL);
    if (mon && mon != screen.handle) PostMessageW(hwnd, WM_APP_MOVE_SCREEN, 0, MAKELPARAM(m.x, m.y));
}

// MARK: icons

void RugController::watchLoop() {
    std::vector<HANDLE> hs;
    for (std::wstring dir : {knownFolder(FOLDERID_Desktop), knownFolder(FOLDERID_PublicDesktop)}) {
        if (dir.empty()) continue;
        HANDLE h = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        if (h != INVALID_HANDLE_VALUE) hs.push_back(h);
    }
    size_t n = hs.size();
    hs.push_back(stopEvent);
    for (;;) {
        DWORD r = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, INFINITE);
        if (r >= WAIT_OBJECT_0 + n) break;
        PostMessageW(hwnd, WM_APP_DESKTOP_CHANGED, 0, 0);
        FindNextChangeNotification(hs[r - WAIT_OBJECT_0]);
    }
    for (size_t i = 0; i < n; i++) FindCloseChangeNotification(hs[i]);
}

void RugController::rescan() {
    if (dragging) return;
    std::vector<DesktopIcon> icons;
    int iconPx = 48;
    if (!DesktopView::shared().icons(icons, &iconPx)) return;
    float iconSize = iconPx / sc;
    std::vector<Bump> bumps;
    for (auto& ic : icons) {
        float cx = ((ic.rect.left + ic.rect.right) / 2.f - frame.left) / sc;
        float cy = ((ic.rect.top + ic.rect.bottom) / 2.f - frame.top) / sc;
        float r = std::max(18.f, iconSize * 0.47f);
        bumps.push_back({float2(cx, camera.size.y - cy), float2(r, r), 22});
    }
    if (bumps != lastBumps) {
        if (rugDebugOn()) {
            std::wstring s = L"icons: ";
            for (size_t i = 0; i < icons.size() && i < 30; i++)
                s += icons[i].name + fmt(L"@%ld,%ld; ", icons[i].rect.left, icons[i].rect.top);
            rugLog(s);
        }
        lastBumps = bumps;
        field->setBumps(bumps);
        wake();
    }
}

// Sweeps the icons into a heap under the rug and drops the rug on top.
// App::pileIcons first makes sure Windows won't arrange them back.
void RugController::pileIcons() {
    Cloth::Pose p = cloth->pose();
    POINT c = {frame.left + LONG(p.center.x * sc), frame.top + LONG((camera.size.y - p.center.y) * sc)};
    IconPile::pile(c, sc);
    cloth->layout(p.center, p.angle, 130);
    wake();
    SetTimer(hwnd, TIMER_RESCAN_SOON, 400, nullptr);
}

void RugController::restoreIcons() {
    IconPile::restore();
    SetTimer(hwnd, TIMER_RESCAN_SOON, 400, nullptr);
}

// Drops the rug so it covers as many icons as it can (icons on this screen, the others are off the floor).
void RugController::coverIcons() {
    float2 lo(FLT_MAX), hi(-FLT_MAX);
    bool any = false;
    for (auto& b : lastBumps) {
        if (b.center.x < 0 || b.center.y < 0 || b.center.x > camera.size.x || b.center.y > camera.size.y) continue;
        lo = vmin(lo, b.center);
        hi = vmax(hi, b.center);
        any = true;
    }
    if (!any) {
        redrop();
        return;
    }
    float2 c = (lo + hi) * 0.5f;
    float2 half = float2(cloth->width, cloth->height) * 0.5f;
    c = clamp(c, half, camera.size - half);
    cloth->layout(c, 0, 110);
    wake();
}

// MARK: window

LRESULT CALLBACK RugController::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    }
    auto* self = (RugController*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && self->hwnd == h) return self->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT RugController::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_FRAME:
        framePending = false;
        if (awake) drawFrame();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_HOVER) pollHover();
        else if (wp == TIMER_SCAN) rescan();
        else if (wp == TIMER_RESCAN_SOON) {
            KillTimer(hwnd, TIMER_RESCAN_SOON);
            rescan();
        } else if (wp == TIMER_TRIM) {
            KillTimer(hwnd, TIMER_TRIM);
            if (!awake) trim();
        } else if (wp == TIMER_BENCH) {
            // grab the rug by the middle of its right edge, drawFrame moves the hand
            KillTimer(hwnd, TIMER_BENCH);
            warmUp();  // as a real cursor would on its way to the rug
            cloth->layout(camera.center(), 0, 0);
            cloth->updateDerived(camera);
            int v = cloth->idx(cloth->nx - 1, cloth->ny / 2);
            cloth->beginGrab(v, camera.unproject(cloth->proj[v], cloth->pos[v].z + liftHeight));
            benchStart = nowSeconds();
            stats = Stats();
            rugLog(L"bench start");
            wake();
        } else if (wp == TIMER_SCREENS) {
            // screens were added, removed or rescaled: stay on ours if it's still there, the primary one otherwise
            KillTimer(hwnd, TIMER_SCREENS);
            moveTo(findScreen(screen.device));
        }
        return 0;
    case WM_APP_DESKTOP_CHANGED:
        // Explorer places a new icon a moment later
        SetTimer(hwnd, TIMER_RESCAN_SOON, 600, nullptr);
        return 0;
    case WM_APP_MOVE_SCREEN: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        moveTo(screenAt(p), &p);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        mouseDown(toPoints(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_MOUSEMOVE:
        if (dragging) mouseDragged(toPoints(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        else if (!cloth->pick(toPoints(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), false)) setIgnoresMouse(true);
        return 0;
    case WM_LBUTTONUP:
        mouseUp();
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && dragging && !primaryButtonDown()) mouseUp();
        return 0;
    case WM_MOUSEWHEEL: {
        float d = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA) * 0.05f;
        if (d != 0) {
            cloth->rotate(d);
            wake();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &p);
        if (menuProvider) menuProvider(p);
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        SetTimer(hwnd, TIMER_SCREENS, 500, nullptr);
        return 0;
    case WM_ENDSESSION:
        savePose();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
