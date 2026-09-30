#pragma once
#include "geometry/Mesh.h"
#include "geometry/ShapeMask.h"

#include <stop_token>
#include <string>
#include <vector>

namespace rock::geometry {
// 山グラフの Rock Scatter。地形（Mesh）の表面に、岩のインスタンスを間隔を空けて撒く。
//
// 面積に比例して表面へ候補の点を落とし、すでに置いた点から「間隔」より近い候補を捨てる（ダーツ投げの
// ポアソンディスク）。マスク（地形の UV の画像）があれば、その値を置く確率にする。どの岩を置くかは重みで選ぶ。
// 結果は決定的（同じ入力と Seed なら同じ配置）。
inline constexpr float kMinScatterSpacing = 0.05f;
inline constexpr int kMaxScatterCount = 50000;

struct RockScatterSettings {
    int seed = 1;
    // 岩どうしの最小の間隔（m）。中心どうしの 3D の距離。
    float spacing = 6.0f;
    // 置く数の上限。
    int maxCount = 5000;
    // 倍率の範囲（Rock ノードの倍率に掛ける）。
    float scaleMin = 0.8f, scaleMax = 1.3f;
    // 地形の法線へ傾ける強さ。0 で真上、1 で面に垂直。
    float alignToNormal = 0.5f;
    // 地形へ沈める量。岩の高さに対する比（0〜0.9）。接地の継ぎ目を隠す。
    float embed = 0.15f;
    bool operator==(const RockScatterSettings&) const = default;
};

struct RockInstance {
    Vec3 position{};  // 地形の上の点（m）
    Vec3 up{0, 1, 0}; // 岩の上向き（単位）
    float yaw = 0;    // 上向きのまわりの回転（ラジアン）
    float scale = 1;  // 倍率（Rock ノードの倍率は含まない）
    uint32_t rock = 0; // 重みの並び（入力の Rock）の番号
    float embed = 0;  // 沈める量（岩の高さに対する比）
};

// surface: 地形。mask: 地形の UV の画像（無ければ nullptr）。weights: 岩ごとの重み（1 つ以上）。
std::vector<RockInstance> ScatterRocks(const Mesh& surface, const MaskImage* mask, bool invertMask,
                                       const std::vector<float>& weights, const RockScatterSettings& settings,
                                       std::string& error, std::stop_token stop = {});
bool ValidateRockScatterSettings(const RockScatterSettings& settings, std::string& error);
}  // namespace rock::geometry
