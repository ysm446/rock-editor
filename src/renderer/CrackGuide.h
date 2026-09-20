#pragma once
#include <DirectXMath.h>

#include <vector>

#include "crack/CrackPatch.h"
namespace rock::renderer {
// 通常は深度付きの線。亀裂ガイドは透視表示の面/線を指定できる。
struct OverlayLineSet {
    DirectX::XMFLOAT4 color{1, 1, 1, 1};
    std::vector<DirectX::XMFLOAT3> points;
    bool triangles = false;
    bool depthTest = true;
};
std::vector<OverlayLineSet> MakeCrackGuides(const crack::CrackPatch& patch);
}  // namespace rock::renderer
