#include "geometry/TerrainErode.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace rock::geometry {
namespace {
bool Finite(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

struct Field {
    uint32_t width = 0, height = 0;
    double dx = 1, dz = 1;        // 格子の間隔（m）
    std::vector<double> h;        // 高さ（m）
    double& at(uint32_t x, uint32_t y) { return h[size_t(y) * width + x]; }
    double at(uint32_t x, uint32_t y) const { return h[size_t(y) * width + x]; }
};

Field ToField(const HeightGrid& grid, const HeightmapSettings& t) {
    Field f;
    f.width = grid.width;
    f.height = grid.height;
    f.dx = double(t.width) / std::max<uint32_t>(grid.width - 1, 1);
    f.dz = double(t.depth) / std::max<uint32_t>(grid.height - 1, 1);
    f.h.resize(grid.values.size());
    const double range = double(t.maxHeight) - t.minHeight;
    for (size_t i = 0; i < grid.values.size(); ++i) f.h[i] = t.minHeight + std::clamp(double(grid.values[i]), 0.0, 1.0) * range;
    return f;
}

HeightGrid ToGrid(const Field& f, const HeightmapSettings& t) {
    HeightGrid grid;
    grid.width = f.width;
    grid.height = f.height;
    grid.values.resize(f.h.size());
    const double range = std::max(double(t.maxHeight) - t.minHeight, 1e-6);
    for (size_t i = 0; i < f.h.size(); ++i) grid.values[i] = float(std::clamp((f.h[i] - t.minHeight) / range, 0.0, 1.0));
    return grid;
}

// 格子の上でのガウスぼかし（分離）。sigma は格子の数。
void BlurField(std::vector<double>& values, uint32_t width, uint32_t height, double sigmaX, double sigmaY) {
    const auto pass = [&](double sigma, bool horizontal) {
        if (sigma <= 0.05) return;
        const int reach = int(std::ceil(sigma * 3));
        std::vector<double> kernel(size_t(reach) * 2 + 1);
        double sum = 0;
        for (int k = -reach; k <= reach; ++k) sum += kernel[size_t(k + reach)] = std::exp(-double(k) * k / (2 * sigma * sigma));
        for (double& w : kernel) w /= sum;
        std::vector<double> out(values.size());
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                double acc = 0;
                for (int k = -reach; k <= reach; ++k) {
                    const int sx = horizontal ? std::clamp(int(x) + k, 0, int(width) - 1) : int(x);
                    const int sy = horizontal ? int(y) : std::clamp(int(y) + k, 0, int(height) - 1);
                    acc += kernel[size_t(k + reach)] * values[size_t(sy) * width + sx];
                }
                out[size_t(y) * width + x] = acc;
            }
        values.swap(out);
    };
    pass(sigmaX, true);
    pass(sigmaY, false);
}

// 熱侵食 1 回。安息角より急な隣へ、最も大きな超過の半分 × 割合を、超過に比例して配る（Musgrave の方式）。
void ThermalStep(Field& f, double tanTalus, double rate) {
    std::vector<double> delta(f.h.size(), 0.0);
    const int offsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x) {
            const double here = f.at(x, y);
            double excess[8];
            double total = 0, largest = 0;
            for (int n = 0; n < 8; ++n) {
                const int nx = int(x) + offsets[n][0], ny = int(y) + offsets[n][1];
                excess[n] = 0;
                if (nx < 0 || ny < 0 || nx >= int(f.width) || ny >= int(f.height)) continue;
                const double distance = std::sqrt(double(offsets[n][0] * offsets[n][0]) * f.dx * f.dx +
                                                  double(offsets[n][1] * offsets[n][1]) * f.dz * f.dz);
                const double drop = here - f.at(uint32_t(nx), uint32_t(ny));
                const double threshold = distance * tanTalus;
                if (drop > threshold) {
                    excess[n] = drop - threshold;
                    total += excess[n];
                    largest = std::max(largest, excess[n]);
                }
            }
            if (total <= 0) continue;
            const double move = rate * largest * 0.5;
            delta[size_t(y) * f.width + x] -= move;
            for (int n = 0; n < 8; ++n) {
                if (excess[n] <= 0) continue;
                const int nx = int(x) + offsets[n][0], ny = int(y) + offsets[n][1];
                delta[size_t(ny) * f.width + nx] += move * excess[n] / total;
            }
        }
    for (size_t i = 0; i < f.h.size(); ++i) f.h[i] += delta[i];
}

// D8 の流れの量（上流の格子の数 + 1）。高い順に処理し、最も急な低い隣へ流す。
std::vector<double> FlowAccumulation(const Field& f) {
    std::vector<size_t> order(f.h.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return f.h[a] > f.h[b]; });
    std::vector<double> acc(f.h.size(), 1.0);
    const int offsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const size_t i : order) {
        const uint32_t x = uint32_t(i % f.width), y = uint32_t(i / f.width);
        const double here = f.h[i];
        double best = 0;
        int target = -1;
        for (int n = 0; n < 8; ++n) {
            const int nx = int(x) + offsets[n][0], ny = int(y) + offsets[n][1];
            if (nx < 0 || ny < 0 || nx >= int(f.width) || ny >= int(f.height)) continue;
            const double distance = std::sqrt(double(offsets[n][0] * offsets[n][0]) * f.dx * f.dx +
                                              double(offsets[n][1] * offsets[n][1]) * f.dz * f.dz);
            const double slope = (here - f.at(uint32_t(nx), uint32_t(ny))) / distance;
            if (slope > best) { best = slope; target = int(size_t(ny) * f.width + nx); }
        }
        if (target >= 0) acc[size_t(target)] += acc[i];
    }
    return acc;
}
}  // namespace

bool ValidateTerrainErodeSettings(const TerrainErodeSettings& s, std::string& error) {
    if (!Finite(s.talusAngle, 10, 80)) { error = "安息角は 10〜80 度にしてください"; return false; }
    if (s.thermalIterations < 0 || s.thermalIterations > 500) { error = "崩れの回数は 0〜500 にしてください"; return false; }
    if (!Finite(s.thermalRate, 0, 1)) { error = "崩れの割合は 0〜1 にしてください"; return false; }
    if (!Finite(s.rillDepth, 0, 50) || !Finite(s.rillWidth, 0, 50)) { error = "流路の深さと幅は 0〜50 m にしてください"; return false; }
    if (!Finite(s.rillSharpness, 0.1f, 4)) { error = "流路の集中は 0.1〜4 にしてください"; return false; }
    return true;
}

bool ValidateTerrainDeformSettings(const TerrainDeformSettings& s, std::string& error) {
    if (!Finite(s.amount, -20, 20)) { error = "量は -20〜20 m にしてください"; return false; }
    if (!Finite(s.blur, 0, 50)) { error = "ぼかしは 0〜50 m にしてください"; return false; }
    return true;
}

HeightGrid ErodeTerrain(const HeightGrid& grid, const HeightmapSettings& terrain, const TerrainErodeSettings& s,
                        std::string& error, std::stop_token stop, const std::function<void(int)>& progress) {
    error.clear();
    if (!ValidateTerrainErodeSettings(s, error)) return {};
    if (!ValidateHeightmapSettings(terrain, error)) return {};
    if (grid.width < 2 || grid.height < 2 || grid.values.size() != size_t(grid.width) * grid.height) {
        error = "ハイトマップが空です";
        return {};
    }
    Field f = ToField(grid, terrain);
    const double tanTalus = std::tan(double(s.talusAngle) * 3.14159265358979 / 180.0);
    for (int i = 0; i < s.thermalIterations; ++i) {
        if (stop.stop_requested()) { error = "Terrain Erode をキャンセルしました"; return {}; }
        ThermalStep(f, tanTalus, s.thermalRate);
        if (progress && (i % 8) == 0) progress(int(i * 70 / std::max(s.thermalIterations, 1)));
    }
    if (s.rillDepth > 0) {
        if (stop.stop_requested()) { error = "Terrain Erode をキャンセルしました"; return {}; }
        std::vector<double> acc = FlowAccumulation(f);
        const double maximum = *std::max_element(acc.begin(), acc.end());
        // 流れの量を 0〜1 にし（対数で、細い筋も見えるように）、集中乗で彫る深さにする。
        std::vector<double> carve(acc.size());
        const double logMax = std::log(std::max(maximum, 2.0));
        for (size_t i = 0; i < acc.size(); ++i) {
            const double unit = std::clamp(std::log(acc[i]) / logMax, 0.0, 1.0);
            carve[i] = std::pow(unit, double(s.rillSharpness)) * s.rillDepth;
        }
        if (s.rillWidth > 0) BlurField(carve, f.width, f.height, s.rillWidth / f.dx * 0.5, s.rillWidth / f.dz * 0.5);
        for (size_t i = 0; i < f.h.size(); ++i) f.h[i] -= carve[i];
    }
    if (progress) progress(100);
    return ToGrid(f, terrain);
}

HeightGrid DeformTerrain(const HeightGrid& grid, const HeightmapSettings& terrain, const MaskImage& mask, bool invertMask,
                         const TerrainDeformSettings& s, std::string& error) {
    error.clear();
    if (!ValidateTerrainDeformSettings(s, error)) return {};
    if (!ValidateHeightmapSettings(terrain, error)) return {};
    if (grid.width < 2 || grid.height < 2 || grid.values.size() != size_t(grid.width) * grid.height) {
        error = "ハイトマップが空です";
        return {};
    }
    if (mask.width == 0 || mask.height == 0 || mask.pixels.size() != size_t(mask.width) * mask.height) {
        error = "マスクの画像がありません";
        return {};
    }
    Field f = ToField(grid, terrain);
    // 格子の点ごとにマスクを読む（地形の UV は u = 列 / (幅 − 1)、v = 行 / (高さ − 1)）。
    std::vector<double> weight(f.h.size());
    for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x) {
            float value = mask.Sample(float(x) / float(f.width - 1), float(y) / float(f.height - 1));
            if (invertMask) value = 1 - value;
            weight[size_t(y) * f.width + x] = value;
        }
    if (s.blur > 0) BlurField(weight, f.width, f.height, s.blur / f.dx * 0.5, s.blur / f.dz * 0.5);
    for (size_t i = 0; i < f.h.size(); ++i) f.h[i] += s.amount * weight[i];
    return ToGrid(f, terrain);
}
}  // namespace rock::geometry
