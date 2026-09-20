#pragma once
#include <optional>

#include "crack/CrackPatch.h"
namespace rock::crack {
// 単一 Box に残る、亀裂終端から -V 側の外面までの未破断断面。
// 独立 Chunk ではなく、同じ連結体内の両側領域をつなぐ。
struct RockBridge {
    float thickness = 0;
    float area = 0;
    std::array<geometry::Vec3, 4> section;
};
struct PartialCutResult {
    geometry::Mesh mesh;
    std::optional<RockBridge> bridge;
    float penetration = 0;
    double removedVolume = 0;
    std::string status;
    std::string error;
};
// 軸に沿ったパッチによる有限の直方体切り込み。任意角度・貫通・複数切断は P3 の対象外。
// aperture/depth がゼロ、または非交差なら Box をそのまま返す。
PartialCutResult CutBox(const std::array<float, 3>& size, const CrackSettings& settings);
}  // namespace rock::crack
