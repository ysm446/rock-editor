#include "geometry/Volume.h"
#include "geometry/DualContouring.h"
#include <algorithm>
#include <cmath>
#include <execution>
#include <unordered_map>
#include <numeric>
#include <numbers>
#include <limits>

namespace rock::geometry {
namespace {
// To Volume の最大解像度128に、余白と任意方向への回転による外接箱の拡大を許容する。
// 密な配列のため上限は残す（256³ で約64MiBの距離データ。ノードごとにキャッシュに持つ）。
constexpr uint32_t kMaxGridPointsPerAxis = 256;
// Plane Cuts の局所モード。等方の法線へ混ぜるランダムな向きの割合と、
// 主方向の法線が表面の外向きと成す余弦の下限。
constexpr double kLocalCutTilt = .75;
constexpr double kLocalCutFacing = .35;
constexpr int kLocalCutConvexNeighbours = 13;
constexpr double kLocalCutRimLimit = 1.3;
// 格子生成・変換・表面抽出で同じ上限を使う。
bool ValidGrid(const VolumeGrid& g) {
    const auto nx = g.dimensions[0], ny = g.dimensions[1], nz = g.dimensions[2];
    return nx >= 2 && ny >= 2 && nz >= 2 && nx <= kMaxGridPointsPerAxis &&
           ny <= kMaxGridPointsPerAxis && nz <= kMaxGridPointsPerAxis &&
           g.values.size() == size_t(nx) * ny * nz && std::isfinite(g.spacing) && g.spacing > 0 &&
           std::isfinite(g.origin.x) && std::isfinite(g.origin.y) && std::isfinite(g.origin.z) &&
           std::none_of(g.values.begin(), g.values.end(), [](float v) { return !std::isfinite(v); });
}
// 格子点はどれも独立に求まるので、Zスライスごとに並列で埋める。
// sampleは値を書き込み、その点が内部かどうかを返す。内部の点が1つでもあれば true。
template <class Sample>
bool FillSlices(const VolumeGrid& grid, Sample sample) {
    std::vector<uint32_t> slices(grid.dimensions[2]);
    std::iota(slices.begin(), slices.end(), 0u);
    std::vector<uint8_t> inside(slices.size(), 0);
    std::for_each(std::execution::par, slices.begin(), slices.end(), [&](uint32_t z) {
        bool any = false;
        for (uint32_t y = 0; y < grid.dimensions[1]; ++y)
            for (uint32_t x = 0; x < grid.dimensions[0]; ++x) any |= sample(x, y, z);
        inside[z] = any ? 1 : 0;
    });
    return std::find(inside.begin(), inside.end(), uint8_t(1)) != inside.end();
}
// 内部の格子点を6近傍でつないだ塊のうち、最大のものだけを残す。
// 重なった切り落としが角を切り離すと、浮いた小片ができる。小片は外部にする。
void KeepLargestComponent(VolumeGrid& grid) {
    const size_t nx = grid.dimensions[0], ny = grid.dimensions[1], nz = grid.dimensions[2];
    std::vector<uint32_t> labels(grid.values.size(), 0);
    std::vector<size_t> sizes{0}, stack;
    for (size_t start = 0; start < grid.values.size(); ++start) {
        if (grid.values[start] >= 0 || labels[start] != 0) continue;
        const uint32_t label = uint32_t(sizes.size());
        sizes.push_back(0);
        labels[start] = label;
        stack.push_back(start);
        while (!stack.empty()) {
            const size_t index = stack.back();
            stack.pop_back();
            ++sizes[label];
            const size_t x = index % nx, y = (index / nx) % ny, z = index / (nx * ny);
            const auto visit = [&](bool valid, size_t next) {
                if (!valid || grid.values[next] >= 0 || labels[next] != 0) return;
                labels[next] = label;
                stack.push_back(next);
            };
            visit(x > 0, index - 1);
            visit(x + 1 < nx, index + 1);
            visit(y > 0, index - nx);
            visit(y + 1 < ny, index + nx);
            visit(z > 0, index - nx * ny);
            visit(z + 1 < nz, index + nx * ny);
        }
    }
    if (sizes.size() <= 2) return;
    const uint32_t largest = uint32_t(std::max_element(sizes.begin() + 1, sizes.end()) - sizes.begin());
    for (size_t i = 0; i < grid.values.size(); ++i)
        if (labels[i] != 0 && labels[i] != largest) grid.values[i] = -grid.values[i];
}
// 内部の格子点を囲む箱の最長辺。長さの設定を形に対する比で持つノードが使う。内部が無ければ 0。
float InteriorLongestSide(const VolumeGrid& g) {
    uint32_t lowest[3] = {g.dimensions[0], g.dimensions[1], g.dimensions[2]}, highest[3] = {0, 0, 0};
    bool any = false;
    for (uint32_t z = 0; z < g.dimensions[2]; ++z)
        for (uint32_t y = 0; y < g.dimensions[1]; ++y)
            for (uint32_t x = 0; x < g.dimensions[0]; ++x) {
                if (g.values[g.Index(x, y, z)] >= 0) continue;
                const uint32_t cell[3] = {x, y, z};
                for (int i = 0; i < 3; ++i) {
                    lowest[i] = std::min(lowest[i], cell[i]);
                    highest[i] = std::max(highest[i], cell[i]);
                }
                any = true;
            }
    if (!any) return 0;
    return float(std::max({highest[0] - lowest[0], highest[1] - lowest[1], highest[2] - lowest[2], 1u})) * g.spacing;
}
// 固定のハッシュ（splitmix64 の仕上げ）。[0, 1) を返す。
double HashUnit(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    value ^= value >> 31;
    return double(value >> 11) / 9007199254740992.0;
}
// 整数格子の値を滑らかに補間する 3D ノイズ。[0, 1)。
float ValueNoise(float x, float y, float z, uint64_t seed) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const auto fade = [](float t) { return t * t * (3 - 2 * t); };
    const float tx = fade(x - fx), ty = fade(y - fy), tz = fade(z - fz);
    const auto corner = [&](int dx, int dy, int dz) {
        const uint64_t ix = uint64_t(int64_t(fx) + dx), iy = uint64_t(int64_t(fy) + dy), iz = uint64_t(int64_t(fz) + dz);
        return float(HashUnit(seed ^ (ix * 0x8DA6B343ull) ^ (iy * 0xD8163841ull) ^ (iz * 0xCB1AB31Full)));
    };
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    return mix(mix(mix(corner(0, 0, 0), corner(1, 0, 0), tx), mix(corner(0, 1, 0), corner(1, 1, 0), tx), ty),
               mix(mix(corner(0, 0, 1), corner(1, 0, 1), tx), mix(corner(0, 1, 1), corner(1, 1, 1), tx), ty), tz);
}
// 格子の外周から届かない外部（閉じた空洞）のうち、加工で新しくできたものを加工前の値へ戻す。
// before は同じ格子の加工前の値。加工前から外部だった空洞（内向きの殻）は残す。
void FillNewVoids(VolumeGrid& grid, const std::vector<float>& before) {
    std::vector<uint8_t> reached(grid.values.size(), 0);
    std::vector<size_t> stack;
    const size_t nx = grid.dimensions[0], ny = grid.dimensions[1], nz = grid.dimensions[2];
    const auto visit = [&](size_t next) {
        if (grid.values[next] < 0 || reached[next]) return;
        reached[next] = 1;
        stack.push_back(next);
    };
    for (size_t z = 0; z < nz; ++z)
        for (size_t y = 0; y < ny; ++y)
            for (size_t x = 0; x < nx; ++x)
                if (x == 0 || y == 0 || z == 0 || x + 1 == nx || y + 1 == ny || z + 1 == nz)
                    visit((z * ny + y) * nx + x);
    while (!stack.empty()) {
        const size_t index = stack.back();
        stack.pop_back();
        const size_t x = index % nx, y = (index / nx) % ny, z = index / (nx * ny);
        if (x > 0) visit(index - 1);
        if (x + 1 < nx) visit(index + 1);
        if (y > 0) visit(index - nx);
        if (y + 1 < ny) visit(index + nx);
        if (z > 0) visit(index - nx * ny);
        if (z + 1 < nz) visit(index + nx * ny);
    }
    for (size_t i = 0; i < grid.values.size(); ++i)
        if (grid.values[i] >= 0 && !reached[i] && before[i] < 0) grid.values[i] = before[i];
}
// 右手系 Z → X → Y。Model と同じ向きに回す。
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
    if (boxes.empty() || boxes.size() > 32 || settings.resolution < 16 || settings.resolution > 128) {
        error = "Box は1～32個、ボリュームの解像度は16～128にしてください";
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
    const bool inside = FillSlices(grid, [&](uint32_t x, uint32_t y, uint32_t z) {
        const float value = BoxUnionField(grid.Position(x, y, z), boxes);
        // 等値面が格子頂点に一致する場合も同じ符号に寄せ、ゼロ長の交点辺を避ける。
        grid.values[grid.Index(x, y, z)] =
            std::abs(value) < grid.spacing * 1e-4f ? grid.spacing * 1e-4f : value;
        return value < 0;
    });
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
            error = "変換後の格子が各軸256点の上限を超えます。連続する Volume Transform をまとめるか、上流の解像度を下げてください";
            return {};
        }
        out.dimensions[i] = static_cast<uint32_t>(cells);
    }
    // 移動量に対してセルが小さいと、floatの格子座標が隣どうしで同じ値に潰れる。
    for (int i = 0; i < 3; ++i) {
        const uint32_t last = out.dimensions[i] - 1;
        const auto at = [&](uint32_t n) {
            const auto p = out.Position(i == 0 ? n : 0, i == 1 ? n : 0, i == 2 ? n : 0);
            return i == 0 ? p.x : (i == 1 ? p.y : p.z);
        };
        if (!std::isfinite(at(last)) || at(1) == at(0) || at(last) == at(last - 1)) {
            error = "移動量に対してセルが小さすぎます。原点に近づけるか倍率・解像度を調整してください";
            return {};
        }
    }
    out.values.resize(size_t(out.dimensions[0]) * out.dimensions[1] * out.dimensions[2]);
    const float threshold = out.spacing * 1e-4f;
    const bool inside = FillSlices(out, [&](uint32_t x, uint32_t y, uint32_t z) {
        // 逆変換で元の格子を読む。距離の単位を保つため倍率を掛け戻す。
        const auto p = out.Position(x, y, z);
        const Vec3 d{p.x - s.position[0], p.y - s.position[1], p.z - s.position[2]};
        const Vec3 source{Dot(d, ax) / s.scale, Dot(d, ay) / s.scale, Dot(d, az) / s.scale};
        const float value = SampleVolume(g, source) * s.scale;
        out.values[out.Index(x, y, z)] = std::abs(value) < threshold ? threshold : value;
        return value < 0;
    });
    if (!inside) {
        error = "変換後に内部が残りません。倍率や上流の解像度を見直してください";
        return {};
    }
    return out;
}
const char* VolumeBooleanOperationName(VolumeBooleanOperation operation) {
    switch (operation) {
        case VolumeBooleanOperation::Intersection: return "intersection";
        case VolumeBooleanOperation::Difference: return "difference";
        default: return "union";
    }
}
VolumeBooleanOperation ParseVolumeBooleanOperation(std::string_view name) {
    if (name == "intersection") return VolumeBooleanOperation::Intersection;
    if (name == "difference") return VolumeBooleanOperation::Difference;
    return VolumeBooleanOperation::Union;
}
VolumeGrid CombineVolumes(const VolumeGrid& a, const VolumeGrid& b, const VolumeBooleanSettings& s,
                          std::string& error) {
    error.clear();
    if (!ValidGrid(a) || !ValidGrid(b)) {
        error = "ボリュームの格子が不正です";
        return {};
    }
    if (s.operation != VolumeBooleanOperation::Union && s.operation != VolumeBooleanOperation::Intersection &&
        s.operation != VolumeBooleanOperation::Difference) {
        error = "不明なブーリアン演算です";
        return {};
    }
    if (!std::isfinite(s.blend) || s.blend < 0 || s.blend > 10) {
        error = "なめらかさは 0～10 m にしてください";
        return {};
    }
    // 出力は A の格子点の上に置く。A の値は補間でなまらず、そのまま引き継がれる。
    // 範囲は A の格子の添字で持つ（負や A の外も取り得る）。
    const auto last = [](const VolumeGrid& g, int i) { return double(g.dimensions[i] - 1); };
    const auto origin = [](const VolumeGrid& g, int i) {
        return double(i == 0 ? g.origin.x : (i == 1 ? g.origin.y : g.origin.z));
    };
    int64_t first[3], count[3];
    for (int i = 0; i < 3; ++i) {
        // B の範囲を A の格子の添字へ直す。
        const double bLow = (origin(b, i) - origin(a, i)) / a.spacing;
        const double bHigh = bLow + last(b, i) * double(b.spacing) / a.spacing;
        double low = 0, high = last(a, i);
        if (s.operation == VolumeBooleanOperation::Union) {
            // なめらかな和は、つなぎ目が外へ最大で幅の1/4ふくらむ。外周を空に保つ分だけ広げる。
            const double margin = std::ceil(s.blend * .25 / a.spacing);
            low = std::min(low, std::floor(bLow)) - margin;
            high = std::max(high, std::ceil(bHigh)) + margin;
        } else if (s.operation == VolumeBooleanOperation::Intersection) {
            low = std::max(low, std::floor(bLow));
            high = std::min(high, std::ceil(bHigh));
        }
        if (!std::isfinite(low) || !std::isfinite(high) || high - low < 1) {
            error = "A と B が重なっていません";
            return {};
        }
        if (high - low + 1 > kMaxGridPointsPerAxis) {
            error = "結果の格子が各軸256点の上限を超えます。A と B を近づけるか、A の解像度を下げてください";
            return {};
        }
        first[i] = int64_t(low);
        count[i] = int64_t(high - low) + 1;
    }
    VolumeGrid out;
    out.spacing = a.spacing;
    out.origin = {a.origin.x + float(first[0]) * a.spacing, a.origin.y + float(first[1]) * a.spacing,
                  a.origin.z + float(first[2]) * a.spacing};
    out.dimensions = {uint32_t(count[0]), uint32_t(count[1]), uint32_t(count[2])};
    out.values.resize(size_t(out.dimensions[0]) * out.dimensions[1] * out.dimensions[2]);
    const float threshold = out.spacing * 1e-4f;
    const float k = s.blend;
    const bool inside = FillSlices(out, [&](uint32_t x, uint32_t y, uint32_t z) {
        const int64_t ax = first[0] + x, ay = first[1] + y, az = first[2] + z;
        const bool onA = ax >= 0 && ay >= 0 && az >= 0 && ax < a.dimensions[0] && ay < a.dimensions[1] &&
                         az < a.dimensions[2];
        const auto p = out.Position(x, y, z);
        const float da = onA ? a.values[a.Index(uint32_t(ax), uint32_t(ay), uint32_t(az))] : SampleVolume(a, p);
        float db = SampleVolume(b, p);
        float value;
        if (s.operation == VolumeBooleanOperation::Union) {
            value = std::min(da, db);
            // 多項式のなめらかな最小値。差が幅以上なら通常の最小値と同じ。
            if (k > 0) {
                const float h = std::max(k - std::abs(da - db), 0.f) / k;
                value -= h * h * k * .25f;
            }
        } else {
            if (s.operation == VolumeBooleanOperation::Difference) db = -db;
            value = std::max(da, db);
            if (k > 0) {
                const float h = std::max(k - std::abs(da - db), 0.f) / k;
                value += h * h * k * .25f;
            }
        }
        // 等値面が格子頂点に一致する場合も同じ符号に寄せ、ゼロ長の交点辺を避ける。
        out.values[out.Index(x, y, z)] = std::abs(value) < threshold ? threshold : value;
        return value < 0;
    });
    if (!inside) {
        error = "演算の結果に内部が残りません。A と B の位置や演算を見直してください";
        return {};
    }
    return out;
}
const char* PlaneCutsDistributionName(PlaneCutsDistribution distribution) {
    return distribution == PlaneCutsDistribution::Directional ? "directional" : "isotropic";
}
PlaneCutsDistribution ParsePlaneCutsDistribution(std::string_view name) {
    return name == "directional" ? PlaneCutsDistribution::Directional : PlaneCutsDistribution::Isotropic;
}
const char* PlaneCutsScopeName(PlaneCutsScope scope) {
    return scope == PlaneCutsScope::Local ? "local" : "global";
}
PlaneCutsScope ParsePlaneCutsScope(std::string_view name) {
    return name == "local" ? PlaneCutsScope::Local : PlaneCutsScope::Global;
}
std::vector<CutPlane> MakeCutPlanes(const VolumeGrid& g, const PlaneCutsSettings& s, std::string& error) {
    error.clear();
    if (!ValidGrid(g)) {
        error = "ボリュームの格子が不正です";
        return {};
    }
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (s.count < 1 || s.count > MaxPlaneCuts) {
        error = "平面の枚数は 1～256 にしてください";
        return {};
    }
    if (!range(s.depthMin, 0, MaxPlaneCutDepth) || !range(s.depthMax, 0, MaxPlaneCutDepth) ||
        s.depthMin > s.depthMax) {
        error = "切り込みの深さは 0～0.45 で、最小を最大以下にしてください";
        return {};
    }
    if (s.distribution != PlaneCutsDistribution::Isotropic &&
        s.distribution != PlaneCutsDistribution::Directional) {
        error = "不明な法線の分布です";
        return {};
    }
    if (s.scope != PlaneCutsScope::Global && s.scope != PlaneCutsScope::Local) {
        error = "不明な適用範囲です";
        return {};
    }
    if (!range(s.radius, .02f, 1)) {
        error = "局所の半径は 0.02～1 にしてください";
        return {};
    }
    if (s.systems < 1 || s.systems > 3) {
        error = "主方向の系統数は 1～3 にしてください";
        return {};
    }
    for (float v : s.rotationDegrees)
        if (!range(v, -360, 360)) {
            error = "向きは有限の -360～360 度にしてください";
            return {};
        }
    if (!range(s.spreadDegrees, 0, 90)) {
        error = "ばらつきは 0～90 度にしてください";
        return {};
    }
    if (!range(s.blend, 0, 10)) {
        error = "なめらかさは 0～10 m にしてください";
        return {};
    }
    // 形の幅は表面のすぐ内側の格子点から測る。ある方向の端は必ず表面にある。
    // 表面の点は距離の小ささではなく、隣に外部の点があることで選ぶ。重なった立体から作ったボリュームは
    // 内部に残る面の近くでも距離が小さくなり、距離で選ぶと形の内側を中心に選んでしまう。
    // 局所の中心もこの点から選ぶ。外周は必ず空なので、内部の点には全方向の隣がある。
    std::vector<Vec3> shell;
    std::vector<std::array<uint32_t, 3>> shellCells;
    Vec3 lowest{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()},
        highest{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest()};
    for (uint32_t z = 1; z + 1 < g.dimensions[2]; ++z)
        for (uint32_t y = 1; y + 1 < g.dimensions[1]; ++y)
            for (uint32_t x = 1; x + 1 < g.dimensions[0]; ++x) {
                if (g.values[g.Index(x, y, z)] >= 0) continue;
                if (g.values[g.Index(x - 1, y, z)] < 0 && g.values[g.Index(x + 1, y, z)] < 0 &&
                    g.values[g.Index(x, y - 1, z)] < 0 && g.values[g.Index(x, y + 1, z)] < 0 &&
                    g.values[g.Index(x, y, z - 1)] < 0 && g.values[g.Index(x, y, z + 1)] < 0)
                    continue;
                const auto p = g.Position(x, y, z);
                shell.push_back(p);
                shellCells.push_back({x, y, z});
                lowest = {std::min(lowest.x, p.x), std::min(lowest.y, p.y), std::min(lowest.z, p.z)};
                highest = {std::max(highest.x, p.x), std::max(highest.y, p.y), std::max(highest.z, p.z)};
            }
    if (shell.empty()) {
        error = "入力のボリュームに内部がありません";
        return {};
    }
    const float longest = std::max({highest.x - lowest.x, highest.y - lowest.y, highest.z - lowest.z, g.spacing});
    // 局所の欠けは稜線や角で起こす。平らな面の中央では球の縁が丸いくぼみとして残るが、
    // 凸な場所なら縁が形の外へ出て、平面の小面だけが残る。26近傍の外部の点の数で凸さを測る
    // （平面で約9、稜線で約15、角で約19）。凸な点がなければ表面全体から選ぶ。
    std::vector<uint32_t> candidates;
    if (s.scope == PlaneCutsScope::Local) {
        for (uint32_t i = 0; i < shellCells.size(); ++i) {
            const auto [x, y, z] = shellCells[i];
            int outside = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) outside += g.values[g.Index(x + dx, y + dy, z + dz)] >= 0 ? 1 : 0;
            if (outside >= kLocalCutConvexNeighbours) candidates.push_back(i);
        }
        if (candidates.empty()) {
            candidates.resize(shellCells.size());
            std::iota(candidates.begin(), candidates.end(), 0u);
        }
    }
    // 固定の乱数列（splitmix64）。標準ライブラリの分布は処理系で結果が変わるので使わない。
    uint64_t state = (uint64_t(uint32_t(s.seed)) << 32) ^ 0x9E3779B97F4A7C15ull;
    const auto uniform = [&state]() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return double(z >> 11) / 9007199254740992.0;  // [0, 1)
    };
    const Vec3 axes[3] = {Rotate({1, 0, 0}, s.rotationDegrees), Rotate({0, 1, 0}, s.rotationDegrees),
                          Rotate({0, 0, 1}, s.rotationDegrees)};
    constexpr double pi = std::numbers::pi;
    std::vector<CutPlane> planes;
    planes.reserve(size_t(s.count));
    for (int i = 0; i < s.count; ++i) {
        // 分布によらず1枚あたり同じ個数の乱数を使い、設定を切り替えても他の平面の乱数がずれないようにする。
        const double u0 = uniform(), u1 = uniform(), u2 = uniform(), u3 = uniform(), u4 = uniform(),
                     u5 = uniform();
        double n[3];
        if (s.distribution == PlaneCutsDistribution::Isotropic) {
            const double z = 1 - 2 * u0, r = std::sqrt(std::max(0.0, 1 - z * z)), phi = 2 * pi * u1;
            n[0] = r * std::cos(phi);
            n[1] = r * std::sin(phi);
            n[2] = z;
        } else {
            const int system = std::min(int(u0 * s.systems), s.systems - 1);
            const double sign = u1 < .5 ? -1 : 1;
            const Vec3 main = axes[system], side = axes[(system + 1) % 3], up = axes[(system + 2) % 3];
            // 主方向まわりの円錐の中で一様に傾ける。
            const double tilt = double(s.spreadDegrees) * pi / 180 * std::sqrt(u2), phi = 2 * pi * u3;
            const double c = std::cos(tilt) * sign, a = std::sin(tilt) * std::cos(phi), b = std::sin(tilt) * std::sin(phi);
            n[0] = main.x * c + side.x * a + up.x * b;
            n[1] = main.y * c + side.y * a + up.y * b;
            n[2] = main.z * c + side.z * a + up.z * b;
        }
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (double& v : n) v /= length;
        double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
        for (const auto& p : shell) {
            const double d = n[0] * p.x + n[1] * p.y + n[2] * p.z;
            low = std::min(low, d);
            high = std::max(high, d);
        }
        const double depth = s.depthMin + (double(s.depthMax) - s.depthMin) * u4;
        if (s.scope == PlaneCutsScope::Global) {
            planes.push_back({{float(n[0]), float(n[1]), float(n[2])}, float(high - depth * (high - low)), {}, 0});
            continue;
        }
        // 局所。表面の点を中心に選ぶ。平面が表面に対して急だと、浅い欠けにならず球の半径いっぱいまで
        // 食い込む。法線はその点の外向き（距離場の勾配）に近いものへ寄せる。
        const size_t pick = candidates[std::min(size_t(u5 * double(candidates.size())), candidates.size() - 1)];
        const auto [cx, cy, cz] = shellCells[pick];
        double outward[3] = {double(g.values[g.Index(cx + 1, cy, cz)]) - g.values[g.Index(cx - 1, cy, cz)],
                             double(g.values[g.Index(cx, cy + 1, cz)]) - g.values[g.Index(cx, cy - 1, cz)],
                             double(g.values[g.Index(cx, cy, cz + 1)]) - g.values[g.Index(cx, cy, cz - 1)]};
        const double steepness = std::sqrt(outward[0] * outward[0] + outward[1] * outward[1] + outward[2] * outward[2]);
        if (steepness > 0) {
            for (double& v : outward) v /= steepness;
            const auto facing = [&](const double* v) { return v[0] * outward[0] + v[1] * outward[1] + v[2] * outward[2]; };
            if (s.distribution == PlaneCutsDistribution::Directional) {
                // 外を向く側へ反転する。それでも表面と向きが合わない系統なら、最も合う系統の軸を
                // 同じ傾きのまま使う（節理に沿う面は、その向きの表面にだけ現れる）。
                if (facing(n) < 0)
                    for (double& v : n) v = -v;
                if (facing(n) < kLocalCutFacing) {
                    int best = 0;
                    double bestFacing = -1;
                    for (int a = 0; a < s.systems; ++a) {
                        const double axis[3] = {axes[a].x, axes[a].y, axes[a].z};
                        if (std::abs(facing(axis)) > bestFacing) {
                            bestFacing = std::abs(facing(axis));
                            best = a;
                        }
                    }
                    const double sign = axes[best].x * outward[0] + axes[best].y * outward[1] + axes[best].z * outward[2] < 0 ? -1 : 1;
                    const Vec3 main = axes[best], side = axes[(best + 1) % 3], up = axes[(best + 2) % 3];
                    const double tilt = double(s.spreadDegrees) * pi / 180 * std::sqrt(u2), phi = 2 * pi * u3;
                    const double c = std::cos(tilt) * sign, a = std::sin(tilt) * std::cos(phi),
                                 b = std::sin(tilt) * std::sin(phi);
                    n[0] = main.x * c + side.x * a + up.x * b;
                    n[1] = main.y * c + side.y * a + up.y * b;
                    n[2] = main.z * c + side.z * a + up.z * b;
                }
            } else {
                // 外向きの法線にランダムな向きを混ぜる。傾きは最大で約50度。
                for (int a = 0; a < 3; ++a) n[a] = outward[a] + n[a] * kLocalCutTilt;
                const double mixed = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (double& v : n) v /= mixed;
            }
        }
        const Vec3 center = shell[pick];
        const double radius = double(s.radius) * longest;
        const double along = n[0] * center.x + n[1] * center.y + n[2] * center.z;
        // 切り取る材料が球の縁に多く掛かると、球の壁が丸いくぼみとして残る。縁に掛かる割合を
        // 半径で正規化すると、平らな面のくぼみで約2、稜線の面取りで約1、角の欠けで約0になる。
        // 稜線と角だけを通し、掛かりすぎる欠けは浅くして試し直す。それでも駄目なら使わない
        // （枚数は設定より減る）。
        const int reach = int(std::ceil(radius / g.spacing)) + 1;
        const auto clipped = [&](int value, uint32_t limit) { return uint32_t(std::clamp(value, 0, int(limit) - 1)); };
        const uint32_t x0 = clipped(int(cx) - reach, g.dimensions[0]), x1 = clipped(int(cx) + reach, g.dimensions[0]),
                       y0 = clipped(int(cy) - reach, g.dimensions[1]), y1 = clipped(int(cy) + reach, g.dimensions[1]),
                       z0 = clipped(int(cz) - reach, g.dimensions[2]), z1 = clipped(int(cz) + reach, g.dimensions[2]);
        const double rim = std::max(radius - 1.5 * g.spacing, 0.0);
        double chipDepth = depth * radius;
        for (int attempt = 0; attempt < 4; ++attempt, chipDepth *= .5) {
            const double offset = along - chipDepth;
            size_t removed = 0, onRim = 0;
            for (uint32_t z = z0; z <= z1; ++z)
                for (uint32_t y = y0; y <= y1; ++y)
                    for (uint32_t x = x0; x <= x1; ++x) {
                        if (g.values[g.Index(x, y, z)] >= 0) continue;
                        const auto p = g.Position(x, y, z);
                        if (n[0] * p.x + n[1] * p.y + n[2] * p.z <= offset) continue;
                        const double dx = p.x - center.x, dy = p.y - center.y, dz = p.z - center.z;
                        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (distance > radius) continue;
                        ++removed;
                        if (distance >= rim) ++onRim;
                    }
            if (double(onRim) * radius > kLocalCutRimLimit * double(removed) * (radius - rim)) continue;
            planes.push_back({{float(n[0]), float(n[1]), float(n[2])}, float(offset), center, float(radius)});
            break;
        }
    }
    return planes;
}
VolumeGrid CutVolume(const VolumeGrid& g, const PlaneCutsSettings& s, std::string& error) {
    const auto planes = MakeCutPlanes(g, s, error);
    if (!error.empty()) return {};
    VolumeGrid out;
    out.origin = g.origin;
    out.spacing = g.spacing;
    out.dimensions = g.dimensions;
    out.values.resize(g.values.size());
    const float threshold = out.spacing * 1e-4f;
    const float k = s.blend;
    const bool inside = FillSlices(out, [&](uint32_t x, uint32_t y, uint32_t z) {
        const auto p = out.Position(x, y, z);
        const size_t index = out.Index(x, y, z);
        float value = g.values[index];
        for (const auto& plane : planes) {
            float cut = Dot(plane.normal, p) - plane.offset;
            if (plane.radius > 0) {
                // 局所の欠け。切り落とす領域は「平面の外側」かつ「球の中」。
                // 球の壁が形に当たらない欠けだけを MakeCutPlanes が選ぶので、切断面は平面だけになる。
                const Vec3 d{p.x - plane.center.x, p.y - plane.center.y, p.z - plane.center.z};
                cut = std::min(cut, plane.radius - std::sqrt(Dot(d, d)));
            }
            if (k > 0) {
                // 多項式のなめらかな最大値。差が幅以上なら通常の最大値と同じ。
                const float h = std::max(k - std::abs(value - cut), 0.f) / k;
                value = std::max(value, cut) + h * h * k * .25f;
            } else {
                value = std::max(value, cut);
            }
        }
        // 等値面が格子頂点に一致する場合も同じ符号に寄せ、ゼロ長の交点辺を避ける。
        out.values[index] = std::abs(value) < threshold ? threshold : value;
        return value < 0;
    });
    if (!inside) {
        error = "切り落とした結果に内部が残りません。枚数か切り込みの深さを減らしてください";
        return {};
    }
    KeepLargestComponent(out);
    return out;
}
VolumeGrid CrackVolume(const VolumeGrid& g, const std::vector<Vec3>& points, const VolumeCrackSettings& s,
                       std::string& error) {
    error.clear();
    if (!ValidGrid(g)) {
        error = "ボリュームの格子が不正です";
        return {};
    }
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (points.size() < 2 || points.size() > size_t(MaxCrackPoints)) {
        error = "割れ目には2～512個の点が必要です";
        return {};
    }
    for (const auto& p : points)
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            error = "点が不正です";
            return {};
        }
    if (!range(s.width, 0, .2f)) {
        error = "割れ目の幅は 0～0.2 にしてください";
        return {};
    }
    if (!range(s.depth, .01f, 1)) {
        error = "割れ目の深さは 0.01～1 にしてください";
        return {};
    }
    if (!range(s.variation, 0, 1) || !range(s.noise, 0, 1)) {
        error = "ばらつきとゆらぎは 0～1 にしてください";
        return {};
    }
    if (!range(s.noiseScale, .5f, 16)) {
        error = "ゆらぎの細かさは 0.5～16 にしてください";
        return {};
    }
    const float longest = InteriorLongestSide(g);
    if (longest <= 0) {
        error = "入力のボリュームに内部がありません";
        return {};
    }
    const size_t count = points.size();
    // 点の対ごとの距離と幅の倍率。倍率は対で決まるので、境界面のどちら側から見ても同じ割れ目になる。
    std::vector<float> separation(count * count, 0), factor(count * count, 0);
    const uint64_t seed = uint64_t(uint32_t(s.seed)) << 40;
    for (size_t i = 0; i < count; ++i)
        for (size_t j = i + 1; j < count; ++j) {
            const Vec3 d{points[j].x - points[i].x, points[j].y - points[i].y, points[j].z - points[i].z};
            const float length = std::sqrt(Dot(d, d));
            if (!(length > longest * 1e-6f)) {
                error = "重複または近接しすぎた点があります";
                return {};
            }
            // ばらつき 1 では半分ほどの割れ目が閉じ、残りは 0～1 倍に散らばる。
            const float scale = std::clamp(1 - s.variation * 2 * float(HashUnit(seed ^ (uint64_t(i) << 20) ^ uint64_t(j))), 0.f, 1.f);
            separation[i * count + j] = separation[j * count + i] = length;
            factor[i * count + j] = factor[j * count + i] = scale;
        }
    VolumeGrid out;
    out.origin = g.origin;
    out.spacing = g.spacing;
    out.dimensions = g.dimensions;
    out.values.resize(g.values.size());
    const float threshold = out.spacing * 1e-4f;
    const float halfWidth = s.width * longest * .5f, depth = s.depth * longest;
    const float frequency = s.noiseScale / longest;
    const bool inside = FillSlices(out, [&](uint32_t x, uint32_t y, uint32_t z) {
        const size_t index = out.Index(x, y, z);
        float value = g.values[index];
        // 割れ目は深くなるほど狭まる。表面（と外側）で最も広く、指定の深さで幅が 0 になる。
        const float profile = std::clamp(1 + value / depth, 0.f, 1.f);
        float reach = halfWidth * profile;
        // 最大の幅でも届かない点（形の外側の遠くと、深い内部）は境界面までの距離を求めない。
        if (reach > 0 && value < reach) {
            const auto p = out.Position(x, y, z);
            if (s.noise > 0) {
                const float n = ValueNoise(p.x * frequency, p.y * frequency, p.z * frequency, seed);
                // ゆらぎ 1 なら、ノイズの低いところで割れ目が途切れる。
                reach *= std::clamp(1 - s.noise * 2 * (1 - n), 0.f, 1.f);
            }
            size_t nearest = 0;
            float nearestSquared = std::numeric_limits<float>::max();
            thread_local std::vector<float> squared;
            squared.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const Vec3 d{p.x - points[i].x, p.y - points[i].y, p.z - points[i].z};
                squared[i] = Dot(d, d);
                if (squared[i] < nearestSquared) {
                    nearestSquared = squared[i];
                    nearest = i;
                }
            }
            // Voronoi のセルは凸なので、各垂直二等分面までの距離の最小が境界面までの厳密な距離になる。
            // 割れ目ごとに幅が違うため、最小ではなく「幅 − 距離」の最大を取る。
            float carve = -std::numeric_limits<float>::max();
            for (size_t j = 0; j < count; ++j) {
                if (j == nearest) continue;
                const float boundary = (squared[j] - nearestSquared) / (2 * separation[nearest * count + j]);
                carve = std::max(carve, reach * factor[nearest * count + j] - boundary);
            }
            value = std::max(value, carve);
        }
        // 等値面が格子頂点に一致する場合も同じ符号に寄せ、ゼロ長の交点辺を避ける。
        out.values[index] = std::abs(value) < threshold ? threshold : value;
        return value < 0;
    });
    if (!inside) {
        error = "割れ目を彫った結果に内部が残りません。幅か深さを減らしてください";
        return {};
    }
    // 重なった立体から作ったボリュームは、内部に残る面の近くでも距離が小さい。そこは表面と
    // 同じ幅で彫られ、外へつながらない空洞になる。
    FillNewVoids(out, g.values);
    return out;
}
const char* VolumeNoiseTypeName(VolumeNoiseType type) {
    switch (type) {
        case VolumeNoiseType::Cellular: return "cellular";
        case VolumeNoiseType::Facet: return "facet";
        default: return "smooth";
    }
}
VolumeNoiseType ParseVolumeNoiseType(std::string_view name) {
    if (name == "cellular") return VolumeNoiseType::Cellular;
    if (name == "facet") return VolumeNoiseType::Facet;
    return VolumeNoiseType::Smooth;
}
namespace {
// セル状のノイズ。各格子セルに特徴点を1つ置き、最も近い特徴点から値を作る。どちらも 0～1。
// Cellular は特徴点までの距離（丸い盛り上がりと、その間の谷）。
// Facet は特徴点ごとのランダムな平面（平らな小面と、セルの境での段差）。
float CellNoise(float x, float y, float z, uint64_t seed, bool facet) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    float best = std::numeric_limits<float>::max(), bx = 0, by = 0, bz = 0;
    uint64_t bestKey = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int64_t ix = int64_t(fx) + dx, iy = int64_t(fy) + dy, iz = int64_t(fz) + dz;
                const uint64_t key = seed ^ (uint64_t(ix) * 0x8DA6B343ull) ^ (uint64_t(iy) * 0xD8163841ull) ^
                                     (uint64_t(iz) * 0xCB1AB31Full);
                const float px = float(ix) + float(HashUnit(key)), py = float(iy) + float(HashUnit(key ^ 0x51ull)),
                            pz = float(iz) + float(HashUnit(key ^ 0xA3ull));
                const float d = (x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz);
                if (d < best) {
                    best = d;
                    bestKey = key;
                    bx = px;
                    by = py;
                    bz = pz;
                }
            }
    if (!facet) return std::min(std::sqrt(best), 1.f);
    const float h = 1 - 2 * float(HashUnit(bestKey ^ 0x1F3ull)), r = std::sqrt(std::max(0.f, 1 - h * h)),
                phi = 2 * std::numbers::pi_v<float> * float(HashUnit(bestKey ^ 0x2E7ull));
    const float along = (x - bx) * r * std::cos(phi) + (y - by) * r * std::sin(phi) + (z - bz) * h;
    return .5f + .5f * std::clamp(along * 1.5f, -1.f, 1.f);
}
}  // namespace
VolumeGrid NoiseVolume(const VolumeGrid& g, const VolumeNoiseSettings& s, std::string& error) {
    error.clear();
    if (!ValidGrid(g)) {
        error = "ボリュームの格子が不正です";
        return {};
    }
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (s.type != VolumeNoiseType::Smooth && s.type != VolumeNoiseType::Cellular && s.type != VolumeNoiseType::Facet) {
        error = "不明なノイズの種類です";
        return {};
    }
    if (!range(s.amount, 0, .2f) || !range(s.warp, 0, .2f)) {
        error = "ノイズの量と歪みは 0～0.2 にしてください";
        return {};
    }
    if (!range(s.scale, .5f, 64) || !range(s.warpScale, .5f, 16)) {
        error = "細かさは 0.5～64、歪みの細かさは 0.5～16 にしてください";
        return {};
    }
    if (s.octaves < 1 || s.octaves > 5) {
        error = "重ねる数は 1～5 にしてください";
        return {};
    }
    const float longest = InteriorLongestSide(g);
    if (longest <= 0) {
        error = "入力のボリュームに内部がありません";
        return {};
    }
    const float amount = s.amount * longest, warp = s.warp * longest;
    // 歪みは表面を外へも動かす。外周を空に保てるよう、動く量だけ格子を広げる。
    const uint32_t pad = warp > 0 ? uint32_t(std::ceil(warp * 1.75f / g.spacing)) + 1 : 0;
    VolumeGrid out;
    out.spacing = g.spacing;
    out.origin = {g.origin.x - float(pad) * g.spacing, g.origin.y - float(pad) * g.spacing,
                  g.origin.z - float(pad) * g.spacing};
    for (int i = 0; i < 3; ++i) {
        if (uint64_t(g.dimensions[i]) + 2 * pad > kMaxGridPointsPerAxis) {
            error = "歪みで広げた格子が各軸256点の上限を超えます。歪みを減らすか、上流の解像度を下げてください";
            return {};
        }
        out.dimensions[i] = g.dimensions[i] + 2 * pad;
    }
    out.values.resize(size_t(out.dimensions[0]) * out.dimensions[1] * out.dimensions[2]);
    std::vector<float> before(out.values.size());
    const float threshold = out.spacing * 1e-4f;
    const uint64_t seed = uint64_t(uint32_t(s.seed)) << 40;
    const float frequency = s.scale / longest, warpFrequency = s.warpScale / longest;
    // 表面からこれより離れた点は、どう加工しても同じ側に残り、隣の点も同じ側にある。
    const float band = warp * 1.75f + amount + 2 * g.spacing;
    const bool inside = FillSlices(out, [&](uint32_t x, uint32_t y, uint32_t z) {
        const size_t index = out.Index(x, y, z);
        const auto p = out.Position(x, y, z);
        const bool onInput = x >= pad && y >= pad && z >= pad && x - pad < g.dimensions[0] &&
                             y - pad < g.dimensions[1] && z - pad < g.dimensions[2];
        float value = onInput ? g.values[g.Index(x - pad, y - pad, z - pad)] : SampleVolume(g, p);
        before[index] = value;
        if (std::abs(value) < band) {
            if (warp > 0) {
                const float wx = p.x * warpFrequency, wy = p.y * warpFrequency, wz = p.z * warpFrequency;
                const Vec3 moved{p.x + (ValueNoise(wx, wy, wz, seed ^ 0x11ull) - .5f) * 2 * warp,
                                 p.y + (ValueNoise(wx, wy, wz, seed ^ 0x22ull) - .5f) * 2 * warp,
                                 p.z + (ValueNoise(wx, wy, wz, seed ^ 0x33ull) - .5f) * 2 * warp};
                value = SampleVolume(g, moved);
            }
            if (amount > 0) {
                float noise = 0, weight = 0, gain = 1, f = frequency;
                for (int octave = 0; octave < s.octaves; ++octave, gain *= .5f, f *= 2) {
                    const uint64_t octaveSeed = seed ^ (uint64_t(octave + 1) * 0x9E37ull);
                    const float n = s.type == VolumeNoiseType::Smooth
                                        ? ValueNoise(p.x * f, p.y * f, p.z * f, octaveSeed)
                                        : CellNoise(p.x * f, p.y * f, p.z * f, octaveSeed,
                                                    s.type == VolumeNoiseType::Facet);
                    noise += n * gain;
                    weight += gain;
                }
                // 削る方向にだけ効かせる。形は広がらない。
                value += amount * noise / weight;
            }
        }
        out.values[index] = std::abs(value) < threshold ? threshold : value;
        return value < 0;
    });
    if (!inside) {
        error = "ノイズで削った結果に内部が残りません。量を減らしてください";
        return {};
    }
    // 細かいノイズは、浮いた小片や閉じた空洞を作ることがある。
    KeepLargestComponent(out);
    FillNewVoids(out, before);
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
        // 表面の頂点数は断面のセル数に比例する。成長中の再ハッシュを減らす（IDの割り当て順は変わらない）。
        crossings.reserve(16 * std::max({size_t(nx) * ny, size_t(ny) * nz, size_t(nx) * nz}));
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
    return mesh;
}
}  // namespace rock::geometry
