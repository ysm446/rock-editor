#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "app/RoadMaskUi.h"
#include "graph/SurfaceLayoutEditing.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "graph/SurfaceBandGeometry.h"
#include "ui/UiStyle.h"
#include "graph/SurfacePresetGraph.h"
#include "graph/RoadMask.h"
#include <imgui_internal.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "io/SurfaceLayoutIo.h"
#include <nlohmann/json.hpp>

namespace tg {
namespace ed = ax::NodeEditor;
namespace {
graph::CompiledMeshGraph BuildLayerPreviewScene(const graph::LayerMaterial& material, float meters, bool displacement) {
    const auto source = graph::MaterialPreviewPreset(material);
    graph::NodeGraph graph;
    const auto pathId = graph.CreateNode(graph::NodeKind::Path);
    const auto roadId = graph.CreateNode(graph::NodeKind::Road);
    auto& path = std::get<graph::PathNodeSettings>(graph.FindMutableNode(pathId)->settings).path;
    const auto first = graph::AddPathPoint(path, 0, -meters * 0.5f, 0);
    graph::AddPathPoint(path, 0, meters * 0.5f, first);
    std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings).widthMeters = meters;
    graph.CreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(roadId)->inputs[0].id);
    graph::SurfaceLayoutDocument document;
    std::string error;
    if (graph::CreateRoadLayout(document, graph, roadId, error)) {
        const auto id = document.presets[0].id;
        const auto section = document.presets[0].section;
        document.presets[0] = source;
        auto& preset = document.presets[0];
        preset.id = id; preset.section = section; preset.parameters.clear();
        if (!displacement) preset.displacementMeters = 0;
        return graph::CompileSurfaceLayoutPreview(graph, document, roadId);
    }
    graph::CompiledMeshGraph failed; failed.error = error; return failed;
}
const char* PresetNodeLabel(graph::PresetNodeKind kind) {
    switch (kind) {
        case graph::PresetNodeKind::Material: return "素材";
        case graph::PresetNodeKind::Mask: return "マスク";
        case graph::PresetNodeKind::Blend: return "合成";
        default: return "出力";
    }
}
void PresetPin(uint32_t id, const char* label, bool output, bool mask) {
    ed::BeginPin(ed::PinId(id), output ? ed::PinKind::Output : ed::PinKind::Input);
    const auto color = ImGui::GetStyleColorVec4(mask ? ImGuiCol_PlotHistogramHovered : ImGuiCol_Text);
    if (output) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui::TextScaled(130) - ImGui::CalcTextSize(label).x);
    ImGui::TextColored(color, "%s", label);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 pivot(output ? max.x + 8 : min.x - 8, (min.y + max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(pivot, 4, ImGui::ColorConvertFloat4ToU32(color));
    ed::PinPivotRect(pivot, pivot);
    ed::PinRect(ImVec2(min.x - 14, min.y), ImVec2(max.x + 14, max.y));
    ed::EndPin();
}
}
bool Application::DrawSurfacePresetGraph(graph::LayerMaterial& preset) {
    auto& graph = *preset.materialGraph;
    bool changed = false;
    bool frameAll = false;
    if (!m_presetNodeEditor || m_presetEditorId != preset.id) {
        if (m_presetNodeEditor) ed::DestroyEditor(m_presetNodeEditor);
        ed::Config config{}; config.SettingsFile = nullptr; config.NavigateButtonIndex = 2;
        m_presetNodeEditor = ed::CreateEditor(&config);
        m_presetEditorId = preset.id;
        m_selectedPresetNode = graph.nodes.front().id;
        frameAll = true;
    }
    ed::SetCurrentEditor(m_presetNodeEditor);
    if (frameAll) { ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode)); }
    if (ui::Button("層を追加", 100)) {
        if (graph::AppendPresetLayer(graph, m_surfacePresetError)) {
            changed = true; frameAll = true;
            m_selectedPresetNode = graph.nodes.back().id;
            ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode));
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(graph.nodes.size() >= 32);
    for (const auto kind : {graph::PresetNodeKind::Material, graph::PresetNodeKind::Mask, graph::PresetNodeKind::Blend}) {
        if (kind != graph::PresetNodeKind::Material) ImGui::SameLine();
        const std::string label = std::string(PresetNodeLabel(kind)) + "を追加";
        if (ui::Button(label.c_str(), 100)) {
            const auto position = ed::ScreenToCanvas(ImGui::GetCursorScreenPos());
            m_selectedPresetNode = graph::AddPresetNode(graph, kind, {position.x + 40, position.y + 100});
            ed::SetNodePosition(ed::NodeId(m_selectedPresetNode), ImVec2(position.x + 40, position.y + 100));
            ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode));
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (ui::Button("全体を表示", 100)) frameAll = true;
    ImGui::SameLine();
    if (ui::Button("選択を削除", 100)) changed |= graph::DeletePresetNode(graph, m_selectedPresetNode);
    ui::HintText("丸をドラッグして接続。合成の上層には素材を接続します（最大4層）");
    const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f, ui::Scaled(180), ui::Scaled(370));
    const auto screenMin = ImGui::GetCursorScreenPos();
    const ImVec2 screenMax(screenMin.x + ImGui::GetContentRegionAvail().x, screenMin.y + height);
    auto gridColor = ImGui::GetStyleColorVec4(ImGuiCol_Border); gridColor.w = 0;
    ed::PushStyleColor(ed::StyleColor_Bg, gridColor);
    ed::PushStyleColor(ed::StyleColor_Grid, gridColor);
    ed::Begin("presetMaterialGraph", ImVec2(0, height));
    DrawGraphBackground(screenMin, screenMax);
    const bool moving = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    for (const auto& node : graph.nodes) {
        const ed::NodeId id(node.id);
        if (!moving || frameAll) ed::SetNodePosition(id, ImVec2(node.position[0], node.position[1]));
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImGui::GetStyleColorVec4(ImGuiCol_Border));
        ed::BeginNode(id);
        ImGui::TextUnformatted(PresetNodeLabel(node.kind));
        ImGui::Dummy(ImVec2(ui::TextScaled(130), 3));
        if (node.kind == graph::PresetNodeKind::Material) {
            const auto* asset = m_materialLibrary.Find(node.settings.material);
            ui::ThumbnailImage(static_cast<ImTextureID>(m_materialLibrary.ThumbnailHandle(node.settings.material).ptr),
                ui::Scaled(ui::kNodeThumbnail));
            const char* name = asset ? asset->name.c_str() : "定数マテリアル";
            std::string label = name;
            while (!label.empty() && ImGui::CalcTextSize(label.c_str()).x > ui::TextScaled(130)) {
                size_t end = label.size() - 1;
                while (end && (static_cast<unsigned char>(label[end]) & 0xC0) == 0x80) --end;
                label.resize(end);
            }
            ImGui::TextUnformatted(label.c_str());
        }
        if (node.kind == graph::PresetNodeKind::Blend || node.kind == graph::PresetNodeKind::Output)
            PresetPin(node.id * 8 + 1, node.kind == graph::PresetNodeKind::Output ? "面" : "下地", false, false);
        if (node.kind == graph::PresetNodeKind::Blend) {
            PresetPin(node.id * 8 + 2, "上層素材", false, false);
            PresetPin(node.id * 8 + 3, "マスク", false, true);
            ImGui::TextUnformatted(node.settings.blendMode ? "ハイトで競合" : "マスクどおり");
        }
        if (node.kind != graph::PresetNodeKind::Output)
            PresetPin(node.id * 8 + 4, node.kind == graph::PresetNodeKind::Mask ? "マスク出力" : "マテリアル出力", true,
                node.kind == graph::PresetNodeKind::Mask);
        ed::EndNode(); ed::PopStyleColor(2);
    }
    if (frameAll || changed) ed::SelectNode(ed::NodeId(m_selectedPresetNode));
    for (const auto& node : graph.nodes) for (uint32_t input = 0; input < 3; ++input) if (node.inputs[input]) {
        const auto color = ImGui::GetStyleColorVec4(input == 2 ? ImGuiCol_PlotHistogramHovered : ImGuiCol_Text);
        ed::Link(ed::LinkId(node.id * 8 + input + 1), ed::PinId(node.inputs[input] * 8 + 4),
            ed::PinId(node.id * 8 + input + 1), color, 2);
    }
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            uint32_t source = static_cast<uint32_t>(a.Get()), target = static_cast<uint32_t>(b.Get());
            if (target % 8 == 4) std::swap(source, target);
            auto candidate = graph;
            std::string error;
            if (source % 8 == 4 && target % 8 >= 1 && target % 8 <= 3 &&
                graph::ConnectPresetNodes(candidate, source / 8, target / 8, target % 8 - 1, error)) {
                if (ed::AcceptNewItem()) { graph = std::move(candidate); changed = true; m_surfacePresetError.clear(); }
            } else {
                ed::RejectNewItem();
                m_surfacePresetError = error.empty() ? "出力と入力の丸を接続してください" : error;
            }
        }
    }
    ed::EndCreate();
    if (ed::BeginDelete()) {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link)) if (ed::AcceptDeletedItem()) {
            const auto id = static_cast<uint32_t>(link.Get());
            std::string error;
            changed |= graph::ConnectPresetNodes(graph, 0, id / 8, id % 8 - 1, error);
        }
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node)) {
            const auto id = static_cast<uint32_t>(node.Get());
            const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const auto& n) { return n.id == id; });
            if (found != graph.nodes.end() && found->kind != graph::PresetNodeKind::Output) {
                if (ed::AcceptDeletedItem()) changed |= graph::DeletePresetNode(graph, id);
            } else ed::RejectDeletedItem();
        }
    }
    ed::EndDelete();
    ed::NodeId selected;
    if (ed::GetSelectedNodes(&selected, 1)) m_selectedPresetNode = static_cast<uint32_t>(selected.Get());
    for (auto& node : graph.nodes) {
        const auto position = ed::GetNodePosition(ed::NodeId(node.id));
        if (moving && (std::abs(position.x - node.position[0]) > 0.1f || std::abs(position.y - node.position[1]) > 0.1f)) {
            node.position = {position.x, position.y}; changed = true;
        }
    }
    if (frameAll) ed::NavigateToContent(0);
    ed::End(); ed::PopStyleColor(2); ed::SetCurrentEditor(nullptr);
    return changed;
}
void Application::DrawSurfacePresetGraphEditor() {
    if (ui::Button("配置へ戻る", ui::kWideButtonWidth)) { m_editSurfacePreset = 0; return; }
    auto edited = m_surfaceLayouts;
    auto found = std::find_if(edited.layerMaterials.begin(), edited.layerMaterials.end(), [&](const auto& p) { return p.id == m_editSurfacePreset; });
    if (found == edited.layerMaterials.end()) { ui::HintText("プリセットが削除されました。配置へ戻って選び直してください"); return; }
    auto& preset = *found;
    ui::SectionHeader("プリセット編集");
    ui::HintText("ここでの変更は、このプリセットを使うすべての区間へ反映します");
    if (!m_surfacePresetError.empty()) ui::HintText(m_surfacePresetError.c_str());
    if (!preset.materialGraph) preset.materialGraph = graph::MakePresetGraph(preset.materials);
    bool changed = DrawSurfacePresetGraph(preset);
    ImGui::BeginChild("presetProperties", ImVec2(0, 0));
    if (ui::BeginPropertyTable("presetEditorRows")) {
        char name[128]; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
        if (ui::PropertyTextInput("名前", name, sizeof(name), "プリセット一覧に表示する名前") && name[0]) { preset.name = name; changed = true; }
        const graph::LayerMaterial defaults;
        changed |= ui::PropertyFloat("凹凸の高さ", &preset.displacementMeters, 0, 10, defaults.displacementMeters,
            "素材のハイトで押し出す量。0なら形状を変えない", "%.3f m");
        auto selected = std::find_if(preset.materialGraph->nodes.begin(), preset.materialGraph->nodes.end(),
            [&](const auto& n) { return n.id == m_selectedPresetNode; });
        if (selected != preset.materialGraph->nodes.end()) {
            ui::PropertyValue("編集対象", "%s", PresetNodeLabel(selected->kind));
            auto& material = selected->settings;
            const auto kind = selected->kind;
            const graph::PresetMaterial materialDefaults;
            if (kind == graph::PresetNodeKind::Material) {
                changed |= DrawMaterialSlotRow("素材", material.material, m_materialLibrary, true);
                changed |= ui::PropertyFloat("反復長", &material.uvRepeatMeters, 0.01f, 100, materialDefaults.uvRepeatMeters,
                "素材が繰り返す実距離。大きくすると模様が大きくなる", "%.2f m");
                const char* spaces[] = {"面に沿う", "ワールド XZ"};
                int space = material.worldUv ? 1 : 0;
                if (ui::PropertyCombo("座標", &space, spaces, 2, materialDefaults.worldUv ? 1 : 0,
                "面に沿う: 道路の曲がりに追従。ワールド XZ: 地面や隣の面と同じ座標で素材を配置")) {
                    material.worldUv = space == 1; changed = true;
                }
                if (!material.material) {
                    changed |= ui::PropertyColorLinear("色", material.baseColor.data(), materialDefaults.baseColor.data(), "素材未指定時の路面色");
                    changed |= ui::PropertyFloat("粗さ", &material.roughness, 0, 1, materialDefaults.roughness, "大きいほど反射がぼける");
                    changed |= ui::PropertyFloat("金属度", &material.metallic, 0, 1, materialDefaults.metallic, "素材未指定時の金属の割合");
                    changed |= ui::PropertyFloat("AO", &material.ambientOcclusion, 0, 1, materialDefaults.ambientOcclusion, "素材未指定時の環境光の遮蔽。1で遮蔽なし");
                }
            }
            if (kind == graph::PresetNodeKind::Blend) {
                const char* modes[] = {"マスクどおり", "ハイトで競合"};
                int mode = static_cast<int>(material.blendMode);
                if (ui::PropertyCombo("混ぜ方", &mode, modes, 2, static_cast<int>(materialDefaults.blendMode),
                "マスクどおり: 被覆率で混ぜる。ハイトで競合: 素材の高い部分を優先する")) {
                    material.blendMode = static_cast<uint32_t>(mode); changed = true;
                }
                const char* gates[] = {"使わない", "下地の高い所", "下地の低い所"};
                int gate = static_cast<int>(material.heightGate);
                if (ui::PropertyCombo("下地のハイト", &gate, gates, 3, static_cast<int>(materialDefaults.heightGate),
                "下地の凹凸で上層を絞る。粒の露出や低い所に溜まる土を表現する")) {
                    material.heightGate = static_cast<uint32_t>(gate); changed = true;
                }
                if (material.heightGate) {
                    changed |= ui::PropertyFloat("高さのしきい値", &material.heightGateThreshold, 0, 1,
                    materialDefaults.heightGateThreshold, "下地のハイト0〜1のうち、境界にする高さ");
                    changed |= ui::PropertyFloat("高さの柔らかさ", &material.heightGateSoftness, 0.001f, 1,
                    materialDefaults.heightGateSoftness, "高さ条件の境界をぼかす幅");
                }
            }
            if (kind == graph::PresetNodeKind::Mask && material.mask) {
                ImGui::PushID("presetMask");
                changed |= DrawRoadMaskPropertyRows(*material.mask);
                ImGui::PopID();
            }
        }
        changed |= ui::PropertyFloat("ブレンド幅", &preset.layerBlendRange, 0, 1, defaults.layerBlendRange,
            "プリセット内の全層に共通する、ハイトによる境界の柔らかさ");
        ui::EndPropertyTable();
    }
    ImGui::EndChild();
    if (!changed) return;
    std::string error;
    if (graph::ValidateSurfaceLayouts(edited, error)) {
        for (const auto& layout : edited.layouts) for (const auto& band : layout.bands) {
            if (!error.empty() || std::none_of(band.spans.begin(), band.spans.end(), [&](const auto& span) { return graph::PresetLayerMaterial(edited, span.preset) == preset.id; })) continue;
            error = band.side == graph::SurfaceSide::Road
                ? graph::CompileSurfaceLayoutPreview(m_graph, edited, layout.roadNode).error
                : graph::CompileSurfaceBandPreview(m_graph, edited, layout.roadNode, band.id).error;
        }
    }
    m_surfacePresetError = error;
    if (error.empty()) {
        m_surfaceLayouts = std::move(edited);
        m_graph.MarkDirty(); MarkDocumentChanged();
    }
}

void Application::ProcessLayerPreview() {
    if (!m_editSurfacePreset) { m_layerPreviewPreset = 0; return; }
    const auto found = std::find_if(m_surfaceLayouts.layerMaterials.begin(), m_surfaceLayouts.layerMaterials.end(),
        [&](const auto& p) { return p.id == m_editSurfacePreset; });
    if (found == m_surfaceLayouts.layerMaterials.end()) return;
    if (!m_layerPreviewInitialized) {
        m_layerPreview.RequestShadowCascadeCount(1);
        m_layerPreview.RequestShadowResolution(1024);
        if (!m_layerPreview.Initialize(m_device, m_pipelineCache)) { m_layerPreview.Shutdown(m_device); return; }
        m_layerPreviewInitialized = true;
        m_layerPreview.Light() = m_renderer.WorkLight();
        m_layerPreview.ShowSkybox() = false;
        m_layerPreview.ShowReferenceGrid() = false;
        m_layerPreview.TessellationEnabled() = true;
        m_layerPreview.RequestMaterialResolution(512);
        m_layerPreview.Resize(m_device, 768, 768);
    }
    const bool switched = m_layerPreviewPreset != found->id;
    if (switched || m_layerPreviewDirty) {
        auto compiled = BuildLayerPreviewScene(*found, m_layerPreviewMeters, m_layerPreviewDisplacement);
            if (compiled.error.empty() && m_layerPreview.SetGeneratedMeshScene(m_device, compiled.scene)) {
                m_layerPreview.InvalidateSceneMaterials();
                if (switched) {
                    renderer::CameraState camera;
                    camera.yaw = 0.785398f; camera.pitch = 0.61548f;
                    camera.distance = m_layerPreviewMeters * 2.1f;
                    m_layerPreview.GetCamera().SetState(camera);
                }
                m_layerPreviewPreset = found->id; m_layerPreviewDirty = false;
            } else m_surfacePresetError = compiled.error;
    }
    m_layerPreview.Debug() = m_layerPreviewView == 1 ? renderer::DebugView::Height : renderer::DebugView::Shaded;
    m_layerPreview.SetActiveSky(m_renderer.ActiveSky());
    m_layerPreview.Exposure() = m_renderer.Exposure();
    m_layerPreview.Tonemap() = m_renderer.Tonemap();
    m_layerPreview.ProcessPendingWork(m_device, m_pipelineCache);
}


void Application::ProcessLayerThumbnails() {
    if (m_layerThumbnailsDirty) {
        nlohmann::json materials = nlohmann::json::array();
        for (const auto& asset : m_materialLibrary.Entries()) {
            const auto map = [](const auto& slot) { return nlohmann::json{slot.texture, static_cast<uint32_t>(slot.channel)}; };
            materials.push_back(nlohmann::json{asset.id, asset.baseColor, asset.normal,
                map(asset.roughness), map(asset.metallic), map(asset.ambientOcclusion), map(asset.height), map(asset.opacity),
                asset.opacityValue, static_cast<uint32_t>(asset.blendMode), asset.maskThreshold,
                asset.baseColorTint.x, asset.baseColorTint.y, asset.baseColorTint.z,
                asset.hueShiftDegrees, asset.saturation, asset.brightness, asset.flipNormalGreen,
                asset.roughnessValue, asset.metallicValue, asset.ambientOcclusionValue});
        }
        for (auto& entry : m_layerThumbnails) {
            const auto preset = std::find_if(m_surfaceLayouts.layerMaterials.begin(), m_surfaceLayouts.layerMaterials.end(),
                [&](const auto& p) { return p.id == entry.id; });
            if (preset == m_surfaceLayouts.layerMaterials.end()) {
                if (m_layerThumbnailActive == entry.id) m_layerThumbnailActive = 0;
                continue;
            }
            graph::SurfaceLayoutDocument document;
            document.layerMaterials.push_back(*preset);
            const auto key = nlohmann::json{io::WriteSurfaceLayouts(document), materials, m_layerThumbnailTextureRevision}.dump();
            if (entry.contentKey != key) {
                entry.contentKey = key; entry.dirty = true;
                if (m_layerThumbnailActive == entry.id) m_layerThumbnailActive = 0;
            }
        }
        m_layerThumbnailsDirty = false;
    }
    for (auto it = m_layerThumbnails.begin(); it != m_layerThumbnails.end();) {
        if (std::none_of(m_surfaceLayouts.layerMaterials.begin(), m_surfaceLayouts.layerMaterials.end(), [&](const auto& p) { return p.id == it->id; })) {
            m_device.DeferRelease(it->texture); it = m_layerThumbnails.erase(it);
        } else ++it;
    }
    if (m_surfaceLayouts.layerMaterials.empty()) return;
    for (const auto& preset : m_surfaceLayouts.layerMaterials)
        if (std::none_of(m_layerThumbnails.begin(), m_layerThumbnails.end(), [&](const auto& t) { return t.id == preset.id; })) {
            LayerThumbnail entry; entry.id = preset.id; m_layerThumbnails.push_back(std::move(entry));
            m_layerThumbnailsDirty = true;
        }
    if (!m_layerThumbnailInitialized) {
        auto& renderer = m_layerThumbnailRenderer;
        renderer.RequestShadowCascadeCount(1); renderer.RequestShadowResolution(1024);
        if (!renderer.Initialize(m_device, m_pipelineCache)) { renderer.Shutdown(m_device); return; }
        m_layerThumbnailInitialized = true;
        if (!renderer.Resize(m_device, 256, 256)) {
            renderer.Shutdown(m_device); m_layerThumbnailInitialized = false; return;
        }
        renderer.ShowSkybox() = false; renderer.ShowReferenceGrid() = false;
        renderer.TessellationEnabled() = true; renderer.RequestMaterialResolution(256);
        renderer::CameraState camera;
        camera.yaw = 0.785398f; camera.pitch = 0.61548f; camera.distance = 32; camera.fovY = 0.2f;
        renderer.GetCamera().SetState(camera);
        renderer.Light() = m_renderer.WorkLight(); renderer.Exposure() = m_renderer.Exposure();
        renderer.SetActiveSky(m_renderer.ActiveSky()); renderer.Tonemap() = m_renderer.Tonemap();
    }
    if (!m_layerThumbnailActive) {
        auto entry = std::find_if(m_layerThumbnails.begin(), m_layerThumbnails.end(), [](const auto& t) { return t.dirty; });
        if (entry == m_layerThumbnails.end()) return;
        const auto preset = std::find_if(m_surfaceLayouts.layerMaterials.begin(), m_surfaceLayouts.layerMaterials.end(), [&](const auto& p) { return p.id == entry->id; });
        if (!entry->texture.IsValid()) {
            rhi::TextureDesc desc; desc.width = desc.height = 256; desc.debugName = L"LayerMaterialThumbnail";
            if (!m_device.Allocator().CreateTexture2D(desc, entry->texture)) { entry->dirty = false; return; }
        }
        auto compiled = BuildLayerPreviewScene(*preset, 4, true);
        if (!compiled.error.empty() || !m_layerThumbnailRenderer.SetGeneratedMeshScene(m_device, compiled.scene)) {
            entry->dirty = false; return;
        }
        m_layerThumbnailActive = entry->id; m_layerThumbnailFrames = 0;
    }
    m_layerThumbnailRenderer.ProcessPendingWork(m_device, m_pipelineCache);
}

void Application::RenderLayerThumbnails(ID3D12GraphicsCommandList* commandList) {
    if (!m_layerThumbnailActive || !m_layerThumbnailInitialized) return;
    m_layerThumbnailRenderer.Render(m_device, m_pipelineCache, commandList, m_textureLibrary, m_materialLibrary);
    if (++m_layerThumbnailFrames < 3 || m_layerThumbnailRenderer.IsEvaluating()) return;
    const auto entry = std::find_if(m_layerThumbnails.begin(), m_layerThumbnails.end(), [&](const auto& t) { return t.id == m_layerThumbnailActive; });
    if (entry != m_layerThumbnails.end()) {
        entry->ready = m_layerThumbnailRenderer.CopyOutputTo(commandList, entry->texture) || entry->ready;
        entry->dirty = false;
    }
    m_layerThumbnailActive = 0;
}

// レイヤーマテリアルをシーンから外す。ファイル（.tglayer）は残す。配置で使っていれば外さない。
// 一覧はアセットの帯にあり、右クリックの「シーンから外す」から呼ぶ。
bool Application::RemoveLayerMaterialFromScene(graph::SurfaceId id) {
    for (const auto& layout : m_surfaceLayouts.layouts) for (const auto& band : layout.bands)
        for (const auto& span : band.spans)
            if (graph::PresetLayerMaterial(m_surfaceLayouts, span.preset) == id) {
                TG_LOG_WARN("配置で使用中のレイヤーマテリアルはシーンから外せません。割り当てを変更してください");
                return false;
            }
    const auto found = std::find_if(m_surfaceLayouts.layerMaterials.begin(), m_surfaceLayouts.layerMaterials.end(),
        [&](const auto& p) { return p.id == id; });
    if (found == m_surfaceLayouts.layerMaterials.end()) return false;
    if (m_editSurfacePreset == id) m_editSurfacePreset = 0;
    std::erase_if(m_surfaceLayouts.presets, [&](const auto& p) { return p.layerMaterial == id; });
    m_surfaceLayouts.layerMaterials.erase(found);
    m_graph.MarkDirty(); MarkDocumentChanged();
    return true;
}

void Application::DrawSurfacePresetEditor() {
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(1100), ui::Scaled(760)), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("レイヤーマテリアル編集", &open,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End(); if (!open) m_editSurfacePreset = 0; return;
    }
    auto edited = m_surfaceLayouts;
    auto found = std::find_if(edited.layerMaterials.begin(), edited.layerMaterials.end(), [&](const auto& p) { return p.id == m_editSurfacePreset; });
    if (found == edited.layerMaterials.end()) { ImGui::End(); m_editSurfacePreset = 0; return; }
    auto& preset = *found;
    bool changed = false;
    const float previewWidth = std::max(ui::Scaled(220), ImGui::GetContentRegionAvail().x * 0.52f);
    ImGui::BeginChild("layerPreview", ImVec2(previewWidth, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ui::BeginPropertyTable("layerPreviewSettings")) {
        if (ui::PropertyFloat("表示範囲", &m_layerPreviewMeters, 1, 16, 4, "正方形の一辺の実寸", "%.1f m")) m_layerPreviewDirty = true;
        if (ui::PropertyBool("変位を表示", &m_layerPreviewDisplacement, true, "合成後のハイトで平面を変位する")) m_layerPreviewDirty = true;
        const char* views[] = {"マテリアル", "ハイト"};
        ui::PropertyCombo("表示", &m_layerPreviewView, views, 2, 0, "マテリアルの陰影または合成ハイト");
        ui::EndPropertyTable();
    }
    if (ui::Button("視点を戻す", 120)) m_layerPreviewPreset = 0;
    const float hintHeight = ImGui::GetTextLineHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y;
    const float imageSize = std::max(1.0f, std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - hintHeight));
    if (m_layerPreview.HasOutput()) {
        const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(m_layerPreview.OutputHandle().ptr), ImVec2(imageSize, imageSize));
        // 画像上で開始したドラッグを保持し、編集ウインドウの移動と競合させない。
        ImGui::SetCursorScreenPos(imageOrigin);
        ImGui::InvisibleButton("##layerViewportInput", ImVec2(imageSize, imageSize),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
        const bool itemActive = ImGui::IsItemActive();
        const bool itemHovered = ImGui::IsItemHovered();
        HandleLightDrag(m_layerPreview.Light(), m_layerLightInteraction, itemActive);
        HandleCameraInput(m_layerPreview, itemActive, itemHovered);
        const ImVec2 imageMax(imageOrigin.x + imageSize, imageOrigin.y + imageSize);
        DrawLightGizmo(m_layerPreview.Light(), m_layerLightInteraction, m_layerPreview.GetCamera(), imageOrigin, imageMax);
    }
    ui::HintText("Alt＋左: 回転 / 中: 平行移動 / 右: ズーム\nホイール: ズーム / F: 中心 / A: 全体 / L＋左: ライト");
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("layerEditor", ImVec2(0, 0));
    if (preset.materialGraph) {
        ui::HintText("ノード形式のマテリアルです。変換すると出力につながる層を取り込みます。未使用ノードは除かれます（Undo可能）");
        if (ui::Button("レイヤー形式に変換", ui::kWideButtonWidth)) {
            std::vector<graph::PresetMaterial> layers; std::string error;
            if (graph::CompilePresetMaterials(preset, layers, error)) {
                preset.materials = std::move(layers); preset.materialGraph.reset(); changed = true;
            } else m_surfacePresetError = error;
        }
        if (!changed) DrawSurfacePresetGraphEditor();
    } else {
        ui::HintText("上の行ほど上に重なります。このマテリアルを使う全区間へ反映します");
        auto& layers = preset.materials;
        m_selectedPresetLayer = std::clamp(m_selectedPresetLayer, 0, static_cast<int>(layers.size()) - 1);
        ImGui::BeginDisabled(layers.size() >= 4);
        if (ui::Button("追加", 70)) {
            graph::PresetMaterial layer; layer.mask.emplace(); layer.mask->shape = graph::RoadMaskShape::Constant; layer.mask->breakupAmount = 0;
            layers.push_back(layer); m_selectedPresetLayer = static_cast<int>(layers.size()) - 1; changed = true;
        }
        ImGui::SameLine();
        if (ui::Button("複製", 70)) {
            auto layer = layers[m_selectedPresetLayer];
            if (!layer.mask) { layer.mask.emplace(); layer.mask->shape = graph::RoadMaskShape::Constant; layer.mask->breakupAmount = 0; }
            layers.push_back(layer); m_selectedPresetLayer = static_cast<int>(layers.size()) - 1; changed = true;
        }
        ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(layers.size() <= 1);
        if (ui::Button("削除", 70)) { layers.erase(layers.begin() + m_selectedPresetLayer); m_selectedPresetLayer = 0; changed = true; }
        ImGui::EndDisabled();
        for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
            const auto material = layers[i]; ImGui::PushID(i);
            bool enabled = material.enabled;
            if (ui::EyeToggle("visible", &enabled, ui::Scaled(28))) { layers[i].enabled = enabled; changed = true; }
            ImGui::SameLine();
            const auto* asset = m_materialLibrary.Find(material.material);
            if (ui::ThumbnailButton("material", static_cast<ImTextureID>(m_materialLibrary.ThumbnailHandle(material.material).ptr), ui::Scaled(64), i == m_selectedPresetLayer && !m_selectedPresetMask).clicked) {
                m_selectedPresetLayer = i; m_selectedPresetMask = false;
            }
            if (ImGui::BeginDragDropSource()) { ImGui::SetDragDropPayload("TG_PRESET_LAYER", &i, sizeof(i)); ImGui::TextUnformatted(asset ? asset->name.c_str() : "定数マテリアル"); ImGui::EndDragDropSource(); }
            // 行のサムネイルと名前は、レイヤーの並べ替えとマテリアル一覧からのドロップの両方を受ける。
            // 落とした先のレイヤーへ入れ、そのレイヤーを選ぶ（選択中のレイヤーではない）。
            const auto acceptLayerDrops = [&]() {
                if (!ImGui::BeginDragDropTarget()) return;
                if (const auto* payload = ImGui::AcceptDragDropPayload("TG_PRESET_LAYER")) {
                    const int from = *static_cast<const int*>(payload->Data);
                    if (from >= 0 && from < static_cast<int>(layers.size()) && from != i) {
                        auto moved = layers[from]; layers.erase(layers.begin() + from); layers.insert(layers.begin() + i, moved);
                        for (size_t upper = 1; upper < layers.size(); ++upper) if (!layers[upper].mask) {
                            layers[upper].mask.emplace(); layers[upper].mask->shape = graph::RoadMaskShape::Constant; layers[upper].mask->breakupAmount = 0;
                        }
                        m_selectedPresetLayer = i; changed = true;
                    }
                }
                if (const auto* payload = ImGui::AcceptDragDropPayload(kMaterialDragDropType)) {
                    layers[i].material = *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                    m_selectedPresetLayer = i; m_selectedPresetMask = false; changed = true;
                }
                ImGui::EndDragDropTarget();
            };
            acceptLayerDrops();
            ImGui::SameLine();
            const auto pos = ImGui::GetCursorScreenPos(); const float side = ui::Scaled(64);
            if (ImGui::InvisibleButton("mask", ImVec2(side, side))) { m_selectedPresetLayer = i; m_selectedPresetMask = true; }
            auto* draw = ImGui::GetWindowDrawList();
            graph::RoadNodeSettings road; road.widthMeters = m_layerPreviewMeters;
            const auto lanes = graph::ComputeRoadLanes(road, true);
            for (int y = 0; y < 24; ++y) for (int x = 0; x < 24; ++x) {
                const float across = ((x + 0.5f) / 24 - 0.5f) * m_layerPreviewMeters;
                const float distance = (y + 0.5f) / 24 * m_layerPreviewMeters;
                const float value = i == 0 ? 1 : material.mask ? graph::EvaluateRoadMask(*material.mask, across, distance, m_layerPreviewMeters * 0.5f,
                    m_layerPreviewMeters, &lanes, -across, distance - m_layerPreviewMeters * 0.5f, true) : 0;
                draw->AddRectFilled(ImVec2(pos.x + x * side / 24, pos.y + y * side / 24),
                    ImVec2(pos.x + (x + 1) * side / 24, pos.y + (y + 1) * side / 24), ImGui::ColorConvertFloat4ToU32(ImVec4(value, value, value, 1)));
            }
            draw->AddRect(pos, ImVec2(pos.x + side, pos.y + side), ImGui::GetColorU32(i == m_selectedPresetLayer && m_selectedPresetMask ? ImGuiCol_HeaderActive : ImGuiCol_Border));
            ImGui::SameLine();
            if (ImGui::Selectable(asset ? asset->name.c_str() : "定数マテリアル", i == m_selectedPresetLayer, 0, ImVec2(0, side))) m_selectedPresetLayer = i;
            acceptLayerDrops();
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ui::BeginPropertyTable("layerProperties")) {
            char name[128]; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
            // 名前は `.tglayer` のファイル名と同じ。保存済みなら確定でファイルを改名する。
            if (ui::PropertyTextInputCommit("名前", name, sizeof(name), "アセットのファイル名（拡張子なし）。保存済みならファイルも改名する"))
                changed |= RequestAssetNameChange(preset.assetPath, preset.name, name);
            const graph::LayerMaterial defaults;
            changed |= ui::PropertyFloat("凹凸の高さ", &preset.displacementMeters, 0, 10, defaults.displacementMeters, "合成ハイトで押し出す実寸", "%.3f m");
            changed |= ui::PropertyFloat("ブレンド幅", &preset.layerBlendRange, 0, 1, defaults.layerBlendRange, "ハイト境界の柔らかさ");
            auto& material = layers[m_selectedPresetLayer];
            changed |= ui::PropertyBool("レイヤーを表示", &material.enabled, true, "下地を隠すと定数マテリアルを表示する");
            const graph::PresetMaterial materialDefaults;
            if (!m_selectedPresetMask) {
                changed |= DrawMaterialSlotRow("素材", material.material, m_materialLibrary, true);
                changed |= ui::PropertyFloat("反復長", &material.uvRepeatMeters, 0.01f, 100, materialDefaults.uvRepeatMeters,
                "素材が繰り返す実距離。大きくすると模様が大きくなる", "%.2f m");
                const char* spaces[] = {"面に沿う", "ワールド XZ"};
                int space = material.worldUv ? 1 : 0;
                if (ui::PropertyCombo("座標", &space, spaces, 2, materialDefaults.worldUv ? 1 : 0,
                "面に沿う: 道路の曲がりに追従。ワールド XZ: 地面や隣の面と同じ座標で素材を配置")) {
                    material.worldUv = space == 1; changed = true;
                }
                if (!material.material) {
                    changed |= ui::PropertyColorLinear("色", material.baseColor.data(), materialDefaults.baseColor.data(), "素材未指定時の路面色");
                    changed |= ui::PropertyFloat("粗さ", &material.roughness, 0, 1, materialDefaults.roughness, "大きいほど反射がぼける");
                    changed |= ui::PropertyFloat("金属度", &material.metallic, 0, 1, materialDefaults.metallic, "素材未指定時の金属の割合");
                    changed |= ui::PropertyFloat("AO", &material.ambientOcclusion, 0, 1, materialDefaults.ambientOcclusion, "素材未指定時の環境光の遮蔽。1で遮蔽なし");
                }
            }
            if (m_selectedPresetLayer > 0) {
                const char* modes[] = {"マスクどおり", "ハイトで競合"};
                int mode = static_cast<int>(material.blendMode);
                if (ui::PropertyCombo("混ぜ方", &mode, modes, 2, static_cast<int>(materialDefaults.blendMode),
                "マスクどおり: 被覆率で混ぜる。ハイトで競合: 素材の高い部分を優先する")) {
                    material.blendMode = static_cast<uint32_t>(mode); changed = true;
                }
                const char* gates[] = {"使わない", "下地の高い所", "下地の低い所"};
                int gate = static_cast<int>(material.heightGate);
                if (ui::PropertyCombo("下地のハイト", &gate, gates, 3, static_cast<int>(materialDefaults.heightGate),
                "下地の凹凸で上層を絞る。粒の露出や低い所に溜まる土を表現する")) {
                    material.heightGate = static_cast<uint32_t>(gate); changed = true;
                }
                if (material.heightGate) {
                    changed |= ui::PropertyFloat("高さのしきい値", &material.heightGateThreshold, 0, 1,
                    materialDefaults.heightGateThreshold, "下地のハイト0〜1のうち、境界にする高さ");
                    changed |= ui::PropertyFloat("高さの柔らかさ", &material.heightGateSoftness, 0.001f, 1,
                    materialDefaults.heightGateSoftness, "高さ条件の境界をぼかす幅");
                }
            }
            if (m_selectedPresetMask && m_selectedPresetLayer > 0 && material.mask) {
                ImGui::PushID("presetMask");
                changed |= DrawRoadMaskPropertyRows(*material.mask);
                ImGui::PopID();
            }

            if (m_selectedPresetMask && (m_selectedPresetLayer == 0 || !material.mask))
                ui::PropertyValue("マスク", "%s", m_selectedPresetLayer == 0 ? "下地は全面を覆います" : "未設定");
            ui::EndPropertyTable();
        }
        if (m_selectedPresetMask && m_selectedPresetLayer > 0 && !layers[m_selectedPresetLayer].mask && ui::Button("マスクを作成", 140)) {
            layers[m_selectedPresetLayer].mask.emplace();
            layers[m_selectedPresetLayer].mask->shape = graph::RoadMaskShape::Constant;
            layers[m_selectedPresetLayer].mask->breakupAmount = 0;
            changed = true;
        }
    }
    if (!m_surfacePresetError.empty()) ui::HintText("%s", m_surfacePresetError.c_str());
    ImGui::EndChild(); ImGui::End();
    if (!open) m_editSurfacePreset = 0;
    if (changed) {
        std::string error;
        if (graph::ValidateSurfaceLayouts(edited, error)) {
            for (const auto& layout : edited.layouts) for (const auto& band : layout.bands) {
                if (!error.empty() || std::none_of(band.spans.begin(), band.spans.end(), [&](const auto& span) { return graph::PresetLayerMaterial(edited, span.preset) == preset.id; })) continue;
                error = band.side == graph::SurfaceSide::Road
                    ? graph::CompileSurfaceLayoutPreview(m_graph, edited, layout.roadNode).error
                    : graph::CompileSurfaceBandPreview(m_graph, edited, layout.roadNode, band.id).error;
            }
            if (error.empty()) { m_surfaceLayouts = std::move(edited); m_graph.MarkDirty(); MarkDocumentChanged(); }
        }
        m_surfacePresetError = error;
    }
}

}  // namespace tg
