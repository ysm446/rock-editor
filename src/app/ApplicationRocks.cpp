// 山グラフの岩（Rock / Rock Scatter）。焼いた岩アセットを読み、撒いた岩をインスタンス描画する。
//
// 岩アセットは岩グラフの付属フォルダ（<シーン>.bake）から読み、モデル（renderer::ModelAsset）の形にして
// ModelPreview のインスタンス描画で描く。段（LOD）は毎フレーム、インスタンスごとに画面上の大きさで選び、
// 行列をフレームごとのアップロードバッファへ段ごとにまとめて並べる。
// 仕様は docs/reference/rock-scatter.md。

#include "app/Application.h"

#include "core/ImageIo.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include "io/RockAssetIo.h"
#include "renderer/RockMesh.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rock {
using namespace DirectX;

namespace {
// これより画面上で小さい岩は描かない（岩を包む球の直径が画面の高さに占める割合）。
constexpr float kRockCullScreenSize = 0.002f;
}  // namespace

void Application::ReleaseRockAssets() {
    for (auto& [scene, asset] : m_rockAssets) {
        if (asset.gpu) asset.gpu->Destroy(m_device);
        if (asset.model.materials.size() && asset.model.materials[0]) m_materialLibrary.Remove(m_device, asset.model.materials[0]);
        for (const auto texture : asset.textures)
            if (texture) m_textureLibrary.Remove(m_device, texture);
    }
    m_rockAssets.clear();
    m_rockInstanceSets.clear();
    m_rockDraws.clear();
}

// 岩アセットを読む（フレームの外で呼ぶ。GPU への転送を伴う）。目録が更新されていれば読み直す。
Application::LoadedRockAsset* Application::RockAssetFor(const std::string& scene) {
    const auto path = FromUtf8(scene);
    std::error_code fileError;
    const auto time = std::filesystem::last_write_time(io::RockAssetFolder(path) / L"asset.json", fileError);
    auto found = m_rockAssets.find(scene);
    if (found != m_rockAssets.end() && found->second.time == time && !fileError == found->second.exists) return &found->second;
    // 読み直す。前の GPU メッシュと材質は捨てる。
    if (found != m_rockAssets.end()) {
        auto& old = found->second;
        if (old.gpu) old.gpu->Destroy(m_device);
        if (old.model.materials.size() && old.model.materials[0]) m_materialLibrary.Remove(m_device, old.model.materials[0]);
        for (const auto texture : old.textures)
            if (texture) m_textureLibrary.Remove(m_device, texture);
        m_rockAssets.erase(found);
    }
    LoadedRockAsset& asset = m_rockAssets[scene];
    asset.time = time;
    asset.exists = !fileError;
    io::RockAssetData data;
    std::string error;
    if (!io::LoadRockAsset(path, data, error)) {
        asset.error = error;
        return &asset;
    }
    // 形。段ごとに部品 1 つ（ノード 1 つ、スロット 1 つ）のモデルにする。
    auto geometry = std::make_shared<renderer::ModelGeometry>();
    geometry->nodes.push_back({"Rock", -1, {}});
    XMStoreFloat4x4(&geometry->nodes[0].bindLocal, XMMatrixIdentity());
    geometry->slots.push_back({"Rock", {}, {1.0f, 1.0f, 1.0f}, 1.0f});
    geometry->minimum = {data.minimum.x, data.minimum.y, data.minimum.z};
    geometry->maximum = {data.maximum.x, data.maximum.y, data.maximum.z};
    for (const auto& lod : data.lods) {
        renderer::ModelLod level;
        renderer::ModelPart part;
        part.mesh = renderer::MakeRockMeshData(lod.mesh, m_settings.Display().smoothShading, m_settings.Display().smoothShadingAngle);
        part.minimum = geometry->minimum;
        part.maximum = geometry->maximum;
        level.triangles = uint32_t(lod.mesh.triangles.size());
        level.parts.push_back(std::move(part));
        geometry->lods.push_back(std::move(level));
        asset.screenSizes.push_back(lod.screenSize);
        asset.triangles.push_back(lod.mesh.triangles.size());
    }
    asset.model.name = ToUtf8Display(path.stem());
    asset.model.path = path;
    asset.model.geometry = geometry;
    asset.textured = data.textured;
    // 材質。焼いたテクスチャ 4 枚を一時のテクスチャと材質にする（シーンには保存しない）。
    compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
    if (data.textured) {
        const auto folder = io::RockAssetFolder(path);
        for (size_t channel = 0; channel < 4; ++channel) {
            LdrImage image;
            if (!LoadLdrImage(folder / io::kRockAssetTextures[channel], image)) break;
            asset.textures.push_back(m_textureLibrary.AddTransient(
                m_device, m_pipelineCache, "Rock " + asset.model.name + " " + io::kRockAssetTextures[channel] + "（一時）", image));
        }
        if (asset.textures.size() == 4 && std::all_of(asset.textures.begin(), asset.textures.end(), [](auto id) { return id != 0; })) {
            material = m_materialLibrary.Add("岩 " + asset.model.name + "（一時）");
            auto* m = m_materialLibrary.FindMutable(material);
            m->transient = true;
            m->baseColor = asset.textures[0];
            m->normal = asset.textures[1];
            m->flipNormalGreen = false;
            m->roughness = {asset.textures[2], compositor::TextureChannel::R};
            m->roughnessValue = 1;
            m->metallic = {asset.textures[2], compositor::TextureChannel::G};
            m->metallicValue = 1;
            m->ambientOcclusion = {asset.textures[2], compositor::TextureChannel::B};
            m->ambientOcclusionValue = 1;
            m->height = {asset.textures[3], compositor::TextureChannel::R};
        } else {
            asset.error = "岩アセットのテクスチャを読めません（形だけで描きます）";
        }
    }
    asset.model.materials = {material};
    asset.gpu = std::make_unique<renderer::ModelPreview>(16);
    if (!asset.gpu->PrepareAllLods(m_device, asset.model)) {
        asset.error = "岩アセットを GPU へ転送できません";
        asset.gpu.reset();
        return &asset;
    }
    const XMFLOAT3 extent{geometry->maximum.x - geometry->minimum.x, geometry->maximum.y - geometry->minimum.y,
                          geometry->maximum.z - geometry->minimum.z};
    asset.radius = 0.5f * std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z);
    asset.height = extent.y;
    asset.loaded = true;
    return &asset;
}

// SyncMeshGraph が評価の結果を受け取ったときに呼ぶ。撒いた岩を覚え、使う岩アセットを読む。
void Application::SyncRockInstances(const std::vector<graph::RockInstanceSet>& sets) {
    m_rockInstanceSets = sets;
    m_rockInstancesRadius = 0.0f;
    for (const auto& set : m_rockInstanceSets) {
        const LoadedRockAsset* asset = RockAssetFor(set.scene);
        if (asset == nullptr || !asset->loaded) continue;
        for (const auto& instance : set.instances) {
            const auto& p = instance.position;
            const float reach = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) + asset->radius * instance.scale * set.scale;
            m_rockInstancesRadius = std::max(m_rockInstancesRadius, reach);
        }
    }
    m_rockInstanceFrame = UINT64_MAX;
}

// インスタンスのワールド行列（行ベクトルの規約）。
// 底面の中心を原点へ → 倍率 → 高さの embed 分だけ沈める → 上向きのまわりに回す → 上向きへ傾ける → 置く点へ。
static XMMATRIX RockInstanceWorld(const renderer::ModelAsset& model, const geometry::RockInstance& instance, float scale,
                                  float height) {
    const XMVECTOR up = XMVector3Normalize(XMVectorSet(instance.up.x, instance.up.y, instance.up.z, 0.0f));
    const XMVECTOR y = XMVectorSet(0, 1, 0, 0);
    XMMATRIX align = XMMatrixIdentity();
    const float cosine = XMVectorGetX(XMVector3Dot(y, up));
    if (cosine < 0.9999f) {
        const XMVECTOR axis = XMVector3Normalize(XMVector3Cross(y, up));
        align = XMMatrixRotationAxis(axis, std::acos(std::clamp(cosine, -1.0f, 1.0f)));
    }
    return renderer::ModelPivotMatrix(model) * XMMatrixScaling(scale, scale, scale) *
           XMMatrixTranslation(0.0f, -instance.embed * height * scale, 0.0f) * XMMatrixRotationY(instance.yaw) * align *
           XMMatrixTranslation(instance.position.x, instance.position.y, instance.position.z);
}

// 1 フレームに 1 回、カメラから見た大きさで段を選び、行列を段ごとにまとめてバッファへ並べる。
void Application::BuildRockInstanceBatches() {
    m_rockDraws.clear();
    m_rockInstanceStats = {};
    size_t total = 0;
    for (const auto& set : m_rockInstanceSets) total += set.instances.size();
    if (total == 0) return;
    // バッファ（フレームごと）。足りなければ作り直す。
    const uint32_t frame = m_device.FrameIndex();
    auto& buffer = m_rockInstanceBuffers[frame];
    if (!buffer.IsValid() || m_rockInstanceCapacity[frame] < total) {
        if (buffer.IsValid()) m_device.DeferRelease(buffer);
        const uint32_t capacity = uint32_t(std::max<size_t>(total + total / 4, 1024));
        if (!m_device.Allocator().CreateUploadStructuredBuffer(capacity, sizeof(XMFLOAT4X4), L"RockInstances", buffer)) {
            m_rockInstanceCapacity[frame] = 0;
            return;
        }
        m_rockInstanceCapacity[frame] = capacity;
    }
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(buffer.resource->Map(0, &noRead, &mapped))) return;
    auto* matrices = static_cast<XMFLOAT4X4*>(mapped);
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMFLOAT3 eye = camera.Position();
    const float tangent = std::tan(camera.FovY() * 0.5f);
    uint32_t cursor = 0;
    for (const auto& set : m_rockInstanceSets) {
        const auto found = m_rockAssets.find(set.scene);
        if (found == m_rockAssets.end() || !found->second.loaded || !found->second.gpu) continue;
        const LoadedRockAsset& asset = found->second;
        const size_t lods = asset.screenSizes.size();
        // 段ごとの行列（このフレームの分）。
        std::vector<std::vector<XMFLOAT4X4>> perLod(lods);
        for (const auto& instance : set.instances) {
            const float scale = instance.scale * set.scale;
            // 岩を包む球。中心は置いた点から上向きへ、沈めた分を引いた高さの半分。
            const float radius = asset.radius * scale;
            const float lift = asset.height * scale * (0.5f - instance.embed);
            const float cx = instance.position.x + instance.up.x * lift, cy = instance.position.y + instance.up.y * lift,
                        cz = instance.position.z + instance.up.z * lift;
            const float dx = eye.x - cx, dy = eye.y - cy, dz = eye.z - cz;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float size = distance <= radius ? 10.0f : radius / (distance * tangent);
            if (size < kRockCullScreenSize) {
                ++m_rockInstanceStats.culled;
                continue;
            }
            size_t lod = 0;
            for (size_t level = 1; level < lods; ++level)
                if (size < asset.screenSizes[level]) lod = level;
            XMFLOAT4X4 world;
            XMStoreFloat4x4(&world, XMMatrixTranspose(RockInstanceWorld(asset.model, instance, scale, asset.height)));
            perLod[lod].push_back(world);
        }
        RockDraw draw;
        draw.scene = set.scene;
        for (size_t lod = 0; lod < lods; ++lod) {
            if (perLod[lod].empty()) continue;
            std::memcpy(matrices + cursor, perLod[lod].data(), perLod[lod].size() * sizeof(XMFLOAT4X4));
            draw.batches.push_back({int(lod), cursor, uint32_t(perLod[lod].size())});
            cursor += uint32_t(perLod[lod].size());
            if (m_rockInstanceStats.perLod.size() <= lod) m_rockInstanceStats.perLod.resize(lod + 1, 0);
            m_rockInstanceStats.perLod[lod] += perLod[lod].size();
            m_rockInstanceStats.drawn += perLod[lod].size();
        }
        if (!draw.batches.empty()) m_rockDraws.push_back(std::move(draw));
    }
    buffer.resource->Unmap(0, nullptr);
    m_rockInstanceBufferSrv = buffer.srv.index;
}

// レンダラの本描画・シャドウパスから呼ばれる。フレームの最初の呼び出しで段を選び直す。
void Application::DrawRockInstances(ID3D12GraphicsCommandList* commandList, const renderer::SceneDrawContext& context) {
    if (m_rockInstanceSets.empty()) return;
    if (m_rockInstanceFrame != m_frameCounter) {
        BuildRockInstanceBatches();
        m_rockInstanceFrame = m_frameCounter;
    }
    for (const auto& draw : m_rockDraws) {
        const auto found = m_rockAssets.find(draw.scene);
        if (found == m_rockAssets.end() || !found->second.gpu) continue;
        found->second.gpu->RenderInstancedInScene(m_device, m_pipelineCache, commandList, found->second.model, m_materialLibrary,
                                                  m_textureLibrary, context, m_rockInstanceBufferSrv, draw.batches);
    }
}

}  // namespace rock
