#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/TextureLibrary.h"
#include "io/ProjectWorkspace.h"
#include "io/ThumbnailStore.h"
#include "renderer/Environment.h"
#include "renderer/PreviewRenderer.h"
#include "renderer/SkyLibrary.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace tg {

// アセットの帯に出す、**まだシーンへ読み込んでいない**ファイルのサムネイル。
//
// 画像は CPU で縮小して転送する。マテリアルと天球は、シーンのライブラリとは別の
// 読み込み領域（m_textures / m_materials / m_skies）へ一時的に読み、既存の球の
// サムネイルを取り出して使う。モデル（.tgmodel / .fbx）は同じ領域へ読んで ModelPreview で描く。
// シーン側の一覧・ID・アンドゥには一切触れない。
//
// 生成はフレームの外で 1 フレームに 1 件だけ（GPU 待機と画像の読み込みを伴う）。
// ルートの走査はマテリアルごとには行わない（ルートの切り替えと Invalidate のときだけ）。
// 読み込んだ画像は、同じフォルダの続くマテリアルで使い回す（共有の _ORD や複製は同じ画像を指す）。
// 要求を作り終えたとき、または MaxScratchTextures 枚を超えたときに返す。
// 生成したものは `<ルート>/.terrain-graph/thumbnails/` へ PNG で残し、
// 次回はそれを読む（io/ThumbnailStore）。メモリには最大 MaxEntries 件を保持する。
class AssetThumbnailCache {
public:
    // 毎フレームの頭で呼ぶ。このフレームの要求を空にする。
    void BeginRequests();
    // 表示したいファイルを申告し、あれば SRV を返す（無ければ ptr が 0）。
    D3D12_GPU_DESCRIPTOR_HANDLE Request(const std::filesystem::path& path);
    bool Failed(const std::filesystem::path& path) const;
    // 申告されたのにまだ無いものがあるか。UI スクリーンショットの撮り時の判定に使う。
    bool HasPendingWork() const;
    // 次の Process で全部捨てて作り直す（「更新」・アセットの保存・削除のあと）。
    void Invalidate() { m_invalidate = true; }
    // フレームの外で呼ぶ。表示中のフォルダの要求から 1 件だけ作る。
    void Process(rhi::Device& device, rhi::PipelineCache& pipelines, io::ProjectWorkspace& workspace,
                 const std::filesystem::path& directory);
    void Destroy(rhi::Device& device);
    static bool Supports(const std::filesystem::path& path);
    // モデルのサムネイルの照らし方（ビューポートと同じ環境・太陽・露出）。Process の前に渡す。
    // environment は Process の間だけ参照する。
    struct ModelLighting {
        const renderer::Environment* environment = nullptr;
        float iblIntensity = 1.0f;
        renderer::LightSettings light;
        float exposure = 1.0f;
        renderer::TonemapMode tonemap{};
    };
    void SetModelLighting(const ModelLighting& lighting) { m_modelLighting = lighting; }

private:
    struct Entry {
        rhi::GpuTexture texture;
        uint64_t lastUsed = 0;
        bool failed = false;
    };
    void ClearScratch(rhi::Device& device, bool textures = true);
    bool BuildImage(rhi::Device& device, const std::filesystem::path& path, rhi::GpuTexture& output);
    bool BuildModel(rhi::Device& device, rhi::PipelineCache& pipelines, io::ProjectWorkspace& workspace,
                    const std::filesystem::path& path, rhi::GpuTexture& output);
    void Store(rhi::Device& device, const std::filesystem::path& path, rhi::GpuTexture texture, bool persist = true);

    std::unordered_map<std::filesystem::path, Entry> m_entries;
    std::vector<std::filesystem::path> m_requests;
    std::filesystem::path m_root;
    std::filesystem::path m_directory;
    io::ThumbnailRecord m_diskRecord;
    uint64_t m_frame = 0;
    bool m_invalidate = false;
    // 一覧専用の読み込み領域。シーンのライブラリとは別に持つ。
    compositor::TextureLibrary m_textures;
    compositor::MaterialLibrary m_materials;
    renderer::SkyLibrary m_skies;
    ModelLighting m_modelLighting;
};

}  // namespace tg
