#include "geometry/ShapeMask.h"
#include "geometry/UvUnwrap.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <execution>
#include <numeric>
#include <unordered_map>

namespace rock::geometry {
namespace {
struct P {
    double x = 0, y = 0, z = 0;
    P operator+(P b) const { return {x + b.x, y + b.y, z + b.z}; }
    P operator-(P b) const { return {x - b.x, y - b.y, z - b.z}; }
    P operator*(double s) const { return {x * s, y * s, z * s}; }
};
P Convert(Vec3 v) { return {v.x, v.y, v.z}; }
double Dot(P a, P b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
P Cross(P a, P b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// 向きの違う面どうしの重み。裏合わせ（薄い板の表と裏）は混ぜず、角をまたぐ所（直角で 1/3）は弱めに混ぜる。
double FacingWeight(P a, P b) {
    return std::clamp((Dot(a, b) + .5) / 1.5, 0., 1.);
}

bool ValidLevels(const MaskFilterSettings& s) {
    const auto unit = [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; };
    return unit(s.inputLow) && unit(s.inputHigh) && s.inputHigh - s.inputLow >= .001f && std::isfinite(s.gamma) &&
           s.gamma >= .1f && s.gamma <= 10 && unit(s.outputLow) && unit(s.outputHigh);
}

float ApplyLevels(float v, const MaskFilterSettings& s) {
    double level = std::clamp((v - s.inputLow) / double(s.inputHigh - s.inputLow), 0., 1.);
    if (s.gamma != 1) level = std::pow(level, double(s.gamma));
    return float(s.outputLow + (s.outputHigh - s.outputLow) * level);
}
}  // namespace

MaskImage FilterMask(const Mesh& mesh, const MaskImage& input, bool invertInput, const MaskFilterSettings& settings,
                     std::string& error, std::stop_token stop, const std::function<void(int)>& progress) {
    error.clear();
    if (input.width == 0 || input.height == 0 || input.pixels.size() != size_t(input.width) * input.height) {
        error = "加工するマスクの画像がありません";
        return {};
    }
    if (static_cast<uint32_t>(settings.type) > static_cast<uint32_t>(MaskFilterType::Levels) ||
        !std::isfinite(settings.radius) || settings.radius < kMinMaskFilterRadius || settings.radius > kMaxMaskFilterRadius ||
        !std::isfinite(settings.amount) || settings.amount < 0 || settings.amount > 4 || !ValidLevels(settings)) {
        error = "Mask Filterの設定が不正です";
        return {};
    }
    const size_t width = input.width, height = input.height, count = width * height;
    std::vector<float> values(count);
    for (size_t i = 0; i < count; ++i) {
        const float v = input.pixels[i] / 255.f;
        values[i] = invertInput ? 1 - v : v;
    }
    MaskImage out;
    out.width = input.width;
    out.height = input.height;
    out.pixels.resize(count);
    const auto store = [&](size_t i, float v) {
        v = std::clamp(v, 0.f, 1.f);
        if (settings.invert) v = 1 - v;
        out.pixels[i] = uint8_t(std::lround(255 * v));
    };

    // レベルは画素ごとに独立。島の外の画素もそのまま写す（埋めてある値に同じ写像を掛けるだけ）。
    if (settings.type == MaskFilterType::Levels) {
        for (size_t i = 0; i < count; ++i) store(i, ApplyLevels(values[i], settings));
        if (progress) progress(100);
        return out;
    }

    MeshInfo info;
    if (!HasValidUvs(mesh) || !InspectMesh(mesh, info)) {
        error = "UV付きのMeshから作ったマスクを接続してください";
        return {};
    }

    // --- 画素ごとの表面の点 -------------------------------------------------------
    // Shape Mask と同じく画素の中心が当たる面を求める。辺を共有する面が同じ画素へ書くので直列にして再現させる。
    constexpr uint32_t kNoFace = UINT32_MAX;
    std::vector<uint32_t> faces(count, kNoFace);
    std::vector<P> positions(count);
    std::vector<P> faceNormals(mesh.triangles.size());
    double surfaceArea = 0;
    const auto cross2 = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        if (stop.stop_requested()) { error = "Mask Filterをキャンセルしました"; return {}; }
        const auto tri = mesh.triangles[f];
        const P a = Convert(mesh.positions[tri[0]]), b = Convert(mesh.positions[tri[1]]), c = Convert(mesh.positions[tri[2]]);
        const P n = Cross(b - a, c - a);
        const double twiceArea = std::sqrt(Dot(n, n));
        surfaceArea += twiceArea * .5;
        faceNormals[f] = twiceArea > 1e-20 ? n * (1 / twiceArea) : P{0, 1, 0};
        const auto uv = mesh.cornerUvs[f];
        const double area = cross2(uv[1].u - uv[0].u, uv[1].v - uv[0].v, uv[2].u - uv[0].u, uv[2].v - uv[0].v);
        if (std::abs(area) < 1e-16) continue;
        const int x0 = std::max(0, int(std::floor(std::min({uv[0].u, uv[1].u, uv[2].u}) * width))),
                  x1 = std::min(int(width) - 1, int(std::ceil(std::max({uv[0].u, uv[1].u, uv[2].u}) * width)));
        const int y0 = std::max(0, int(std::floor(std::min({uv[0].v, uv[1].v, uv[2].v}) * height))),
                  y1 = std::min(int(height) - 1, int(std::ceil(std::max({uv[0].v, uv[1].v, uv[2].v}) * height)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double u = (x + .5) / width - uv[0].u, v = (y + .5) / height - uv[0].v;
                const double wb = cross2(u, v, uv[2].u - uv[0].u, uv[2].v - uv[0].v) / area,
                             wc = cross2(uv[1].u - uv[0].u, uv[1].v - uv[0].v, u, v) / area;
                if (wb < 0 || wc < 0 || wb + wc > 1) continue;
                const size_t i = size_t(y) * width + x;
                faces[i] = uint32_t(f);
                positions[i] = a + (b - a) * wb + (c - a) * wc;
            }
    }
    std::vector<size_t> covered;
    for (size_t i = 0; i < count; ++i)
        if (faces[i] != kNoFace) covered.push_back(i);
    if (covered.empty()) {
        error = "マスクのUVに面がありません";
        return {};
    }
    if (progress) progress(10);

    // --- 箱に集めてぼかす -----------------------------------------------------------
    // 画素ごとに半径内の画素を全部探すと、半径が大きいとき重すぎる。空間を箱に区切って画素を集め、
    // 箱の単位でガウスぼかしを掛けてから画素へ戻す。箱は半径の 1/4 だが、画素の大きさより細かくはしない
    // （細かくしても箱の数が画素の数を超えるだけで、ぼけ方は変わらない）。
    const double texelSize = std::sqrt(surfaceArea / double(covered.size()));
    const double radius = settings.radius, sigma = radius * .5;
    const double cell = std::max(radius * .25, texelSize);
    const P origin = Convert(info.minimum) - P{cell, cell, cell};
    const auto cellCoord = [&](P p, int axis) {
        const double v = axis == 0 ? p.x - origin.x : axis == 1 ? p.y - origin.y : p.z - origin.z;
        return int64_t(std::floor(v / cell));
    };
    constexpr int64_t kCoordMask = (int64_t(1) << 21) - 1;
    const auto key = [&](int64_t x, int64_t y, int64_t z) {
        return uint64_t(x & kCoordMask) | (uint64_t(y & kCoordMask) << 21) | (uint64_t(z & kCoordMask) << 42);
    };
    struct Cell {
        P position, normal;
        double value = 0, weight = 0;  // 画素の数を重みにする（UV Unwrap は面積あたりの画素がほぼ一定）
        int64_t x = 0, y = 0, z = 0;
        double blurred = 0;
    };
    std::vector<Cell> cells;
    std::unordered_map<uint64_t, uint32_t> lookup;
    lookup.reserve(covered.size() / 2 + 16);
    for (const size_t i : covered) {
        const P p = positions[i];
        const int64_t x = cellCoord(p, 0), y = cellCoord(p, 1), z = cellCoord(p, 2);
        auto [it, added] = lookup.emplace(key(x, y, z), uint32_t(cells.size()));
        if (added) cells.push_back(Cell{{}, {}, 0, 0, x, y, z, 0});
        Cell& target = cells[it->second];
        target.position = target.position + p;
        target.normal = target.normal + faceNormals[faces[i]];
        target.value += values[i];
        target.weight += 1;
    }
    for (Cell& c : cells) {
        c.position = c.position * (1 / c.weight);
        const double length = std::sqrt(Dot(c.normal, c.normal));
        c.normal = length > 1e-12 ? c.normal * (1 / length) : P{0, 1, 0};
        c.value /= c.weight;
    }
    const int reach = int(std::ceil(radius / cell));
    const double inverseTwoSigma2 = 1 / (2 * sigma * sigma);
    std::vector<size_t> order(cells.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::atomic<bool> cancelled{false};
    std::atomic<size_t> done{0};
    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t index) {
        if (cancelled.load(std::memory_order_relaxed)) return;
        if (stop.stop_requested()) { cancelled = true; return; }
        Cell& c = cells[index];
        double sum = 0, total = 0;
        for (int dz = -reach; dz <= reach; ++dz)
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx) {
                    // 箱の中心どうしが半径より明らかに遠いものは調べない。
                    if ((dx * dx + dy * dy + dz * dz) > (reach + 1) * (reach + 1)) continue;
                    const auto found = lookup.find(key(c.x + dx, c.y + dy, c.z + dz));
                    if (found == lookup.end()) continue;
                    const Cell& other = cells[found->second];
                    const P d = other.position - c.position;
                    const double d2 = Dot(d, d);
                    if (d2 > radius * radius) continue;
                    const double w = other.weight * std::exp(-d2 * inverseTwoSigma2) * FacingWeight(c.normal, other.normal);
                    sum += w * other.value;
                    total += w;
                }
        c.blurred = total > 0 ? sum / total : c.value;
        if (progress && (++done % 4096) == 0) progress(10 + int(done * 60 / cells.size()));
    });
    if (cancelled || stop.stop_requested()) { error = "Mask Filterをキャンセルしました"; return {}; }

    // --- 画素へ戻す -----------------------------------------------------------------
    // 近くの箱（前後左右上下 1 つ）をガウスで補間する。箱の境目で段にならない。
    // **補間もぼかしの一種なので、幅は箱の半分とぼかしの σ の小さいほうにする。** 箱の大きさのままだと、
    // 半径が画素ほどに小さいとき（箱＝画素の大きさ）に、指定より大きくにじむ。
    const double interpolation = std::min(cell * .5, sigma);
    const double inverseTwoCell2 = 1 / (2 * interpolation * interpolation);
    std::vector<float> blurred(count, 0.f);
    std::for_each(std::execution::par, covered.begin(), covered.end(), [&](size_t i) {
        const P p = positions[i];
        const P n = faceNormals[faces[i]];
        const int64_t x = cellCoord(p, 0), y = cellCoord(p, 1), z = cellCoord(p, 2);
        double sum = 0, total = 0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto found = lookup.find(key(x + dx, y + dy, z + dz));
                    if (found == lookup.end()) continue;
                    const Cell& c = cells[found->second];
                    const P d = c.position - p;
                    const double w = c.weight * std::exp(-Dot(d, d) * inverseTwoCell2) * FacingWeight(n, c.normal);
                    sum += w * c.blurred;
                    total += w;
                }
        blurred[i] = total > 0 ? float(sum / total) : values[i];
    });
    if (stop.stop_requested()) { error = "Mask Filterをキャンセルしました"; return {}; }
    if (progress) progress(80);

    std::vector<uint8_t> filled(count, 0);
    std::vector<size_t> frontier, next;
    for (const size_t i : covered) {
        const float v = settings.type == MaskFilterType::Sharpen
                            ? values[i] + settings.amount * (values[i] - blurred[i])
                            : blurred[i];
        store(i, v);
        filled[i] = 1;
        frontier.push_back(i);
    }
    // 島の無い画素を、縦横の歩数で最も近い島の値で埋める（Shape Mask と同じ。縮小表示や線形補間で縁がにじまない）。
    while (!frontier.empty()) {
        if (stop.stop_requested()) { error = "Mask Filterをキャンセルしました"; return {}; }
        next.clear();
        for (const size_t i : frontier) {
            const size_t x = i % width, y = i / width;
            const auto visit = [&](bool valid, size_t j) {
                if (!valid || filled[j]) return;
                filled[j] = 1;
                out.pixels[j] = out.pixels[i];
                next.push_back(j);
            };
            visit(x > 0, i - 1);
            visit(x + 1 < width, i + 1);
            visit(y > 0, i - width);
            visit(y + 1 < height, i + width);
        }
        frontier.swap(next);
    }
    if (progress) progress(100);
    return out;
}
}  // namespace rock::geometry
