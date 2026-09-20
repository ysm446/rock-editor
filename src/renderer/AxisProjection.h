#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace tg::renderer {

// 投影位置での1mあたりの画面変位。有限距離のプローブを使わず、
// 透視除算の微分でカメラ面をまたぐ反転と差分の桁落ちを避ける。
struct AxisProjection {
    DirectX::XMFLOAT2 delta[3]{};
    float pixelsPerMeter[3]{};
};

inline AxisProjection ProjectMoveAxes(const DirectX::XMMATRIX& viewProjection,
                                     const DirectX::XMFLOAT3& center,
                                     float width, float height, float maxLength,
                                     int axisCount = 3, const DirectX::XMFLOAT3* localAxes = nullptr) {
    using namespace DirectX;
    AxisProjection result;
    XMFLOAT4 clip;
    XMStoreFloat4(&clip, XMVector3Transform(XMLoadFloat3(&center), viewProjection));
    if (!std::isfinite(clip.w) || clip.w <= 1e-4f || width <= 0 || height <= 0) return result;
    const XMFLOAT3 worldAxes[] = {{1,0,0}, {0,0,1}, {0,1,0}}; // X / Z / Y
    const auto* axes = localAxes ? localAxes : worldAxes;
    axisCount = std::clamp(axisCount, 0, 3);
    float maximum = 0;
    for (int i = 0; i < axisCount; ++i) {
        XMFLOAT4 direction;
        XMStoreFloat4(&direction, XMVector4Transform(XMVectorSet(
            axes[i].x, axes[i].y, axes[i].z, 0), viewProjection));
        const float x = (direction.x - clip.x / clip.w * direction.w) / clip.w * width * 0.5f;
        const float y = -(direction.y - clip.y / clip.w * direction.w) / clip.w * height * 0.5f;
        const float pixels = std::hypot(x, y);
        if (!std::isfinite(pixels)) continue;
        result.delta[i] = {x, y};
        result.pixelsPerMeter[i] = pixels;
        maximum = std::max(maximum, pixels);
    }
    if (maximum <= 1e-4f) return {};
    // 全軸に共通の倍率を使い、視線方向への短縮を保つ。
    for (int i = 0; i < axisCount; ++i) {
        result.delta[i].x *= maxLength / maximum;
        result.delta[i].y *= maxLength / maximum;
    }
    return result;
}

}  // namespace tg::renderer
