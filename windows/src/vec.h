#pragma once
#include <algorithm>
#include <cmath>

// Just enough vector math to stand in for Swift's simd types.

struct float2 {
    float x, y;
    float2() : x(0), y(0) {}
    float2(float x_, float y_) : x(x_), y(y_) {}
    explicit float2(float s) : x(s), y(s) {}
};

struct float3 {
    float x, y, z;
    float3() : x(0), y(0), z(0) {}
    float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    float2 xy() const { return {x, y}; }
};

inline float2 operator+(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
inline float2 operator-(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
inline float2 operator*(float2 a, float2 b) { return {a.x * b.x, a.y * b.y}; }
inline float2 operator/(float2 a, float2 b) { return {a.x / b.x, a.y / b.y}; }
inline float2 operator*(float2 a, float s) { return {a.x * s, a.y * s}; }
inline float2 operator*(float s, float2 a) { return {a.x * s, a.y * s}; }
inline float2 operator/(float2 a, float s) { return {a.x / s, a.y / s}; }
inline float2 operator-(float2 a) { return {-a.x, -a.y}; }
inline float2& operator+=(float2& a, float2 b) { return a = a + b; }
inline float2& operator-=(float2& a, float2 b) { return a = a - b; }
inline float2& operator*=(float2& a, float s) { return a = a * s; }
inline float2& operator/=(float2& a, float s) { return a = a / s; }
inline bool operator==(float2 a, float2 b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(float2 a, float2 b) { return !(a == b); }

inline float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator*(float s, float3 a) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator/(float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline float3 operator-(float3 a) { return {-a.x, -a.y, -a.z}; }
inline float3& operator+=(float3& a, float3 b) { return a = a + b; }
inline float3& operator-=(float3& a, float3 b) { return a = a - b; }
inline float3& operator*=(float3& a, float s) { return a = a * s; }

inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length2(float2 a) { return dot(a, a); }
inline float length2(float3 a) { return dot(a, a); }
inline float length(float2 a) { return std::sqrt(dot(a, a)); }
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { return a / length(a); }
inline float3 cross(float3 a, float3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float2 vmin(float2 a, float2 b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
inline float2 vmax(float2 a, float2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline float2 clamp(float2 v, float2 lo, float2 hi) { return vmin(vmax(v, lo), hi); }
