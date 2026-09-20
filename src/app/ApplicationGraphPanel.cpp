#include "graph/Road.h"
#include "graph/ConnectionPrototype.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "graph/SurfaceLayoutEditing.h"
#include "core/Log.h"
#include <chrono>
// ノードグラフパネル。imgui-node-editor によるエディタと、
// 選択中ノードのプロパティ（レイヤーパネルと共有）を持つ。
//
// エディタの作法（カード描画・丸ピン・ドット背景・リンクの作成 / 削除）は
// terrain-editor のノードエディタ UI から移植した。ノードそのものは
// このプロジェクト独自（サーフェス / シェイプ / 水面 / 出力）。

#include "app/Application.h"
#include "graph/SurfaceBandGeometry.h"
#include "app/RoadMaskUi.h"

#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace ed = ax::NodeEditor;

namespace tg {
namespace {

ImU32 ColorToU32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

// 種類ごとのアクセント色。グレー基調を崩さないよう彩度は低め。
ImVec4 NodeAccentColor(graph::NodeKind kind) {
    switch (kind) {
        case graph::NodeKind::Surface:
            return ImVec4(0.55f, 0.66f, 0.58f, 1.0f);
        case graph::NodeKind::Path:
            return ImVec4(0.52f, 0.74f, 0.84f, 1.0f);
        case graph::NodeKind::RoadMarking:
            return ImVec4(0.80f, 0.80f, 0.76f, 1.0f);
        case graph::NodeKind::RoadMask:
            return ImVec4(0.78f, 0.66f, 0.50f, 1.0f);
        case graph::NodeKind::Decal:
            return ImVec4(0.72f, 0.60f, 0.76f, 1.0f);
        case graph::NodeKind::Shoulder:
            return ImVec4(0.70f, 0.64f, 0.52f, 1.0f);
        case graph::NodeKind::Merge:
            return ImVec4(0.62f, 0.70f, 0.66f, 1.0f);
        case graph::NodeKind::Crack:
            return ImVec4(0.66f, 0.58f, 0.62f, 1.0f);
        case graph::NodeKind::Model:
        case graph::NodeKind::Transform:
            return ImVec4(0.62f, 0.60f, 0.78f, 1.0f);
        default:
            return ImVec4(0.59f, 0.64f, 0.68f, 1.0f);
    }
}

// ピンとリンクの色。**線が何を運んでいるかを色で見分ける。**
// 値は terrain-editor に合わせてある（あちらの HeightField がこちらの Material）。
// 緑とオレンジは明度が近く、色相だけが離れているので、
// 暗い盤面でどちらも同じ強さで読める。
ImVec4 PinTypeColor(graph::ValueType valueType) {
    switch (valueType) {
        // パスは水色。線（点とエッジ）が流れる。緑 / オレンジと色相が離れていて、
        // 明度は同じくらいなので暗い盤面で同じ強さで読める。
        case graph::ValueType::Path:
            return ImVec4(0.55f, 0.80f, 0.95f, 1.0f);
        // マテリアルは緑。4 チャンネル一式（ハイトを含む）。
        case graph::ValueType::Mesh:
            return ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
        // 道路空間マスクは茶色寄りのオレンジ。タイル空間のマスク（オレンジ）と区別する。
        case graph::ValueType::RoadMask:
            return ImVec4(0.80f, 0.56f, 0.34f, 1.0f);
        // モデルは藤色。道路のメッシュ（Mesh）とは繋がらないことを色でも分ける。
        case graph::ValueType::Model:
            return ImVec4(0.70f, 0.62f, 0.90f, 1.0f);
        // どちらも受ける入力（Merge / Mesh Output）と、何も繋がっていない Merge の出力は無彩色。
        case graph::ValueType::Any:
            return ImVec4(0.80f, 0.80f, 0.82f, 1.0f);
        case graph::ValueType::Material:
        default:
            return ImVec4(0.70f, 0.93f, 0.78f, 1.0f);
    }
}

// ピンの矩形。当たり判定をラベルまで広げるので、丸の位置は別に持つ。
struct PinGeometry {
    ImVec2 min;     // 丸の矩形
    ImVec2 max;
    ImVec2 center;  // 接続点（リンクの端）
};

// 丸ピンを描いて矩形を返す。**当たり判定（ed::PinRect）は呼び出し側で決める。**
// ラベルまで含めて掴めるようにするため（出力ピンはクリックでプレビューも切り替える）。
// filled が真なら丸を塗る。**ビューポートに出ている出力**の印に使う。
PinGeometry DrawRoundPin(const graph::Pin& pin, bool filled = false) {
    const ImVec2 size(14.0f, 20.0f);
    ImGui::Dummy(size);
    PinGeometry geometry;
    geometry.min = ImGui::GetItemRectMin();
    geometry.max = ImGui::GetItemRectMax();
    geometry.center = ImVec2((geometry.min.x + geometry.max.x) * 0.5f,
                             (geometry.min.y + geometry.max.y) * 0.5f);
    ed::PinPivotRect(ImVec2(geometry.center.x - 6.0f, geometry.center.y - 6.0f),
                     ImVec2(geometry.center.x + 6.0f, geometry.center.y + 6.0f));
    const ImU32 pinColor = ColorToU32(PinTypeColor(pin.valueType));
    if (filled) {
        ImGui::GetWindowDrawList()->AddCircleFilled(geometry.center, 4.3f, pinColor, 16);
    } else {
        ImGui::GetWindowDrawList()->AddCircle(geometry.center, 4.3f, pinColor, 16, 1.6f);
    }
    return geometry;
}

// ドットグリッドの背景。既定のグリッド線は消して自前で描く。
void DrawGraphDots(const ImVec2& screenMin, const ImVec2& screenMax) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMin = ed::ScreenToCanvas(screenMin);
    const ImVec2 canvasMax = ed::ScreenToCanvas(screenMax);
    constexpr float kBaseSpacing = 24.0f;
    const ImU32 backgroundColor = ColorToU32(ImVec4(0.112f, 0.112f, 0.112f, 1.0f));
    const ImU32 dotColor = ColorToU32(ImVec4(0.26f, 0.26f, 0.26f, 0.46f));

    ed::Suspend();
    drawList->PushClipRect(screenMin, screenMax, true);
    drawList->AddRectFilled(screenMin, screenMax, backgroundColor);
    const ImVec2 screen0 = ed::CanvasToScreen(ImVec2(0.0f, 0.0f));
    const ImVec2 screenStep = ed::CanvasToScreen(ImVec2(kBaseSpacing, 0.0f));
    const float baseScreenSpacing = std::max(1.0f, std::abs(screenStep.x - screen0.x));
    // 引きで見たときにドットが密集しないよう、画面上の間隔が保たれる倍率へ広げる。
    const float spacing = kBaseSpacing * std::max(1.0f, std::ceil(12.0f / baseScreenSpacing));
    const float startX = std::floor(std::min(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float endX = std::ceil(std::max(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float startY = std::floor(std::min(canvasMin.y, canvasMax.y) / spacing) * spacing;
    const float endY = std::ceil(std::max(canvasMin.y, canvasMax.y) / spacing) * spacing;
    for (float y = startY; y <= endY; y += spacing) {
        for (float x = startX; x <= endX; x += spacing) {
            const ImVec2 screen = ed::CanvasToScreen(ImVec2(x, y));
            if (screen.x < screenMin.x || screen.x > screenMax.x || screen.y < screenMin.y ||
                screen.y > screenMax.y) {
                continue;
            }
            drawList->AddRectFilled(ImVec2(screen.x - 1.0f, screen.y - 1.0f),
                                    ImVec2(screen.x + 1.0f, screen.y + 1.0f), dotColor);
        }
    }
    drawList->PopClipRect();
    ed::Resume();
}

// ノードの表示名。レイヤー設定を持つ種類はレイヤー名を出す。
const char* NodeDisplayName(const graph::Node& node) {
    if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
        if (!settings->layer.name.empty()) {
            return settings->layer.name.c_str();
        }
    }
    const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
    return (definition != nullptr) ? definition->title : "?";
}

int ToGraphId(uintptr_t id) {
    return static_cast<int>(id);
}

}  // namespace

// 材質スロットの行（Road と Shoulder で共通）。1 は下地、2〜4 は Mask 2〜4 で被覆する。
// 座標と反復長はスロットごと。変更があれば真。
bool Application::DrawMaterialSlotRows(const graph::Node& node, bool* layerWorldUv, float* layerUvRepeatMeters,
                                       float& layerBlendRange, float defaultBlendRange,
                                       uint32_t* layerHeightGate, float* layerHeightGateThreshold, float* layerHeightGateSoftness,
                                       uint32_t* layerBlendMode) {
    bool changed = false;
    ui::SectionHeader("マテリアルスロット");
    if (!ui::BeginPropertyTable("layerRows")) return false;
    static const char* const kUvSpaceLabels[] = {"面に沿う", "ワールド XZ"};
    std::vector<const graph::Pin*> materialPins;
    std::vector<const graph::Pin*> maskPins;
    for (const auto& pin : node.inputs) {
        if (pin.valueType == graph::ValueType::Material) materialPins.push_back(&pin);
        if (pin.valueType == graph::ValueType::RoadMask) maskPins.push_back(&pin);
    }
    for (int slot = 0; slot < graph::kRoadMaterialSlots; ++slot) {
        char label[32];
        std::snprintf(label, sizeof(label), "スロット %d", slot + 1);
        const bool hasMaterial = slot < static_cast<int>(materialPins.size()) &&
                                 m_graph.FindUpstreamNodeForPin(materialPins[slot]->id) != nullptr;
        const bool hasMask = slot == 0 || (slot - 1 < static_cast<int>(maskPins.size()) &&
                                          m_graph.FindUpstreamNodeForPin(maskPins[slot - 1]->id) != nullptr);
        ui::PropertyValue(label, "%s", !hasMaterial ? "マテリアルなし" : (hasMask ? "有効" : "マスクなし（無効）"));
        if (!hasMaterial) continue;
        char spaceId[32];
        std::snprintf(spaceId, sizeof(spaceId), "  座標##slot%d", slot);
        int space = layerWorldUv[slot] ? 1 : 0;
        if (ui::PropertyCombo(spaceId, &space, kUvSpaceLabels, IM_ARRAYSIZE(kUvSpaceLabels), 0,
                              "面に沿う: 道路 UV。ワールド XZ: 位置の XZ 平面。路肩や地面と地続きにする層は XZ")) {
            layerWorldUv[slot] = (space == 1);
            changed = true;
        }
        if (slot > 0) {
            char repeatId[32];
            std::snprintf(repeatId, sizeof(repeatId), "  UV反復長##slot%d", slot);
            changed |= ui::PropertyFloat(repeatId, &layerUvRepeatMeters[slot], 0.1f, 100.0f, 1.0f,
                                         "このスロットのマテリアルで UV が 1 増える実距離", "%.2f m");
            // 混ぜ方。マスクどおりが既定で、マスクを塗った所にそのまま出る。
            static const char* const kBlendModeLabels[] = {"マスクどおり", "ハイトで競合"};
            char modeId[32];
            std::snprintf(modeId, sizeof(modeId), "  混ぜ方##slot%d", slot);
            int mode = static_cast<int>(std::min(1u, layerBlendMode[slot]));
            if (ui::PropertyCombo(modeId, &mode, kBlendModeLabels, IM_ARRAYSIZE(kBlendModeLabels), 0,
                                  "マスクどおり: 被覆率がそのまま重み。境界だけ下地とのハイト差 × ブレンド幅で崩す。"
                                  "ハイトで競合: 被覆率をハイトに足して勝った方が出る（砂利の粒だけ顔を出す表現）")) {
                layerBlendMode[slot] = static_cast<uint32_t>(mode);
                changed = true;
            }
            // 下地のハイトで絞る。Road Mask が「だいたいこの辺」、下地の凹凸が「その中のどこ」。
            static const char* const kGateLabels[] = {"使わない", "下地の高い所", "下地の低い所"};
            char gateId[32];
            std::snprintf(gateId, sizeof(gateId), "  下地のハイト##slot%d", slot);
            int gate = static_cast<int>(std::min(2u, layerHeightGate[slot]));
            if (ui::PropertyCombo(gateId, &gate, kGateLabels, IM_ARRAYSIZE(kGateLabels), 0,
                                  "スロット 1 のハイトで被覆率を絞る。高い所: 砂利の粒が顔を出す。低い所: 土や泥が溜まる")) {
                layerHeightGate[slot] = static_cast<uint32_t>(gate);
                changed = true;
            }
            if (layerHeightGate[slot] != 0u) {
                char thresholdId[32], softnessId[32];
                std::snprintf(thresholdId, sizeof(thresholdId), "  しきい値##gate%d", slot);
                std::snprintf(softnessId, sizeof(softnessId), "  柔らかさ##gate%d", slot);
                changed |= ui::PropertyFloat(thresholdId, &layerHeightGateThreshold[slot], 0.0f, 1.0f, 0.5f,
                                             "下地のハイト（0〜1）のこの値を境にする", "%.2f");
                changed |= ui::PropertyFloat(softnessId, &layerHeightGateSoftness[slot], 0.001f, 1.0f, 0.2f,
                                             "境の遷移幅（ハイト 0〜1 の単位）", "%.2f");
            }
        }
    }
    changed |= ui::PropertyFloat("ブレンド幅", &layerBlendRange, 0.0f, 1.0f, defaultBlendRange,
                                 "スロット同士をハイトで競合させるときの境界の柔らかさ。小さいほど凹凸なりにぎざぎざ", "%.2f");
    ui::EndPropertyTable();
    return changed;
}

namespace {

// エディタへ渡してよい座標か。エディタは**知らないノードの位置を FLT_MAX で返す**ので、
// それを信じて書き戻す・流し込むとノードが無限遠へ飛び、キャンバスの座標計算が
// 壊れて操作できなくなる。読み込んだファイルの値の検証にも使う。
bool IsValidNodePosition(float x, float y) {
    constexpr float kMaxCoordinate = 1.0e6f;
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= kMaxCoordinate &&
           std::abs(y) <= kMaxCoordinate;
}

}  // namespace

void Application::DestroyGraphEditor() {
    if (m_presetNodeEditor) {
        ed::DestroyEditor(m_presetNodeEditor);
        m_presetNodeEditor = nullptr;
    }
    if (m_nodeEditor != nullptr) {
        ed::DestroyEditor(m_nodeEditor);
        m_nodeEditor = nullptr;
    }
}

void Application::DrawGraphBackground(const ImVec2& min, const ImVec2& max) {
    DrawGraphDots(min, max);
}

void Application::RequestGraphNodePlacement(bool navigate) {
    m_graphNodesToPlace.clear();
    for (const graph::Node& node : m_graph.Nodes()) {
        m_graphNodesToPlace.push_back(node.id);
    }
    // 全体の流し込みの後だけ画面へ収め直す。1 個の追加やアンドゥでは
    // 視点を動かさない（そのたびに視点が飛ぶと編集にならない）。
    if (navigate) {
        m_graphNavigateCountdown = 3;
    }
}

// ビューポートに出すノードを決める。**選択とは別**に持つので、
// 結果を見ながら別のノードのプロパティをいじれる。
void Application::SetPreviewGraphNode(graph::GraphId nodeId, graph::GraphId outputPin) {
    const graph::Node* node = m_graph.FindNode(nodeId);
    // Mesh Output と、プレビューできない種類（Surface / Path）は「Mesh Output の鎖」に落とす。
    const bool previewable = (node != nullptr && graph::IsPreviewableNodeKind(node->kind));
    m_previewGraphNode = previewable ? node->id : 0;
    // 見る出力。そのノードの出力ピンでなければ 0（＝最初の出力）に落とす。
    m_previewGraphPin = 0;
    if (previewable) {
        for (const graph::Pin& pin : node->outputs) {
            if (pin.id == outputPin) {
                m_previewGraphPin = pin.id;
                break;
            }
        }
    }
}

void Application::SyncMeshGraph() {
    // 途中のメッシュノード（Road / Lane Marking / Decal）を見ているときは、そのノードまでの鎖を出す。
    graph::GraphId previewMeshNode = 0;
    if (const graph::Node* node = m_graph.FindNode(m_previewGraphNode);
        node != nullptr && (graph::IsMeshNodeKind(node->kind) || graph::IsModelNodeKind(node->kind))) {
        previewMeshNode = node->id;
    }
    if (m_meshGraphRevision == m_graph.Revision() && m_meshGraphPreviewNode == previewMeshNode) return;
    const auto compileStart = std::chrono::steady_clock::now();
    auto compiled = m_options.surfaceLayoutRoad != 0
        ? graph::CompileSurfaceLayoutPreview(m_graph, m_surfaceLayouts, m_options.surfaceLayoutRoad)
        : m_options.prototypeRoad != 0
        ? graph::CompileConnectionPrototype(m_graph, m_options.prototypeRoad, m_options.prototypeGravel,
                                            m_options.prototypeSidewalk, m_options.prototypeDisplacement)
        : graph::CompileMeshGraphWithLayouts(m_graph, m_surfaceLayouts, previewMeshNode);
    if (m_previewSurfaceBands) {
        for (const auto& layout : m_surfaceLayouts.layouts) {
            if (std::find(compiled.meshSources.begin(), compiled.meshSources.end(), layout.roadNode) == compiled.meshSources.end()) continue;
            graph::SurfaceId leftBand = 0, rightBand = 0;
            size_t leftCount = 0, rightCount = 0;
            for (const auto& band : layout.bands) {
                if (band.spans.empty()) continue;
                if (band.side == graph::SurfaceSide::Left) { leftBand = band.id; ++leftCount; }
                if (band.side == graph::SurfaceSide::Right) { rightBand = band.id; ++rightCount; }
            }
            const bool connectBoth = m_connectSurfaceBands && leftCount == 1 && rightCount == 1;
            if (connectBoth) {
                std::string error;
                if (graph::ConnectSurfaceLayoutBands(compiled, m_graph, m_surfaceLayouts, layout.roadNode,
                                                  leftBand, rightBand, error, m_displaceConnectedBands)) continue;
                if (!compiled.error.empty()) compiled.error += " / ";
                compiled.error += error;
            }
            for (const auto& band : layout.bands) {
                if (band.side == graph::SurfaceSide::Road || band.spans.empty()) continue;
                if (std::count_if(layout.bands.begin(), layout.bands.end(), [&](const auto& other) {
                    return other.side == band.side && !other.spans.empty();
                }) > 1) {
                    if (!compiled.error.empty()) compiled.error += " / ";
                    compiled.error += "沿道形状の試作は左右それぞれ1帯に対応します";
                    continue;
                }
                auto preview = graph::CompileSurfaceBandPreview(m_graph, m_surfaceLayouts, layout.roadNode, band.id);
                const auto selectedSide = m_surfaceBandSide == 0 ? graph::SurfaceSide::Left : graph::SurfaceSide::Right;
                const bool hasSelectedSide = std::any_of(layout.bands.begin(), layout.bands.end(), [&](const auto& other) {
                    return other.side == selectedSide && !other.spans.empty();
                });
                if (m_connectSurfaceBands && !connectBoth && (band.side == selectedSide || !hasSelectedSide) && preview.error.empty()) {
                    std::string error;
                    if (graph::ConnectSurfaceLayoutBands(compiled, m_graph, m_surfaceLayouts, layout.roadNode,
                        band.side == graph::SurfaceSide::Left ? band.id : 0, band.side == graph::SurfaceSide::Right ? band.id : 0, error, m_displaceConnectedBands)) continue;
                    if (!compiled.error.empty()) compiled.error += " / ";
                    compiled.error += error;
                }
                if (preview.error.empty()) {
                    const int offset = static_cast<int>(compiled.scene.meshes.size());
                    for (auto& source : preview.scene.meshes[0].connectionSources) source += offset;
                    for (auto& mesh : preview.scene.meshes) {
                        compiled.scene.meshes.push_back(std::move(mesh)); compiled.meshSources.push_back(0);
                    }
                } else {
                    if (!compiled.error.empty()) compiled.error += " / ";
                    compiled.error += preview.error;
                }
            }
        }
    }
    if (m_options.prototypeRoad != 0 || m_options.surfaceLayoutRoad != 0 || m_options.measurePreview) {
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();
        size_t vertices = 0, triangles = 0, maskBytes = 0;
        for (const auto& mesh : compiled.scene.meshes) {
            vertices += mesh.geometry.vertices.size();
            triangles += mesh.geometry.indices.size() / 3;
            maskBytes += mesh.roadMask.rgba.size();
        }
        TG_LOG_INFO("道路生成 (%s): %.2f ms, %zu 頂点, %zu 三角形, マスク %zu bytes",
                    m_options.surfaceLayoutRoad != 0 ? "layout" : m_options.prototypeRoad != 0 ? "connection" : "ordinary", elapsed, vertices, triangles, maskBytes);
        if (!compiled.error.empty()) TG_LOG_ERROR("道路生成: %s", compiled.error.c_str());
    }
    // 鎖が無くなったらシーンを空にする（グリッドと背景だけになる）。
    bool uploaded = true;
    if (compiled.active) {
        uploaded = m_renderer.SetGeneratedMeshScene(m_device, compiled.scene);
    } else if (m_meshGraphActive) {
        m_renderer.ClearMeshScene(m_device);
    }
    m_meshGraphError = compiled.error;
    if (!uploaded) m_meshGraphError = "道路メッシュをGPUへ転送できませんでした";
    if (uploaded) {
        m_meshGraphActive = compiled.active;
        m_meshHighlight = MeshHighlightState{};
    }
    m_meshGraphRevision = m_graph.Revision();
    m_meshGraphPreviewNode = previewMeshNode;
}

// 選択中のノードを控える。
void Application::CopySelectedGraphNodes() {
    std::vector<const graph::Node*> nodes;
    for (const graph::GraphId id : m_selectedGraphNodes) {
        const graph::Node* node = m_graph.FindNode(id);
        if (node != nullptr) {
            nodes.push_back(node);
        }
    }
    if (nodes.empty()) {
        return;
    }

    m_graphClipboard.clear();
    m_graphPasteCount = 0;
    for (const graph::Node* node : nodes) {
        GraphClipboardNode entry;
        entry.kind = node->kind;
        entry.settings = node->settings;
        entry.posX = node->posX;
        entry.posY = node->posY;
        const ImVec2 size = ed::GetNodeSize(ed::NodeId(node->id));
        entry.sizeX = size.x;
        entry.sizeY = size.y;
        for (const graph::Pin& pin : node->inputs) {
            GraphClipboardNode::Source source;
            // この入力へ繋がっているリンクの「出力ピン」を覚える。
            for (const graph::Link& link : m_graph.Links()) {
                if (link.endPin != pin.id) {
                    continue;
                }
                const graph::Pin* startPin = m_graph.FindPin(link.startPin);
                if (startPin == nullptr) {
                    break;
                }
                // コピーした集合の中を指しているなら、貼った側どうしで繋ぎ直す。
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i]->id == startPin->nodeId) {
                        source.copiedIndex = static_cast<int>(i);
                        break;
                    }
                }
                // 集合の外なら、**元の親へ繋いだまま**にする。
                if (source.copiedIndex < 0) {
                    source.externalPin = link.startPin;
                }
                break;
            }
            entry.inputs.push_back(source);
        }
        m_graphClipboard.push_back(std::move(entry));
    }
    TG_LOG_INFO("ノードをコピーしました: %zu 個", m_graphClipboard.size());
}

void Application::PasteGraphNodes(const ImVec2& viewCenter) {
    if (m_graphClipboard.empty()) {
        return;
    }
    // 貼るたびに少しずらす。同じ場所に重ねると、貼れたのかどうか分からない。
    // **4 回で一巡させる。** 増やし続けると、貼るほど画面の中央から遠ざかる。
    ++m_graphPasteCount;
    const float offset = 28.0f * static_cast<float>(m_graphPasteCount % 4);

    // **貼る先は今見えている所**。コピー元が画面の外にあっても、貼ったノードが
    // どこかへ消えないように、集合の中心をキャンバスの中央へ持ってくる。
    // 集合の中の相対の配置はそのまま。
    float minX = m_graphClipboard.front().posX;
    float minY = m_graphClipboard.front().posY;
    float maxX = minX;
    float maxY = minY;
    for (const GraphClipboardNode& entry : m_graphClipboard) {
        minX = std::min(minX, entry.posX);
        minY = std::min(minY, entry.posY);
        maxX = std::max(maxX, entry.posX + entry.sizeX);
        maxY = std::max(maxY, entry.posY + entry.sizeY);
    }
    const float deltaX = viewCenter.x - (minX + maxX) * 0.5f + offset;
    const float deltaY = viewCenter.y - (minY + maxY) * 0.5f + offset;

    std::vector<graph::GraphId> created(m_graphClipboard.size(), 0);
    for (size_t i = 0; i < m_graphClipboard.size(); ++i) {
        const GraphClipboardNode& entry = m_graphClipboard[i];
        const graph::GraphId nodeId = m_graph.CreateNode(entry.kind);
        graph::Node* node = m_graph.FindMutableNode(nodeId);
        if (node == nullptr) {
            continue;
        }
        node->settings = entry.settings;
        node->posX = entry.posX + deltaX;
        node->posY = entry.posY + deltaY;
        node->positionValid = true;
        created[i] = nodeId;
        m_graphNodesToPlace.push_back(nodeId);
    }

    // 接続を張り直す。集合の中どうしは貼った側で、外は**元の親のまま**繋ぐ。
    // 出力側（自分を使っていた下流）は繋がない。入力ピンは 1 本しか持てないので、
    // 繋ぐと元のノードから奪ってしまう。
    for (size_t i = 0; i < m_graphClipboard.size(); ++i) {
        const graph::Node* node = m_graph.FindNode(created[i]);
        if (node == nullptr) {
            continue;
        }
        const GraphClipboardNode& entry = m_graphClipboard[i];
        for (size_t pinIndex = 0; pinIndex < entry.inputs.size(); ++pinIndex) {
            if (pinIndex >= node->inputs.size()) {
                break;
            }
            const GraphClipboardNode::Source& source = entry.inputs[pinIndex];
            const graph::GraphId endPin = node->inputs[pinIndex].id;
            if (source.copiedIndex >= 0 &&
                static_cast<size_t>(source.copiedIndex) < created.size()) {
                const graph::Node* upstream = m_graph.FindNode(created[source.copiedIndex]);
                if (upstream != nullptr && !upstream->outputs.empty()) {
                    m_graph.CreateLink(upstream->outputs.front().id, endPin);
                }
            } else if (source.externalPin != 0) {
                // 元のノードが消えていれば CanCreateLink が弾く（何も起きない）。
                m_graph.CreateLink(source.externalPin, endPin);
            }
        }
    }

    for (const graph::GraphId id : created) {
        if (id != 0) {
            m_selectedGraphNode = id;
            break;
        }
    }
    MarkDocumentChanged();
    TG_LOG_INFO("ノードを貼り付けました: %zu 個", m_graphClipboard.size());
}

bool Application::IsGraphPinVisible(const graph::Pin& pin) const {
    if (pin.valueType != graph::ValueType::Material && pin.valueType != graph::ValueType::RoadMask) return true;
    for (const auto& layout : m_surfaceLayouts.layouts) {
        if (layout.roadNode != pin.nodeId) continue;
        for (const auto& band : layout.bands)
            if (band.side == graph::SurfaceSide::Road && !band.spans.empty()) return false;
    }
    return true;
}

void Application::DrawGraphNode(const graph::Node& node) {
    // ノードの幅。**ピンのラベルが重ならない幅まで広げる。**
    // 入力は左、出力は右へ寄せるので、同じ行に並ぶ 2 つのラベルの合計が要る幅になる。
    // 200px 固定にしていたときは、Mask Blend の Foreground / Background のような
    // 長い名前が出力の Mask と重なっていた。
    constexpr float kNodeMinWidth = 200.0f;
    // 丸ピン 1 つぶん（丸の幅 + ImGui の項目間隔）。
    const float pinWidth = 14.0f + ImGui::GetStyle().ItemSpacing.x;
    float rowWidth = 0.0f;
    std::vector<const graph::Pin*> visibleInputs;
    for (const graph::Pin& input : node.inputs) {
        if (!IsGraphPinVisible(input)) continue;
        visibleInputs.push_back(&input);
    }
    for (size_t row = 0; row < std::max(visibleInputs.size(), node.outputs.size()); ++row) {
        float width = 0.0f;
        if (row < visibleInputs.size()) {
            width += pinWidth + ImGui::CalcTextSize(visibleInputs[row]->label.c_str()).x;
        }
        if (row < node.outputs.size()) {
            width += ImGui::CalcTextSize(node.outputs[row].label.c_str()).x + pinWidth;
        }
        rowWidth = std::max(rowWidth, width);
    }
    // 入力と出力のラベルの間に隙間を空ける。詰まっていると 1 語に見える。
    const float kNodeWidth = std::max(kNodeMinWidth, rowWidth + 24.0f);
    const ImVec4 accent = NodeAccentColor(node.kind);
    // **プレビュー中のノードは枠を明るくする。** 選択（プロパティ）と
    // プレビューは別なので、どれが画面に出ているのかが分かるようにする。
    const bool isPreview = (node.id == m_previewGraphNode);
    const ImVec4 nodeBorderColor = isPreview ? ImVec4(0.72f, 0.76f, 0.62f, 1.0f)
                                             : ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    const ImVec4 activeNodeBorderColor(0.59f, 0.64f, 0.68f, 1.0f);
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12.0f, 10.0f, 12.0f, 10.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 6.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, isPreview ? 2.0f : 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.150f, 0.150f, 0.150f, 0.98f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, activeNodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, activeNodeBorderColor);

    ed::BeginNode(ed::NodeId(node.id));

    // ヘッダ: 種類色の印 + 名前。レイヤーが無効なら名前を落とした色で描く。
    const auto* layerSettings = std::get_if<graph::LayerNodeSettings>(&node.settings);
    const bool enabled = (layerSettings == nullptr) || layerSettings->layer.enabled;
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(cursor.x, cursor.y + 3.0f),
                                ImVec2(cursor.x + 10.0f, cursor.y + 13.0f),
                                ColorToU32(accent), 2.0f);
        ImGui::Dummy(ImVec2(16.0f, 16.0f));
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
        const ImVec4 titleColor =
            enabled ? ImVec4(0.88f, 0.88f, 0.88f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        ImGui::TextColored(titleColor, "%s", NodeDisplayName(node));
        if (isPreview) {
            // ビューポートに出ている印。名前の右に小さく添える。
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.62f, 1.0f), "●");
        }
        // 種類はヘッダの下に小さく添える。名前と種類の両方が分かるようにする。
        if (const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
            definition != nullptr && layerSettings != nullptr) {
            ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.55f, 1.0f), "%s%s", definition->title,
                               enabled ? "" : "（無効）");
        }
    }

    // サムネイル。**繋ぎ替えずに中身が分かる**ようにするためのもの。
    //   - Surface: 割り当てた材質のサムネイル（マテリアル一覧と同じ球の絵）。
    //   - 材質が無ければ枠だけの空き。
    {
        const float thumbnailSize = ui::Scaled(ui::kNodeThumbnail);
        if (layerSettings != nullptr) {
            ImGui::Dummy(ImVec2(kNodeWidth, 2.0f));
            D3D12_GPU_DESCRIPTOR_HANDLE result{0};
            if (layerSettings->layer.material != compositor::kNoMaterialAsset) {
                if (const compositor::MaterialAsset* asset = m_materialLibrary.Find(layerSettings->layer.material);
                    asset != nullptr && asset->thumbnail.IsValid()) {
                    result = asset->thumbnail.srv.gpu;
                }
            }
            ui::ThumbnailImage(static_cast<ImTextureID>(result.ptr), thumbnailSize);
            // マテリアル一覧からサムネイルへ落とすと、そのノードに割り当たる
            // （Surface だけ）。ID の無いアイテムでも BeginDragDropTarget は矩形から
            // ID を作るので受けられる。
            if (node.kind == graph::NodeKind::Surface && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kMaterialDragDropType);
                    payload != nullptr) {
                    const auto dropped =
                        *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                    if (graph::Node* mutableNode = m_graph.FindMutableNode(node.id)) {
                        if (auto* mutableSettings =
                                std::get_if<graph::LayerNodeSettings>(&mutableNode->settings);
                            mutableSettings != nullptr &&
                            mutableSettings->layer.material != dropped) {
                            mutableSettings->layer.material = dropped;
                            m_graph.MarkDirty();
                            MarkDocumentChanged();
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    ImGui::Dummy(ImVec2(kNodeWidth, 8.0f));
    const float rowStartX = ImGui::GetCursorPosX();
    const float rowY = ImGui::GetCursorPosY();

    // **ラベルもピンの当たり判定に入れる。** 丸だけだと小さく、
    // 出力ピンのクリック（プレビューの切り替え）も接続も狙いにくい。
    const ImVec4 pinLabelColor(0.62f, 0.64f, 0.62f, 1.0f);

    for (size_t inputIndex = 0; inputIndex < visibleInputs.size(); ++inputIndex) {
        const graph::Pin& input = *visibleInputs[inputIndex];
        const float inputY = rowY + static_cast<float>(inputIndex) * 24.0f;
        ImGui::SetCursorPos(ImVec2(rowStartX, inputY));
        ed::BeginPin(ed::PinId(input.id), ed::PinKind::Input);
        const PinGeometry geometry = DrawRoundPin(input);
        ImGui::SameLine();
        ImGui::SetCursorPosY(inputY + 2.0f);
        ImGui::TextColored(pinLabelColor, "%s", input.label.c_str());
        // 丸からラベルの右端まで。**縦は丸の高さに揃える**（行が重ならないように）。
        ed::PinRect(geometry.min, ImVec2(ImGui::GetItemRectMax().x, geometry.max.y));
        ed::EndPin();
    }

    for (size_t outputIndex = 0; outputIndex < node.outputs.size(); ++outputIndex) {
        const graph::Pin& output = node.outputs[outputIndex];
        // **どの出力を見ているか**を丸の塗りで示す。堆積のように出力が 2 つある
        // ノードでは、Result と Mask のどちらが画面に出ているのかが要る。
        const bool previewOutput = isPreview && ((output.id == m_previewGraphPin) ||
                                                 (m_previewGraphPin == 0 && outputIndex == 0));
        const float outputY = rowY + static_cast<float>(outputIndex) * 24.0f;
        const float labelWidth = ImGui::CalcTextSize(output.label.c_str()).x;
        ImGui::SetCursorPos(ImVec2(rowStartX + kNodeWidth - labelWidth - 22.0f, outputY + 2.0f));
        ed::BeginPin(ed::PinId(output.id), ed::PinKind::Output);
        ImGui::TextColored(pinLabelColor, "%s", output.label.c_str());
        const ImVec2 labelMin = ImGui::GetItemRectMin();
        ImGui::SameLine();
        ImGui::SetCursorPosY(outputY);
        const PinGeometry geometry = DrawRoundPin(output, previewOutput);
        // ラベルの左端から丸まで。
        ed::PinRect(ImVec2(labelMin.x, geometry.min.y), geometry.max);
        ed::EndPin();
    }
    const size_t pinRowCount = std::max(visibleInputs.size(), node.outputs.size());
    ImGui::Dummy(
        ImVec2(kNodeWidth, std::max(4.0f, static_cast<float>(pinRowCount) * 24.0f - 20.0f)));

    ed::EndNode();
    ed::PopStyleColor(4);
    ed::PopStyleVar(4);
}

void Application::DrawGraphEditor() {
    if (m_nodeEditor == nullptr) {
        ed::Config config{};
        // 位置は Node が持ち、プロジェクトに保存する。エディタ側の設定ファイルは使わない。
        config.SettingsFile = nullptr;
        config.NavigateButtonIndex = 2;
        m_nodeEditor = ed::CreateEditor(&config);
        RequestGraphNodePlacement();
    }

    static ImVec2 addNodePosition(0.0f, 0.0f);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvasMax(canvasMin.x + avail.x, canvasMin.y + avail.y);
    // ホバー判定は ed::Begin より前に取る。フレーム内では io.MousePos が
    // キャンバス座標に差し替えられていて、スクリーン座標の矩形と比べられない。
    const bool canvasHovered = ImGui::IsMouseHoveringRect(canvasMin, canvasMax);

    ed::SetCurrentEditor(m_nodeEditor);
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::Begin("terrainGraphEditor", avail);
    DrawGraphDots(canvasMin, canvasMax);


    // 積まれた位置要求をエディタへ流し込む。まだ位置を持たないノード
    // （と、壊れた座標を持つノード）には現在のビューの中央を与える。
    if (!m_graphNodesToPlace.empty()) {
        for (const graph::GraphId nodeId : m_graphNodesToPlace) {
            graph::Node* node = m_graph.FindMutableNode(nodeId);
            if (node == nullptr) {
                continue;
            }
            if (!node->positionValid || !IsValidNodePosition(node->posX, node->posY)) {
                const ImVec2 center = ed::ScreenToCanvas(
                    ImVec2((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f));
                node->posX = center.x;
                node->posY = center.y;
                node->positionValid = true;
            }
            ed::SetNodePosition(ed::NodeId(node->id), ImVec2(node->posX, node->posY));
            // 追加やアンドゥで選ばれたノードは、エディタ側の選択も合わせる。
            // 合わせないと、次のフレームの選択同期（未選択 → 0）に消されてしまう。
            if (nodeId == m_selectedGraphNode) {
                ed::SelectNode(ed::NodeId(nodeId));
            }
        }
        m_graphNodesToPlace.clear();
    }

    for (const graph::Node& node : m_graph.Nodes()) {
        DrawGraphNode(node);
    }

    // A でグラフ全体を画面に収める（ビューポートの A と同じ作法）。
    // 内容の矩形は live なノードから計算されるため、描画の後に呼ぶ。
    const ImGuiIO& io = ImGui::GetIO();
    if (canvasHovered && !io.WantTextInput && !io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        ed::NavigateToContent();
    }

    // Ctrl+C / Ctrl+V でノードをコピーする。**キャンバスの上にいるときだけ**
    // 拾う（名前の入力中や他のパネルの操作を横取りしない）。
    if (canvasHovered && !io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopySelectedGraphNodes();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            PasteGraphNodes(ed::ScreenToCanvas(ImVec2((canvasMin.x + canvasMax.x) * 0.5f,
                                                      (canvasMin.y + canvasMax.y) * 0.5f)));
        }
    }

    // 位置を流し込んだ後の整列。**ノードを描いた後**でないと内容の矩形が空で
    // 何も起きない（live なノードから計算されるため）。さらに、エディタは
    // キャンバスのサイズ変化のたびに前の表示領域を復元する（ed::Begin 内）ので、
    // ドックの確定を待ってサイズが安定してから寄せる。
    const bool canvasStable =
        (avail.x == m_graphCanvasSize.x && avail.y == m_graphCanvasSize.y);
    m_graphCanvasSize = avail;
    if (m_graphNavigateCountdown > 0 && m_graphNodesToPlace.empty() && canvasStable) {
        if (--m_graphNavigateCountdown == 0) {
            ed::NavigateToContent(0.0f);
        }
    }

    for (const graph::Link& link : m_graph.Links()) {
        if (const auto* pin = m_graph.FindPin(link.endPin); pin && !IsGraphPinVisible(*pin)) {
            ed::DeselectLink(ed::LinkId(link.id));
            continue;
        }
        ImVec4 color(0.52f, 0.60f, 0.55f, 1.0f);
        if (const graph::Pin* startPin = m_graph.FindPin(link.startPin)) {
            color = PinTypeColor(startPin->valueType);
        }
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPin), ed::PinId(link.endPin), color,
                 2.5f);
    }

    // --- リンクの作成 -------------------------------------------------------
    if (ed::BeginCreate(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f)) {
        ed::PinId startPinId;
        ed::PinId endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId)) {
            const int startPin = ToGraphId(startPinId.Get());
            const int endPin = ToGraphId(endPinId.Get());
            if (m_graph.CanCreateLink(startPin, endPin)) {
                if (ed::AcceptNewItem(ImVec4(0.70f, 0.78f, 0.72f, 1.0f), 3.0f)) {
                    if (m_graph.CreateLink(startPin, endPin)) {
                        MarkDocumentChanged();
                    }
                }
            } else {
                ed::RejectNewItem(ImVec4(0.78f, 0.28f, 0.24f, 1.0f), 2.0f);
            }
        }
    }
    ed::EndCreate();

    // --- リンクとノードの削除 -----------------------------------------------
    if (ed::BeginDelete()) {
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId)) {
            if (ed::AcceptDeletedItem()) {
                if (m_graph.DeleteLink(ToGraphId(deletedLinkId.Get()))) {
                    MarkDocumentChanged();
                }
            }
        }
        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId)) {
            if (ed::AcceptDeletedItem()) {
                const int nodeId = ToGraphId(deletedNodeId.Get());
                if (m_graph.DeleteNode(nodeId)) {
                    std::erase_if(m_surfaceLayouts.layouts, [nodeId](const auto& layout) { return layout.roadNode == nodeId; });
                    MarkDocumentChanged();
                    if (m_previewGraphNode == nodeId) {
                        m_previewGraphNode = 0;
                        m_previewGraphPin = 0;
                    }
                    if (m_selectedGraphNode == nodeId) {
                        m_selectedGraphNode = 0;
                    }
                }
            }
        }
    }
    ed::EndDelete();

    // --- 背景の右クリックでノードを追加 -------------------------------------
    if (ed::ShowBackgroundContextMenu()) {
        // エディタのフレーム内では io.MousePos が**キャンバス座標に差し替えられている**
        // （imgui_canvas が Begin で変換する）。そのまま使う。ScreenToCanvas を
        // 重ねると二重変換になり、ノードが視界の外へ飛ぶ（実際に踏んだ）。
        addNodePosition = ImGui::GetMousePos();
        // 念のため現在の視界へ収める。視界の外に生まれると見失う。
        // 深いズームでは上限が下限を割り得るので、max で順序を保証する。
        const ImVec2 viewMin = ed::ScreenToCanvas(canvasMin);
        const ImVec2 viewMax = ed::ScreenToCanvas(canvasMax);
        const float loX = viewMin.x + 16.0f;
        const float loY = viewMin.y + 16.0f;
        addNodePosition.x = std::clamp(addNodePosition.x, loX, std::max(loX, viewMax.x - 240.0f));
        addNodePosition.y = std::clamp(addNodePosition.y, loY, std::max(loY, viewMax.y - 120.0f));
        ed::Suspend();
        ImGui::OpenPopup("addGraphNode");
        ed::Resume();
    }
    ed::Suspend();
    if (ImGui::BeginPopup("addGraphNode")) {
        ImGui::TextDisabled("ノードを追加");
        ImGui::Separator();
        const auto addNodeMenuItem = [&](graph::NodeKind kind, const char* label) {
            if (!ImGui::MenuItem(label)) {
                return;
            }
            const graph::GraphId nodeId = m_graph.CreateNode(kind);
            graph::Node* node = m_graph.FindMutableNode(nodeId);
            if (node == nullptr) {
                TG_LOG_WARN("ノードを追加できませんでした（種類の定義が見つかりません）");
                return;
            }
            if (auto* settings = std::get_if<graph::LayerNodeSettings>(&node->settings)) {
                // 追加時の初期値は旧レイヤーパネルと同じ既定値を使う。
                settings->layer = kDefaultLayer;
                settings->layer.name +=
                    " " + std::to_string(m_graph.Nodes().size());
            }
            // Model はモデルプレビューで選んでいるモデルを最初から入れておく。
            if (auto* model = std::get_if<graph::ModelNodeSettings>(&node->settings); model && FindModel(m_selectedModel))
                model->model = m_selectedModel;
            node->posX = addNodePosition.x;
            node->posY = addNodePosition.y;
            node->positionValid = true;
            m_graphNodesToPlace.push_back(nodeId);
            m_selectedGraphNode = nodeId;
            // 作った直後は、その結果を見たいはず。プレビューも移す。
            SetPreviewGraphNode(nodeId);
            MarkDocumentChanged();
            // ステータスバーに残す。追加が効いたかを画面で確かめられるようにする。
            TG_LOG_INFO("ノードを追加しました: %s", NodeDisplayName(*node));
        };
        // 道路系（Mesh を受け渡す）とモデル系（Model を受け渡す）を分けて並べる。互いには繋がらない。
        ImGui::TextDisabled("道路");
        addNodeMenuItem(graph::NodeKind::Road, "Road — Pathから道路面と左右境界を生成");
        addNodeMenuItem(graph::NodeKind::RoadMarking, "Lane Marking — 道路面に白線の帯を生成");
        addNodeMenuItem(graph::NodeKind::RoadMask, "Road Mask — 轍・端・ムラの道路空間マスク");
        addNodeMenuItem(graph::NodeKind::Decal, "Decal — 面上のPathに沿って模様の帯を貼る");
        addNodeMenuItem(graph::NodeKind::Shoulder, "Shoulder — 道路の境界から外側へ路肩を張る");
        addNodeMenuItem(graph::NodeKind::Crack, "Crack — ひび割れの塊を乱数で配置する");
        ImGui::Separator();
        ImGui::TextDisabled("モデル");
        addNodeMenuItem(graph::NodeKind::Model, "Model — 3D モデル（.tgmodel）を 1 つ置く");
        addNodeMenuItem(graph::NodeKind::Transform, "Transform — 上流のモデルをまとめて移動・回転・拡大");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Merge, "Merge — 道路メッシュとモデルをまとめる（モデルだけなら Transform へ繋げる）");
        addNodeMenuItem(graph::NodeKind::MeshOutput, "Mesh Output — 道路メッシュとモデルを表示");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Path, "Path — 実寸の3次元カーブを編集");
        addNodeMenuItem(graph::NodeKind::Surface, "Surface — マテリアルを Road / Shoulder のスロットへ渡す");
        // 旧地形ノード（Heightmap / Shape / Liquid / 侵食系 / Mask 系 / Output）はメニューから外した。
        // 旧ファイルの読込のためにノードの種類は残っている。
        ImGui::EndPopup();
    }
    ed::Resume();

    // --- プレビュー対象の切り替え -------------------------------------------
    // **選択とは別。** ノードを選んでプロパティをいじりながら、別のメッシュノードの
    // 出力をビューポートに出しておけるようにする。
    //
    // 出力ピンは押した瞬間からリンクのドラッグが始まるので、
    // **同じピンの上でほとんど動かずに離したとき**だけクリックとみなす。
    {
        const graph::GraphId hoveredPin = ToGraphId(ed::GetHoveredPin().Get());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_graphPressedPin = hoveredPin;
            m_graphPressedPinPos = ImGui::GetMousePos();
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 released = ImGui::GetMousePos();
            const float moved = std::abs(released.x - m_graphPressedPinPos.x) +
                                std::abs(released.y - m_graphPressedPinPos.y);
            if (m_graphPressedPin != 0 && hoveredPin == m_graphPressedPin && moved < 6.0f) {
                if (const graph::Pin* pin = m_graph.FindPin(m_graphPressedPin);
                    pin != nullptr && pin->kind == graph::PinKind::Output) {
                    // 押したピンそのもののノードを見る（メッシュノードだけ）。
                    SetPreviewGraphNode(pin->nodeId, pin->id);
                }
            }
            m_graphPressedPin = 0;
        }
        // ノードのダブルクリックでも切り替える（ピンが小さいときの逃げ道）。
        if (const ed::NodeId doubleClicked = ed::GetDoubleClickedNode()) {
            SetPreviewGraphNode(ToGraphId(doubleClicked.Get()));
        }
        // 背景のダブルクリックで Mesh Output の鎖へ戻す。
        if (ed::IsBackgroundDoubleClicked()) {
            SetPreviewGraphNode(0);
        }
    }

    // --- 選択 ---------------------------------------------------------------
    // 選択はプロパティに出すノード。外したら 0 に戻す（プレビューには影響しない）。
    // **配置待ちのノードがある間は消さない。** 追加した直後のフレームは
    // エディタ側の選択がまだ無く、ここで 0 に戻すと「追加 → 選択」が消える
    // （エディタへの選択の反映は次のフレームの流し込みで行う）。
    // コピーは複数選択（枠で囲む）にも効かせたいので、全部控えておく。
    // ビューポートでモデルを選んだ・外したときは、エディタの選択もそれに合わせる。
    if (m_graphSelectionRequest) {
        ed::ClearSelection();
        if (*m_graphSelectionRequest != 0) ed::SelectNode(ed::NodeId(*m_graphSelectionRequest));
        m_graphSelectionRequest.reset();
    }
    ed::NodeId selectedNodes[64];
    const int selectedCount = ed::GetSelectedNodes(selectedNodes, IM_ARRAYSIZE(selectedNodes));
    if (selectedCount > 0) {
        m_selectedGraphNodes.clear();
        for (int i = 0; i < selectedCount; ++i) {
            m_selectedGraphNodes.push_back(ToGraphId(selectedNodes[i].Get()));
        }
        m_selectedGraphNode = m_selectedGraphNodes.front();
    } else if (m_graphNodesToPlace.empty()) {
        m_selectedGraphNodes.clear();
        m_selectedGraphNode = 0;
    }

    ed::End();

    // エディタが持つ位置をノードへ書き戻す（保存はここから読む）。
    // **このフレーム中に作られたばかりでエディタが知らないノードは飛ばす。**
    // エディタは知らないノードに FLT_MAX を返すため、書き戻すと次の流し込みで
    // ノードが無限遠へ飛び、キャンバスが操作不能になる（実際に踏んだ）。
    for (graph::Node& node : m_graph.MutableNodes()) {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.id));
        if (!IsValidNodePosition(position.x, position.y)) {
            continue;
        }
        node.posX = position.x;
        node.posY = position.y;
        node.positionValid = true;
    }
    ed::PopStyleColor(2);
    ed::SetCurrentEditor(nullptr);
}

void Application::DrawGraphPanel() {
    // 既定レイアウトを組んだ直後は、右カラムの前面タブをこのパネルにする。
    if (m_focusDefaultTabs > 0) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("グラフ")) {
        ImGui::End();
        return;
    }

    if (m_renderer.HasMeshScene()) {
        if (ui::BeginPropertyTable("meshSceneRows")) {
            ui::PropertyValue("メッシュ数", "%zu", static_cast<size_t>(std::count_if(m_renderer.Scene().meshes.begin(),
                m_renderer.Scene().meshes.end(), [](const auto& mesh) { return !mesh.materialOnly; })));
            ui::EndPropertyTable();
        }
    } else {
        ui::HintText("Path → Road → Mesh Output と繋ぐと道路がビューポートに出る");
    }

    float editorHeight = ui::Scaled(m_graphEditorHeight);
    const float paneWidth = ImGui::GetContentRegionAvail().x;
    const float maxHeight =
        std::max(ui::Scaled(160.0f), ImGui::GetContentRegionAvail().y - ui::Scaled(120.0f));
    ImGui::BeginChild("graphEditorPane", ImVec2(0.0f, editorHeight));
    DrawGraphEditor();
    ImGui::EndChild();

    ui::HorizontalSplitter("graphSplitter", &editorHeight, ui::Scaled(160.0f), maxHeight,
                           paneWidth);
    m_graphEditorHeight = editorHeight / std::max(ui::Scaled(1.0f), 0.01f);

    ImGui::BeginChild("graphPropertyPane", ImVec2(0.0f, 0.0f));

    // **プレビュー対象は選択とは別。** どれが画面に出ているかをここに出し、
    // Mesh Output へ戻す手段も置く（出力ピンのクリックで切り替わる、と気づけるように）。
    if (m_meshGraphActive) {
        // 途中のメッシュノードを見ているときは、そのノード名を出して Mesh Output へ戻す手段を置く。
        const graph::Node* previewMeshNode = m_graph.FindNode(m_meshGraphPreviewNode);
        if (ui::BeginPropertyTable("meshGraphPreviewRow")) {
            ui::PropertyValue("プレビュー", "%s",
                              previewMeshNode != nullptr ? NodeDisplayName(*previewMeshNode) : "Mesh Output");
            ui::EndPropertyTable();
        }
        if (previewMeshNode != nullptr) {
            ui::HintText("このノードまでの道路メッシュを表示中。出力ピンのクリックで切り替わる。");
            if (ui::Button("Mesh Output へ戻す", ui::kWideButtonWidth)) {
                SetPreviewGraphNode(0);
            }
        } else {
            ui::HintText("Mesh Outputへ接続した道路を表示中。Pathを選択するとカーブを編集できます。");
        }
    } else {
        ui::HintText("メッシュノードの出力ピンをクリック（またはノードをダブルクリック）で、"
                     "そのノードまでの道路をビューポートに出す");
        ImGui::Spacing();
    }

    if (!m_meshGraphError.empty()) ui::HintText("%s", m_meshGraphError.c_str());
    graph::Node* selected = m_graph.FindMutableNode(m_selectedGraphNode);
    if (selected == nullptr) {
        ui::HintText("ノードを選ぶと設定が出る。背景の右クリックで追加、"
                     "ピンをドラッグして接続、Ctrl+C / Ctrl+V でコピー");
    } else if (selected->kind == graph::NodeKind::Model || selected->kind == graph::NodeKind::Transform) {
        // 置き方の変更は道路を作り直さない（MarkDirty しない）。描画は毎フレーム設定から行う。
        if (DrawModelNodeSettings(*selected)) m_documentDirty = true;
    } else if (auto* road = std::get_if<graph::RoadNodeSettings>(&selected->settings)) {
        if (DrawSurfaceLayoutSettings(selected->id)) { m_graph.MarkDirty(); MarkDocumentChanged(); }
        const auto* activeBand = graph::FindRoadBand(m_surfaceLayouts, selected->id);
        const bool hasLayout = activeBand && !activeBand->spans.empty();
        ui::SectionHeader("道路の形状");
        bool changed = false;
        const graph::RoadNodeSettings defaults;
        if (ui::BeginPropertyTable("roadRows")) {
            changed |= ui::PropertyFloat("道路幅", &road->widthMeters, 0.1f, 50.0f,
                defaults.widthMeters, "中心線から左右へ半分ずつ広げる全幅", "%.2f m");
            {
                int forward = static_cast<int>(road->lanesForward);
                int backward = static_cast<int>(road->lanesBackward);
                if (ui::PropertyInt("車線数（進行方向）", &forward, 1, 8, static_cast<int>(defaults.lanesForward),
                                    "線形の向きへ進む車線の数。どちら側に並ぶかは走行側で決まる")) {
                    road->lanesForward = static_cast<uint32_t>(forward);
                    changed = true;
                }
                if (ui::PropertyInt("車線数（対向）", &backward, 0, 8, static_cast<int>(defaults.lanesBackward),
                                    "対向車線の数。0 で一方通行（中央線は出ない）")) {
                    road->lanesBackward = static_cast<uint32_t>(backward);
                    changed = true;
                }
                ui::PropertyValue("車線幅", "%.2f m",
                                  road->widthMeters / static_cast<float>(std::max(1u, road->lanesForward) + road->lanesBackward));
            }
            if (!hasLayout) changed |= ui::PropertyFloat("UV反復長", &road->uvRepeatMeters, 0.1f, 100.0f,
                defaults.uvRepeatMeters, "UVが1増える実距離。道路の長さと幅の両方に適用する", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = road->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を道路の長さ方向に沿わせるか。横長の素材は U")) {
                    road->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            if (!hasLayout) changed |= ui::PropertyFloat("変位量", &road->displacementMeters, 0.0f, 1.0f,
                defaults.displacementMeters,
                "Materialのハイトで路面を法線方向へ押し出す量。ハイト0〜1の全幅がこの高さ（m）。"
                "0なら形は変わらない。テセレーションはプレビュー設定の「道路」で", "%.3f m", 0, 0.005f);
            ui::PropertyValue("走行側", "%s", m_graph.RoadNetwork().leftHandTraffic ? "左側通行" : "右側通行");
            ui::EndPropertyTable();
        }
        // 材質スロット。1 は下地、2〜4 は Mask 2〜4 で被覆する。座標と反復長はスロットごと。
        if (!hasLayout) changed |= DrawMaterialSlotRows(*selected, road->layerWorldUv, road->layerUvRepeatMeters,
                                        road->layerBlendRange, defaults.layerBlendRange,
                                        road->layerHeightGate, road->layerHeightGateThreshold, road->layerHeightGateSoftness,
                                        road->layerBlendMode);
        if (!hasLayout) ui::HintText("Material にSurfaceなどのResultを接続してマテリアルを適用。Material 2〜4 は Road Mask を Mask 2〜4 へ繋いだ所に出る。"
                     "RoadSurfaceはMesh Outputへ、Left / Rightは進行方向に向かって左右の境界Path。走行側はプレビュー設定の「道路」で切り替える。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* decal = std::get_if<graph::DecalNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::DecalNodeSettings defaults;
        if (ui::BeginPropertyTable("decalRows")) {
            changed |= DrawMeshMaterialSlotRow("マテリアル", decal->material, m_materialLibrary);
            changed |= ui::PropertyFloat("凹凸量", &decal->heightMeters, 0.0f, 1.0f, defaults.heightMeters,
                                         "マテリアルのHeightを路面の凹凸に加算。黒は0、白は指定の高さ。細かな凹凸にはプレビュー設定のテセレーションを使う", "%.3f m");
            changed |= ui::PropertyBool("帯ワイヤー", &decal->showWireframe, defaults.showWireframe, "この帯の分割前メッシュを重ねて表示する");
            changed |= ui::PropertyFloat("画像幅倍率", &decal->imageWidthScale, 0.01f, 100.0f, defaults.imageWidthScale,
                                         "帯の中心を基準に画像を幅方向へ拡大縮小。2で画像が2倍の大きさ。帯幅は変えない", "%.2f 倍");
            changed |= ui::PropertyFloat("画像長さ倍率", &decal->imageLengthScale, 0.01f, 100.0f, defaults.imageLengthScale,
                                         "帯の始点を基準に画像を長さ方向へ拡大縮小。UV反復長に掛ける倍率", "%.2f 倍");
            changed |= ui::PropertyFloat("幅", &decal->widthMeters, 0.05f, 50.0f, defaults.widthMeters, "帯の幅", "%.2f m");
            changed |= ui::PropertyFloat("浮かせ量", &decal->liftMeters, 0.0f, 0.1f, defaults.liftMeters,
                                         "路面から法線方向へ持ち上げる量", "%.3f m");
            changed |= ui::PropertyFloat("UV反復長", &decal->uvRepeatMeters, 0.05f, 100.0f, defaults.uvRepeatMeters,
                                         "画像倍率1のときの長さ方向の反復距離。幅方向は帯幅に画像1枚が収まる", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = decal->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を帯の長さ方向に沿わせるか")) {
                    decal->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            ui::EndPropertyTable();
        }
        ui::HintText("RoadのRoadSurfaceと、Surfaceにその道路を繋いだPathを接続する。Pathは路面の上でCtrl＋クリックして引く。"
                     "選択したマテリアルの不透明度で模様をくり抜き、出力のRoadSurfaceをLane MarkingかMesh Outputへ。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* shoulder = std::get_if<graph::ShoulderNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::ShoulderNodeSettings defaults;
        if (ui::BeginPropertyTable("shoulderRows")) {
            changed |= ui::PropertyFloat("幅", &shoulder->widthMeters, 0.1f, 50.0f, defaults.widthMeters,
                                         "境界から外側へ張る幅", "%.2f m");
            changed |= ui::PropertyFloat("横断勾配", &shoulder->crossSlopePercent, -50.0f, 50.0f, defaults.crossSlopePercent,
                                         "外側へ向かって下がる割合。1 m 進んで何 cm 下がるか", "%.1f %%");
            changed |= ui::PropertyFloat("段差", &shoulder->stepHeightMeters, 0.0f, 0.5f, defaults.stepHeightMeters,
                                         "舗装端の段差。0 より大きいと境界の直後に面取り列を挟み、路肩全体をこの高さだけ下げる",
                                         "%.3f m", 0, 0.005f);
            if (shoulder->stepHeightMeters > 0.0f) {
                changed |= ui::PropertyFloat("面取り幅", &shoulder->stepWidthMeters, 0.005f, 1.0f, defaults.stepWidthMeters,
                                             "境界から段差の底までの横幅。小さいほど垂直に近い", "%.3f m", 0, 0.005f);
            }
            changed |= ui::PropertyFloat("UV反復長", &shoulder->uvRepeatMeters, 0.1f, 100.0f, defaults.uvRepeatMeters,
                                         "UV が 1 増える実距離", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = shoulder->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を路肩の長さ方向に沿わせるか")) {
                    shoulder->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("変位量", &shoulder->displacementMeters, 0.0f, 1.0f, defaults.displacementMeters,
                                         "Materialのハイトで路肩を法線方向へ押し出す量。ハイト0〜1の全幅がこの高さ（m）。"
                                         "境界で道路と同じマテリアル・同じ量にすると段が出ない", "%.3f m", 0, 0.005f);
            ui::EndPropertyTable();
        }
        changed |= DrawMaterialSlotRows(*selected, shoulder->layerWorldUv, shoulder->layerUvRepeatMeters,
                                        shoulder->layerBlendRange, defaults.layerBlendRange,
                                        shoulder->layerHeightGate, shoulder->layerHeightGateThreshold, shoulder->layerHeightGateSoftness,
                                        shoulder->layerBlendMode);
        ui::HintText("PathにRoadのLeft / Right（または別のShoulderのOuter）を接続する。境界の頂点を共有するので道路と水密。"
                     "マテリアルスロットとMask 2〜4はRoadと同じ。Road Maskの「側」は路肩では 右＝境界側、左＝外側。"
                     "出力のRoadSurfaceをMesh Outputへ、Outerは次の路肩や縁石へ。走行側には依存しない。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* crack = std::get_if<graph::CrackNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::CrackNodeSettings defaults;
        if (ui::BeginPropertyTable("crackRows")) {
            changed |= DrawMeshMaterialSlotRow("マテリアル", crack->material, m_materialLibrary);
            {
                int seed = static_cast<int>(crack->seed);
                if (ui::PropertyInt("シード", &seed, 0, 99999, static_cast<int>(defaults.seed), "変えると配置と形が変わる")) {
                    crack->seed = static_cast<uint32_t>(std::max(0, seed));
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("密度", &crack->densityPer100m, 0.0f, 200.0f, defaults.densityPer100m,
                                         "100 m あたりの塊の数の目安。別の塊と重ならない場所を探し、収まらない場合は数を減らす", "%.1f /100m");
            changed |= ui::PropertyFloat("長さ（最小）", &crack->lengthMinMeters, 0.5f, 30.0f, defaults.lengthMinMeters,
                                         "幹の長さの下限", "%.1f m");
            changed |= ui::PropertyFloat("長さ（最大）", &crack->lengthMaxMeters, 0.5f, 30.0f, defaults.lengthMaxMeters,
                                         "幹の長さの上限。横向きは車線幅が上限", "%.1f m");
            if (crack->lengthMaxMeters < crack->lengthMinMeters) { crack->lengthMaxMeters = crack->lengthMinMeters; changed = true; }
            {
                static const char* const kOrientationLabels[] = {"縦（長さ方向）", "横（車線を横切る）", "混合"};
                int orientation = static_cast<int>(crack->orientation);
                if (ui::PropertyCombo("向き", &orientation, kOrientationLabels, IM_ARRAYSIZE(kOrientationLabels), 2,
                                      "幹の向き。混合は割合で混ぜる")) {
                    crack->orientation = static_cast<graph::CrackOrientation>(orientation);
                    changed = true;
                }
                if (crack->orientation == graph::CrackOrientation::Mixed) {
                    changed |= ui::PropertyFloat("横の割合", &crack->transverseRatio, 0.0f, 1.0f, defaults.transverseRatio,
                                                 "混合のときに横向きになる割合", "%.2f");
                }
            }
            changed |= ui::PropertyFloat("折れの強さ", &crack->angleJitterDegrees, 0.0f, 90.0f, defaults.angleJitterDegrees,
                                         "幹の左右への折れの強さ。0で直線。折れの間隔は自動で決まり、枝は折れ点の外側から伸びる", "%.0f°");
            {
                static const char* const kPlacementLabels[] = {"一様", "轍寄り", "端寄り"};
                int placement = static_cast<int>(crack->placement);
                if (ui::PropertyCombo("横位置", &placement, kPlacementLabels, IM_ARRAYSIZE(kPlacementLabels), 0,
                                      "塊の横位置の分布。轍寄りは Road の車線から決める")) {
                    crack->placement = static_cast<graph::CrackPlacement>(placement);
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("幹の幅", &crack->trunkWidthMeters, 0.01f, 1.0f, defaults.trunkWidthMeters,
                                         "幹の帯の幅。素材のアルファで割れ目の細さが決まるので、帯は少し広め", "%.3f m", 0, 0.005f);
            {
                int lo = static_cast<int>(crack->branchesMin);
                int hi = static_cast<int>(crack->branchesMax);
                if (ui::PropertyInt("枝の数（最小）", &lo, 0, 12, static_cast<int>(defaults.branchesMin), "幹から分かれる枝の本数の下限")) {
                    crack->branchesMin = static_cast<uint32_t>(lo); changed = true;
                }
                if (ui::PropertyInt("枝の数（最大）", &hi, 0, 12, static_cast<int>(defaults.branchesMax), "枝の本数の上限。半分の枝がさらに 1 本の子枝を出す")) {
                    crack->branchesMax = static_cast<uint32_t>(hi); changed = true;
                }
                if (crack->branchesMax < crack->branchesMin) { crack->branchesMax = crack->branchesMin; changed = true; }
            }
            changed |= ui::PropertyFloat("枝の長さ", &crack->branchLengthRatio, 0.05f, 2.0f, defaults.branchLengthRatio,
                                         "幹の長さに対する枝の長さの比", "%.2f");
            changed |= ui::PropertyFloat("枝の幅", &crack->branchWidthRatio, 0.05f, 1.0f, defaults.branchWidthRatio,
                                         "幹の幅に対する枝の根元の幅の比。先端で 0 へ絞る", "%.2f");
            changed |= ui::PropertyFloat("浮かせ量", &crack->liftMeters, 0.0f, 0.1f, defaults.liftMeters,
                                         "路面から法線方向へ持ち上げる量", "%.3f m");
            changed |= ui::PropertyFloat("UV反復長", &crack->uvRepeatMeters, 0.05f, 100.0f, defaults.uvRepeatMeters,
                                         "帯の長さ方向で UV が 1 増える実距離。幅方向は 0〜1", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = crack->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を帯の長さ方向に沿わせるか。1024×128 のような横長素材は U")) {
                    crack->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            ui::EndPropertyTable();
        }
        ui::HintText("RoadのRoadSurfaceを接続すると、3〜6 m の枝分かれしたひび割れを乱数で置く。プロパティで割れ目のマテリアル（マスク抜き）を選ぶ。"
                     "個別に置きたいものは面上のPath＋Decalで描く。出力のRoadSurfaceをMergeかMesh Outputへ。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (selected->kind == graph::NodeKind::Merge) {
        if (ui::BeginPropertyTable("mergeRows")) {
            ui::PropertyValue("入力", "%zu 本（空き 1）", selected->inputs.size());
            ui::EndPropertyTable();
        }
        ui::HintText("Road・Shoulder・Decal などのRoadSurfaceを繋ぐと、まとめて1つのRoadSurfaceにする。"
                     "繋ぐたびに入力が1本増える。同じノード由来のメッシュは1回だけ積む。下流の白線・Decalは Mesh 1 の面に乗る。");
    } else if (auto* roadMask = std::get_if<graph::RoadMaskNodeSettings>(&selected->settings)) {
        bool changed = false;
        if (ui::BeginPropertyTable("roadMaskRows")) {
            changed |= DrawRoadMaskPropertyRows(*roadMask);
            ui::EndPropertyTable();
        }
        ui::HintText("Mask を Road の Mask 2〜4 へ繋ぐと、対応する Material 2〜4 の被覆率になる。横位置と実距離で決まり、タイルは繰り返さない。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* marking = std::get_if<graph::RoadMarkingNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::RoadMarkingNodeSettings defaults;
        if (ui::BeginPropertyTable("roadMarkingRows", 130.0f)) {
            changed |= ui::PropertyBool("中央線", &marking->centerLine, defaults.centerLine,
                "進行方向と対向の境に1本引く。Road の車線数で位置が決まる。一方通行なら出ない");
            if (marking->centerLine) {
                changed |= DrawMeshMaterialSlotRow("中央線のマテリアル", marking->materials[0], m_materialLibrary);
                changed |= ui::PropertyFloat("中央線の幅", &marking->centerLineWidthMeters, 0.05f, 1.0f,
                    defaults.centerLineWidthMeters, "中央線の帯の幅。実線・破線の両方に適用", "%.2f m");
                static const char* const labels[] = {"実線", "破線"};
                int style = marking->centerLineDashed ? 1 : 0;
                if (ui::PropertyCombo("中央線の種類", &style, labels, IM_ARRAYSIZE(labels), 0,
                    "中央線を実線または破線にする。破線の長さ・間隔は車線境界線と共通")) {
                    marking->centerLineDashed = style == 1;
                    changed = true;
                }
            }
            changed |= ui::PropertyBool("外側線", &marking->edgeLines, defaults.edgeLines,
                "左右の道路端の手前に1本ずつ引く");
            if (marking->edgeLines) {
                changed |= DrawMeshMaterialSlotRow("外側線のマテリアル", marking->materials[1], m_materialLibrary);
                changed |= ui::PropertyFloat("外側線の幅", &marking->edgeLineWidthMeters, 0.05f, 1.0f,
                    defaults.edgeLineWidthMeters, "左右の外側線に共通する帯の幅", "%.2f m");
            }
            changed |= ui::PropertyBool("車線境界線", &marking->laneLines, defaults.laneLines,
                "同方向の車線の間に破線で引く。Road の車線数が片側 2 以上のときに出る");
            if (marking->laneLines) {
                changed |= DrawMeshMaterialSlotRow("車線境界線のマテリアル", marking->materials[2], m_materialLibrary);
                changed |= ui::PropertyFloat("車線境界線の幅", &marking->laneLineWidthMeters, 0.05f, 1.0f,
                    defaults.laneLineWidthMeters, "同方向の車線を分ける帯の幅", "%.2f m");
            }
            if (marking->laneLines || (marking->centerLine && marking->centerLineDashed)) {
                changed |= ui::PropertyFloat("破線の長さ", &marking->dashLengthMeters, 0.1f, 50.0f,
                    defaults.dashLengthMeters, "破線 1 本の長さ", "%.1f m");
                changed |= ui::PropertyFloat("破線の間隔", &marking->dashGapMeters, 0.0f, 50.0f,
                    defaults.dashGapMeters, "破線と破線の間の空き。0 で実線", "%.1f m");
            }
            changed |= ui::PropertyBool("停止線", &marking->stopLines, defaults.stopLines,
                "Path の点に付けた停止線を、その向きの車線の幅いっぱいに引く");
            if (marking->stopLines) {
                changed |= DrawMeshMaterialSlotRow("停止線のマテリアル", marking->materials[3], m_materialLibrary);
                changed |= ui::PropertyFloat("停止線の幅", &marking->stopLineWidthMeters, 0.1f, 2.0f,
                    defaults.stopLineWidthMeters, "停止線の道路の長さ方向の幅", "%.2f m");
            }
            changed |= ui::PropertyFloat("端からの距離", &marking->edgeInsetMeters, 0.0f, 5.0f,
                defaults.edgeInsetMeters, "道路端から外側線の中心までの距離", "%.2f m");
            changed |= ui::PropertyFloat("浮かせ量", &marking->liftMeters, 0.0f, 0.1f,
                defaults.liftMeters, "路面から法線方向へ持ち上げる量。0だと路面とちらつく", "%.3f m");
            changed |= ui::PropertyFloat("UV反復長", &marking->uvRepeatMeters, 0.1f, 100.0f,
                defaults.uvRepeatMeters, "帯の長さ方向でUVが1増える実距離。幅方向は0〜1", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = marking->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を帯の長さ方向に沿わせるか。2048×256 のような横長の白線素材は U")) {
                    marking->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            changed |= ui::PropertyBool("進行方向の矢印", &marking->arrows, defaults.arrows,
                "各車線の中央に矢印を置く。進行方向の車線は線形の向き、対向車線は逆向き");
            if (marking->arrows) {
                changed |= DrawMeshMaterialSlotRow("矢印のマテリアル", marking->materials[4], m_materialLibrary);
                changed |= ui::PropertyFloat("矢印の間隔", &marking->arrowIntervalMeters, 1.0f, 200.0f,
                    defaults.arrowIntervalMeters, "矢印を置く間隔", "%.0f m");
                changed |= ui::PropertyFloat("矢印の長さ", &marking->arrowLengthMeters, 0.5f, 20.0f,
                    defaults.arrowLengthMeters, "矢印の全長。幅は道路幅から決める", "%.1f m");
            }
            ui::PropertyValue("走行側", "%s", m_graph.RoadNetwork().leftHandTraffic ? "左側通行" : "右側通行");
            ui::EndPropertyTable();
        }
        ui::HintText("RoadのRoadSurfaceを接続し、出力のRoadSurfaceをMesh Outputへ。各線のマテリアルはプロパティで選択する。「なし」は白。走行側はプレビュー設定の「道路」で切り替える。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (selected->kind == graph::NodeKind::MeshOutput) {
        ui::HintText("RoadSurfaceを接続すると道路を表示する。複数のMesh Outputを同時に表示できる。");
    } else if (auto* settings = std::get_if<graph::LayerNodeSettings>(&selected->settings)) {
        bool changed = false;
        if (ui::BeginPropertyTable("graphNodeBasicRows")) {
            changed |= ui::PropertyBool("有効", &settings->layer.enabled, true,
                                        "無効にすると合成から外れる");
            ui::EndPropertyTable();
        }
        changed |= DrawLayerSettings(settings->layer);
        if (changed) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (std::get_if<graph::PathNodeSettings>(&selected->settings) != nullptr) {
        if (DrawPathSettings(*selected)) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else {
        ui::HintText("このノードに設定は無い");
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
