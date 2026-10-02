#pragma once
#include "geometry/Mesh.h"
#include "geometry/ShapeMask.h"

#include <array>
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
    // --- M3: 大きさを考えた間隔と浮きの補正 ---
    // 大きさの間隔（倍）。2 つの岩の足元の半径（底の投影を包む円。倍率込み）の和にこの倍率を掛けた距離より
    // 近ければ置かない。「間隔」との大きい方で判定する。0 で無効（間隔だけで判定）。岩アセットが未焼成なら効かない。
    float sizeSpacing = 1.0f;
    // 浮きの補正（0〜1）。底の 4 隅で地形の高さを読み、どの隅も浮かない深さまで追加で沈める強さ。
    float settle = 1.0f;
    // 浮きの補正の上限。岩の高さに対する比（0〜0.9）。急斜面で沈み過ぎないようにする。
    float settleMax = 0.5f;
    // 目標の被覆率（0〜1）。置いた岩の足元の面積の合計が地形の面積のこの割合に達したら止める。0 で無効。
    float coverageTarget = 0.0f;
    // --- M3 の 4: 向き ---
    // 上向きのまわりの向き（度）。yaw を中心に ±yawVariation の範囲で乱数。yawVariation 180 で従来どおり全周の乱数。
    // 層の向き（露岩の走向）を全体でそろえるときに yawVariation を小さくする。
    float yaw = 0.0f;
    float yawVariation = 180.0f;
    // 沈める量のばらつき（0〜1）。岩ごとに沈める量を embed × (1 ± ばらつき) の範囲で変える（0〜0.9 に収める）。
    // 土に埋まった破片と転がった破片が混ざる崖錐のため。乱数は向きの乱数から派生させ、配置は変えない。
    float embedVariation = 0.0f;
    bool operator==(const RockScatterSettings&) const = default;
};

// 撒く岩 1 種類（Rock ノード 1 つ）。
struct RockScatterSource {
    float weight = 1.0f;     // 選ばれる相対的な重み
    float scale = 1.0f;      // Rock ノードの倍率
    // 焼いた岩アセット（LOD0）の範囲（倍率 1、m）。hasBounds が false なら未焼成で、大きさを使う処理は効かない。
    bool hasBounds = false;
    Vec3 minimum{}, maximum{};
};

struct RockInstance {
    Vec3 position{};  // 地形の上の点（m）
    Vec3 up{0, 1, 0}; // 岩の上向き（単位）
    float yaw = 0;    // 上向きのまわりの回転（ラジアン）
    float scale = 1;  // 倍率（Rock ノードの倍率は含まない）
    uint32_t rock = 0; // 重みの並び（入力の Rock）の番号
    float embed = 0;  // 沈める量（岩の高さに対する比）
};

struct RockScatterResult {
    std::vector<RockInstance> instances;
    // 置いた岩の足元の面積の合計 ÷ 地形の面積（推定の被覆率）。足元は底の投影の長方形で近似する。
    float coverage = 0.0f;
};

// surface: 地形。mask: 地形の UV の画像（無ければ nullptr）。sources: 岩ごとの重みと大きさ（1 つ以上）。
RockScatterResult ScatterRocks(const Mesh& surface, const MaskImage* mask, bool invertMask,
                               const std::vector<RockScatterSource>& sources, const RockScatterSettings& settings,
                               std::string& error, std::stop_token stop = {});
bool ValidateRockScatterSettings(const RockScatterSettings& settings, std::string& error);

// 岩の足元（倍率・向き・沈めた量を含む、底の長方形の 4 隅）。ワールド座標。岩に範囲が無ければ false。
bool RockFootprint(const RockInstance& instance, const RockScatterSource& source, std::array<Vec3, 4>& corners);
// 被覆マスク。置いた岩の足元（底の投影）を地形の UV の画像に白で描く。resolution は一辺（地形の uvWidth が 0 のとき用）。
MaskImage RasterizeRockCoverage(const Mesh& surface, const std::vector<RockInstance>& instances,
                                const std::vector<RockScatterSource>& sources, uint32_t resolution, std::string& error,
                                std::stop_token stop = {});
}  // namespace rock::geometry
