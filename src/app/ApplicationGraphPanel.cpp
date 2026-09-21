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
        case graph::ValueType::Mask: return ImVec4(.65f,.65f,.65f,1);
        case graph::ValueType::Points: return ImVec4(.8f,.65f,.35f,1);
        case graph::ValueType::Pieces: return ImVec4(.75f,.5f,.32f,1);
        case graph::ValueType::Selection: return ImVec4(.85f,.75f,.25f,1);
        case graph::ValueType::Mesh:
            return ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
        // モデルは藤色。メッシュ（Mesh）とは繋がらないことを色でも分ける。
        case graph::ValueType::Model:
            return ImVec4(0.70f, 0.62f, 0.90f, 1.0f);
        // どちらも受ける入力（Merge / Mesh Output）と、何も繋がっていない Merge の出力は無彩色。
        case graph::ValueType::Boxes:
            return ImVec4(0.78f, 0.65f, 0.43f, 1.0f);
        case graph::ValueType::Volume:
            return ImVec4(0.42f, 0.70f, 0.82f, 1.0f);
        case graph::ValueType::Preview:
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

// UV Unwrap の段階名。RockEvaluationProgress::stage は geometry::UvUnwrapStage + 1。
const char* UvUnwrapStageName(int stage) {
    switch (stage) {
        case 1: return "メッシュを準備中";
        case 2: return "島へ分割中";
        case 3: return "島を配置中";
        case 4: return "結果を作成中";
        default: return "";
    }
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

graph::GraphId Application::EvaluatingNode() const {
    if (!m_pieceUpdating || !m_pieceProgress) return 0;
    return m_pieceProgress->node.load(std::memory_order_relaxed);
}

std::string Application::EvaluationProgressText() const {
    const graph::GraphId id = EvaluatingNode();
    const graph::Node* node = id ? m_graph.FindNode(id) : nullptr;
    if (node == nullptr) return {};
    std::string text = NodeDisplayName(*node);
    const int stage = m_pieceProgress->stage.load(std::memory_order_relaxed);
    const int percent = m_pieceProgress->percent.load(std::memory_order_relaxed);
    if (node->kind == graph::NodeKind::UvUnwrap && stage > 0) text += std::string(": ") + UvUnwrapStageName(stage);
    // xatlas の「島へ分割」は、メッシュ1つにつき 0% と 100% しか通知しない。0% のまま長く待つので、
    // 進み具合が分かる段階だけ百分率を出し、どの段階でも経過時間を添える。
    if (percent > 0) text += " " + std::to_string(percent) + "%";
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_pieceTaskStart).count();
    if (seconds >= 2) text += "・" + std::to_string(seconds) + "秒";
    return text;
}

void Application::DrawBakedTextureTiles(const graph::MaterialBakeSettings& bake) {
    const compositor::MaterialAsset* asset = m_materialLibrary.Find(bake.bakedLayer.material);
    if (asset == nullptr) return;
    // Roughness / Metallic / AO は1枚の画像の R / G / B へ詰めて焼いている。RGB のまま見ても読めないので、
    // テクスチャライブラリが持つ「1チャンネルだけを灰色で描く SRV」で分けて出す。
    struct Tile {
        const char* label;
        compositor::TextureId texture;
        int channel;  // -1 なら RGB のまま。0..3 は R / G / B / A。
    };
    const auto channelOf = [](compositor::TextureChannel c) { return static_cast<int>(c); };
    const Tile tiles[] = {
        {"Base Color", asset->baseColor, -1},
        {"Normal", asset->normal, -1},
        {"Roughness", asset->roughness.texture, channelOf(asset->roughness.channel)},
        {"Metallic", asset->metallic.texture, channelOf(asset->metallic.channel)},
        {"AO", asset->ambientOcclusion.texture, channelOf(asset->ambientOcclusion.channel)},
        {"Height", asset->height.texture, channelOf(asset->height.channel)},
    };
    ui::SectionHeader("ベイク結果");
    // 欄の幅に合わせて 3 列か 2 列に並べる。
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    // スクロールバーが出ても右端の1枚が隠れないよう、その幅を先に引いておく。
    const float available = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    const int columns = available >= ui::Scaled(330.0f) ? 3 : 2;
    const float size = std::max(ui::Scaled(48.0f), (available - spacing * float(columns - 1)) / float(columns));
    const auto& entries = m_textureLibrary.Entries();
    for (int i = 0; i < int(IM_ARRAYSIZE(tiles)); ++i) {
        const Tile& tile = tiles[i];
        if (i % columns != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(i);
        const compositor::LibraryTexture* texture = m_textureLibrary.Find(tile.texture);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        if (texture == nullptr || texture->missing) {
            ui::MissingThumbnail(min, ImVec2(min.x + size, min.y + size));
            ImGui::Dummy(ImVec2(size, size));
        } else {
            // UV の島が無い部分は、RGB の画像では透明、1チャンネルの表示では黒になる。同じ下地を敷いて揃える。
            ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + size, min.y + size), IM_COL32(0, 0, 0, 255));
            ImGui::Image(static_cast<ImTextureID>(texture->ChannelHandle(tile.channel).ptr), ImVec2(size, size));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s\nクリックで拡大", tile.label, texture->name.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    // テクスチャプレビューの窓で、同じ画像・同じチャンネルを開く。
                    for (size_t index = 0; index < entries.size(); ++index)
                        if (entries[index].id == tile.texture) m_selectedTexture = int(index);
                    m_previewChannel = tile.channel + 1;
                    m_showTexturePreview = true;
                }
            }
        }
        ImGui::TextDisabled("%s", tile.label);
        ImGui::PopID();
        ImGui::EndGroup();
    }
    if (const compositor::LibraryTexture* color = m_textureLibrary.Find(asset->baseColor); color && !color->missing) {
        if (color->transient)
            ui::HintText("%u × %u。一時的な結果です（未出力）。", color->texture.width, color->texture.height);
        else
            ui::HintText("%u × %u。保存先: %s", color->texture.width, color->texture.height,
                         ToUtf8Display(color->path.parent_path()).c_str());
    }
}

void Application::SyncMeshGraph() {
    if (m_uvLastSelectedNode != m_selectedGraphNode) {
        if (const auto* previous = m_graph.FindNode(m_uvLastSelectedNode);
            previous && previous->kind == graph::NodeKind::UvUnwrap && m_previewGraphNode == previous->id)
            SetPreviewGraphNode(m_uvPreviousPreviewNode, m_uvPreviousPreviewPin);
        m_pieceSelectionEditing = false;
        m_pieceGizmoId = -1;
        m_uvLastSelectedNode = m_selectedGraphNode;
        if (const auto* node = m_graph.FindNode(m_selectedGraphNode); node && node->kind == graph::NodeKind::UvUnwrap) {
            m_uvPreviousPreviewNode = m_previewGraphNode;
            m_uvPreviousPreviewPin = m_previewGraphPin;
            SetPreviewGraphNode(node->id);
        }
    }
    // メッシュを作るノードを見ているときは、そのノードまでの鎖を出す。
    graph::GraphId previewMeshNode = 0;
    if (const graph::Node* node = m_graph.FindNode(m_previewGraphNode);
        node != nullptr && (graph::IsMeshNodeKind(node->kind) || graph::IsModelNodeKind(node->kind))) {
        previewMeshNode = node->id;
    }
    m_uvCheckerPreview = previewMeshNode && m_graph.FindNode(previewMeshNode)->kind == graph::NodeKind::UvUnwrap;
    const bool hasPieces = std::any_of(m_graph.Nodes().begin(), m_graph.Nodes().end(), [](const auto& n) {
        // UV Unwrap は大きなメッシュで数秒〜数十秒かかる。UI スレッドで走らせるとアプリが固まり、
        // 計算中であることも表示できない。
        return graph::IsPieceNodeKind(n.kind) || n.kind == graph::NodeKind::ToVolume ||
               n.kind == graph::NodeKind::UvUnwrap || n.kind == graph::NodeKind::Decimate;
    });
    // 形状を決める部分と、選択中ノード（ピース操作欄に出す入力の評価先）を分けて持つ。
    const std::string geometryKey = std::to_string(m_graph.Revision()) + ":" + std::to_string(m_pieceEpoch) + ":" + std::to_string(previewMeshNode) + ":" + std::to_string(int(m_settings.Display().sdfPreviewMethod));
    const std::string taskKey = geometryKey + ":" + std::to_string(m_selectedGraphNode);
    if ((!hasPieces || m_pieceCompletedKey == taskKey) && m_meshGraphRevision == m_graph.Revision() && m_meshGraphPreviewNode == previewMeshNode &&
        m_meshGraphSmoothShading == m_settings.Display().smoothShading &&
        m_meshGraphSdfPreviewMethod == m_settings.Display().sdfPreviewMethod)
        return;
    m_meshGraphSmoothShading = m_settings.Display().smoothShading;
    m_meshGraphSdfPreviewMethod = m_settings.Display().sdfPreviewMethod;
    graph::RockEvaluation evaluated;
    if (hasPieces) {
        m_pieceUpdating = true;
        if (m_pieceTask.valid()) {
            // 選択を変えただけなら実行中の重い評価を止めない。完了後にそのキャッシュを引き継ぎ、
            // 新しい選択の評価はキャッシュを使ってすぐ終わる。
            if (m_pieceTaskGeometryKey != geometryKey) m_pieceStop.request_stop();
            if (m_pieceTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            auto finished = m_pieceTask.get();
            m_rockEvaluationCache = std::move(finished.cache);
            if (m_pieceTaskKey == taskKey) {
                evaluated = std::move(finished.output);
                m_pieceInput = finished.input.pieces;
                m_pieceInputNode = m_selectedGraphNode;
                m_pieceTransformSelection = finished.selection.selection;
                m_pieceCompletedKey = taskKey;
                m_pieceUpdating = false;
            }
        }
        if (m_pieceUpdating) {
            m_pieceTaskKey = taskKey;
            m_pieceTaskGeometryKey = geometryKey;
            m_pieceStop = std::stop_source{};
            m_pieceProgress = std::make_shared<graph::RockEvaluationProgress>();
            m_pieceTaskStart = std::chrono::steady_clock::now();
            m_pieceTask = std::async(std::launch::async, [snapshot=m_graph, previewMeshNode, selected=m_selectedGraphNode,
                method=m_settings.Display().sdfPreviewMethod, cache=m_rockEvaluationCache, stop=m_pieceStop.get_token(),
                progress=m_pieceProgress]() mutable {
                PieceTaskResult result;
                // ここから漏れた例外は get() でUIスレッドへ再送出され、未保存の編集ごとアプリが落ちる。
                // メモリ不足などは生成エラーとして表示する。
                try {
                    result.output = graph::EvaluateRocks(snapshot, previewMeshNode, &cache, method, stop, progress.get());
                    if (const auto* n = snapshot.FindNode(selected); n && graph::IsPieceNodeKind(n->kind) && !n->inputs.empty())
                        if (const auto* parent = snapshot.FindUpstreamNodeForPin(n->inputs[0].id))
                            result.input = graph::EvaluateRocks(snapshot, parent->id, &cache, method, stop, progress.get());
                    if (const auto* n = snapshot.FindNode(selected); n && n->kind == graph::NodeKind::PieceTransform && n->inputs.size() > 1)
                        if (const auto* parent = snapshot.FindUpstreamNodeForPin(n->inputs[1].id))
                            result.selection = graph::EvaluateRocks(snapshot, parent->id, &cache, method, stop, progress.get());
                    result.cache = std::move(cache);
                } catch (const std::exception& e) {
                    result = {};
                    result.output.error = std::string("評価中に例外が発生しました: ") + e.what();
                } catch (...) {
                    result = {};
                    result.output.error = "評価中に不明な例外が発生しました";
                }
                return result;
            });
            return;
        }
    } else {
        m_pieceStop.request_stop();
        m_pieceUpdating = false; m_pieceInput.reset();
        evaluated = graph::EvaluateRocks(m_graph, previewMeshNode, &m_rockEvaluationCache, m_settings.Display().sdfPreviewMethod);
    }
    m_piecePreview = evaluated.pieces;
    renderer::MeshScene scene;
    m_uvPreviewMesh = {};
    m_rockMeshReferences.clear();
    m_rockTriangleCounts.clear();
    std::vector<int> selectedPieces;
    for (const auto& rock : evaluated.rocks) {
        if (geometry::HasValidUvs(rock.mesh) && m_uvPreviewMesh.cornerUvs.empty()) m_uvPreviewMesh = rock.mesh;
        renderer::SceneMesh mesh;
        mesh.geometry = renderer::MakeRockMeshData(rock.mesh, m_settings.Display().smoothShading);
        mesh.material.baseColor = DirectX::XMFLOAT3{0.35f, 0.32f, 0.28f};
        if (rock.pieceId >= 0) {
            float r,g,b;
            ImGui::ColorConvertHSVtoRGB(std::fmod(float(rock.pieceId)*.618034f,.999f),.5f,.75f,r,g,b);
            mesh.material.baseColor = {r,g,b};
            if (rock.pieceSelected) selectedPieces.push_back(int(scene.meshes.size()));
        }
        m_rockMeshReferences.push_back({rock.source, rock.pieceId});
        m_rockTriangleCounts.push_back(rock.mesh.triangles.size());
        mesh.material.roughness = 0.8f;
        ApplyRockMaterial(mesh, rock, true);
        scene.meshes.push_back(std::move(mesh));
    }
    m_meshGraphError = evaluated.error;
    m_meshHighlight = MeshHighlightState{};
    m_meshHighlight.selected = std::move(selectedPieces);
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
        entry.originalId = node->id;
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

    for (const auto id : created) if (auto* pasted = m_graph.FindMutableNode(id)) {
        const auto remap = [&](int& producer) {
            for (size_t i=0; i<m_graphClipboard.size(); ++i)
                if (m_graphClipboard[i].kind == graph::NodeKind::VoronoiFracture && m_graphClipboard[i].originalId == producer) {
                    producer = created[i]; break;
                }
        };
        if (auto* s = std::get_if<geometry::PieceSelectSettings>(&pasted->settings)) remap(s->producer);
        if (auto* s = std::get_if<geometry::PieceTransformSettings>(&pasted->settings)) remap(s->producer);
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
        if (EvaluatingNode() == node.id) {
            // いま評価スレッドが計算しているノード。どこで待っているのかをグラフの上で示す。
            const int percent = m_pieceProgress->percent.load(std::memory_order_relaxed);
            ImGui::SameLine();
            if (percent > 0)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "計算中 %d%%", percent);
            else
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "計算中…");
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
        // 扱う型ごとに分けて並べる。見出しは出力する型（変換ノードは変換後の型）で選ぶ。
        ImGui::TextDisabled("メッシュ（ポリゴン）");
        addNodeMenuItem(graph::NodeKind::BaseRock, "Base Shape — 基本形状と弱いノイズ");
        addNodeMenuItem(graph::NodeKind::RandomBoxes, "Random Boxes — 直方体メッシュを重ねて塊を作る");
        addNodeMenuItem(graph::NodeKind::VolumeToMesh, "Volume to Mesh — ボリュームをメッシュに変換");
        addNodeMenuItem(graph::NodeKind::Decimate, "Decimate — 形を保ったまま三角形を減らす");
        addNodeMenuItem(graph::NodeKind::UvUnwrap, "UV Unwrap — 自動UV展開");
        ImGui::Separator();
        ImGui::TextDisabled("分割・ピース操作");
        addNodeMenuItem(graph::NodeKind::ScatterPoints, "Scatter Points — 内部に点を配置");
        addNodeMenuItem(graph::NodeKind::VoronoiFracture, "Voronoi Fracture — 凸形状を立体分割");
        addNodeMenuItem(graph::NodeKind::PieceSelect, "Piece Select — ピースを選別");
        addNodeMenuItem(graph::NodeKind::PieceFilter, "Piece Filter — 選択を削除・抽出");
        addNodeMenuItem(graph::NodeKind::PieceTransform, "Piece Transform — ピースを配置");
        addNodeMenuItem(graph::NodeKind::PiecesToMesh, "Pieces to Mesh — UV・材質工程へ変換");
        ImGui::Separator();
        ImGui::TextDisabled("ボリューム");
        addNodeMenuItem(graph::NodeKind::ToVolume, "To Volume — メッシュをボリュームに変換");
        addNodeMenuItem(graph::NodeKind::VolumeTransform, "Volume Transform — ボリュームを移動・回転・拡大");
        addNodeMenuItem(graph::NodeKind::VolumeBoolean, "Volume Boolean — 2つのボリュームの和・交差・差");
        addNodeMenuItem(graph::NodeKind::PlaneCuts, "Plane Cuts — 平面の群で切り落とし、角張った面を作る");
        addNodeMenuItem(graph::NodeKind::VolumeCrack, "Volume Crack — 点の群の境界に沿って割れ目を彫る");
        addNodeMenuItem(graph::NodeKind::VolumeNoise, "Volume Noise — 表面をノイズで削り、直線的な面を崩す");
        ImGui::Separator();
        ImGui::TextDisabled("モデル");
        addNodeMenuItem(graph::NodeKind::Model, "Model — 3D モデル（.rockmodel）を 1 つ置く");
        addNodeMenuItem(graph::NodeKind::Transform, "Transform — 上流のモデルをまとめて移動・回転・拡大");
        ImGui::Separator();
        ImGui::TextDisabled("共通");
        addNodeMenuItem(graph::NodeKind::Merge, "Merge — メッシュとモデルをまとめる（モデルだけなら Transform へ繋げる）");
        addNodeMenuItem(graph::NodeKind::MeshOutput, "Mesh Output — メッシュとモデルを表示");
        ImGui::Separator();
        ImGui::TextDisabled("材質");
        addNodeMenuItem(graph::NodeKind::Surface, "Surface — マテリアルを Material スロットへ渡す");
        addNodeMenuItem(graph::NodeKind::ApplyMaterial, "Apply Material — マスクで素材を適用");
        addNodeMenuItem(graph::NodeKind::MaterialMask, "Material Mask — 定数・画像マスク");
        addNodeMenuItem(graph::NodeKind::MaterialBake, "Material Bake — UVへ材質を焼き付ける");
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
    } else if (graph::IsPieceNodeKind(selected->kind)) {
        DrawPieceSettings(*selected);
    } else if (auto* boxes = std::get_if<geometry::BoxClusterSettings>(&selected->settings)) {
        auto edited = *boxes;
        bool changed = false;
        if (ui::BeginPropertyTable("randomBoxesRows")) {
            changed |= ui::PropertyInt("個数", &edited.count, 1, 32, 8);
            changed |= ui::PropertyFloat("基準寸法 X (m)", &edited.size[0], .1f, 100, 2);
            changed |= ui::PropertyFloat("基準寸法 Y (m)", &edited.size[1], .1f, 100, 2.4f);
            changed |= ui::PropertyFloat("基準寸法 Z (m)", &edited.size[2], .1f, 100, 1.8f);
            changed |= ui::PropertyFloat("サイズばらつき", &edited.sizeVariation, 0, .8f, .55f);
            changed |= ui::PropertyFloat("配置の広がり", &edited.spread, 0, .95f, .85f);
            changed |= ui::PropertyFloat("回転幅 (度)", &edited.rotation, 0, 90, 25);
            changed |= ui::PropertyInt("Seed", &edited.seed, 0, 1000000000, 42);
            ui::EndPropertyTable();
        }
        ui::HintText("直方体を互いに重ねて塊を作ります。同じ Seed で同じ形になります。基準寸法は最初の直方体の大きさです。");
        ui::HintText("MeshをTo Volumeに接続すると、重なりを一体化した外側の表面を表示します。UV UnwrapやMergeにも接続できます。");
        if (changed) {
            *boxes = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* volume = std::get_if<geometry::VolumeSettings>(&selected->settings)) {
        auto edited = *volume;
        bool changed = false;
        if (ui::BeginPropertyTable("volumeRows")) {
            changed |= ui::PropertyInt("解像度", &edited.resolution, 16, 96, 48);
            ui::EndPropertyTable();
        }
        ui::HintText("閉じたMeshをボリュームに変換します。Random Boxes、Base Shape、Pieces to Meshなどを接続できます。重なった外向きの立体は和集合になります。");
        ui::HintText("穴の開いたメッシュや不正な面の向きは診断します。Volume to Meshへ接続すると、表面をMeshとして取り出せます。");
        ui::HintText("解像度は最長辺の分割数です。高くすると角や細い形を保ちやすくなり、処理時間も増えます。");
        if (changed) {
            *volume = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* noise = std::get_if<geometry::VolumeNoiseSettings>(&selected->settings)) {
        auto edited = *noise;
        bool changed = false;
        if (ui::BeginPropertyTable("volumeNoiseRows")) {
            const char* types[] = {"なめらか", "セル状（丸い盛り上がり）", "小面（割れ肌）"};
            int type = std::clamp(static_cast<int>(edited.type), 0, 2);
            if (ui::PropertyCombo("種類", &type, types, 3, 2)) {
                edited.type = static_cast<geometry::VolumeNoiseType>(type);
                changed = true;
            }
            changed |= ui::PropertyFloat("量", &edited.amount, 0, .2f, .02f,
                                         "削る量の最大。形の最長辺に対する比です。削る方向にだけ効き、形は広がりません。");
            changed |= ui::PropertyFloat("細かさ", &edited.scale, .5f, 64, 8,
                                         "最長辺あたりのノイズの山の数。セル間隔より細かい凹凸は格子で潰れます。");
            changed |= ui::PropertyInt("重ねる数", &edited.octaves, 1, 5, 2,
                                       "細かさを倍、量を半分にしながら重ねる数。");
            changed |= ui::PropertyFloat("歪み", &edited.warp, 0, .2f, 0,
                                         "サンプル位置をずらす量の最大。平面や直線的な割れ目を波打たせます。");
            changed |= ui::PropertyFloat("歪みの細かさ", &edited.warpScale, .5f, 16, 2);
            changed |= ui::PropertyInt("Seed", &edited.seed, 0, 1000000000, 1);
            ui::EndPropertyTable();
        }
        ui::HintText("表面をノイズで削ります。「小面」はセルごとの平面で段差のある割れ肌、「セル状」は丸い盛り上がりと谷、「なめらか」は緩やかな起伏になります。");
        ui::HintText("「歪み」は形そのものをゆらし、Plane Cuts の平面や Volume Crack の直線的な割れ目を崩します。格子を作り直すので、境界がセル1個ぶん程度なまります。"
                     "歪みがあると、その分だけ格子が広がります。浮いた小片と閉じた空洞は自動で除きます。");
        if (changed) {
            edited.amount = std::clamp(edited.amount, 0.0f, 0.2f);
            edited.scale = std::clamp(edited.scale, 0.5f, 64.0f);
            edited.octaves = std::clamp(edited.octaves, 1, 5);
            edited.warp = std::clamp(edited.warp, 0.0f, 0.2f);
            edited.warpScale = std::clamp(edited.warpScale, 0.5f, 16.0f);
            *noise = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* crack = std::get_if<geometry::VolumeCrackSettings>(&selected->settings)) {
        auto edited = *crack;
        bool changed = false;
        if (ui::BeginPropertyTable("volumeCrackRows")) {
            changed |= ui::PropertyFloat("幅", &edited.width, 0, .2f, .03f,
                                         "表面での割れ目の幅。形の最長辺に対する比です。セル間隔より細い割れ目は格子で潰れます。");
            changed |= ui::PropertyFloat("深さ", &edited.depth, .01f, 1, .15f,
                                         "割れ目が届く深さ。形の最長辺に対する比です。深くなるほど狭まり、この深さで閉じます。1 で形を貫きます。");
            changed |= ui::PropertyFloat("ばらつき", &edited.variation, 0, 1, .6f,
                                         "割れ目ごとの幅の差。大きいほど細い割れ目が増え、一部は閉じます。");
            changed |= ui::PropertyFloat("ゆらぎ", &edited.noise, 0, 1, .5f,
                                         "割れ目に沿った幅の変化。大きいほど途中で細くなり、途切れます。");
            changed |= ui::PropertyFloat("ゆらぎの細かさ", &edited.noiseScale, .5f, 16, 3);
            changed |= ui::PropertyInt("Seed", &edited.seed, 0, 1000000000, 1);
            ui::EndPropertyTable();
        }
        ui::HintText("Points の点が作る Voronoi の境界面に沿って、Volume の表面から割れ目を彫ります。格子は変わりません。"
                     "Points には Scatter Points をつなぎます。点を増やすと割れ目が細かくなります。");
        ui::HintText("割れ目の配置は Scatter Points の点数と Seed、幅の散らばり方はこのノードの Seed で変わります。"
                     "細い割れ目を出すには、上流の To Volume の解像度を上げます。");
        if (changed) {
            edited.width = std::clamp(edited.width, 0.0f, 0.2f);
            edited.depth = std::clamp(edited.depth, 0.01f, 1.0f);
            edited.variation = std::clamp(edited.variation, 0.0f, 1.0f);
            edited.noise = std::clamp(edited.noise, 0.0f, 1.0f);
            edited.noiseScale = std::clamp(edited.noiseScale, 0.5f, 16.0f);
            *crack = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* cuts = std::get_if<geometry::PlaneCutsSettings>(&selected->settings)) {
        auto edited = *cuts;
        bool changed = false;
        const float zero[3] = {0, 0, 0};
        if (ui::BeginPropertyTable("planeCutsRows")) {
            changed |= ui::PropertyInt("枚数", &edited.count, 1, geometry::MaxPlaneCuts, 12);
            changed |= ui::PropertyInt("Seed", &edited.seed, 0, 1000000000, 1);
            const char* scopes[] = {"全体（大きな面取り）", "局所（欠け）"};
            int scope = std::clamp(static_cast<int>(edited.scope), 0, 1);
            if (ui::PropertyCombo("適用範囲", &scope, scopes, 2, 0)) {
                edited.scope = static_cast<geometry::PlaneCutsScope>(scope);
                changed = true;
            }
            if (edited.scope == geometry::PlaneCutsScope::Local)
                changed |= ui::PropertyFloat("半径", &edited.radius, .02f, 1, .2f,
                                             "欠け1つが届く範囲。形の最長辺に対する比です。");
            changed |= ui::PropertyFloat("深さ 最小", &edited.depthMin, 0, geometry::MaxPlaneCutDepth, .05f,
                                         "切り込みの深さ。全体では平面の向きに測った形の幅、局所では欠けの半径に対する比です。");
            changed |= ui::PropertyFloat("深さ 最大", &edited.depthMax, 0, geometry::MaxPlaneCutDepth, .25f);
            const char* distributions[] = {"等方（あらゆる向き）", "主方向（節理の系統）"};
            int distribution = std::clamp(static_cast<int>(edited.distribution), 0, 1);
            if (ui::PropertyCombo("法線の分布", &distribution, distributions, 2, 0)) {
                edited.distribution = static_cast<geometry::PlaneCutsDistribution>(distribution);
                changed = true;
            }
            if (edited.distribution == geometry::PlaneCutsDistribution::Directional) {
                changed |= ui::PropertyInt("系統数", &edited.systems, 1, 3, 2,
                                           "向きを回した座標系の X / Y / Z 軸を、この数だけ主方向に使います。");
                changed |= ui::PropertyFloat3Input("向き (度)", edited.rotationDegrees.data(), zero) != 0;
                changed |= ui::PropertyFloat("ばらつき (度)", &edited.spreadDegrees, 0, 90, 12);
            }
            changed |= ui::PropertyFloat("なめらかさ (m)", &edited.blend, 0, 1, 0,
                                         "稜線を丸める幅。0 で角を残します。Ctrl + クリックで 10 m まで入力できます。");
            ui::EndPropertyTable();
        }
        ui::HintText("平面の群でボリュームを切り落とし、割れた岩のような角張った面を作ります。入力・出力とも Volume 型で、格子は変わりません。");
        ui::HintText("「全体」は形全体を平面で切ります。枚数を増やすほど凸な形に近づき、凹みが消えるので、少ない枚数（3〜8）で大きな面取りに使います。"
                     "「局所」は稜線や角を平面で欠き、凹凸を残したまま小面を増やします。平らな面の中央のように、切り口が丸い壁になる欠けは自動で除きます（枚数 40〜、半径 0.1〜0.3）。");
        ui::HintText("2つ直列につなぐと両方を重ねられます。稜線を残すには Volume to Mesh を Dual Contouring にします。");
        if (changed) {
            edited.count = std::clamp(edited.count, 1, geometry::MaxPlaneCuts);
            edited.systems = std::clamp(edited.systems, 1, 3);
            edited.depthMin = std::clamp(edited.depthMin, 0.0f, geometry::MaxPlaneCutDepth);
            // 片方を動かして大小が逆転したら、動かした側へもう一方を合わせる。
            if (edited.depthMin != cuts->depthMin) edited.depthMax = std::max(edited.depthMax, edited.depthMin);
            edited.depthMax = std::clamp(edited.depthMax, 0.0f, geometry::MaxPlaneCutDepth);
            edited.depthMin = std::min(edited.depthMin, edited.depthMax);
            edited.blend = std::clamp(edited.blend, 0.0f, 10.0f);
            edited.radius = std::clamp(edited.radius, 0.02f, 1.0f);
            *cuts = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* boolean = std::get_if<geometry::VolumeBooleanSettings>(&selected->settings)) {
        auto edited = *boolean;
        bool changed = false;
        if (ui::BeginPropertyTable("volumeBooleanRows")) {
            const char* operations[] = {"和 (A ∪ B)", "交差 (A ∩ B)", "差 (A − B)"};
            int operation = std::clamp(static_cast<int>(edited.operation), 0, 2);
            if (ui::PropertyCombo("演算", &operation, operations, 3, 0)) {
                edited.operation = static_cast<geometry::VolumeBooleanOperation>(operation);
                changed = true;
            }
            changed |= ui::PropertyFloat("なめらかさ (m)", &edited.blend, 0, 2, 0,
                                         "つなぎ目を丸める幅。0 で角を残します。Ctrl + クリックで 10 m まで入力できます。");
            ui::EndPropertyTable();
        }
        ui::HintText("A を基準に B との和・交差・差を取ります。入力・出力とも Volume 型です。"
                     "B を動かすには、B の上流に Volume Transform を置きます。");
        ui::HintText("結果は A の格子（セル間隔）を引き継ぎます。和は B を含む範囲まで格子を広げ、"
                     "各軸192点を超えると生成できません。B が A より細かくても、細部は A のセル間隔までしか残りません。");
        if (changed) {
            edited.blend = std::clamp(edited.blend, 0.0f, 10.0f);
            *boolean = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* moved = std::get_if<geometry::VolumeTransformSettings>(&selected->settings)) {
        auto edited = *moved;
        bool changed = false;
        const float zero[3] = {0, 0, 0};
        if (ui::BeginPropertyTable("volumeTransformRows")) {
            changed |= ui::PropertyFloat3Input("移動 (m)", edited.position.data(), zero) != 0;
            changed |= ui::PropertyFloat3Input("回転 (度)", edited.rotationDegrees.data(), zero) != 0;
            changed |= ui::PropertyFloat("倍率", &edited.scale, .05f, 20, 1);
            ui::EndPropertyTable();
        }
        ui::HintText("ボリュームを、倍率 → 回転 → 移動の順に原点まわりで動かします。入力・出力とも Volume 型です。");
        ui::HintText("格子を作り直すため、回転すると境界がセル1個ぶん程度なまります。セル間隔は倍率に比例し、解像度は保たれます。");
        ui::HintText("このノードを選ぶとビューポートにギズモが出ます。W 移動 / E 回転 / R 倍率、Ctrl で刻み、Esc で取り消し。"
                     "ギズモの中心は移動量の位置で、回転と倍率もその点が中心です。");
        if (changed) {
            *moved = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* rock = std::get_if<graph::BaseRockNodeSettings>(&selected->settings)) {
        bool changed = false;
        auto edited = *rock;
        if (ui::BeginPropertyTable("baseRockRows")) {
            const char* shapes[] = {"Box", "RoundedBox", "Sphere", "Ellipsoid", "不明（選び直してください）"};
            int shape = std::clamp(static_cast<int>(edited.shape), 0, 4);
            if (ui::PropertyCombo("形状", &shape, shapes, shape == 4 ? 5 : 4, 0)) {
                edited.shape = static_cast<geometry::BaseShape>(shape);
                changed = true;
            }
            changed |=
                ui::PropertyFloat(edited.shape == geometry::BaseShape::Sphere ? "直径 (m)" : "Size X (m)",
                                  &edited.size[0], 0.001f, 1000.0f, 2.0f);
            if (edited.shape != geometry::BaseShape::Sphere) {
                changed |= ui::PropertyFloat("Size Y (m)", &edited.size[1], 0.001f, 1000.0f, 2.0f);
                changed |= ui::PropertyFloat("Size Z (m)", &edited.size[2], 0.001f, 1000.0f, 2.0f);
            }
            if (edited.shape == geometry::BaseShape::RoundedBox)
                changed |= ui::PropertyFloat("丸み", &edited.roundness, 0, 1, 0.25f);
            const char* levels[] = {"4", "8", "16", "32", "不明（選び直してください）"};
            const int divisions[] = {4, 8, 16, 32};
            int level = 4;
            for (int i = 0; i < 4; ++i)
                if (edited.subdivisions == divisions[i]) level = i;
            if (ui::PropertyCombo("面の分割数", &level, levels, level == 4 ? 5 : 4, 1) && level < 4) {
                edited.subdivisions = divisions[level];
                changed = true;
            }
            changed |= ui::PropertyFloat("ノイズ強度", &edited.noiseStrength, 0, 0.15f, 0);
            changed |= ui::PropertyFloat("ノイズ細かさ", &edited.noiseScale, 0.5f, 4, 2);
            changed |= ui::PropertyInt("Seed", &edited.seed, 0, 1000000000, 0);
            ui::EndPropertyTable();
        }
        ui::HintText(
            "原点中心の母岩。寸法はノイズを加える前の大きさです。ノイズ強度は半径に対する変位率、細かさを上げ"
            "ると細かな凹凸になります。");
        ui::HintText("Seed はノイズがあるときに形を変えます。");
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
    } else if (selected->kind == graph::NodeKind::VolumeToMesh) {
        const auto* settings = std::get_if<geometry::VolumeToMeshSettings>(&selected->settings);
        int method = settings ? static_cast<int>(settings->method) : 0;
        const char* methods[] = {"Marching Tetrahedra", "Dual Contouring"};
        bool changed = false;
        if (ui::BeginPropertyTable("volumeToMeshRows")) {
            changed = ui::PropertyCombo("変換方式", &method, methods, 2, 0);
            ui::EndPropertyTable();
        }
        if (changed) {
            selected->settings = geometry::VolumeToMeshSettings{static_cast<geometry::VolumeMeshingMethod>(method)};
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
        ui::HintText("Volume の表面を三角形メッシュへ変換します。Mesh 出力を Mesh Output や Merge に接続できます。");
        ui::HintText("解像度は上流の To Volume で調整します。Marching Tetrahedra は従来方式、Dual Contouring は角や稜線を保つために頂点位置を調整する方式です。");
        if (method == 1)
            ui::HintText("Dual Contouring は格子から交点・法線を推定します。細部や角の再現には入力の解像度も影響します。");
    } else if (auto* decimate = std::get_if<geometry::DecimateSettings>(&selected->settings)) {
        auto edited = *decimate;
        bool changed = false;
        if (ui::BeginPropertyTable("decimateRows")) {
            changed |= ui::PropertyInt("目標の三角形数", &edited.targetTriangles, geometry::MinDecimateTriangles, 100000, 10000,
                                       "入力がこれ以下なら何もしません。Ctrl + クリックで 500000 まで入力できます。");
            changed |= ui::PropertyFloat("形のずれの上限", &edited.maxError, 0, .02f, .004f,
                                         "形の最長辺に対する比。これを超える縮約はしないので、目標に届かないことがあります。0 なら上限なしで目標まで減らします。",
                                         "%.4f");
            changed |= ui::PropertyFloat("稜線の保護", &edited.creaseWeight, 0, 10, 1,
                                         "折れ角の大きい辺を動かしにくくします。0 で保護なし。");
            ui::EndPropertyTable();
        }
        if (EvaluatingNode() == selected->id) {
            const int percent = m_pieceProgress->percent.load(std::memory_order_relaxed);
            ImGui::ProgressBar(percent > 0 ? float(percent) / 100.0f : -1.0f * float(ImGui::GetTime()), ImVec2(-1, 0),
                               EvaluationProgressText().c_str());
        } else if (!m_pieceUpdating) {
            // 実際に何枚になったかを出す。上限や形の制約で、目標に届かないことがある。
            size_t triangles = 0;
            bool shown = false;
            for (size_t i = 0; i < m_rockMeshReferences.size() && i < m_rockTriangleCounts.size(); ++i)
                if (m_rockMeshReferences[i].source == selected->id) {
                    triangles += m_rockTriangleCounts[i];
                    shown = true;
                }
            if (shown && triangles > size_t(edited.targetTriangles) + size_t(edited.targetTriangles) / 20)
                ui::HintText("現在の出力: %zu 三角形。目標に届いていません。形のずれの上限を上げる（0 で上限なし）と、さらに減ります。", triangles);
            else if (shown)
                ui::HintText("現在の出力: %zu 三角形", triangles);
        }
        ui::HintText("形をできるだけ保ったまま三角形を減らします（QEM による辺の縮約）。平らな場所は大きく減り、稜線や割れ目の縁は残ります。"
                     "UV Unwrap の前に置くと、展開が大幅に速くなります。");
        ui::HintText("入力は閉じたメッシュです。面の裏返りや穴を作る縮約は行いません。細かい凹凸は失われ、UV は引き継ぎません。");
        if (changed) {
            edited.targetTriangles = std::clamp(edited.targetTriangles, geometry::MinDecimateTriangles, geometry::MaxDecimateTriangles);
            edited.maxError = std::clamp(edited.maxError, 0.0f, 0.1f);
            edited.creaseWeight = std::clamp(edited.creaseWeight, 0.0f, 10.0f);
            *decimate = edited;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* uvSettings = std::get_if<geometry::UvUnwrapSettings>(&selected->settings)) {
        bool changed = false;
        if (ui::BeginPropertyTable("uvUnwrapRows")) {
            const char* resolutions[] = {"128", "256", "512", "1024", "2048", "4096"};
            int resolutionIndex = 0;
            while ((128 << resolutionIndex) < uvSettings->resolution && resolutionIndex < 5) ++resolutionIndex;
            if (ui::PropertyCombo("テクスチャ解像度", &resolutionIndex, resolutions, 6, 3)) {
                uvSettings->resolution = 128 << resolutionIndex;
                changed = true;
            }
            changed |= ui::PropertyInt("余白 (px)", &uvSettings->padding, 1, 32, 4);
            changed |= ui::PropertyInt("品質", &uvSettings->quality, 1, 4, 1);
            ui::EndPropertyTable();
        }
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
        if (EvaluatingNode() == selected->id) {
            const int percent = m_pieceProgress->percent.load(std::memory_order_relaxed);
            // 進み具合が分からない段階は、バーを左右へ動かして「動いている」ことだけを示す。
            ImGui::ProgressBar(percent > 0 ? float(percent) / 100.0f : -1.0f * float(ImGui::GetTime()), ImVec2(-1, 0),
                               EvaluationProgressText().c_str());
            ui::HintText("UV展開はCPUで行います。三角形が多いほど時間がかかり、6万三角形で15秒ほどです。島への分割の前半は進み具合が出ず、その間は打ち切りも効きません。"
                         "表示は前回の結果です。");
        }
        ui::HintText("選択するとUVチェッカーを表示します。UVビューのタブで島の配置を確認できます。複数の入力メッシュは1枚のアトラスへまとめます。");
    } else if (selected->kind == graph::NodeKind::ApplyMaterial) {
        ui::HintText("MeshとSurfaceを接続します。Mask未接続なら全面を置換。Mask接続時は白で新しい素材、黒で上流の素材、中間値で混合します。最大8段まで重ねられます。");
    } else if (auto* mask = std::get_if<graph::MaterialMaskSettings>(&selected->settings)) {
        bool changed = false;
        if (ui::BeginPropertyTable("materialMask")) {
            changed |= DrawTextureSlotRow("画像（R）", mask->texture, m_textureLibrary);
            changed |= ui::PropertyFloat("値・画像の強度", &mask->value, 0, 1, 1);
            changed |= ui::PropertyBool("反転", &mask->invert, false);
            changed |= ui::PropertyBool("Triplanar", &mask->triplanar, false);
            changed |= ui::PropertyFloat("反復幅", &mask->repeatMeters, .001f, 10000, 1);
            ui::EndPropertyTable();
        }
        ui::HintText("画像未指定なら定数。画像はリニアのRを使用。反復幅はUV時はUV単位、Triplanar時はメートルです。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (selected->kind == graph::NodeKind::MaterialBake) {
        ui::HintText("UV付きのMeshを接続します。Apply Materialの素材を焼き付けます。MaterialにSurfaceを接続すると全面を置換します。");
        auto& bake = std::get<graph::MaterialBakeSettings>(selected->settings);
        bool changed = false;
        if (ui::BeginPropertyTable("geometryAo")) {
            changed |= ui::PropertyBool("形状AOをベイク", &bake.geometryAo, false);
            if (bake.geometryAo) {
                changed |= ui::PropertyFloat("AO距離 (m)", &bake.aoDistance, .001f, 1000, .5f);
                changed |= ui::PropertyFloat("AO強度", &bake.aoStrength, 0, 1, 1);
                changed |= ui::PropertyInt("AOサンプル数", &bake.aoSamples, 8, 128, 32);
            }
            ui::EndPropertyTable();
        }
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
        ui::HintText("形状AOはGPUで同じ入力メッシュの遮蔽を計算し、素材AOに乗算します。サンプル数と画像サイズが大きいほど時間がかかります。");
        ImGui::BeginDisabled(m_pieceUpdating || m_bakeJob.has_value() || m_pendingBake != 0);
        if (ImGui::Button("ベイク実行")) m_pendingBake=selected->id;
        ImGui::EndDisabled();
        if (m_bakeJob && m_bakeJob->id == selected->id)
            ui::HintText("形状AOをGPUで計算中です。進捗ウィンドウからキャンセルできます。");
        else if (const auto status=m_bakeStatus.find(selected->id); status!=m_bakeStatus.end())
            ui::HintText("%s", status->second.c_str());
        else ui::HintText("未ベイク。ノードの出力をプレビューして状態を確認してください。");
        DrawBakedTextureTiles(bake);
        {
            // 出力できるのは、この起動中にベイクした結果だけ（画像をメモリに持っているもの）。
            const bool exportable = m_bakeImages.contains(selected->id);
            ImGui::BeginDisabled(!exportable || m_bakeJob.has_value() || m_pendingBake != 0);
            if (ImGui::Button("テクスチャを出力…")) ExportBakedTextures(selected->id);
            ImGui::EndDisabled();
            if (!exportable && m_materialLibrary.Find(bake.bakedLayer.material) != nullptr)
                ui::HintText("このベイク結果は旧版がファイルへ保存したものです。もう一度ベイクすると出力できます。");
        }
        ui::HintText("UV Unwrapのアトラス寸法で4枚の画像（Base Color / Normal / Roughness・Metallic・AO / Height）を作ります。結果は一時的なもので、ファイルにもシーンにも保存しません。残すには「テクスチャを出力…」でフォルダへ書き出します。シーンを開き直したら、もう一度ベイクしてください。形状・材質・スムーズシェーディングを変えたときも再ベイクが必要です。");
    } else if (selected->kind == graph::NodeKind::MeshOutput) {
        ui::HintText("メッシュ・モデル・Volumeを接続すると表示する。複数のMesh Outputを同時に表示できる。");
        ui::HintText("MaterialにSurfaceを接続すると、生成メッシュに材質を適用します。UVのない岩にはSurfaceでTriplanarを選びます。モデルの材質はモデル側で設定します。");
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
