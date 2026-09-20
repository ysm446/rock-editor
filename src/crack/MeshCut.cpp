#include "crack/MeshCut.h"
#include "fracture/MultiSplit.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
namespace rock::crack {
namespace {
using geometry::Vec3;
double Dot(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Offset(Vec3 p, Vec3 n, float d) { return {p.x + n.x * d, p.y + n.y * d, p.z + n.z * d}; }
PartialCutResult Fail(const std::string& error) {
    PartialCutResult r;
    r.error = error;
    return r;
}
}  // namespace
PartialCutResult CutMesh(const geometry::Mesh& input, const CrackSettings& s) {
    geometry::MeshInfo original;
    if (input.triangles.size() > 4096 || !geometry::InspectMesh(input, original) || !original.closed ||
        original.components != 1 || original.volume <= 0)
        return Fail("Mesh 部分切断は4096三角形以下の閉じた未分割岩1個にしてください");
    CrackPatch patch;
    std::string error;
    if (!BuildCrackPatch(s, patch, error)) return Fail(error);
    PartialCutResult result;
    result.mesh = input;
    if (s.aperture == 0 || patch.effectiveDepth == 0) {
        result.status = "開口または深さがゼロのため形状は変更しません";
        return result;
    }
    const double scale =
        std::max({original.maximum.x - original.minimum.x, original.maximum.y - original.minimum.y,
                  original.maximum.z - original.minimum.z});
    const double eps = scale * 1e-6;
    double minU = 1e30, maxU = -1e30, minV = 1e30, maxV = -1e30, minN = 1e30, maxN = -1e30;
    for (auto p : input.positions) {
        p = Sub(p, patch.center);
        const double u = Dot(p, patch.tangentU), v = Dot(p, patch.tangentV), n = Dot(p, patch.normal);
        minU = std::min(minU, u);
        maxU = std::max(maxU, u);
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
        minN = std::min(minN, n);
        maxN = std::max(maxN, n);
    }
    const float half = s.aperture * 0.5f, tip = s.extentV - patch.effectiveDepth;
    if (minN >= half - eps || maxN <= -half + eps || maxV <= tip + eps || minV >= s.extentV - eps) {
        result.status = "交差がないため形状は変更しません";
        return result;
    }
    if (-s.extentU > minU + eps || s.extentU < maxU - eps || s.extentV < maxV - eps)
        return Fail("Mesh 部分切断は U 全幅と +V 側の外面を覆う範囲にしてください");
    if (tip <= minV + eps) return Fail("未破断部が残りません。深さを下げてください");
    if (minN >= -half - eps || maxN <= half + eps || s.aperture <= eps * 4)
        return Fail("亀裂の両側に岩が残る位置と、許容誤差より十分大きい開口幅にしてください");
    const std::vector<fracture::SplitPlane> planes = {
        {Offset(patch.center, patch.normal, -half), patch.normal, "left"},
        {Offset(patch.center, patch.normal, half), patch.normal, "right"},
        {Offset(patch.center, patch.tangentV, tip), patch.tangentV, "tip"}};
    auto partition = fracture::SplitByPlanes(input, planes);
    if (!partition.error.empty()) return Fail(partition.error);
    // 全セルで同じ境界平面を使い、内部の区切り面を除いて外皮と溝の壁だけを結合する。
    geometry::Mesh mesh;
    using Bucket = std::tuple<long long, long long, long long>;
    std::map<Bucket, std::vector<uint32_t>> buckets;
    const double weld = eps * 0.5;
    const auto vertex = [&](Vec3 p) {
        const auto x = static_cast<long long>(std::floor((double(p.x) - original.minimum.x) / weld));
        const auto y = static_cast<long long>(std::floor((double(p.y) - original.minimum.y) / weld));
        const auto z = static_cast<long long>(std::floor((double(p.z) - original.minimum.z) / weld));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto found = buckets.find({x + dx, y + dy, z + dz});
                    if (found == buckets.end()) continue;
                    for (auto id : found->second) {
                        const auto d = Sub(p, mesh.positions[id]);
                        if (Dot(d, d) <= weld * weld) return id;
                    }
                }
        const auto id = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(p);
        buckets[{x, y, z}].push_back(id);
        return id;
    };
    double removed = 0;
    bool hasWall[3] = {};
    for (const auto& piece : partition.pieces) {
        // 分割キーはこの関数で指定した3平面の負/正側。
        const bool discarded = piece.key == "left+;right-;tip+;";
        if (discarded) {
            geometry::MeshInfo info;
            geometry::InspectMesh(piece.mesh, info);
            removed += info.volume;
            continue;
        }
        for (const auto& triangle : piece.mesh.triangles) {
            const Vec3 a = piece.mesh.positions[triangle[0]], b = piece.mesh.positions[triangle[1]],
                       c = piece.mesh.positions[triangle[2]];
            const Vec3 mid{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
            const auto local = Sub(mid, patch.center);
            const double v = Dot(local, patch.tangentV), n = Dot(local, patch.normal);
            const auto normal = geometry::FaceNormal(piece.mesh, triangle);
            bool keep = true;
            for (size_t i = 0; i < planes.size(); ++i) {
                const auto& plane = planes[i];
                const auto distance = [&](Vec3 p) { return Dot(Sub(p, plane.center), plane.normal); };
                if (std::abs(distance(a)) > eps * 2 || std::abs(distance(b)) > eps * 2 ||
                    std::abs(distance(c)) > eps * 2)
                    continue;
                keep = i == 0   ? v > tip + eps && Dot(normal, patch.normal) > 0
                       : i == 1 ? v > tip + eps && Dot(normal, patch.normal) < 0
                                : n > -half + eps && n < half - eps && Dot(normal, patch.tangentV) > 0;
                if (keep) hasWall[i] = true;
                break;
            }
            if (keep) mesh.triangles.push_back({vertex(a), vertex(b), vertex(c)});
        }
    }
    if (removed == 0) {
        result.status = "溝の到達範囲と母岩が交差しないため形状は変更しません";
        return result;
    }
    if (!hasWall[0] || !hasWall[1] || !hasWall[2])
        return Fail("両側の亀裂壁と奥の終端を確認できません。位置・深さを調整してください");
    geometry::MeshInfo info;
    if (!geometry::InspectMesh(mesh, info) || !info.closed || info.components != 1 || info.volume <= 0 ||
        std::abs(info.volume + removed - original.volume) > original.volume * 1e-4)
        return Fail("部分切断後の閉包・1連結体・体積を確認できません。位置や角度を調整してください");
    result.mesh = std::move(mesh);
    result.removedVolume = removed;
    result.status = "部分破断：両側の壁と終端を持つ1連結体（Bridge 断面の計測・表示は未対応）";
    return result;
}
}  // namespace rock::crack
