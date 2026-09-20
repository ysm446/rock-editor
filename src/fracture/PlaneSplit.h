#pragma once
#include <array>
#include <string>
#include "geometry/Mesh.h"

namespace rock::fracture {
struct ChunkSettings {
    bool locked = true;
    std::array<float, 3> position{};
    std::array<float, 3> rotationDegrees{};
};
struct FractureSettings {
    std::array<float, 3> center{};
    std::array<float, 3> rotationDegrees{};
    std::array<ChunkSettings, 2> chunks;
};
struct SplitResult {
    // 0 は平面の負側、1 は正側。双方とも元の座標系を使う。
    std::array<geometry::Mesh, 2> meshes;
    double sectionArea = 0;
    std::string error;
};
// 単一の閉じた入力を、単純な断面ループ1本で2つに分ける。
// 複数ループ・穴・平面上の面・極小片は診断し、部分結果を返さない。
SplitResult SplitByPlane(const geometry::Mesh& input, geometry::Vec3 center, geometry::Vec3 normal);
}  // namespace rock::fracture
