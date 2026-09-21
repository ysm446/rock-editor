#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"
#include <algorithm>
namespace rock {
void Application::DrawPieceSettings(graph::Node &node) {
    bool changed = false;
    const float zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    const bool ready = !m_pieceUpdating && m_pieceInputNode == node.id && m_pieceInput;
    if (m_pieceUpdating)
        ui::HintText("更新中…表示は前回の結果です。選択・ベイクは完了後に操作できます。");
    if (auto *scatter = std::get_if<geometry::ScatterSettings>(&node.settings)) {
        if (ui::BeginPropertyTable("scatter")) {
            changed |= ui::PropertyInt("点数", &scatter->count, 2, 128, 24);
            int seed = int(scatter->seed);
            if (ui::PropertyInt("Seed", &seed, 0, 1000000, 1)) {
                scatter->seed = uint32_t(seed);
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::HintText("閉じた凸形状の内部に点を配置します。同じMeshをVoronoi Fractureにも接続してください。");
    } else if (auto *voronoi = std::get_if<geometry::VoronoiSettings>(&node.settings)) {
        if (ui::BeginPropertyTable("voronoi")) {
            changed |= ui::PropertyFloat3Input("方向 (度)", voronoi->rotation.data(), zero) != 0;
            changed |= ui::PropertyFloat3Input("伸長 XYZ", voronoi->stretch.data(), one) != 0;
            ui::EndPropertyTable();
        }
        ui::HintText("Yの伸長を4にすると縦に長い分割片になります。倍率の最大/"
                     "最小比は16以下。最初はノイズ0のBoxを使用してください。");
    } else if (auto *selection = std::get_if<geometry::PieceSelectSettings>(&node.settings)) {
        if (ui::BeginPropertyTable("pieceSelect")) {
            const char *modes[] = {"Manual — 手動ID", "Outer — 外面に接する片", "Region — 重心の範囲",
                                   "Volume — 体積", "Random — ランダム"};
            int mode = int(selection->mode);
            if (ui::PropertyCombo("選別方法", &mode, modes, 5, 1)) {
                selection->mode = geometry::PieceSelectMode(mode);
                changed = true;
                m_pieceSelectionEditing = false;
            }
            if (selection->mode == geometry::PieceSelectMode::Region) {
                changed |= ui::PropertyFloat3Input("範囲 最小", selection->minimum.data(), zero) != 0;
                changed |= ui::PropertyFloat3Input("範囲 最大", selection->maximum.data(), one) != 0;
            }
            if (selection->mode == geometry::PieceSelectMode::Volume) {
                changed |= ui::PropertyFloat("最小体積 (m³)", &selection->minVolume, 0, 1000000, 0);
                changed |= ui::PropertyFloat("最大体積 (m³)", &selection->maxVolume, 0, 1000000, 1000000);
            }
            if (selection->mode == geometry::PieceSelectMode::Random) {
                changed |= ui::PropertyFloat("選択率", &selection->fraction, 0, 1, .5f);
                int seed = int(selection->seed);
                if (ui::PropertyInt("Seed", &seed, 0, 1000000, 1)) {
                    selection->seed = uint32_t(seed);
                    changed = true;
                }
            }
            ui::EndPropertyTable();
        }
        if (selection->mode == geometry::PieceSelectMode::Outer) {
            const char *labels[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
            for (int i = 0; i < 6; ++i) {
                if (i % 2)
                    ImGui::SameLine();
                bool on = (selection->outerFaces & (1u << i)) != 0;
                if (ImGui::Checkbox(labels[i], &on)) {
                    selection->outerFaces ^= 1u << i;
                    changed = true;
                }
            }
            ui::HintText(
                "Boxの指定した外面に接する片を選びます。全6面を選ぶと全片が対象になる場合があります。");
        }
        changed |= ImGui::Checkbox("選択を反転", &selection->invert);
        if (selection->mode == geometry::PieceSelectMode::Manual) {
            ImGui::BeginDisabled(!ready);
            if (ImGui::Checkbox("ビューポートで選択編集", &m_pieceSelectionEditing)) {
                if (m_pieceSelectionEditing)
                    SetPreviewGraphNode(node.id);
            }
            if (ImGui::Button("手動選択をリセット")) {
                selection->ids.clear();
                selection->producer = m_pieceInput->producer;
                selection->generation = m_pieceInput->generation;
                changed = true;
            }
            if (ready && ImGui::TreeNode("ピースID一覧")) {
                for (const auto &p : m_pieceInput->pieces) {
                    bool on =
                        std::find(selection->ids.begin(), selection->ids.end(), p.id) != selection->ids.end();
                    auto label = "ID " + std::to_string(p.id);
                    if (ImGui::Checkbox(label.c_str(), &on)) {
                        if (selection->producer != m_pieceInput->producer ||
                            selection->generation != m_pieceInput->generation)
                            selection->ids.clear();
                        selection->producer = m_pieceInput->producer;
                        selection->generation = m_pieceInput->generation;
                        if (on)
                            selection->ids.push_back(p.id);
                        else
                            std::erase(selection->ids, p.id);
                        changed = true;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::EndDisabled();
            ui::HintText("クリックで選択、Shiftで追加・解除、空白で解除。入力の分割を変更した後はリセットが必"
                         "要です。");
        }
        if (ready) {
            std::string error;
            auto evaluated = geometry::SelectPieces(*m_pieceInput, *selection, error);
            if (error.empty()) {
                ImGui::Text("選択 %zu / %zu", evaluated.ids.size(), m_pieceInput->pieces.size());
                if (!m_pieceInput->pieces.empty() && evaluated.ids.size() == m_pieceInput->pieces.size())
                    ui::HintText("全片を選択しています。Deleteすると空になります。");
            } else
                ui::HintText("%s", error.c_str());
        }
        const auto sourcePin = node.inputs.empty() ? 0 : m_graph.FindUpstreamPin(node.inputs[0].id);
        ImGui::BeginDisabled(!sourcePin);
        bool addKeep = ImGui::Button("Keepを追加");
        ImGui::SameLine();
        bool addDelete = ImGui::Button("Deleteを追加");
        ImGui::EndDisabled();
        if (addKeep || addDelete) {
            auto selectionPin = node.outputs[0].id;
            float x = node.posX + 250, y = node.posY;
            auto id = m_graph.CreateNode(graph::NodeKind::PieceFilter);
            auto *filter = m_graph.FindMutableNode(id);
            std::get<geometry::PieceFilterSettings>(filter->settings).keep = addKeep;
            filter->posX = x;
            filter->posY = y;
            filter->positionValid = true;
            auto piecesPin = filter->inputs[0].id, selectPin = filter->inputs[1].id;
            m_graph.CreateLink(sourcePin, piecesPin);
            m_graph.CreateLink(selectionPin, selectPin);
            m_graphNodesToPlace.push_back(id);
            m_selectedGraphNode = id;
            SetPreviewGraphNode(id);
            MarkDocumentChanged();
            return;
        }
    } else if (auto *filter = std::get_if<geometry::PieceFilterSettings>(&node.settings)) {
        changed |= ImGui::Checkbox("Keep（選択した片だけ残す）", &filter->keep);
        ui::HintText("オフはDelete（選択した片を削除）。PiecesとSelectionは同じ枝から接続してください。");
    } else if (auto *transform = std::get_if<geometry::PieceTransformSettings>(&node.settings)) {
        const auto pose = [&](geometry::PiecePose &p, const char *table) {
            bool edited = false;
            if (ui::BeginPropertyTable(table)) {
                edited |= ui::PropertyFloat3Input("移動 (m)", p.position.data(), zero) != 0;
                edited |= ui::PropertyFloat3Input("回転 (度)", p.rotation.data(), zero) != 0;
                edited |= ui::PropertyFloat3Input("倍率 XYZ", p.scale.data(), one) != 0;
                ui::EndPropertyTable();
            }
            return edited;
        };
        changed |= ImGui::Checkbox("各片の重心を中心に変換", &transform->individual);
        changed |= pose(transform->pose, "pieceGroupPose");
        if (ImGui::Button("全体のギズモ")) {
            m_pieceGizmoId = -1;
            SetPreviewGraphNode(node.id);
        }
        ui::HintText("W:移動 / E:回転 / R:均等倍率。XYZ別倍率は数値欄で調整できます。");
        ui::HintText("Selection未接続なら全片が対象です。オフでは選択全体の体積重心を中心に変換します。");
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("個別変換をリセット")) {
            transform->overrides.clear();
            transform->producer = m_pieceInput->producer;
            transform->generation = m_pieceInput->generation;
            changed = true;
        }
        if (ready && ImGui::TreeNode("IDごとの個別変換")) {
            if (!transform->overrides.empty() && (transform->producer != m_pieceInput->producer ||
                                                  transform->generation != m_pieceInput->generation))
                ui::HintText("分割が変わりました。個別変換をリセットしてください。");
            else
                for (const auto &p : m_pieceInput->pieces) {
                    ImGui::PushID(int(p.id));
                    if (ImGui::TreeNode("pose", "ID %u", p.id)) {
                        auto it = std::find_if(transform->overrides.begin(), transform->overrides.end(),
                                               [&](const auto &v) { return v.id == p.id; });
                        geometry::PiecePose edited =
                            it == transform->overrides.end() ? geometry::PiecePose{} : it->pose;
                        if (ImGui::Button("この片のギズモ")) {
                            if (it == transform->overrides.end()) {
                                transform->overrides.push_back({p.id, {}});
                                transform->producer = m_pieceInput->producer;
                                transform->generation = m_pieceInput->generation;
                                changed = true;
                            }
                            m_pieceGizmoId = int(p.id);
                            SetPreviewGraphNode(node.id);
                            it = std::find_if(transform->overrides.begin(), transform->overrides.end(),
                                              [&](const auto &v) { return v.id == p.id; });
                        }
                        if (pose(edited, "individualPose")) {
                            if (it == transform->overrides.end())
                                transform->overrides.push_back({p.id, edited});
                            else
                                it->pose = edited;
                            transform->producer = m_pieceInput->producer;
                            transform->generation = m_pieceInput->generation;
                            changed = true;
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
    } else
        ui::HintText("ピースの配置を適用して単一Meshにまとめます。内部の接触面は残します。UV Unwrap → "
                     "Material Bakeへ接続できます。");
    if (ready)
        ImGui::TextDisabled("入力: %zu ピース", m_pieceInput->pieces.size());
    if (changed) {
        m_graph.MarkDirty();
        MarkDocumentChanged();
    }
}
void Application::CommitPieceViewportSelection() {
    if (!m_pieceSelectionEditing || m_pieceUpdating || !m_piecePreview ||
        m_previewGraphNode != m_selectedGraphNode)
        return;
    auto *node = m_graph.FindMutableNode(m_selectedGraphNode);
    if (!node)
        return;
    auto *s = std::get_if<geometry::PieceSelectSettings>(&node->settings);
    if (!s || s->mode != geometry::PieceSelectMode::Manual)
        return;
    std::vector<uint32_t> ids;
    for (int index : m_meshHighlight.selected)
        if (index >= 0 && size_t(index) < m_rockMeshReferences.size() &&
            m_rockMeshReferences[index].pieceId >= 0)
            ids.push_back(uint32_t(m_rockMeshReferences[index].pieceId));
    std::sort(ids.begin(), ids.end());
    if (s->invert) {
        std::vector<uint32_t> unselected;
        for (const auto &p : m_piecePreview->pieces)
            if (!std::binary_search(ids.begin(), ids.end(), p.id))
                unselected.push_back(p.id);
        ids = std::move(unselected);
        std::sort(ids.begin(), ids.end());
    }
    if (ids == s->ids && s->producer == m_piecePreview->producer &&
        s->generation == m_piecePreview->generation)
        return;
    s->ids = std::move(ids);
    s->producer = m_piecePreview->producer;
    s->generation = m_piecePreview->generation;
    m_graph.MarkDirty();
    MarkDocumentChanged();
}
} // namespace rock
