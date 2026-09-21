#include "geometry/Mesh.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <numeric>

namespace rock::geometry {
namespace {
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double Dot(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
}  // namespace
Mesh MakeBox(const std::array<float, 3>& size) {
    for (float v : size)
        if (!std::isfinite(v) || v < 0.001f || v > 1000.0f) return {};
    const float x = size[0] * 0.5f, y = size[1] * 0.5f, z = size[2] * 0.5f;
    Mesh mesh;
    mesh.positions = {{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z},
                      {-x, -y, z},  {x, -y, z},  {x, y, z},  {-x, y, z}};
    mesh.triangles = {{{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}}, {{0, 4, 7}}, {{0, 7, 3}},
                      {{1, 2, 6}}, {{1, 6, 5}}, {{0, 1, 5}}, {{0, 5, 4}}, {{3, 7, 6}}, {{3, 6, 2}}};
    return mesh;
}
Vec3 FaceNormal(const Mesh& mesh, const std::array<uint32_t, 3>& f) {
    Vec3 n = Cross(Sub(mesh.positions[f[1]], mesh.positions[f[0]]),
                   Sub(mesh.positions[f[2]], mesh.positions[f[0]]));
    const float length = static_cast<float>(std::sqrt(Dot(n, n)));
    return length > 0 ? Vec3{n.x / length, n.y / length, n.z / length} : Vec3{};
}
bool InspectMesh(const Mesh& mesh, MeshInfo& info) {
    info = {};
    if (mesh.positions.empty() || mesh.triangles.empty()) return false;
    info.minimum = info.maximum = mesh.positions.front();
    for (auto p : mesh.positions) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        info.minimum = {std::min(info.minimum.x, p.x), std::min(info.minimum.y, p.y),
                        std::min(info.minimum.z, p.z)};
        info.maximum = {std::max(info.maximum.x, p.x), std::max(info.maximum.y, p.y),
                        std::max(info.maximum.z, p.z)};
    }
    std::vector<size_t> parent(mesh.positions.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    std::vector<bool> used(mesh.positions.size());
    // 頂点IDの組を64bitに詰めて照合する。辺の順序は検証結果に影響しない。
    std::unordered_map<uint64_t, std::pair<int, int>> edges;
    edges.reserve(mesh.triangles.size() * 3 / 2);
    for (const auto& f : mesh.triangles) {
        for (auto i : f)
            if (i >= mesh.positions.size()) return false;
        const Vec3 a = mesh.positions[f[0]], b = mesh.positions[f[1]], c = mesh.positions[f[2]];
        const Vec3 ab = Sub(b, a), ac = Sub(c, a), n = Cross(ab, ac);
        // 面積は辺長に対する相対値で判定し、小さな正常 Box を拒否しない。
        if (Dot(n, n) <= 1e-12 * Dot(ab, ab) * Dot(ac, ac)) return false;
        // 原点から遠い Chunk でも桁落ちしないよう、メッシュ上の点を基準に積分する。
        const auto va = Sub(a, mesh.positions.front()), vb = Sub(b, mesh.positions.front()),
                   vc = Sub(c, mesh.positions.front());
        info.volume += (double(va.x) * (double(vb.y) * vc.z - double(vb.z) * vc.y) +
                        double(va.y) * (double(vb.z) * vc.x - double(vb.x) * vc.z) +
                        double(va.z) * (double(vb.x) * vc.y - double(vb.y) * vc.x)) /
                       6.0;
        for (size_t k = 0; k < 3; ++k) {
            const auto u = f[k], v = f[(k + 1) % 3];
            used[u] = true;
            parent[root(u)] = root(v);
            const auto key = (uint64_t(std::min(u, v)) << 32) | std::max(u, v);
            auto& edge = edges[key];
            ++edge.first;
            edge.second += u < v ? 1 : -1;
        }
    }
    info.closed = true;
    for (const auto& [key, edge] : edges)
        if (edge.first != 2 || edge.second != 0) info.closed = false;
    for (size_t i = 0; i < parent.size(); ++i)
        if (used[i] && root(i) == i) ++info.components;
    return true;
}
}  // namespace rock::geometry
