#include "geometry/Volume.h"
#include "geometry/DualContouring.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <numeric>
#include <numbers>
#include <limits>

namespace rock::geometry {
namespace {
// To Volume の最大解像度96に、余白と任意方向への回転による外接箱の拡大を許容する。
// 密な配列のため上限は残す（最大約28MiBの距離データ）。
constexpr uint32_t kMaxGridPointsPerAxis = 192;
// 格子生成・変換・表面抽出で同じ上限を使う。
bool ValidGrid(const VolumeGrid& g) {
    const auto nx = g.dimensions[0], ny = g.dimensions[1], nz = g.dimensions[2];
    return nx >= 2 && ny >= 2 && nz >= 2 && nx <= kMaxGridPointsPerAxis &&
           ny <= kMaxGridPointsPerAxis && nz <= kMaxGridPointsPerAxis &&
           g.values.size() == size_t(nx) * ny * nz && std::isfinite(g.spacing) && g.spacing > 0 &&
           std::isfinite(g.origin.x) && std::isfinite(g.origin.y) && std::isfinite(g.origin.z) &&
           std::none_of(g.values.begin(), g.values.end(), [](float v) { return !std::isfinite(v); });
}
// 右手系 Z → X → Y。Crack / Model と同じ向きに回す。
Vec3 Rotate(Vec3 p, const std::array<float, 3>& degrees) {
    const float x = degrees[0] * std::numbers::pi_v<float> / 180;
    const float y = degrees[1] * std::numbers::pi_v<float> / 180;
    const float z = degrees[2] * std::numbers::pi_v<float> / 180;
    const Vec3 rz{std::cos(z) * p.x - std::sin(z) * p.y, std::sin(z) * p.x + std::cos(z) * p.y, p.z};
    const Vec3 rx{rz.x, std::cos(x) * rz.y - std::sin(x) * rz.z, std::sin(x) * rz.y + std::cos(x) * rz.z};
    return {std::cos(y) * rx.x + std::sin(y) * rx.z, rx.y, -std::sin(y) * rx.x + std::cos(y) * rx.z};
}
float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
// 格子の外側は、外周の正値に外へ出た距離を足して返す。外周は必ず空なので符号は正のまま。
float SampleVolume(const VolumeGrid& g, Vec3 p) {
    const float f[3] = {(p.x - g.origin.x) / g.spacing, (p.y - g.origin.y) / g.spacing,
                        (p.z - g.origin.z) / g.spacing};
    float clamped[3];
    double outside = 0;
    uint32_t base[3];
    float fraction[3];
    for (int i = 0; i < 3; ++i) {
        clamped[i] = std::clamp(f[i], 0.f, float(g.dimensions[i] - 1));
        outside += double(f[i] - clamped[i]) * (f[i] - clamped[i]);
        const float floored = std::floor(clamped[i]);
        base[i] = std::min(static_cast<uint32_t>(floored), g.dimensions[i] - 2);
        fraction[i] = clamped[i] - base[i];
    }
    double value = 0;
    for (int c = 0; c < 8; ++c) {
        const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
        const double weight = (dx ? fraction[0] : 1 - fraction[0]) * (dy ? fraction[1] : 1 - fraction[1]) *
                              (dz ? fraction[2] : 1 - fraction[2]);
        if (weight > 0) value += weight * g.values[g.Index(base[0] + dx, base[1] + dy, base[2] + dz)];
    }
    return float(value + std::sqrt(outside) * g.spacing);
}
}  // namespace
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
VolumeGrid TransformVolume(const VolumeGrid& g, const VolumeTransformSettings& s, std::string& error) {
    error.clear();
    if (!ValidGrid(g)) {
        error = "ボリュームの格子が不正です";
        return {};
    }
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    for (float v : s.position)
        if (!range(v, -10000, 10000)) {
            error = "移動量は有限の -10000～10000 m にしてください";
            return {};
        }
    for (float v : s.rotationDegrees)
        if (!range(v, -360, 360)) {
            error = "回転は有限の -360～360 度にしてください";
            return {};
        }
    if (!range(s.scale, .05f, 20)) {
        error = "倍率は 0.05～20 にしてください";
        return {};
    }
    // 回転後の基底。列が x / y / z 軸の行き先になる。
    const Vec3 ax = Rotate({1, 0, 0}, s.rotationDegrees), ay = Rotate({0, 1, 0}, s.rotationDegrees),
               az = Rotate({0, 0, 1}, s.rotationDegrees);
    const auto forward = [&](Vec3 p) {
        const float x = p.x * s.scale, y = p.y * s.scale, z = p.z * s.scale;
        return Vec3{ax.x * x + ay.x * y + az.x * z + s.position[0],
                    ax.y * x + ay.y * y + az.y * z + s.position[1],
                    ax.z * x + ay.z * y + az.z * z + s.position[2]};
    };
    // 元の格子の8隅を動かし、その AABB を新しい格子の範囲にする。
    Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()},
        maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest()};
    for (int c = 0; c < 8; ++c) {
        const auto corner = forward(g.Position(c & 1 ? g.dimensions[0] - 1 : 0, c & 2 ? g.dimensions[1] - 1 : 0,
                                               c & 4 ? g.dimensions[2] - 1 : 0));
        minimum = {std::min(minimum.x, corner.x), std::min(minimum.y, corner.y), std::min(minimum.z, corner.z)};
        maximum = {std::max(maximum.x, corner.x), std::max(maximum.y, corner.y), std::max(maximum.z, corner.z)};
    }
    VolumeGrid out;
    // セル間隔を倍率に比例させ、拡大しても格子の数を増やさない。
    out.spacing = g.spacing * s.scale;
    if (!std::isfinite(out.spacing) || out.spacing <= 0) {
        error = "変換後のセル間隔が不正です";
        return {};
    }
    out.origin = {minimum.x - out.spacing * 2, minimum.y - out.spacing * 2, minimum.z - out.spacing * 2};
    const float extent[3] = {maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z};
    for (int i = 0; i < 3; ++i) {
        const double cells = std::ceil(extent[i] / out.spacing) + 5;
        if (!std::isfinite(cells) || cells < 2 || cells > kMaxGridPointsPerAxis) {
            error = "変換後の格子が各軸192点の上限を超えます。連続する Volume Transform をまとめるか、上流の解像度を下げてください";
            return {};
        }
        out.dimensions[i] = static_cast<uint32_t>(cells);
    }
    out.values.resize(size_t(out.dimensions[0]) * out.dimensions[1] * out.dimensions[2]);
    const float threshold = out.spacing * 1e-4f;
    bool inside = false;
    for (uint32_t z = 0; z < out.dimensions[2]; ++z)
        for (uint32_t y = 0; y < out.dimensions[1]; ++y)
            for (uint32_t x = 0; x < out.dimensions[0]; ++x) {
                // 逆変換で元の格子を読む。距離の単位を保つため倍率を掛け戻す。
                const auto p = out.Position(x, y, z);
                const Vec3 d{p.x - s.position[0], p.y - s.position[1], p.z - s.position[2]};
                const Vec3 source{Dot(d, ax) / s.scale, Dot(d, ay) / s.scale, Dot(d, az) / s.scale};
                const float value = SampleVolume(g, source) * s.scale;
                out.values[out.Index(x, y, z)] = std::abs(value) < threshold ? threshold : value;
                inside |= value < 0;
            }
    if (!inside) {
        error = "変換後に内部が残りません。倍率や上流の解像度を見直してください";
        return {};
    }
    return out;
}
Mesh VolumeSurface(const VolumeGrid& g, std::string& error, VolumeMeshingMethod method) {
    error.clear();
    const auto nx = g.dimensions[0], ny = g.dimensions[1], nz = g.dimensions[2];
    if (!ValidGrid(g)) {
        error = "ボリュームの格子が不正です";
        return {};
    }
    Mesh mesh;
    if (method == VolumeMeshingMethod::DualContouring) {
        mesh = ExtractDualContour(g, error);
        if (!error.empty()) return {};
    } else if (method == VolumeMeshingMethod::MarchingTetrahedra) {
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
    } else {
        error = "不明なボリュームのメッシュ変換方式です";
        return {};
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
