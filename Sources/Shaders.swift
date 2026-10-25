// Шейдеры компилируются на старте из строки, чтобы не тащить metal-тулчейн.
let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4 lightDir;
    float4 floorRect;
    float2 viewSize;
    float2 lightK;
    float2 rugSize;
    float2 shadowMapSize;
    float camD;
    float floorZ;
    float shadowStrength;
    float time;
    float projectionLift;
};

struct V { float4 pos; float4 nrm; };

constant float2 poisson[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
};

static float depthFromZ(float z) { return clamp(0.5 - z / 4000.0, 0.0, 1.0); }

static float2 toTC(float2 f, constant Uniforms& u) {
    return float2(f.x / u.viewSize.x, 1.0 - f.y / u.viewSize.y);
}

static float ign(float2 px) {
    return fract(52.9829189 * fract(dot(px, float2(0.06711056, 0.00583715))));
}

// Мягкая тень от карты высот (PCSS). p точка приемника, gp наклон плоскости приемника в пространстве карты.
static float shadowAt(float3 p, float2 gp, float bias, float2 px,
                      texture2d<float> sm, sampler s, constant Uniforms& u) {
    float2 f = p.xy - u.lightK * p.z;
    float a = ign(px) * 6.2831853;
    float2x2 R = float2x2(float2(cos(a), sin(a)), float2(-sin(a), cos(a)));
    float sumD = 0.0, nB = 0.0;
    for (int i = 0; i < 16; i++) {
        float2 o = R * poisson[i] * 34.0;
        float zr = p.z + dot(gp, o);
        float occ = sm.sample(s, toTC(f + o, u)).r;
        if (occ > zr + bias) { sumD += occ - zr; nB += 1.0; }
    }
    if (nB < 0.5) return 0.0;
    float r = clamp(2.5 + (sumD / nB) * 0.2, 2.5, 30.0);
    float sh = 0.0;
    for (int i = 0; i < 16; i++) {
        float2 o = R * poisson[i] * r;
        float zr = p.z + dot(gp, o);
        float occ = sm.sample(s, toTC(f + o, u)).r;
        sh += (occ > zr + bias) ? 1.0 : 0.0;
    }
    return sh / 16.0;
}

// Проход 1. Карта высот затеняющих точек, вид из источника света на пол.
struct ShadowOut { float4 position [[position]]; float z; };

vertex ShadowOut shadow_vs(uint vid [[vertex_id]],
                           const device V* verts [[buffer(0)]],
                           constant Uniforms& u [[buffer(2)]]) {
    float3 p = verts[vid].pos.xyz;
    float2 f = p.xy - u.lightK * p.z;
    float2 c = u.viewSize * 0.5;
    ShadowOut o;
    o.position = float4((f - c) / c, depthFromZ(p.z), 1.0);
    o.z = p.z;
    return o;
}

fragment float4 shadow_fs(ShadowOut in [[stage_in]]) { return float4(in.z, 0.0, 0.0, 1.0); }

// Проход 2. Тень на полу (обои под окном), рисуется только в рамке вокруг ковра.
struct FloorOut { float4 position [[position]]; float2 xy; };

vertex FloorOut floor_vs(uint vid [[vertex_id]], constant Uniforms& u [[buffer(2)]]) {
    float4 r = u.floorRect;
    float2 xy = float2((vid & 1) ? r.z : r.x, (vid & 2) ? r.w : r.y);
    float2 c = u.viewSize * 0.5;
    FloorOut o;
    o.position = float4((xy - c) / c, 0.999, 1.0);
    o.xy = xy;
    return o;
}

fragment float4 floor_fs(FloorOut in [[stage_in]],
                         texture2d<float> sm [[texture(1)]],
                         sampler ss [[sampler(1)]],
                         constant Uniforms& u [[buffer(2)]]) {
    float s = shadowAt(float3(in.xy, u.floorZ), float2(0.0), 0.5, in.position.xy, sm, ss, u);
    float a = s * u.shadowStrength;
    return float4(0.035, 0.02, 0.01, 1.0) * a;
}

// Фон для демо-кадров, обои с заполнением экрана.
struct BgOut { float4 position [[position]]; float2 uv; };

vertex BgOut bg_vs(uint vid [[vertex_id]]) {
    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    BgOut o;
    o.position = float4(c, 0.999, 1.0);
    o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    return o;
}

fragment float4 bg_fs(BgOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                      sampler ts [[sampler(0)]], constant Uniforms& u [[buffer(2)]]) {
    float ta = float(tex.get_width()) / float(tex.get_height());
    float va = u.viewSize.x / u.viewSize.y;
    float2 uv = in.uv;
    if (ta > va) uv.x = 0.5 + (uv.x - 0.5) * va / ta; else uv.y = 0.5 + (uv.y - 0.5) * ta / va;
    return float4(tex.sample(ts, uv).rgb, 1.0);
}

// Проход 3. Сам ковер.
struct RugOut {
    float4 position [[position]];
    float3 world;
    float3 nrm;
    float2 uv;
};

vertex RugOut rug_vs(uint vid [[vertex_id]],
                     const device V* verts [[buffer(0)]],
                     const device float2* uvs [[buffer(1)]],
                     constant Uniforms& u [[buffer(2)]]) {
    float3 p = verts[vid].pos.xyz;
    float2 c = u.viewSize * 0.5;
    float w = (u.camD - p.z + u.projectionLift) / u.camD;
    RugOut o;
    o.position = float4((p.xy - c) / c, depthFromZ(p.z) * w, w);
    o.world = p;
    o.nrm = verts[vid].nrm.xyz;
    o.uv = uvs[vid];
    return o;
}

fragment float4 rug_fs(RugOut in [[stage_in]],
                       bool front [[front_facing]],
                       texture2d<float> tex [[texture(0)]],
                       texture2d<float> sm [[texture(1)]],
                       sampler ts [[sampler(0)]],
                       sampler ss [[sampler(1)]],
                       constant Uniforms& u [[buffer(2)]]) {
    float3 Ng = normalize(in.nrm);
    float3 N = front ? Ng : -Ng;
    float3 col = tex.sample(ts, in.uv).rgb;

    // ворс, рельеф из яркости
    float h = dot(col, float3(0.299, 0.587, 0.114));
    float2 g = float2(dfdx(h), -dfdy(h)) * (front ? 1.6 : 0.6);
    float3 Nd = normalize(N - float3(g, 0.0));

    if (!front) {
        float gray = dot(col, float3(0.299, 0.587, 0.114));
        col = mix(col, float3(gray), 0.4) * 0.8 + float3(0.05, 0.04, 0.03);
        float2 k = fract(in.uv * float2(260.0, 175.0));
        float knot = smoothstep(0.0, 0.18, min(min(k.x, 1.0 - k.x), min(k.y, 1.0 - k.y)));
        col *= mix(0.86, 1.0, knot);
    }

    // окантовка по краю
    float2 e2 = min(in.uv, 1.0 - in.uv) * u.rugSize;
    float e = min(e2.x, e2.y);
    col *= mix(0.62, 1.0, smoothstep(0.4, 3.0, e));

    float3 L = u.lightDir.xyz;
    float amb = 0.28, kd = 0.95;
    float shade = (amb + kd * max(dot(Nd, L), 0.0)) / (amb + kd * L.z);

    // самозатенение складками, смещение по плоскости приемника
    float2 gp = float2(0.0);
    float bias = 1.5;
    if (abs(Ng.z) > 0.2) {
        float2 gg = -Ng.xy / Ng.z;
        float den = 1.0 - dot(gg, u.lightK);
        if (abs(den) > 0.25) gp = clamp(gg / den, -3.0, 3.0); else bias = 5.0;
    } else {
        bias = 5.0;
    }
    float sh = shadowAt(in.world, gp, bias, in.position.xy, sm, ss, u);
    shade *= 1.0 - 0.55 * sh;

    return float4(min(col * min(shade, 1.3), 1.0), 1.0);
}
"""
