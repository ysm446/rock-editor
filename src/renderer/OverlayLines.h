#pragma once
#include <DirectXMath.h>

#include <vector>

namespace rock::renderer {
// 通常は深度付きの線。triangles で半透明の面、depthTest=false で透視表示にできる。
struct OverlayLineSet {
    DirectX::XMFLOAT4 color{1, 1, 1, 1};
    std::vector<DirectX::XMFLOAT3> points;
    bool triangles = false;
    bool depthTest = true;
};
}  // namespace rock::renderer
