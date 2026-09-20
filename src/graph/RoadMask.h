#pragma once

#include "graph/NodeGraph.h"

#include <cstdint>
#include <vector>

// 道路空間マスク。横位置（m。正が Left 側）と実距離（m）から 0〜1 を返す純粋な関数と、
// それを道路 1 本ぶんの低解像度画像（横 256 px、長さ 1 m あたり 16 px）へ焼く処理。
// 設計は docs/design/road-material-layers.md。
namespace tg::graph {

struct RoadLanes;

struct RoadGeometry;

// 1 点の評価。halfWidth は道路幅の半分、length は道路の全長（m）。
// lanes を渡すと轍を各車線の中央に置ける（tracksFromLanes のとき）。nullptr なら手入力の車線中央。
// worldX / worldZ はその点のワールド座標（ワールドノイズ用）。無ければ道路座標で代用する。
float EvaluateRoadMask(const RoadMaskNodeSettings& settings, float lateralMeters, float distanceMeters,
                       float halfWidthMeters, float lengthMeters, const RoadLanes* lanes = nullptr,
                       float worldX = 0.0f, float worldZ = 0.0f, bool hasWorld = false);

// 道路 1 本ぶんのマスク画像（RGBA8）。R / G / B がスロット 2〜4、A は予約（255）。
struct RoadMaskImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    bool IsValid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4; }
};
// channels[i] が nullptr のスロットは 0。列 0 が Right 端（u = 0）、行 0 が始点。
// geometry を渡すとテクセルごとのワールド座標を行の左右端から補間する（ワールドノイズ用）。
RoadMaskImage BakeRoadMask(const RoadMaskNodeSettings* const channels[3], float widthMeters,
                           float lengthMeters, const RoadLanes* lanes = nullptr,
                           const RoadGeometry* geometry = nullptr);

}  // namespace tg::graph
