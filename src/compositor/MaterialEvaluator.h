#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/MaterialStack.h"
#include "compositor/TextureLibrary.h"

#include <vector>
#include "rhi/ComputeQueue.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace tg::compositor {

// 合成結果のチャンネルセット。plan.md の定義に対応する。
struct MaterialTextureSet {
    rhi::GpuTexture baseColor;  // R11G11B10_FLOAT
    rhi::GpuTexture normal;     // R16G16_FLOAT（xy のみ、z は再構成）
    rhi::GpuTexture surface;    // R8G8B8A8_UNORM（R=Roughness, G=Metallic, B=AO, A=不透明度）
    // R32_FLOAT。R16 だと 0〜1 の全幅が標高差なので、600 m の地形で 1 ULP が約 0.3 m。
    // ここから作り直す法線が階段にならないよう 32bit にしてある。
    rhi::GpuTexture height;

    bool IsValid() const { return baseColor.IsValid(); }
};

// 評価する出力領域。全体を 1 回で評価するときは矩形に全体を渡す。
struct TileRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// レイヤースタックを GPU で評価する。
//
// 評価は「出力タイル矩形と解像度」を引数に取る形で固定する。
// 編集中はプレビュー解像度で全体を 1 パス、エクスポート時はフル解像度を
// タイル分割して順に評価する、という二段構えを最初から通すため。
//
// **プレビューの評価は非同期。** `asynchronous` で作ると出力を 2 組持ち、
// 評価は専用のコンピュートキュー（`rhi::ComputeQueue`）へ流す。描画は前回の結果
// （表側）を読み続け、終わった時点で裏側と入れ替える（`Update`）。
// 同期で作ると `Evaluate` を直接呼び、1 組だけ持つ。
class MaterialEvaluator {
public:
    // asynchronous: 出力を 2 組持ち、評価をコンピュートキューへ流す（プレビュー用）。
    bool Create(rhi::Device& device, uint32_t resolution, bool asynchronous = false);
    void Destroy(rhi::Device& device);

    bool Resize(rhi::Device& device, uint32_t resolution);

    // 指定したタイル群を評価する。呼び出し前後の状態遷移もここで行う。
    //
    // ループはレイヤー優先。1 レイヤーぶんを全タイルで終えてから次へ進む。
    // 全レイヤー・全タイルを記録できたら true。false のときは結果が半端なので、
    // 呼び出し側は「評価済み」にせず、次のフレームで評価し直すこと。
    bool Evaluate(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                  ID3D12GraphicsCommandList* commandList, const MaterialStack& stack,
                  const TextureLibrary& textures, const MaterialLibrary& materials,
                  const std::vector<TileRect>& tiles);

    // 毎フレーム呼ぶ。スタックに変更があれば評価を投入し、終わった評価があれば
    // 結果を表側へ入れ替える。全体はタイルに分割して評価する。エクスポート時と
    // 同じ経路を常に通しておくことで、タイル評価が壊れたままになるのを防ぐ。
    //
    // 非同期で作ってあれば評価はコンピュートキューへ流し、commandList には
    // 引き渡しの状態遷移だけを記録する。まだ結果が 1 つも無いとき（起動直後や
    // 解像度変更の直後）だけは、その場で同期評価して最初のフレームから絵を出す。
    void Update(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const MaterialStack& stack,
                const TextureLibrary& textures, const MaterialLibrary& materials);

    // 非同期の評価が走っている最中か（UI の「評価中」表示と、開発用の撮影の待ちに使う）。
    bool IsEvaluating() const;
    // 走っている評価の完了を CPU で待つ。
    void WaitForEvaluation();

    uint32_t TileSize() const { return m_tileSize; }

    void SetTileSize(uint32_t tileSize) { m_tileSize = (tileSize > 0) ? tileSize : 1; }
    uint32_t EvaluatedTileCount() const { return m_evaluatedTileCount; }

    // 描画が読む結果。非同期なら表側（評価済みで入れ替えたもの）、同期なら評価先そのもの。
    const MaterialTextureSet& Textures() const {
        return m_frontTextures.IsValid() ? m_frontTextures : m_textures;
    }
    uint32_t Resolution() const { return m_resolution; }
    uint32_t EvaluatedLayerCount() const { return m_evaluatedLayerCount; }

    // --- ノードの結果サムネイル ------------------------------------------
    // そのレイヤーまで合成した結果（アルベド + Height の勾配の陰影）の 64² のサムネイル。
    // 添字は評価したスタックのレイヤーの添字。評価は非同期なので、
    // 表側の結果がどの版のスタックかは EvaluatedRevision() で見分ける。
    // ImGui へ渡すハンドル。まだ無ければ ptr が 0。
    D3D12_GPU_DESCRIPTOR_HANDLE LayerThumbnailHandle(size_t layerIndex) const;
    // 表側の結果が、どの版のスタックを評価したものか。
    uint64_t EvaluatedRevision() const { return m_evaluatedRevision; }

    // 変更を検知していなくても次回に評価し直す。
    void Invalidate() { m_evaluatedRevision = 0; }

private:
    void ReleaseTextures(rhi::Device& device);
    // レイヤーの数ぶんの結果サムネイル（評価先＝裏側）を用意する。
    void EnsureLayerThumbnails(rhi::Device& device, size_t layerCount);
    // そのレイヤーまで合成した BaseColor と Height から結果サムネイルを焼く。
    // 合成ループの中で、レイヤーを走らせた直後に呼ぶ。
    void BakeLayerThumbnail(rhi::Device& device, ID3D12PipelineState* pipeline,
                            ID3D12GraphicsCommandList* commandList, const MaterialStack& stack,
                            size_t layerIndex);
    const std::vector<rhi::GpuTexture>& DisplayedLayerThumbnails() const {
        return m_frontTextures.IsValid() ? m_frontLayerThumbnails : m_layerThumbnails;
    }

    // 定数バッファの置き場。コンピュートキューへ記録している間はそのキューの
    // 置き場から、それ以外（同期評価）はフレームのアップロードリングから取る。
    // **評価器の中で device.Upload() を直接呼ばない**（キューの仕事はフレームより長生きする）。
    rhi::UploadAllocation AllocateConstants(rhi::Device& device, uint64_t size);
    // 全体をタイルに分ける。
    std::vector<TileRect> MakeTiles() const;
    // 描画（頂点 / ドメイン / ピクセル）から読める状態へ。**グラフィックスキューでだけ**
    // 記録できる（PIXEL_SHADER_RESOURCE はコンピュートキューでは使えない）。
    void TransitionForDisplay(ID3D12GraphicsCommandList* commandList, MaterialTextureSet& set);
    // ノード用のサムネイルも ImGui（ピクセルシェーダ）が読むので同じ扱い。
    void TransitionThumbnailsForDisplay(ID3D12GraphicsCommandList* commandList,
                                        std::vector<rhi::GpuTexture>& thumbnails);

    // 評価先。非同期のときは裏側で、終わったら m_frontTextures と入れ替わる。
    MaterialTextureSet m_textures;
    // 描画が読む表側。同期のときは持たない。
    MaterialTextureSet m_frontTextures;
    // レイヤーごとの結果サムネイル（RGBA8）。評価先（裏側）と描画が読む表側を、
    // 合成結果と一緒に入れ替える（評価中に ImGui が読む側へ書かないため）。
    std::vector<rhi::GpuTexture> m_layerThumbnails;
    std::vector<rhi::GpuTexture> m_frontLayerThumbnails;
    uint32_t m_resolution = 0;
    uint64_t m_evaluatedRevision = 0;
    uint32_t m_evaluatedLayerCount = 0;
    uint32_t m_evaluatedTileCount = 0;
    uint32_t m_tileSize = 512;

    // --- 非同期評価 ---------------------------------------------------------
    rhi::ComputeQueue m_compute;
    bool m_asynchronous = false;
    // いまコンピュートキューへ記録している最中か（AllocateConstants の振り分け）。
    bool m_recordingAsync = false;
    // 投入済みで、まだ結果を回収していない評価があるか。
    bool m_asyncInFlight = false;
    // 投入した評価が対応するスタックの版。回収時に m_evaluatedRevision へ写す。
    uint64_t m_asyncRevision = 0;
    // 表側に描ける結果があるか。無いうちは同期で評価する。
    bool m_hasResult = false;
};

}  // namespace tg::compositor
