#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/TextureLibrary.h"
#include "graph/NodeGraph.h"
#include "io/ProjectWorkspace.h"
#include "renderer/ModelAsset.h"
#include "renderer/PreviewRenderer.h"
#include "renderer/SkyLibrary.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <filesystem>

// プロジェクトとマテリアルのファイル入出力。
//
// 形式の仕様は docs/reference/file-format.md にある。変更したらそちらも直すこと。
namespace tg::io {

// 保存・読み込みの対象。Application が持っているものへの参照をまとめたもの。
// 合成の構造はグラフが唯一の持ち主（旧形式の layers[] は読み込み時にグラフへ移行する）。
struct ProjectRefs {
    compositor::TextureLibrary& textures;
    compositor::MaterialLibrary& materials;
    renderer::SkyLibrary& skies;
    renderer::PreviewRenderer& renderer;
    graph::NodeGraph& graph;
    // モデル（.tgmodel）。渡さなければモデルは読み書きしない。
    std::vector<renderer::ModelAsset>* models = nullptr;
};

// --- プロジェクト (.tgproj) -----------------------------------------------
//
// マテリアルの構造は丸ごと埋め込む。開くのに別のマテリアルファイルは要らない。
// テクスチャの画像だけは参照で持ち、パスはプロジェクトからの相対で書く。
//
// 読み込みは GPU 待機を伴うため、**フレームの外で呼ぶこと。**

// workspace を渡すとシーン (.tgscene) として扱う。マテリアルと天球は共有アセット
// （`.tgmat` / `.tgsky`）へ分離し、画像はルート内へ取り込んで ID で参照する。
// 渡さなければ従来の `.tgproj`（埋め込み・相対パス）をそのまま読み書きする。
bool SaveProject(const std::filesystem::path& path, const ProjectRefs& refs,
                 ProjectWorkspace* workspace = nullptr);
bool LoadProject(const std::filesystem::path& path, rhi::Device& device,
                 rhi::PipelineCache& pipelineCache, const ProjectRefs& refs,
                 ProjectWorkspace* workspace = nullptr);

// --- 共有アセット（ルート内の .tgmat / .tgsky / .tgmodel） ---------------
//
// 読み込み済みのマテリアル・天球・モデルをそれぞれのファイルへ書く。
// 置き場所が未定のものは `Materials/` / `Skies/` / `Models/` に名前から作る。
// ただし ID を持たないものは、同じ中身の既存アセットがあればそれへ書く
// （保存し直すたびに連番の複製を作らない）。
// シーンの保存はこれを先に行う。
bool SaveSharedAssets(ProjectWorkspace& workspace, const ProjectRefs& refs);
// 共有アセット 1 つを現在のライブラリへ足す。同じ ID がすでにあれば足さずにそれを使う
// （天球は適用する）。参照している画像もその場で読み込む。
// rescan を false にすると、ルートを走査し直さず手持ちの ID 表で解決する（サムネイルの連続生成用）。
// .tgmodel は models へ足し（参照するマテリアルもライブラリへ）、models が無ければ失敗する。
bool LoadSharedAsset(ProjectWorkspace& workspace, const std::filesystem::path& path,
                     rhi::Device& device, rhi::PipelineCache& pipelineCache,
                     compositor::TextureLibrary& textures, compositor::MaterialLibrary& materials,
                     renderer::SkyLibrary& skies, bool rescan = true,
                     std::vector<renderer::ModelAsset>* models = nullptr);
// --- マテリアル単体 (.tgmat) ----------------------------------------------
//
// プロジェクト間でマテリアルを持ち回るための書き出し / 読み込み。
// テクスチャはこのファイルのある場所からの相対パスで参照する。
// 読み込みは既存のライブラリへ 1 つ追加する形で、他のマテリアルには触らない。

bool SaveMaterial(const std::filesystem::path& path, const compositor::MaterialAsset& asset,
                  const compositor::TextureLibrary& textures);
compositor::MaterialAssetId LoadMaterial(const std::filesystem::path& path, rhi::Device& device,
                                         rhi::PipelineCache& pipelineCache,
                                         compositor::TextureLibrary& textures,
                                         compositor::MaterialLibrary& materials);

}  // namespace tg::io
