#include "geometry/RockScatter.h"
#include "geometry/UvUnwrap.h"

#include <algorithm>
#include <cmath>
#include <memory>
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

// 地形を XZ の格子で引き、点の真上の面と高さ・UV を読む。地形は高さ場（XZ の 1 点に面が 1 つ）を前提にするが、
// 重なっていれば最も高い面を返す。
struct SurfaceLocator {
    const Mesh& mesh;
    double minX = 0, minZ = 0, cell = 1;
    int nx = 1, nz = 1;
    std::vector<std::vector<uint32_t>> bins;

    explicit SurfaceLocator(const Mesh& m) : mesh(m) {
        double maxX = -1e300, maxZ = -1e300;
        minX = minZ = 1e300;
        for (const auto& p : mesh.positions) {
            minX = std::min<double>(minX, p.x); maxX = std::max<double>(maxX, p.x);
            minZ = std::min<double>(minZ, p.z); maxZ = std::max<double>(maxZ, p.z);
        }
        if (mesh.positions.empty()) { minX = minZ = 0; maxX = maxZ = 1; }
        const double sizeX = std::max(maxX - minX, 1e-6), sizeZ = std::max(maxZ - minZ, 1e-6);
        // 面 1 つあたりの XZ の面積から格子の一辺を決める（1 セルに数面）。
        const double perFace = std::sqrt(sizeX * sizeZ / double(std::max<size_t>(mesh.triangles.size(), 1)));
        cell = std::max(perFace * 2.0, std::max(sizeX, sizeZ) / 2048.0);
        nx = std::max(1, int(std::ceil(sizeX / cell)) + 1);
        nz = std::max(1, int(std::ceil(sizeZ / cell)) + 1);
        bins.resize(size_t(nx) * nz);
        for (uint32_t f = 0; f < mesh.triangles.size(); ++f) {
            const auto& t = mesh.triangles[f];
            double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
            for (int k = 0; k < 3; ++k) {
                const auto& p = mesh.positions[t[k]];
                x0 = std::min<double>(x0, p.x); x1 = std::max<double>(x1, p.x);
                z0 = std::min<double>(z0, p.z); z1 = std::max<double>(z1, p.z);
            }
            const int ix0 = Clamp(int((x0 - minX) / cell), nx), ix1 = Clamp(int((x1 - minX) / cell), nx);
            const int iz0 = Clamp(int((z0 - minZ) / cell), nz), iz1 = Clamp(int((z1 - minZ) / cell), nz);
            for (int iz = iz0; iz <= iz1; ++iz)
                for (int ix = ix0; ix <= ix1; ++ix) bins[size_t(iz) * nx + ix].push_back(f);
        }
    }
    static int Clamp(int v, int n) { return std::clamp(v, 0, n - 1); }

    // 面 f の XZ での重心座標（面の外では範囲外の値になる。縮退していれば false）。
    bool Barycentric(uint32_t f, double x, double z, double& w0, double& w1, double& w2) const {
        const auto& t = mesh.triangles[f];
        const auto& a = mesh.positions[t[0]];
        const auto& b = mesh.positions[t[1]];
        const auto& c = mesh.positions[t[2]];
        const double det = (double(b.x) - a.x) * (double(c.z) - a.z) - (double(c.x) - a.x) * (double(b.z) - a.z);
        if (std::abs(det) < 1e-12) return false;
        w1 = ((x - a.x) * (double(c.z) - a.z) - (double(c.x) - a.x) * (z - a.z)) / det;
        w2 = ((double(b.x) - a.x) * (z - a.z) - (x - a.x) * (double(b.z) - a.z)) / det;
        w0 = 1 - w1 - w2;
        return true;
    }
    // 点 (x, z) の真上の面。重なっていれば最も高い面。
    bool Locate(double x, double z, uint32_t& face, double& w0, double& w1, double& w2) const {
        const int ix = int(std::floor((x - minX) / cell)), iz = int(std::floor((z - minZ) / cell));
        if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return false;
        bool found = false;
        double best = -1e300;
        for (const uint32_t f : bins[size_t(iz) * nx + ix]) {
            double b0, b1, b2;
            if (!Barycentric(f, x, z, b0, b1, b2)) continue;
            const double eps = -1e-6;
            if (b0 < eps || b1 < eps || b2 < eps) continue;
            const auto& t = mesh.triangles[f];
            const double y = mesh.positions[t[0]].y * b0 + mesh.positions[t[1]].y * b1 + mesh.positions[t[2]].y * b2;
            if (!found || y > best) { found = true; best = y; face = f; w0 = b0; w1 = b1; w2 = b2; }
        }
        return found;
    }
    bool HeightAt(double x, double z, double& y) const {
        uint32_t f; double w0, w1, w2;
        if (!Locate(x, z, f, w0, w1, w2)) return false;
        const auto& t = mesh.triangles[f];
        y = mesh.positions[t[0]].y * w0 + mesh.positions[t[1]].y * w1 + mesh.positions[t[2]].y * w2;
        return true;
    }
};

// 上向きのまわりの回転（描画の XMMatrixRotationY と同じ向き）。
Vec3 RotateYaw(const Vec3& p, float yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    return {float(p.x * c + p.z * s), p.y, float(-p.x * s + p.z * c)};
}
// +Y を up へ向ける回転（描画の align と同じ）。
Vec3 TiltToUp(const Vec3& p, const Vec3& up) {
    const double ul = std::sqrt(double(up.x) * up.x + double(up.y) * up.y + double(up.z) * up.z);
    if (!(ul > 0)) return p;
    const double ux = up.x / ul, uy = up.y / ul, uz = up.z / ul;
    const double cosine = uy;
    if (cosine > 0.9999) return p;
    // 軸 = y × up = (uz, 0, -ux)、角 = acos(cosine)。Rodrigues の回転。
    double ax = uz, az = -ux;
    const double al = std::sqrt(ax * ax + az * az);
    if (!(al > 0)) return {p.x, -p.y, p.z};
    ax /= al; az /= al;
    const double angle = std::acos(std::clamp(cosine, -1.0, 1.0));
    const double c = std::cos(angle), s = std::sin(angle);
    const double dot = ax * p.x + az * p.z;
    // 軸 × p（軸の y は 0）。
    const double cx = -az * p.y, cy = az * p.x - ax * p.z, cz = ax * p.y;
    return {float(p.x * c + cx * s + ax * dot * (1 - c)), float(p.y * c + cy * s),
            float(p.z * c + cz * s + az * dot * (1 - c))};
}
// 足元の半径（底の投影を包む円。倍率込み）。範囲が無ければ 0。
double FootprintRadius(const RockScatterSource& source, float scale) {
    if (!source.hasBounds) return 0;
    const double ex = (double(source.maximum.x) - source.minimum.x) * scale, ez = (double(source.maximum.z) - source.minimum.z) * scale;
    return 0.5 * std::sqrt(ex * ex + ez * ez);
}
double FootprintArea(const RockScatterSource& source, float scale) {
    if (!source.hasBounds) return 0;
    return (double(source.maximum.x) - source.minimum.x) * (double(source.maximum.z) - source.minimum.z) * scale * scale;
}
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
    if (!std::isfinite(s.sizeSpacing) || s.sizeSpacing < 0 || s.sizeSpacing > 10) {
        error = "大きさの間隔は 0〜10 倍にしてください";
        return false;
    }
    if (!std::isfinite(s.settle) || s.settle < 0 || s.settle > 1 || !std::isfinite(s.settleMax) || s.settleMax < 0 ||
        s.settleMax > 0.9f) {
        error = "浮きの補正は 0〜1、補正の上限は 0〜0.9 にしてください";
        return false;
    }
    if (!std::isfinite(s.coverageTarget) || s.coverageTarget < 0 || s.coverageTarget > 1) {
        error = "目標の被覆率は 0〜1 にしてください";
        return false;
    }
    return true;
}

bool RockFootprint(const RockInstance& instance, const RockScatterSource& source, std::array<Vec3, 4>& corners) {
    if (!source.hasBounds) return false;
    const float scale = instance.scale * source.scale;
    const float hx = 0.5f * (source.maximum.x - source.minimum.x) * scale, hz = 0.5f * (source.maximum.z - source.minimum.z) * scale;
    const float height = (source.maximum.y - source.minimum.y) * scale;
    // 底面の中心を原点に、沈めた分だけ下げた 4 隅。描画の行列と同じ順（倍率 → 沈める → 回す → 傾ける → 置く）。
    const float y = -instance.embed * height;
    const Vec3 local[4] = {{-hx, y, -hz}, {hx, y, -hz}, {hx, y, hz}, {-hx, y, hz}};
    for (int i = 0; i < 4; ++i) {
        const Vec3 world = TiltToUp(RotateYaw(local[i], instance.yaw), instance.up);
        corners[i] = {world.x + instance.position.x, world.y + instance.position.y, world.z + instance.position.z};
    }
    return true;
}

RockScatterResult ScatterRocks(const Mesh& surface, const MaskImage* mask, bool invertMask,
                               const std::vector<RockScatterSource>& sources, const RockScatterSettings& s,
                               std::string& error, std::stop_token stop) {
    error.clear();
    RockScatterResult output;
    if (!ValidateRockScatterSettings(s, error)) return {};
    if (surface.triangles.empty()) {
        error = "地形に三角形がありません";
        return {};
    }
    double weightSum = 0;
    for (const auto& source : sources) weightSum += std::isfinite(source.weight) ? std::max(0.0f, source.weight) : 0.0f;
    if (sources.empty() || !(weightSum > 0)) {
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
    // 大きさの間隔。いちばん大きい岩の足元の半径の 2 倍が格子の一辺に収まるようにする。
    double maxRadius = 0;
    bool anyBounds = false;
    for (const auto& source : sources) {
        if (!source.hasBounds || !(source.weight > 0)) continue;
        anyBounds = true;
        maxRadius = std::max(maxRadius, FootprintRadius(source, s.scaleMax));
    }
    const bool useSizeSpacing = s.sizeSpacing > 0 && anyBounds;
    const bool useSettle = s.settle > 0 && s.settleMax > 0 && anyBounds;
    std::unique_ptr<SurfaceLocator> locator;
    if (useSettle) locator = std::make_unique<SurfaceLocator>(surface);
    // 候補の数。間隔の円が表面を覆う数のおよそ 3 倍を投げる（ダーツ投げは詰まるほど外れが増える）。
    const double spacing = s.spacing;
    const double cellSize = std::max(spacing, useSizeSpacing ? 2.0 * maxRadius * s.sizeSpacing : 0.0);
    const double candidates = std::min(2.0e6, std::ceil(area / (spacing * spacing) * 3.0) + 16.0);
    Random random(uint64_t(uint32_t(s.seed)) + 1);
    std::unordered_map<Cell, std::vector<uint32_t>, CellHash> grid;
    const auto cellOf = [&](const Vec3& p) {
        return Cell{int(std::floor(p.x / cellSize)), int(std::floor(p.y / cellSize)), int(std::floor(p.z / cellSize))};
    };
    std::vector<RockInstance>& result = output.instances;
    std::vector<double> radii;  // 置いた岩の足元の半径（倍率込み）
    const double spacing2 = spacing * spacing;
    double covered = 0;
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
        // 向き・倍率・どの岩か（近さの判定より先に決める。大きさの間隔はこの岩の大きさで判定する）。
        const double yaw = random.Uniform() * 2.0 * std::numbers::pi;
        const double scaleRandom = random.Uniform();
        const double choose = random.Uniform() * weightSum;
        RockInstance instance;
        instance.scale = float(s.scaleMin + (s.scaleMax - s.scaleMin) * scaleRandom);
        double accumulated = 0;
        instance.rock = uint32_t(sources.size() - 1);
        for (size_t i = 0; i < sources.size(); ++i) {
            accumulated += std::isfinite(sources[i].weight) ? std::max(0.0f, sources[i].weight) : 0.0f;
            if (choose < accumulated) { instance.rock = uint32_t(i); break; }
        }
        const RockScatterSource& source = sources[instance.rock];
        const double radius = useSizeSpacing ? FootprintRadius(source, instance.scale * source.scale) : 0.0;
        // 近くに置いた岩があれば捨てる。間隔と、足元の半径の和 × 大きさの間隔の大きい方。
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
                        const double d2 = ex * ex + ey * ey + ez * ez;
                        if (d2 < spacing2) { near = true; break; }
                        if (useSizeSpacing) {
                            const double required = (radius + radii[other]) * s.sizeSpacing;
                            if (d2 < required * required) { near = true; break; }
                        }
                    }
                }
        if (near) continue;
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
        instance.embed = s.embed;
        // 浮きの補正。底の 4 隅で地形の高さを読み、浮いている隅が無くなる深さまで追加で沈める（上限あり）。
        if (useSettle && source.hasBounds) {
            std::array<Vec3, 4> corners;
            double floating = 0;
            if (RockFootprint(instance, source, corners)) {
                for (const auto& corner : corners) {
                    double ground;
                    if (locator->HeightAt(corner.x, corner.z, ground)) floating = std::max(floating, double(corner.y) - ground);
                }
            }
            const double height = (double(source.maximum.y) - source.minimum.y) * instance.scale * source.scale;
            if (floating > 0 && height > 0) {
                // 沈めるのは上向きに沿うので、鉛直の浮きを up.y で割る（寝かせ過ぎた岩でも無限にはしない）。
                const double along = floating / std::max(upy, 0.25);
                const double extra = std::min(double(s.settleMax), s.settle * along / height);
                instance.embed = float(std::min(0.95, double(instance.embed) + extra));
            }
        }
        grid[cell].push_back(uint32_t(result.size()));
        radii.push_back(radius);
        result.push_back(instance);
        covered += FootprintArea(source, instance.scale * source.scale);
        if (s.coverageTarget > 0 && anyBounds && covered / area >= s.coverageTarget) break;
    }
    output.coverage = float(std::min(1.0, covered / area));
    return output;
}

MaskImage RasterizeRockCoverage(const Mesh& surface, const std::vector<RockInstance>& instances,
                                const std::vector<RockScatterSource>& sources, uint32_t resolution, std::string& error,
                                std::stop_token stop) {
    error.clear();
    MaskImage image;
    if (!HasValidUvs(surface)) {
        error = "被覆マスクを作るには、地形に UV が必要です";
        return image;
    }
    image.width = surface.uvWidth > 0 ? surface.uvWidth : std::max<uint32_t>(resolution, 16);
    image.height = surface.uvHeight > 0 ? surface.uvHeight : image.width;
    image.pixels.assign(size_t(image.width) * image.height, 0);
    const SurfaceLocator locator(surface);
    for (size_t index = 0; index < instances.size(); ++index) {
        if ((index & 1023) == 0 && stop.stop_requested()) {
            error = "評価をキャンセルしました";
            return {};
        }
        const auto& instance = instances[index];
        if (instance.rock >= sources.size()) continue;
        std::array<Vec3, 4> corners;
        if (!RockFootprint(instance, sources[instance.rock], corners)) continue;
        // 中心の真下の面で UV を求める。隅はその面の重心座標の外挿で読む（格子の地形は局所的にアフィン）。
        uint32_t face; double w0, w1, w2;
        if (!locator.Locate(instance.position.x, instance.position.z, face, w0, w1, w2)) continue;
        const auto& uv = surface.cornerUvs[face];
        double px[4], py[4];
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) {
            ok = locator.Barycentric(face, corners[i].x, corners[i].z, w0, w1, w2);
            px[i] = (uv[0].u * w0 + uv[1].u * w1 + uv[2].u * w2) * image.width;
            py[i] = (uv[0].v * w0 + uv[1].v * w1 + uv[2].v * w2) * image.height;
        }
        if (!ok) continue;
        // 凸四角形を塗る。向きは符号付き面積で決める。
        double signedArea = 0;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) & 3;
            signedArea += px[i] * py[j] - px[j] * py[i];
        }
        if (std::abs(signedArea) < 1e-9) continue;
        const double sign = signedArea > 0 ? 1 : -1;
        const int x0 = std::max(0, int(std::floor(*std::min_element(px, px + 4))));
        const int x1 = std::min(int(image.width) - 1, int(std::ceil(*std::max_element(px, px + 4))));
        const int y0 = std::max(0, int(std::floor(*std::min_element(py, py + 4))));
        const int y1 = std::min(int(image.height) - 1, int(std::ceil(*std::max_element(py, py + 4))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double cx = x + 0.5, cy = y + 0.5;
                bool inside = true;
                for (int i = 0; i < 4 && inside; ++i) {
                    const int j = (i + 1) & 3;
                    const double edge = (px[j] - px[i]) * (cy - py[i]) - (py[j] - py[i]) * (cx - px[i]);
                    inside = edge * sign >= 0;
                }
                if (inside) image.pixels[size_t(y) * image.width + x] = 255;
            }
    }
    return image;
}
}  // namespace rock::geometry
