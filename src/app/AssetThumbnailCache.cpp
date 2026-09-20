#include "app/AssetThumbnailCache.h"

#include "core/ColorSpace.h"
#include "core/ImageIo.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include "io/ProjectIo.h"
#include "renderer/ModelPreview.h"
#include "rhi/TextureReadback.h"

#include <pix3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <utility>

namespace tg {
namespace fs = std::filesystem;
namespace {

constexpr uint32_t ThumbnailSize = 128;
constexpr size_t MaxEntries = 128;
// 使い回す画像の上限。2K の EXR はミップ込みで 1 枚約 21MB、4K は約 85MB。
constexpr size_t MaxScratchTextures = 12;

std::string Extension(const fs::path& path) {
    auto extension = ToUtf8Portable(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return extension;
}

}  // namespace

bool AssetThumbnailCache::Supports(const fs::path& path) {
    const auto ext = Extension(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
           ext == ".exr" || ext == ".hdr" || ext == ".tgmat" || ext == ".tgsky" || ext == ".tgscene" ||
           ext == ".tglayer" || ext == ".tgboundary" || ext == ".tgmodel" || ext == ".fbx";
}

void AssetThumbnailCache::BeginRequests() {
    m_requests.clear();
    ++m_frame;
}

D3D12_GPU_DESCRIPTOR_HANDLE AssetThumbnailCache::Request(const fs::path& path) {
    if (!Supports(path)) return {};
    m_requests.push_back(path);
    const auto found = m_entries.find(path);
    if (found == m_entries.end()) return {};
    found->second.lastUsed = m_frame;
    return found->second.texture.srv.gpu;
}

bool AssetThumbnailCache::Failed(const fs::path& path) const {
    const auto found = m_entries.find(path);
    return found != m_entries.end() && found->second.failed;
}

bool AssetThumbnailCache::HasPendingWork() const {
    return std::any_of(m_requests.begin(), m_requests.end(),
                       [&](const auto& path) { return !m_entries.contains(path); });
}

void AssetThumbnailCache::ClearScratch(rhi::Device& device, bool textures) {
    m_materials.Destroy(device);
    m_skies.Destroy(device);
    if (textures) m_textures.Destroy(device);
}

void AssetThumbnailCache::Destroy(rhi::Device& device) {
    ClearScratch(device);
    for (auto& [path, entry] : m_entries) device.DeferRelease(entry.texture);
    m_entries.clear();
    m_requests.clear();
    m_root.clear();
    m_directory.clear();
    m_invalidate = false;
}

void AssetThumbnailCache::Store(rhi::Device& device, const fs::path& path, rhi::GpuTexture texture, bool persist) {
    if (m_entries.size() >= MaxEntries) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(), [](const auto& a, const auto& b) {
            return a.second.lastUsed < b.second.lastUsed;
        });
        device.DeferRelease(oldest->second.texture);
        m_entries.erase(oldest);
    }
    if (persist && texture.IsValid() && !m_diskRecord.image.empty()) {
        std::error_code error;
        fs::create_directories(m_diskRecord.image.parent_path(), error);
        if (!error && rhi::SaveTextureToPng(device, texture, m_diskRecord.image, ThumbnailSize))
            io::CommitThumbnail(m_diskRecord);
    }
    Entry entry;
    entry.failed = !texture.IsValid();
    entry.lastUsed = m_frame;
    entry.texture = std::move(texture);
    // 生成を試みて失敗したときだけ知らせる（保存前のシーンや、未保存のレイヤーマテリアルの画像が無いのは正常）。
    if (entry.failed && persist)
        TG_LOG_WARN("アセットのサムネイルを生成できません: %s", ToUtf8Display(path).c_str());
    m_entries.emplace(path, std::move(entry));
}

bool AssetThumbnailCache::BuildImage(rhi::Device& device, const fs::path& path, rhi::GpuTexture& output) {
    const auto extension = Extension(path);
    const bool hdr = extension == ".hdr", linear = hdr || extension == ".exr";
    LdrImage ldrImage;
    HdrImage hdrImage;
    if (linear ? !(hdr ? LoadHdrImage(path, hdrImage) : LoadExrImage(path, hdrImage)) : !LoadLdrImage(path, ldrImage))
        return false;
    const uint32_t width = linear ? hdrImage.width : ldrImage.width;
    const uint32_t height = linear ? hdrImage.height : ldrImage.height;
    if (!width || !height) return false;
    const float scale = float(ThumbnailSize) / float(std::max(width, height));
    const uint32_t scaledWidth = std::max(1u, uint32_t(float(width) * scale));
    const uint32_t scaledHeight = std::max(1u, uint32_t(float(height) * scale));
    const uint32_t offsetX = (ThumbnailSize - scaledWidth) / 2, offsetY = (ThumbnailSize - scaledHeight) / 2;
    std::vector<uint8_t> pixels(ThumbnailSize * ThumbnailSize * 4, 0);
    // HDRI だけ代表輝度で露出を揃える。EXR の通常テクスチャはリニア→sRGB のみ。
    const float exposure = hdr ? 0.18f / std::max(MedianSkyLuminance(hdrImage), 0.0001f) : 1.0f;
    for (uint32_t y = 0; y < scaledHeight; ++y) for (uint32_t x = 0; x < scaledWidth; ++x) {
        const uint32_t x0 = uint32_t(uint64_t(x) * width / scaledWidth);
        const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * width / scaledWidth));
        const uint32_t y0 = uint32_t(uint64_t(y) * height / scaledHeight);
        const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * height / scaledHeight));
        float sum[4]{};
        for (uint32_t sy = y0; sy < y1; ++sy) for (uint32_t sx = x0; sx < x1; ++sx) {
            const size_t source = (size_t(sy) * width + sx) * 4;
            for (size_t c = 0; c < 4; ++c) {
                const float value = linear ? hdrImage.pixels[source + c] : float(ldrImage.pixels[source + c]) / 255.0f;
                sum[c] += std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
            }
        }
        const float count = float(uint64_t(x1 - x0) * (y1 - y0));
        const size_t target = (size_t(y + offsetY) * ThumbnailSize + x + offsetX) * 4;
        for (size_t c = 0; c < 4; ++c) {
            float value = sum[c] / count;
            if (linear && c < 3) {
                value *= exposure;
                if (hdr) value = value / (1.0f + value);
                value = LinearToSrgb(value);
            }
            pixels[target + c] = uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    rhi::TextureDesc desc;
    desc.width = desc.height = ThumbnailSize;
    desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    desc.debugName = L"AssetImageThumbnail";
    if (!device.Allocator().CreateTexture2D(desc, output)) return false;
    const uint32_t rowPitch = ThumbnailSize * 4;
    rhi::GpuBuffer staging;
    if (!device.Allocator().CreateUploadBuffer(pixels.size(), L"AssetThumbnailUpload", staging)) return false;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &range, &mapped))) { device.DeferRelease(staging); return false; }
    std::memcpy(mapped, pixels.data(), pixels.size());
    staging.resource->Unmap(0, nullptr);
    const bool result = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        PIXBeginEvent(list, PIX_COLOR_DEFAULT, "AssetImageThumbnail");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, ThumbnailSize, ThumbnailSize, 1, rowPitch};
        const CD3DX12_TEXTURE_COPY_LOCATION source(staging.resource.Get(), footprint), target(output.resource.Get(), 0);
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        rhi::TransitionIfNeeded(list, output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        PIXEndEvent(list);
    });
    device.DeferRelease(staging);
    return result;
}

// モデルを一時の領域へ読み、斜め上からの全体を描く。.tgmodel は割り当てたマテリアルで、
// FBX 単体は灰色で描く。
bool AssetThumbnailCache::BuildModel(rhi::Device& device, rhi::PipelineCache& pipelines,
                                     io::ProjectWorkspace& workspace, const fs::path& path, rhi::GpuTexture& output) {
    std::vector<renderer::ModelAsset> models;
    if (Extension(path) == ".tgmodel") {
        if (!io::LoadSharedAsset(workspace, path, device, pipelines, m_textures, m_materials, m_skies, false, &models))
            return false;
    } else {
        renderer::ModelAsset model;
        if (!renderer::LoadModel(path, model)) return false;
        models.push_back(std::move(model));
    }
    if (models.empty() || !models.front().geometry) return false;
    // 縮小したときに細い部品が途切れないよう、残す大きさの 2 倍で描く。
    renderer::ModelPreview preview(ThumbnailSize * 2);
    bool rendered = false;
    if (preview.Prepare(device, models.front(), 0)) {
        const renderer::Environment unlit;
        const auto& lighting = m_modelLighting;
        rendered = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
            preview.Render(device, pipelines, list, models.front(), m_materials, m_textures,
                           lighting.environment ? *lighting.environment : unlit, lighting.iblIntensity, lighting.light,
                           lighting.exposure, lighting.tonemap);
        });
        output = preview.TakeOutput();
    }
    preview.Destroy(device);
    return rendered && output.IsValid();
}

void AssetThumbnailCache::Process(rhi::Device& device, rhi::PipelineCache& pipelines,
                                  io::ProjectWorkspace& workspace, const fs::path& directory) {
    if (m_invalidate || m_root != workspace.Root() || m_directory != directory) {
        // 走査はルートの切り替えと「更新」などの Invalidate のときだけ。フォルダの移動では手持ちの ID 表を使う。
        const bool rescan = m_invalidate || m_root != workspace.Root();
        Destroy(device);
        m_root = workspace.Root();
        m_directory = directory;
        if (rescan) workspace.Scan();
    }
    const auto request = std::find_if(m_requests.begin(), m_requests.end(), [&](const auto& path) {
        return path.parent_path() == directory && !m_entries.contains(path);
    });
    if (request == m_requests.end()) {
        // 表示中の要求を作り終えたら、使い回していた画像を返す。
        if (!m_textures.Entries().empty()) ClearScratch(device);
        return;
    }
    const auto path = *request;
    const auto extension = Extension(path);
    rhi::GpuTexture thumbnail;
    m_diskRecord = {};
    if (extension == ".tgscene") {
        const auto preview = io::SceneThumbnailPath(workspace, path);
        std::error_code error;
        if (fs::is_regular_file(preview, error)) BuildImage(device, preview, thumbnail);
        Store(device, path, std::move(thumbnail), false);
        // 保存前のシーンにプレビューが無いのは正常。次に保存すると生成される。
        m_entries[path].failed = false;
        return;
    }
    m_diskRecord = io::AssetThumbnailRecord(workspace, path);
    if (io::ThumbnailIsCurrent(m_diskRecord)) {
        if (BuildImage(device, m_diskRecord.image, thumbnail)) {
            Store(device, path, std::move(thumbnail), false);
            return;
        }
        device.DeferRelease(thumbnail);
    }
    if (extension == ".tglayer" || extension == ".tgboundary") {
        fs::path source;
        nlohmann::json body;
        if (extension == ".tgboundary" && workspace.ReadAsset(path, "boundary-material-asset", body))
            for (const char* slot : {"mask", "height"})
                if (source.empty() && body.contains(slot) && body[slot].is_object()) source = workspace.Resolve(body[slot]);
        if (!source.empty()) {
            // 境界マテリアルは境界マスク（無ければハイト）の画像そのもの。
            if (!BuildImage(device, source, thumbnail)) device.DeferRelease(thumbnail);
            Store(device, path, std::move(thumbnail));
            return;
        }
        // レイヤーマテリアルはここでは描画できない（道路の評価と描画が要る）。読み込んで保存したときに
        // アプリが残した画像（上の ThumbnailIsCurrent）だけを使い、無ければ帯が種類の文字を出す。
        Store(device, path, std::move(thumbnail), false);
        m_entries[path].failed = false;
        return;
    }
    if (extension == ".tgmodel" || extension == ".fbx") {
        ClearScratch(device, m_textures.Entries().size() >= MaxScratchTextures);
        if (!BuildModel(device, pipelines, workspace, path, thumbnail)) device.DeferRelease(thumbnail);
        Store(device, path, std::move(thumbnail));
        ClearScratch(device, false);
        return;
    }
    if (extension != ".tgmat" && extension != ".tgsky") {
        if (!BuildImage(device, path, thumbnail)) device.DeferRelease(thumbnail);
        Store(device, path, std::move(thumbnail));
        return;
    }
    ClearScratch(device, m_textures.Entries().size() >= MaxScratchTextures);
    bool loaded = false;
    nlohmann::json header;
    const auto format = io::ProjectWorkspace::ReadJson(path, header) ? io::ProjectWorkspace::String(header, "format") : "";
    if (extension == ".tgmat" && (format == "terrain-graph.material" || format == "material-mixer.material")) {
        // 持ち出し用の旧 .tgmat。相対パスの画像を読んでそのまま球を作る。
        loaded = io::LoadMaterial(path, device, pipelines, m_textures, m_materials) != compositor::kNoMaterialAsset;
    } else {
        loaded = io::LoadSharedAsset(workspace, path, device, pipelines, m_textures, m_materials, m_skies, false);
    }
    if (loaded && !m_materials.Entries().empty()) {
        m_materials.ProcessPendingWork(device, pipelines, m_textures);
        auto* material = m_materials.FindMutable(m_materials.Entries().front().id);
        thumbnail = std::exchange(material->thumbnail, {});
    } else if (loaded && !m_skies.Entries().empty()) {
        m_skies.ProcessPendingWork(device, pipelines);
        thumbnail = std::exchange(m_skies.ActiveMutable()->thumbnail, {});
    }
    Store(device, path, std::move(thumbnail));
    ClearScratch(device, false);
}

}  // namespace tg
