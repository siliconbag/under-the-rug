#pragma once
#include <cmath>
#include <vector>
#include "vec.h"

struct Bump {
    float2 center;
    float2 radius;
    float height;
    bool operator==(const Bump& o) const { return center == o.center && radius == o.radius && height == o.height; }
};

// Floor height map. Icon bumps grow and fade smoothly.
class HeightField {
public:
    const float cell = 4;
    int w, h;
    bool animating = false;

    explicit HeightField(float2 size) {
        w = int(size.x / cell) + 3;
        h = int(size.y / cell) + 3;
        data.assign(w * h, 0.f);
        target.assign(w * h, 0.f);
    }

    void setBumps(const std::vector<Bump>& bumps) {
        std::fill(target.begin(), target.end(), 0.f);
        for (const Bump& b : bumps) {
            int x0 = std::max(0, int((b.center.x - b.radius.x) / cell));
            int x1 = std::min(w - 1, int((b.center.x + b.radius.x) / cell) + 1);
            int y0 = std::max(0, int((b.center.y - b.radius.y) / cell));
            int y1 = std::min(h - 1, int((b.center.y + b.radius.y) / cell) + 1);
            if (x0 > x1 || y0 > y1) continue;
            for (int yy = y0; yy <= y1; yy++) {
                for (int xx = x0; xx <= x1; xx++) {
                    float dx = (float(xx) * cell - b.center.x) / b.radius.x;
                    float dy = (float(yy) * cell - b.center.y) / b.radius.y;
                    float q = dx * dx + dy * dy;
                    if (q < 1) target[yy * w + xx] += b.height * std::pow(1 - q, 0.6f);
                }
            }
        }
        animating = true;
    }

    // true while bumps are still growing or fading
    bool animate(float dt) {
        if (!animating) return false;
        float k = std::min(1.f, dt * 4);
        float maxDiff = 0;
        for (int i = 0; i < w * h; i++) {
            float d = target[i] - data[i];
            if (d != 0) {
                data[i] += d * k;
                maxDiff = std::max(maxDiff, std::abs(d));
            }
        }
        if (maxDiff < 0.05f) {
            data = target;
            animating = false;
        }
        return true;
    }

    float sample(float x, float y) const {
        float fx = x / cell, fy = y / cell;
        if (fx < 0 || fy < 0) return 0;
        int ix = int(fx), iy = int(fy);
        if (ix >= w - 1 || iy >= h - 1) return 0;
        float tx = fx - float(ix), ty = fy - float(iy);
        int i = iy * w + ix;
        float a = data[i], b = data[i + 1], c = data[i + w], d = data[i + w + 1];
        return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
    }

private:
    std::vector<float> data, target;
};
