#include "crack/PartialCut.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace rock::crack {
namespace {
using Point = std::array<double, 3>;
struct Axis {
    int index;
    int sign;
};
std::optional<Axis> AlignedAxis(geometry::Vec3 v) {
    const float values[3] = {v.x, v.y, v.z};
    for (int axis = 0; axis < 3; ++axis) {
        // 90 度の三角関数の丸めだけを吸収する。斜めの切断は近似しない。
        if (std::abs(std::abs(values[axis]) - 1) > 1e-6f) continue;
        if (std::abs(values[(axis + 1) % 3]) > 1e-6f || std::abs(values[(axis + 2) % 3]) > 1e-6f) continue;
        return Axis{axis, values[axis] > 0 ? 1 : -1};
    }
    return std::nullopt;
}
// 切り込み境界の平面で Box を厳密に区切る。均等 voxel や解像度による近似ではない。
// 全面に同じ分割座標を用い、面同士の T 字接続を作らない（最大 3*3*3 セル）。
geometry::Mesh BoundaryMesh(const Point& half, const Point& low, const Point& high) {
    std::array<std::vector<double>, 3> grid;
    std::array<int, 3> count;
    for (int axis = 0; axis < 3; ++axis) {
        grid[axis] = {-half[axis], half[axis]};
        for (double value : {low[axis], high[axis]})
            if (value > -half[axis] && value < half[axis]) grid[axis].push_back(value);
        std::sort(grid[axis].begin(), grid[axis].end());
        grid[axis].erase(std::unique(grid[axis].begin(), grid[axis].end()), grid[axis].end());
        count[axis] = static_cast<int>(grid[axis].size()) - 1;
    }
    const auto filled = [&](int x, int y, int z) {
        const int cell[3] = {x, y, z};
        for (int axis = 0; axis < 3; ++axis)
            if (cell[axis] < 0 || cell[axis] >= count[axis]) return false;
        bool removed = true;
        for (int axis = 0; axis < 3; ++axis) {
            const double mid = (grid[axis][cell[axis]] + grid[axis][cell[axis] + 1]) * 0.5;
            removed &= mid > low[axis] && mid < high[axis];
        }
        return !removed;
    };
    geometry::Mesh mesh;
    std::vector<uint32_t> ids(grid[0].size() * grid[1].size() * grid[2].size(),
                              std::numeric_limits<uint32_t>::max());
    const auto vertex = [&](int x, int y, int z) {
        auto& id = ids[(z * grid[1].size() + y) * grid[0].size() + x];
        if (id == std::numeric_limits<uint32_t>::max()) {
            id = static_cast<uint32_t>(mesh.positions.size());
            mesh.positions.push_back({static_cast<float>(grid[0][x]), static_cast<float>(grid[1][y]),
                                      static_cast<float>(grid[2][z])});
        }
        return id;
    };
    constexpr int corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    constexpr int faces[6][4] = {{0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4},
                                 {3, 7, 6, 2}, {0, 3, 2, 1}, {4, 5, 6, 7}};
    constexpr int directions[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (int z = 0; z < count[2]; ++z)
        for (int y = 0; y < count[1]; ++y)
            for (int x = 0; x < count[0]; ++x) {
                if (!filled(x, y, z)) continue;
                for (int f = 0; f < 6; ++f) {
                    if (filled(x + directions[f][0], y + directions[f][1], z + directions[f][2])) continue;
                    std::array<uint32_t, 4> face;
                    for (int k = 0; k < 4; ++k) {
                        const auto& corner = corners[faces[f][k]];
                        face[k] = vertex(x + corner[0], y + corner[1], z + corner[2]);
                    }
                    mesh.triangles.push_back({face[0], face[1], face[2]});
                    mesh.triangles.push_back({face[0], face[2], face[3]});
                }
            }
    return mesh;
}
}  // namespace
PartialCutResult CutBox(const std::array<float, 3>& size, const CrackSettings& s) {
    PartialCutResult result;
    result.mesh = geometry::MakeBox(size);
    CrackPatch patch;
    if (result.mesh.positions.empty()) {
        result.error = "有効な Box 寸法が必要です";
        return result;
    }
    if (!BuildCrackPatch(s, patch, result.error)) {
        result.mesh = {};
        return result;
    }
    const auto fail = [&](const char* message) {
        result.mesh = {};
        result.error = message;
        return result;
    };
    if (s.aperture == 0 || patch.effectiveDepth == 0) {
        result.status = "開口または深さがゼロのため、形状は変更しません";
        return result;
    }
    const auto u = AlignedAxis(patch.tangentU), v = AlignedAxis(patch.tangentV),
               n = AlignedAxis(patch.normal);
    if (!u || !v || !n || u->index == v->index || u->index == n->index || v->index == n->index)
        return fail("部分切断は各軸に沿うパッチのみ対応します（回転は 90 度単位）");
    const Point half{size[0] * 0.5, size[1] * 0.5, size[2] * 0.5};
    Point localMin{}, localMax{};
    const Axis axes[3] = {*u, *v, *n};
    for (int k = 0; k < 3; ++k) {
        const auto axis = axes[k];
        const double center = s.center[axis.index] * axis.sign;
        localMin[k] = -half[axis.index] - center;
        localMax[k] = half[axis.index] - center;
    }
    const double tip = double(s.extentV) - patch.effectiveDepth;
    Point lo{std::max(localMin[0], -double(s.extentU)), std::max(localMin[1], tip),
             std::max(localMin[2], -double(s.aperture) * 0.5)};
    Point hi{std::min(localMax[0], double(s.extentU)), std::min(localMax[1], double(s.extentV)),
             std::min(localMax[2], double(s.aperture) * 0.5)};
    Point epsilon{};
    for (int k = 0; k < 3; ++k) {
        epsilon[k] = size[axes[k].index] * 1e-6;
        if (hi[k] - lo[k] <= epsilon[k]) {
            result.status = "交差なし、または交差幅が数値許容誤差以下のため、形状は変更しません";
            return result;
        }
    }
    if (s.extentV < localMax[1] - epsilon[1])
        return fail("パッチの +V 端を母岩の外面まで伸ばしてください（内部空洞は対象外）");
    if (tip <= localMin[1] + epsilon[1])
        return fail("Rock Bridge が残りません。深さを下げてください（完全分割は未実装）");
    if (lo[2] <= localMin[2] + epsilon[2] || hi[2] >= localMax[2] - epsilon[2])
        return fail("亀裂の両側に岩が残る位置と開口幅にしてください");
    // 境界にほぼ一致する U 端は Box 外面へ揃え、極薄の面を作らない。
    for (int k = 0; k < 3; ++k) {
        if (lo[k] - localMin[k] <= epsilon[k]) lo[k] = localMin[k];
        if (localMax[k] - hi[k] <= epsilon[k]) hi[k] = localMax[k];
    }
    Point worldLo{}, worldHi{};
    for (int k = 0; k < 3; ++k) {
        const auto axis = axes[k];
        const double a = s.center[axis.index] + lo[k] * axis.sign,
                     b = s.center[axis.index] + hi[k] * axis.sign;
        worldLo[axis.index] = std::clamp(std::min(a, b), -half[axis.index], half[axis.index]);
        worldHi[axis.index] = std::clamp(std::max(a, b), -half[axis.index], half[axis.index]);
    }
    auto mesh = BoundaryMesh(half, worldLo, worldHi);
    geometry::MeshInfo info;
    const double originalVolume = double(size[0]) * size[1] * size[2];
    const double removed = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    if (!geometry::InspectMesh(mesh, info) || !info.closed || info.components != 1 || info.volume <= 0 ||
        std::abs(info.volume - (originalVolume - removed)) > originalVolume * 1e-5)
        return fail("切断後の閉包・連結性・体積検証に失敗しました");
    RockBridge bridge;
    bridge.thickness = static_cast<float>(lo[1] - localMin[1]);
    bridge.area = static_cast<float>((hi[0] - lo[0]) * bridge.thickness);
    const auto point = [&](double a, double b) {
        Point p{double(s.center[0]), double(s.center[1]), double(s.center[2])};
        p[u->index] += a * u->sign;
        p[v->index] += b * v->sign;
        return geometry::Vec3{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
    };
    bridge.section = {point(lo[0], lo[1]), point(hi[0], lo[1]), point(hi[0], localMin[1]),
                      point(lo[0], localMin[1])};
    result.mesh = std::move(mesh);
    result.bridge = bridge;
    result.penetration = static_cast<float>(hi[1] - lo[1]);
    result.removedVolume = removed;
    result.status = "部分破断：Rock Bridge が残る1連結体";
    return result;
}
}  // namespace rock::crack
