#pragma once
#include "geometry/Mesh.h"
#include <string>
#include <vector>

namespace rock::geometry {
// 山グラフの地形。ハイトマップ（画像かノイズ）から、UV 付きの格子のメッシュを作る。
//
// 地形は UV 付きの Mesh として下流へ渡す。UV はハイトマップと同じ向き（u が +X、v が +Z）の 1 枚の島なので、
// Shape Mask（上向き度 = 傾斜、高さ、曲率）・Apply Material・Material Bake がそのまま使える。
// 格子は閉じていない（縁で切れた面）。座標は m、右手系 Y-up。中心が原点、底（最低の高さ）が minHeight。
enum class HeightmapSource { Noise, Image };
const char* HeightmapSourceName(HeightmapSource source);
HeightmapSource ParseHeightmapSource(const std::string& name);

inline constexpr int kMinTerrainResolution = 16;
inline constexpr int kMaxTerrainResolution = 1024;
inline constexpr float kMinTerrainSize = 1.0f;
inline constexpr float kMaxTerrainSize = 10000.0f;

struct HeightmapSettings {
    HeightmapSource source = HeightmapSource::Noise;
    // 画像のパス（UTF-8。メモリ上は絶対パス、保存するときはシーンからの相対パス）。16bit / 8bit PNG と EXR。
    std::string image;
    // 実寸（m）。幅が X、奥行きが Z。
    float width = 200.0f, depth = 200.0f;
    // ハイトマップの 0 と 1 に当てる高さ（m）。
    float minHeight = 0.0f, maxHeight = 80.0f;
    // 格子の一辺の分割数。三角形数は 2 × resolution²。
    int resolution = 256;
    // UV のアトラス寸法（Shape Mask や Material Bake が作る画像の大きさの基準）。
    int textureResolution = 1024;
    // --- ノイズ ---
    int seed = 1;
    // いちばん大きな起伏の大きさ（m）。
    float featureSize = 60.0f;
    // 細かい起伏の強さ（0〜1）。大きいほど荒れる。
    float roughness = 0.5f;
    // 山の形（0〜1）。1 で中央が高く縁が低い 1 つの山、0 で一様な起伏。
    float peak = 0.8f;
    bool operator==(const HeightmapSettings&) const = default;
};

// 0〜1 の高さの格子。行 0 が奥（-Z）、列 0 が左（-X）。
struct HeightGrid {
    uint32_t width = 0, height = 0;
    std::vector<float> values;
    // (u, v) は 0〜1。双線形で読む。
    float Sample(float u, float v) const;
};

// ノイズの山。size は格子の一辺の点の数。
HeightGrid MakeNoiseHeights(const HeightmapSettings& settings, uint32_t size);
// 画像の高さ（0〜1 に並べ直したもの）から地形を作る。settings.source は見ない。
Mesh MakeTerrainMesh(const HeightGrid& heights, const HeightmapSettings& settings, std::string& error);
// 設定を検査する（範囲外なら理由を返す）。
bool ValidateHeightmapSettings(const HeightmapSettings& settings, std::string& error);
}  // namespace rock::geometry
