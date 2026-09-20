#include "core/Log.h"
#include <chrono>
// ノードグラフパネル。imgui-node-editor によるエディタと、
// 選択中ノードのプロパティ（レイヤーパネルと共有）を持つ。
//
// エディタの作法（カード描画・丸ピン・ドット背景・リンクの作成 / 削除）は
// ノードエディタ UI から移植した。

#include "app/Application.h"
#include "graph/RockEvaluator.h"
#include "renderer/RockMesh.h"

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

namespace rock {
namespace {

ImU32 ColorToU32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

// 種類ごとのアクセント色。グレー基調を崩さないよう彩度は低め。
ImVec4 NodeAccentColor(graph::NodeKind kind) {
    switch (kind) {
        case graph::NodeKind::Surface:
            return ImVec4(0.55f, 0.66f, 0.58f, 1.0f);
        case graph::NodeKind::Merge:
            return ImVec4(0.62f, 0.70f, 0.66f, 1.0f);
        case graph::NodeKind::Model:
        case graph::NodeKind::Transform:
            return ImVec4(0.62f, 0.60f, 0.78f, 1.0f);
        default:
            return ImVec4(0.59f, 0.64f, 0.68f, 1.0f);
    }
}

// ピンとリンクの色。**線が何を運んでいるかを色で見分ける。**
ImVec4 PinTypeColor(graph::ValueType valueType) {
    switch (valueType) {
        // メッシュは緑。
        case graph::ValueType::Mesh:
            return ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
        // モデルは藤色。メッシュ（Mesh）とは繋がらないことを色でも分ける。
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
    // Mesh Output と、プレビューできない種類（Surface）は「Mesh Output の鎖」に落とす。
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
    // メッシュを作るノードを見ているときは、そのノードまでの鎖を出す。
    graph::GraphId previewMeshNode = 0;
    if (const graph::Node* node = m_graph.FindNode(m_previewGraphNode);
        node != nullptr && (graph::IsMeshNodeKind(node->kind) || graph::IsModelNodeKind(node->kind))) {
        previewMeshNode = node->id;
    }
    if (m_meshGraphRevision == m_graph.Revision() && m_meshGraphPreviewNode == previewMeshNode) return;
    const auto evaluated = graph::EvaluateRocks(m_graph, previewMeshNode);
    renderer::MeshScene scene;
    for (const auto& rock : evaluated.rocks) {
        renderer::SceneMesh mesh;
        mesh.geometry = renderer::MakeRockMeshData(rock.mesh);
        mesh.material.baseColor = {0.35f, 0.32f, 0.28f};
        mesh.material.roughness = 0.8f;
        scene.meshes.push_back(std::move(mesh));
    }
    m_meshGraphError = evaluated.error;
    m_cutReports = evaluated.cuts;
    m_meshHighlight = MeshHighlightState{};
    if (scene.meshes.empty()) {
        if (m_meshGraphActive) m_renderer.ClearMeshScene(m_device);
        m_meshGraphActive = false;
    } else {
        m_meshGraphActive = m_renderer.SetGeneratedMeshScene(m_device, scene);
        if (!m_meshGraphActive) {
            m_renderer.ClearMeshScene(m_device);
            m_meshGraphError = "岩メッシュを描画へ転送できませんでした";
        }
    }
    std::vector<renderer::OverlayLineSet> guides;
    if (m_meshGraphActive && m_meshGraphError.empty()) {
        for (const auto& crack : evaluated.cracks) {
            auto generated = renderer::MakeCrackGuides(crack.patch);
            for (auto& guide : generated) guides.push_back(std::move(guide));
        }
    }
    if (m_meshGraphActive && m_meshGraphError.empty()) {
        for (const auto& report : evaluated.cuts)
            if (report.bridge && report.showBridge) {
                auto generated = renderer::MakeBridgeGuides(*report.bridge);
                for (auto& guide : generated) guides.push_back(std::move(guide));
            }
    }
    m_renderer.SetCrackGuides(std::move(guides));
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
    ROCK_LOG_INFO("ノードをコピーしました: %zu 個", m_graphClipboard.size());
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
    ROCK_LOG_INFO("ノードを貼り付けました: %zu 個", m_graphClipboard.size());
}

bool Application::IsGraphPinVisible(const graph::Pin& /*pin*/) const {
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
                ROCK_LOG_WARN("ノードを追加できませんでした（種類の定義が見つかりません）");
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
            ROCK_LOG_INFO("ノードを追加しました: %s", NodeDisplayName(*node));
        };
        // メッシュ系（Mesh を受け渡す）とモデル系（Model を受け渡す）を分けて並べる。
        ImGui::TextDisabled("モデル");
        addNodeMenuItem(graph::NodeKind::Crack, "Crack — 有限亀裂の表示と Box の部分切断");
        addNodeMenuItem(graph::NodeKind::BaseRock, "Base Rock — Box の母岩を生成");
        addNodeMenuItem(graph::NodeKind::Model, "Model — 3D モデル（.rockmodel）を 1 つ置く");
        addNodeMenuItem(graph::NodeKind::Transform, "Transform — 上流のモデルをまとめて移動・回転・拡大");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Merge, "Merge — メッシュとモデルをまとめる（モデルだけなら Transform へ繋げる）");
        addNodeMenuItem(graph::NodeKind::MeshOutput, "Mesh Output — メッシュとモデルを表示");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Surface, "Surface — マテリアルを Material スロットへ渡す");
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
        ui::HintText("Mesh Output へ繋いだものがビューポートに出る");
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
            ui::HintText("このノードまでのメッシュを表示中。出力ピンのクリックで切り替わる。");
            if (ui::Button("Mesh Output へ戻す", ui::kWideButtonWidth)) {
                SetPreviewGraphNode(0);
            }
        } else {
            ui::HintText("Mesh Output へ接続したメッシュを表示中。");
        }
    } else {
        ui::HintText("メッシュノードの出力ピンをクリック（またはノードをダブルクリック）で、"
                     "そのノードまでをビューポートに出す");
        ImGui::Spacing();
    }

    if (!m_meshGraphError.empty()) ui::HintText("%s", m_meshGraphError.c_str());
    graph::Node* selected = m_graph.FindMutableNode(m_selectedGraphNode);
    if (selected == nullptr) {
        ui::HintText("ノードを選ぶと設定が出る。背景の右クリックで追加、"
                     "ピンをドラッグして接続、Ctrl+C / Ctrl+V でコピー");
    } else if (auto* crack = std::get_if<crack::CrackSettings>(&selected->settings)) {
        auto edited = *crack;
        const float zero[3] = {0, 0, 0};
        bool changed = false;
        if (ui::BeginPropertyTable("crackRows")) {
            changed |= ui::PropertyBool("部分切断 (Box)", &edited.applyCut, false);
            changed |= ui::PropertyBool("ガイド表示", &edited.showGuide, true);
            changed |= ui::PropertyBool("Bridge 表示", &edited.showBridge, true);
            changed |= ui::PropertyFloat3Input("中心 (m)", edited.center.data(), zero) != 0;
            changed |= ui::PropertyFloat3Input("回転 (度)", edited.rotationDegrees.data(), zero) != 0;
            changed |= ui::PropertyFloat("半幅 U (m)", &edited.extentU, 0.001f, 1000, 1.2f);
            changed |= ui::PropertyFloat("半幅 V (m)", &edited.extentV, 0.001f, 1000, 1.2f);
            changed |= ui::PropertyFloat("Depth (m)", &edited.depth, 0, 2000, 1.6f);
            changed |= ui::PropertyFloat("Persistence", &edited.persistence, 0, 1, 0.6f);
            changed |= ui::PropertyFloat("Aperture (m)", &edited.aperture, 0, 100, 0.02f);
            crack::CrackPatch patch;
            std::string error;
            if (crack::BuildCrackPatch(edited, patch, error))
                ui::PropertyValue("到達深さ (m)", "%.3f", patch.effectiveDepth);
            ui::EndPropertyTable();
        }
        if (edited.applyCut) {
            ui::HintText(
                "軸に沿う単一の切り込みを Box に作ります。回転は90度単位。+V "
                "端を母岩の外面まで伸ばしてください。");
            const auto report = std::find_if(m_cutReports.begin(), m_cutReports.end(),
                                             [&](const auto& value) { return value.source == selected->id; });
            if (report != m_cutReports.end()) {
                ui::HintText("%s", report->status.c_str());
                if (report->bridge && ui::BeginPropertyTable("bridgeRows")) {
                    ui::PropertyValue("切込深さ (m)", "%.4f", report->penetration);
                    ui::PropertyValue("未破断厚 (m)", "%.4f", report->bridge->thickness);
                    ui::PropertyValue("未破断面積 (m²)", "%.4f", report->bridge->area);
                    ui::EndPropertyTable();
                }
            }
        } else
            ui::HintText("ガイド表示のみ。部分切断をオンにすると実際の切り込みを作ります。");
        ui::HintText(
            "青: 候補 / 橙: 到達範囲 / 緑: 未破断部。形状確認時はガイドと Bridge 表示をオフにできます。");
        ui::HintText("+V 端から -V へ、min(Depth, V 全幅) × Persistence だけ進みます。");
        if (changed) {
            *crack = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* rock = std::get_if<graph::BaseRockNodeSettings>(&selected->settings)) {
        bool changed = false;
        auto edited = *rock;
        if (ui::BeginPropertyTable("baseRockRows")) {
            ui::PropertyValue("Shape", "%s", "Box");
            changed |= ui::PropertyFloat("Size X (m)", &edited.size[0], 0.001f, 1000.0f, 2.0f);
            changed |= ui::PropertyFloat("Size Y (m)", &edited.size[1], 0.001f, 1000.0f, 2.0f);
            changed |= ui::PropertyFloat("Size Z (m)", &edited.size[2], 0.001f, 1000.0f, 2.0f);
            changed |= ui::PropertyInt("Seed", &edited.seed, 0, 1000000000, 0);
            ui::EndPropertyTable();
        }
        ui::HintText("原点中心の Box。Seed は保存されますが、Box の形状には影響しません。");
        if (changed) {
            *rock = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (selected->kind == graph::NodeKind::Model || selected->kind == graph::NodeKind::Transform) {
        // 置き方の変更はメッシュを作り直さない（MarkDirty しない）。描画は毎フレーム設定から行う。
        if (DrawModelNodeSettings(*selected)) m_documentDirty = true;
    } else if (selected->kind == graph::NodeKind::Merge) {
        if (ui::BeginPropertyTable("mergeRows")) {
            ui::PropertyValue("入力", "%zu 本（空き 1）", selected->inputs.size());
            ui::EndPropertyTable();
        }
        ui::HintText("メッシュやモデルを繋ぐと、まとめて 1 つにする。"
                     "繋ぐたびに入力が 1 本増える。同じノード由来のメッシュは 1 回だけ積む。");
    } else if (selected->kind == graph::NodeKind::MeshOutput) {
        ui::HintText("メッシュやモデルを接続すると表示する。複数の Mesh Output を同時に表示できる。");
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
    } else {
        ui::HintText("このノードに設定は無い");
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace rock
