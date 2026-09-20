#include "renderer/ModelPreview.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rock::renderer {
namespace {

constexpr float kPi = 3.14159265358979f;
// 背景色。パネルの地より少し暗くし、モデルの輪郭を見分けやすくする。
constexpr float kClearColor[4] = {0.025f, 0.025f, 0.025f, 1.0f};

// ModelPreview.hlsl の ModelConstants と一致させること。
struct ModelConstants {
    DirectX::XMFLOAT4X4 viewProjection;
    uint32_t baseColorIndex, normalIndex, roughnessIndex, metallicIndex;
    uint32_t aoIndex, opacityIndex, mapChannels, flipNormalGreen;
    uint32_t irradianceIndex, prefilteredIndex, brdfLutIndex, prefilteredMipCount;
    float baseColorTint[3];
    float roughnessValue;
    float metallicValue, aoValue, opacityValue, maskThreshold;
    float colorAdjust[2];
    float brightness;
    uint32_t blendMode;
    float cameraPosition[3];
    float exposure;
    float lightDirection[3];
    float lightIlluminance;
    float lightColor[3];
    float iblIntensity;
    uint32_t tonemapMode;
    // 0 = プレビュー（sRGB へトーンマップ）、1 = シーン（線形 HDR、影を受ける）。
    uint32_t sceneMode;
    // マップごとの UV（MaterialAsset::mapUvSets）。立っているビットのマップは 2 つ目の UV で読む。
    uint32_t mapUvSets;
    uint32_t pad;
    // ワールド行列（転置して入れる。viewProjection と同じ規約）。
    DirectX::XMFLOAT4X4 world;
    // ここから下はシーンの影。MeshPbr と同じく転置せずに入れる。
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 lightViewProjections[kShadowCascadeCount];
    uint32_t shadowIndices[kShadowCascadeCount];
    float shadowSplits[kShadowCascadeCount];
    float shadowBiases[kShadowCascadeCount];
    float shadowTexelSize;
    float shadowBlend;
    float shadowNear;
    uint32_t shadowCascadeCount;
};
static_assert(sizeof(ModelConstants) % 16 == 0);

}  // namespace

void ModelPreview::Destroy(rhi::Device& device) {
    for (auto& mesh : m_meshes) mesh.Release(device);
    m_meshes.clear();
    m_geometry.reset();
    m_lod = -1;
    device.DeferRelease(m_output);
    device.DeferRelease(m_depth);
}

void ModelPreview::ResetView() {
    m_camera.Reset();
    FrameView();
}

void ModelPreview::FocusView() {
    if (!m_geometry) return;
    using namespace DirectX;
    XMFLOAT3 center;
    XMStoreFloat3(&center, XMVectorScale(XMVectorAdd(XMLoadFloat3(&m_geometry->minimum),
                                                   XMLoadFloat3(&m_geometry->maximum)), 0.5f));
    m_camera.Focus(center);
}

void ModelPreview::FrameView() {
    if (!m_geometry) return;
    using namespace DirectX;
    const auto lo = XMLoadFloat3(&m_geometry->minimum), hi = XMLoadFloat3(&m_geometry->maximum);
    XMFLOAT3 center;
    XMStoreFloat3(&center, XMVectorScale(XMVectorAdd(lo, hi), 0.5f));
    const float radius = std::max(0.0001f, XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))) * 0.5f);
    m_camera.SetViewportSize(m_outputSize, m_outputSize);
    m_camera.SetSceneRadius(radius);
    m_camera.Frame(center, radius);
}

bool ModelPreview::Prepare(rhi::Device& device, const ModelAsset& asset, int lod) {
    if (!asset.geometry) {
        if (m_geometry) Destroy(device);
        return false;
    }
    lod = std::clamp(lod, 0, static_cast<int>(asset.geometry->lods.size()) - 1);
    if (m_geometry == asset.geometry && m_lod == lod) return true;
    for (auto& mesh : m_meshes) mesh.Release(device);
    m_meshes.clear();
    const bool changed = m_geometry != asset.geometry;
    m_geometry = asset.geometry;
    m_lod = -1;
    // LOD の番号が飛んでいる FBX では空の段がある。空の部品は描かないが、添字は部品と揃える。
    m_meshes.resize(m_geometry->lods[lod].parts.size());
    for (size_t i = 0; i < m_meshes.size(); ++i) {
        const auto& part = m_geometry->lods[lod].parts[i];
        if (!part.mesh.indices.empty() && !m_meshes[i].Create(device, part.mesh, L"ModelPreviewMesh")) return false;
    }
    m_lod = lod;
    if (changed) ResetView();
    return true;
}

void ModelPreview::Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                          const compositor::MaterialLibrary& materials,
                          const compositor::TextureLibrary& textures, const Environment& environment,
                          float iblIntensity, const LightSettings& light, float exposure,
                          TonemapMode tonemap) {
    if (!m_geometry || m_lod < 0) return;
    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath = L"ModelPreview.hlsl";
    desc.vertexEntry = L"VsMain";
    desc.pixelEntry = L"PsMain";
    desc.layout = rhi::VertexLayout::MeshStandard;
    desc.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    ID3D12PipelineState* opaquePipeline = pipelineCache.GetGraphics(desc);
    // 半透明は両面を描き、深度は読むだけにする（奥の半透明面を消さない）。
    desc.cullMode = D3D12_CULL_MODE_NONE;
    desc.depthWrite = false;
    desc.alphaBlend = true;
    ID3D12PipelineState* translucentPipeline = pipelineCache.GetGraphics(desc);
    if (opaquePipeline == nullptr || translucentPipeline == nullptr) return;

    if (!m_output.IsValid()) {
        rhi::TextureDesc target;
        target.width = target.height = m_outputSize;
        target.allowRenderTarget = true;
        std::copy(std::begin(kClearColor), std::end(kClearColor), target.clearColor);
        target.debugName = L"ModelPreview";
        if (!device.Allocator().CreateTexture2D(target, m_output)) return;
    }
    if (!m_depth.IsValid()) {
        rhi::TextureDesc depth;
        depth.width = depth.height = m_outputSize;
        depth.format = DXGI_FORMAT_D32_FLOAT;
        depth.allowDepthStencil = true;
        depth.createSrv = false;
        depth.debugName = L"ModelPreviewDepth";
        if (!device.Allocator().CreateTexture2D(depth, m_depth)) return;
    }
    m_camera.SetViewportSize(m_outputSize, m_outputSize);

    PIXBeginEvent(commandList, PIX_COLOR(120, 200, 200), "ModelPreview");
    rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rhi::TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList->ClearRenderTargetView(m_output.rtv.cpu, kClearColor, 0, nullptr);
    commandList->ClearDepthStencilView(m_depth.dsv.cpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList->OMSetRenderTargets(1, &m_output.rtv.cpu, FALSE, &m_depth.dsv.cpu);
    const D3D12_VIEWPORT viewport = {0, 0, float(m_outputSize), float(m_outputSize), 0, 1};
    const D3D12_RECT scissor = {0, 0, LONG(m_outputSize), LONG(m_outputSize)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());

    const compositor::MaterialAsset fallback;
    // 窓とサムネイルは読んだままの姿勢。部品の頂点はノードの座標なので、ノードの行列で運ぶ。
    std::vector<DirectX::XMFLOAT4X4> nodeWorlds;
    ModelNodeWorlds(*m_geometry, {}, nodeWorlds);
    const auto materialOf = [&](size_t part) -> const compositor::MaterialAsset& {
        const auto slot = m_geometry->lods[m_lod].parts[part].slot;
        const auto* material = slot < model.materials.size() ? materials.Find(model.materials[slot]) : nullptr;
        return material ? *material : fallback;
    };
    const auto draw = [&](size_t part) {
        const auto& asset = materialOf(part);
        ModelConstants constants = {};
        DirectX::XMStoreFloat4x4(&constants.viewProjection,
                                 DirectX::XMMatrixTranspose(m_camera.ViewMatrix() * m_camera.ProjectionMatrix()));
        const uint32_t node = m_geometry->lods[m_lod].parts[part].node;
        const DirectX::XMMATRIX world =
            node < nodeWorlds.size() ? DirectX::XMLoadFloat4x4(&nodeWorlds[node]) : DirectX::XMMatrixIdentity();
        DirectX::XMStoreFloat4x4(&constants.world, DirectX::XMMatrixTranspose(world));
        for (auto& index : constants.shadowIndices) index = compositor::kInvalidTextureIndex;
        // ベースカラーだけ sRGB として読む。それ以外はリニア（サムネイルと同じ）。
        constants.baseColorIndex = textures.SrvIndex(asset.baseColor, true);
        constants.normalIndex = textures.SrvIndex(asset.normal, false);
        constants.roughnessIndex = textures.SrvIndex(asset.roughness.texture, false);
        constants.metallicIndex = textures.SrvIndex(asset.metallic.texture, false);
        constants.aoIndex = textures.SrvIndex(asset.ambientOcclusion.texture, false);
        constants.opacityIndex = textures.SrvIndex(asset.opacity.texture, false);
        constants.mapChannels = compositor::PackMaterialChannels(asset);
        constants.mapUvSets = asset.mapUvSets;
        constants.flipNormalGreen = asset.flipNormalGreen ? 1u : 0u;
        const bool hasEnvironment = environment.IsReady();
        constants.irradianceIndex = hasEnvironment ? environment.IrradianceSrvIndex() : compositor::kInvalidTextureIndex;
        constants.prefilteredIndex = environment.PrefilteredSrvIndex();
        constants.brdfLutIndex = environment.BrdfLutSrvIndex();
        constants.prefilteredMipCount = environment.PrefilteredMipCount();
        constants.baseColorTint[0] = asset.baseColorTint.x;
        constants.baseColorTint[1] = asset.baseColorTint.y;
        constants.baseColorTint[2] = asset.baseColorTint.z;
        constants.roughnessValue = asset.roughnessValue;
        constants.metallicValue = asset.metallicValue;
        constants.aoValue = asset.ambientOcclusionValue;
        constants.opacityValue = asset.opacityValue;
        constants.maskThreshold = asset.maskThreshold;
        constants.colorAdjust[0] = asset.hueShiftDegrees * (kPi / 180.0f);
        constants.colorAdjust[1] = asset.saturation;
        constants.brightness = asset.brightness;
        constants.blendMode = static_cast<uint32_t>(asset.blendMode);
        const auto position = m_camera.Position();
        std::memcpy(constants.cameraPosition, &position, sizeof(position));
        constants.exposure = exposure;
        const DirectX::XMFLOAT3 lightDirection = light.Direction();
        std::memcpy(constants.lightDirection, &lightDirection, sizeof(lightDirection));
        constants.lightIlluminance = light.illuminance;
        std::memcpy(constants.lightColor, &light.color, sizeof(light.color));
        constants.iblIntensity = hasEnvironment ? iblIntensity : 0.0f;
        constants.tonemapMode = static_cast<uint32_t>(tonemap);
        const auto cb = device.Upload().Allocate(sizeof(constants), 256);
        if (!cb.IsValid()) return;
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
        m_meshes[part].Draw(commandList);
    };
    // 不透明とマスク抜きを先に描き、半透明はその上に重ねる。
    commandList->SetPipelineState(opaquePipeline);
    for (size_t i = 0; i < m_meshes.size(); ++i)
        if (m_meshes[i].IsValid() && materialOf(i).blendMode != compositor::BlendMode::Translucent) draw(i);
    commandList->SetPipelineState(translucentPipeline);
    for (size_t i = 0; i < m_meshes.size(); ++i)
        if (m_meshes[i].IsValid() && materialOf(i).blendMode == compositor::BlendMode::Translucent) draw(i);

    rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
}

void ModelPreview::RenderInScene(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                 ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                                 const compositor::MaterialLibrary& materials,
                                 const compositor::TextureLibrary& textures, const SceneDrawContext& context,
                                 const std::vector<ModelInstanceDraw>& instances) {
    if (!m_geometry || m_lod < 0 || instances.empty()) return;
    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath = L"ModelPreview.hlsl";
    desc.vertexEntry = L"VsMain";
    desc.layout = rhi::VertexLayout::MeshStandard;
    desc.rtvFormat = context.rtvFormat;
    desc.dsvFormat = context.dsvFormat;
    ID3D12PipelineState* opaquePipeline = nullptr;
    ID3D12PipelineState* translucentPipeline = nullptr;
    if (context.shadowPass) {
        // 深度だけ。片面のパーツも影を落とすよう両面を描く（道路のシャドウパスと同じ）。
        desc.cullMode = D3D12_CULL_MODE_NONE;
        opaquePipeline = pipelineCache.GetGraphics(desc);
    } else {
        desc.pixelEntry = L"PsMain";
        opaquePipeline = pipelineCache.GetGraphics(desc);
        desc.cullMode = D3D12_CULL_MODE_NONE;
        desc.depthWrite = false;
        desc.alphaBlend = true;
        translucentPipeline = pipelineCache.GetGraphics(desc);
    }
    if (opaquePipeline == nullptr) return;
    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());

    const compositor::MaterialAsset fallback;
    const auto materialOf = [&](size_t part) -> const compositor::MaterialAsset& {
        const auto slot = m_geometry->lods[m_lod].parts[part].slot;
        const auto* material = slot < model.materials.size() ? materials.Find(model.materials[slot]) : nullptr;
        return material ? *material : fallback;
    };
    const bool hasEnvironment = context.environment != nullptr && context.environment->IsReady();
    // 置いたモデルごとの、ノードのモデル座標での行列（回転を足したもの）。
    static const std::vector<ModelNodeRotation> kNoRotations;
    std::vector<std::vector<DirectX::XMFLOAT4X4>> nodeWorlds(instances.size());
    for (size_t i = 0; i < instances.size(); ++i)
        ModelNodeWorlds(*m_geometry, instances[i].rotations ? *instances[i].rotations : kNoRotations, nodeWorlds[i]);
    const auto draw = [&](size_t part, size_t instance) {
        const auto& asset = materialOf(part);
        const uint32_t node = m_geometry->lods[m_lod].parts[part].node;
        DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&instances[instance].world);
        if (node < nodeWorlds[instance].size()) world = DirectX::XMLoadFloat4x4(&nodeWorlds[instance][node]) * world;
        ModelConstants constants = {};
        DirectX::XMStoreFloat4x4(&constants.viewProjection,
                                 DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&context.viewProjection)));
        DirectX::XMStoreFloat4x4(&constants.world, DirectX::XMMatrixTranspose(world));
        constants.sceneMode = 1;
        constants.baseColorIndex = textures.SrvIndex(asset.baseColor, true);
        constants.normalIndex = textures.SrvIndex(asset.normal, false);
        constants.roughnessIndex = textures.SrvIndex(asset.roughness.texture, false);
        constants.metallicIndex = textures.SrvIndex(asset.metallic.texture, false);
        constants.aoIndex = textures.SrvIndex(asset.ambientOcclusion.texture, false);
        constants.opacityIndex = textures.SrvIndex(asset.opacity.texture, false);
        constants.mapChannels = compositor::PackMaterialChannels(asset);
        constants.mapUvSets = asset.mapUvSets;
        constants.flipNormalGreen = asset.flipNormalGreen ? 1u : 0u;
        constants.irradianceIndex = hasEnvironment ? context.environment->IrradianceSrvIndex()
                                                   : compositor::kInvalidTextureIndex;
        constants.prefilteredIndex = hasEnvironment ? context.environment->PrefilteredSrvIndex() : 0u;
        constants.brdfLutIndex = hasEnvironment ? context.environment->BrdfLutSrvIndex() : 0u;
        constants.prefilteredMipCount = hasEnvironment ? context.environment->PrefilteredMipCount() : 1u;
        constants.baseColorTint[0] = asset.baseColorTint.x;
        constants.baseColorTint[1] = asset.baseColorTint.y;
        constants.baseColorTint[2] = asset.baseColorTint.z;
        constants.roughnessValue = asset.roughnessValue;
        constants.metallicValue = asset.metallicValue;
        constants.aoValue = asset.ambientOcclusionValue;
        constants.opacityValue = asset.opacityValue;
        constants.maskThreshold = asset.maskThreshold;
        constants.colorAdjust[0] = asset.hueShiftDegrees * (kPi / 180.0f);
        constants.colorAdjust[1] = asset.saturation;
        constants.brightness = asset.brightness;
        constants.blendMode = static_cast<uint32_t>(asset.blendMode);
        std::memcpy(constants.cameraPosition, &context.cameraPosition, sizeof(context.cameraPosition));
        constants.exposure = 1.0f;
        std::memcpy(constants.lightDirection, &context.lightDirection, sizeof(context.lightDirection));
        constants.lightIlluminance = context.lightIlluminance;
        std::memcpy(constants.lightColor, &context.lightColor, sizeof(context.lightColor));
        constants.iblIntensity = hasEnvironment ? context.iblIntensity : 0.0f;
        constants.view = context.view;
        for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
            constants.lightViewProjections[i] = context.lightViewProjections[i];
            constants.shadowIndices[i] = context.shadowPass ? compositor::kInvalidTextureIndex : context.shadowIndices[i];
            constants.shadowSplits[i] = context.shadowSplits[i];
            constants.shadowBiases[i] = context.shadowBiases[i];
        }
        constants.shadowTexelSize = context.shadowTexelSize;
        constants.shadowBlend = context.shadowBlend;
        constants.shadowNear = context.shadowNear;
        constants.shadowCascadeCount = context.shadowCascadeCount;
        const auto cb = device.Upload().Allocate(sizeof(constants), 256);
        if (!cb.IsValid()) return;
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
        m_meshes[part].Draw(commandList);
    };
    // 不透明とマスク抜きを先に描く。半透明は影を落とさず、本描画の最後に重ねる。
    commandList->SetPipelineState(opaquePipeline);
    for (size_t instance = 0; instance < instances.size(); ++instance)
        for (size_t i = 0; i < m_meshes.size(); ++i)
            if (m_meshes[i].IsValid() && materialOf(i).blendMode != compositor::BlendMode::Translucent) draw(i, instance);
    if (translucentPipeline != nullptr) {
        commandList->SetPipelineState(translucentPipeline);
        for (size_t instance = 0; instance < instances.size(); ++instance)
            for (size_t i = 0; i < m_meshes.size(); ++i)
                if (m_meshes[i].IsValid() && materialOf(i).blendMode == compositor::BlendMode::Translucent)
                    draw(i, instance);
    }
}

}  // namespace rock::renderer
