#pragma once
#include <array>
#include <string>

#include "geometry/Mesh.h"
namespace rock::crack {
// 有限パッチの中心。extent は半幅。回転は右手系 Z → X → Y、度。
struct CrackSettings {
    std::array<float, 3> center{0, 0, 0};
    std::array<float, 3> rotationDegrees{0, 0, 0};
    float extentU = 1.2f;
    float extentV = 1.2f;
    float depth = 1.6f;
    float persistence = 0.6f;
    float aperture = 0.02f;
    bool showGuide = true;
    bool meshCut = false;   // 有限長の Mesh 溝。旧シーンは Box 専用の方式を維持する。
    bool applyCut = false;  // 旧シーンはガイド表示を維持する。
    bool showBridge = true;
};
struct CrackPatch {
    geometry::Vec3 center, tangentU, tangentV, normal;
    std::array<geometry::Vec3, 4> boundary;
    std::array<geometry::Vec3, 4> reached;
    float effectiveDepth = 0;
    float aperture = 0;
};
// P2 の可視化規約: +V 端から -V へ min(depth, 2*extentV)*persistence 進む。
// 実形状との交差や Rock Bridge の有無は判定しない。
bool BuildCrackPatch(const CrackSettings& settings, CrackPatch& patch, std::string& error);
}  // namespace rock::crack
