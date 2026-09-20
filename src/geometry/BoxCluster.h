#pragma once
#include "geometry/Mesh.h"
#include <string>

namespace rock::geometry {
struct BoxClusterSettings {
    int count = 8;
    std::array<float, 3> size{2, 2.4f, 1.8f};
    float sizeVariation = .55f;
    float spread = .85f;
    float rotation = 25;
    int seed = 42;
};
struct OrientedBox {
    Vec3 center;
    std::array<Vec3, 3> axes;
    std::array<float, 3> halfSize;
    bool operator==(const OrientedBox&) const = default;
};
// 全ての追加 Box の中心を最初の Box 内に置き、正の体積で重なる塊を作る。
std::vector<OrientedBox> MakeBoxCluster(const BoxClusterSettings& settings, std::string& error);
Mesh BoxClusterPreview(const std::vector<OrientedBox>& boxes);
// 負が内部、正が外部。和集合の符号は正確。重複領域の内部距離は近似。
float BoxUnionField(Vec3 point, const std::vector<OrientedBox>& boxes);
}  // namespace rock::geometry
