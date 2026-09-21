#pragma once
#include "geometry/BoxCluster.h"
#include <stop_token>

namespace rock::geometry {
struct VolumeSettings {
    int resolution = 48;  // 母岩の最長辺のセル数。16～96。
};
enum class VolumeMeshingMethod { MarchingTetrahedra, DualContouring };
struct VolumeToMeshSettings {
    VolumeMeshingMethod method = VolumeMeshingMethod::MarchingTetrahedra;
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
// Volume Transform。倍率 → 回転 → 平行移動の順に、原点まわりで動かす。
// 回転は右手系 Z → X → Y、度（Model と同じ規約）。
struct VolumeTransformSettings {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> rotationDegrees{0, 0, 0};
    float scale = 1;
};
VolumeGrid BoxesToVolume(const std::vector<OrientedBox>& boxes, const VolumeSettings& settings,
                         std::string& error);
// 閉じた向き付きメッシュを変換。重複成分は和集合、内向きの内殻は空洞として扱う。
VolumeGrid MeshToVolume(const Mesh& mesh, const VolumeSettings& settings, std::string& error,
                        std::stop_token stop = {});
// 格子を作り直して移動・回転・拡大する。セル間隔は倍率に比例させ、解像度を保つ。
VolumeGrid TransformVolume(const VolumeGrid& grid, const VolumeTransformSettings& settings,
                           std::string& error);
// 表示用の等値面。グリッドを残し、内部に重複面のない外皮を抽出する。
Mesh VolumeSurface(const VolumeGrid& grid, std::string& error,
                   VolumeMeshingMethod method = VolumeMeshingMethod::MarchingTetrahedra);
}  // namespace rock::geometry
