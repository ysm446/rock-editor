#pragma once
#include <string>
#include <string_view>
#include "geometry/Mesh.h"

namespace rock::geometry {
enum class BaseShape { Box, RoundedBox, Sphere, Ellipsoid, Invalid };
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
};
// Box / Noise 0 は従来の8頂点・12三角形をそのまま返す。
// 曲面は共有頂点を持つ cube surface の投影。描画用の属性は含まない。
Mesh MakeBaseRock(const BaseRockSettings& settings, std::string& error);
}  // namespace rock::geometry
