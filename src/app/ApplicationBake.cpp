#include "io/LayerMaterialIo.h"
#include "app/Application.h"
#include "core/FileDialog.h"
#include "core/PathUtf8.h"
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
        // Shape Mask の画像はファイルを持たない。画素そのものを含める。
        for (const auto& entry : m_shapeMaskTextures)
            if (entry.texture == id) {
                add(entry.image->width); add(entry.image->height);
                bytes(entry.image->pixels.data(), entry.image->pixels.size());
                return;
            }
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
                const auto hashMaterial = [&](const compositor::MaterialAsset* asset) {
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
                };
                hashMaterial(asset);
                if (asset->layerMaterial) {
                    auto body = io::WriteLayerMaterial(*asset->layerMaterial);
                    // 同じファイルを再読込した時の実行時IDの変化を指紋へ持ち込まない。
                    io::MapLayerMaterials(body, [&](const nlohmann::json& value) -> nlohmann::json {
                        const auto* source = m_materialLibrary.Find(value.is_number_integer() ? value.get<uint32_t>() : 0);
                        const bool found = source && !source->layerMaterial;
                        add(found);
                        if (found) hashMaterial(source);
                        return 0;
                    });
                    body.erase("name");
                    const auto serialized = body.dump(); bytes(serialized.data(), serialized.size());
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
        if (applied.heightBlend) { add(applied.heightBlend); add(applied.heightBlendRange); }  // 既存のベイクの指紋は変えない。
        if (applied.opacity != 1) add(applied.opacity);
        texture(applied.mask.texture); hashStack(applied.stack);
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

compositor::TextureId Application::ShapeMaskTextureFor(const std::shared_ptr<const geometry::MaskImage>& image) {
    if (!image || !image->width || !image->height) return compositor::kNoTexture;
    for (auto& entry : m_shapeMaskTextures)
        if (entry.image == image && m_textureLibrary.Find(entry.texture)) { entry.used = true; return entry.texture; }
    LdrImage rgba;
    rgba.width = image->width; rgba.height = image->height;
    rgba.pixels.resize(image->pixels.size() * 4);
    for (size_t i = 0; i < image->pixels.size(); ++i) {
        const uint8_t v = image->pixels[i];
        rgba.pixels[i * 4] = rgba.pixels[i * 4 + 1] = rgba.pixels[i * 4 + 2] = v;
        rgba.pixels[i * 4 + 3] = 255;
    }
    const auto id = m_textureLibrary.AddTransient(m_device, m_pipelineCache, "Shape Mask（一時）", rgba);
    if (id) m_shapeMaskTextures.push_back({image, id, true});
    return id;
}

void Application::ApplyRockMaterial(renderer::SceneMesh &mesh, const graph::GeneratedRock &rock,
                                    bool useBaked) {
    if (rock.previewMask) {
        // Shape Mask を見ているとき。黒の全面の上に、マスクで白を重ねる（既存の素材の合成をそのまま使う）。
        const auto texture = ShapeMaskTextureFor(rock.previewMask);
        for (int stage = 0; stage < 2; ++stage) {
            renderer::SceneMesh::AppliedMaterial applied;
            auto layer = compositor::MaterialStack::MakeBaseLayer();
            layer.material = compositor::kNoMaterialAsset;
            const float c = stage == 0 ? 0.0f : 1.0f;
            layer.baseColor = {c, c, c};
            layer.roughness = 1; layer.metallic = 0; layer.ambientOcclusion = 1;
            layer.heightSource = compositor::ValueSource::Constant;
            applied.stack.Layers() = {layer};
            applied.stack.SetTerrainScale(1, 0);
            if (stage == 1) {
                applied.mask.texture = texture;
                // 反転は画像を作り直さず、キャッシュキーにも入れていない。評価結果の値ではなく、いま見ているノードの設定から読む。
                applied.mask.invert = rock.previewMaskInvert;
                if (const auto* node = m_graph.FindNode(m_previewGraphNode))
                    applied.mask.invert = graph::ImageMaskInvert(*node);
            }
            mesh.appliedMaterials.push_back(std::move(applied));
        }
        mesh.materialStack = mesh.appliedMaterials[0].stack;
        mesh.mapping = mesh.appliedMaterials[0].mapping;
        return;
    }
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
            applied.heightBlend = binding.heightBlend;
            applied.heightBlendRange = binding.heightBlendRange;
            applied.opacity = binding.opacity;
            applied.stack.SetTerrainScale(applied.mapping.repeatMeters, 0);
            if (const auto* mask = m_graph.FindNode(binding.mask))
                if (const auto* settings = std::get_if<graph::MaterialMaskSettings>(&mask->settings)) applied.mask = *settings;
            if (const auto image = rock.maskImages.find(binding.mask); image != rock.maskImages.end()) {
                // 形状マスク。画像をUVでそのまま貼る（反復なし）。
                applied.mask = {};
                applied.mask.texture = ShapeMaskTextureFor(image->second);
                if (const auto* maskNode = m_graph.FindNode(binding.mask))
                    applied.mask.invert = graph::ImageMaskInvert(*maskNode);
            }
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
        const auto result = graph::EvaluateRocks(m_graph, job.id, &m_rockEvaluationCache, m_settings.Display().sdfPreviewMethod, {}, nullptr, m_materialHeights.get());
        if (!result.error.empty() || result.rocks.size() != 1) { discard("入力が変更されたためベイクを中止しました"); return; }
        renderer::SceneMesh mesh;
        mesh.geometry = renderer::MakeRockMeshData(result.rocks[0].mesh, m_settings.Display().smoothShading, m_settings.Display().smoothShadingAngle);
        ApplyRockMaterial(mesh, result.rocks[0], false);
        if (BakeFingerprint(mesh, result.rocks[0].mesh, job.id) != job.fingerprint) {
            discard("材質または設定が変更されたためベイクを中止しました"); return;
        }
        LdrImage ao;
        if (!job.ao.Read(m_device, ao)) { discard("GPU形状AOを読み戻せません"); return; }
        if (ao.pixels.size() != job.images[2].pixels.size()) { discard("GPU形状AOの寸法がベイク画像と一致しません"); return; }
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
    const auto result = graph::EvaluateRocks(m_graph, id, &m_rockEvaluationCache, m_settings.Display().sdfPreviewMethod, {}, nullptr, m_materialHeights.get());
    if (!result.error.empty() || result.rocks.size() != 1) {
        fail(result.error.empty() ? "ベイク入力がありません" : result.error);
        return;
    }
    const auto &rock = result.rocks[0];
    renderer::SceneMesh mesh;
    mesh.geometry = renderer::MakeRockMeshData(rock.mesh, m_settings.Display().smoothShading, m_settings.Display().smoothShadingAngle);
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
    // 結果はメモリ上にだけ持つ。ファイルへは「テクスチャを出力…」を押したときだけ書く。
    // テクスチャと材質は一時的なもので、シーンにも保存しない。開き直したら再ベイクする。
    const char* labels[] = {"BaseColor", "Normal", "RoughnessMetallicAO", "Height"};
    std::array<compositor::TextureId, 4> ids{};
    for (size_t channel = 0; channel < 4; ++channel) {
        auto& image = images[channel];
        // UV の島が無い部分を黒や透明のまま残さない。縮小表示やミップマップで、島の縁へその色がにじむ。
        // 4枚とも、最も近い島の縁の色を全面へ伸ばす（エッジパディング）。法線も同じく伸ばすので、
        // 島の縁で向きが途切れない。
        renderer::FillBakeBackground(image);
        // 島が1つも無い画像は埋められない。法線だけは、既定の向き（128, 128, 255）の不透明にしておく。
        if (channel == 1)
            for (size_t i = 0; i < image.pixels.size(); i += 4)
                if (!image.pixels[i + 3]) {
                    image.pixels[i] = 128;
                    image.pixels[i + 1] = 128;
                    image.pixels[i + 2] = 255;
                    image.pixels[i + 3] = 255;
                }
        ids[channel] = m_textureLibrary.AddTransient(
            m_device, m_pipelineCache, "Bake " + std::to_string(id) + " " + labels[channel] + "（一時）", image);
        if (!ids[channel]) {
            for (size_t created = 0; created < channel; ++created) m_textureLibrary.Remove(m_device, ids[created]);
            fail("ベイク画像をGPUへ転送できません");
            return;
        }
    }
    const auto material = m_materialLibrary.Add("Baked " + std::to_string(id) + "（一時）");
    auto *asset = m_materialLibrary.FindMutable(material);
    asset->transient = true;
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
    m_bakeImages[id] = std::move(images);
    m_materialLibrary.MarkThumbnailDirty(material);
    SetPreviewGraphNode(id);
    m_graph.MarkDirty();
    MarkDocumentChanged();
    SyncMeshGraph();
    ROCK_LOG_INFO("Material Bake完了: %u x %u、4チャンネル（一時。ファイルへは未出力）", m_bakeImages[id][0].width,
                  m_bakeImages[id][0].height);
}

void Application::ExportBakedTextures(graph::GraphId id, std::filesystem::path directory) {
    const auto found = m_bakeImages.find(id);
    if (found == m_bakeImages.end()) return;
    if (directory.empty()) {
        const std::filesystem::path initial = m_workspace.IsOpen() ? m_workspace.Root() : std::filesystem::path{};
        directory = ShowPickFolderDialog(L"ベイクしたテクスチャの出力先", initial);
        if (directory.empty()) return;  // 取り消し。
    }
    std::error_code createError;
    std::filesystem::create_directories(directory, createError);
    const char* names[] = {"BaseColor.png", "Normal.png", "RoughnessMetallicAO.png", "Height.png"};
    for (size_t channel = 0; channel < 4; ++channel) {
        const auto& image = found->second[channel];
        const auto path = directory / names[channel];
        if (!SaveRgba8Png(path, image.width, image.height, image.width * 4, image.pixels.data())) {
            m_toasts.Push("テクスチャを出力できません", ToUtf8Display(path));
            ROCK_LOG_ERROR("ベイクしたテクスチャを出力できません: %s", ToUtf8Display(path).c_str());
            return;
        }
    }
    m_toasts.Push("ベイクしたテクスチャを4枚出力しました", ToUtf8Display(directory), directory);
    ROCK_LOG_INFO("ベイクしたテクスチャを出力しました: %s", ToUtf8Display(directory).c_str());
    // 出力先がルートの中なら、アセット欄に出す。
    m_assetRefresh = true;
}
} // namespace rock
