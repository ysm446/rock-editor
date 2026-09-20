// モデル（FBX）の取り込み、モデルプレビューの窓、スロットへのマテリアルの割り当て。
// rock-editor の ApplicationModelPanel から、配置（Model Scatter）と一覧パネルを外して移植した。
// 一覧はアセットの帯（.rockmodel / .fbx）が兼ねる。仕様は docs/reference/model-assets.md。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/ImageIo.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace rock {
namespace fs = std::filesystem;

renderer::ModelAsset* Application::FindModel(uint64_t id) {
    const auto found = std::find_if(m_models.begin(), m_models.end(),
                                    [id](const renderer::ModelAsset& a) { return a.id == id; });
    return found != m_models.end() ? &*found : nullptr;
}

void Application::CreateModelMaterials(uint64_t modelId) {
    renderer::ModelAsset* model = FindModel(modelId);
    if (model == nullptr || !model->geometry) return;
    // 作ったマテリアルは .rockmodel の隣（未保存なら FBX の隣）へ置く。
    const fs::path directory = (!model->assetPath.empty() ? model->assetPath : model->path).parent_path();
    // 未保存のマテリアルは UniquePath がまだファイルを見られない。同じ名前のスロットが並んでも
    // 同じファイルへ書かないよう、この場で決めた置き場所を覚えておく。
    std::vector<fs::path> claimed;
    for (const compositor::MaterialAsset& entry : m_materialLibrary.Entries())
        if (!entry.assetPath.empty()) claimed.push_back(entry.assetPath);
    const auto uniquePath = [&](const std::string& name) {
        for (int suffix = 0; suffix < 1000; ++suffix) {
            const auto path = m_workspace.UniquePath(directory, suffix ? name + "_" + std::to_string(suffix) : name, ".rockmat");
            if (path.empty()) return path;
            if (std::none_of(claimed.begin(), claimed.end(), [&](const fs::path& p) { return p == path; })) {
                claimed.push_back(path);
                return path;
            }
        }
        return fs::path{};
    };
    size_t created = 0;
    for (size_t i = 0; i < model->geometry->slots.size() && i < model->materials.size(); ++i) {
        if (m_materialLibrary.Find(model->materials[i]) != nullptr) continue;
        const renderer::ModelSlotSource& source = model->geometry->slots[i];
        const std::string name = source.name.empty() ? model->name + "_" + std::to_string(i + 1) : source.name;
        const compositor::MaterialAssetId id = m_materialLibrary.Add(name);
        compositor::MaterialAsset* asset = m_materialLibrary.FindMutable(id);
        if (asset == nullptr) continue;
        compositor::TextureId texture = compositor::kNoTexture;
        if (!source.baseColorTexture.empty()) {
            texture = m_textureLibrary.Load(m_device, m_pipelineCache, source.baseColorTexture);
            if (texture == compositor::kNoTexture)
                ROCK_LOG_WARN("テクスチャを読み込めません: %s", ToUtf8Display(source.baseColorTexture).c_str());
        }
        // テクスチャがあればティントは中立（1）、無ければ FBX の拡散色をそのまま使う。
        asset->baseColor = texture;
        if (texture == compositor::kNoTexture) asset->baseColorTint = source.baseColor;
        if (source.opacity < 0.999f) {
            asset->blendMode = compositor::BlendMode::Translucent;
            asset->opacityValue = source.opacity;
        }
        // FBX の透明度が 1 のままでも、テクスチャのアルファで抜いているもの（ガラスなど）がある。
        // アルファに 1 未満の画素があれば、それを不透明度にして半透明で描く。
        if (texture != compositor::kNoTexture) {
            LdrImage image;
            bool translucent = false;
            if (LoadLdrImage(source.baseColorTexture, image)) {
                for (size_t p = 3; p < image.pixels.size() && !translucent; p += 4) translucent = image.pixels[p] < 250;
            }
            if (translucent) {
                asset->opacity = {texture, compositor::TextureChannel::A};
                asset->blendMode = compositor::BlendMode::Translucent;
            }
        }
        if (m_workspace.Contains(directory)) asset->assetPath = uniquePath(name);
        asset->thumbnailDirty = true;
        model->materials[i] = id;
        ++created;
    }
    if (created == 0) {
        ROCK_LOG_INFO("未割り当てのスロットがありません: %s", model->name.c_str());
        return;
    }
    ROCK_LOG_INFO("「%s」のスロットに %zu 個のマテリアルを作成しました", model->name.c_str(), created);
    m_renderedModelThumbnails.erase(modelId);
    m_pendingAssetsSave = true;
    MarkDocumentChanged();
}

// ルート外の FBX は表示中のフォルダへコピーする。FBX が参照するテクスチャも隣へコピーし、
// 「FBX のマテリアルから作成」がコピー先で見つけられるようにする。
uint64_t Application::ImportModelFile(const fs::path& inputPath) {
    for (const renderer::ModelAsset& asset : m_models) m_nextModelId = std::max(m_nextModelId, asset.id + 1);
    {
        const bool inside = m_workspace.Contains(inputPath);
        const fs::path directory = inside ? inputPath.parent_path() : m_assetDirectory;
        if (!inside) {
            renderer::ModelAsset probe;
            if (renderer::LoadModel(inputPath, probe)) {
                for (const renderer::ModelSlotSource& slot : probe.geometry->slots)
                    if (!slot.baseColorTexture.empty()) m_workspace.Import(slot.baseColorTexture, directory);
            }
        }
        const fs::path path = m_workspace.Import(inputPath, directory);
        if (path.empty()) return 0;
        if (const auto found = std::find_if(m_models.begin(), m_models.end(),
                                            [&](const renderer::ModelAsset& a) { return a.path == path; });
            found != m_models.end()) {
            // 読み込み済みの FBX はそのモデルを使う（同じ FBX から 2 つ作らない）。
            return found->id;
        }
        renderer::ModelAsset asset;
        asset.id = m_nextModelId++;
        asset.name = ToUtf8Display(path.stem());
        asset.assetPath = m_workspace.UniquePath(path.parent_path(), asset.name, ".rockmodel");
        if (!renderer::LoadModel(path, asset)) {
            ROCK_LOG_ERROR("モデルを読み込めません（%s）: %s", asset.error.c_str(), ToUtf8Display(path).c_str());
            return 0;
        }
        ROCK_LOG_INFO("モデルを読み込みました: %s（スロット %zu、三角形 %u）", ToUtf8Display(path.filename()).c_str(),
                    asset.geometry->slots.size(), asset.geometry->lods[0].triangles);
        const uint64_t id = asset.id;
        m_models.push_back(std::move(asset));
        m_pendingAssetsSave = true;
        m_assetRefresh = true;
        MarkDocumentChanged();
        return id;
    }
}

void Application::ProcessModelWork() {
    for (const renderer::ModelAsset& asset : m_models) m_nextModelId = std::max(m_nextModelId, asset.id + 1);

    // --- FBX の取り込み（帯のダブルクリック・ドロップ） ---------------------------
    for (const fs::path& inputPath : std::exchange(m_pendingModelImports, {})) {
        const uint64_t id = ImportModelFile(inputPath);
        if (id == 0) continue;
        m_selectedModel = id;
        m_modelLod = 0;
        m_showModelPreview = true;
        if (m_createImportedModelMaterials) CreateModelMaterials(id);
    }
    m_createImportedModelMaterials = false;

    // --- ビューポートへ落としたモデルを置く --------------------------------------
    for (const auto& placement : std::exchange(m_pendingModelPlacements, {})) {
        const auto ext = placement.path.extension().wstring();
        const bool isModelAsset = _wcsicmp(ext.c_str(), L".rockmodel") == 0;
        uint64_t id = 0;
        for (const auto& model : m_models) {
            if ((isModelAsset ? model.assetPath : model.path).lexically_normal() == placement.path.lexically_normal())
                id = model.id;
        }
        if (id == 0 && isModelAsset) {
            if (io::LoadSharedAsset(m_workspace, placement.path, m_device, m_pipelineCache, m_textureLibrary,
                                    m_materialLibrary, m_skyLibrary, true, &m_models)) {
                for (const auto& model : m_models)
                    if (model.assetPath.lexically_normal() == placement.path.lexically_normal()) id = model.id;
                m_assetRefresh = true;
                MarkDocumentChanged();
            }
        } else if (id == 0) {
            id = ImportModelFile(placement.path);
        }
        if (id == 0) {
            ROCK_LOG_ERROR("モデルを置けません: %s", ToUtf8Display(placement.path).c_str());
            continue;
        }
        m_selectedModel = id;
        const graph::GraphId placed = PlaceModel(id, placement.position);
        // 開発用: --model-node-rotation の回転を、コマンドラインで置いたモデルへ掛ける。
        if (graph::Node* node = m_graph.FindMutableNode(placed); node != nullptr && !m_options.modelNodeRotations.empty()) {
            if (auto* settings = std::get_if<graph::ModelNodeSettings>(&node->settings))
                settings->nodeRotations = std::exchange(m_options.modelNodeRotations, {});
        }
    }

    if (m_pendingModelMaterials != 0) {
        CreateModelMaterials(std::exchange(m_pendingModelMaterials, 0));
    }

    // --- シーンから外す（ファイルは残す） --------------------------------------
    if (m_pendingModelRemove != 0) {
        const uint64_t removed = std::exchange(m_pendingModelRemove, 0);
        if (std::erase_if(m_models, [&](const renderer::ModelAsset& a) { return a.id == removed; }) > 0) {
            if (m_selectedModel == removed) m_selectedModel = 0;
            // そのモデルを置いていた Model ノードは「なし」にする（ノードとリンクは残す。アンドゥで戻る）。
            for (graph::Node& node : m_graph.MutableNodes()) {
                if (auto* model = std::get_if<graph::ModelNodeSettings>(&node.settings); model && model->model == removed)
                    model->model = 0;
            }
            m_assetRefresh = true;
            MarkDocumentChanged();
        }
    }

    // --- GPU メッシュ ---------------------------------------------------------
    for (auto it = m_modelPreviews.begin(); it != m_modelPreviews.end();) {
        if (FindModel(it->first) == nullptr) {
            it->second->Destroy(m_device);
            m_renderedModelThumbnails.erase(it->first);
            it = m_modelPreviews.erase(it);
        } else {
            ++it;
        }
    }
    // 窓で選んでいるモデルだけ表示 LOD を使う。帯のサムネイルは同じ出力なので、窓を閉じた後も
    // その LOD の絵が残る（選び直すか LOD を戻すまで）。
    for (const renderer::ModelAsset& asset : m_models) {
        auto& preview = m_modelPreviews[asset.id];
        if (!preview) preview = std::make_unique<renderer::ModelPreview>();
        preview->Prepare(m_device, asset, asset.id == m_selectedModel ? m_modelLod : 0);
    }
}

void Application::RenderModelPreviews(ID3D12GraphicsCommandList* commandList) {
    for (const renderer::ModelAsset& asset : m_models) {
        const auto found = m_modelPreviews.find(asset.id);
        if (found == m_modelPreviews.end()) continue;
        const bool live = m_modelPreviewVisible && asset.id == m_selectedModel;
        if (!live && m_renderedModelThumbnails.contains(asset.id)) continue;
        found->second->Render(m_device, m_pipelineCache, commandList, asset, m_materialLibrary, m_textureLibrary,
                              m_renderer.GetEnvironment(), m_renderer.EnvironmentIntensity(), m_renderer.EffectiveLight(),
                              m_renderer.Exposure().Exposure(), m_renderer.Tonemap());
        if (found->second->HasOutput()) m_renderedModelThumbnails.insert(asset.id);
    }
}

// モデルプレビューの窓。上が回せるモデル、下が寸法・LOD・マテリアルスロット。
// **映すのはアセットの帯で選んでいるモデル。** マテリアルプレビューの窓と同じ作法。
void Application::DrawModelPreviewWindow() {
    m_modelPreviewVisible = false;
    if (!m_showModelPreview) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(420.0f), ui::Scaled(720.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("モデルプレビュー", &m_showModelPreview,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    renderer::ModelAsset* found = FindModel(m_selectedModel);
    if (found == nullptr) {
        ui::HintText("アセットの帯でモデル（.rockmodel / .fbx）をダブルクリックすると開く");
        ImGui::End();
        return;
    }
    renderer::ModelAsset& asset = *found;
    const auto previewIt = m_modelPreviews.find(asset.id);
    renderer::ModelPreview* preview = previewIt != m_modelPreviews.end() ? previewIt->second.get() : nullptr;
    m_modelPreviewVisible = true;

    // --- モデル ------------------------------------------------------------------
    const float paneSize = PreviewPaneSize();
    ImGui::BeginChild("modelPreviewPane", ImVec2(0.0f, paneSize), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float side = std::max(std::min(ImGui::GetContentRegionAvail().x, paneSize), ui::Scaled(32.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - side) * 0.5f));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + side, min.y + side);
        ImGui::InvisibleButton("##model", ImVec2(side, side),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        if (preview != nullptr) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsItemActive()) {
                // 符号と感度はビューポートのカメラと同じ（0.006 ラジアン / px）。
                if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || io.KeyShift)
                    preview->GetCamera().Pan(io.MouseDelta.x, io.MouseDelta.y);
                else
                    preview->GetCamera().Orbit(io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
            }
            if (ImGui::IsItemHovered()) {
                if (io.MouseWheel != 0.0f) preview->GetCamera().Zoom(io.MouseWheel);
                if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
                    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) preview->FocusView();
                    else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) preview->FrameView();
                }
            }
            if (preview->HasOutput())
                ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(preview->OutputHandle().ptr), min, max);
        }
        if (!asset.geometry) ui::MissingThumbnail(min, max);
        ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border),
                                            ImGui::GetStyle().FrameRounding, 0, ui::Scaled(1.0f));
    }
    ImGui::EndChild();
    ImGui::Separator();

    // --- プロパティ（この区画だけスクロールする）----------------------------------
    ImGui::BeginChild("modelPropertyPane", ImVec2(0.0f, 0.0f));
    ui::HintText("左ドラッグ: 回転 / 中・Shift+左ドラッグ: パン / ホイール: ズーム / F: 中心へ / A: 全体");
    if (preview != nullptr && ui::Button("視点を戻す", ui::kWideButtonWidth)) preview->ResetView();
    ImGui::Separator();

    bool changed = false;
    if (ui::BeginPropertyTable("modelBasic")) {
        char name[256];
        std::snprintf(name, sizeof(name), "%s", asset.name.c_str());
        if (ui::PropertyTextInputCommit("名前", name, sizeof(name),
                                        "ファイル名（拡張子なし）と同じ。変えるとファイルも改名する")) {
            changed |= RequestAssetNameChange(asset.assetPath, asset.name, name);
        }
        const std::string source = ToUtf8Display(asset.path.filename());
        ui::PropertyValue("FBX", "%s", source.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToUtf8Display(asset.path).c_str());
        if (asset.geometry) {
            const renderer::ModelGeometry& geometry = *asset.geometry;
            changed |= ui::PropertyFloat("倍率", &asset.scale, 0.001f, 1000.0f, renderer::ModelAsset{}.scale,
                                         "FBX の座標に掛ける。単位の宣言と中身が合わない FBX を実寸（m）へ直す",
                                         "%.3f", ImGuiSliderFlags_Logarithmic);
            asset.scale = std::clamp(asset.scale, 0.001f, 1000.0f);
            ui::PropertyValue("寸法 X / Y / Z", "%.3f / %.3f / %.3f m",
                              (geometry.maximum.x - geometry.minimum.x) * asset.scale,
                              (geometry.maximum.y - geometry.minimum.y) * asset.scale,
                              (geometry.maximum.z - geometry.minimum.z) * asset.scale);
            const int lodCount = static_cast<int>(geometry.lods.size());
            m_modelLod = std::clamp(m_modelLod, 0, lodCount - 1);
            if (lodCount > 1) {
                ui::PropertyInt("表示 LOD", &m_modelLod, 0, lodCount - 1, 0,
                                "0 が最も詳細。FBX に含まれる LOD から選ぶ。表示だけの設定で保存しない");
            }
            ui::PropertyValue("三角形数", "%u", geometry.lods[m_modelLod].triangles);
        }
        ui::EndPropertyTable();
    }
    if (!asset.error.empty()) ui::HintText("読み込めません: %s", asset.error.c_str());

    if (asset.geometry) {
        ui::SectionHeader("マテリアルスロット");
        if (ui::BeginPropertyTable("modelMaterials")) {
            for (size_t i = 0; i < asset.geometry->slots.size() && i < asset.materials.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const std::string& slotName = asset.geometry->slots[i].name;
                const std::string label = slotName.empty() ? "スロット " + std::to_string(i + 1) : slotName;
                changed |= DrawMaterialSlotRow(label.c_str(), asset.materials[i], m_materialLibrary, true,
                                               "FBX のマテリアル名。「なし」は灰色で描く。マテリアルをドラッグして割り当てもできる");
                ImGui::PopID();
            }
            ui::EndPropertyTable();
        }
        ImGui::Separator();
        if (ui::Button("ビューポートに置く", ui::kWideButtonWidth)) {
            // Model ノードを作り、注視点の真下（地面 y = 0）へ置く。帯からビューポートへドラッグしても置ける。
            const auto target = m_renderer.GetCamera().Target();
            PlaceModel(asset.id, {target.x, 0.0f, target.z});
        }
        ui::HintText("Model ノードを作って Mesh Output へ繋ぐ。アセットの帯からビューポートへドラッグすると、落とした所（道路の面か地面）に置く");
        const bool unassigned = std::any_of(asset.materials.begin(), asset.materials.end(), [&](auto id) {
            return m_materialLibrary.Find(id) == nullptr;
        });
        ImGui::BeginDisabled(!unassigned);
        if (ui::Button("FBX のマテリアルから作成", ui::kWideButtonWidth)) m_pendingModelMaterials = asset.id;
        ImGui::EndDisabled();
        ui::HintText("未割り当てのスロットに、FBX の拡散色（テクスチャ）からマテリアルを作って割り当てる");
    }
    if (changed) {
        m_renderedModelThumbnails.erase(asset.id);
        MarkDocumentChanged();
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace rock
