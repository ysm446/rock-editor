#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"
#include <algorithm>
namespace rock {
void Application::DrawPieceSettings(graph::Node &node) {
    bool changed = false;
    const float zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    const bool hasInput = m_pieceInputNode == node.id && m_pieceInput;
    const bool ready = !m_pieceUpdating && hasInput;
    if (node.kind == graph::NodeKind::PieceSelect) {
        // 非同期評価の開始・終了でスライダーの位置を変えない。折り返さず常に1行を使う。
        ImGui::TextDisabled("%s", m_pieceUpdating ? "更新中…前回の形状を表示" : "設定を変更すると形状に反映します");
    } else if (m_pieceUpdating)
        ui::HintText("更新中…表示は前回の結果です。選択・ベイクは完了後に操作できます。");
    if (auto *layers = std::get_if<geometry::LayeredBoxesSettings>(&node.settings)) {
        if (ui::BeginPropertyTable("layeredBoxes")) {
            const float size[3]={3,.12f,2.4f};
            changed |= ui::PropertyInt("枚数",&layers->count,1,32,5);
            changed |= ui::PropertyFloat3Input("寸法 (m)",layers->size.data(),size,"X/Zが板の幅と奥行き、Yが厚さです。")!=0;
            changed |= ui::PropertyFloat("隙間 (m)",&layers->gap,0,1,.005f);
            changed |= ui::PropertyFloat("厚さのばらつき",&layers->thicknessVariation,0,.8f,.3f);
            changed |= ui::PropertyFloat("広さのばらつき",&layers->sizeVariation,0,.8f,.1f);
            changed |= ui::PropertyFloat("面内のずれ (m)",&layers->offset,0,1,.12f);
            changed |= ui::PropertyFloat3Input("向き (度)",layers->rotation.data(),zero)!=0;
            int seed=int(layers->seed);
            if (ui::PropertyInt("Seed",&seed,0,1000000,1)) {layers->seed=uint32_t(seed);changed=true;}
            ui::EndPropertyTable();
        }
        ui::HintText("板は平行を保ち、下から層0、1…と番号を持ちます。Pieces出力を Scatter Points と Voronoi Fracture の両方へ接続します。");
    } else if (auto *scatter = std::get_if<geometry::ScatterSettings>(&node.settings)) {
        if (ui::BeginPropertyTable("scatter")) {
            changed |= ui::PropertyInt("点数", &scatter->count, 2, geometry::MaxScatterPoints, 24);
            changed |= ui::PropertyBool("面内配置 (XZ)",&scatter->planar,false,
                                       "ローカルYを中央に固定します。Voronoiの方向0・伸長1なら板の厚さを通して分割できます。");
            int seed = int(scatter->seed);
            if (ui::PropertyInt("Seed", &seed, 0, 1000000, 1)) {
                scatter->seed = uint32_t(seed);
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::HintText("閉じた凸形状の内部に点を配置します。同じMesh / PiecesをVoronoi Fractureにも接続してください。Pieces入力では点数は1枚あたり、合計512点までです。");
    } else if (auto *voronoi = std::get_if<geometry::VoronoiSettings>(&node.settings)) {
        if (ui::BeginPropertyTable("voronoi")) {
            changed |= ui::PropertyFloat3Input("方向 (度)", voronoi->rotation.data(), zero) != 0;
            changed |= ui::PropertyFloat3Input("伸長 XYZ", voronoi->stretch.data(), one) != 0;
            ui::EndPropertyTable();
        }
        ui::HintText("Yの伸長を4にすると縦に長い分割片になります。倍率の最大/"
                     "最小比は16以下。最初はノイズ0のBoxを使用してください。");
        ui::SectionHeader("表示");
        if (ui::BeginPropertyTable("voronoiView", 150.0f)) {
            ui::PropertyBool("ワイヤーフレーム", &m_voronoiShowWireframe, false,
                             "このノードを選んでいる間、岩の面を隠して分割片の稜線を表示します。ノードの設定には保存しません。");
            ui::PropertyBool("ポイント", &m_voronoiShowPoints, false,
                             "このノードを選んでいる間、岩の面を隠して Points 入力の点（各分割片の母点）を表示します。"
                             "ノードの設定には保存しません。");
            ui::EndPropertyTable();
        }
    } else if (auto *selection = std::get_if<geometry::PieceSelectSettings>(&node.settings)) {
        if (selection->mode == geometry::PieceSelectMode::Peel) {
            ui::SectionHeader("欠けの進行");
            float progress = selection->fraction * 100.f;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 1.8f));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##peelProgress", &progress, 0, 100, "%.1f %%", ImGuiSliderFlags_AlwaysClamp)) {
                selection->fraction = progress / 100.f;
                changed = true;
            }
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("右へ動かすと外周から欠け、左へ戻すと復元します。Ctrl+クリックで数値を入力できます。");
            if (ImGui::Button("最初に戻す")) { selection->fraction = 0; changed = true; }
            ImGui::SameLine();
            ImGui::TextDisabled("0%% 開始 → 100%% 終点");
            if (ui::BeginPropertyTable("peelProgressSettings")) {
                changed |= ui::PropertyFloat("ばらつき", &selection->peelNoise, 0, 1, .15f,
                    "大きいほど欠ける順序に揺らぎを加えます。0では支持面積だけで決まり、Seedは影響しません。");
                int seed = int(selection->seed);
                if (ui::PropertyInt("Seed", &seed, 0, 1000000, 1,
                                    "同じSeedとばらつきなら同じ順序で欠けます。進行を動かしても引き直しません。")) {
                    selection->seed = uint32_t(seed); changed = true;
                }
                ui::EndPropertyTable();
            }
            ImGui::BeginDisabled(selection->peelNoise == 0);
            if (ImGui::Button("別の欠け方を試す")) {
                selection->seed = (selection->seed + 1) % 1000001;
                changed = true;
            }
            ImGui::EndDisabled();
            ui::HintText("進行は各層の片数を基準にします。中心保護や層の減衰により、100%でも残る片があります。");
        }
        ui::SectionHeader("選択のプレビュー");
        bool viewChanged = false;
        viewChanged |= ImGui::RadioButton("選択状態", &m_pieceSelectView, 0);
        ImGui::SameLine();
        viewChanged |= ImGui::RadioButton("削除後", &m_pieceSelectView, 1);
        if (ui::BeginPropertyTable("pieceSelectView")) {
            viewChanged |= ui::PropertyFloat("板を離して表示 (m)", &m_pieceSelectSpread, 0, 1, 0,
                                            "表示だけ板の間隔を広げます。出力する形状は変わりません。");
            ui::EndPropertyTable();
        }
        if (viewChanged) { ++m_pieceEpoch; SetPreviewGraphNode(node.id); }
        if (m_pieceSelectView == 0) {
            ImGui::TextColored(ImVec4(1,.4f,.15f,1), "□ オレンジの線：欠ける片（Deleteの対象）");
            ImGui::TextColored(ImVec4(1,.85f,.2f,1), selection->mode == geometry::PieceSelectMode::Peel
                ? "■ 外周に露出した次の候補" : "■ 候補のうち今回は選ばれなかった片");
            ImGui::TextColored(ImVec4(.7f,.72f,.75f,1), "■ 対象外の片");
        } else ui::HintText("選択した片を隠した比較表示です。実際の削除には Piece Filter を使います。");
        if (ImGui::Button("外側の板の縁から欠く")) {
            selection->mode = geometry::PieceSelectMode::Rim;
            selection->layer = -1; selection->rimLayers = 1; selection->rimSide = 0;
            selection->rimFalloff = 0; selection->fraction = .75f; selection->invert = false;
            m_pieceSelectionEditing = false; changed = true;
        }
        if (ImGui::Button("外周から内側へ侵食")) {
            selection->mode = geometry::PieceSelectMode::Peel;
            selection->layer = -1; selection->rimLayers = 0; selection->rimSide = 0;
            selection->rimFalloff = .6f; selection->fraction = .3f; selection->invert = false;
            selection->peelNoise = .15f; selection->protectCore = true;
            m_pieceSelectionEditing = false; changed = true;
        }
        if (ui::BeginPropertyTable("pieceSelect")) {
            const char *modes[] = {"Manual — 手動ID", "Outer — 外面に接する片", "Region — 重心の範囲",
                                   "Volume — 体積", "Random — ランダム", "Rim — 元の板の側縁", "Peel — 外周から侵食"};
            int mode = int(selection->mode);
            if (ui::PropertyCombo("選別方法", &mode, modes, 7, 1)) {
                selection->mode = geometry::PieceSelectMode(mode);
                changed = true;
                m_pieceSelectionEditing = false;
            }
            if (selection->mode == geometry::PieceSelectMode::Rim || selection->mode == geometry::PieceSelectMode::Peel) {
                const char* sides[] = {"両側から", "上側から", "下側から"};
                changed |= ui::PropertyCombo("積層の外側", &selection->rimSide, sides, 3, 0);
                changed |= ui::PropertyInt("外側から何層", &selection->rimLayers, 0, 32, 0,
                                           "0は全層。両側・1層なら最上層と最下層が対象です。");
                changed |= ui::PropertyFloat("内側ほど弱く", &selection->rimFalloff, 0, 1, 0,
                                             "対象範囲の内側ほど選択率を下げます。1では最も内側の選択率が0になります。");
            }
            if (selection->mode == geometry::PieceSelectMode::Region) {
                changed |= ui::PropertyFloat3Input("範囲 最小", selection->minimum.data(), zero) != 0;
                changed |= ui::PropertyFloat3Input("範囲 最大", selection->maximum.data(), one) != 0;
            }
            if (selection->mode == geometry::PieceSelectMode::Volume) {
                changed |= ui::PropertyFloat("最小体積 (m³)", &selection->minVolume, 0, 1000000, 0);
                changed |= ui::PropertyFloat("最大体積 (m³)", &selection->maxVolume, 0, 1000000, 1000000);
            }
            changed |= ui::PropertyInt("対象の層",&selection->layer,-1,31,-1,
                                       "-1は全層。Layered Boxesの層番号を指定できます。反転も指定層の中だけに効きます。");
            if (selection->mode == geometry::PieceSelectMode::Peel) {
                changed |= ui::PropertyBool("中心の片を保護", &selection->protectCore, true,
                    "各連結部分で、露出面から最も遠い片を1つ残します。選択反転時は保護対象も選ばれます。");
            }
            if (selection->mode == geometry::PieceSelectMode::Random || selection->mode == geometry::PieceSelectMode::Rim) {
                changed |= ui::PropertyFloat("選択率", &selection->fraction, 0, 1, .5f);
                int seed = int(selection->seed);
                if (ui::PropertyInt("Seed", &seed, 0, 1000000, 1)) {
                    selection->seed = uint32_t(seed);
                    changed = true;
                }
            }
            ui::EndPropertyTable();
        }
        if (selection->mode == geometry::PieceSelectMode::Rim)
            ui::HintText("元の板の側縁に接する片から選択率で選びます。上下面だけに触れる片は選びません。削除後に露出した新しい外周の判定は行いません。");
        if (selection->mode == geometry::PieceSelectMode::Peel)
            ui::HintText("露出した外周から、残る接触面積の割合が小さい片を順に選びます。隣を除くと次の候補が露出します。板同士の支持はまだ計算しません。");
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
                ++m_pieceEpoch;
                if (m_pieceSelectionEditing) { m_pieceSelectView = 0; SetPreviewGraphNode(node.id); }
            }
            if (ImGui::Button("手動選択をリセット")) {
                selection->ids.clear();
                selection->producer = m_pieceInput->producer;
                selection->generation = m_pieceInput->generation;
                changed = true;
            }
            if (hasInput && ImGui::TreeNode("ピースID一覧")) {
                for (const auto &p : m_pieceInput->pieces) {
                    bool on =
                        std::find(selection->ids.begin(), selection->ids.end(), p.id) != selection->ids.end();
                    auto label = "ID " + std::to_string(p.id);
                    if (p.layer>=0) label += " / 層 " + std::to_string(p.layer);
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
            if (m_pieceSelectionEditing) ui::HintText("手動編集中は、選択を解除できるように選択片の面も表示します。");
            ui::HintText("クリックで選択、Shiftで追加・解除、空白で解除。入力の分割を変更した後はリセットが必"
                         "要です。");
        }
        // 上流の入力は更新完了まで保持される。更新中も集計・層一覧を消さず、
        // 下部の高さが縮んでスクロール位置が巻き戻るのを防ぐ。
        if (hasInput) {
            std::string error;
            auto evaluated = geometry::SelectPieces(*m_pieceInput, *selection, error);
            if (error.empty()) {
                ImGui::Text("選択 %zu / 全体 %zu　残る片 %zu", evaluated.ids.size(), m_pieceInput->pieces.size(),
                            m_pieceInput->pieces.size()-evaluated.ids.size());
                auto candidateSettings = *selection;
                candidateSettings.fraction = 1; candidateSettings.rimFalloff = 0; candidateSettings.invert = false;
                auto candidates = geometry::PieceSelection{};
                if (selection->mode == geometry::PieceSelectMode::Peel) candidates.ids = evaluated.frontier;
                else candidates = geometry::SelectPieces(*m_pieceInput, candidateSettings, error);
                if (selection->mode == geometry::PieceSelectMode::Peel) {
                    size_t contacts = 0; double area = 0;
                    for (const auto& p:m_pieceInput->pieces) if (p.neighborhood)
                        for (const auto& edge:p.neighborhood->contacts)
                            if (p.id < edge.neighbor && std::any_of(m_pieceInput->pieces.begin(),m_pieceInput->pieces.end(),
                                [&](const auto& other){return other.id==edge.neighbor;})) {
                                ++contacts; area += geometry::PieceFaceArea(p,edge.areaVector);
                            }
                    ImGui::Text("隣接 %zu 組 / 共有面積 %.3f m²",contacts,area);
                    ImGui::Text("次の候補 %zu 片",evaluated.frontier.size());
                }
                // 上から下への積層図。層の厚さではなく、各層のピース数の内訳を示す。
                bool hasLayers = false;
                for (const auto& p : m_pieceInput->pieces) hasLayers |= p.layer >= 0;
                if (hasLayers && ImGui::TreeNodeEx("層ごとの選択（上 → 下）", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (int layer = 31; layer >= 0; --layer) {
                        size_t total = 0, chosen = 0, possible = 0;
                        for (const auto& p : m_pieceInput->pieces) if (p.layer == layer) {
                            ++total;
                            const bool selected = std::find(evaluated.ids.begin(),evaluated.ids.end(),p.id)!=evaluated.ids.end();
                            if (selected) ++chosen;
                            else if (std::find(candidates.ids.begin(),candidates.ids.end(),p.id)!=candidates.ids.end()) ++possible;
                        }
                        if (!total) continue;
                        ImGui::Text("層 %d：%zu / %zu",layer,chosen,total);
                        const ImVec2 origin = ImGui::GetCursorScreenPos();
                        const float width = ImGui::GetContentRegionAvail().x;
                        const float height = ImGui::GetTextLineHeight()*.55f;
                        auto* draw = ImGui::GetWindowDrawList();
                        draw->AddRectFilled(origin,ImVec2(origin.x+width,origin.y+height),IM_COL32(155,163,173,255));
                        const float selectedWidth = width*float(chosen)/float(total);
                        draw->AddRectFilled(origin,ImVec2(origin.x+selectedWidth,origin.y+height),IM_COL32(255,95,30,255));
                        draw->AddRectFilled(ImVec2(origin.x+selectedWidth,origin.y),
                            ImVec2(origin.x+selectedWidth+width*float(possible)/float(total),origin.y+height),IM_COL32(255,212,40,255));
                        ImGui::Dummy(ImVec2(width,height));
                    }
                    ImGui::TreePop();
                }
                if (!m_pieceInput->pieces.empty() && evaluated.ids.size() == m_pieceInput->pieces.size())
                    ImGui::TextDisabled("全片を選択中：Deleteすると空になります。");
                else ImGui::Dummy(ImVec2(0,ImGui::GetTextLineHeight()));
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
        ui::SectionHeader("表示");
        if (ui::BeginPropertyTable("pieceFilterView", 150.0f)) {
            ui::PropertyBool("除外した片", &m_pieceFilterShowRemoved, false,
                             "このノードを選んでいる間、このノードで除外した分割片をワイヤーフレームで表示します。"
                             "残った片はそのまま面で表示します。ノードの設定には保存しません。");
            ui::EndPropertyTable();
        }
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
    if (hasInput)
        ImGui::TextDisabled("入力: %zu ピース", m_pieceInput->pieces.size());
    if (changed) {
        m_graph.MarkDirty();
        MarkDocumentChanged();
    }
}
void Application::CommitPieceViewportSelection() {
    if (!m_pieceSelectionEditing || m_pieceSelectView != 0 || m_pieceUpdating || !m_piecePreview ||
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
