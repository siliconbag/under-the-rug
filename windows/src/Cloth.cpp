#include "Cloth.h"
#include <cfloat>
#include <cstdlib>
#include <random>

ClothParams ClothParams::tuned() {
    ClothParams p;
    auto f = [](const char* k, float& v) {
        if (const char* s = std::getenv(k)) v = (float)std::atof(s);
    };
    f("RUG_G", p.gravity);
    f("RUG_BEND", p.bendCompliance);
    f("RUG_FAR", p.farBendCompliance);
    f("RUG_DAMP", p.airDamping);
    f("RUG_FS", p.groundStatic);
    f("RUG_FK", p.groundKinetic);
    if (const char* s = std::getenv("RUG_COLS")) p.columns = std::max(3, std::atoi(s));
    f("RUG_FOLLOW", p.handFollow);
    if (const char* s = std::getenv("RUG_SPLIT")) p.maxSplit = std::max(1, std::atoi(s));
    return p;
}

static float randomIn(float lo, float hi) {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

Cloth::Cloth(float width_, float height_, int columns) {
    nx = columns;
    spacing = width_ / float(columns - 1);
    ny = std::max(3, int(std::round(height_ / spacing)) + 1);
    count = nx * ny;
    width = width_;
    height = spacing * float(ny - 1);
    thickness = spacing * 1.1f;

    pos.resize(count);
    prev.resize(count);
    vel.resize(count);
    rest.resize(count);
    nrm.resize(count);
    proj.resize(count);
    invMass.resize(count);
    hash = std::make_unique<SpatialHash>(thickness, count);

    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            int k = idx(i, j);
            rest[k] = float3(float(i) * spacing - width / 2, float(j) * spacing - height / 2, 0);
            pos[k] = rest[k];
            prev[k] = rest[k];
            vel[k] = float3();
            nrm[k] = float3(0, 0, 1);
            invMass[k] = 1;
            uvs.push_back(float2(float(i) / float(nx - 1), 1 - float(j) / float(ny - 1)));
        }
    }
    for (int j = 0; j < ny - 1; j++) {
        for (int i = 0; i < nx - 1; i++) {
            uint32_t a = idx(i, j), b = idx(i + 1, j);
            uint32_t c = idx(i, j + 1), d = idx(i + 1, j + 1);
            if ((i + j) % 2 == 0) {
                indices.insert(indices.end(), {a, b, d, a, d, c});
            } else {
                indices.insert(indices.end(), {a, b, c, b, d, c});
            }
        }
    }
    const ClothParams& p = params;
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            if (i + 1 < nx) addCon(idx(i, j), idx(i + 1, j), 0);
            if (j + 1 < ny) addCon(idx(i, j), idx(i, j + 1), 0);
            if (i + 1 < nx && j + 1 < ny) {
                addCon(idx(i, j), idx(i + 1, j + 1), p.shearCompliance);
                addCon(idx(i + 1, j), idx(i, j + 1), p.shearCompliance);
            }
            if (i + 2 < nx) addCon(idx(i, j), idx(i + 2, j), p.bendCompliance);
            if (j + 2 < ny) addCon(idx(i, j), idx(i, j + 2), p.bendCompliance);
            if (p.farBendCompliance >= 0) {
                if (i + 4 < nx) addCon(idx(i, j), idx(i + 4, j), p.farBendCompliance);
                if (j + 4 < ny) addCon(idx(i, j), idx(i, j + 4), p.farBendCompliance);
            }
        }
    }
}

void Cloth::addCon(int a, int b, float c) {
    cons.push_back(Con{a, b, length(rest[a] - rest[b]), c});
}

// MARK: layout

void Cloth::layout(float2 center, float angle, float drop) {
    endGrab();
    float ca = std::cos(angle), sa = std::sin(angle);
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            int k = idx(i, j);
            float3 r = rest[k];
            float u = float(i) / float(nx - 1);
            float z = drop > 0 ? drop * (0.55f + 0.45f * u) + randomIn(-1.5f, 1.5f) : 0;
            pos[k] = float3(center.x + r.x * ca - r.y * sa, center.y + r.x * sa + r.y * ca, z);
            prev[k] = pos[k];
            vel[k] = float3();
        }
    }
    maxSpeed = 1000;
    calmRef.clear();
}

Cloth::Pose Cloth::pose() const {
    float2 c;
    for (int i = 0; i < count; i++) c += pos[i].xy();
    c /= float(count);
    int j = ny / 2;
    float3 a = pos[idx(0, j)], b = pos[idx(nx - 1, j)];
    return {c, std::atan2(b.y - a.y, b.x - a.x)};
}

void Cloth::rotate(float angle) {
    float2 c = pose().center;
    float ca = std::cos(angle), sa = std::sin(angle);
    auto rot = [&](float3 p) {
        float2 d = p.xy() - c;
        return float3(c.x + d.x * ca - d.y * sa, c.y + d.x * sa + d.y * ca, p.z);
    };
    for (int i = 0; i < count; i++) {
        pos[i] = rot(pos[i]);
        prev[i] = rot(prev[i]);
    }
    if (isGrabbing()) {
        grabPos = rot(grabPos);
        grabTarget = rot(grabTarget);
    }
}

// MARK: grabbing

void Cloth::beginGrab(int v, float3 target) {
    endGrab();
    int vi = v % nx, vj = v / nx;
    float3 center = pos[v];
    for (int j = std::max(0, vj - 1); j <= std::min(ny - 1, vj + 1); j++) {
        for (int i = std::max(0, vi - 1); i <= std::min(nx - 1, vi + 1); i++) {
            int k = idx(i, j);
            grabbedIds.push_back(k);
            float3 o = pos[k] - center;
            o.z = 0;
            grabOffsets.push_back(o);
            invMass[k] = 0;
        }
    }
    grabPos = center;
    grabTarget = target;
    // long-range tethers: no point gets further from the hand than it is along the cloth at rest
    tetherRest.resize(count);
    for (int i = 0; i < count; i++) tetherRest[i] = length(rest[i] - rest[v]) * 1.01f;
    maxSpeed = 1000;
}

void Cloth::endGrab() {
    const float cap = 1400;
    for (int k : grabbedIds) {
        invMass[k] = 1;
        float s = length(vel[k]);
        if (s > cap) vel[k] *= cap / s;
    }
    grabbedIds.clear();
    grabOffsets.clear();
}

// MARK: simulation step

// Substeps have a fixed length (1/600 s by default), so the cloth behaves the same at any frame rate:
// a 60 Hz frame gets 10 of them like the original, a 180 Hz frame 3 or 4.
void Cloth::step(float frameDt) {
    float sdt = 1.0f / (60.0f * params.substeps);
    substepDebt += double(frameDt) / sdt;
    int n = int(substepDebt + 1e-6);
    substepDebt -= n;
    if (n <= 0) return;
    // The hand moves at most 0.3 thickness per substep. When the cursor runs away faster than that,
    // the same time is cut into finer substeps: the hand keeps up and nothing jumps through a layer.
    float follow = params.handFollow;
    if (isGrabbing()) {
        float over = handLag() * std::min(1.f, sdt * follow) / (0.3f * thickness);
        int split = std::min(params.maxSplit, over > 1 ? int(std::ceil(over)) : 1);
        sdt /= float(split);
        n *= split;
    }
    float maxVel = 0.2f * thickness / sdt;
    float travel = std::min(std::max(maxSpeed * (n * sdt) * 1.5f, thickness), thickness * 4);
    hash->create(pos.data(), count);
    hash->queryAll(pos.data(), count, travel);

    for (int s = 0; s < n; s++) {
        integrate(sdt, maxVel);
        moveGrabbed(sdt, follow);
        solveConstraints(sdt);
        solveTethers();
        solveSelfCollisions();
        solveSelfCollisions();
        solveGround();
        float inv = 1 / sdt;
        for (int i = 0; i < count; i++) vel[i] = (pos[i] - prev[i]) * inv;
    }
    float m = 0;
    for (int i = 0; i < count; i++)
        if (invMass[i] > 0) m = std::max(m, length2(vel[i]));
    maxSpeed = std::sqrt(m);
    if (!std::isfinite(maxSpeed)) {
        // just in case: if the solver blew up, lay the rug down again
        float2 c = prev[count / 2].xy();
        layout(std::isfinite(c.x) && std::isfinite(c.y) ? c : float2(width, height), 0, 60);
    }
}

bool Cloth::settled(float frameDt) {
    if (isGrabbing() || calmRef.size() != pos.size()) {
        calmRef.clear();
        calmWindows = 0;
        if (!isGrabbing()) calmRef = pos;
        calmClock = restClock = 0;
        return false;
    }
    calmClock += frameDt;
    restClock += frameDt;
    if (calmClock >= 0.25f) {
        // half a point per quarter second; after 4 s a slow creep of a fold or a roll doesn't keep it awake either
        float tol = restClock < 4 ? 0.5f : 2.0f;
        float m = 0;
        for (int i = 0; i < count; i++) m = std::max(m, length2(pos[i] - calmRef[i]));
        calmWindows = m < tol * tol ? calmWindows + 1 : 0;
        calmRef = pos;
        calmClock = 0;
    }
    return calmWindows >= 3;
}

void Cloth::solveTethers() {
    if (!isGrabbing() || (int)tetherRest.size() != count) return;
    float3 g = grabPos;
    for (int i = 0; i < count; i++) {
        if (invMass[i] == 0) continue;
        float3 d = pos[i] - g;
        float l = length(d);
        float m = tetherRest[i];
        if (l > m) pos[i] = g + d * (m / l);
    }
}

void Cloth::integrate(float sdt, float maxVel) {
    float g = params.gravity * sdt;
    float damp = std::max(0.f, 1 - params.airDamping * sdt);
    float mv2 = maxVel * maxVel;
    for (int i = 0; i < count; i++) {
        if (invMass[i] == 0) continue;
        float3 v = vel[i];
        v.z -= g;
        v *= damp;
        float s2 = length2(v);
        if (s2 > mv2) v *= maxVel / std::sqrt(s2);
        vel[i] = v;
        prev[i] = pos[i];
        pos[i] += v * sdt;
    }
}

void Cloth::moveGrabbed(float sdt, float follow) {
    if (!isGrabbing()) return;
    float3 d = (grabTarget - grabPos) * std::min(1.f, sdt * follow);
    float maxStep = 0.3f * thickness;
    float l = length(d);
    if (l > maxStep) d *= maxStep / l;
    grabPos += d;
    for (size_t n = 0; n < grabbedIds.size(); n++) {
        int k = grabbedIds[n];
        prev[k] = pos[k];
        pos[k] = grabPos + grabOffsets[n];
    }
}

void Cloth::solveConstraints(float sdt) {
    float inv = 1 / (sdt * sdt);
    float3* p = pos.data();
    const float* w = invMass.data();
    for (const Con& c : cons) {
        int a = c.a, b = c.b;
        float wa = w[a], wb = w[b];
        float ws = wa + wb;
        if (ws == 0) continue;
        float3 d = p[b] - p[a];
        float len = length(d);
        if (len < 1e-6f) continue;
        float s = -(len - c.rest) / (ws + c.compliance * inv);
        float3 nd = d / len;
        p[a] -= nd * (s * wa);
        p[b] += nd * (s * wb);
    }
}

// Points held by the mouse collide too: they have infinite mass, only the neighbour moves.
void Cloth::solveSelfCollisions() {
    float t = thickness, t2 = t * t;
    float fr = params.selfFriction;
    float3* p = pos.data();
    const float3* q = prev.data();
    const int32_t* firstAdj = hash->firstAdj.data();
    const int32_t* adj = hash->adj.data();
    for (int i = 0; i < count; i++) {
        int first = firstAdj[i], last = firstAdj[i + 1];
        if (first == last) continue;
        float wi = invMass[i];
        for (int k = first; k < last; k++) {
            int j = adj[k];
            float wj = invMass[j];
            float w = wi + wj;
            if (w == 0) continue;
            float3 d = p[j] - p[i];
            float dist2 = length2(d);
            if (dist2 > t2 || dist2 == 0) continue;
            float restDist2 = length2(rest[i] - rest[j]);
            if (dist2 > restDist2) continue;
            float minDist = t;
            if (restDist2 < t2) minDist = std::sqrt(restDist2);
            float dist = std::sqrt(dist2);
            d *= (minDist - dist) / (dist * w);
            p[i] -= d * wi;
            p[j] += d * wj;
            float3 v0 = p[i] - q[i], v1 = p[j] - q[j];
            float3 avg = (v0 * wj + v1 * wi) / w;
            p[i] += (avg - v0) * (fr * wi / w * 2);
            p[j] += (avg - v1) * (fr * wj / w * 2);
        }
    }
}

void Cloth::solveGround() {
    float ms = params.groundStatic, mk = params.groundKinetic;
    for (int i = 0; i < count; i++) {
        if (invMass[i] == 0) continue;
        float3 p = pos[i];
        float h = ground ? ground->sample(p.x, p.y) : 0;
        if (p.z >= h) continue;
        float pen = h - p.z;
        p.z = h;
        float3 q = prev[i];
        float dx = p.x - q.x, dy = p.y - q.y;
        float l = std::sqrt(dx * dx + dy * dy);
        if (l < ms * pen) {
            p.x = q.x;
            p.y = q.y;
        } else if (l > 0) {
            float k = std::min(mk * pen / l, 1.f);
            p.x -= dx * k;
            p.y -= dy * k;
        }
        pos[i] = p;
    }
}

// MARK: derived data for rendering and the mouse

void Cloth::updateDerived(const Camera& cam) {
    for (int i = 0; i < count; i++) nrm[i] = float3();
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        float3 n = cross(pos[b] - pos[a], pos[c] - pos[a]);
        nrm[a] += n;
        nrm[b] += n;
        nrm[c] += n;
    }
    float2 lo(FLT_MAX), hi(-FLT_MAX);
    for (int i = 0; i < count; i++) {
        float l = length(nrm[i]);
        nrm[i] = l > 0 ? nrm[i] / l : float3(0, 0, 1);
        float2 s = cam.project(pos[i]);
        proj[i] = s;
        lo = vmin(lo, s);
        hi = vmax(hi, s);
    }
    bboxMin = lo;
    bboxMax = hi;
}

bool Cloth::pick(float2 s, bool needVertex, int* vertex, float* z) const {
    if (s.x < bboxMin.x || s.y < bboxMin.y || s.x > bboxMax.x || s.y > bboxMax.y) return false;
    float bestZ = -FLT_MAX;
    int bestV = -1;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        float2 pa = proj[a], pb = proj[b], pc = proj[c];
        if (s.x < std::min({pa.x, pb.x, pc.x}) || s.x > std::max({pa.x, pb.x, pc.x})) continue;
        if (s.y < std::min({pa.y, pb.y, pc.y}) || s.y > std::max({pa.y, pb.y, pc.y})) continue;
        float2 v0 = pb - pa, v1 = pc - pa, v2 = s - pa;
        float den = v0.x * v1.y - v1.x * v0.y;
        if (std::abs(den) < 1e-6f) continue;
        float u = (v2.x * v1.y - v1.x * v2.y) / den;
        float v = (v0.x * v2.y - v2.x * v0.y) / den;
        if (u < 0 || v < 0 || u + v > 1) continue;
        float w = 1 - u - v;
        float zz = pos[a].z * w + pos[b].z * u + pos[c].z * v;
        if (zz > bestZ) {
            bestZ = zz;
            bestV = (w >= u && w >= v) ? a : (u >= v ? b : c);
            if (!needVertex) break;
        }
    }
    if (bestV < 0) return false;
    if (vertex) *vertex = bestV;
    if (z) *z = bestZ;
    return true;
}
