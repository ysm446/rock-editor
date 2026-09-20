#include "fracture/PlaneSplit.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace rock::fracture {
namespace {
using geometry::Mesh;
using geometry::Vec3;
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double Dot(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
// 差分から倍精度で符号付き面積を求める。
double Turn(Vec3 a, Vec3 b, Vec3 c, Vec3 n) {
    const auto u = Sub(b, a), v = Sub(c, a);
    return (double(u.y) * v.z - double(u.z) * v.y) * n.x + (double(u.z) * v.x - double(u.x) * v.z) * n.y +
           (double(u.x) * v.y - double(u.y) * v.x) * n.z;
}
Mesh Compact(const Mesh& mesh) {
    Mesh out;
    std::map<uint32_t, uint32_t> ids;
    for (auto face : mesh.triangles) {
        for (auto& id : face) {
            auto [it, added] = ids.emplace(id, static_cast<uint32_t>(out.positions.size()));
            if (added) out.positions.push_back(mesh.positions[id]);
            id = it->second;
        }
        out.triangles.push_back(face);
    }
    return out;
}
SplitResult Fail(const char* text) {
    SplitResult r;
    r.error = text;
    return r;
}
}  // namespace
SplitResult SplitByPlane(const Mesh& input, Vec3 center, Vec3 normal) {
    geometry::MeshInfo original;
    if (input.triangles.size() > 100000 || !geometry::InspectMesh(input, original) || !original.closed ||
        original.components != 1 || original.volume <= 0)
        return Fail("入力は10万三角形以下の閉じた1連結体にしてください");
    const double length = std::sqrt(Dot(normal, normal));
    if (!std::isfinite(length) || length < 1e-12 || !std::isfinite(center.x) || !std::isfinite(center.y) ||
        !std::isfinite(center.z))
        return Fail("分割平面が不正です");
    normal = {float(normal.x / length), float(normal.y / length), float(normal.z / length)};
    const auto extent = Sub(original.maximum, original.minimum);
    const double scale = std::max({extent.x, extent.y, extent.z});
    const double eps = scale * 1e-6, areaEps = eps * eps;
    std::vector<Vec3> points = input.positions;
    std::vector<double> distance;
    bool positive = false, negative = false;
    for (auto& p : points) {
        double d = Dot(Sub(p, center), normal);
        if (std::abs(d) <= eps) {
            p = {float(p.x - d * normal.x), float(p.y - d * normal.y), float(p.z - d * normal.z)};
            d = 0;
        }
        distance.push_back(d);
        positive |= d > 0;
        negative |= d < 0;
    }
    if (!positive || !negative) return Fail("平面が内部を横切りません。接触だけでは分割しません");
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> crossings;
    const auto crossing = [&](uint32_t a, uint32_t b) {
        if (distance[a] == 0) return a;
        if (distance[b] == 0) return b;
        const auto key = std::pair{std::min(a, b), std::max(a, b)};
        if (const auto it = crossings.find(key); it != crossings.end()) return it->second;
        const double t = distance[a] / (distance[a] - distance[b]);
        const auto pa = points[a], pb = points[b];
        const auto id = static_cast<uint32_t>(points.size());
        points.push_back({float(pa.x + (pb.x - pa.x) * t), float(pa.y + (pb.y - pa.y) * t),
                          float(pa.z + (pb.z - pa.z) * t)});
        crossings.emplace(key, id);
        return id;
    };
    SplitResult result;
    for (const auto& face : input.triangles) {
        if (distance[face[0]] == 0 && distance[face[1]] == 0 && distance[face[2]] == 0)
            return Fail("分割平面と既存の面が重なります。平面を少し移動してください");
        for (int side = 0; side < 2; ++side) {
            const double sign = side == 0 ? -1 : 1;
            std::vector<uint32_t> polygon;
            for (size_t k = 0; k < 3; ++k) {
                const auto a = face[k], b = face[(k + 1) % 3];
                const bool insideA = distance[a] * sign >= 0, insideB = distance[b] * sign >= 0;
                if (insideA) polygon.push_back(a);
                if (insideA != insideB) polygon.push_back(crossing(a, b));
            }
            polygon.erase(std::unique(polygon.begin(), polygon.end()), polygon.end());
            if (polygon.size() > 1 && polygon.front() == polygon.back()) polygon.pop_back();
            for (size_t k = 1; k + 1 < polygon.size(); ++k)
                result.meshes[side].triangles.push_back({polygon[0], polygon[k], polygon[k + 1]});
        }
    }
    // 負側の境界辺を逆向きにたどると、+normal を向く閉包の外周になる。
    struct Edge {
        int count = 0;
        uint32_t a = 0, b = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, Edge> edges;
    for (const auto& face : result.meshes[0].triangles)
        for (size_t k = 0; k < 3; ++k) {
            const auto a = face[k], b = face[(k + 1) % 3];
            auto& edge = edges[{std::min(a, b), std::max(a, b)}];
            ++edge.count;
            edge.a = a;
            edge.b = b;
        }
    std::map<uint32_t, uint32_t> next;
    std::set<uint32_t> ends;
    for (const auto& [key, edge] : edges) {
        if (edge.count > 2) return Fail("分割後の境界が非多様体です");
        if (edge.count == 1) {
            if (!next.emplace(edge.b, edge.a).second || !ends.insert(edge.a).second)
                return Fail("断面が分岐しています。平面を移動してください");
        }
    }
    if (next.size() < 3 || next.size() > 4096) return Fail("断面の頂点数は3～4096個にしてください");
    std::vector<uint32_t> loop;
    auto current = next.begin()->first;
    const auto start = current;
    do {
        loop.push_back(current);
        const auto it = next.find(current);
        if (it == next.end() || loop.size() > next.size()) return Fail("断面を閉じられません");
        current = it->second;
    } while (current != start);
    if (loop.size() != next.size()) return Fail("複数の断面ループや穴のある断面にはまだ対応していません");
    double signedArea = 0;
    for (size_t i = 1; i + 1 < loop.size(); ++i)
        signedArea += Turn(points[loop[0]], points[loop[i]], points[loop[i + 1]], normal) * 0.5;
    if (signedArea <= areaEps) return Fail("断面が小さすぎるか向きが不正です");
    result.sectionArea = signedArea;
    // 共線の境界頂点も保持する ear clipping。境界上の別頂点を跨ぐ耳は使わない。
    const auto quality = [&](uint32_t a, uint32_t b, uint32_t c) {
        const double area = Turn(points[a], points[b], points[c], normal);
        const auto ab = Sub(points[b], points[a]), ac = Sub(points[c], points[a]),
                   bc = Sub(points[c], points[b]);
        const double maxEdge = std::max({Dot(ab, ab), Dot(ac, ac), Dot(bc, bc)});
        return area > areaEps && maxEdge > 0 ? area / maxEdge : 0.0;
    };
    const auto insideEdge = [&](uint32_t a, uint32_t b, uint32_t p) {
        const auto edge = Sub(points[b], points[a]);
        return Turn(points[a], points[b], points[p], normal) >= -eps * std::sqrt(Dot(edge, edge));
    };
    while (loop.size() > 3) {
        size_t best = loop.size();
        double bestQuality = 1e-6;
        for (size_t i = 0; i < loop.size(); ++i) {
            const auto a = loop[(i + loop.size() - 1) % loop.size()], b = loop[i],
                       c = loop[(i + 1) % loop.size()];
            const double q = quality(a, b, c);
            if (q <= bestQuality) continue;
            bool occupied = false;
            for (const auto p : loop) {
                if (p == a || p == b || p == c) continue;
                if (insideEdge(a, b, p) && insideEdge(b, c, p) && insideEdge(c, a, p)) {
                    occupied = true;
                    break;
                }
            }
            if (occupied) continue;
            best = i;
            bestQuality = q;
        }
        if (best == loop.size()) return Fail("断面の三角形化に失敗しました。平面を移動してください");
        const auto a = loop[(best + loop.size() - 1) % loop.size()], b = loop[best],
                   c = loop[(best + 1) % loop.size()];
        result.meshes[0].triangles.push_back({a, b, c});
        result.meshes[1].triangles.push_back({c, b, a});
        loop.erase(loop.begin() + best);
    }
    result.meshes[0].triangles.push_back({loop[0], loop[1], loop[2]});
    result.meshes[1].triangles.push_back({loop[2], loop[1], loop[0]});
    double volume = 0;
    for (auto& mesh : result.meshes) {
        mesh.positions = points;
        mesh = Compact(mesh);
        geometry::MeshInfo info;
        if (!geometry::InspectMesh(mesh, info) || !info.closed || info.components != 1 ||
            info.volume <= original.volume * 1e-7)
            return Fail("分割後の閉包・連結性・最小体積を確認できませんでした");
        volume += info.volume;
    }
    if (std::abs(volume - original.volume) > original.volume * 1e-5)
        return Fail("分割前後の体積が一致しません");
    return result;
}
}  // namespace rock::fracture
