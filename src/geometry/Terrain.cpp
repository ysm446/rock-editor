#include "geometry/Terrain.h"

#include <algorithm>
#include <cmath>

namespace rock::geometry {
namespace {
uint32_t Hash(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}
// 格子点の値を五次曲線で補間する 2D のノイズ（0〜1）。
double ValueNoise(double x, double y, uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const auto lattice = [](double v) { return uint32_t(int64_t(v)); };
    const uint32_t ix = lattice(fx), iy = lattice(fy);
    const auto at = [&](uint32_t a, uint32_t b) {
        return double(Hash(seed ^ Hash(a) ^ Hash(b + 0x9e3779b9u))) / double(UINT32_MAX);
    };
    const auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    const double tx = fade(x - fx), ty = fade(y - fy);
    return std::lerp(std::lerp(at(ix, iy), at(ix + 1, iy), tx), std::lerp(at(ix, iy + 1), at(ix + 1, iy + 1), tx), ty);
}
}  // namespace

const char* HeightmapSourceName(HeightmapSource source) {
    return source == HeightmapSource::Image ? "image" : "noise";
}

HeightmapSource ParseHeightmapSource(const std::string& name) {
    return name == "image" ? HeightmapSource::Image : HeightmapSource::Noise;
}

float HeightGrid::Sample(float u, float v) const {
    if (width == 0 || height == 0 || values.size() != size_t(width) * height) return 0.0f;
    const float x = std::clamp(u, 0.0f, 1.0f) * float(width - 1), y = std::clamp(v, 0.0f, 1.0f) * float(height - 1);
    const uint32_t x0 = std::min(uint32_t(x), width - 1), y0 = std::min(uint32_t(y), height - 1);
    const uint32_t x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float tx = x - float(x0), ty = y - float(y0);
    const auto at = [&](uint32_t a, uint32_t b) { return values[size_t(b) * width + a]; };
    return std::lerp(std::lerp(at(x0, y0), at(x1, y0), tx), std::lerp(at(x0, y1), at(x1, y1), tx), ty);
}

bool ValidateHeightmapSettings(const HeightmapSettings& s, std::string& error) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (!finite(s.width) || !finite(s.depth) || s.width < kMinTerrainSize || s.width > kMaxTerrainSize ||
        s.depth < kMinTerrainSize || s.depth > kMaxTerrainSize) {
        error = "幅と奥行きは 1〜10000 m にしてください";
        return false;
    }
    if (!finite(s.minHeight) || !finite(s.maxHeight) || s.maxHeight - s.minHeight < 0.001f ||
        std::abs(s.minHeight) > kMaxTerrainSize || std::abs(s.maxHeight) > kMaxTerrainSize) {
        error = "高さの範囲が不正です（最高は最低より高くしてください）";
        return false;
    }
    if (s.resolution < kMinTerrainResolution || s.resolution > kMaxTerrainResolution) {
        error = "格子の細かさは 16〜1024 にしてください";
        return false;
    }
    if (s.textureResolution < 128 || s.textureResolution > 4096 || (s.textureResolution & (s.textureResolution - 1)) != 0) {
        error = "テクスチャ解像度は 128〜4096 の 2 のべき乗にしてください";
        return false;
    }
    if (!finite(s.featureSize) || s.featureSize < 0.1f || !finite(s.roughness) || s.roughness < 0 || s.roughness > 1 ||
        !finite(s.peak) || s.peak < 0 || s.peak > 1) {
        error = "ノイズの設定が不正です";
        return false;
    }
    return true;
}

// 起伏（fBm）と山の形（中央が高いドーム）を混ぜる。peak が 1 なら起伏はドームの上にだけ乗る。
HeightGrid MakeNoiseHeights(const HeightmapSettings& s, uint32_t size) {
    HeightGrid grid;
    grid.width = grid.height = std::max(size, 2u);
    grid.values.resize(size_t(grid.width) * grid.height);
    const double gain = 0.25 + 0.5 * std::clamp(double(s.roughness), 0.0, 1.0);
    const uint32_t seed = Hash(uint32_t(s.seed) * 0x9e3779b9u + 1);
    float low = 1e30f, high = -1e30f;
    for (uint32_t row = 0; row < grid.height; ++row)
        for (uint32_t column = 0; column < grid.width; ++column) {
            const double u = double(column) / (grid.width - 1), v = double(row) / (grid.height - 1);
            // 実寸（m）で起伏の大きさを決める。
            const double x = (u - 0.5) * s.width / s.featureSize, y = (v - 0.5) * s.depth / s.featureSize;
            double value = 0, weight = 1, total = 0, frequency = 1;
            for (int octave = 0; octave < 6; ++octave) {
                value += weight * ValueNoise(x * frequency + 17.3 * octave, y * frequency - 9.1 * octave, seed + octave);
                total += weight;
                weight *= gain;
                frequency *= 2.03;
            }
            const double relief = value / total;
            // 中央 1、縁 0 のドーム（楕円の半径で測り、なめらかに落とす）。
            const double r = std::sqrt((u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5)) * 2.0;
            const double t = std::clamp(1.0 - r, 0.0, 1.0);
            const double dome = t * t * (3 - 2 * t);
            const double mountain = dome * (0.55 + 0.45 * relief);
            const double h = std::lerp(relief, mountain, std::clamp(double(s.peak), 0.0, 1.0));
            grid.values[size_t(row) * grid.width + column] = float(h);
            low = std::min(low, float(h));
            high = std::max(high, float(h));
        }
    // 0〜1 へ伸ばす（最低が minHeight、最高が maxHeight になる）。
    if (high - low > 1e-6f)
        for (float& value : grid.values) value = (value - low) / (high - low);
    return grid;
}

Mesh MakeTerrainMesh(const HeightGrid& heights, const HeightmapSettings& s, std::string& error) {
    error.clear();
    if (!ValidateHeightmapSettings(s, error)) return {};
    if (heights.width < 2 || heights.height < 2 || heights.values.size() != size_t(heights.width) * heights.height) {
        error = "ハイトマップが空です";
        return {};
    }
    const uint32_t n = uint32_t(s.resolution);
    Mesh mesh;
    mesh.positions.reserve(size_t(n + 1) * (n + 1));
    for (uint32_t row = 0; row <= n; ++row)
        for (uint32_t column = 0; column <= n; ++column) {
            const float u = float(column) / n, v = float(row) / n;
            float h = heights.Sample(u, v);
            if (!std::isfinite(h)) h = 0.0f;
            mesh.positions.push_back({(u - 0.5f) * s.width, s.minHeight + std::clamp(h, 0.0f, 1.0f) * (s.maxHeight - s.minHeight),
                                      (v - 0.5f) * s.depth});
        }
    mesh.triangles.reserve(size_t(n) * n * 2);
    mesh.cornerUvs.reserve(size_t(n) * n * 2);
    const auto index = [&](uint32_t column, uint32_t row) { return row * (n + 1) + column; };
    const auto uv = [&](uint32_t column, uint32_t row) { return Mesh::Uv{float(column) / n, float(row) / n}; };
    for (uint32_t row = 0; row < n; ++row)
        for (uint32_t column = 0; column < n; ++column) {
            // 上（+Y）を向く巻き方: (x, z) → (x, z + 1) → (x + 1, z)。
            mesh.triangles.push_back({index(column, row), index(column, row + 1), index(column + 1, row)});
            mesh.cornerUvs.push_back({uv(column, row), uv(column, row + 1), uv(column + 1, row)});
            mesh.triangles.push_back({index(column + 1, row), index(column, row + 1), index(column + 1, row + 1)});
            mesh.cornerUvs.push_back({uv(column + 1, row), uv(column, row + 1), uv(column + 1, row + 1)});
        }
    mesh.uvCharts.assign(mesh.triangles.size(), 0);
    mesh.uvWidth = mesh.uvHeight = uint32_t(s.textureResolution);
    return mesh;
}
}  // namespace rock::geometry
