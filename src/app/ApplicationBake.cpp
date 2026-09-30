#include "io/LayerMaterialIo.h"
#include "app/Application.h"
#include "core/FileDialog.h"
#include "core/PathUtf8.h"
#include "renderer/MaterialBake.h"
#include "renderer/RockMesh.h"
#include "geometry/DetailTransfer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cstring>
namespace rock {
std::string Application::BakeFingerprint(const renderer::SceneMesh &mesh, const geometry::Mesh &input, graph::GraphId bakeNode,
                                         uint64_t detail) const {
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
            // ハイポリの転写。繋いでいないときは含めない（従来の指紋と変えない）。
            if (detail) { add(detail); add(settings->cageDistance); }
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

bool Application::ApplyRockMaterial(renderer::SceneMesh &mesh, const graph::GeneratedRock &rock,
                                    bool useBaked, const renderer::MeshData* bakeGeometry, std::string* inputFingerprint) {
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
        return false;
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
    if (!settings && mesh.appliedMaterials.empty()) return false;
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
        return false;
    const auto *node = m_graph.FindNode(rock.bakeSource);
    const auto *bake = node ? std::get_if<graph::MaterialBakeSettings>(&node->settings) : nullptr;
    std::string fingerprint;
    if (bake && bakeGeometry) {
        // Rock Asset の LOD。焼いたときのメッシュの形で照らし、表示する形へ戻す。
        renderer::MeshData shown = std::move(mesh.geometry);
        mesh.geometry = *bakeGeometry;
        fingerprint = BakeFingerprint(mesh, rock.bakeMesh ? *rock.bakeMesh : rock.mesh, rock.bakeSource, rock.bakeDetail);
        mesh.geometry = std::move(shown);
    } else if (bake) {
        fingerprint = BakeFingerprint(mesh, rock.mesh, rock.bakeSource, rock.bakeDetail);
    }
    if (inputFingerprint) *inputFingerprint = fingerprint;
    bool ready = bake && !bake->fingerprint.empty() && bake->fingerprint == fingerprint;
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
    return ready;
}

std::vector<io::RockAssetLod> Application::MergeRockAssetLods(const graph::RockEvaluation& evaluated,
                                                              const graph::RockAssetSettings& settings) {
    size_t count = 0;
    for (const auto& rock : evaluated.rocks)
        if (rock.lods) count = std::max(count, rock.lods->size());
    std::vector<io::RockAssetLod> lods(count);
    for (size_t level = 0; level < count; ++level) {
        auto& merged = lods[level].mesh;
        lods[level].screenSize = level == 0 ? 1.0f : settings.screenSize[std::min<size_t>(level, graph::kMaxRockAssetLods - 1)];
        for (const auto& rock : evaluated.rocks) {
            if (!rock.lods || rock.lods->empty()) continue;
            // 自分の UV を持つ段は、その段のテクスチャのフォルダを使う。
            if (int(level) >= rock.sharedUvLods) lods[level].textures = io::RockAssetLodTextureFolder(level);
            const auto& mesh = (*rock.lods)[std::min(level, rock.lods->size() - 1)];
            const auto offset = static_cast<uint32_t>(merged.positions.size());
            merged.positions.insert(merged.positions.end(), mesh.positions.begin(), mesh.positions.end());
            for (auto face : mesh.triangles) {
                for (auto& index : face) index += offset;
                merged.triangles.push_back(face);
            }
            merged.cornerUvs.insert(merged.cornerUvs.end(), mesh.cornerUvs.begin(), mesh.cornerUvs.end());
            merged.uvCharts.insert(merged.uvCharts.end(), mesh.uvCharts.begin(), mesh.uvCharts.end());
            merged.uvWidth = std::max(merged.uvWidth, mesh.uvWidth);
            merged.uvHeight = std::max(merged.uvHeight, mesh.uvHeight);
        }
        // UV を持たないメッシュが混ざったら、UV は付けない（並びが三角形と合わなくなる）。
        if (merged.cornerUvs.size() != merged.triangles.size()) { merged.cornerUvs.clear(); merged.uvCharts.clear(); }
        if (merged.uvCharts.size() != merged.triangles.size()) merged.uvCharts.clear();
    }
    return lods;
}

// 「岩アセットを焼く」。シーンの付属フォルダへ、段ごとのメッシュと Material Bake の結果を書く。
// 上流の Material Bake が古い（未ベイク）なら先にベイクを頼み、終わってからもう一度ここへ来る。
void Application::ProcessPendingAssetBake() {
    if (!m_pendingAssetBake || m_bakeJob || m_pendingBake || m_pieceUpdating) return;
    const graph::GraphId id = m_pendingAssetBake;
    const auto finish = [&](const std::string& status, bool failed) {
        m_pendingAssetBake = 0;
        // 先に Material Bake を焼くと、プレビューが Material Bake へ移る。焼く前に見ていたもの（Rock Asset の段）へ戻す。
        if (m_assetBakeRequestedMaterial) SetPreviewGraphNode(m_assetBakePreviewNode, m_assetBakePreviewPin);
        m_assetBakeRequestedMaterial = false;
        m_assetBakeStatus = status;
        if (failed) ROCK_LOG_ERROR("岩アセット: %s", status.c_str());
        else ROCK_LOG_INFO("岩アセット: %s", status.c_str());
    };
    const auto* node = m_graph.FindNode(id);
    const auto* settings = node ? std::get_if<graph::RockAssetSettings>(&node->settings) : nullptr;
    if (!settings) return finish("Rock Asset が見つかりません", true);
    if (m_projectPath.empty() || !io::IsSceneFile(m_projectPath) || io::IsMountainFile(m_projectPath) ||
        !m_workspace.Contains(m_projectPath))
        return finish("先にシーン（.rockgraph）をルートの中へ保存してください", true);
    const auto evaluated = graph::EvaluateRocks(m_graph, id, &m_rockEvaluationCache, m_settings.Display().sdfPreviewMethod, {},
                                                nullptr, m_materialHeights.get());
    if (!evaluated.error.empty()) return finish(evaluated.error, true);
    // Material Bake の結果。描画と同じ照らし方で、まだ使えるかを確かめる。
    graph::GraphId bakeNode = 0;
    bool ready = true;
    // いまの入力でベイクしたときの指紋。焼いたときのハッシュに入れる（ベイクの結果は一時的なので、
    // 「ベイクしたか」ではなく「何をベイクするか」で比べる。開き直しても古いと判定しない）。
    std::string fingerprint;
    for (const auto& rock : evaluated.rocks) {
        if (!rock.lods) continue;
        if (!rock.bakeSource) { ready = false; continue; }
        bakeNode = rock.bakeSource;
        renderer::SceneMesh probe;
        probe.geometry = renderer::MakeRockMeshData(rock.mesh, m_settings.Display().smoothShading, m_settings.Display().smoothShadingAngle);
        renderer::MeshData bakeGeometry;
        if (rock.bakeMesh)
            bakeGeometry = renderer::MakeRockMeshData(*rock.bakeMesh, m_settings.Display().smoothShading,
                                                      m_settings.Display().smoothShadingAngle);
        std::string input;
        ready &= ApplyRockMaterial(probe, rock, true, rock.bakeMesh ? &bakeGeometry : nullptr, &input);
        fingerprint += input;
    }
    ready &= bakeNode != 0 && m_bakeImages.contains(bakeNode);
    if (bakeNode && !ready) {
        if (m_assetBakeRequestedMaterial) return finish("Material Bake に失敗したので、焼けませんでした", true);
        // 先に Material Bake を焼く。終わったらもう一度ここへ来る。
        m_assetBakeRequestedMaterial = true;
        m_assetBakePreviewNode = m_previewGraphNode;
        m_assetBakePreviewPin = m_previewGraphPin;
        m_pendingBake = bakeNode;
        m_assetBakeStatus = "Material Bake を先に実行しています…";
        return;
    }
    io::RockAssetData data;
    data.lods = MergeRockAssetLods(evaluated, *settings);
    if (data.lods.empty() || data.lods[0].mesh.triangles.empty()) return finish("焼く段がありません", true);
    data.textured = ready && bakeNode;
    // 自分の UV を持つ段のテクスチャは、LOD0 の Material Bake の結果から転写する。転写元の岩は 1 つだけのはず
    // （Material Bake は 1 つのメッシュを焼く）。
    const graph::GeneratedRock* owner = nullptr;
    size_t owners = 0;
    for (const auto& rock : evaluated.rocks)
        if (rock.lods) { owner = &rock; ++owners; }
    const bool ownUv = std::any_of(data.lods.begin(), data.lods.end(), [](const auto& lod) { return !lod.textures.empty(); });
    if (ownUv && owners != 1) data.textured = false;
    data.hash = io::RockAssetHash(data.lods, fingerprint);
    geometry::MeshInfo info;
    if (geometry::InspectMesh(data.lods[0].mesh, info)) {
        data.minimum = info.minimum;
        data.maximum = info.maximum;
    }
    const auto folder = io::RockAssetFolder(m_projectPath);
    std::error_code fileError;
    std::filesystem::create_directories(folder, fileError);
    if (data.textured) {
        const auto& images = m_bakeImages[bakeNode];
        for (size_t channel = 0; channel < 4; ++channel) {
            const auto& image = images[channel];
            if (!SaveRgba8Png(folder / io::kRockAssetTextures[channel], image.width, image.height, image.width * 4, image.pixels.data()))
                return finish(std::string("テクスチャを書けません: ") + io::kRockAssetTextures[channel], true);
        }
        for (size_t level = 0; level < data.lods.size(); ++level) {
            const std::string& textures = data.lods[level].textures;
            if (textures.empty()) continue;
            std::string lodError;
            const auto* set = RockLodTextures(*owner, level, lodError);
            if (!set)
                return finish("LOD" + std::to_string(level) + " のテクスチャを作れません" + (lodError.empty() ? "" : ": " + lodError), true);
            const auto lodFolder = folder / FromUtf8(textures);
            std::filesystem::create_directories(lodFolder, fileError);
            for (size_t channel = 0; channel < 4; ++channel) {
                const auto& image = set->images[channel];
                if (!SaveRgba8Png(lodFolder / io::kRockAssetTextures[channel], image.width, image.height, image.width * 4,
                                  image.pixels.data()))
                    return finish(textures + "/" + io::kRockAssetTextures[channel] + " を書けません", true);
            }
        }
    }
    std::string error;
    if (!io::SaveRockAsset(m_projectPath, data, error)) return finish(error, true);
    m_assetRefresh = true;
    m_bakedAssetCache = {};
    char text[160] = {};
    std::snprintf(text, sizeof(text), "焼きました（%zu 段、LOD0 %zu 三角形%s）", data.lods.size(), data.lods[0].mesh.triangles.size(),
                  data.textured ? "、テクスチャあり" : "、テクスチャなし");
    finish(text, false);
}

const Application::BakedAssetCache& Application::BakedAsset() {
    const auto manifest = io::RockAssetFolder(m_projectPath) / L"asset.json";
    std::error_code error;
    const auto time = m_projectPath.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(manifest, error);
    const bool exists = !m_projectPath.empty() && !error;
    if (m_bakedAssetCache.scene != m_projectPath || m_bakedAssetCache.exists != exists || m_bakedAssetCache.time != time) {
        m_bakedAssetCache = {m_projectPath, time, exists, {}};
        if (exists && !io::ReadRockAssetHash(m_projectPath, m_bakedAssetCache.hash)) m_bakedAssetCache.exists = false;
    }
    return m_bakedAssetCache;
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
        if (BakeFingerprint(mesh, result.rocks[0].mesh, job.id, result.rocks[0].bakeDetail) != job.fingerprint) {
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
    const auto fingerprint = BakeFingerprint(mesh, rock.mesh, rock.bakeSource, rock.bakeDetail);
    std::array<LdrImage, 4> images;
    std::string error;
    if (!renderer::BakeMaterial(m_device, m_pipelineCache, mesh, m_textureLibrary, m_materialLibrary,
                                rock.mesh.uvWidth, rock.mesh.uvHeight, images, error)) {
        fail(error);
        return;
    }
    const auto& bakeSettings = std::get<graph::MaterialBakeSettings>(node->settings);
    // High（ハイポリ）を繋いでいれば、法線とハイトをそこから転写して、素材の法線とハイトに重ねる。
    if (rock.bakeDetail) {
        if (!TransferHighDetail(*node, rock, mesh, bakeSettings, images, error)) {
            fail(error);
            return;
        }
    }
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

// 描画用のデータ（MakeRockMeshData は面ごとに 3 頂点を順に並べる）から、面ごとの角の向きを取り出す。
// 転写の接線空間を描画と揃えるために使う。
static bool CornerFrames(const renderer::MeshData& geometry, const geometry::Mesh& mesh,
                         std::vector<std::array<geometry::CornerFrame, 3>>& frames) {
    if (geometry.vertices.size() != mesh.triangles.size() * 3) return false;
    frames.resize(mesh.triangles.size());
    for (size_t f = 0; f < frames.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const auto& v = geometry.vertices[f * 3 + k];
            frames[f][k] = {{v.normal.x, v.normal.y, v.normal.z}, {v.tangent.x, v.tangent.y, v.tangent.z}, v.tangent.w};
        }
    return true;
}

// Rock Asset の自分の UV を持つ段のテクスチャ。LOD0 の面へレイを飛ばし、LOD0 の UV で Material Bake の結果を読む。
// 法線は LOD0 の接線空間からこの段の接線空間へ直すので、LOD0 の形の凹凸も法線マップに入る。
// 呼び出す側は、Material Bake の結果がいまの入力で使えること（ApplyRockMaterial が true）を確かめておく。
// 返す要素は m_lodTextures の中を指すので、次にこの関数を呼ぶまでに使い終える。
const Application::LodTextureSet* Application::RockLodTextures(const graph::GeneratedRock& rock, size_t level, std::string& error) {
    error.clear();
    if (!rock.lods || level >= rock.lods->size() || int(level) < rock.sharedUvLods || !rock.bakeSource) return nullptr;
    const auto* node = m_graph.FindNode(rock.bakeSource);
    const auto* bake = node ? std::get_if<graph::MaterialBakeSettings>(&node->settings) : nullptr;
    const auto images = m_bakeImages.find(rock.bakeSource);
    if (!bake || bake->fingerprint.empty() || images == m_bakeImages.end()) return nullptr;
    const geometry::Mesh& source = (*rock.lods)[0];
    const geometry::Mesh& target = (*rock.lods)[level];
    if (!geometry::HasValidUvs(source) || !geometry::HasValidUvs(target)) return nullptr;
    // 転写元（ベイクの指紋と LOD0）とこの段のメッシュが同じなら、前に作ったものを使う。
    const std::string key = io::RockAssetHash({{source}, {target}}, bake->fingerprint);
    for (auto& entry : m_lodTextures)
        if (entry.key == key) {
            entry.used = true;
            return &entry;
        }
    const auto start = std::chrono::steady_clock::now();
    const auto& display = m_settings.Display();
    std::vector<std::array<geometry::CornerFrame, 3>> sourceFrames, targetFrames;
    if (!CornerFrames(renderer::MakeRockMeshData(source, display.smoothShading, display.smoothShadingAngle), source, sourceFrames) ||
        !CornerFrames(renderer::MakeRockMeshData(target, display.smoothShading, display.smoothShadingAngle), target, targetFrames)) {
        error = "段の法線を作れませんでした";
        return nullptr;
    }
    std::array<geometry::TextureView, 4> views;
    for (size_t channel = 0; channel < 4; ++channel)
        views[channel] = {images->second[channel].width, images->second[channel].height, images->second[channel].pixels.data()};
    // 探す距離は LOD0 の大きさの 5%。段どうしの形のずれより大きく、岩の薄い所の厚みより小さくしたい。
    geometry::MeshInfo info;
    geometry::InspectMesh(source, info);
    const float extent = std::max({info.maximum.x - info.minimum.x, info.maximum.y - info.minimum.y, info.maximum.z - info.minimum.z});
    // 粗い段ほど LOD0 から離れるので、届かなかった画素だけ距離を広げて探し直す（最初に当たったものを優先する）。
    float cage = std::clamp(extent * 0.05f, geometry::kMinCageDistance, geometry::kMaxCageDistance);
    geometry::TextureTransferResult transfer;
    if (!geometry::TransferTextures(target, targetFrames, source, sourceFrames, views, cage, target.uvWidth, target.uvHeight,
                                    transfer, error))
        return nullptr;
    const auto coveredCount = [](const geometry::TextureTransferResult& result) {
        return size_t(std::count(result.covered.begin(), result.covered.end(), uint8_t(1)));
    };
    for (int retry = 0; retry < 2 && transfer.hits < coveredCount(transfer) * 99 / 100; ++retry) {
        cage = std::min(cage * 3, geometry::kMaxCageDistance);
        geometry::TextureTransferResult wider;
        if (!geometry::TransferTextures(target, targetFrames, source, sourceFrames, views, cage, target.uvWidth, target.uvHeight,
                                        wider, error))
            return nullptr;
        for (size_t i = 0; i < transfer.covered.size(); ++i) {
            if (transfer.images[0][i * 4 + 3] || !wider.images[0][i * 4 + 3]) continue;
            for (size_t channel = 0; channel < 4; ++channel)
                std::copy_n(&wider.images[channel][i * 4], 4, &transfer.images[channel][i * 4]);
            ++transfer.hits;
        }
    }
    LodTextureSet entry;
    entry.key = key;
    for (size_t channel = 0; channel < 4; ++channel) {
        entry.images[channel].width = transfer.width;
        entry.images[channel].height = transfer.height;
        entry.images[channel].pixels = std::move(transfer.images[channel]);
    }
    const std::string name = "LOD" + std::to_string(level) + " " + std::to_string(rock.bakeSource);
    entry.material = MakeBakedMaterial("Bake " + name, "Baked " + name, entry.images, entry.textures);
    if (entry.material == compositor::kNoMaterialAsset) {
        error = "段のテクスチャをGPUへ転送できません";
        return nullptr;
    }
    entry.used = true;
    const size_t covered = size_t(std::count(transfer.covered.begin(), transfer.covered.end(), uint8_t(1)));
    const double ratio = covered ? double(transfer.hits) / double(covered) : 0.0;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ROCK_LOG_INFO("Rock Asset: LOD%zu のテクスチャを LOD0 から転写しました（%u x %u、当たった画素 %.1f%%、%.2f 秒）", level,
                  transfer.width, transfer.height, ratio * 100.0, seconds);
    if (covered && ratio < 0.95)
        ROCK_LOG_WARN("Rock Asset: LOD%zu で LOD0 に当たらなかった画素が %.1f%% あります（周りの色で埋めました）", level,
                      (1.0 - ratio) * 100.0);
    m_lodTextures.push_back(std::move(entry));
    return &m_lodTextures.back();
}

// ハイポリから転写した法線とハイトを、素材を焼いた画像（images[1] と images[3]）に重ねる。
//   法線: ハイポリの法線（ローポリの接線空間）に、素材の法線を細部として重ねる（whiteout 合成）。
//   ハイト: 素材のハイトに、ローポリの面からハイポリまでの距離を足す。距離はケージ距離の 2 倍を 1 とする
//           （±ケージ距離が ±0.5）。
// ハイポリに当たらなかった画素は、素材の値のまま残す。
bool Application::TransferHighDetail(const graph::Node& node, const graph::GeneratedRock& rock, const renderer::SceneMesh& mesh,
                                     const graph::MaterialBakeSettings& settings, std::array<LdrImage, 4>& images,
                                     std::string& error) {
    const auto* highNode = node.inputs.size() > 2 ? m_graph.FindUpstreamNodeForPin(node.inputs[2].id) : nullptr;
    if (!highNode) {
        error = "Highの接続が見つかりません";
        return false;
    }
    const auto evaluated = graph::EvaluateRocks(m_graph, highNode->id, &m_rockEvaluationCache, m_settings.Display().sdfPreviewMethod,
                                                {}, nullptr, m_materialHeights.get());
    if (!evaluated.error.empty()) {
        error = evaluated.error;
        return false;
    }
    // 複数のメッシュは 1 つにまとめて探す。
    geometry::Mesh high;
    for (const auto& part : evaluated.rocks) {
        const auto offset = static_cast<uint32_t>(high.positions.size());
        high.positions.insert(high.positions.end(), part.mesh.positions.begin(), part.mesh.positions.end());
        for (auto face : part.mesh.triangles) {
            for (auto& index : face) index += offset;
            high.triangles.push_back(face);
        }
    }
    // 描画と同じ法線・接線で接線空間を取る。
    std::vector<std::array<geometry::CornerFrame, 3>> frames;
    if (!CornerFrames(mesh.geometry, rock.mesh, frames)) {
        error = "ローポリの法線を作れませんでした";
        return false;
    }
    const uint32_t width = images[1].width, height = images[1].height;
    geometry::DetailTransferImage transfer;
    const auto start = std::chrono::steady_clock::now();
    if (!geometry::TransferDetail(rock.mesh, frames, high, settings.cageDistance, width, height, transfer, error)) return false;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (size_t i = 0; i < transfer.hit.size(); ++i) {
        if (!transfer.hit[i]) continue;
        uint8_t* normal = &images[1].pixels[i * 4];
        uint8_t* heightPixel = &images[3].pixels[i * 4];
        if (!normal[3]) continue;  // 素材を焼いていない画素（島の外）
        const float mx = normal[0] / 255.0f * 2 - 1, my = normal[1] / 255.0f * 2 - 1, mz = normal[2] / 255.0f * 2 - 1;
        const auto& h = transfer.normals[i];
        float rx = h.x + mx, ry = h.y + my, rz = h.z * mz;
        const float length = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (length > 1e-6f) { rx /= length; ry /= length; rz /= length; } else { rx = h.x; ry = h.y; rz = h.z; }
        const auto encode = [](float value) { return uint8_t(std::lround(std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f) * 255)); };
        normal[0] = encode(rx); normal[1] = encode(ry); normal[2] = encode(rz);
        const float offset = transfer.heights[i] / (2 * settings.cageDistance);
        const uint8_t value = uint8_t(std::lround(std::clamp(heightPixel[0] / 255.0f + offset, 0.0f, 1.0f) * 255));
        heightPixel[0] = heightPixel[1] = heightPixel[2] = value;
    }
    const size_t covered = transfer.CoveredCount(), hits = transfer.HitCount();
    const double ratio = covered ? double(hits) / double(covered) : 0.0;
    ROCK_LOG_INFO("Material Bake: ハイポリ（%zu 三角形）から法線とハイトを転写しました。当たった画素 %.1f%%、%.1f 秒",
                  high.triangles.size(), ratio * 100.0, seconds);
    if (covered && ratio < 0.95)
        ROCK_LOG_WARN("Material Bake: ハイポリに当たらなかった画素が %.1f%% あります。ケージ距離を広げてください",
                      (1.0 - ratio) * 100.0);
    return true;
}

compositor::MaterialAssetId Application::MakeBakedMaterial(const std::string& name, const std::string& materialName,
                                                           std::array<LdrImage, 4>& images,
                                                           std::array<compositor::TextureId, 4>& textures) {
    const char* labels[] = {"BaseColor", "Normal", "RoughnessMetallicAO", "Height"};
    textures = {};
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
        textures[channel] = m_textureLibrary.AddTransient(m_device, m_pipelineCache, name + " " + labels[channel] + "（一時）", image);
        if (!textures[channel]) {
            for (size_t created = 0; created < channel; ++created) m_textureLibrary.Remove(m_device, textures[created]);
            textures = {};
            return compositor::kNoMaterialAsset;
        }
    }
    const auto material = m_materialLibrary.Add(materialName + "（一時）");
    auto *asset = m_materialLibrary.FindMutable(material);
    asset->transient = true;
    asset->baseColor = textures[0];
    asset->normal = textures[1];
    asset->flipNormalGreen = false;
    asset->roughness = {textures[2], compositor::TextureChannel::R};
    asset->roughnessValue = 1;
    asset->metallic = {textures[2], compositor::TextureChannel::G};
    asset->metallicValue = 1;
    asset->ambientOcclusion = {textures[2], compositor::TextureChannel::B};
    asset->ambientOcclusionValue = 1;
    asset->height = {textures[3], compositor::TextureChannel::R};
    return material;
}

void Application::FinishBake(graph::GraphId id, std::array<LdrImage, 4>& images, const std::string& fingerprint) {
    auto* node = m_graph.FindMutableNode(id);
    if (!node || node->kind != graph::NodeKind::MaterialBake) return;
    const auto fail = [&](const std::string& error) { m_bakeStatus[id] = error; ROCK_LOG_ERROR("Material Bake: %s", error.c_str()); };
    // 結果はメモリ上にだけ持つ。ファイルへは「テクスチャを出力…」を押したときだけ書く。
    // テクスチャと材質は一時的なもので、シーンにも保存しない。開き直したら再ベイクする。
    std::array<compositor::TextureId, 4> ids{};
    const auto material = MakeBakedMaterial("Bake " + std::to_string(id), "Baked " + std::to_string(id), images, ids);
    if (material == compositor::kNoMaterialAsset) {
        fail("ベイク画像をGPUへ転送できません");
        return;
    }
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
