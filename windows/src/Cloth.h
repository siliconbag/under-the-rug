#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "HeightField.h"
#include "SpatialHash.h"
#include "vec.h"

// Rug cloth. XPBD with substeps (as in Ten Minute Physics), floor with Coulomb friction,
// icon bumps via a height map, self-collisions via a spatial hash.
// Coordinates are screen points (DIPs), x right, y up, z towards the viewer.

struct Con {
    int32_t a;
    int32_t b;
    float rest;
    float compliance;
};

struct ClothParams {
    float gravity = 5000;
    int substeps = 10;  // per 1/60 s
    float shearCompliance = 2e-7f;
    float bendCompliance = 3e-7f;
    // Bending across 4 nodes, negative disables it. On by default on Windows: it keeps a heavy rug from rolling
    // into a tube or crumpling into a ball after a fast drag, so it comes to rest in seconds.
    float farBendCompliance = 1e-6f;
    float airDamping = 1.3f;
    float groundStatic = 2;
    float groundKinetic = 1.3f;
    float selfFriction = 0.3f;
    int columns = 44;
    float handFollow = 60;  // how fast the grabbed point closes in on the cursor, 1/s
    int maxSplit = 2;       // substeps may be split this many times so a fast hand doesn't tunnel through layers

    // for tuning in the demo: RUG_G, RUG_BEND, RUG_FAR, RUG_DAMP, RUG_FS, RUG_FK, RUG_COLS, RUG_FOLLOW, RUG_SPLIT
    static ClothParams tuned();
};

struct Camera {
    float2 size;
    float D() const { return std::max(size.x, size.y) * 1.15f; }
    float2 center() const { return size * 0.5f; }

    float2 project(float3 p) const {
        float w = (D() - p.z) / D();
        return center() + (p.xy() - center()) / w;
    }

    float3 unproject(float2 s, float z) const {
        float w = (D() - z) / D();
        float2 xy = center() + (s - center()) * w;
        return {xy.x, xy.y, z};
    }
};

class Cloth {
public:
    Cloth(float width, float height, int columns);

    int nx, ny, count;
    float width, height, spacing, thickness;
    ClothParams params = ClothParams::tuned();

    std::vector<float3> pos, prev, vel, rest, nrm;
    std::vector<float2> proj;
    std::vector<float> invMass;
    std::vector<uint32_t> indices;
    std::vector<float2> uvs;
    HeightField* ground = nullptr;

    float2 bboxMin, bboxMax;
    float maxSpeed = 0;
    float3 grabTarget;
    bool isGrabbing() const { return !grabbedIds.empty(); }
    // how far the grabbed point trails behind the cursor
    float handLag() const { return isGrabbing() ? length(grabTarget - grabPos) : 0; }

    int idx(int i, int j) const { return j * nx + i; }

    void layout(float2 center, float angle, float drop);
    struct Pose {
        float2 center;
        float angle;
    };
    Pose pose() const;
    void rotate(float angle);

    void beginGrab(int v, float3 target);
    void endGrab();

    void step(float frameDt);
    // True once the rug has come to rest. Judged by how far the nodes travel over a quarter second, not by speed:
    // a node caught between layers can twitch back and forth forever without going anywhere.
    bool settled(float frameDt);
    void updateDerived(const Camera& cam);
    // Topmost triangle under a screen point. Returns its nearest vertex and the height there.
    bool pick(float2 s, bool needVertex, int* vertex = nullptr, float* z = nullptr) const;

private:
    std::vector<Con> cons;
    std::unique_ptr<SpatialHash> hash;
    std::vector<int> grabbedIds;
    std::vector<float3> grabOffsets;
    float3 grabPos;
    std::vector<float> tetherRest;
    double substepDebt = 0;
    std::vector<float3> calmRef;
    float calmClock = 0, restClock = 0;
    int calmWindows = 0;

    void addCon(int a, int b, float c);
    void solveTethers();
    void integrate(float sdt, float maxVel);
    void moveGrabbed(float sdt, float follow);
    void solveConstraints(float sdt);
    void solveSelfCollisions();
    void solveGround();
};
