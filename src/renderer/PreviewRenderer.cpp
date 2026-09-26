#include "renderer/PreviewRenderer.h"
#include "renderer/MaterialBake.h"
#include "rhi/TextureReadback.h"

#include "core/ImageIo.h"
#include "core/Log.h"

#include <pix3.h>

#include <cmath>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <initializer_list>

using namespace DirectX;

namespace rock::renderer {
namespace {

constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
// 路面に貼る帯（白線）の深度バイアス。D32 なので定数項は深度の指数に対する相対値。負で手前。
constexpr int kDecalDepthBias = -2000;
constexpr float kDecalSlopeScaledDepthBias = -2.0f;
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// ガイド線の端点の最大数。シェーダの ROCK_OVERLAY_MAX_VERTICES と一致させること。
// 定数バッファの上限（64 KiB）に収まる数。断面の色分けのような三角形の多いオーバーレイを少ない描画回数で送る。
constexpr uint32_t kOverlayLineMaxVertices = 4000;

// GPU 側の OverlayLineConstants と一致させること。
struct OverlayLineConstants {
    DirectX::XMFLOAT4X4 viewProjection;
    float color[4];
    DirectX::XMFLOAT4 options; // x: 深度バイアス（NDC）
    // xyz: ワールド座標、w: 端点ごとの不透明度。
    DirectX::XMFLOAT4 positions[kOverlayLineMaxVertices];
};

// 背景をぼかすときに引くプリフィルタ済みキューブのミップ。小数で指定する。
//
// **ミップ m はラフネス m / (段数 - 1) に対応する**（プリフィルタは 6 段）。
// 1.6 でおよそラフネス 0.32。空と地面の分かれ目や光の向きは残るが、
// 木立や建物の形は溶けて目に留まらなくなる、という強さ。
// 素材を見比べるときに背景が目移りの原因にならないことを優先している。
constexpr float kSkyboxBlurMip = 1.6f;
constexpr DXGI_FORMAT kShadowDsvFormat = DXGI_FORMAT_D32_FLOAT;

// シェーダの「影を落とさない」印。
constexpr uint32_t kNoShadowIndex = 0xFFFFFFFFu;

// GPU 側の MeshConstants と一致させること。
struct LayerContextConstants {
    uint32_t layerBaseColorIndex[4];
    uint32_t layerNormalIndex[4];
    uint32_t layerSurfaceIndex[4];
    uint32_t layerHeightIndex[4];
    uint32_t layerWorldUv[4];
    float layerUvRepeat[4];
    uint32_t layerHeightGate[4];
    float layerHeightGateThreshold[4];
    float layerHeightGateSoftness[4];
    uint32_t layerBlendMode[4];
    uint32_t roadMaskIndex;
    float layerBlendRange;
    uint32_t roadUvAlongU;
    float displacementMeters;
    float roadMaskScale[2];
    float origin[2];
};
static_assert(sizeof(LayerContextConstants) == 192);

// MeshConstants::meshDisplayFlags のビット。**HLSL 側の ROCK_MESH_FLAG_* と一致させること。**
constexpr uint32_t kMeshFlagUvChecker = 2u;
constexpr uint32_t kMeshFlagOutlineHovered = 4u;
constexpr uint32_t kMeshFlagOutlineSelected = 8u;

struct AppliedConstants {
    uint32_t maps[4];
    XMFLOAT4 axisX, axisY, axisZ;
    XMFLOAT3 offset; uint32_t method;
    uint32_t maskIndex; float maskValue, maskRepeat; uint32_t maskFlags;
};
struct MeshConstants {
    XMFLOAT4X4 viewProjection;
    // 法線をカメラ空間で見るためのビュー行列。**HLSL 側と同じ並びにすること。**
    XMFLOAT4X4 view;
    XMFLOAT4X4 model;
    XMFLOAT4X4 normalMatrix;

    XMFLOAT3 cameraPosition;
    uint32_t uvCheckerIndex;

    XMFLOAT3 lightDirection;
    float lightIlluminance;

    XMFLOAT3 lightColor;
    float surfaceDepthBiasMeters;

    XMFLOAT3 baseColor;
    float roughness;

    float metallic;
    float iblIntensity;
    uint32_t prefilteredMipCount;
    float additiveHeightMeters;

    uint32_t irradianceIndex;
    uint32_t prefilteredIndex;
    uint32_t brdfLutIndex;
    uint32_t useMaterialTextures;

    uint32_t materialBaseColorIndex;
    uint32_t materialNormalIndex;
    uint32_t materialSurfaceIndex;
    uint32_t materialHeightIndex;

    uint32_t debugView;
    float displacementScale;
    // float4 の区切りを守るための詰め物。**HLSL 側と必ず同じ数だけ置くこと。**
    float roadMetersPerUv;
    uint32_t meshDisplayFlags;

    XMFLOAT4X4 lightViewProjections[kShadowCascadeCount];
    uint32_t shadowIndices[kShadowCascadeCount];
    float shadowSplits[kShadowCascadeCount];
    float shadowBiases[kShadowCascadeCount];
    float shadowTexelSize;
    float shadowBlend;
    float shadowNear;
    uint32_t shadowCascadeCount;

    XMFLOAT4X4 tessellationViewProjection;
    float viewportSize[2];
    float tessellationMaxFactor;
    float tessellationTargetPixels;

    uint32_t displacementHeightIndex;
    uint32_t displacementUseRoadUv;
    // 不透明度の扱い。0 = 不透明、1 = マスク抜き（opacityThreshold 未満を捨てる）、2 = 半透明。
    uint32_t opacityMode;
    float opacityThreshold;

    // 道路のレイヤー（スロット 1〜4）。HLSL 側と同じ並び。docs/design/road-material-layers.md。
    uint32_t layerBaseColorIndex[4];
    uint32_t layerNormalIndex[4];
    uint32_t layerSurfaceIndex[4];
    uint32_t layerHeightIndex[4];
    uint32_t layerWorldUv[4];
    float layerUvRepeat[4];
    uint32_t roadMaskIndex;    // 道路空間マスクの SRV。無ければ kNoShadowIndex
    uint32_t layerCount;       // 変位に使うスロット数（0 なら変位しない）
    float layerBlendRange;
    uint32_t roadUvAlongU;
    float roadMaskScale[2];    // (1/幅, 1/長さ)。道路座標（m）→ マスク UV
    float roadUvMetersPerUv;   // roadUv 1 あたりの実距離（道路のスロット 1 の反復長）
    uint32_t shadeLayers;      // 1 ならピクセルもレイヤーでブレンドする（道路面）
    // 下地のハイトで絞る。HLSL 側と同じ並び。
    uint32_t layerHeightGate[4];
    float layerHeightGateThreshold[4];
    float layerHeightGateSoftness[4];
    uint32_t layerBlendMode[4];  // 0 = マスクどおり、1 = ハイトで競合
    float layerDisplacementMeters[4];
    uint32_t connectionPrototype;
    uint32_t connectionContextCount;
    float connectionHeightFade[2];
    LayerContextConstants connectionContexts[7];
    float connectionSecondHeightFade[2];
    uint32_t connectionRoadMixIndex;
    uint32_t connectionEndPad;
    struct BoundaryConstants {
        uint32_t mask, height, alongU, invertMask;
        float center, acrossSign, width, repeat;
        float depth, heightCenter, pad0, pad1;
    } boundaries[16];
    uint32_t boundaryControlIndex;
    float boundaryFrameSign;
    uint32_t boundaryCount;
    float boundaryPad;
    XMFLOAT4 mappingAxisX;
    XMFLOAT4 mappingAxisY;
    XMFLOAT4 mappingAxisZ;
    XMFLOAT3 mappingOffset;
    uint32_t mappingMethod;
    AppliedConstants applied[8];
    uint32_t appliedCount; uint32_t appliedPadding[3];
};

void FillApplied(AppliedConstants& out, const SceneMesh::AppliedMaterial& source,
                 const compositor::MaterialTextureSet& maps, const compositor::TextureLibrary& textures) {
    out.maps[0] = maps.baseColor.SrvIndex(); out.maps[1] = maps.normal.SrvIndex();
    out.maps[2] = maps.surface.SrvIndex(); out.maps[3] = maps.height.SrvIndex();
    const auto& m = source.mapping;
    const auto rotation = XMMatrixRotationRollPitchYaw(XMConvertToRadians(m.rotationDegrees.x),
        XMConvertToRadians(m.rotationDegrees.y), XMConvertToRadians(m.rotationDegrees.z));
    XMStoreFloat4(&out.axisX, rotation.r[0]); XMStoreFloat4(&out.axisY, rotation.r[1]); XMStoreFloat4(&out.axisZ, rotation.r[2]);
    out.axisX.w = 1 / m.repeatMeters; out.axisY.w = m.sharpness;
    // axisZ.w はハイト合成のなだらかさ（maskFlags の 4 が立つときだけ読む）。
    out.axisZ.w = std::clamp(source.heightBlendRange, .01f, 1.f);
    out.offset = m.offset; out.method = uint32_t(m.method);
    out.maskIndex = textures.SrvIndex(source.mask.texture, false);
    out.maskValue = source.mask.value; out.maskRepeat = source.mask.repeatMeters;
    out.maskFlags = (source.mask.invert ? 1u : 0u) | (source.mask.triplanar ? 2u : 0u) | (source.heightBlend ? 4u : 0u) | (source.channels << 8);
}

// 道路空間マスク（RGBA8）を GPU へ上げる。ミップは持たない（低解像度でぼかして読む）。
bool CreateRoadMaskTexture(rhi::Device& device, const SceneMesh::RoadMaskPixels& pixels, rhi::GpuTexture& outTexture) {
    rhi::TextureDesc desc;
    desc.width = pixels.width;
    desc.height = pixels.height;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    desc.debugName = L"RoadMask";
    if (!device.Allocator().CreateTexture2D(desc, outTexture)) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC resourceDesc = outTexture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &totalBytes);
    rhi::GpuBuffer staging;
    if (!device.Allocator().CreateUploadBuffer(totalBytes, L"RoadMaskStaging", staging)) {
        device.DeferRelease(outTexture);
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (FAILED(staging.resource->Map(0, &readRange, &mapped))) {
        device.DeferRelease(staging);
        device.DeferRelease(outTexture);
        return false;
    }
    auto* destination = static_cast<uint8_t*>(mapped) + footprint.Offset;
    const size_t sourcePitch = size_t(pixels.width) * 4;
    for (uint32_t row = 0; row < rowCount; ++row) {
        std::memcpy(destination + size_t(row) * footprint.Footprint.RowPitch, pixels.rgba.data() + size_t(row) * sourcePitch,
                    static_cast<size_t>(rowSizeInBytes));
    }
    staging.resource->Unmap(0, nullptr);
    const bool uploaded = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(160, 200, 120), "UploadRoadMask");
        const CD3DX12_TEXTURE_COPY_LOCATION destinationLocation(outTexture.resource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(staging.resource.Get(), footprint);
        commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        // 読み取り状態への遷移は描画側のコマンドリストで行う（ExecuteImmediate は転送専用）。
        outTexture.state = D3D12_RESOURCE_STATE_COPY_DEST;
        PIXEndEvent(commandList);
    });
    device.DeferRelease(staging);
    if (!uploaded) {
        device.DeferRelease(outTexture);
        return false;
    }
    return true;
}

// GPU 側の SkyboxConstants と一致させること。
struct SkyboxConstants {
    XMFLOAT4X4 inverseViewProjection;

    XMFLOAT3 cameraPosition;
    float intensity;

    uint32_t environmentIndex;
    float mipLevel;
    float pad0[2];

    // 太陽の円盤（シーンの空のときだけ。sunRadiance が 0 なら描かない）。
    XMFLOAT3 sunDirection;
    float sunAngularRadius;
    XMFLOAT3 sunRadiance;
    float pad1;
};

// GPU 側の DofConstants と一致させること。
struct DofConstants {
    uint32_t sourceIndex;
    uint32_t depthIndex;
    uint32_t outputIndex;
    uint32_t width;

    uint32_t height;
    float focalLengthMm;
    float fStop;
    float focusDistance;

    float nearZ;
    float farZ;
    float maxBlurPixels;
    float apertureRotation;

    float apertureBlades;
    float blurScale;
    // シーンの距離に掛ける縮尺（1 / ミニチュアの縮尺）。1 で実物大。
    float sceneScale;
    float pad0;
};

// メッシュ 1 回ぶんの描画を数える。
void CountMeshDraw(RenderStats& stats, const Mesh& mesh, bool asPatches) {
    if (!mesh.IsValid()) {
        return;
    }
    ++stats.drawCalls;
    stats.vertices += mesh.IndexCount();
    if (asPatches) {
        // 3 制御点のパッチとして投入する。実際の三角形はドメインシェーダが決める。
        stats.patches += mesh.IndexCount() / 3;
    } else {
        stats.triangles += mesh.IndexCount() / 3;
    }
}

// 絞りの形から羽根の数へ。0 なら円。
float ApertureBladeCount(ApertureShape shape) {
    switch (shape) {
        case ApertureShape::Triangle: return 3.0f;
        case ApertureShape::Hexagon: return 6.0f;
        case ApertureShape::Octagon: return 8.0f;
        default: return 0.0f;
    }
}

struct TonemapConstants {
    uint32_t sourceIndex;
    uint32_t outputIndex;
    uint32_t width;
    uint32_t height;
    float exposure;
    uint32_t tonemapMode;
    // 0 以外なら、メッシュ側が書いた値をそのまま出す（チャンネルを覗く表示）。
    uint32_t passthrough;
};

// 陰影を付けて描く表示か。**クレイもこちら側**（テクスチャを貼らないだけで
// 陰影は本物なので、背景・被写界深度・露出・トーンマップはシェーディングと同じ）。
// チャンネルを覗く表示だけが、値をそのまま画面へ出す。
bool IsShadedView(DebugView view) {
    return view == DebugView::Shaded || view == DebugView::Clay;
}

}  // namespace

bool BakeMaterial(rhi::Device& device, rhi::PipelineCache& pipelines, const SceneMesh& source,
                  const compositor::TextureLibrary& textures, const compositor::MaterialLibrary& materials,
                  uint32_t width, uint32_t height, std::array<LdrImage,4>& images, std::string& error) {
    images = {}; error.clear();
    if (!source.materialStack || !width || !height || width>8192 || height>8192) { error="ベイク入力または解像度が無効です"; return false; }
    compositor::MaterialEvaluator evaluator;
    std::vector<std::unique_ptr<compositor::MaterialEvaluator>> appliedEvaluators;
    const uint32_t sourceResolution=std::clamp(std::max(width,height),128u,4096u);
    Mesh mesh;
    rhi::GpuTexture target;
    rhi::GpuBuffer constantsBuffer;
    const auto cleanup = [&] { mesh.Release(device); evaluator.Destroy(device); for (auto& e : appliedEvaluators) e->Destroy(device); device.DeferRelease(target); device.DeferRelease(constantsBuffer); };
    if (!mesh.Create(device,source.geometry,L"BakeMesh") || !evaluator.Create(device,sourceResolution,false)) {
        error="ベイク用リソースを作成できません"; cleanup(); return false;
    }
    bool evaluated=false;
    if (!device.ExecuteImmediate([&](auto* list) { evaluated=evaluator.Evaluate(device,pipelines,list,*source.materialStack,textures,materials,{{0,0,sourceResolution,sourceResolution}}); }) || !evaluated) {
        error="ベイク元の材質を評価できません"; cleanup(); return false;
    }
    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath=L"MeshPbr.hlsl"; desc.vertexEntry=L"VsBake"; desc.pixelEntry=L"PsBake";
    desc.rtvFormat=DXGI_FORMAT_R8G8B8A8_UNORM; desc.layout=rhi::VertexLayout::MeshStandard;
    desc.cullMode=D3D12_CULL_MODE_NONE; desc.depthTest=false; desc.depthWrite=false;
    auto* pipeline=pipelines.GetGraphics(desc);
    rhi::TextureDesc targetDesc;
    targetDesc.width=width; targetDesc.height=height; targetDesc.allowRenderTarget=true;
    targetDesc.initialState=D3D12_RESOURCE_STATE_RENDER_TARGET;
    targetDesc.debugName=L"MaterialBake";
    if (!pipeline || !device.Allocator().CreateTexture2D(targetDesc,target) ||
        !device.Allocator().CreateUploadBuffer((sizeof(MeshConstants)+255)&~255ull,L"BakeConstants",constantsBuffer)) {
        error="ベイク用シェーダまたはターゲットを作成できません"; cleanup(); return false;
    }
    MeshConstants constants{};
    const auto& maps=evaluator.Textures();
    constants.materialBaseColorIndex=maps.baseColor.SrvIndex(); constants.materialNormalIndex=maps.normal.SrvIndex();
    constants.materialSurfaceIndex=maps.surface.SrvIndex(); constants.materialHeightIndex=maps.height.SrvIndex();
    const auto& mapping=source.mapping;
    const auto rotation=XMMatrixRotationRollPitchYaw(XMConvertToRadians(mapping.rotationDegrees.x),XMConvertToRadians(mapping.rotationDegrees.y),XMConvertToRadians(mapping.rotationDegrees.z));
    XMStoreFloat4(&constants.mappingAxisX,rotation.r[0]); XMStoreFloat4(&constants.mappingAxisY,rotation.r[1]); XMStoreFloat4(&constants.mappingAxisZ,rotation.r[2]);
    constants.mappingAxisX.w=1.0f/mapping.repeatMeters; constants.mappingAxisY.w=mapping.sharpness;
    constants.mappingOffset=mapping.offset; constants.mappingMethod=static_cast<uint32_t>(mapping.method);
    if (source.appliedMaterials.size() > 8) { error="素材の重ね合わせは8段までです"; cleanup(); return false; }
    for (const auto& applied : source.appliedMaterials) {
        if (constants.appliedCount == 0) { FillApplied(constants.applied[constants.appliedCount++], applied, evaluator.Textures(), textures); continue; }
        auto& e = appliedEvaluators.emplace_back(std::make_unique<compositor::MaterialEvaluator>());
        if (!e->Create(device, sourceResolution, false)) { error="素材評価器を作成できません"; cleanup(); return false; }
        bool ok = false;
        if (!device.ExecuteImmediate([&](auto* list) { ok = e->Evaluate(device,pipelines,list,applied.stack,textures,materials,{{0,0,sourceResolution,sourceResolution}}); }) || !ok) {
            error="適用素材を評価できません"; cleanup(); return false;
        }
        FillApplied(constants.applied[constants.appliedCount++], applied, e->Textures(), textures);
    }
    for (uint32_t channel=0;channel<4;++channel) {
        constants.debugView=channel;
        void* mapped=nullptr; const D3D12_RANGE read{0,0};
        if (FAILED(constantsBuffer.resource->Map(0,&read,&mapped))) { error="ベイク定数を転送できません"; cleanup(); return false; }
        std::memcpy(mapped,&constants,sizeof(constants)); constantsBuffer.resource->Unmap(0,nullptr);
        if (!device.ExecuteImmediate([&](auto* list) {
            const float clear[4]={0,0,0,0};
            list->ClearRenderTargetView(target.rtv.cpu,clear,0,nullptr);
            list->OMSetRenderTargets(1,&target.rtv.cpu,FALSE,nullptr);
            const auto viewport=CD3DX12_VIEWPORT(0.0f,0.0f,float(width),float(height));
            const auto scissor=CD3DX12_RECT(0,0,LONG(width),LONG(height));
            list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
            list->SetGraphicsRootSignature(pipelines.GlobalRootSignature()); list->SetPipelineState(pipeline);
            list->SetGraphicsRootConstantBufferView(1,constantsBuffer.resource->GetGPUVirtualAddress());
            mesh.Draw(list);
        }) || !rhi::ReadTextureRgba8(device,target,images[channel])) {
            error="ベイク描画または読み戻しに失敗しました"; cleanup(); return false;
        }
    }
    cleanup(); return true;
}

float ExposureSettings::Ev100() const {
    if (automatic) {
        return std::clamp(autoEv100 + compensation, std::min(minEv100, maxEv100), std::max(minEv100, maxEv100));
    }
    if (useManualEv) {
        return manualEv100;
    }
    const float safeAperture = (aperture > 0.0f) ? aperture : 1.0f;
    const float safeShutter = (shutterSpeed > 0.0f) ? shutterSpeed : 1.0f;
    const float safeIso = (iso > 0.0f) ? iso : 100.0f;
    return std::log2((safeAperture * safeAperture) / safeShutter) - std::log2(safeIso / 100.0f);
}

float ExposureSettings::Exposure() const {
    return 1.0f / (1.2f * std::pow(2.0f, Ev100()));
}

XMFLOAT3 LightSettings::Direction() const {
    const float cosElevation = std::cos(elevation);
    return XMFLOAT3{cosElevation * std::sin(azimuth), std::sin(elevation),
                    cosElevation * std::cos(azimuth)};
}

bool PreviewRenderer::Initialize(rhi::Device& device, rhi::PipelineCache& pipelineCache) {
    m_camera.Frame({0.0f, 0.0f, 0.0f}, kReferenceGridRadius);
    if (!m_environment.Initialize(device, pipelineCache)) {
        return false;
    }

    // 実行ファイルに同梱する表示専用画像。カレントディレクトリには依存しない。
    std::wstring executable(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return false;
    executable.resize(length);
    const auto checkerPath = std::filesystem::path(executable).parent_path() / L"assets/textures/uv_checker.png";
    m_uvCheckerTexture = m_previewTextures.Load(device, pipelineCache, checkerPath);
    if (m_uvCheckerTexture == compositor::kNoTexture) return false;

    // 自動露出の測光バッファ。ヒストグラム 256 ビンと結果 4 要素、読み戻しはフレーム数ぶんの枠。
    if (!device.Allocator().CreateStructuredBuffer(256, sizeof(uint32_t), L"ExposureHistogram", m_meterHistogram, true) ||
        !device.Allocator().CreateStructuredBuffer(4, sizeof(float), L"ExposureMeterResult", m_meterResult, true) ||
        !device.Allocator().CreateReadbackBuffer(sizeof(float) * 4 * rhi::kFrameCount, L"ExposureMeterReadback",
                                                 m_meterReadback)) {
        return false;
    }
    return ResizeShadowMap(device, m_requestedShadowResolution, m_requestedShadowCascadeCount);
}

namespace {
void TransitionBuffer(ID3D12GraphicsCommandList* commandList, rhi::GpuBuffer& buffer, D3D12_RESOURCE_STATES state) {
    if (buffer.state == state) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = buffer.state;
    barrier.Transition.StateAfter = state;
    commandList->ResourceBarrier(1, &barrier);
    buffer.state = state;
}
void UavBarrier(ID3D12GraphicsCommandList* commandList, rhi::GpuBuffer& buffer) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = buffer.resource.Get();
    commandList->ResourceBarrier(1, &barrier);
}
}  // namespace

void PreviewRenderer::MeterExposure(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                    ID3D12GraphicsCommandList* commandList) {
    ID3D12PipelineState* clearPipeline = pipelineCache.GetCompute(L"ExposureMeter.hlsl", L"CsClear");
    ID3D12PipelineState* histogramPipeline = pipelineCache.GetCompute(L"ExposureMeter.hlsl", L"CsHistogram");
    ID3D12PipelineState* resolvePipeline = pipelineCache.GetCompute(L"ExposureMeter.hlsl", L"CsResolve");
    if (!clearPipeline || !histogramPipeline || !resolvePipeline || !m_meterHistogram.IsValid()) return;
    PIXBeginEvent(commandList, PIX_COLOR(220, 170, 60), "PreviewExposureMeter");
    struct MeterConstants {
        uint32_t sourceIndex, histogramIndex, resultIndex, width, height;
        float minLog2, rangeLog2, lowFraction, highFraction, calibration;
    };
    // 対数輝度 2^-16〜2^34 cd/m^2 を 256 ビンで覆う（約 0.2 段刻み）。
    // 暗い側の半分と明るい側の 2% を捨て、黒い地面や太陽の円盤に引っ張られないようにする。
    const MeterConstants constants{m_sceneColor.SrvIndex(), m_meterHistogram.uav.index, m_meterResult.uav.index,
                                   m_width, m_height, -16.0f, 50.0f, 0.5f, 0.02f, 12.5f};
    TransitionBuffer(commandList, m_meterHistogram, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionBuffer(commandList, m_meterResult, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
    commandList->SetPipelineState(clearPipeline);
    commandList->Dispatch(1, 1, 1);
    UavBarrier(commandList, m_meterHistogram);
    commandList->SetPipelineState(histogramPipeline);
    commandList->Dispatch(rhi::DispatchCount(m_width, 16), rhi::DispatchCount(m_height, 16), 1);
    UavBarrier(commandList, m_meterHistogram);
    commandList->SetPipelineState(resolvePipeline);
    commandList->Dispatch(1, 1, 1);
    TransitionBuffer(commandList, m_meterResult, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t slot = device.FrameIndex();
    commandList->CopyBufferRegion(m_meterReadback.resource.Get(), sizeof(float) * 4 * slot,
                                  m_meterResult.resource.Get(), 0, sizeof(float) * 4);
    m_meterPending[slot] = true;
    PIXEndEvent(commandList);
}

void PreviewRenderer::ReadExposureMeter(rhi::Device& device) {
    const uint32_t slot = device.FrameIndex();
    if (!m_meterPending[slot] || !m_meterReadback.IsValid()) return;
    m_meterPending[slot] = false;
    const SIZE_T offset = sizeof(float) * 4 * slot;
    const D3D12_RANGE readRange = {offset, offset + sizeof(float) * 4};
    void* mapped = nullptr;
    if (FAILED(m_meterReadback.resource->Map(0, &readRange, &mapped))) return;
    float values[4];
    std::memcpy(values, static_cast<const uint8_t*>(mapped) + offset, sizeof(values));
    const D3D12_RANGE writtenRange = {0, 0};
    m_meterReadback.resource->Unmap(0, &writtenRange);
    if (values[2] <= 0.0f || !std::isfinite(values[0])) return;
    const auto now = std::chrono::steady_clock::now();
    const float delta = m_meterTime.time_since_epoch().count() == 0
                            ? 0.0f
                            : std::clamp(std::chrono::duration<float>(now - m_meterTime).count(), 0.0f, 0.25f);
    m_meterTime = now;
    if (!m_exposure.autoValid) {
        // 最初の測光は即座に採用する。起動直後やスクリーンショットで暗いままにしない。
        m_exposure.autoEv100 = values[0];
        m_exposure.autoValid = true;
        return;
    }
    const float weight = 1.0f - std::exp(-std::max(m_exposure.adaptationSpeed, 0.0f) * delta);
    m_exposure.autoEv100 += (values[0] - m_exposure.autoEv100) * weight;
}

bool PreviewRenderer::ResizeShadowMap(rhi::Device& device, uint32_t resolution, uint32_t count) {
    rhi::TextureDesc shadowDesc;
    shadowDesc.width = resolution;
    shadowDesc.height = resolution;
    shadowDesc.format = DXGI_FORMAT_R32_TYPELESS;
    shadowDesc.dsvFormat = kShadowDsvFormat;
    shadowDesc.srvFormat = DXGI_FORMAT_R32_FLOAT;
    shadowDesc.allowDepthStencil = true;
    shadowDesc.createSrv = true;
    shadowDesc.clearDepth = 1.0f;
    shadowDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    shadowDesc.debugName = L"ShadowMap";
    std::array<rhi::GpuTexture, kShadowCascadeCount> replacements;
    for (uint32_t i = 0; i < count; ++i) {
        if (!device.Allocator().CreateTexture2D(shadowDesc, replacements[i])) {
            for (auto& allocated : replacements) device.DeferRelease(allocated);
            return false;
        }
    }
    // 必要な枚数を確保できてから切り替える。旧ビューもGPU完了後に解放する。
    for (auto& map : m_shadowMaps) device.DeferRelease(map);
    m_shadowMaps = std::move(replacements);
    m_shadowResolution = resolution;
    m_shadowCascadeCount = count;
    return true;
}

bool PreviewRenderer::SetGeneratedMeshScene(rhi::Device& device, const MeshScene& scene) {
    return UploadMeshScene(device, scene);
}

void PreviewRenderer::InvalidateSceneMaterials() {
    m_diagnostics.Invalidate();
    for (auto& material : m_sceneMaterials) {
        material.stack.MarkDirty();
        for (auto& layerStack : material.layerStacks) layerStack.MarkDirty();
    }
}

bool PreviewRenderer::IsEvaluating() const {
    for (const auto& material : m_sceneMaterials) {
        if (material.evaluator && material.evaluator->IsEvaluating()) return true;
        for (const auto& layer : material.layerEvaluators) {
            if (layer && layer->IsEvaluating()) return true;
        }
    }
    return false;
}

bool PreviewRenderer::UploadMeshScene(rhi::Device& device, const MeshScene& input) {
    auto scene = input;
    for (size_t i = 0; i < input.meshes.size(); ++i) {
        if (input.meshes[i].appliedMaterials.size() > 8) return false;
        scene.meshes[i].appliedSources.clear();
        for (const auto& applied : input.meshes[i].appliedMaterials) {
            if (scene.meshes[i].appliedSources.empty()) { scene.meshes[i].appliedSources.push_back(i); continue; }
            const size_t index = scene.meshes.size();
            SceneMesh entry; entry.materialOnly = true; entry.materialStack = applied.stack; entry.mapping = applied.mapping;
            scene.meshes.push_back(std::move(entry));
            scene.meshes[i].appliedSources.push_back(index);
        }
    }
    if (!ValidateMeshScene(scene)) return false;
    std::vector<Mesh> uploaded(scene.meshes.size());
    for (size_t i = 0; i < uploaded.size(); ++i) {
        if (scene.meshes[i].materialOnly) continue;
        if (!uploaded[i].Create(device, scene.meshes[i].geometry, L"SceneMesh")) {
            for (auto& mesh : uploaded) mesh.Release(device);
            return false;
        }
    }
    // 必要な評価器（スロット 1〜4）と道路マスクを先に確保し、失敗時は現在のシーンを保つ。
    struct Created {
        std::unique_ptr<compositor::MaterialEvaluator> base;
        std::array<std::unique_ptr<compositor::MaterialEvaluator>, 3> layers;
        rhi::GpuTexture roadMask;
        rhi::GpuTexture boundaryControl;
    };
    std::vector<Created> created(scene.meshes.size());
    const auto failCleanup = [&]() {
        for (auto& entry : created) {
            if (entry.base) entry.base->Destroy(device);
            for (auto& layer : entry.layers) if (layer) layer->Destroy(device);
            if (entry.roadMask.IsValid()) device.DeferRelease(entry.roadMask);
            if (entry.boundaryControl.IsValid()) device.DeferRelease(entry.boundaryControl);
        }
        for (auto& mesh : uploaded) mesh.Release(device);
        return false;
    };
    const auto ensure = [&](std::unique_ptr<compositor::MaterialEvaluator>& slot) {
        slot = std::make_unique<compositor::MaterialEvaluator>();
        return slot->Create(device, m_materialResolution);
    };
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const bool existing = i < m_sceneMaterials.size();
        if (scene.meshes[i].materialStack && !(existing && m_sceneMaterials[i].evaluator)) {
            if (!ensure(created[i].base)) return failCleanup();
        }
        for (size_t layer = 0; layer < 3; ++layer) {
            if (scene.meshes[i].layerStacks[layer] && !(existing && m_sceneMaterials[i].layerEvaluators[layer])) {
                if (!ensure(created[i].layers[layer])) return failCleanup();
            }
        }
        if (scene.meshes[i].boundaryControl.IsValid() &&
            !CreateRoadMaskTexture(device, scene.meshes[i].boundaryControl, created[i].boundaryControl)) return failCleanup();
        if (scene.meshes[i].roadMask.IsValid() &&
            !CreateRoadMaskTexture(device, scene.meshes[i].roadMask, created[i].roadMask)) {
            return failCleanup();
        }
    }
    // 作成・破棄はフレーム開始前。GPUリソースは遅延解放する。
    const auto destroyMaterial = [&](SceneMaterial& material) {
        if (material.evaluator) material.evaluator->Destroy(device);
        material.evaluator.reset();
        for (auto& layer : material.layerEvaluators) {
            if (layer) layer->Destroy(device);
            layer.reset();
        }
        if (material.roadMask.IsValid()) device.DeferRelease(material.roadMask);
        if (material.boundaryControl.IsValid()) device.DeferRelease(material.boundaryControl);
    };
    while (m_sceneMaterials.size() > scene.meshes.size()) {
        destroyMaterial(m_sceneMaterials.back());
        m_sceneMaterials.pop_back();
    }
    m_sceneMaterials.resize(scene.meshes.size());
    const auto assignStack = [](compositor::MaterialStack& target, const compositor::MaterialStack& source) {
        target.Layers() = source.Layers();
        target.SetTerrainScale(source.SizeMeters(), source.HeightMeters());
        target.MarkDirty();
    };
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        auto& target = m_sceneMaterials[i];
        const auto& source = scene.meshes[i].materialStack;
        if (!source) {
            if (target.evaluator) target.evaluator->Destroy(device);
            target.evaluator.reset();
        } else {
            if (created[i].base) target.evaluator = std::move(created[i].base);
            assignStack(target.stack, *source);
        }
        for (size_t layer = 0; layer < 3; ++layer) {
            const auto& layerSource = scene.meshes[i].layerStacks[layer];
            if (!layerSource) {
                if (target.layerEvaluators[layer]) target.layerEvaluators[layer]->Destroy(device);
                target.layerEvaluators[layer].reset();
            } else {
                if (created[i].layers[layer]) target.layerEvaluators[layer] = std::move(created[i].layers[layer]);
                assignStack(target.layerStacks[layer], *layerSource);
            }
        }
        if (target.roadMask.IsValid()) device.DeferRelease(target.roadMask);
        target.roadMask = std::move(created[i].roadMask);
        if (target.boundaryControl.IsValid()) device.DeferRelease(target.boundaryControl);
        target.boundaryControl = std::move(created[i].boundaryControl);
    }
    for (auto& mesh : m_sceneMeshes) mesh.Release(device);
    m_sceneMeshes = std::move(uploaded);
    // 全頂点を含むので複製しない。
    m_meshSceneRadius = MeshSceneRadius(scene);
    m_meshScene = std::move(scene);
    // 頂点走査はシーン更新時だけ。比較用の人は評価・書き出し用メッシュへ混ぜない。
    XMFLOAT3 minimum{FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool hasVertex = false;
    for (const auto& mesh : m_meshScene.meshes) {
        for (const auto& vertex : mesh.geometry.vertices) {
            hasVertex = true;
            minimum.x = std::min(minimum.x, vertex.position.x);
            minimum.y = std::min(minimum.y, vertex.position.y);
            minimum.z = std::min(minimum.z, vertex.position.z);
            maximum.x = std::max(maximum.x, vertex.position.x);
            maximum.z = std::max(maximum.z, vertex.position.z);
        }
    }
    m_humanScaleAnchor = hasVertex
        ? XMFLOAT3{maximum.x + 0.65f, minimum.y, (minimum.z + maximum.z) * 0.5f}
        : XMFLOAT3{3.0f, 0.0f, 0.0f};
    m_diagnostics.ResetScene(device);
    m_meshSceneEnabled = true;
    return true;
}

void PreviewRenderer::ClearMeshScene(rhi::Device& device) {
    m_diagnostics.ResetScene(device);
    for (auto& mesh : m_sceneMeshes) mesh.Release(device);
    m_sceneMeshes.clear();
    for (auto& material : m_sceneMaterials) {
        if (material.evaluator) material.evaluator->Destroy(device);
        for (auto& layer : material.layerEvaluators) if (layer) layer->Destroy(device);
        if (material.roadMask.IsValid()) device.DeferRelease(material.roadMask);
        if (material.boundaryControl.IsValid()) device.DeferRelease(material.boundaryControl);
    }
    m_sceneMaterials.clear();
    m_meshScene.meshes.clear();
    m_meshSceneEnabled = false;
    m_humanScaleAnchor = {3.0f, 0.0f, 0.0f};
}

void PreviewRenderer::Shutdown(rhi::Device& device) {
    m_previewTextures.Destroy(device);
    m_uvCheckerTexture = compositor::kNoTexture;
    m_diagnostics.Shutdown(device);
    ClearMeshScene(device);
    for (auto& map : m_shadowMaps) device.DeferRelease(map);
    m_environment.Shutdown(device);
    m_atmosphere.Shutdown(device);
    ReleaseTargets(device);
    device.DeferRelease(m_meterHistogram);
    device.DeferRelease(m_meterResult);
    device.DeferRelease(m_meterReadback);
    for (bool& pending : m_meterPending) pending = false;
}

void PreviewRenderer::ProcessPendingWork(rhi::Device& device,
                                        rhi::PipelineCache& pipelineCache) {
    if (!m_meshSceneEnabled && !m_sceneMeshes.empty()) ClearMeshScene(device);
    if (m_requestedShadowResolution != m_shadowResolution || m_requestedShadowCascadeCount != m_shadowCascadeCount) {
        if (!ResizeShadowMap(device, m_requestedShadowResolution, m_requestedShadowCascadeCount)) {
            m_requestedShadowResolution = m_shadowResolution;
            m_requestedShadowCascadeCount = m_shadowCascadeCount;
            ROCK_LOG_ERROR("影の設定を変更できませんでした。元の設定を維持します");
        } else ROCK_LOG_INFO("影の設定を %u 枚・解像度 %u に変更しました", m_shadowCascadeCount, m_shadowResolution);
    }

    // 合成解像度の変更。シーンの評価器（スロット 1〜4）を作り直す。
    // 新しく作る評価器（UploadMeshScene）は m_materialResolution を見るので、先に値を確定する。
    if (m_requestedMaterialResolution != m_materialResolution) {
        m_diagnostics.ResetScene(device);
        m_materialResolution = m_requestedMaterialResolution;
        for (auto& material : m_sceneMaterials) {
            if (material.evaluator && !material.evaluator->Resize(device, m_materialResolution))
                ROCK_LOG_WARN("マテリアルの解像度を変更できませんでした");
            for (auto& layer : material.layerEvaluators) {
                if (layer && !layer->Resize(device, m_materialResolution))
                    ROCK_LOG_WARN("レイヤーの解像度を変更できませんでした");
            }
        }
    }

    // シーンの空。太陽はライトの節（m_atmosphericLight）が持つので、大気の設定へ写してから作る。
    // 変わっていなければ何もしない（Atmosphere::Update が比べる）。
    if (m_atmosphericMode) {
        AtmosphereSettings settings = m_atmosphereSettings;
        settings.azimuth = m_atmosphericLight.azimuth;
        settings.elevation = m_atmosphericLight.elevation;
        settings.illuminance = m_atmosphericLight.illuminance;
        m_atmosphere.Update(device, pipelineCache, settings);
    }

    if (m_skyRebuildRequested) {
        m_skyRebuildRequested = false;
        m_skyLuminanceRebuildRequested = false;
        ApplyActiveSky(device, pipelineCache);
        return;
    }

    // 較正倍率だけの変更。equirect は読み込んだままのものを使うので速い。
    if (m_skyLuminanceRebuildRequested) {
        m_skyLuminanceRebuildRequested = false;
        if (!m_loadedHdriPath.empty()) {
            m_environment.RebuildWithSkyLuminance(device, pipelineCache,
                                                  m_activeSky.skyLuminance);
        }
    }
}

void PreviewRenderer::ResetSettings() {
    m_meshSceneEnabled = false;
    const PreviewDefaults& defaults = kPreviewDefaults;
    m_tonemap = defaults.tonemap;
    m_tessellationEnabled = defaults.tessellationEnabled;
    m_tessellationFactor = defaults.tessellationFactor;
    m_tessellationTargetPixels = defaults.tessellationTargetPixels;
    m_showSkybox = defaults.showSkybox;
    m_skyboxBlur = defaults.skyboxBlur;
    m_shadowEnabled = defaults.shadowEnabled;
    RequestShadowResolution(defaults.shadowResolution);
    RequestShadowCascadeCount(defaults.shadowCascadeCount);
    // 解像度の作り直しは GPU 待機を伴うので、要求だけ積む。
    RequestMaterialResolution(defaults.materialResolution);

    // 各節の既定値は構造体の初期値。数値を直接書かない。
    m_camera.SetState(CameraState{});
    m_camera.Frame({0.0f, 0.0f, 0.0f}, kReferenceGridRadius);
    m_light = LightSettings{};
    m_atmosphericMode = false;
    m_atmosphereSettings = AtmosphereSettings{};
    m_atmosphericLight = {m_atmosphereSettings.azimuth, m_atmosphereSettings.elevation,
                          m_atmosphereSettings.illuminance, {1.0f, 1.0f, 1.0f}};
    m_skylightIntensity = kDefaultSkylightIntensity;
    m_exposure = ExposureSettings{};
    m_dof = DofSettings{};
    m_ssao = SsaoSettings{};

    // 表示モードはプロジェクトに保存しないが、ここでは戻す。
    // ハイトやラフネスを覗いたまま「新規」を押すと、
    // 真っ白な球が出て「何も描かれていない」ように見えるため。
    m_debugView = DebugView::Shaded;
}

void PreviewRenderer::SetActiveSky(const SkyDefinition& sky) {
    if (NeedsEnvironmentRebuild(m_activeSky, sky)) {
        m_skyRebuildRequested = true;
    } else if (NeedsLuminanceRebuild(m_activeSky, sky)) {
        m_skyLuminanceRebuildRequested = true;
    }
    // IBL の倍率は毎フレームそのまま使うので、作り直しは要らない。
    m_activeSky = sky;
}

void PreviewRenderer::ApplyActiveSky(rhi::Device& device, rhi::PipelineCache& pipelineCache) {
    if (m_activeSky.source == SkySource::Hdri && !m_activeSky.hdriPath.empty()) {
        if (m_environment.BuildFromHdrFile(device, pipelineCache, m_activeSky.hdriPath,
                                           m_activeSky.skyLuminance)) {
            m_loadedHdriPath = m_activeSky.hdriPath;
            return;
        }
        // 読み込みに失敗しても、天球アセットの中身は書き換えない（ユーザーの
        // 指定を黙って消さない）。環境だけを手続き的な空へ落とす。
        ROCK_LOG_WARN("HDRI の読み込みに失敗したため、手続き的な空で描きます");
    }
    m_environment.BuildFromSky(device, pipelineCache, m_activeSky.procedural);
    m_loadedHdriPath.clear();
}

float PreviewRenderer::FocusDistance() const {
    // 軌道カメラなので、注視点までの距離がそのまま「見ているものまでの距離」。
    return m_dof.focusOnTarget ? m_camera.State().distance : m_dof.focusDistance;
}

// 現在のシーンを包む球の半径。カメラの Frame()（A キー）が使う。
// シーンが無ければ作業グリッドの半径（グリッドだけが見えている状態の基準）。
LightSettings PreviewRenderer::EffectiveLight() const {
    if (!m_atmosphericMode) return m_light;
    LightSettings result = m_atmosphericLight;
    AtmosphereSettings settings = m_atmosphereSettings;
    settings.elevation = result.elevation;
    result.color = AtmosphereSunTransmittance(settings);
    return result;
}

float PreviewRenderer::BoundingRadius() const {
    if (m_extraSceneRadius > 0.0f) {
        return std::max(m_meshSceneEnabled ? m_meshSceneRadius : 0.0f, m_extraSceneRadius);
    }
    return m_meshSceneEnabled ? m_meshSceneRadius : kReferenceGridRadius;
}

void PreviewRenderer::ReleaseTargets(rhi::Device& device) {
    rhi::GpuTexture* targets[] = {&m_sceneColor, &m_sceneColorDof, &m_sceneColorAo, &m_depth, &m_output};
    for (rhi::GpuTexture* target : targets) {
        if (!target->IsValid()) {
            continue;
        }
        // ディスクリプタも含めてフレーム同期後に解放する。
        device.DeferRelease(*target);
    }
    m_width = 0;
    m_height = 0;
}

bool PreviewRenderer::CopyOutputTo(ID3D12GraphicsCommandList* commandList, rhi::GpuTexture& destination) {
    if (!m_output.IsValid() || !destination.IsValid() || destination.width != m_output.width ||
        destination.height != m_output.height || destination.format != m_output.format) return false;
    PIXBeginEvent(commandList, PIX_COLOR(120, 200, 200), "LayerMaterialThumbnailCopy");
    const auto previous = m_output.state;
    TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionIfNeeded(commandList, destination, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(destination.resource.Get(), m_output.resource.Get());
    TransitionIfNeeded(commandList, destination, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_output, previous);
    PIXEndEvent(commandList);
    return true;
}

bool PreviewRenderer::SaveOutputToPng(rhi::Device& device, const std::filesystem::path& path, uint32_t maxSize) {
    if (!m_output.IsValid()) {
        return false;
    }
    return rhi::SaveTextureToPng(device, m_output, path, maxSize);
}

bool PreviewRenderer::Resize(rhi::Device& device, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    if (width == m_width && height == m_height) {
        return true;
    }

    // 作り直す前に、GPU がまだ参照しているターゲットを解放できる状態にする。
    device.WaitForGpu();
    ReleaseTargets(device);
    m_diagnostics.ResetScene(device);

    rhi::TextureDesc colorDesc;
    colorDesc.width = width;
    colorDesc.height = height;
    colorDesc.format = kSceneColorFormat;
    colorDesc.allowRenderTarget = true;
    colorDesc.createSrv = true;
    colorDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    colorDesc.debugName = L"SceneColor";
    if (!device.Allocator().CreateTexture2D(colorDesc, m_sceneColor)) {
        return false;
    }

    // 被写界深度の出力。シーンカラーと同じ形式で、コンピュートから書く。
    rhi::TextureDesc dofDesc;
    dofDesc.width = width;
    dofDesc.height = height;
    dofDesc.format = kSceneColorFormat;
    dofDesc.allowUnorderedAccess = true;
    dofDesc.createSrv = true;
    dofDesc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dofDesc.debugName = L"SceneColorAo";
    if (!device.Allocator().CreateTexture2D(dofDesc, m_sceneColorAo)) return false;
    dofDesc.debugName = L"SceneColorDof";
    if (!device.Allocator().CreateTexture2D(dofDesc, m_sceneColorDof)) {
        return false;
    }

    // **被写界深度が深度を読むので SRV も張る。** 深度として書き、SRV としても
    // 読むため TYPELESS で作る（シャドウマップと同じ作法）。
    rhi::TextureDesc depthDesc;
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.dsvFormat = kDepthFormat;
    depthDesc.srvFormat = DXGI_FORMAT_R32_FLOAT;
    depthDesc.allowDepthStencil = true;
    depthDesc.createSrv = true;
    depthDesc.clearDepth = 1.0f;
    depthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    depthDesc.debugName = L"SceneDepth";
    if (!device.Allocator().CreateTexture2D(depthDesc, m_depth)) {
        return false;
    }

    rhi::TextureDesc outputDesc;
    outputDesc.width = width;
    outputDesc.height = height;
    outputDesc.format = kOutputFormat;
    outputDesc.allowUnorderedAccess = true;
    // トーンマップ（コンピュート）後に、深度テスト付きのガイド線を
    // グラフィックスパスで重ねるため、RTV としても使えるようにする。
    outputDesc.allowRenderTarget = true;
    outputDesc.createSrv = true;
    outputDesc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outputDesc.debugName = L"PreviewOutput";
    if (!device.Allocator().CreateTexture2D(outputDesc, m_output)) {
        return false;
    }

    // RTV フラグ付きのリソースは、最初に Clear / Discard / Copy で初期化しないと
    // デバッグレイヤーが「未初期化のまま描画に使った」というエラーを出す
    // （NOT_ZEROED ヒープの規則）。中身はトーンマップが毎フレーム全画素を
    // 書き潰すので、Discard で十分。
    device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->DiscardResource(m_output.resource.Get(), nullptr);
        TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    });

    m_width = width;
    m_height = height;
    m_camera.SetViewportSize(width, height);
    return true;
}

void PreviewRenderer::Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                             ID3D12GraphicsCommandList* commandList,
                             const compositor::TextureLibrary& textures,
                             const compositor::MaterialLibrary& materials) {
    if (!m_sceneColor.IsValid() || !m_output.IsValid()) {
        return;
    }
    // チェッカーは照明付きで確認する。選択していたチャンネル表示の設定は変更しない。
    const DebugView displayView = m_showUvChecker ? DebugView::Shaded : m_debugView;
    // この枠の前回の測光値は、コマンドアロケータの再利用時点で完了している。
    ReadExposureMeter(device);

    // 軌道の距離とクリップ面を被写体の大きさへ合わせる。**毎フレーム渡してよい。**
    // シーンはいつでも差し替わるので、描く直前に見るのが確実。
    m_camera.SetSceneRadius(BoundingRadius());

    // 描画の量はフレームごとに数え直す。**描くところで足す**ので、
    // パスを増やしたときに数え漏らしても、増やした本人が気づきやすい。
    m_stats = RenderStats{};

    // シーンの材質（スロット 1〜4）に変更があれば評価を投入し、終わった評価があれば結果を受け取る。
    // 評価はコンピュートキューで走るので、このフレームは前回の結果を描く。
    if (m_meshSceneEnabled) {
        for (auto& material : m_sceneMaterials) {
            // 道路マスクは転送直後は COPY_DEST。頂点 / ドメイン / ピクセルで読むので両方の読み取り状態へ。
            if (material.boundaryControl.IsValid()) {
                TransitionIfNeeded(commandList, material.boundaryControl,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (material.roadMask.IsValid()) {
                TransitionIfNeeded(commandList, material.roadMask,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (material.evaluator)
                material.evaluator->Update(device, pipelineCache, commandList, material.stack,
                                           textures, materials);
            for (size_t layer = 0; layer < 3; ++layer) {
                if (material.layerEvaluators[layer])
                    material.layerEvaluators[layer]->Update(device, pipelineCache, commandList,
                                                            material.layerStacks[layer], textures, materials);
            }
        }
    }

    rhi::GraphicsPipelineDesc meshPipelineDesc;
    meshPipelineDesc.shaderPath = L"MeshPbr.hlsl";
    meshPipelineDesc.vertexEntry = L"VsMain";
    meshPipelineDesc.pixelEntry = L"PsMain";
    meshPipelineDesc.rtvFormat = kSceneColorFormat;
    meshPipelineDesc.dsvFormat = kDepthFormat;
    meshPipelineDesc.layout = rhi::VertexLayout::MeshStandard;
    meshPipelineDesc.cullMode = D3D12_CULL_MODE_BACK;
    // テセレーションを使うときは、頂点シェーダを制御点の出力だけに差し替える。
    // 分割量は画面上の辺の長さで決まる。
    const bool useTessellation = m_tessellationEnabled;
    if (useTessellation) {
        meshPipelineDesc.vertexEntry = L"VsControl";
        meshPipelineDesc.hullEntry = L"HsMain";
        meshPipelineDesc.domainEntry = L"DsMain";
    }
    // ワイヤーフレーム表示のときだけラスタライザを切り替える。
    if (displayView == DebugView::Wireframe) {
        meshPipelineDesc.fillMode = D3D12_FILL_MODE_WIREFRAME;
    }

    ID3D12PipelineState* meshPipeline = pipelineCache.GetGraphics(meshPipelineDesc);
    ID3D12PipelineState* tonemapPipeline =
        pipelineCache.GetCompute(L"TonemapPass.hlsl", L"CsMain");
    if (meshPipeline == nullptr || tonemapPipeline == nullptr) {
        return;
    }

    m_diagnostics.Begin(device, commandList);
    D3D12_GPU_VIRTUAL_ADDRESS probeConstants = 0;
    size_t probeMesh = 0;

    // DirectXMath は行ベクトル規約、HLSL の行列は既定で列優先。
    // XMMATRIX をそのまま積むと HLSL 側では転置として解釈され、
    // mul(matrix, vector) が意図どおりの結果になる。転置は入れない。
    const XMMATRIX view = m_camera.ViewMatrix();
    const XMMATRIX projection = m_camera.ProjectionMatrix();
    const XMMATRIX viewProjection = XMMatrixMultiply(view, projection);

    // シーンのメッシュは世界座標（m）で受け取るので、モデル行列は単位行列。
    MeshConstants constants = {};
    XMStoreFloat4x4(&constants.viewProjection, viewProjection);
    XMStoreFloat4x4(&constants.view, view);
    XMStoreFloat4x4(&constants.model, XMMatrixIdentity());
    XMStoreFloat4x4(&constants.normalMatrix, XMMatrixIdentity());

    constants.cameraPosition = m_camera.Position();
    // シーンの空では、太陽は大気を通った色と照度、環境は大気の環境（作業用IBLとは別）。
    const LightSettings light = EffectiveLight();
    const Environment& environment = GetEnvironment();
    constants.lightDirection = light.Direction();
    constants.lightIlluminance = light.illuminance;
    constants.lightColor = light.color;
    constants.iblIntensity = environment.IsReady() ? EnvironmentIntensity() : 0.0f;
    constants.prefilteredMipCount = environment.PrefilteredMipCount();
    constants.irradianceIndex = environment.IrradianceSrvIndex();
    constants.prefilteredIndex = environment.PrefilteredSrvIndex();
    constants.brdfLutIndex = environment.BrdfLutSrvIndex();

    // 材質（合成結果）と変位はメッシュごとに決める（下の drawMeshes）。ここでは無しにしておく。
    constants.useMaterialTextures = 0u;
    constants.displacementScale = 0.0f;
    constants.debugView = static_cast<uint32_t>(displayView);
    constants.meshDisplayFlags = m_showUvChecker ? kMeshFlagUvChecker : 0u;
    constants.uvCheckerIndex = m_previewTextures.SrvIndex(m_uvCheckerTexture, true);
    // 分割量はカメラから見た見え方で決める。本描画では viewProjection と同一で、
    // シャドウパスだけが viewProjection 側を上書きして分岐する。
    XMStoreFloat4x4(&constants.tessellationViewProjection, viewProjection);
    constants.viewportSize[0] = static_cast<float>(m_width);
    constants.viewportSize[1] = static_cast<float>(m_height);
    constants.tessellationMaxFactor = m_tessellationFactor;
    constants.tessellationTargetPixels = m_tessellationTargetPixels;

    // メッシュの合成モード。材質の属性から決める。帯（白線）以外は常に不透明。
    const auto blendModeOf = [&](size_t i) {
        if (!m_meshScene.meshes[i].useBlendMode) return compositor::BlendMode::Opaque;
        const compositor::MaterialAsset* asset = materials.Find(m_meshScene.meshes[i].blendMaterial);
        return asset ? asset->blendMode : compositor::BlendMode::Opaque;
    };
    // 各メッシュは世界座標で受け取る。
    // passMask: kPassOpaque = 路面など、kPassDecal = 路面に貼る帯（白線。深度バイアス付き）、
    // kPassTranslucent = 半透明の帯。ワイヤーフレームには半透明の帯も含める。
    constexpr uint32_t kPassOpaque = 1u;
    constexpr uint32_t kPassDecal = 2u;
    constexpr uint32_t kPassTranslucent = 4u;
    const auto passOf = [&](size_t i) {
        if (blendModeOf(i) == compositor::BlendMode::Translucent) return kPassTranslucent;
        return m_meshScene.meshes[i].useBlendMode ? kPassDecal : kPassOpaque;
    };
    // シーンが無ければ何も描かない（m_sceneMeshes が空）。背景とグリッドだけが出る。
    // outlineMesh が 0 以上なら、そのメッシュの外周だけを LINELIST で描く（ホバー / 選択の枠）。
    const auto drawMeshes = [&](const MeshConstants& passConstants, uint32_t passMask, bool tessellate,
                                int outlineMesh = -1) {
        if (m_meshSceneHidden) return;
        for (size_t i = 0; i < m_sceneMeshes.size(); ++i) {
            if (outlineMesh >= 0 && i != static_cast<size_t>(outlineMesh)) continue;
            if (m_meshScene.meshes[i].materialOnly) continue;
            MeshConstants drawConstants = passConstants;
            const Mesh& drawMesh = m_sceneMeshes[i];
            const compositor::BlendMode blendMode = blendModeOf(i);
            if ((passOf(i) & passMask) == 0u) continue;
            if (blendMode != compositor::BlendMode::Opaque) {
                const compositor::MaterialAsset* asset = materials.Find(m_meshScene.meshes[i].blendMaterial);
                drawConstants.opacityMode = static_cast<uint32_t>(blendMode);
                drawConstants.opacityThreshold = asset ? asset->maskThreshold : 0.5f;
            }
            drawConstants.roadMetersPerUv = m_meshScene.meshes[i].roadMetersPerUv;
            drawConstants.displacementScale = m_meshScene.meshes[i].displacementMeters;
            drawConstants.additiveHeightMeters = m_meshScene.meshes[i].additiveHeightMeters;
            drawConstants.surfaceDepthBiasMeters = m_meshScene.meshes[i].surfaceDepthBiasMeters;
            // レイヤー（スロット 1〜4）と道路マスク。白線は押し出し元の道路面のものを写す。
            const int source = m_meshScene.meshes[i].displacementSource;
            const size_t layerSource = (source >= 0 && static_cast<size_t>(source) < m_sceneMaterials.size())
                                           ? static_cast<size_t>(source) : i;
            {
                const auto& lm = m_meshScene.meshes[layerSource];
                drawConstants.connectionPrototype = lm.connectionPrototype ? 1u : 0u;
                const auto& lsm = m_sceneMaterials[layerSource];
                for (int slot = 0; slot < 4; ++slot) {
                    drawConstants.layerBaseColorIndex[slot] = kNoShadowIndex;
                    drawConstants.layerNormalIndex[slot] = kNoShadowIndex;
                    drawConstants.layerSurfaceIndex[slot] = kNoShadowIndex;
                    drawConstants.layerHeightIndex[slot] = kNoShadowIndex;
                    drawConstants.layerWorldUv[slot] = lm.layerWorldUv[static_cast<size_t>(slot)] ? 1u : 0u;
                    drawConstants.layerUvRepeat[slot] = slot == 0 ? lm.roadMetersPerUv : lm.layerUvRepeat[static_cast<size_t>(slot)];
                    drawConstants.layerHeightGate[slot] = slot == 0 ? 0u : lm.layerHeightGate[static_cast<size_t>(slot)];
                    drawConstants.layerHeightGateThreshold[slot] = lm.layerHeightGateThreshold[static_cast<size_t>(slot)];
                    drawConstants.layerHeightGateSoftness[slot] = lm.layerHeightGateSoftness[static_cast<size_t>(slot)];
                    drawConstants.layerBlendMode[slot] = lm.layerBlendMode[static_cast<size_t>(slot)];
                    drawConstants.layerDisplacementMeters[slot] = lm.layerDisplacementMeters[static_cast<size_t>(slot)];
                    const compositor::MaterialEvaluator* evaluator =
                        slot == 0 ? lsm.evaluator.get() : lsm.layerEvaluators[static_cast<size_t>(slot - 1)].get();
                    if (evaluator && evaluator->EvaluatedRevision() != 0 && evaluator->Textures().IsValid()) {
                        const auto& maps = evaluator->Textures();
                        drawConstants.layerBaseColorIndex[slot] = maps.baseColor.SrvIndex();
                        drawConstants.layerNormalIndex[slot] = maps.normal.SrvIndex();
                        drawConstants.layerSurfaceIndex[slot] = maps.surface.SrvIndex();
                        drawConstants.layerHeightIndex[slot] = maps.height.SrvIndex();
                    }
                }
                const bool baseReady = drawConstants.layerHeightIndex[0] != kNoShadowIndex;
                drawConstants.layerCount = baseReady ? 4u : 0u;
                drawConstants.roadMaskIndex = lsm.roadMask.IsValid() ? lsm.roadMask.SrvIndex() : kNoShadowIndex;
                drawConstants.layerBlendRange = lm.layerBlendRange;
                drawConstants.roadUvAlongU = lm.roadUvAlongU ? 1u : 0u;
                drawConstants.roadMaskScale[0] = lm.roadWidthMeters > 0.0f ? 1.0f / lm.roadWidthMeters : 0.0f;
                drawConstants.roadMaskScale[1] = lm.roadLengthMeters > 0.0f ? 1.0f / lm.roadLengthMeters : 0.0f;
                drawConstants.roadUvMetersPerUv = lm.roadMetersPerUv;
                // 道路面自身はレイヤーで陰影を付ける。白線は自分の材質で描き、押し出しだけ道路に合わせる。
                drawConstants.shadeLayers = (layerSource == i && baseReady && drawConstants.roadMaskIndex != kNoShadowIndex) ? 1u : 0u;
                if (!baseReady) drawConstants.displacementScale = 0.0f;
            }
            const auto& connection = m_meshScene.meshes[layerSource];
            const auto& control = m_sceneMaterials[layerSource].boundaryControl;
            drawConstants.boundaryControlIndex = control.IsValid() ? control.SrvIndex() : kNoShadowIndex;
            drawConstants.boundaryFrameSign = connection.connectionFrameSign;
            drawConstants.boundaryCount = 0;
            for (size_t boundary = 0; boundary < connection.boundaries.size(); ++boundary) {
                const auto& input = connection.boundaries[boundary];
                const auto& settings = input.material;
                if (settings.id) drawConstants.boundaryCount = static_cast<uint32_t>(boundary + 1);
                auto& output = drawConstants.boundaries[boundary];
                output.mask = textures.SrvIndex(settings.mask, false);
                output.height = textures.SrvIndex(settings.height, false);
                output.alongU = settings.alongU; output.invertMask = settings.invertMask;
                output.center = input.center; output.acrossSign = input.acrossSign;
                output.width = settings.widthMeters; output.repeat = settings.repeatMeters;
                output.depth = settings.depthMeters; output.heightCenter = settings.heightCenter;
            }
            drawConstants.connectionHeightFade[0] = connection.connectionHeightFade.x;
            drawConstants.connectionHeightFade[1] = connection.connectionHeightFade.y;
            if (connection.connectionSources[0] >= 0) {
                drawConstants.connectionContextCount = connection.connectionRoadMixSource >= 0 ? 7 : connection.connectionExtraSources[0] >= 0 ? 5 : 3;
                if (connection.connectionRoadMixSource >= 0) {
                    const auto& mask = m_sceneMaterials[connection.connectionRoadMixSource].roadMask;
                    if (!mask.IsValid()) continue;
                    drawConstants.connectionRoadMixIndex = mask.SrvIndex();
                }
                drawConstants.connectionSecondHeightFade[0] = connection.connectionSecondHeightFade.x;
                drawConstants.connectionSecondHeightFade[1] = connection.connectionSecondHeightFade.y;
                bool ready = true;
                for (size_t context = 0; context < drawConstants.connectionContextCount; ++context) {
                    const int contextSource = context < 3 ? connection.connectionSources[context] : context < 5
                        ? connection.connectionExtraSources[context - 3] : connection.connectionRoadSources[context - 5];
                    const size_t index = static_cast<size_t>(contextSource >= 0 ? contextSource : connection.connectionSources[0]);
                    const float acrossSign = context < 3 ? connection.connectionAcrossSigns[context] : context < 5
                        ? connection.connectionExtraSigns[context - 3] : connection.connectionAcrossSigns[0];
                    const auto origin = context < 3 ? connection.connectionOrigins[context] : context < 5
                        ? connection.connectionExtraOrigins[context - 3] : connection.connectionOrigins[0];
                    const auto& cpu = m_meshScene.meshes[index];
                    const auto& gpu = m_sceneMaterials[index];
                    auto& c = drawConstants.connectionContexts[context];
                    for (size_t slot = 0; slot < 4; ++slot) {
                        c.layerBaseColorIndex[slot] = c.layerNormalIndex[slot] =
                            c.layerSurfaceIndex[slot] = c.layerHeightIndex[slot] = kNoShadowIndex;
                        const auto* evaluation = slot == 0 ? gpu.evaluator.get() : gpu.layerEvaluators[slot - 1].get();
                        if (evaluation && evaluation->EvaluatedRevision() != 0 && evaluation->Textures().IsValid()) {
                            const auto& maps = evaluation->Textures();
                            c.layerBaseColorIndex[slot] = maps.baseColor.SrvIndex();
                            c.layerNormalIndex[slot] = maps.normal.SrvIndex();
                            c.layerSurfaceIndex[slot] = maps.surface.SrvIndex();
                            c.layerHeightIndex[slot] = maps.height.SrvIndex();
                        } else if (slot == 0 || cpu.layerStacks[slot - 1]) {
                            ready = false;
                        }
                        c.layerWorldUv[slot] = cpu.layerWorldUv[slot] ? 1u : 0u;
                        c.layerUvRepeat[slot] = cpu.layerUvRepeat[slot];
                        c.layerHeightGate[slot] = slot == 0 ? 0u : cpu.layerHeightGate[slot];
                        c.layerHeightGateThreshold[slot] = cpu.layerHeightGateThreshold[slot];
                        c.layerHeightGateSoftness[slot] = cpu.layerHeightGateSoftness[slot];
                        c.layerBlendMode[slot] = cpu.layerBlendMode[slot];
                    }
                    c.roadMaskIndex = gpu.roadMask.IsValid() ? gpu.roadMask.SrvIndex() : kNoShadowIndex;
                    c.layerBlendRange = cpu.layerBlendRange;
                    // bit0: UV軸交換、bit1: 素材の幅反転、bit2: 描画接線に対する法線X反転。
                    c.roadUvAlongU = (cpu.roadUvAlongU ? 1u : 0u) |
                        (acrossSign < 0 ? 2u : 0u) |
                        (acrossSign * connection.connectionFrameSign < 0 ? 4u : 0u);
                    c.displacementMeters = cpu.displacementMeters;
                    c.roadMaskScale[0] = cpu.roadWidthMeters > 0 ? 1.0f / cpu.roadWidthMeters : 0;
                    c.roadMaskScale[1] = cpu.roadLengthMeters > 0 ? 1.0f / cpu.roadLengthMeters : 0;
                    c.origin[0] = origin.x;
                    c.origin[1] = origin.y;
                }
                // 評価途中の欠落した材質を混ぜず、全入力が揃ったフレームから描く。
                if (!ready) continue;
                drawConstants.layerCount = 4;
                drawConstants.shadeLayers = layerSource == i ? 1u : 0u;
                if (layerSource == i) drawConstants.useMaterialTextures = 1;
                drawConstants.displacementScale = connection.displacementMeters;
            }
            const auto& material = m_meshScene.meshes[i].material;
            drawConstants.baseColor = material.baseColor;
            drawConstants.roughness = material.roughness;
            drawConstants.metallic = material.metallic;
            const auto& mapping = m_meshScene.meshes[i].mapping;
            const auto rotation = XMMatrixRotationRollPitchYaw(XMConvertToRadians(mapping.rotationDegrees.x),
                XMConvertToRadians(mapping.rotationDegrees.y), XMConvertToRadians(mapping.rotationDegrees.z));
            XMStoreFloat4(&drawConstants.mappingAxisX, rotation.r[0]);
            XMStoreFloat4(&drawConstants.mappingAxisY, rotation.r[1]);
            XMStoreFloat4(&drawConstants.mappingAxisZ, rotation.r[2]);
            drawConstants.mappingAxisX.w = 1.0f / mapping.repeatMeters;
            drawConstants.mappingAxisY.w = mapping.sharpness;
            drawConstants.mappingOffset = mapping.offset;
            drawConstants.mappingMethod = static_cast<uint32_t>(mapping.method);
            const auto& evaluator = m_sceneMaterials[i].evaluator;
            if (evaluator && evaluator->EvaluatedRevision() != 0 && evaluator->Textures().IsValid()) {
                const auto& maps = evaluator->Textures();
                drawConstants.useMaterialTextures = 1u;
                drawConstants.materialBaseColorIndex = maps.baseColor.SrvIndex();
                drawConstants.materialNormalIndex = maps.normal.SrvIndex();
                drawConstants.materialSurfaceIndex = maps.surface.SrvIndex();
                drawConstants.materialHeightIndex = maps.height.SrvIndex();
            }
            drawConstants.appliedCount = 0;
            const auto& appliedMesh = m_meshScene.meshes[i];
            for (size_t j = 0; j < appliedMesh.appliedSources.size(); ++j) {
                const auto& eval = m_sceneMaterials[appliedMesh.appliedSources[j]].evaluator;
                if (!eval || !eval->EvaluatedRevision() || !eval->Textures().IsValid()) { drawConstants.appliedCount = 0; break; }
                FillApplied(drawConstants.applied[j], appliedMesh.appliedMaterials[j], eval->Textures(), textures);
                ++drawConstants.appliedCount;
            }
            const auto allocation = device.Upload().Allocate(sizeof(MeshConstants), 256);
            if (!allocation.IsValid()) continue;
            std::memcpy(allocation.cpu, &drawConstants, sizeof(drawConstants));
            commandList->SetGraphicsRootConstantBufferView(1, allocation.gpuAddress);
            if (drawConstants.connectionContextCount != 0 && probeConstants == 0) {
                probeConstants = allocation.gpuAddress;
                probeMesh = i;
            }
            if (outlineMesh >= 0) {
                drawMesh.DrawOutline(commandList);
                ++m_stats.drawCalls;
                continue;
            }
            drawMesh.Draw(commandList, tessellate);
            CountMeshDraw(m_stats, drawMesh, tessellate);
        }
    };

    // --- シャドウマップ ----------------------------------------------------
    // ライトから深度だけを描く。同じ頂点シェーダを通るので、
    // ディスプレイスメントで押し出した形がそのまま影になる。
    for (auto& index : constants.shadowIndices) index = kNoShadowIndex;
    constants.shadowTexelSize = 1.0f / static_cast<float>(m_shadowResolution);
    constants.shadowBlend = kShadowCascadeBlend;
    constants.shadowCascadeCount = m_shadowCascadeCount;

    // 描くものが無ければシャドウパスも走らせない。
    const bool drawExtras = drawSceneExtras && m_extraSceneRadius > 0.0f && IsShadedView(displayView);
    if (m_shadowEnabled && m_shadowMaps[0].IsValid() && (!m_sceneMeshes.empty() || drawExtras)) {
        float displacementMargin = 0;
        for (const auto& mesh : m_meshScene.meshes)
            displacementMargin = std::max(displacementMargin, std::abs(mesh.displacementMeters));
        const auto cascades = BuildShadowCascades(m_camera, light.Direction(), BoundingRadius() + displacementMargin,
            float(m_width) / float(std::max(m_height, 1u)), m_shadowResolution, m_shadowCascadeCount);
        constants.shadowNear = cascades.nearDistance;
        for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
            constants.lightViewProjections[i] = cascades.matrices[i];
            constants.shadowSplits[i] = cascades.splits[i];
            constants.shadowBiases[i] = cascades.biases[i];
        }

        rhi::GraphicsPipelineDesc shadowPipelineDesc;
        shadowPipelineDesc.shaderPath = L"MeshPbr.hlsl";
        shadowPipelineDesc.vertexEntry = L"VsMain";
        // ピクセルシェーダは要らない。深度だけ書く。
        shadowPipelineDesc.dsvFormat = kShadowDsvFormat;
        shadowPipelineDesc.layout = rhi::VertexLayout::MeshStandard;
        // 路面のような片面のメッシュも影を落とすので、両面を描く。
        shadowPipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
        // 本描画と同じ分割で描く。違う形を影にすると自己遮蔽がずれる。
        if (useTessellation) {
            shadowPipelineDesc.vertexEntry = L"VsControl";
            shadowPipelineDesc.hullEntry = L"HsMain";
            shadowPipelineDesc.domainEntry = L"DsMain";
        }

        ID3D12PipelineState* shadowPipeline = pipelineCache.GetGraphics(shadowPipelineDesc);

        if (shadowPipeline != nullptr) {
            for (uint32_t cascade = 0; cascade < m_shadowCascadeCount; ++cascade) {
                auto& shadowMap = m_shadowMaps[cascade];
                // ライトから見た行列で描く。ほかの値は本描画と同じ。
                MeshConstants shadowConstants = constants;
                shadowConstants.viewProjection = cascades.matrices[cascade];

                PIXBeginEvent(commandList, PIX_COLOR(220, 200, 120), "PreviewShadowCascade %u", cascade);
                TransitionIfNeeded(commandList, shadowMap, D3D12_RESOURCE_STATE_DEPTH_WRITE);

                const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = shadowMap.dsv.cpu;
                commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
                commandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                                                   nullptr);

                const auto shadowViewport =
                    CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(m_shadowResolution),
                                     static_cast<float>(m_shadowResolution));
                const auto shadowScissor = CD3DX12_RECT(0, 0, static_cast<LONG>(m_shadowResolution),
                                                        static_cast<LONG>(m_shadowResolution));
                commandList->RSSetViewports(1, &shadowViewport);
                commandList->RSSetScissorRects(1, &shadowScissor);

                commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
                commandList->SetPipelineState(shadowPipeline);
                // 路面に貼る白線・ひび割れ・Decalは影を受けるだけにする。
                // 深度だけのパスへ含めると透明マスクが無視され、帯全体が路面を遮光する。
                drawMeshes(shadowConstants, kPassOpaque, useTessellation);
                if (drawExtras) {
                    SceneDrawContext shadowContext;
                    shadowContext.shadowPass = true;
                    shadowContext.dsvFormat = kShadowDsvFormat;
                    shadowContext.viewProjection = cascades.matrices[cascade];
                    drawSceneExtras(commandList, shadowContext);
                }

                TransitionIfNeeded(commandList, shadowMap,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                PIXEndEvent(commandList);

                constants.shadowIndices[cascade] = shadowMap.SrvIndex();
            }
        }
    }

    PIXBeginEvent(commandList, PIX_COLOR(80, 200, 120), "PreviewScene");

    TransitionIfNeeded(commandList, m_sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_sceneColor.rtv.cpu;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depth.dsv.cpu;
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(m_width),
                                           static_cast<float>(m_height));
    const auto scissor = CD3DX12_RECT(0, 0, static_cast<LONG>(m_width),
                                      static_cast<LONG>(m_height));
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);


    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(meshPipeline);
    drawMeshes(constants, kPassOpaque, useTessellation);
    // 配置したモデル。不透明の道路の後、路面に貼る帯と半透明の帯の前に描く。
    if (drawExtras) {
        PIXBeginEvent(commandList, PIX_COLOR(120, 200, 200), "PreviewSceneExtras");
        SceneDrawContext context;
        context.rtvFormat = kSceneColorFormat;
        context.dsvFormat = kDepthFormat;
        context.viewProjection = constants.viewProjection;
        context.view = constants.view;
        context.cameraPosition = m_camera.Position();
        for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
            context.lightViewProjections[i] = constants.lightViewProjections[i];
            context.shadowIndices[i] = constants.shadowIndices[i];
            context.shadowSplits[i] = constants.shadowSplits[i];
            context.shadowBiases[i] = constants.shadowBiases[i];
        }
        context.shadowTexelSize = constants.shadowTexelSize;
        context.shadowBlend = constants.shadowBlend;
        context.shadowNear = constants.shadowNear;
        context.shadowCascadeCount = constants.shadowCascadeCount;
        context.environment = &environment;
        context.iblIntensity = EnvironmentIntensity();
        context.lightDirection = light.Direction();
        context.lightIlluminance = light.illuminance;
        context.lightColor = light.color;
        drawSceneExtras(commandList, context);
        commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
        commandList->SetPipelineState(meshPipeline);
        PIXEndEvent(commandList);
    }
    if (m_meshSceneEnabled) {
        uint32_t passes = 0u;
        for (size_t i = 0; i < m_sceneMeshes.size(); ++i) passes |= passOf(i);
        // 路面に貼る帯は深度バイアスで手前へ寄せ、路面の分割との差で石が突き抜けないようにする。
        if (passes & kPassDecal) {
            rhi::GraphicsPipelineDesc decalDesc = meshPipelineDesc;
            decalDesc.depthBias = kDecalDepthBias;
            decalDesc.slopeScaledDepthBias = kDecalSlopeScaledDepthBias;
            if (ID3D12PipelineState* decalPipeline = pipelineCache.GetGraphics(decalDesc)) {
                commandList->SetPipelineState(decalPipeline);
                drawMeshes(constants, kPassDecal, useTessellation);
            }
        }
        // 半透明の帯は最後にアルファ合成で描く。深度は読むだけ。
        if (passes & kPassTranslucent) {
            rhi::GraphicsPipelineDesc blendDesc = meshPipelineDesc;
            blendDesc.alphaBlend = true;
            blendDesc.depthWrite = false;
            blendDesc.depthBias = kDecalDepthBias;
            blendDesc.slopeScaledDepthBias = kDecalSlopeScaledDepthBias;
            if (ID3D12PipelineState* blendPipeline = pipelineCache.GetGraphics(blendDesc)) {
                commandList->SetPipelineState(blendPipeline);
                drawMeshes(constants, kPassTranslucent, useTessellation);
            }
        }
        // 陰影の上に三角形の辺を重ねる。本描画と同じ変位・分割で線を引き、
        // 深度バイアスで面の手前に寄せる。深度は読むだけで、色は黒の単色。
        if (m_showWireframeOverlay && displayView != DebugView::Wireframe) {
            rhi::GraphicsPipelineDesc wireDesc = meshPipelineDesc;
            wireDesc.pixelEntry = L"PsWireframeOverlay";
            wireDesc.fillMode = D3D12_FILL_MODE_WIREFRAME;
            wireDesc.depthWrite = false;
            wireDesc.depthBias = kDecalDepthBias;
            wireDesc.slopeScaledDepthBias = kDecalSlopeScaledDepthBias;
            if (ID3D12PipelineState* wirePipeline = pipelineCache.GetGraphics(wireDesc)) {
                PIXBeginEvent(commandList, PIX_COLOR(60, 60, 60), "PreviewWireframeOverlay");
                commandList->SetPipelineState(wirePipeline);
                drawMeshes(constants, kPassOpaque | kPassDecal | kPassTranslucent, useTessellation);
                PIXEndEvent(commandList);
            }
        }
        commandList->SetPipelineState(meshPipeline);
    }
    m_stats.tessellation = useTessellation;
    m_stats.tessellationFactor = m_tessellationFactor;

    PIXEndEvent(commandList);

    // --- スカイボックス ----------------------------------------------------
    // メッシュのあとに描く。深度は書かず、まだ何も描かれていない画素だけを埋める。

    // チャンネルを覗く表示のときは背景を描かない。値だけを見たいため。
    if (m_showSkybox && environment.IsReady() && IsShadedView(displayView)) {
        rhi::GraphicsPipelineDesc skyboxPipelineDesc;
        skyboxPipelineDesc.shaderPath = L"Skybox.hlsl";
        skyboxPipelineDesc.vertexEntry = L"VsMain";
        skyboxPipelineDesc.pixelEntry = L"PsMain";
        skyboxPipelineDesc.rtvFormat = kSceneColorFormat;
        skyboxPipelineDesc.dsvFormat = kDepthFormat;
        skyboxPipelineDesc.layout = rhi::VertexLayout::None;
        skyboxPipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
        skyboxPipelineDesc.depthTest = true;
        skyboxPipelineDesc.depthWrite = false;

        ID3D12PipelineState* skyboxPipeline = pipelineCache.GetGraphics(skyboxPipelineDesc);
        const rhi::UploadAllocation skyboxCb =
            device.Upload().Allocate(sizeof(SkyboxConstants), 256);

        if (skyboxPipeline != nullptr && skyboxCb.IsValid()) {
            PIXBeginEvent(commandList, PIX_COLOR(120, 160, 220), "PreviewSkybox");

            SkyboxConstants skyboxConstants = {};
            // メッシュ側と同じ理由で転置は入れない。
            XMStoreFloat4x4(&skyboxConstants.inverseViewProjection,
                            XMMatrixInverse(nullptr, XMMatrixMultiply(view, projection)));
            skyboxConstants.cameraPosition = m_camera.Position();
            // シーンの空の背景は空の輝度そのもの（スカイライトの強さは IBL だけに掛ける）。
            skyboxConstants.intensity = m_atmosphericMode ? 1.0f : m_activeSky.iblIntensity;
            if (m_skyboxBlur) {
                // プリフィルタ済みキューブはラフネス別に GGX で畳み込んである。
                // 粗いミップを引けば、ぼかしパスを足さずに背景だけを柔らかくできる。
                // ミップを落とすだけの（箱フィルタの）環境キューブより滑らか。
                skyboxConstants.environmentIndex = environment.PrefilteredSrvIndex();
                skyboxConstants.mipLevel = std::min(
                    kSkyboxBlurMip, static_cast<float>(environment.PrefilteredMipCount() - 1));
            } else {
                skyboxConstants.environmentIndex = environment.EnvironmentSrvIndex();
                skyboxConstants.mipLevel = 0.0f;
            }
            if (m_atmosphericMode && m_atmosphere.IsReady()) {
                // 太陽の円盤。環境マップには入れていないので、大気を通った照度を円盤の立体角で割った輝度で足す。
                // 視半径 0.00465 rad（約 0.27 度）。RGBA16F に収まるよう頭を抑える（直接光・IBL はそのまま）。
                constexpr float kSunAngularRadius = 0.00465f;
                const float solidAngle = 3.14159265f * kSunAngularRadius * kSunAngularRadius;
                skyboxConstants.sunDirection = light.Direction();
                skyboxConstants.sunAngularRadius = kSunAngularRadius;
                skyboxConstants.sunRadiance = {std::min(light.color.x * light.illuminance / solidAngle, 60000.0f),
                                               std::min(light.color.y * light.illuminance / solidAngle, 60000.0f),
                                               std::min(light.color.z * light.illuminance / solidAngle, 60000.0f)};
            }
            std::memcpy(skyboxCb.cpu, &skyboxConstants, sizeof(skyboxConstants));

            commandList->SetPipelineState(skyboxPipeline);
            commandList->SetGraphicsRootConstantBufferView(1, skyboxCb.gpuAddress);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->IASetIndexBuffer(nullptr);
            commandList->DrawInstanced(3, 1, 0, 0);
            // 画面全体を覆う三角形 1 枚。頂点は頂点シェーダが作る。
            ++m_stats.drawCalls;
            m_stats.vertices += 3;
            m_stats.triangles += 1;

            PIXEndEvent(commandList);
        }
    }

    TransitionIfNeeded(commandList, m_sceneColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // --- 自動露出の測光 ------------------------------------------------------
    // 被写界深度と露出の前の線形 HDR を測る。結果は次のフレーム以降に CPU で読む。
    if (m_exposure.automatic && IsShadedView(displayView)) MeterExposure(device, pipelineCache, commandList);

    // --- 被写界深度 --------------------------------------------------------
    // **トーンマップの前に、線形 HDR のまま掛ける。** 露出後だと明るい点が
    // 飽和してから広がり、玉ボケの芯が白く潰れる。
    // チャンネルを覗く表示には掛けない（値そのものを見るための表示）。
    uint32_t tonemapSourceIndex = m_sceneColor.SrvIndex();
    if (m_ssao.enabled && IsShadedView(displayView)) {
        if (auto* pipeline = pipelineCache.GetCompute(L"ScreenSpaceAo.hlsl", L"CsMain")) {
            TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            TransitionIfNeeded(commandList, m_sceneColorAo, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            struct Constants {
                uint32_t source, depth, output, width, height;
                float nearZ, farZ, tanHalfFov, radius, strength;
            } ao{m_sceneColor.SrvIndex(), m_depth.SrvIndex(), m_sceneColorAo.UavIndex(), m_width, m_height,
                 m_camera.NearZ(), m_camera.FarZ(), std::tan(m_camera.FovY() * 0.5f),
                 std::clamp(m_ssao.radius, 0.001f, 10.0f), std::clamp(m_ssao.strength, 0.0f, 3.0f)};
            commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
            commandList->SetPipelineState(pipeline);
            commandList->SetComputeRoot32BitConstants(0, sizeof(ao)/sizeof(uint32_t), &ao, 0);
            commandList->Dispatch(rhi::DispatchCount(m_width), rhi::DispatchCount(m_height), 1);
            TransitionIfNeeded(commandList, m_sceneColorAo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            tonemapSourceIndex = m_sceneColorAo.SrvIndex();
        }
    }
    ID3D12PipelineState* dofPipeline =
        (m_dof.enabled && IsShadedView(displayView) && m_sceneColorDof.IsValid())
            ? pipelineCache.GetCompute(L"DepthOfField.hlsl", L"CsMain")
            : nullptr;
    if (dofPipeline != nullptr) {
        PIXBeginEvent(commandList, PIX_COLOR(120, 160, 220), "PreviewDepthOfField");

        TransitionIfNeeded(commandList, m_depth,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_sceneColorDof, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        DofConstants dofConstants = {};
        dofConstants.sourceIndex = tonemapSourceIndex;
        dofConstants.depthIndex = m_depth.SrvIndex();
        dofConstants.outputIndex = m_sceneColorDof.UavIndex();
        dofConstants.width = m_width;
        dofConstants.height = m_height;
        dofConstants.focalLengthMm = FocalLengthFromFovY(m_camera.FovY());
        // ミニチュアの縮尺は「1 : N」で持つ。シーンの距離を 1/N にして式へ入れる。
        dofConstants.sceneScale = 1.0f / std::max(m_dof.miniatureScale, 1.0f);
        dofConstants.fStop = m_exposure.aperture;
        dofConstants.focusDistance = FocusDistance();
        dofConstants.nearZ = m_camera.NearZ();
        dofConstants.farZ = m_camera.FarZ();
        dofConstants.maxBlurPixels = m_dof.maxBlurPixels;
        dofConstants.apertureRotation = m_dof.rotationDegrees * (3.14159265358979f / 180.0f);
        dofConstants.apertureBlades = ApertureBladeCount(m_dof.shape);
        dofConstants.blurScale = m_dof.blurScale;

        commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
        commandList->SetPipelineState(dofPipeline);
        commandList->SetComputeRoot32BitConstants(0, sizeof(dofConstants) / sizeof(uint32_t),
                                                  &dofConstants, 0);
        commandList->Dispatch(rhi::DispatchCount(m_width), rhi::DispatchCount(m_height), 1);

        TransitionIfNeeded(commandList, m_sceneColorDof,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tonemapSourceIndex = m_sceneColorDof.SrvIndex();

        PIXEndEvent(commandList);
    }

    PIXBeginEvent(commandList, PIX_COLOR(200, 120, 80), "PreviewTonemap");

    TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const TonemapConstants tonemapConstants{
        tonemapSourceIndex,    m_output.UavIndex(),
        m_width,               m_height,
        m_exposure.Exposure(), static_cast<uint32_t>(m_tonemap),
        IsShadedView(displayView) ? 0u : 1u};

    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(tonemapPipeline);
    commandList->SetComputeRoot32BitConstants(0, sizeof(tonemapConstants) / sizeof(uint32_t),
                                              &tonemapConstants, 0);

    commandList->Dispatch(rhi::DispatchCount(m_width), rhi::DispatchCount(m_height), 1);

    PIXEndEvent(commandList);

    // ホバー / 選択メッシュのシルエット枠。外周の辺を、本描画と同じ変位で押し出して重ねる。
    // 深度は見ない（手前の物に隠れても輪郭が分かるようにする）。選択を先に描き、ホバーを上に重ねる。
    if (m_meshSceneEnabled) {
        rhi::GraphicsPipelineDesc outlineDesc;
        outlineDesc.shaderPath = L"MeshPbr.hlsl";
        outlineDesc.vertexEntry = L"VsMain";
        outlineDesc.pixelEntry = L"PsOutline";
        outlineDesc.rtvFormat = kOutputFormat;
        outlineDesc.dsvFormat = kDepthFormat;
        outlineDesc.layout = rhi::VertexLayout::MeshStandard;
        outlineDesc.cullMode = D3D12_CULL_MODE_NONE;
        outlineDesc.depthTest = false;
        outlineDesc.depthWrite = false;
        outlineDesc.lineTopology = true;
        outlineDesc.alphaBlend = true;
        ID3D12PipelineState* outlinePipeline = nullptr;
        const auto drawOutline = [&](int meshIndex, uint32_t flag) {
            if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= m_sceneMeshes.size()) return;
            if (outlinePipeline == nullptr) outlinePipeline = pipelineCache.GetGraphics(outlineDesc);
            if (outlinePipeline == nullptr) return;
            PIXBeginEvent(commandList, PIX_COLOR(240, 200, 120), "PreviewMeshOutline");
            TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
            TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            const D3D12_CPU_DESCRIPTOR_HANDLE outputRtv = m_output.rtv.cpu;
            const D3D12_CPU_DESCRIPTOR_HANDLE depthDsv = m_depth.dsv.cpu;
            commandList->OMSetRenderTargets(1, &outputRtv, FALSE, &depthDsv);
            commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
            commandList->SetPipelineState(outlinePipeline);
            MeshConstants outlineConstants = constants;
            outlineConstants.meshDisplayFlags |= flag;
            drawMeshes(outlineConstants, kPassOpaque | kPassDecal | kPassTranslucent, false, meshIndex);
            PIXEndEvent(commandList);
        };
        for (const int selected : m_selectedMeshes) drawOutline(selected, kMeshFlagOutlineSelected);
        const bool hoveredIsSelected =
            std::find(m_selectedMeshes.begin(), m_selectedMeshes.end(), m_hoveredMesh) != m_selectedMeshes.end();
        if (!hoveredIsSelected) drawOutline(m_hoveredMesh, kMeshFlagOutlineHovered);
    }

    // 作業グリッド。シーンの深度でテストするため、ImGui ではなくここで描く。
    DrawGuideOverlay(device, pipelineCache, commandList);

    // ImGui から SRV として読むため、ピクセルシェーダ可視の状態へ移す。
    TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_diagnostics.End(device, commandList, !IsEvaluating() && m_stats.drawCalls > 0, m_width, m_height, useTessellation);
    if (probeConstants != 0 && !IsEvaluating())
        m_diagnostics.Probe(device, pipelineCache, commandList, m_meshScene.meshes[probeMesh], probeConstants);
}

// 作業グリッドの線。
//
// トーンマップ後の表示用テクスチャへ、露出を通さない表示色のまま描く
// （ギズモは画面上で一定の明るさに見えるべきもの）。深度は読むだけで書かない。
void PreviewRenderer::DrawGuideOverlay(rhi::Device& device,
                                       rhi::PipelineCache& pipelineCache,
                                       ID3D12GraphicsCommandList* commandList) {
    if (!m_showReferenceGrid && !m_showHumanScale && m_overlayLines.empty()) {
        return;
    }

    rhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.shaderPath = L"OverlayLines.hlsl";
    pipelineDesc.vertexEntry = L"VsMain";
    pipelineDesc.pixelEntry = L"PsMain";
    pipelineDesc.rtvFormat = kOutputFormat;
    pipelineDesc.dsvFormat = kDepthFormat;
    pipelineDesc.layout = rhi::VertexLayout::None;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = false;
    pipelineDesc.lineTopology = true;
    pipelineDesc.alphaBlend = true;

    ID3D12PipelineState* pipeline = pipelineCache.GetGraphics(pipelineDesc);
    if (pipeline == nullptr) {
        return;
    }

    PIXBeginEvent(commandList, PIX_COLOR(160, 170, 190), "PreviewReferenceGrid");

    TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // DoF が有効なフレームでは深度が SRV になっている。DSV として束ね直す
    // （書き込みは PSO 側で無効にしてある）。
    TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_output.rtv.cpu;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depth.dsv.cpu;
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);

    // 端点を定数バッファへ詰めて 1 回描く。上限を超えたら分けて描く。
    // 使う端点の分だけ送る（シェーダは count より先を読まない）。
    const auto submit = [&](const OverlayLineConstants& constants, uint32_t count) {
        if (count == 0) return;
        const size_t bytes = offsetof(OverlayLineConstants, positions) + sizeof(XMFLOAT4) * count;
        const rhi::UploadAllocation cb = device.Upload().Allocate(bytes, 256);
        if (!cb.IsValid()) return;
        std::memcpy(cb.cpu, &constants, bytes);
        commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
        commandList->DrawInstanced(count, 1, 0, 0);
        ++m_stats.drawCalls;
        m_stats.vertices += count;
    };
    const auto drawGuide = [&](const OverlayLineSet& set) {
        auto description = pipelineDesc;
        description.lineTopology = !set.triangles;
        description.depthTest = set.depthTest;
        auto* guidePipeline = pipelineCache.GetGraphics(description);
        if (!guidePipeline) return;
        commandList->SetPipelineState(guidePipeline);
        commandList->IASetPrimitiveTopology(set.triangles ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                                          : D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        // 端点の配列は大きいので 0 で埋めない。使う分だけ書いて送る。
        OverlayLineConstants constants;
        constants.options = {};
        XMStoreFloat4x4(&constants.viewProjection,
                        XMMatrixMultiply(m_camera.ViewMatrix(), m_camera.ProjectionMatrix()));
        constants.color[0] = set.color.x;
        constants.color[1] = set.color.y;
        constants.color[2] = set.color.z;
        constants.color[3] = set.color.w;
        uint32_t count = 0;
        const size_t stride = set.triangles ? 3 : 2;
        for (size_t i = 0; i + stride <= set.points.size(); i += stride) {
            if (count + stride > kOverlayLineMaxVertices) {
                submit(constants, count);
                count = 0;
            }
            for (size_t k = 0; k < stride; ++k) {
                const auto& p = set.points[i + k];
                constants.positions[count++] = XMFLOAT4{p.x, p.y, p.z, 1.0f};
            }
        }
        submit(constants, count);
    };
    for (const auto& set : m_overlayLines) drawGuide(set);
    if (m_showHumanScale) {
        // Y軸だけを回すビルボード。頭頂から靴底までを指定身長に保つ。
        // 不透明な単色のパーツを重ね、腕と胴・左右の脚の隙間を残す。
        const auto right = m_camera.Basis().right;
        const float length = std::hypot(right.x, right.z);
        const float rx = length > 0.0001f ? right.x / length : 1.0f;
        const float rz = length > 0.0001f ? right.z / length : 0.0f;
        const float scale = m_humanScaleHeight / 1.7f;
        const XMFLOAT3 origin{m_humanScaleAnchor.x + m_humanScaleOffset.x,
                             m_humanScaleAnchor.y + m_humanScaleOffset.y,
                             m_humanScaleAnchor.z + m_humanScaleOffset.z};
        OverlayLineSet person;
        person.triangles = true;
        person.color = {0.48f, 0.50f, 0.53f, 1.0f};
        const auto point = [&](XMFLOAT2 p) {
            return XMFLOAT3{origin.x + rx * p.x * scale, origin.y + p.y * scale,
                            origin.z + rz * p.x * scale};
        };
        const auto polygon = [&](std::initializer_list<XMFLOAT2> points) {
            const auto* p = points.begin();
            for (size_t i = 1; i + 1 < points.size(); ++i) {
                person.points.push_back(point(p[0]));
                person.points.push_back(point(p[i]));
                person.points.push_back(point(p[i + 1]));
            }
        };
        // 上から順に {中心 x, 高さ y, 半幅} の断面を並べ、隣り合う断面を台形でつなぐ。
        // 凸でない輪郭（首の付け根・腰のくびれ）もそのまま描ける。
        const auto strip = [&](float side, const XMFLOAT3* rows, size_t count) {
            for (size_t i = 0; i + 1 < count; ++i) {
                const XMFLOAT3& a = rows[i];
                const XMFLOAT3& b = rows[i + 1];
                polygon({{side * (a.x - a.z), a.y}, {side * (a.x + a.z), a.y},
                         {side * (b.x + b.z), b.y}, {side * (b.x - b.z), b.y}});
            }
        };
        const auto limb = [&](float side, std::initializer_list<XMFLOAT3> rows) {
            strip(side, rows.begin(), rows.size());
        };
        // 身長 1.70 m の標準的な成人の比率（約 7.5 頭身）。頭頂 1.70、顎 1.47、
        // 肩峰 1.39、肘 1.07、股 0.80、手首 0.82、指先 0.64、膝 0.49、足首 0.07。
        XMFLOAT3 head[17];
        for (int i = 0; i <= 16; ++i) {
            const float t = XM_PI * float(i) / 16.0f;
            const float y = 0.115f * std::cos(t);
            // 下半分は顎へ向けて少し細くする。
            const float jaw = y < 0.0f ? 1.0f + 0.2f * y / 0.115f : 1.0f;
            head[i] = {0.0f, 1.585f + y, 0.078f * std::sin(t) * jaw};
        }
        strip(1.0f, head, 17);
        limb(1.0f, {{0, 1.52f, 0.047f}, {0, 1.46f, 0.052f}, {0, 1.44f, 0.08f},
                    {0, 1.425f, 0.13f}, {0, 1.41f, 0.17f}, {0, 1.395f, 0.195f},
                    {0, 1.37f, 0.20f}, {0, 1.30f, 0.178f}, {0, 1.22f, 0.160f},
                    {0, 1.12f, 0.145f}, {0, 1.04f, 0.137f}, {0, 0.98f, 0.150f},
                    {0, 0.92f, 0.165f}, {0, 0.86f, 0.162f}, {0, 0.82f, 0.11f},
                    {0, 0.79f, 0.02f}});
        for (float side : {-1.0f, 1.0f}) {
            // 腕は体側からわずかに離して下ろす。
            limb(side, {{0.175f, 1.415f, 0.02f}, {0.19f, 1.405f, 0.036f}, {0.205f, 1.375f, 0.045f},
                        {0.222f, 1.28f, 0.040f}, {0.228f, 1.16f, 0.034f}, {0.233f, 1.07f, 0.031f},
                        {0.240f, 0.98f, 0.031f}, {0.246f, 0.88f, 0.024f}, {0.249f, 0.83f, 0.021f},
                        {0.251f, 0.80f, 0.027f}, {0.252f, 0.72f, 0.025f}, {0.248f, 0.66f, 0.016f},
                        {0.245f, 0.64f, 0.006f}});
            limb(side, {{0.090f, 0.88f, 0.080f}, {0.092f, 0.78f, 0.078f}, {0.090f, 0.65f, 0.066f},
                        {0.088f, 0.52f, 0.050f}, {0.088f, 0.485f, 0.047f}, {0.088f, 0.44f, 0.046f},
                        {0.090f, 0.36f, 0.052f}, {0.090f, 0.25f, 0.042f}, {0.090f, 0.12f, 0.030f},
                        {0.090f, 0.07f, 0.029f}});
            polygon({{side * 0.062f, 0.075f}, {side * 0.118f, 0.075f}, {side * 0.14f, 0.025f},
                     {side * 0.135f, 0.0f}, {side * 0.05f, 0.0f}, {side * 0.048f, 0.025f}});
        }
        drawGuide(person);
    }
    commandList->SetPipelineState(pipeline);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    if (!m_showReferenceGrid) {
        PIXEndEvent(commandList);
        return;
    }

    const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(OverlayLineConstants), 256);
    if (!cb.IsValid()) {
        PIXEndEvent(commandList);
        return;
    }

    OverlayLineConstants constants = {};
    XMStoreFloat4x4(&constants.viewProjection,
                    XMMatrixMultiply(m_camera.ViewMatrix(), m_camera.ProjectionMatrix()));
    // ImGui のギズモと同じ無彩色（表示色）。
    constants.color[0] = 150.0f / 255.0f;
    constants.color[1] = 160.0f / 255.0f;
    constants.color[2] = 175.0f / 255.0f;
    constants.color[3] = 1.0f;

    uint32_t count = 0;
    const auto addLine = [&](const XMFLOAT3& a, const XMFLOAT3& b, float alpha) {
        if (count + 2 > kOverlayLineMaxVertices) {
            return;
        }
        constants.positions[count++] = XMFLOAT4{a.x, a.y, a.z, alpha};
        constants.positions[count++] = XMFLOAT4{b.x, b.y, b.z, alpha};
    };

    {
        // 1 unit = 1 m。各方向51本、50区画。基準面と同一面のメッシュとのちらつきだけを抑える。
        constants.options.x = 0.000001f;
        constexpr int halfExtent = 25;
        for (int i = -halfExtent; i <= halfExtent; ++i) {
            const float coordinate = static_cast<float>(i);
            const float alpha = i == 0 ? 0.95f : (i % 5 == 0 ? 0.65f : 0.28f);
            addLine({coordinate, 0.0f, -25.0f}, {coordinate, 0.0f, 25.0f}, alpha);
            addLine({-25.0f, 0.0f, coordinate}, {25.0f, 0.0f, coordinate}, alpha);
        }
    }

    std::memcpy(cb.cpu, &constants, sizeof(constants));
    commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
    commandList->DrawInstanced(count, 1, 0, 0);
    ++m_stats.drawCalls;
    m_stats.vertices += count;

    PIXEndEvent(commandList);
}

}  // namespace rock::renderer
