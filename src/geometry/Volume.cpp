#include "geometry/Volume.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <numeric>

namespace rock::geometry {
VolumeGrid BoxesToVolume(const std::vector<OrientedBox>& boxes, const VolumeSettings& settings,
                         std::string& error) {
    error.clear();
    if (boxes.empty() || boxes.size() > 32 || settings.resolution < 16 || settings.resolution > 96) {
        error = "Box は1～32個、ボリュームの解像度は16～96にしてください";
        return {};
    }
    // 内部生成の Box も検証し、NaN や過大な格子を確保しない。
    for (const auto& box : boxes) {
        for (int a = 0; a < 3; ++a) {
            const auto axis = box.axes[a];
            if (!std::isfinite(box.halfSize[a]) || box.halfSize[a] < .001f || box.halfSize[a] > 100 ||
                !std::isfinite(axis.x) || !std::isfinite(axis.y) || !std::isfinite(axis.z)) {
                error = "Box の寸法・回転が不正です";
                return {};
            }
            for (int b = 0; b < 3; ++b) {
                const auto other = box.axes[b];
                if (std::abs(axis.x * other.x + axis.y * other.y + axis.z * other.z - (a == b ? 1.f : 0.f)) >
                    1e-4f) {
                    error = "Box の回転軸が直交していません";
                    return {};
                }
            }
        }
    }
    MeshInfo bounds;
    if (!InspectMesh(BoxClusterPreview(boxes), bounds)) {
        error = "Box の境界を取得できません";
        return {};
    }
    const float extent[3] = {bounds.maximum.x - bounds.minimum.x, bounds.maximum.y - bounds.minimum.y,
                             bounds.maximum.z - bounds.minimum.z};
    VolumeGrid grid;
    grid.spacing = std::max({extent[0], extent[1], extent[2]}) / settings.resolution;
    if (!std::isfinite(grid.spacing) || grid.spacing <= 0) {
        error = "ボリュームの範囲が不正です";
        return {};
    }
    grid.origin = {bounds.minimum.x - grid.spacing * 2, bounds.minimum.y - grid.spacing * 2,
                   bounds.minimum.z - grid.spacing * 2};
    for (int i = 0; i < 3; ++i)
        grid.dimensions[i] = static_cast<uint32_t>(std::ceil(extent[i] / grid.spacing)) + 5;
    grid.values.resize(size_t(grid.dimensions[0]) * grid.dimensions[1] * grid.dimensions[2]);
    bool inside = false;
    for (uint32_t z = 0; z < grid.dimensions[2]; ++z)
        for (uint32_t y = 0; y < grid.dimensions[1]; ++y)
            for (uint32_t x = 0; x < grid.dimensions[0]; ++x) {
                const float value = BoxUnionField(grid.Position(x, y, z), boxes);
                // 等値面が格子頂点に一致する場合も同じ符号に寄せ、ゼロ長の交点辺を避ける。
                grid.values[grid.Index(x, y, z)] =
                    std::abs(value) < grid.spacing * 1e-4f ? grid.spacing * 1e-4f : value;
                inside |= value < 0;
            }
    if (!inside) {
        error = "形がセルより薄いため内部を捉えられません。解像度を上げるか寸法を調整してください";
        return {};
    }
    return grid;
}
Mesh VolumeSurface(const VolumeGrid& g, std::string& error) {
    error.clear();
    const auto nx = g.dimensions[0], ny = g.dimensions[1], nz = g.dimensions[2];
    if (nx < 2 || ny < 2 || nz < 2 || nx > 102 || ny > 102 || nz > 102 ||
        g.values.size() != size_t(nx) * ny * nz || !std::isfinite(g.spacing) || g.spacing <= 0 ||
        !std::isfinite(g.origin.x) || !std::isfinite(g.origin.y) || !std::isfinite(g.origin.z) ||
        std::any_of(g.values.begin(), g.values.end(), [](float v) { return !std::isfinite(v); })) {
        error = "ボリュームの格子が不正です";
        return {};
    }
    Mesh mesh;
    std::unordered_map<uint64_t, uint32_t> crossings;
    // 全セルで同じ体対角を使う6四面体。隣接セルの面の分割も一致する。
    constexpr int corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    constexpr int tetrahedra[6][4] = {{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6},
                                      {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}};
    const auto position = [&](uint32_t id) { return g.Position(id % nx, (id / nx) % ny, id / (nx * ny)); };
    const auto intersection = [&](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        const uint64_t key = (uint64_t(a) << 32) | b;
        if (const auto found = crossings.find(key); found != crossings.end()) return found->second;
        const auto p = position(a), q = position(b);
        const double t = double(g.values[a]) / (double(g.values[a]) - g.values[b]);
        const auto id = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(
            {float(p.x + (q.x - p.x) * t), float(p.y + (q.y - p.y) * t), float(p.z + (q.z - p.z) * t)});
        crossings.emplace(key, id);
        return id;
    };
    const auto face = [&](uint32_t a, uint32_t b, uint32_t c, Vec3 outward) {
        const auto p = mesh.positions[a], q = mesh.positions[b], r = mesh.positions[c];
        const double ux = double(q.x) - p.x, uy = double(q.y) - p.y, uz = double(q.z) - p.z,
                     vx = double(r.x) - p.x, vy = double(r.y) - p.y, vz = double(r.z) - p.z;
        if ((uy * vz - uz * vy) * outward.x + (uz * vx - ux * vz) * outward.y +
                (ux * vy - uy * vx) * outward.z <
            0)
            std::swap(b, c);
        mesh.triangles.push_back({a, b, c});
    };
    for (uint32_t z = 0; z + 1 < nz; ++z)
        for (uint32_t y = 0; y + 1 < ny; ++y)
            for (uint32_t x = 0; x + 1 < nx; ++x) {
                uint32_t cell[8];
                bool negative = false, positive = false;
                for (int c = 0; c < 8; ++c) {
                    cell[c] = static_cast<uint32_t>(
                        g.Index(x + corners[c][0], y + corners[c][1], z + corners[c][2]));
                    negative |= g.values[cell[c]] < 0;
                    positive |= g.values[cell[c]] >= 0;
                }
                if (!negative || !positive) continue;
                for (const auto& tet : tetrahedra) {
                    uint32_t in[4], out[4];
                    int ni = 0, no = 0;
                    for (auto c : tet) {
                        if (g.values[cell[c]] < 0)
                            in[ni++] = cell[c];
                        else
                            out[no++] = cell[c];
                    }
                    if (ni == 0 || no == 0) continue;
                    const auto p = position(in[0]), q = position(out[0]);
                    const Vec3 outward{q.x - p.x, q.y - p.y, q.z - p.z};
                    if (ni == 1)
                        face(intersection(in[0], out[0]), intersection(in[0], out[1]),
                             intersection(in[0], out[2]), outward);
                    else if (no == 1)
                        face(intersection(in[0], out[0]), intersection(in[1], out[0]),
                             intersection(in[2], out[0]), outward);
                    else {
                        const auto a = intersection(in[0], out[0]), b = intersection(in[0], out[1]),
                                   c = intersection(in[1], out[1]), d = intersection(in[1], out[0]);
                        face(a, b, c, outward);
                        face(a, c, d, outward);
                    }
                }
            }
    MeshInfo info;
    if (!InspectMesh(mesh, info)) {
        error = "表面に細すぎる三角形が生じました。解像度を調整してください";
        return {};
    }
    if (!info.closed || info.volume <= 0) {
        error = "閉じた表面を抽出できません。解像度か直方体の寸法・配置を調整してください";
        return {};
    }
    if (info.components > 1) {
        // 外皮と内部空洞の壁は別の表面成分でも同じ固体。空洞を勝手に埋めない。
        // 正の体積の外皮が複数なら、解像度で細い接続が失われたので診断する。
        std::vector<uint32_t> parents(mesh.positions.size());
        std::iota(parents.begin(), parents.end(), 0);
        const auto root = [&](uint32_t id) {
            while (parents[id] != id) {
                parents[id] = parents[parents[id]];
                id = parents[id];
            }
            return id;
        };
        for (const auto& t : mesh.triangles) {
            parents[root(t[1])] = root(t[0]);
            parents[root(t[2])] = root(t[0]);
        }
        std::unordered_map<uint32_t, double> volumes;
        const auto reference = mesh.positions.front();
        for (const auto& t : mesh.triangles) {
            const auto a = mesh.positions[t[0]], b = mesh.positions[t[1]], c = mesh.positions[t[2]];
            const double ax = double(a.x) - reference.x, ay = double(a.y) - reference.y,
                         az = double(a.z) - reference.z, bx = double(b.x) - reference.x,
                         by = double(b.y) - reference.y, bz = double(b.z) - reference.z,
                         cx = double(c.x) - reference.x, cy = double(c.y) - reference.y,
                         cz = double(c.z) - reference.z;
            volumes[root(t[0])] +=
                (ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx)) / 6;
        }
        if (std::count_if(volumes.begin(), volumes.end(), [](const auto& v) { return v.second > 0; }) != 1) {
            error = "解像度によって塊の細い接続が失われました。解像度を上げるか直方体を厚くしてください";
            return {};
        }
    }
    return mesh;
}
}  // namespace rock::geometry
