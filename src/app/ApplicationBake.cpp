#include "app/Application.h"
#include "renderer/MaterialBake.h"
#include "renderer/RockMesh.h"
#include <fstream>
#include <sstream>
#include <cstring>
namespace rock {
std::string Application::BakeFingerprint(const renderer::SceneMesh &mesh, const geometry::Mesh &input, graph::GraphId bakeNode) const {
    uint64_t hash = 14695981039346656037ull;
    const auto bytes = [&](const void *data, size_t size) {
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
    };
    const auto add = [&](const auto &value) { bytes(&value, sizeof(value)); };
    if (const auto* node = m_graph.FindNode(bakeNode))
        if (const auto* settings = std::get_if<graph::MaterialBakeSettings>(&node->settings)) {
            add(settings->geometryAo); add(settings->aoDistance); add(settings->aoStrength); add(settings->aoSamples);
        }
    const uint32_t version = 3;
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
    const auto hashStack = [&](const compositor::MaterialStack& stack) {
        for (const auto &layer : stack.Layers()) {
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
    };
    if (mesh.materialStack) hashStack(*mesh.materialStack);
    for (const auto& applied : mesh.appliedMaterials) {
        add(applied.mapping.method); add(applied.mapping.offset); add(applied.mapping.rotationDegrees);
        add(applied.mapping.repeatMeters); add(applied.mapping.sharpness);
        add(applied.mask.value); add(applied.mask.repeatMeters); add(applied.mask.invert); add(applied.mask.triplanar);
        texture(applied.mask.texture); hashStack(applied.stack);
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

void Application::ApplyRockMaterial(renderer::SceneMesh &mesh, const graph::GeneratedRock &rock,
                                    bool useBaked) {
    if (!rock.materials.empty()) {
        for (const auto& binding : rock.materials) {
            const auto* node = m_graph.FindNode(binding.surface);
            const auto* layer = node ? std::get_if<graph::LayerNodeSettings>(&node->settings) : nullptr;
            if (!layer) continue;
            renderer::SceneMesh::AppliedMaterial applied;
            applied.stack.Layers() = m_graph.CompileLayersTo(node->id).layers;
            for (auto& l : applied.stack.Layers()) if (l.material) {
                l.heightSource = compositor::ValueSource::Texture;
                l.heightBase = compositor::kHeightPivot; l.heightGain = 1;
            }
            applied.mapping = layer->layer.mapping;
            applied.channels = layer->layer.channelMask;
            applied.stack.SetTerrainScale(applied.mapping.repeatMeters, 0);
            if (const auto* mask = m_graph.FindNode(binding.mask))
                if (const auto* settings = std::get_if<graph::MaterialMaskSettings>(&mask->settings)) applied.mask = *settings;
            mesh.appliedMaterials.push_back(std::move(applied));
        }
        if (!mesh.appliedMaterials.empty()) {
            mesh.materialStack = mesh.appliedMaterials[0].stack;
            mesh.mapping = mesh.appliedMaterials[0].mapping;
        }
    }
    const auto *surface = m_graph.FindNode(rock.materialSource);
    const auto *settings = surface ? std::get_if<graph::LayerNodeSettings>(&surface->settings) : nullptr;
    if (!settings && mesh.appliedMaterials.empty()) return;
    if (settings) {
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
    }
    if (!rock.bakeSource || !useBaked)
        return;
    const auto *node = m_graph.FindNode(rock.bakeSource);
    const auto *bake = node ? std::get_if<graph::MaterialBakeSettings>(&node->settings) : nullptr;
    bool ready = bake && !bake->fingerprint.empty() && bake->fingerprint == BakeFingerprint(mesh, rock.mesh, rock.bakeSource);
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
        mesh.appliedMaterials.clear();
    }
}

void Application::ProcessPendingBake() {
    if (m_bakeJob) {
        auto& job = *m_bakeJob;
        const auto discard = [&](const std::string& status) {
            if (job.epoch == m_pieceEpoch) m_bakeStatus[job.id] = status;
            job.ao.Release(m_device); m_bakeJob.reset();
        };
        if (job.cancel || job.epoch != m_pieceEpoch || job.root != m_workspace.Root() || job.revision != m_graph.Revision()) {
            discard(job.cancel ? "ベイクをキャンセルしました" : "入力が変更されたためベイクを中止しました"); return;
        }
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        do {
            if (!job.ao.Step(m_device, m_pipelineCache, error)) { ROCK_LOG_ERROR("Material Bake: %s", error.c_str()); discard(error); return; }
        } while (!job.ao.Complete() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(6));
        if (!job.ao.Complete()) return;
        // 素材ライブラリや表示設定の変更はグラフのRevisionだけでは検出できない。
        const auto result = graph::EvaluateRocks(m_graph, job.id, &m_rockEvaluationCache);
        if (!result.error.empty() || result.rocks.size() != 1) { discard("入力が変更されたためベイクを中止しました"); return; }
        renderer::SceneMesh mesh;
        mesh.geometry = renderer::MakeRockMeshData(result.rocks[0].mesh, m_settings.Display().smoothShading);
        ApplyRockMaterial(mesh, result.rocks[0], false);
        if (BakeFingerprint(mesh, result.rocks[0].mesh, job.id) != job.fingerprint) {
            discard("材質または設定が変更されたためベイクを中止しました"); return;
        }
        LdrImage ao;
        if (!job.ao.Read(m_device, ao)) { discard("GPU形状AOを読み戻せません"); return; }
        for (size_t i = 0; i < ao.pixels.size(); i += 4)
            job.images[2].pixels[i + 2] = uint8_t((unsigned(job.images[2].pixels[i + 2]) * ao.pixels[i] + 127) / 255);
        job.ao.Release(m_device);
        FinishBake(job.id, job.images, job.fingerprint);
        m_bakeJob.reset(); return;
    }
    if (m_pieceUpdating) return;
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
    const auto fingerprint = BakeFingerprint(mesh, rock.mesh, rock.bakeSource);
    std::array<LdrImage, 4> images;
    std::string error;
    if (!renderer::BakeMaterial(m_device, m_pipelineCache, mesh, m_textureLibrary, m_materialLibrary,
                                rock.mesh.uvWidth, rock.mesh.uvHeight, images, error)) {
        fail(error);
        return;
    }
    const auto& bakeSettings = std::get<graph::MaterialBakeSettings>(node->settings);
    if (bakeSettings.geometryAo) {
        auto& job = m_bakeJob.emplace();
        job.id = id; job.epoch = m_pieceEpoch; job.revision = m_graph.Revision();
        job.root = m_workspace.Root(); job.fingerprint = fingerprint; job.images = std::move(images);
        if (!job.ao.Create(m_device, rock.mesh, bakeSettings.aoDistance, bakeSettings.aoSamples,
                           bakeSettings.aoStrength, error)) {
            fail(error); job.ao.Release(m_device); m_bakeJob.reset();
        }
        return;
    }
    FinishBake(id, images, fingerprint);
}

void Application::FinishBake(graph::GraphId id, std::array<LdrImage, 4>& images, const std::string& fingerprint) {
    auto* node = m_graph.FindMutableNode(id);
    if (!node || node->kind != graph::NodeKind::MaterialBake) return;
    const auto fail = [&](const std::string& error) { m_bakeStatus[id] = error; ROCK_LOG_ERROR("Material Bake: %s", error.c_str()); };
    // UVの余白内を伸ばす。保存する画像は再実行ごとに新しいフォルダへ置き、Undoで戻れるようにする。
    int padding = 4;
    const auto* parent = m_graph.FindUpstreamNodeForPin(node->inputs[0].id);
    for (int depth = 0; parent && depth < 256; ++depth) {
        if (const auto* uv = std::get_if<geometry::UvUnwrapSettings>(&parent->settings)) { padding = uv->padding; break; }
        parent = parent->inputs.empty() ? nullptr : m_graph.FindUpstreamNodeForPin(parent->inputs[0].id);
    }
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
    ROCK_LOG_INFO("Material Bake完了: %u x %u、4チャンネル", images[0].width, images[0].height);
}
} // namespace rock
