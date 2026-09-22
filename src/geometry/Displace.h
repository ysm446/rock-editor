#pragma once
#include "geometry/Mesh.h"
#include <functional>
#include <stop_token>
#include <string>

namespace rock::geometry {
inline constexpr size_t kMaxDetailTriangles = 4000000;
struct SubdivideSettings {
    int levels = 1;
    // マスクで割る面を選ぶときのしきい値。面の中心のマスク値がこれ以上なら割る。
    float threshold = .5f;
    bool operator==(const SubdivideSettings&) const = default;
};
struct DisplaceSettings {
    float amount = .05f, midpoint = .5f;
    bool operator==(const DisplaceSettings&) const = default;
};
using DetailProgress = std::function<void(int)>;
using HeightSample = std::function<float(Vec3 position, Vec3 normal, Mesh::Uv uv)>;
// faceMask は入力の面ごとのマスク値（0〜1）。空なら全面を割る。
// マスクで選んだ面は各段で4分割し、隣の面は共有辺の分割に合わせて2〜4分割して閉じたまま保つ（red-green）。
// 選ばれなかった面から生まれた面は、次の段でも割らない。
Mesh SubdivideMesh(const Mesh&, const SubdivideSettings&, std::string&, std::stop_token = {}, DetailProgress = {},
                   const std::vector<float>& faceMask = {});
Mesh DisplaceMesh(const Mesh&, const DisplaceSettings&, const HeightSample&, std::string&, std::stop_token = {}, DetailProgress = {});
}
