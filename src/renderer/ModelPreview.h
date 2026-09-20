#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/TextureLibrary.h"
#include "renderer/Camera.h"
#include "renderer/Environment.h"
#include "renderer/Mesh.h"
#include "renderer/ModelAsset.h"
#include "renderer/PreviewRenderer.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <memory>
#include <utility>
#include <vector>

namespace rock::renderer {

// シーンへ置いたモデル 1 つぶん。ワールド行列と、ノードに足す回転（Model ノードの設定。無ければ読んだままの姿勢）。
struct ModelInstanceDraw {
    DirectX::XMFLOAT4X4 world{};
    const std::vector<ModelNodeRotation>* rotations = nullptr;
};

// モデル 1 つを回せるカメラで描く（モデルプレビューの窓と、アセットの帯のサムネイル）。
// rock-editor の ModelPreview から、配置（インスタンス描画）と大気を外したもの。
//
// **照らし方はビューポートに合わせる**（適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。
// マテリアルの合成モードも見る。マスク抜きはしきい値でくり抜き、半透明は不透明の後に重ねる。
class ModelPreview {
public:
    // 出力の一辺。窓は大きく、ディスクへ残すサムネイルは小さく作る。
    explicit ModelPreview(uint32_t outputSize = 1024) : m_outputSize(outputSize) {}

    void Destroy(rhi::Device& device);
    // GPU のメッシュを用意する。形状か LOD が変わったときだけ作り直す（GPU 待機を伴うのでフレームの外で呼ぶ）。
    bool Prepare(rhi::Device& device, const ModelAsset& asset, int lod);
    // 出力へ描く。**フレームの中で**呼ぶ。出力はピクセルシェーダ可視の状態で返す。
    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                const compositor::MaterialLibrary& materials,
                const compositor::TextureLibrary& textures, const Environment& environment,
                float iblIntensity, const LightSettings& light, float exposure,
                TonemapMode tonemap);

    // シーン（ビューポート）の本描画・シャドウパスの中で、置いたモデルを描く（PreviewRenderer::drawSceneExtras）。
    // instances は置いた数だけ。部品は「ノードの行列 × ワールド行列」で描く。
    // 本描画は線形 HDR を書き、露出とトーンマップはレンダラが掛ける。
    void RenderInScene(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                       ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                       const compositor::MaterialLibrary& materials, const compositor::TextureLibrary& textures,
                       const SceneDrawContext& context, const std::vector<ModelInstanceDraw>& instances);

    Camera& GetCamera() { return m_camera; }
    bool HasOutput() const { return m_output.IsValid(); }
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }
    rhi::GpuTexture TakeOutput() { return std::exchange(m_output, {}); }
    // 既定の角度で全体を収める。
    void ResetView();
    // 注視点をモデルの中心へ戻す（F キー）。
    void FocusView();
    // 注視点を中心へ戻し、全体が収まる距離へ寄せる（A キー）。
    void FrameView();

private:
    uint32_t m_outputSize = 1024;
    std::shared_ptr<const ModelGeometry> m_geometry;
    int m_lod = -1;
    std::vector<Mesh> m_meshes;
    rhi::GpuTexture m_output, m_depth;
    Camera m_camera;
};

}  // namespace rock::renderer
