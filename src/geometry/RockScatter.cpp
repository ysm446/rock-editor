#include "geometry/RockScatter.h"
#include "geometry/UvUnwrap.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace rock::geometry {
namespace {
// 決まった順に数を出す乱数（splitmix64）。
struct Random {
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e019ull) {}
    uint64_t Next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    // [0, 1)
    double Uniform() { return double(Next() >> 11) * (1.0 / 9007199254740992.0); }
};

struct Cell {
    int x, y, z;
    bool operator==(const Cell&) const = default;
};
struct CellHash {
    size_t operator()(const Cell& c) const {
        return size_t(uint32_t(c.x) * 73856093u ^ uint32_t(c.y) * 19349663u ^ uint32_t(c.z) * 83492791u);
    }
};
}  // namespace

bool ValidateRockScatterSettings(const RockScatterSettings& s, std::string& error) {
    if (!std::isfinite(s.spacing) || s.spacing < kMinScatterSpacing || s.spacing > 10000.0f) {
        error = "間隔は 0.05〜10000 m にしてください";
        return false;
    }
    if (s.maxCount < 1 || s.maxCount > kMaxScatterCount) {
        error = "置く数の上限は 1〜50000 にしてください";
        return false;
    }
    if (!std::isfinite(s.scaleMin) || !std::isfinite(s.scaleMax) || s.scaleMin <= 0 || s.scaleMax < s.scaleMin ||
        s.scaleMax > 100) {
        error = "倍率の範囲が不正です（最小 ≦ 最大、0 より大きく）";
        return false;
    }
    if (!std::isfinite(s.alignToNormal) || s.alignToNormal < 0 || s.alignToNormal > 1 || !std::isfinite(s.embed) ||
        s.embed < 0 || s.embed > 0.9f) {
        error = "法線への合わせ込みは 0〜1、沈める量は 0〜0.9 にしてください";
        return false;
    }
    return true;
}

std::vector<RockInstance> ScatterRocks(const Mesh& surface, const MaskImage* mask, bool invertMask,
                                       const std::vector<float>& weights, const RockScatterSettings& s,
                                       std::string& error, std::stop_token stop) {
    error.clear();
    if (!ValidateRockScatterSettings(s, error)) return {};
    if (surface.triangles.empty()) {
        error = "地形に三角形がありません";
        return {};
    }
    double weightSum = 0;
    for (const float weight : weights) weightSum += std::isfinite(weight) ? std::max(0.0f, weight) : 0.0f;
    if (weights.empty() || !(weightSum > 0)) {
        error = "岩の重みの合計が 0 です";
        return {};
    }
    const bool useMask = mask != nullptr && mask->width > 0 && mask->height > 0 && HasValidUvs(surface);
    if (mask != nullptr && !useMask) {
        error = "マスクを使うには、地形に UV が必要です";
        return {};
    }
    // 面積の累積。面積に比例して面を選ぶ。
    std::vector<double> cumulative(surface.triangles.size());
    double area = 0;
    for (size_t f = 0; f < surface.triangles.size(); ++f) {
        const auto& t = surface.triangles[f];
        const Vec3 a = surface.positions[t[0]], b = surface.positions[t[1]], c = surface.positions[t[2]];
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        area += std::sqrt(cx * cx + cy * cy + cz * cz) * 0.5;
        cumulative[f] = area;
    }
    if (!(area > 0)) {
        error = "地形の面積が 0 です";
        return {};
    }
    // 候補の数。間隔の円が表面を覆う数のおよそ 3 倍を投げる（ダーツ投げは詰まるほど外れが増える）。
    const double spacing = s.spacing;
    const double candidates = std::min(2.0e6, std::ceil(area / (spacing * spacing) * 3.0) + 16.0);
    Random random(uint64_t(uint32_t(s.seed)) + 1);
    std::unordered_map<Cell, std::vector<uint32_t>, CellHash> grid;
    const auto cellOf = [&](const Vec3& p) {
        return Cell{int(std::floor(p.x / spacing)), int(std::floor(p.y / spacing)), int(std::floor(p.z / spacing))};
    };
    std::vector<RockInstance> result;
    const double spacing2 = spacing * spacing;
    for (double trial = 0; trial < candidates && result.size() < size_t(s.maxCount); ++trial) {
        if ((uint64_t(trial) & 4095) == 0 && stop.stop_requested()) {
            error = "評価をキャンセルしました";
            return {};
        }
        // 面を面積で選び、面の中の点を一様に選ぶ。
        const double pick = random.Uniform() * area;
        const size_t f = size_t(std::lower_bound(cumulative.begin(), cumulative.end(), pick) - cumulative.begin());
        const auto& t = surface.triangles[std::min(f, surface.triangles.size() - 1)];
        double r1 = random.Uniform(), r2 = random.Uniform();
        if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
        const double r0 = 1 - r1 - r2;
        const Vec3 a = surface.positions[t[0]], b = surface.positions[t[1]], c = surface.positions[t[2]];
        const Vec3 p{float(a.x * r0 + b.x * r1 + c.x * r2), float(a.y * r0 + b.y * r1 + c.y * r2),
                     float(a.z * r0 + b.z * r1 + c.z * r2)};
        // マスク（地形の UV の画像）の値を置く確率にする。乱数はマスクが無くても同じだけ引く（配置を揃える）。
        const double gate = random.Uniform();
        if (useMask) {
            const auto& uv = surface.cornerUvs[std::min(f, surface.triangles.size() - 1)];
            const float u = float(uv[0].u * r0 + uv[1].u * r1 + uv[2].u * r2);
            const float v = float(uv[0].v * r0 + uv[1].v * r1 + uv[2].v * r2);
            float value = mask->Sample(u, v);
            if (invertMask) value = 1.0f - value;
            if (gate >= value) {
                // 向き・倍率・岩の選択の分も引いておく（候補ごとの乱数の数を揃え、マスクを変えても
                // 残った場所の岩の向きや大きさが変わらないようにする）。
                random.Next(); random.Next(); random.Next();
                continue;
            }
        }
        // 近くに置いた岩があれば捨てる。
        const Cell cell = cellOf(p);
        bool near = false;
        for (int dx = -1; dx <= 1 && !near; ++dx)
            for (int dy = -1; dy <= 1 && !near; ++dy)
                for (int dz = -1; dz <= 1 && !near; ++dz) {
                    const auto found = grid.find(Cell{cell.x + dx, cell.y + dy, cell.z + dz});
                    if (found == grid.end()) continue;
                    for (const uint32_t other : found->second) {
                        const Vec3& q = result[other].position;
                        const double ex = q.x - p.x, ey = q.y - p.y, ez = q.z - p.z;
                        if (ex * ex + ey * ey + ez * ez < spacing2) { near = true; break; }
                    }
                }
        const double yaw = random.Uniform() * 2.0 * std::numbers::pi;
        const double scaleRandom = random.Uniform();
        const double choose = random.Uniform() * weightSum;
        if (near) continue;
        RockInstance instance;
        instance.position = p;
        // 面の法線へ alignToNormal だけ傾けた上向き。
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl > 0) { nx /= nl; ny /= nl; nz /= nl; } else { nx = 0; ny = 1; nz = 0; }
        if (ny < 0) { nx = -nx; ny = -ny; nz = -nz; }  // 地形の裏を向いた面でも上へ
        const double k = s.alignToNormal;
        double upx = nx * k, upy = (1 - k) + ny * k, upz = nz * k;
        const double ul = std::sqrt(upx * upx + upy * upy + upz * upz);
        if (ul > 0) { upx /= ul; upy /= ul; upz /= ul; } else { upx = 0; upy = 1; upz = 0; }
        instance.up = {float(upx), float(upy), float(upz)};
        instance.yaw = float(yaw);
        instance.scale = float(s.scaleMin + (s.scaleMax - s.scaleMin) * scaleRandom);
        double accumulated = 0;
        instance.rock = uint32_t(weights.size() - 1);
        for (size_t i = 0; i < weights.size(); ++i) {
            accumulated += std::isfinite(weights[i]) ? std::max(0.0f, weights[i]) : 0.0f;
            if (choose < accumulated) { instance.rock = uint32_t(i); break; }
        }
        instance.embed = s.embed;
        grid[cell].push_back(uint32_t(result.size()));
        result.push_back(instance);
    }
    return result;
}
}  // namespace rock::geometry
