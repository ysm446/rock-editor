#include "renderer/Atmosphere.h"

#include "core/Log.h"
#include "../../shaders/AtmosphereIntegration.hlsli"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tg::renderer {
namespace {

constexpr auto kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

bool CreateTarget(rhi::Device& device, rhi::GpuTexture& texture, uint32_t width, uint32_t height, const wchar_t* name) {
    rhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.allowUnorderedAccess = true;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    desc.debugName = name;
    return device.Allocator().CreateTexture2D(desc, texture);
}

}  // namespace

bool Atmosphere::Update(rhi::Device& device, rhi::PipelineCache& pipelines, const AtmosphereSettings& settings) {
    if (m_ready && std::memcmp(&settings, &m_applied, sizeof(settings)) == 0) return true;
    if (!m_initialized) {
        if (!m_environment.Initialize(device, pipelines, false) ||
            !CreateTarget(device, m_multiScatter, 32, 32, L"AtmosphereMultiScatter") ||
            !CreateTarget(device, m_ground, 1, 1, L"AtmosphereGround")) {
            Shutdown(device);
            return false;
        }
        m_initialized = true;
    }
    auto* lutPipeline = pipelines.GetCompute(L"AtmosphereMultiScatter.hlsl", L"CSGenerate");
    auto* groundPipeline = pipelines.GetCompute(L"AtmosphereGround.hlsl", L"CsMain");
    if (lutPipeline == nullptr || groundPipeline == nullptr) return false;

    // 多重散乱の LUT は大気の濃さと地面の反射率だけで決まる（太陽の向きでは変わらない）。
    const bool updateLut = !m_ready || settings.density != m_applied.density || settings.mie != m_applied.mie ||
                           settings.groundAlbedo != m_applied.groundAlbedo;
    struct GroundConstants {
        AtmosphereSettings settings;
        uint32_t output, lut, pad[2];
    };
    const GroundConstants groundConstants{settings, m_ground.UavIndex(), m_multiScatter.SrvIndex(), {0, 0}};
    const auto groundAllocation = device.Upload().Allocate(sizeof(GroundConstants), 256);
    if (!groundAllocation.IsValid()) return false;
    std::memcpy(groundAllocation.cpu, &groundConstants, sizeof(groundConstants));
    if (!device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
            PIXBeginEvent(commands, PIX_COLOR(120, 180, 255), "AtmosphereCaches");
            commands->SetComputeRootSignature(pipelines.GlobalRootSignature());
            if (updateLut) {
                TransitionIfNeeded(commands, m_multiScatter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                struct Constants { float density, mie, groundAlbedo; uint32_t output; };
                const Constants constants{settings.density, settings.mie, settings.groundAlbedo, m_multiScatter.UavIndex()};
                commands->SetPipelineState(lutPipeline);
                commands->SetComputeRoot32BitConstants(0, 4, &constants, 0);
                commands->Dispatch(4, 4, 1);
                TransitionIfNeeded(commands, m_multiScatter, kReadState);
            }
            TransitionIfNeeded(commands, m_ground, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetComputeRootConstantBufferView(1, groundAllocation.gpuAddress);
            commands->SetPipelineState(groundPipeline);
            commands->Dispatch(1, 1, 1);
            TransitionIfNeeded(commands, m_ground, kReadState);
            PIXEndEvent(commands);
        })) {
        return false;
    }
    if (!m_environment.BuildFromAtmosphere(device, pipelines, settings, m_multiScatter.SrvIndex(), m_ground.SrvIndex())) {
        m_ready = false;
        TG_LOG_WARN("大気散乱の環境マップを生成できませんでした");
        return false;
    }
    m_applied = settings;
    m_ready = true;
    return true;
}

void Atmosphere::Shutdown(rhi::Device& device) {
    m_environment.Shutdown(device);
    device.DeferRelease(m_multiScatter);
    device.DeferRelease(m_ground);
    m_initialized = false;
    m_ready = false;
}

DirectX::XMFLOAT3 AtmosphereSunTransmittance(const AtmosphereSettings& p) {
    // 地球の半径・大気の上端・スケールハイト・散乱係数は shaders/AtmosphereScattering.hlsli と同じ。
    const double radius = 6360000.0, top = 6420000.0, origin = radius + std::max(1.0f, p.altitude);
    const double mu = std::sin(p.elevation), b = origin * mu;
    const double groundD = b * b - (origin * origin - radius * radius);
    if (groundD >= 0.0 && -b - std::sqrt(groundD) > 0.0) return {0.0f, 0.0f, 0.0f};
    const double distance = -b + std::sqrt(b * b - origin * origin + top * top);
    double opticalR = 0.0, opticalM = 0.0;
    for (int i = 0; i < 32; ++i) {
        const double start = distance * AtmosphereRayFraction(static_cast<float>(i) / 32.0f);
        const double end = distance * AtmosphereRayFraction(static_cast<float>(i + 1) / 32.0f);
        const double t = (start + end) * 0.5;
        const double height = std::sqrt(origin * origin + t * t + 2.0 * b * t) - radius;
        opticalR += std::exp(-height / 7994.0) * (end - start);
        opticalM += std::exp(-height / 1200.0) * (end - start);
    }
    const double mie = 21e-6 * p.mie * 1.1 * opticalM;
    return {static_cast<float>(std::exp(-5.802e-6 * p.density * opticalR - mie)),
            static_cast<float>(std::exp(-13.558e-6 * p.density * opticalR - mie)),
            static_cast<float>(std::exp(-33.1e-6 * p.density * opticalR - mie))};
}

}  // namespace tg::renderer
