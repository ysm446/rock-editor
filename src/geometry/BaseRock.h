#pragma once
#include <string>
#include <string_view>
#include "geometry/Mesh.h"

namespace rock::geometry {
enum class BaseShape { Box, RoundedBox, Sphere, Ellipsoid, ConvexPeak, Invalid };
const char* BaseShapeName(BaseShape shape);
BaseShape ParseBaseShape(std::string_view name);
struct BaseRockSettings {
    std::array<float, 3> size{2, 2, 2};
    int seed = 0;
    BaseShape shape = BaseShape::Box;
    int subdivisions = 8;     // 各面の分割数。4/8/16/32。
    float roundness = 0.25f;  // 最短辺の半分に対する丸み半径。
    float noiseStrength = 0;  // 原点からの距離に対する最大変位率。
    float noiseScale = 2;     // 正規化した方向上のノイズ周波数。
    // 凸岩峰。裾・肩・頂の点群の凸包を取り、外接箱をsizeに合わせる。
    int peakSides = 7;
    float peakTopWidth = .08f, peakShoulderHeight = .55f;
    float peakLeanX = .18f, peakLeanZ = -.08f, peakVariation = .25f;
    // 頂を一点ではなく稜線にする（裾の幅に対する比。0 で従来どおりの点）。向きは走向と同じ。
    float peakRidgeLength = 0;
    // 走向（Y 軸まわりの度。Parallel Planes の回転 Y と同じ向き）。稜線と節理面が共有する。
    float peakStrike = 0;
    // 節理面で両側を切り、同じ向きの板にする。間隔は寸法 X に対する比。0 で切らない。
    // 傾きは水平からの度（Parallel Planes の回転 Z と同じ）。切ると外接箱は size より小さくなる。
    float peakSlabThickness = 0, peakSlabDip = 76;
};
// Box / Noise 0 は従来の8頂点・12三角形をそのまま返す。
// 曲面は共有頂点を持つ cube surface の投影。描画用の属性は含まない。
Mesh MakeBaseRock(const BaseRockSettings& settings, std::string& error);
}  // namespace rock::geometry
