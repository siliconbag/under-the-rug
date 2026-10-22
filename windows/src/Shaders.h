#pragma once
// HLSL port of the Metal shaders. Compiled at startup from this string (D3DCompile), so no shader toolchain is needed.
static const char* kShaderSource = R"HLSL(
cbuffer Uniforms : register(b0) {
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
};

struct V { float4 pos; float4 nrm; };

StructuredBuffer<V> verts : register(t2);
StructuredBuffer<float2> uvs : register(t3);
Texture2D<float4> tex : register(t0);
Texture2D<float> sm : register(t1);
SamplerState ts : register(s0);
SamplerState ss : register(s1);

static const float2 poisson[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
};

float depthFromZ(float z) { return clamp(0.5 - z / 4000.0, 0.0, 1.0); }

float2 toTC(float2 f) { return float2(f.x / viewSize.x, 1.0 - f.y / viewSize.y); }

float ign(float2 px) { return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))); }

// Soft shadow from the height map (PCSS). p is the receiver point, gp the slope of the receiver plane in map space.
float shadowAt(float3 p, float2 gp, float bias, float2 px) {
    float2 f = p.xy - lightK * p.z;
    float a = ign(px) * 6.2831853;
    float ca = cos(a), sa = sin(a);
    float sumD = 0.0, nB = 0.0;
    [unroll] for (int i = 0; i < 16; i++) {
        float2 q = poisson[i];
        float2 o = float2(ca * q.x - sa * q.y, sa * q.x + ca * q.y) * 34.0;
        float zr = p.z + dot(gp, o);
        float occ = sm.SampleLevel(ss, toTC(f + o), 0);
        if (occ > zr + bias) { sumD += occ - zr; nB += 1.0; }
    }
    if (nB < 0.5) return 0.0;
    float r = clamp(2.5 + (sumD / nB) * 0.2, 2.5, 30.0);
    float sh = 0.0;
    [unroll] for (int j = 0; j < 16; j++) {
        float2 q = poisson[j];
        float2 o = float2(ca * q.x - sa * q.y, sa * q.x + ca * q.y) * r;
        float zr = p.z + dot(gp, o);
        float occ = sm.SampleLevel(ss, toTC(f + o), 0);
        sh += (occ > zr + bias) ? 1.0 : 0.0;
    }
    return sh / 16.0;
}

// Pass 1. Height map of the occluders, seen from the light onto the floor.
struct ShadowOut { float4 position : SV_Position; float z : TEXCOORD0; };

ShadowOut shadow_vs(uint vid : SV_VertexID) {
    float3 p = verts[vid].pos.xyz;
    float2 f = p.xy - lightK * p.z;
    float2 c = viewSize * 0.5;
    ShadowOut o;
    o.position = float4((f - c) / c, depthFromZ(p.z), 1.0);
    o.z = p.z;
    return o;
}

float4 shadow_fs(ShadowOut i) : SV_Target { return float4(i.z, 0.0, 0.0, 1.0); }

// Pass 2. Shadow on the floor (the wallpaper under the window), drawn only in a box around the rug.
struct FloorOut { float4 position : SV_Position; float2 xy : TEXCOORD0; };

FloorOut floor_vs(uint vid : SV_VertexID) {
    float4 r = floorRect;
    float2 xy = float2((vid & 1) ? r.z : r.x, (vid & 2) ? r.w : r.y);
    float2 c = viewSize * 0.5;
    FloorOut o;
    o.position = float4((xy - c) / c, 0.999, 1.0);
    o.xy = xy;
    return o;
}

float4 floor_fs(FloorOut i) : SV_Target {
    float s = shadowAt(float3(i.xy, floorZ), float2(0.0, 0.0), 0.5, i.position.xy);
    float a = s * shadowStrength;
    return float4(0.035, 0.02, 0.01, 1.0) * a;
}

// Background for demo frames, the wallpaper filling the screen.
struct BgOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };

BgOut bg_vs(uint vid : SV_VertexID) {
    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    BgOut o;
    o.position = float4(c, 0.999, 1.0);
    o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    return o;
}

float4 bg_fs(BgOut i) : SV_Target {
    uint tw, th;
    tex.GetDimensions(tw, th);
    float ta = float(tw) / float(th);
    float va = viewSize.x / viewSize.y;
    float2 uv = i.uv;
    if (ta > va) uv.x = 0.5 + (uv.x - 0.5) * va / ta; else uv.y = 0.5 + (uv.y - 0.5) * ta / va;
    return float4(tex.Sample(ts, uv).rgb, 1.0);
}

// Pass 3. The rug itself.
struct RugOut {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

RugOut rug_vs(uint vid : SV_VertexID) {
    float3 p = verts[vid].pos.xyz;
    float2 c = viewSize * 0.5;
    float w = (camD - p.z) / camD;
    RugOut o;
    o.position = float4((p.xy - c) / c, depthFromZ(p.z) * w, w);
    o.world = p;
    o.nrm = verts[vid].nrm.xyz;
    o.uv = uvs[vid];
    return o;
}

float4 rug_fs(RugOut i, bool front : SV_IsFrontFace) : SV_Target {
    float3 Ng = normalize(i.nrm);
    float3 N = front ? Ng : -Ng;
    float3 col = tex.Sample(ts, i.uv).rgb;

    // pile, relief from brightness
    float h = dot(col, float3(0.299, 0.587, 0.114));
    float2 g = float2(ddx(h), -ddy(h)) * (front ? 1.6 : 0.6);
    float3 Nd = normalize(N - float3(g, 0.0));

    if (!front) {
        float gray = dot(col, float3(0.299, 0.587, 0.114));
        col = lerp(col, gray.xxx, 0.4) * 0.8 + float3(0.05, 0.04, 0.03);
        float2 k = frac(i.uv * float2(260.0, 175.0));
        float knot = smoothstep(0.0, 0.18, min(min(k.x, 1.0 - k.x), min(k.y, 1.0 - k.y)));
        col *= lerp(0.86, 1.0, knot);
    }

    // darker binding along the edge
    float2 e2 = min(i.uv, 1.0 - i.uv) * rugSize;
    float e = min(e2.x, e2.y);
    col *= lerp(0.62, 1.0, smoothstep(0.4, 3.0, e));

    float3 L = lightDir.xyz;
    float amb = 0.28, kd = 0.95;
    float shade = (amb + kd * max(dot(Nd, L), 0.0)) / (amb + kd * L.z);

    // self-shadowing in folds, bias along the receiver plane
    float2 gp = float2(0.0, 0.0);
    float bias = 1.5;
    if (abs(Ng.z) > 0.2) {
        float2 gg = -Ng.xy / Ng.z;
        float den = 1.0 - dot(gg, lightK);
        if (abs(den) > 0.25) gp = clamp(gg / den, -3.0, 3.0); else bias = 5.0;
    } else {
        bias = 5.0;
    }
    float sh = shadowAt(i.world, gp, bias, i.position.xy);
    shade *= 1.0 - 0.55 * sh;

    return float4(min(col * min(shade, 1.3), 1.0), 1.0);
}
)HLSL";
