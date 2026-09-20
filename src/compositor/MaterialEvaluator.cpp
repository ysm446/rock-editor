#include "compositor/MaterialEvaluator.h"

#include "core/Log.h"

#include <pix3.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace DirectX;

namespace tg::compositor {
namespace {

using rhi::DispatchCount;

constexpr DXGI_FORMAT kBaseColorFormat = DXGI_FORMAT_R11G11B10_FLOAT;
constexpr DXGI_FORMAT kNormalFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kSurfaceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// Height は R32。0〜1 の全幅が標高差なので、R16 では 600 m の地形で 1 ULP が約 0.3 m になる。
constexpr DXGI_FORMAT kHeightFormat = DXGI_FORMAT_R32_FLOAT;
// 非同期評価の定数の置き場。レイヤーごとにタイル数ぶんの定数が載る。
// 使い切ったら次の評価で倍に広げる。
constexpr uint64_t kAsyncUploadBytes = 2ull * 1024 * 1024;

// 法線マップの緑を反転して読む（OpenGL 規約の素材）。シェーダの TG_FLAG_* と一致させること。
constexpr uint32_t kFlagFlipNormalGreen = 0x1u;

// ノードに出す合成結果のサムネイルの一辺。ノード上では 64px で描くので同じ大きさ。
constexpr uint32_t kLayerThumbnailSize = 64;

// GPU 側の ThumbnailConstants（CompositeThumbnail.hlsl）と一致させること。
struct LayerThumbnailConstants {
    uint32_t indices[4];  // BaseColor の SRV, Height の SRV, 出力 UAV, 出力の一辺
    float params[4];      // 一辺（m）, 標高差（m）, 未使用 x2
};

// GPU 側の LayerConstants（CompositeLayer.hlsl）と一致させること。
struct LayerConstants {
    uint32_t outputIndices[4];  // BaseColor, Normal, Surface, Height の UAV
    uint32_t tile[4];           // x, y, width, height
    uint32_t resolution[2];
    uint32_t channelMask;
    uint32_t flags;

    float baseColor[4];           // rgb, 未使用
    float surfaceParams[4];       // roughness, metallic, ao, heightBase
    float heightParams[4];        // heightPerSize, uvScale, heightSource, 不透明度の定数
    float heightNoise[4];         // scale, heightGain, octaves, offset
    uint32_t textureIndices0[4];  // baseColor, normal, roughness, metallic
    uint32_t textureIndices1[4];  // ao, height, opacity, 未使用
    uint32_t noiseTypes[4];       // height, 未使用 x3
    uint32_t mapChannels[4];      // x にすべて入る。yzw は未使用
    float colorAdjust[4];         // 色相（ラジアン）, 彩度, 明るさ, 未使用
};

bool CreateChannelTexture(rhi::Device& device, uint32_t resolution, DXGI_FORMAT format,
                          const wchar_t* debugName, rhi::GpuTexture& outTexture) {
    rhi::TextureDesc desc;
    desc.width = resolution;
    desc.height = resolution;
    desc.format = format;
    desc.allowUnorderedAccess = true;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    desc.debugName = debugName;
    return device.Allocator().CreateTexture2D(desc, outTexture);
}

bool CreateTextureSet(rhi::Device& device, uint32_t resolution, MaterialTextureSet& set) {
    return CreateChannelTexture(device, resolution, kBaseColorFormat, L"MaterialBaseColor",
                                set.baseColor) &&
           CreateChannelTexture(device, resolution, kNormalFormat, L"MaterialNormal",
                                set.normal) &&
           CreateChannelTexture(device, resolution, kSurfaceFormat, L"MaterialSurface",
                                set.surface) &&
           CreateChannelTexture(device, resolution, kHeightFormat, L"MaterialHeight",
                                set.height);
}

void ReleaseTextureSet(rhi::Device& device, MaterialTextureSet& set) {
    device.DeferRelease(set.baseColor);
    device.DeferRelease(set.normal);
    device.DeferRelease(set.surface);
    device.DeferRelease(set.height);
}

}  // namespace

bool MaterialEvaluator::Create(rhi::Device& device, uint32_t resolution, bool asynchronous) {
    // 走っている評価が前の組を読んでいるかもしれない。作り直す前に必ず待つ。
    WaitForEvaluation();
    ReleaseTextures(device);
    m_asyncInFlight = false;
    m_hasResult = false;
    m_asynchronous = asynchronous;

    if (!CreateTextureSet(device, resolution, m_textures)) {
        return false;
    }
    if (asynchronous) {
        // 表側。描画はこちらを読み、評価は裏側（m_textures）へ書く。
        if (!CreateTextureSet(device, resolution, m_frontTextures)) {
            return false;
        }
        if (!m_compute.IsValid() &&
            !m_compute.Create(device, kAsyncUploadBytes, L"MaterialEvaluatorCompute")) {
            // キューが作れなくても同期で評価はできる。落とさずに続ける。
            TG_LOG_WARN("合成の評価用のコンピュートキューを作れませんでした。同期で評価します");
            m_compute.Destroy(device);
        }
    }

    m_resolution = resolution;
    m_evaluatedRevision = 0;
    return true;
}

void MaterialEvaluator::Destroy(rhi::Device& device) {
    // コンピュートキューがまだ出力を読んでいるかもしれない。先に待つ。
    WaitForEvaluation();
    m_compute.Destroy(device);
    m_asyncInFlight = false;
    m_hasResult = false;
    ReleaseTextures(device);
    for (rhi::GpuTexture& thumbnail : m_layerThumbnails) {
        device.DeferRelease(thumbnail);
    }
    m_layerThumbnails.clear();
    for (rhi::GpuTexture& thumbnail : m_frontLayerThumbnails) {
        device.DeferRelease(thumbnail);
    }
    m_frontLayerThumbnails.clear();
    m_resolution = 0;
    m_evaluatedRevision = 0;
}

void MaterialEvaluator::EnsureLayerThumbnails(rhi::Device& device, size_t layerCount) {
    while (m_layerThumbnails.size() > layerCount) {
        device.DeferRelease(m_layerThumbnails.back());
        m_layerThumbnails.pop_back();
    }
    while (m_layerThumbnails.size() < layerCount) {
        rhi::GpuTexture thumbnail;
        if (!CreateChannelTexture(device, kLayerThumbnailSize, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  L"LayerThumbnail", thumbnail)) {
            TG_LOG_WARN("結果のサムネイルを作れませんでした");
            break;
        }
        m_layerThumbnails.push_back(std::move(thumbnail));
    }
}

// BaseColor と Height を SRV にして読み、終わったら UAV へ戻す（次のレイヤーが書く）。
void MaterialEvaluator::BakeLayerThumbnail(rhi::Device& device, ID3D12PipelineState* pipeline,
                                           ID3D12GraphicsCommandList* commandList,
                                           const MaterialStack& stack, size_t layerIndex) {
    if (pipeline == nullptr || layerIndex >= m_layerThumbnails.size() ||
        !m_layerThumbnails[layerIndex].IsValid()) {
        return;
    }
    LayerThumbnailConstants constants = {};
    constants.indices[0] = m_textures.baseColor.SrvIndex();
    constants.indices[1] = m_textures.height.SrvIndex();
    constants.indices[2] = m_layerThumbnails[layerIndex].UavIndex();
    constants.indices[3] = kLayerThumbnailSize;
    constants.params[0] = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    constants.params[1] = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(LayerThumbnailConstants));
    if (!cb.IsValid()) {
        return;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(120, 140, 160), "LayerThumbnail");
    TransitionIfNeeded(commandList, m_textures.baseColor,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_layerThumbnails[layerIndex],
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->Dispatch(DispatchCount(kLayerThumbnailSize), DispatchCount(kLayerThumbnailSize),
                          1);
    TransitionIfNeeded(commandList, m_textures.baseColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PIXEndEvent(commandList);
}

D3D12_GPU_DESCRIPTOR_HANDLE MaterialEvaluator::LayerThumbnailHandle(size_t layerIndex) const {
    const std::vector<rhi::GpuTexture>& thumbnails = DisplayedLayerThumbnails();
    if (layerIndex >= thumbnails.size() || !thumbnails[layerIndex].IsValid()) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }
    return thumbnails[layerIndex].srv.gpu;
}

void MaterialEvaluator::ReleaseTextures(rhi::Device& device) {
    ReleaseTextureSet(device, m_textures);
    ReleaseTextureSet(device, m_frontTextures);
}

bool MaterialEvaluator::Resize(rhi::Device& device, uint32_t resolution) {
    if (resolution == m_resolution) {
        return true;
    }
    // 作り直す前に GPU の参照が切れるのを待つ（コンピュートキューの評価も含む）。
    device.WaitForGpu();
    return Create(device, resolution, m_asynchronous);
}

bool MaterialEvaluator::IsEvaluating() const {
    return m_asyncInFlight && m_compute.IsBusy();
}

void MaterialEvaluator::WaitForEvaluation() {
    m_compute.Wait();
}

rhi::UploadAllocation MaterialEvaluator::AllocateConstants(rhi::Device& device, uint64_t size) {
    if (m_recordingAsync) {
        return m_compute.Allocate(size, 256);
    }
    return device.Upload().Allocate(size, 256);
}

std::vector<TileRect> MaterialEvaluator::MakeTiles() const {
    std::vector<TileRect> tiles;
    for (uint32_t y = 0; y < m_resolution; y += m_tileSize) {
        for (uint32_t x = 0; x < m_resolution; x += m_tileSize) {
            TileRect tile;
            tile.x = x;
            tile.y = y;
            tile.width = std::min(m_tileSize, m_resolution - x);
            tile.height = std::min(m_tileSize, m_resolution - y);
            tiles.push_back(tile);
        }
    }
    return tiles;
}

// メッシュの描画から読めるようにする。Height は頂点 / ドメインシェーダ
// （ディスプレイスメント）からも読まれるため、NON_PIXEL も含める。
// 状態の食い違いを避けるため 4 枚とも同じ状態に揃える。
void MaterialEvaluator::TransitionThumbnailsForDisplay(
    ID3D12GraphicsCommandList* commandList, std::vector<rhi::GpuTexture>& thumbnails) {
    constexpr D3D12_RESOURCE_STATES kDisplayReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (rhi::GpuTexture& thumbnail : thumbnails) {
        TransitionIfNeeded(commandList, thumbnail, kDisplayReadState);
    }
}

void MaterialEvaluator::TransitionForDisplay(ID3D12GraphicsCommandList* commandList,
                                             MaterialTextureSet& set) {
    constexpr D3D12_RESOURCE_STATES kDisplayReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    TransitionIfNeeded(commandList, set.baseColor, kDisplayReadState);
    TransitionIfNeeded(commandList, set.normal, kDisplayReadState);
    TransitionIfNeeded(commandList, set.surface, kDisplayReadState);
    TransitionIfNeeded(commandList, set.height, kDisplayReadState);
}
bool MaterialEvaluator::Evaluate(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                 ID3D12GraphicsCommandList* commandList,
                                 const MaterialStack& stack, const TextureLibrary& textures,
                                 const MaterialLibrary& materials,
                                 const std::vector<TileRect>& tiles) {
    if (!m_textures.IsValid() || tiles.empty()) {
        return false;
    }

    ID3D12PipelineState* layerPipeline =
        pipelineCache.GetCompute(L"CompositeLayer.hlsl", L"CsMain");
    if (layerPipeline == nullptr) {
        return false;
    }
    ID3D12PipelineState* layerThumbnailPipeline =
        pipelineCache.GetCompute(L"CompositeThumbnail.hlsl", L"CsMain");

    // 有効なレイヤーが 1 枚も無いときは npos。合成はしない。
    const size_t baseIndex = stack.FirstEnabledIndex();

    bool complete = true;

    PIXBeginEvent(commandList, PIX_COLOR(220, 140, 60), "CompositeStack");

    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());

    TransitionIfNeeded(commandList, m_textures.baseColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.normal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    EnsureLayerThumbnails(device, stack.Layers().size());

    m_evaluatedLayerCount = 0;
    m_evaluatedTileCount = static_cast<uint32_t>(tiles.size());

    // レイヤー優先で回す。1 レイヤーぶんを全タイルで終えてから次へ進む。
    for (size_t layerIndex = 0; layerIndex < stack.Layers().size(); ++layerIndex) {
        const MaterialLayer& layer = stack.Layers()[layerIndex];
        const bool isBaseLayer = (layerIndex == baseIndex);

        // タイルごとに変わるのは矩形だけなので、定数はレイヤー 1 枚につき 1 回組む。
        LayerConstants constants = {};
        constants.outputIndices[0] = m_textures.baseColor.UavIndex();
        constants.outputIndices[1] = m_textures.normal.UavIndex();
        constants.outputIndices[2] = m_textures.surface.UavIndex();
        constants.outputIndices[3] = m_textures.height.UavIndex();

        constants.resolution[0] = m_resolution;
        constants.resolution[1] = m_resolution;

        // 一番下のレイヤーは下地なので、必ず全チャンネルを埋める。
        // そうしないと未初期化のテクセルが残る。
        constants.channelMask = isBaseLayer ? kAllChannelBits : layer.channelMask;

        constants.flags = 0;
        // マップはレイヤーが参照するマテリアルから引く。
        const MaterialAsset* material = materials.Find(layer.material);

        // 法線マップの規約はマテリアルごと。マップが無ければ関係ない。
        if (material != nullptr && material->flipNormalGreen) {
            constants.flags |= kFlagFlipNormalGreen;
        }

        // 定数もマテリアルが持っているほうを優先する。
        // マテリアル側とレイヤー側の両方が掛かると、どちらが効いているか分からない。
        const DirectX::XMFLOAT3 baseColor =
            (material != nullptr) ? material->baseColorTint : layer.baseColor;
        constants.baseColor[0] = baseColor.x;
        constants.baseColor[1] = baseColor.y;
        constants.baseColor[2] = baseColor.z;

        // 色相 / 彩度は**マテリアルだけ**が持つ（レイヤー側には無い）。
        // 度で持ち、シェーダへはラジアンで渡す。
        constants.colorAdjust[0] =
            (material != nullptr)
                ? material->hueShiftDegrees * (3.14159265358979f / 180.0f)
                : 0.0f;
        constants.colorAdjust[1] = (material != nullptr) ? material->saturation : 1.0f;
        constants.colorAdjust[2] = (material != nullptr) ? material->brightness : 1.0f;

        constants.surfaceParams[0] =
            (material != nullptr) ? material->roughnessValue : layer.roughness;
        constants.surfaceParams[1] =
            (material != nullptr) ? material->metallicValue : layer.metallic;
        constants.surfaceParams[2] = (material != nullptr)
                                         ? material->ambientOcclusionValue
                                         : layer.ambientOcclusion;
        constants.surfaceParams[3] = layer.heightBase;

        // 法線は実寸の勾配から作る。ハイト 0〜1 の全幅が標高差（m）、
        // 出力 UV 0〜1 が地形の一辺（m）なので、その比を渡す。
        constants.heightParams[0] =
            (stack.SizeMeters() > 0.0f) ? (stack.HeightMeters() / stack.SizeMeters()) : 0.0f;
        constants.heightParams[1] = layer.uvScale;
        constants.heightParams[2] = static_cast<float>(layer.heightSource);
        // 不透明度の定数。マップが無ければこれが Surface の A になる。
        constants.heightParams[3] = (material != nullptr) ? material->opacityValue : 1.0f;

        constants.heightNoise[0] = layer.heightNoise.scale;
        // ハイトはノイズの amount ではなく heightGain を使う。
        constants.heightNoise[1] = layer.heightGain;
        constants.heightNoise[2] = static_cast<float>(layer.heightNoise.octaves);
        constants.heightNoise[3] = layer.heightNoise.offset;

        // ベースカラーだけ sRGB として読む。それ以外はリニア。
        constants.textureIndices0[0] =
            (material != nullptr) ? textures.SrvIndex(material->baseColor, true)
                                  : kInvalidTextureIndex;
        constants.textureIndices0[1] =
            (material != nullptr) ? textures.SrvIndex(material->normal, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices0[2] =
            (material != nullptr) ? textures.SrvIndex(material->roughness.texture, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices0[3] =
            (material != nullptr) ? textures.SrvIndex(material->metallic.texture, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices1[0] =
            (material != nullptr)
                ? textures.SrvIndex(material->ambientOcclusion.texture, false)
                : kInvalidTextureIndex;
        // ハイトはマテリアルのハイトマップから引く。マテリアルが無ければ定数 / ノイズ。
        constants.textureIndices1[1] =
            (material != nullptr) ? textures.SrvIndex(material->height.texture, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices1[2] =
            (material != nullptr) ? textures.SrvIndex(material->opacity.texture, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices1[3] = kInvalidTextureIndex;

        // スカラーのマップは「どのチャンネルを読むか」も渡す。
        // Megascans の _ORD のように 1 枚へ詰めたテクスチャに対応するため。
        constants.mapChannels[0] = (material != nullptr) ? PackMaterialChannels(*material) : 0u;

        constants.noiseTypes[0] = static_cast<uint32_t>(layer.heightNoise.type);

        // 無効なレイヤーは合成しない。
        if (layer.enabled) {
            commandList->SetPipelineState(layerPipeline);

            for (const TileRect& tile : tiles) {
                LayerConstants tileConstants = constants;
                tileConstants.tile[0] = tile.x;
                tileConstants.tile[1] = tile.y;
                tileConstants.tile[2] = tile.width;
                tileConstants.tile[3] = tile.height;

                const rhi::UploadAllocation cb =
                    AllocateConstants(device, sizeof(LayerConstants));
                if (!cb.IsValid()) {
                    complete = false;
                    break;
                }
                std::memcpy(cb.cpu, &tileConstants, sizeof(tileConstants));

                commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
                commandList->Dispatch(DispatchCount(tile.width), DispatchCount(tile.height), 1);
            }

            // 次のレイヤーは前のレイヤーの結果を読むので、必ず区切る。
            const D3D12_RESOURCE_BARRIER barriers[] = {
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.baseColor.resource.Get()),
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.normal.resource.Get()),
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.surface.resource.Get()),
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.height.resource.Get()),
            };
            commandList->ResourceBarrier(_countof(barriers), barriers);

            ++m_evaluatedLayerCount;
        }

        BakeLayerThumbnail(device, layerThumbnailPipeline, commandList, stack, layerIndex);
    }

    // 読み取りへ。**ここでは NON_PIXEL までしか遷移させない。** コンピュートキューでは
    // PIXEL_SHADER_RESOURCE を含む遷移を記録できないため、描画から読める状態への
    // 遷移はグラフィックス側（TransitionForDisplay）で行う。
    constexpr D3D12_RESOURCE_STATES kOutputReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    TransitionIfNeeded(commandList, m_textures.baseColor, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.normal, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.surface, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.height, kOutputReadState);

    // サムネイルも同じ理由で NON_PIXEL まで。表側へ入れ替えたときに Update が
    // グラフィックス側で PIXEL へ遷移させる。
    for (rhi::GpuTexture& thumbnail : m_layerThumbnails) {
        TransitionIfNeeded(commandList, thumbnail, kOutputReadState);
    }

    PIXEndEvent(commandList);
    return complete;
}

// 毎フレームの駆動。
//
//   1. 終わった評価があれば回収する（裏側を表側へ入れ替え、描画で読める状態へ）。
//   2. スタックが変わっていて、評価が走っていなければ次を投入する。
//
// 走っている最中に何度編集されても、投入は 1 本ずつ。終わった時点で最新の版を
// 評価し直すので、途中の版は飛ばされる（GPU の仕事は途中で止められない）。
void MaterialEvaluator::Update(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                               ID3D12GraphicsCommandList* commandList,
                               const MaterialStack& stack, const TextureLibrary& textures,
                               const MaterialLibrary& materials) {
    if (!m_textures.IsValid()) {
        return;
    }
    // --- 回収 -----------------------------------------------------------------
    if (m_asyncInFlight && !m_compute.IsBusy()) {
        m_asyncInFlight = false;
        // 裏側に新しい結果が入った。表側と入れ替える。古い表側は次の評価先になる。
        // まだ描画中のフレームが古い表側を読んでいるかもしれないが、次の評価は
        // 投入時のフレームを GPU 側で待ってから走るので、書き込みが追い越すことはない。
        std::swap(m_textures, m_frontTextures);
        std::swap(m_layerThumbnails, m_frontLayerThumbnails);
        m_evaluatedRevision = m_asyncRevision;
        m_hasResult = true;
        TransitionForDisplay(commandList, m_frontTextures);
        TransitionThumbnailsForDisplay(commandList, m_frontLayerThumbnails);
    }

    if (m_evaluatedRevision == stack.Revision() || m_asyncInFlight) {
        return;
    }

    const std::vector<TileRect> tiles = MakeTiles();
    const bool canRunAsync = m_asynchronous && m_compute.IsValid() && m_frontTextures.IsValid();

    // --- 同期評価 ---------------------------------------------------------------
    // 結果がまだ 1 つも無いとき（起動直後、解像度変更の直後）はその場で評価する。
    // 前回の絵を出しておけないので、非同期にすると最初のフレームがゴミになる。
    // キューが作れなかったときもここ（従来どおりフレームの中で評価する）。
    if (!canRunAsync || !m_hasResult) {
        // 途中で定数バッファが確保できなかった場合などは「評価済み」にせず、
        // 次のフレームで評価し直す（タイルの継ぎ目が残ったまま確定するのを防ぐ）。
        if (Evaluate(device, pipelineCache, commandList, stack, textures, materials, tiles)) {
            m_evaluatedRevision = stack.Revision();
            if (m_frontTextures.IsValid()) {
                std::swap(m_textures, m_frontTextures);
                std::swap(m_layerThumbnails, m_frontLayerThumbnails);
            }
            m_hasResult = true;
            TransitionForDisplay(commandList, m_frontTextures.IsValid() ? m_frontTextures
                                                                        : m_textures);
            TransitionThumbnailsForDisplay(commandList, m_frontTextures.IsValid()
                                                            ? m_frontLayerThumbnails
                                                            : m_layerThumbnails);
        }
        return;
    }

    // --- 非同期評価 -------------------------------------------------------------
    // 前回の記録で定数の置き場を使い切っていたら、倍に広げてから記録する。
    if (m_compute.UploadExhausted()) {
        const uint64_t bytes = m_compute.UploadBytes() * 2;
        TG_LOG_INFO("合成の評価の定数の置き場を %llu KB へ広げます",
                    static_cast<unsigned long long>(bytes / 1024));
        m_compute.Destroy(device);
        if (!m_compute.Create(device, bytes, L"MaterialEvaluatorCompute")) {
            m_compute.Destroy(device);
            return;
        }
    }

    // 評価先（裏側）は前回まで描画が読んでいた組。PIXEL を含む状態からの遷移は
    // コンピュートキューでは記録できないので、**このフレームのリストで** UAV へ戻す。
    // キューはこのフレームの完了を待ってから走るので、順序は保たれる。
    TransitionIfNeeded(commandList, m_textures.baseColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.normal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // ノード用のサムネイルも同じ（裏側は前回まで ImGui が読んでいた PIXEL の状態）。
    for (rhi::GpuTexture& thumbnail : m_layerThumbnails) {
        TransitionIfNeeded(commandList, thumbnail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    ID3D12GraphicsCommandList* computeList = m_compute.Begin(device);
    if (computeList == nullptr) {
        return;
    }

    // 記録が途中で失敗したら投入しない。
    m_recordingAsync = true;
    const bool recorded =
        Evaluate(device, pipelineCache, computeList, stack, textures, materials, tiles);
    m_recordingAsync = false;
    if (!recorded) {
        m_compute.Abort();
        return;
    }
    if (!m_compute.Submit(device)) {
        return;
    }
    // 評価が参照しているテクスチャ（素材、サムネイル）を、評価が終わる前に
    // 解放しないよう Device に知らせる。
    device.SetAuxiliaryFence(m_compute.Fence(), m_compute.SubmittedValue());
    m_asyncInFlight = true;
    m_asyncRevision = stack.Revision();
}

}  // namespace tg::compositor
