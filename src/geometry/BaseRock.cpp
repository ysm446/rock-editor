#include "geometry/BaseRock.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace rock::geometry {
namespace {
uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
double Sample(int x, int y, int z, int seed) {
    const auto hash = Mix(Mix(uint32_t(x)) ^ Mix(uint32_t(y) + 0x9e3779b9u) ^ Mix(uint32_t(z) + 0x85ebca6bu) ^
                          Mix(uint32_t(seed) + 0xc2b2ae35u));
    return double(hash) / double(UINT32_MAX) * 2 - 1;
}
double Noise(Vec3 direction, float scale, int seed) {
    const double x = double(direction.x) * scale, y = double(direction.y) * scale,
                 z = double(direction.z) * scale;
    const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    const auto smooth = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    const double tx = smooth(x - ix), ty = smooth(y - iy), tz = smooth(z - iz);
    double value = 0;
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
                value += Sample(ix + i, iy + j, iz + k, seed) * (i ? tx : 1 - tx) * (j ? ty : 1 - ty) *
                         (k ? tz : 1 - tz);
    return value;
}
Vec3 Normalize(Vec3 p) {
    const double length = std::sqrt(double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z);
    return {float(p.x / length), float(p.y / length), float(p.z / length)};
}
}  // namespace
const char* BaseShapeName(BaseShape shape) {
    switch (shape) {
        case BaseShape::Box:
            return "box";
        case BaseShape::RoundedBox:
            return "roundedBox";
        case BaseShape::Sphere:
            return "sphere";
        case BaseShape::Ellipsoid:
            return "ellipsoid";
        default:
            return "invalid";
    }
}
BaseShape ParseBaseShape(std::string_view name) {
    for (auto shape : {BaseShape::Box, BaseShape::RoundedBox, BaseShape::Sphere, BaseShape::Ellipsoid})
        if (name == BaseShapeName(shape)) return shape;
    return BaseShape::Invalid;
}
Mesh MakeBaseRock(const BaseRockSettings& s, std::string& error) {
    error.clear();
    const auto fail = [&](const char* text) {
        error = text;
        return Mesh{};
    };
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    for (float v : s.size)
        if (!range(v, 0.001f, 1000)) return fail("寸法は有限の 0.001～1000 m にしてください");
    if (s.shape != BaseShape::Box && s.shape != BaseShape::RoundedBox && s.shape != BaseShape::Sphere &&
        s.shape != BaseShape::Ellipsoid)
        return fail("母岩の形状が不明です");
    if (s.subdivisions != 4 && s.subdivisions != 8 && s.subdivisions != 16 && s.subdivisions != 32)
        return fail("分割数は 4 / 8 / 16 / 32 にしてください");
    if (!range(s.roundness, 0, 1) || !range(s.noiseStrength, 0, 0.15f) || !range(s.noiseScale, 0.5f, 4))
        return fail("丸みは0～1、ノイズ強度は0～0.15、ノイズ細かさは0.5～4にしてください");
    if (s.noiseStrength == 0 &&
        (s.shape == BaseShape::Box || (s.shape == BaseShape::RoundedBox && s.roundness == 0)))
        return MakeBox(s.size);
    const int n = s.subdivisions;
    const std::array<float, 3> half{s.size[0] * 0.5f, s.size[1] * 0.5f, s.size[2] * 0.5f};
    const float radius = *std::min_element(half.begin(), half.end()) * s.roundness;
    Mesh mesh;
    std::map<std::array<int, 3>, uint32_t> vertices;
    const auto vertex = [&](const std::array<int, 3>& key) {
        auto [it, inserted] = vertices.emplace(key, static_cast<uint32_t>(mesh.positions.size()));
        if (!inserted) return it->second;
        const std::array<float, 3> cube{2.0f * key[0] / n - 1, 2.0f * key[1] / n - 1, 2.0f * key[2] / n - 1};
        Vec3 p{cube[0] * half[0], cube[1] * half[1], cube[2] * half[2]};
        if (s.shape == BaseShape::Sphere || s.shape == BaseShape::Ellipsoid) {
            const auto unit = Normalize({cube[0], cube[1], cube[2]});
            p = {unit.x * half[0], unit.y * (s.shape == BaseShape::Sphere ? half[0] : half[1]),
                 unit.z * (s.shape == BaseShape::Sphere ? half[0] : half[2])};
        } else if (s.shape == BaseShape::RoundedBox && radius > 0) {
            const Vec3 inner{std::clamp(p.x, -half[0] + radius, half[0] - radius),
                             std::clamp(p.y, -half[1] + radius, half[1] - radius),
                             std::clamp(p.z, -half[2] + radius, half[2] - radius)};
            const auto unit = Normalize({p.x - inner.x, p.y - inner.y, p.z - inner.z});
            p = {inner.x + radius * unit.x, inner.y + radius * unit.y, inner.z + radius * unit.z};
        }
        if (s.noiseStrength > 0) {
            // 正の放射方向変位で、共有頂点と各面の原点に対する向きを保つ。
            const float factor = float(1 + s.noiseStrength * Noise(Normalize(p), s.noiseScale, s.seed));
            p = {p.x * factor, p.y * factor, p.z * factor};
        }
        mesh.positions.push_back(p);
        return it->second;
    };
    for (int axis = 0; axis < 3; ++axis)
        for (int side = 0; side < 2; ++side) {
            // 循環する2軸の外積は +axis。負側だけ面の順を反転する。
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    std::array<uint32_t, 4> ids;
                    for (int k = 0; k < 4; ++k) {
                        std::array<int, 3> key{};
                        key[axis] = side * n;
                        key[u] = i + (k == 1 || k == 2);
                        key[v] = j + (k >= 2);
                        ids[k] = vertex(key);
                    }
                    if (side == 0) std::swap(ids[1], ids[3]);
                    mesh.triangles.push_back({ids[0], ids[1], ids[2]});
                    mesh.triangles.push_back({ids[0], ids[2], ids[3]});
                }
        }
    MeshInfo info;
    if (!InspectMesh(mesh, info) || !info.closed || info.components != 1 || info.volume <= 0)
        return fail("母岩の閉包・体積・面の品質を確認できませんでした。寸法比や設定を見直してください");
    return mesh;
}
}  // namespace rock::geometry
