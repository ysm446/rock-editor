#include "fracture/MultiSplit.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace rock::fracture {
namespace {
double Distance(geometry::Vec3 p, const SplitPlane& plane) {
    return (double(p.x) - plane.center.x) * plane.normal.x + (double(p.y) - plane.center.y) * plane.normal.y +
           (double(p.z) - plane.center.z) * plane.normal.z;
}
double Dot(geometry::Vec3 a, geometry::Vec3 b) {
    return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
}
MultiSplitResult Fail(std::string error) {
    MultiSplitResult r;
    r.error = std::move(error);
    return r;
}
double FaceArea(const geometry::Mesh& mesh, const SplitPlane& plane, double eps) {
    double sum = 0;
    for (const auto& t : mesh.triangles) {
        const auto a = mesh.positions[t[0]], b = mesh.positions[t[1]], c = mesh.positions[t[2]];
        if (std::abs(Distance(a, plane)) > eps || std::abs(Distance(b, plane)) > eps ||
            std::abs(Distance(c, plane)) > eps)
            continue;
        const double ux = double(b.x) - a.x, uy = double(b.y) - a.y, uz = double(b.z) - a.z;
        const double vx = double(c.x) - a.x, vy = double(c.y) - a.y, vz = double(c.z) - a.z;
        sum += std::abs((uy * vz - uz * vy) * plane.normal.x + (uz * vx - ux * vz) * plane.normal.y +
                        (ux * vy - uy * vx) * plane.normal.z) *
               0.5;
    }
    return sum;
}
}  // namespace
MultiSplitResult SplitByPlanes(const geometry::Mesh& input, const std::vector<SplitPlane>& requested) {
    geometry::MeshInfo original;
    if (input.triangles.size() > 100000 || !geometry::InspectMesh(input, original) || !original.closed ||
        original.components != 1 || original.volume <= 0)
        return Fail("入力は10万三角形以下の閉じた1連結体にしてください");
    if (requested.empty() || requested.size() > 32)
        return Fail("Joint Set の完全分割は合計1～32平面にしてください");
    const double scale =
        std::max({original.maximum.x - original.minimum.x, original.maximum.y - original.minimum.y,
                  original.maximum.z - original.minimum.z});
    const double eps = scale * 1e-6;
    MultiSplitResult result;
    std::set<std::string> keys;
    for (auto plane : requested) {
        const double length = std::sqrt(Dot(plane.normal, plane.normal));
        if (!std::isfinite(length) || length < 1e-12 || !std::isfinite(plane.center.x) ||
            !std::isfinite(plane.center.y) || !std::isfinite(plane.center.z) || plane.key.empty() ||
            !keys.insert(plane.key).second)
            return Fail("平面または識別キーが不正です");
        plane.normal = {float(plane.normal.x / length), float(plane.normal.y / length),
                        float(plane.normal.z / length)};
        // 同一平面は最初の定義を使う。反転した法線も重複として扱う。
        if (std::any_of(result.planes.begin(), result.planes.end(), [&](const auto& other) {
                const double sign = Dot(plane.normal, other.normal) < 0 ? -1 : 1;
                const double dx = plane.normal.x - sign * other.normal.x,
                             dy = plane.normal.y - sign * other.normal.y,
                             dz = plane.normal.z - sign * other.normal.z;
                return dx * dx + dy * dy + dz * dz <= 1e-12 && std::abs(Distance(plane.center, other)) <= eps;
            }))
            continue;
        result.planes.push_back(std::move(plane));
    }
    struct Working {
        geometry::Mesh mesh;
        std::string sides;
    };
    std::vector<Working> pieces{{input, {}}};
    for (size_t pi = 0; pi < result.planes.size(); ++pi) {
        const auto& plane = result.planes[pi];
        std::vector<Working> next;
        size_t triangles = 0;
        for (auto& piece : pieces) {
            geometry::MeshInfo info;
            geometry::InspectMesh(piece.mesh, info);
            const double localEps =
                std::max({info.maximum.x - info.minimum.x, info.maximum.y - info.minimum.y,
                          info.maximum.z - info.minimum.z}) *
                1e-6;
            bool positive = false, negative = false;
            for (const auto p : piece.mesh.positions) {
                const double d = Distance(p, plane);
                positive |= d > localEps;
                negative |= d < -localEps;
            }
            if (positive && negative) {
                auto split = SplitByPlane(piece.mesh, plane.center, plane.normal);
                if (!split.error.empty()) return Fail("平面 " + plane.key + ": " + split.error);
                for (int side = 0; side < 2; ++side) {
                    geometry::MeshInfo child;
                    geometry::InspectMesh(split.meshes[side], child);
                    if (child.volume <= original.volume * 1e-7)
                        return Fail("母岩に対して小さすぎる片が発生します。間隔・角度を調整してください");
                    triangles += split.meshes[side].triangles.size();
                    next.push_back({std::move(split.meshes[side]), piece.sides + (side == 0 ? '-' : '+')});
                }
            } else {
                triangles += piece.mesh.triangles.size();
                next.push_back({std::move(piece.mesh), piece.sides + (positive ? '+' : '-')});
            }
            if (next.size() > 128 || triangles > 200000)
                return Fail("生成量の上限（128片・合計20万三角形）を超えます。平面数を減らしてください");
        }
        pieces = std::move(next);
    }
    if (pieces.size() < 2) return Fail("節理平面が母岩の内部を横切りません");
    double volume = 0;
    std::vector<std::vector<double>> areas(pieces.size(), std::vector<double>(result.planes.size()));
    for (size_t i = 0; i < pieces.size(); ++i) {
        geometry::MeshInfo info;
        if (!geometry::InspectMesh(pieces[i].mesh, info) || !info.closed || info.components != 1 ||
            info.volume <= 0)
            return Fail("最終片の閉包・体積を確認できません");
        volume += info.volume;
        for (size_t p = 0; p < result.planes.size(); ++p)
            areas[i][p] = FaceArea(pieces[i].mesh, result.planes[p], eps * 2);
    }
    if (std::abs(volume - original.volume) > original.volume * 1e-4)
        return Fail("多片分割前後の体積が一致しません");
    for (size_t a = 0; a < pieces.size(); ++a)
        for (size_t b = a + 1; b < pieces.size(); ++b) {
            size_t differences = 0, plane = 0;
            for (size_t p = 0; p < result.planes.size(); ++p)
                if (pieces[a].sides[p] != pieces[b].sides[p]) {
                    ++differences;
                    plane = p;
                }
            if (differences != 1) continue;
            const double aa = areas[a][plane], ab = areas[b][plane];
            if (std::max(aa, ab) <= eps * eps) continue;
            if (std::abs(aa - ab) > std::max(aa, ab) * 1e-4)
                return Fail("隣接する片の共有断面積が一致しません");
            const bool aNegative = pieces[a].sides[plane] == '-';
            result.connections.push_back({aNegative ? a : b, aNegative ? b : a, plane, (aa + ab) * 0.5});
        }
    for (auto& piece : pieces) {
        std::string key;
        for (size_t p = 0; p < result.planes.size(); ++p) key += result.planes[p].key + piece.sides[p] + ";";
        result.pieces.push_back({std::move(piece.mesh), std::move(key)});
    }
    return result;
}
}  // namespace rock::fracture
