#pragma once
#include "geometry/BoxCluster.h"

namespace rock::geometry {
struct VolumeSettings {
    int resolution = 48;  // 母岩の最長辺のセル数。16～96。
};
struct VolumeGrid {
    Vec3 origin;
    float spacing = 0;
    std::array<uint32_t, 3> dimensions{};  // サンプル点数。外側に余白を持つ。
    std::vector<float> values;             // X が最速。負が内部。
    size_t Index(uint32_t x, uint32_t y, uint32_t z) const {
        return (size_t(z) * dimensions[1] + y) * dimensions[0] + x;
    }
    Vec3 Position(uint32_t x, uint32_t y, uint32_t z) const {
        return {origin.x + x * spacing, origin.y + y * spacing, origin.z + z * spacing};
    }
};
VolumeGrid BoxesToVolume(const std::vector<OrientedBox>& boxes, const VolumeSettings& settings,
                         std::string& error);
// 表示用の等値面。グリッドを残し、内部に重複面のない外皮を抽出する。
Mesh VolumeSurface(const VolumeGrid& grid, std::string& error);
}  // namespace rock::geometry
