#pragma once

#include "renderer/Environment.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <cstdint>

// シーンの空（大気散乱）。terrain-graph の Atmosphere から、雲・月と星空・ゴッドレイ・アニメーションを除いたもの。
// 仕様は docs/design/rendering.md の「シーンの空（大気散乱）」。
namespace tg::renderer {

// HLSL の AtmosphericParameters（shaders/AtmosphereCommon.hlsli）と同じ並び。
// 距離は m、角度はラジアン、太陽は大気圏外照度（lux）。
struct AtmosphereSettings {
    float azimuth = 0.9f;
    float elevation = 0.9f;
    float illuminance = 120000.0f;
    float density = 1.0f;        // レイリー散乱の強さ（1 が地球の標準）
    float mie = 0.3f;            // ミー散乱（エアロゾル）の密度の倍率
    float eccentricity = 0.8f;   // ミー散乱の異方性 g
    float altitude = 0.0f;       // 地表の基準標高（m）
    float groundAlbedo = 0.2f;
    uint32_t lowerHemisphere = 1;  // 0: 空の延長（上半球の折り返し）、1: 地面反射
    float padding[3] = {};
};
static_assert(sizeof(AtmosphereSettings) == 48);

class Atmosphere {
public:
    // 設定が変わっていれば、多重散乱の LUT・地面反射の輝度・環境マップ（背景と IBL）を作り直す。
    // GPU の完了を待つので、フレームの外（PreviewRenderer::ProcessPendingWork）で呼ぶ。
    bool Update(rhi::Device& device, rhi::PipelineCache& pipelines, const AtmosphereSettings& settings);
    void Shutdown(rhi::Device& device);
    const Environment& GetEnvironment() const { return m_environment; }
    bool IsReady() const { return m_ready; }
    const AtmosphereSettings& AppliedSettings() const { return m_applied; }

private:
    Environment m_environment;
    rhi::GpuTexture m_multiScatter;  // 多重散乱の LUT（32 x 32）
    rhi::GpuTexture m_ground;        // 地面反射の輝度（1 x 1）
    AtmosphereSettings m_applied;
    bool m_initialized = false;
    bool m_ready = false;
};

// 大気を通った太陽の透過率（地表の基準標高から見た、大気圏外に対する割合）。地平線より下は 0。
// シェーダの AtmComputeSunTransmittance と同じ積分を CPU で行う（直接光の色と太陽の円盤に使う）。
DirectX::XMFLOAT3 AtmosphereSunTransmittance(const AtmosphereSettings& settings);

}  // namespace tg::renderer
