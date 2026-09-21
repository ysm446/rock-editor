#include "app/Application.h"
#include "renderer/MaterialBake.h"
#include "renderer/RockMesh.h"
#include <fstream>
#include <sstream>
#include <cstring>
namespace rock {
std::string Application::BakeFingerprint(const renderer::SceneMesh &mesh, const geometry::Mesh &input) const {
    uint64_t hash = 14695981039346656037ull;
    const auto bytes = [&](const void *data, size_t size) {
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
    };
    const auto add = [&](const auto &value) { bytes(&value, sizeof(value)); };
    const uint32_t version = 1;
    add(version);
    add(input.uvWidth);
    add(input.uvHeight);
    for (const auto &v : mesh.geometry.vertices) {
        add(v.position);
        add(v.normal);
        add(v.tangent);
        add(v.uv);
    }
    for (auto i : mesh.geometry.indices)
        add(i);
    add(mesh.mapping.method);
    add(mesh.mapping.offset);
    add(mesh.mapping.rotationDegrees);
    add(mesh.mapping.repeatMeters);
    add(mesh.mapping.sharpness);
    const auto texture = [&](compositor::TextureId id) {
        const auto *image = m_textureLibrary.Find(id);
        const bool valid = image && !image->missing;
        add(valid);
        if (!valid)
            return;
        // IDや絶対パスに依存させない。保存・ルート移動後も同じ画像なら一致する。
        std::ifstream stream(image->path, std::ios::binary);
        char buffer[16384];
        while (stream) {
            stream.read(buffer, sizeof(buffer));
            bytes(buffer, static_cast<size_t>(stream.gcount()));
        }
    };
    if (mesh.materialStack)
        for (const auto &layer : mesh.materialStack->Layers()) {
            add(layer.enabled);
            add(layer.channelMask);
            add(layer.uvScale);
            add(layer.heightSource);
            add(layer.heightBase);
            add(layer.heightGain);
            add(layer.heightNoise.type);
            add(layer.heightNoise.scale);
            add(layer.heightNoise.octaves);
            add(layer.heightNoise.offset);
            if (const auto *asset = m_materialLibrary.Find(layer.material)) {
                add(asset->baseColorTint);
                add(asset->hueShiftDegrees);
                add(asset->saturation);
                add(asset->brightness);
                add(asset->roughnessValue);
                add(asset->metallicValue);
                add(asset->ambientOcclusionValue);
                add(asset->flipNormalGreen);
                add(asset->opacityValue);
                texture(asset->baseColor);
                texture(asset->normal);
                for (auto slot : {asset->roughness, asset->metallic, asset->ambientOcclusion, asset->height,
                                  asset->opacity}) {
                    add(slot.channel);
                    texture(slot.texture);
                }
            } else {
                add(layer.baseColor);
                add(layer.roughness);
                add(layer.metallic);
                add(layer.ambientOcclusion);
            }
        }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

void Application::ApplyRockMaterial(renderer::SceneMesh &mesh, const graph::GeneratedRock &rock,
                                    bool useBaked) {
    const auto *surface = m_graph.FindNode(rock.materialSource);
    const auto *settings = surface ? std::get_if<graph::LayerNodeSettings>(&surface->settings) : nullptr;
    if (!settings)
        return;
    compositor::MaterialStack stack;
    stack.Layers() = m_graph.CompileLayersTo(surface->id).layers;
    for (auto &layer : stack.Layers())
        if (layer.material != compositor::kNoMaterialAsset) {
            layer.heightSource = compositor::ValueSource::Texture;
            layer.heightBase = compositor::kHeightPivot;
            layer.heightGain = 1;
        }
    stack.SetTerrainScale(settings->layer.mapping.repeatMeters, 0);
    mesh.materialStack = std::move(stack);
    mesh.mapping = settings->layer.mapping;
    if (!rock.bakeSource || !useBaked)
        return;
    const auto *node = m_graph.FindNode(rock.bakeSource);
    const auto *bake = node ? std::get_if<graph::MaterialBakeSettings>(&node->settings) : nullptr;
    bool ready = bake && !bake->fingerprint.empty() && bake->fingerprint == BakeFingerprint(mesh, rock.mesh);
    const auto *baked = ready ? m_materialLibrary.Find(bake->bakedLayer.material) : nullptr;
    ready &= baked != nullptr;
    if (baked)
        for (auto id : {baked->baseColor, baked->normal, baked->roughness.texture, baked->height.texture}) {
            const auto *image = m_textureLibrary.Find(id);
            ready &= image && !image->missing;
        }
    m_bakeStatus[rock.bakeSource] =
        ready ? "ベイク済み（UVで表示中）" : "再ベイクが必要です（元の材質で表示中）";
    if (ready) {
        compositor::MaterialStack bakedStack;
        bakedStack.Layers() = {bake->bakedLayer};
        bakedStack.SetTerrainScale(1, 0);
        mesh.materialStack = std::move(bakedStack);
        mesh.mapping = {};
    }
}

void Application::ProcessPendingBake() {
    if (!m_pendingBake)
        return;
    const auto id = m_pendingBake;
    m_pendingBake = 0;
    const auto fail = [&](const std::string &error) {
        m_bakeStatus[id] = error;
        ROCK_LOG_ERROR("Material Bake: %s", error.c_str());
    };
    auto *node = m_graph.FindMutableNode(id);
    if (!node || node->kind != graph::NodeKind::MaterialBake)
        return;
    const auto result = graph::EvaluateRocks(m_graph, id, &m_rockEvaluationCache);
    if (!result.error.empty() || result.rocks.size() != 1) {
        fail(result.error.empty() ? "ベイク入力がありません" : result.error);
        return;
    }
    const auto &rock = result.rocks[0];
    renderer::SceneMesh mesh;
    mesh.geometry = renderer::MakeRockMeshData(rock.mesh, m_settings.Display().smoothShading);
    ApplyRockMaterial(mesh, rock, false);
    if (!mesh.materialStack || m_workspace.Root().empty()) {
        fail("ルートフォルダとSurface材質を指定してください");
        return;
    }
    const auto fingerprint = BakeFingerprint(mesh, rock.mesh);
    std::array<LdrImage, 4> images;
    std::string error;
    if (!renderer::BakeMaterial(m_device, m_pipelineCache, mesh, m_textureLibrary, m_materialLibrary,
                                rock.mesh.uvWidth, rock.mesh.uvHeight, images, error)) {
        fail(error);
        return;
    }
    // UVの余白内を伸ばす。保存する画像は再実行ごとに新しいフォルダへ置き、Undoで戻れるようにする。
    int padding = 4;
    if (const auto *parent = m_graph.FindUpstreamNodeForPin(node->inputs[0].id))
        if (const auto *uv = std::get_if<geometry::UvUnwrapSettings>(&parent->settings))
            padding = uv->padding;
    const auto directory =
        m_workspace.UniquePath(m_workspace.Root() / L"Bakes", "MaterialBake_" + std::to_string(id), "");
    if (directory.empty()) {
        fail("ベイク保存先を作成できません");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        fail("ベイク保存先を作成できません");
        return;
    }
    const char *names[] = {"BaseColor.png", "Normal.png", "RoughnessMetallicAO.png", "Height.png"};
    std::array<compositor::TextureId, 4> ids{};
    for (size_t channel = 0; channel < 4; ++channel) {
        renderer::DilateBakePixels(images[channel], padding);
        auto &image = images[channel];
        // 未被覆領域にも安全な法線を置く。余白を越える強い縮小では島の混色は残り得る。
        if (channel == 1)
            for (size_t i = 0; i < image.pixels.size(); i += 4)
                if (!image.pixels[i + 3]) {
                    image.pixels[i] = 128;
                    image.pixels[i + 1] = 128;
                    image.pixels[i + 2] = 255;
                }
        const auto path = directory / names[channel];
        if (!SaveRgba8Png(path, image.width, image.height, image.width * 4, image.pixels.data())) {
            fail("ベイク画像を保存できません");
            return;
        }
        ids[channel] = m_textureLibrary.Load(m_device, m_pipelineCache, path);
        if (!ids[channel]) {
            fail("ベイク画像を読み込めません");
            return;
        }
    }
    const auto material = m_materialLibrary.Add("Baked " + std::to_string(id));
    auto *asset = m_materialLibrary.FindMutable(material);
    asset->baseColor = ids[0];
    asset->normal = ids[1];
    asset->flipNormalGreen = false;
    asset->roughness = {ids[2], compositor::TextureChannel::R};
    asset->roughnessValue = 1;
    asset->metallic = {ids[2], compositor::TextureChannel::G};
    asset->metallicValue = 1;
    asset->ambientOcclusion = {ids[2], compositor::TextureChannel::B};
    asset->ambientOcclusionValue = 1;
    asset->height = {ids[3], compositor::TextureChannel::R};
    auto &bake = std::get<graph::MaterialBakeSettings>(node->settings);
    bake.bakedLayer = compositor::MaterialStack::MakeBaseLayer();
    bake.bakedLayer.material = material;
    bake.bakedLayer.heightSource = compositor::ValueSource::Texture;
    bake.bakedLayer.heightGain = 1;
    bake.fingerprint = fingerprint;
    m_materialLibrary.MarkThumbnailDirty(material);
    m_assetRefresh = true;
    SetPreviewGraphNode(id);
    m_graph.MarkDirty();
    MarkDocumentChanged();
    SyncMeshGraph();
    ROCK_LOG_INFO("Material Bake完了: %u x %u、4チャンネル", rock.mesh.uvWidth, rock.mesh.uvHeight);
}
} // namespace rock
