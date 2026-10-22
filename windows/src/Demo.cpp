#include "Demo.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include "Common.h"
#include "Floors.h"
#include "Renderer.h"

// Windowless run of the scene that saves frames: Rug.exe --demo <folder> [rug image]
namespace Demo {

static std::wstring env(const wchar_t* k) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(k, buf, ARRAYSIZE(buf));
    return n > 0 && n < ARRAYSIZE(buf) ? std::wstring(buf, n) : L"";
}

// Every number in a JSON file like [[x, y], ...], in order.
static std::vector<double> numbers(const std::wstring& path) {
    std::vector<double> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return out;
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    const char* p = s.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
            char* end;
            out.push_back(strtod(p, &end));
            p = end > p ? end : p + 1;
        } else {
            p++;
        }
    }
    return out;
}

int run(const std::wstring& outDir, const std::wstring& rugPath) {
    CreateDirectoryW(outDir.c_str(), nullptr);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(createDevice(device, ctx))) {
        printf("no Direct3D 11 device\n");
        return 1;
    }
    float2 size(1470, 956);
    Camera cam{size};
    std::wstring rugURL = rugPath;
    if (rugURL.empty()) {
        auto rugs = listFiles(resourcesDir() + L"\\rugs", {L"jpg", L"jpeg", L"png"});
        if (rugs.empty()) {
            printf("no rugs in %ls\n", resourcesDir().c_str());
            return 1;
        }
        rugURL = rugs.front();
    }
    std::unique_ptr<Renderer> r;
    Texture bg;
    try {
        r = std::make_unique<Renderer>(device.Get(), ctx.Get(), 4, rugURL);
        auto floors = Floors::all();
        std::wstring floor = floors.empty() ? L"" : floors.front();
        std::wstring q = env(L"RUG_FLOOR");
        for (auto& f : floors)
            if (!q.empty() && fileName(f).find(q) != std::wstring::npos) floor = f;
        // RUG_BG swaps the background for a ready picture (say, a floor with icons drawn on it)
        std::wstring bgPath = env(L"RUG_BG").empty() ? floor : env(L"RUG_BG");
        if (!bgPath.empty()) bg = Renderer::loadTexture(device.Get(), ctx.Get(), bgPath);
    } catch (const std::exception& e) {
        printf("renderer error: %s\n", e.what());
        return 1;
    }

    float aspect = float(r->rugTexture().height) / float(r->rugTexture().width);
    float w = size.x * 0.46f;
    Cloth cloth(w, w * aspect, ClothParams::tuned().columns);
    HeightField field(size);
    std::vector<Bump> bumps;
    std::wstring icons = env(L"RUG_ICONS");
    std::vector<double> pts = icons.empty() ? std::vector<double>() : numbers(icons);
    if (pts.size() >= 2) {
        // icon centres in points, y up
        for (size_t i = 0; i + 1 < pts.size(); i += 2)
            bumps.push_back({float2(float(pts[i]), float(pts[i + 1])), float2(30, 30), 22});
    } else {
        int grid[5][2] = {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}};
        for (auto& g : grid)
            bumps.push_back({float2(size.x * 0.5f - 140 + float(g[0]) * 110, size.y * 0.5f + 70 - float(g[1]) * 115),
                             float2(30, 30), 22});
    }
    field.setBumps(bumps);
    cloth.ground = &field;
    cloth.layout(cam.center(), 0, 110);
    r->setMesh(cloth);

    // RUG_FPS steps the cloth at another frame rate (say 180), sim(n) still covers n sixtieths of a second
    double fps = env(L"RUG_FPS").empty() ? 60 : _wtof(env(L"RUG_FPS").c_str());
    const float dt = float(1.0 / fps);
    double simTime = 0;
    int simFrames = 0;
    auto sim = [&](int n) {
        for (int i = 0; i < int(n * fps / 60 + 0.5); i++) {
            field.animate(dt);
            double t0 = nowSeconds();
            cloth.step(dt);
            simTime += nowSeconds() - t0;
            simFrames++;
        }
        cloth.updateDerived(cam);
    };
    std::vector<uint8_t> px;
    auto save = [&](const std::wstring& path) {
        int pw, ph;
        if (!r->snapshot(cloth, cam, 1, bg.srv.Get(), px, pw, ph)) return false;
        return savePNG(path, px.data(), pw, ph, pw * 4);
    };
    auto shot = [&](const char* name) {
        std::wstring n = utf8ToWide(name);
        if (!save(outDir + L"\\" + n + L".png")) {
            printf("snapshot failed\n");
            return;
        }
        printf("%s maxSpeed %.1f\n", name, cloth.maxSpeed);
    };
    auto avg = [&] { printf("sim avg %.2f ms/frame over %d frames\n", simTime / simFrames * 1000, simFrames); };

    sim(12);
    shot("0-falling");
    sim(170);
    shot("1-settled");
    float mz = 0;
    for (int i = 0; i < cloth.count; i++) mz = std::max(mz, cloth.pos[i].z);
    printf("field at bump %.2f max z %.2f animating %d\n", field.sample(bumps[0].center.x, bumps[0].center.y), mz,
           field.animating);

    // like a user: press at a screen point, drag, release
    auto drag = [&](float2 a, float2 b, int frames, int hold = 10) {
        int v;
        float z;
        if (!cloth.pick(a, true, &v, &z)) {
            printf("miss %.0f,%.0f\n", a.x, a.y);
            return;
        }
        float lift = std::max(z, 0.f) + 120;
        cloth.beginGrab(v, cam.unproject(a, lift));
        for (int f = 0; f < frames; f++) {
            float t = float(f + 1) / float(frames);
            cloth.grabTarget = cam.unproject(a + (b - a) * t, lift);
            sim(1);
        }
        sim(hold);
    };
    std::wstring scenario = env(L"RUG_SCENARIO");
    if (scenario == L"bench") {
        // the live RUG_BENCH drag: by the middle of the right edge, a fast figure eight for 6 s, then let go
        cloth.layout(cam.center(), 0, 0);
        cloth.updateDerived(cam);
        int v = cloth.idx(cloth.nx - 1, cloth.ny / 2);
        cloth.beginGrab(v, cam.unproject(cloth.proj[v], cloth.pos[v].z + 120));
        // RUG_SEED varies the rhythm and phase of the figure eight
        double seed = env(L"RUG_SEED").empty() ? 0 : _wtof(env(L"RUG_SEED").c_str());
        double fa = 4.5 * (1 + 0.13 * seed), fb = 9 * (1 - 0.07 * seed), ph = seed * 0.9;
        int frames = int(fps * 6);
        double lag = 0, maxLag = 0, t0 = nowSeconds();
        for (int f = 0; f < frames; f++) {
            double t = (f + 1) / fps;
            float2 p = cam.center() + float2(float(std::sin(t * fa + ph)) * 320, float(std::sin(t * fb)) * 140);
            cloth.grabTarget = cam.unproject(p, cloth.grabTarget.z);
            field.animate(dt);
            cloth.step(dt);
            cloth.updateDerived(cam);
            lag += cloth.handLag();
            maxLag = std::max(maxLag, double(cloth.handLag()));
            if (f == frames / 2) shot("b1-dragging");
        }
        printf("dragging: hand %.0f pt behind on average, %.0f at most; sim %.2f ms/frame\n", lag / frames, maxLag,
               (nowSeconds() - t0) / frames * 1000);
        cloth.endGrab();
        shot("b2-released");
        // when would the rug go to sleep: the old rule (every node under 3 pt/s for 0.75 s) and the new one
        double oldRule = -1, newRule = -1, calm = 0;
        std::vector<float3> late = cloth.pos;
        float maxZ = 0, lateMove = 0;  // height reached 2 s after release, and the biggest half-second move after 5 s
        for (int f = 1; f <= int(fps * 15); f++) {
            field.animate(dt);
            cloth.step(dt);
            if (f > fps * 2)
                for (int i = 0; i < cloth.count; i++) maxZ = std::max(maxZ, cloth.pos[i].z);
            if (f % int(fps / 2) == 0) {
                if (f > fps * 5)
                    for (int i = 0; i < cloth.count; i++) lateMove = std::max(lateMove, length(cloth.pos[i] - late[i]));
                late = cloth.pos;
            }
            calm = cloth.maxSpeed < 3 ? calm + dt : 0;
            if (oldRule < 0 && calm > 0.75) oldRule = f / fps;
            if (newRule < 0 && cloth.settled(dt)) newRule = f / fps;
            if (f == int(fps * 3)) {
                cloth.updateDerived(cam);
                shot("b3-3s");
            }
        }
        cloth.updateDerived(cam);
        printf("asleep after release: old rule %s, new rule %.2f s\n",
               oldRule < 0 ? "never (15 s)" : wideToUtf8(fmt(L"%.2f s", oldRule)).c_str(), newRule);
        printf("summary: lag %.0f pt, settles %s, highest after 2 s %.0f pt, moves after 5 s up to %.1f pt\n",
               lag / frames, newRule < 0 ? "never" : wideToUtf8(fmt(L"%.1f s", newRule)).c_str(), maxZ, lateMove);
        shot("b3-settled");
        avg();
        return 0;
    }
    if (scenario == L"readme") {
        // README clip: the rug drops on a pile of icons, a corner is lifted, mess underneath, the rug goes back
        std::wstring rec = outDir + L"\\frames";
        CreateDirectoryW(rec.c_str(), nullptr);
        int n = 0, tick = 0;
        auto frame = [&] {
            if (++tick % 2 != 0) return;
            if (save(rec + fmt(L"\\f%04d.png", n))) n++;
        };
        auto runFrames = [&](int k, auto each) {
            for (int i = 0; i < k; i++) {
                each(i);
                sim(1);
                frame();
            }
        };
        auto idle = [](int) {};
        cloth.layout(cam.center(), 0, 140);
        runFrames(110, idle);
        int corner = cloth.idx(cloth.nx - 1, cloth.ny - 1);
        float2 home = cloth.proj[corner];
        const float lift = 130;
        cloth.beginGrab(corner, cam.unproject(home, lift));
        float2 to = cam.center() + float2(-cloth.width * 0.22f, -cloth.height * 0.2f);
        runFrames(50, [&](int i) {
            float t = float(i + 1) / 50, e = t * t * (3 - 2 * t);
            cloth.grabTarget = cam.unproject(home + (to - home) * e, lift);
        });
        runFrames(45, idle);
        cloth.endGrab();
        runFrames(60, idle);
        float2 from = cloth.proj[corner];
        int v;
        float z;
        if (cloth.pick(from, true, &v, &z)) {
            cloth.beginGrab(v, cam.unproject(from, z + lift));
            runFrames(50, [&](int i) {
                float t = float(i + 1) / 50, e = t * t * (3 - 2 * t);
                cloth.grabTarget = cam.unproject(from + (home - from) * e, z + lift);
            });
            runFrames(10, idle);
            cloth.endGrab();
        }
        runFrames(70, idle);
        printf("frames %d\n", n);
        return 0;
    }
    if (scenario == L"curl") {
        // an edge folded over and dropped onto itself
        float2 tr = cloth.proj[cloth.idx(cloth.nx - 1, cloth.ny - 1)];
        drag(tr + float2(-3, -3), tr + float2(-90, -60), 15);
        cloth.endGrab();
        sim(90);
        shot("c1-curl");
        float2 br = cloth.proj[cloth.idx(cloth.nx - 1, 0)];
        drag(br + float2(-3, 3), br + float2(-40, 160), 10);
        cloth.endGrab();
        sim(90);
        shot("c2-curl2");
        float2 ml = cloth.proj[cloth.idx(0, cloth.ny / 2)];
        drag(ml + float2(3, 0), ml + float2(260, 10), 8);
        cloth.endGrab();
        sim(120);
        shot("c3-fast");
        avg();
        return 0;
    }
    if (scenario == L"flip") {
        float2 c = cam.center();
        float hw = cloth.width / 2, hh = cloth.height / 2;
        // flip it whole, by the middle of the right edge over to the other side
        drag(c + float2(hw - 4, 0), c + float2(-hw - 160, 20), 25);
        shot("f1-flipping");
        cloth.endGrab();
        sim(120);
        shot("f2-flipped");
        // and back, fast, by the top corner
        drag(c + float2(-hw - 100, hh - 20), c + float2(hw, -40), 12);
        shot("f3-back");
        cloth.endGrab();
        sim(120);
        shot("f4-back-settled");
        // crumple it, circling by the middle
        if (cloth.pick(cloth.pose().center, true)) {
            float2 p = cloth.pose().center;
            drag(p, p + float2(150, 0), 8, 0);
            for (int k = 0; k < 40; k++) {
                float a = float(k) * 0.35f;
                p = cloth.pose().center;
                cloth.grabTarget = cam.unproject(p + float2(std::cos(a), std::sin(a)) * 220, cloth.grabTarget.z);
                sim(1);
            }
            shot("f5-crumple");
            cloth.endGrab();
            sim(150);
            shot("f6-crumple-settled");
        }
        avg();
        return 0;
    }

    // take the top right corner and pull it across the rug
    int v = cloth.idx(cloth.nx - 1, cloth.ny - 1);
    float2 start = cloth.proj[v];
    float2 end = cam.center() + float2(-40, -30);
    cloth.beginGrab(v, cam.unproject(start, 120));
    sim(20);
    shot("2-lift");
    for (int f = 0; f < 50; f++) {
        float t = float(f) / 49;
        cloth.grabTarget = cam.unproject(start + (end - start) * t, 120);
        sim(1);
    }
    sim(30);
    shot("3-pulled");
    cloth.endGrab();
    sim(160);
    shot("4-folded");

    // just lift the bottom left corner, the floor shows underneath
    int v2 = cloth.idx(0, 0);
    float2 s2 = cloth.proj[v2];
    cloth.beginGrab(v2, cam.unproject(s2 + float2(60, 50), 140));
    sim(60);
    shot("5-corner");
    avg();
    return 0;
}

}  // namespace Demo
