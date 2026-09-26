#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/TextureLibrary.h"
#include "renderer/Environment.h"
#include "renderer/PreviewRenderer.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace rock::renderer {

// マテリアル 1 つを、回せる球で描く（マテリアルプレビューの窓が使う）。
//
// **照らし方はビューポートに合わせる**（適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。
// 素材が本番の環境でどう見えるかを確かめるためのもので、一覧のサムネイルとは目的が違う
// （あちらは見比べるための固定 2 灯で、正面から見た円板）。
class MaterialSphere {
public:
    void Destroy(rhi::Device& device);

    // 窓が開いている間、**フレームの中で**毎フレーム呼ぶ。
    // 出力は ImGui が読むので、ピクセルシェーダ可視の状態で置いて返す。
    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const compositor::MaterialAsset& asset,
                const compositor::TextureLibrary& textures, const Environment& environment,
                float iblIntensity, const LightSettings& light, float exposure,
                TonemapMode tonemap);

    bool HasOutput() const { return m_output.IsValid(); }
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }

    // 軌道の操作。ドラッグ量（度）とホイールの段で動かす。
    // **符号はビューポートのカメラと同じ**（右へ引けば右へ回る）。
    void Orbit(float deltaXDegrees, float deltaYDegrees);
    void Zoom(float steps);
    // 正面・既定の距離へ戻す。窓を開いた直後と「戻す」ボタンで使う。
    void ResetView();

    // 以下はどれも素材の見え方を確かめるための表示設定で、マテリアル自体には保存しない。
    // 変位もこのプレビューの中だけで、ノードの計算やビューポートの形には関係しない。

    // 形の 1 周（平面は一辺）に並べるタイル数。
    float& UvScale() { return m_uvScale; }
    // 0 球、1 平面。
    int& Shape() { return m_shape; }
    // 形の大きさ（m）。球は直径、平面は一辺。変位の高さをこれと比べて描く。
    float& SizeMeters() { return m_sizeMeters; }
    // 素材のハイト 0〜1 の差を何 m の凹凸にするか。0 なら変位しない。
    float& DisplacementMeters() { return m_displacementMeters; }

private:
    rhi::GpuTexture m_output;
    // 変位するときに素材のハイトを形の座標へ焼いたもの（R32_FLOAT）。
    rhi::GpuTexture m_heightField;

    float m_yawDegrees = 0.0f;
    float m_pitchDegrees = 12.0f;
    // 既定値は .cpp の kDefault* と揃える（ResetView が入れ直す値）。
    float m_distance = 4.5f;
    float m_uvScale = 2.0f;
    int m_shape = 0;
    float m_sizeMeters = 1.0f;
    float m_displacementMeters = 0.0f;
};

}  // namespace rock::renderer
