#include "app/Application.h"
#include "graph/SurfaceBandGeometry.h"
#include "app/RoadMaskUi.h"
#include "app/ApplicationUiHelpers.h"
#include "graph/SurfaceLayoutEditing.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "ui/UiStyle.h"
#include "core/Log.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <unordered_set>

namespace tg {
namespace {

// 区間タブのバー。区間が増えても横に収まるよう、少ないうちは縮め、多ければスクロールにする。
ImGuiTabBarFlags SpanTabBarFlags(size_t spanCount) {
    return (spanCount > 5 ? ImGuiTabBarFlags_FittingPolicyScroll : ImGuiTabBarFlags_FittingPolicyResizeDown) |
           ImGuiTabBarFlags_NoTooltip;
}

// 区間タブのラベル。距離だけを出し、ID は区間 ID で固定する（分割で範囲が変わっても選択が外れない）。
void SpanTabLabel(char* buffer, size_t size, const char* prefix, const graph::SurfaceSpan& span) {
    std::snprintf(buffer, size, "%.2f～%.2f m###%s%u", span.startMeters, span.endMeters, prefix,
                  static_cast<unsigned>(span.id));
}

}  // namespace

// Road ノードの区間と沿道。共通の切り替えの下に「路面 / 左沿道 / 右沿道」のタブ、
// その中に区間ごとのタブを置く。選んでいる側と区間は m_surfaceBandSide / m_surfaceBandSpan /
// m_surfaceLayoutSpan に写す（横接続の適用側や、分割・削除の対象に使う）。
bool Application::DrawSurfaceLayoutSettings(graph::GraphId roadId) {
    ui::SectionHeader("区間と沿道");
    if (!m_surfacePresetError.empty()) ui::HintText(m_surfacePresetError.c_str());
    if (ui::BeginPropertyTable("surfaceBandPreviewRows", 150)) {
        if (ui::PropertyBool("形状を表示", &m_previewSurfaceBands, false,
            "道路に沿って路肩や歩道を表示する。素材のハイトによる凹凸は「凹凸をなじませる」で有効にする")) m_graph.MarkDirty();
        if (ui::PropertyBool("マテリアルをなじませる", &m_connectSurfaceBands, false,
            "道路と左右の沿道の境界でマテリアルを滑らかに混ぜる。形状表示もオンにする。道路は最大3種類、沿道は左右それぞれ最大2種類のプリセットに対応。横接続は開いている沿道タブの側へ適用する")) {
            if (m_connectSurfaceBands) m_previewSurfaceBands = true;
            m_graph.MarkDirty();
        }
        if (ui::PropertyBool("凹凸をなじませる", &m_displaceConnectedBands, false,
            "素材のハイトによる凹凸を有効にし、境界付近で滑らかに抑える。歩道の段差は保つ。形状表示とマテリアルのなじませもオンにする")) {
            if (m_displaceConnectedBands) { m_connectSurfaceBands = true; m_previewSurfaceBands = true; }
            m_graph.MarkDirty();
        }
        ui::EndPropertyTable();
    }
    if (m_previewSurfaceBands && m_connectSurfaceBands) ui::HintText(m_displaceConnectedBands
        ? "境界付近の凹凸を滑らかに抑え、段差の基準高さへ接続します"
        : "マテリアルの境界をなじませています。素材の凹凸も使う場合は「凹凸をなじませる」をオンにします");

    bool changed = false;
    if (ImGui::BeginTabBar("surfaceSideTabs", ImGuiTabBarFlags_NoTooltip)) {
        if (ImGui::BeginTabItem("路面")) {
            changed |= DrawRoadSpanTab(roadId);
            ImGui::EndTabItem();
        }
        const char* sides[] = {"左沿道", "右沿道"};
        for (int index = 0; index < 2; ++index) {
            if (!ImGui::BeginTabItem(sides[index])) continue;
            if (m_surfaceBandSide != index) { m_surfaceBandSide = index; m_graph.MarkDirty(); }
            changed |= DrawRoadsideTab(roadId, index == 0 ? graph::SurfaceSide::Left : graph::SurfaceSide::Right);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    return changed;
}

// 片側の沿道。無ければ作成の入口、あれば区間ごとのタブ。
// 変更は m_surfaceLayouts の写しへ加え、形状とプリセット共有の検証を通ってから差し替える。
bool Application::DrawRoadsideTab(graph::GraphId roadId, graph::SurfaceSide side) {
    bool exists = false;
    for (const auto& layout : m_surfaceLayouts.layouts) if (layout.roadNode == roadId)
        for (const auto& candidate : layout.bands) if (candidate.side == side && !candidate.spans.empty()) exists = true;
    if (!exists) {
        if (ui::BeginPropertyTable("roadsideCreateRows")) {
            const char* kinds[] = {"路肩", "歩道"};
            ui::PropertyCombo("作る沿道", &m_surfaceBandCreateRole, kinds, 2, 0, "この側の全長を同じ種類で作成。途中の変更は作成後に区間を分割する");
            ui::EndPropertyTable();
        }
        if (ui::Button("全長に作成", ui::kWideButtonWidth)) {
            std::string error;
            if (graph::CreateUniformRoadside(m_surfaceLayouts, m_graph, roadId, side,
                m_surfaceBandCreateRole == 0 ? graph::SurfaceRole::Ground : graph::SurfaceRole::Sidewalk, error)) {
                m_previewSurfaceBands = true;
                return true;
            }
            TG_LOG_ERROR("沿道形状: %s", error.c_str());
        }
        return false;
    }

    auto roadsideEdit = m_surfaceLayouts;
    for (auto& layout : roadsideEdit.layouts) if (layout.roadNode == roadId) {
        for (auto& candidate : layout.bands) if (candidate.side == side && !candidate.spans.empty()) {
            graph::RoadGeometry road; std::string error;
            if (!graph::EvaluateRoad(m_graph, roadId, road, error)) continue;
            const float length = road.rowDistances.back();
            bool rangeChanged = false;
            bool materialChanged = false;
            m_surfaceBandSpan = std::clamp(m_surfaceBandSpan, 0, static_cast<int>(candidate.spans.size()) - 1);
            if (std::abs(candidate.spans.back().endMeters - length) > 0.001f) {
                ui::HintText("沿道の範囲と道路長が異なります。区間の割合を保って合わせます");
                if (ui::Button("道路長に合わせる##roadside", ui::kWideButtonWidth)) rangeChanged = graph::ResizeSurfaceBand(candidate, length);
            }

            // 区間のタブ。開いているタブが編集対象。
            if (ImGui::BeginTabBar("roadsideSpanTabs", SpanTabBarFlags(candidate.spans.size()))) {
                for (size_t i = 0; i < candidate.spans.size(); ++i) {
                    char label[64];
                    SpanTabLabel(label, sizeof(label), "rs", candidate.spans[i]);
                    if (!ImGui::BeginTabItem(label)) continue;
                    m_surfaceBandSpan = static_cast<int>(i);
                    auto& current = candidate.spans[i];
                    if (ui::BeginPropertyTable("roadsideSpanRows")) {
                        ui::PropertyValue("始点", "%.2f m", current.startMeters);
                        if (i + 1 < candidate.spans.size()) {
                            auto& next = candidate.spans[i + 1];
                            if (next.endMeters - current.startMeters > 0.1f && ui::PropertyFloat("切替位置", &current.endMeters,
                                current.startMeters + 0.05f, next.endMeters - 0.05f, (current.startMeters + next.endMeters) * 0.5f,
                                "次の沿道区間との境界。両区間を隙間なく動かす", "%.2f m")) {
                                next.startMeters = current.endMeters; graph::ClampSpanBlends(current); graph::ClampSpanBlends(next); rangeChanged = true;
                            }
                            float transition = std::min(current.blendOutMeters, next.blendInMeters);
                            const float limit = std::min(current.endMeters - current.startMeters, next.endMeters - next.startMeters) * 0.5f;
                            if (limit >= 0.01f && ui::PropertyFloat("移行距離", &transition, 0.01f, limit, std::min(2.0f, limit),
                                "次の区間との境界の前後で形状とマテリアルを変える距離", "%.2f m")) {
                                current.blendOutMeters = next.blendInMeters = transition; rangeChanged = true;
                            }
                        } else ui::PropertyValue("終点", "%.2f m", current.endMeters);
                        ui::EndPropertyTable();
                    }
                    if (ui::Button("沿道区間を分割", ui::kWideButtonWidth)) rangeChanged |= graph::SplitSurfaceSpan(roadsideEdit, candidate, i);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(candidate.spans.size() < 2);
                    if (ui::Button("沿道区間を削除", ui::kWideButtonWidth)) {
                        rangeChanged |= graph::RemoveSurfaceSpan(candidate, i);
                        m_surfaceBandSpan = std::clamp(m_surfaceBandSpan, 0, static_cast<int>(candidate.spans.size()) - 1);
                    }
                    ImGui::EndDisabled();

                    // 分割・削除で区間が入れ替わったフレームは、参照を取り直さずに次フレームへ回す。
                    if (!rangeChanged) {
                        ui::SectionHeader("マテリアルと形状");
                        if (ui::BeginPropertyTable("roadsideMaterialRows")) {
                            auto& span = candidate.spans[i];
                            std::vector<const char*> boundaryNames{"なし"};
                            std::vector<graph::SurfaceId> boundaryIds{0};
                            // サムネールはアセット帯と同じく境界マスク（無ければハイト）の R チャンネル。
                            std::vector<ImTextureID> boundaryThumbnails{0};
                            int boundaryIndex = 0;
                            for (const auto& material : roadsideEdit.boundaryMaterials) {
                                if (material.id == span.boundaryMaterial) boundaryIndex = static_cast<int>(boundaryIds.size());
                                boundaryIds.push_back(material.id); boundaryNames.push_back(material.name.c_str());
                                const auto* texture = m_textureLibrary.Find(material.mask ? material.mask : material.height);
                                boundaryThumbnails.push_back(texture ? static_cast<ImTextureID>(texture->ChannelHandle(0).ptr) : 0);
                            }
                            if (ui::PropertyCombo("境界マテリアル", &boundaryIndex, boundaryNames.data(), static_cast<int>(boundaryNames.size()), 0,
                                "この区間へ適用。なしへの移行も区間の移行距離でなじませる。片側8種類まで", boundaryThumbnails.data())) {
                                span.boundaryMaterial = boundaryIds[boundaryIndex]; materialChanged = true;
                            }
                            if (graph::SurfaceId dropped = 0; AcceptComboDrop(kBoundaryMaterialDragDropType, dropped) &&
                                std::find(boundaryIds.begin(), boundaryIds.end(), dropped) != boundaryIds.end()) {
                                span.boundaryMaterial = dropped; materialChanged = true;
                            }
                            std::vector<graph::SurfaceId> presetIds;
                            std::vector<const char*> presetNames;
                            std::vector<ImTextureID> presetThumbnails;
                            int presetIndex = 0;
                            for (const auto& p : roadsideEdit.layerMaterials) {
                                if (p.id == graph::PresetLayerMaterial(roadsideEdit, span.preset)) presetIndex = static_cast<int>(presetIds.size());
                                presetIds.push_back(p.id); presetNames.push_back(p.name.c_str());
                                const auto thumbnail = std::find_if(m_layerThumbnails.begin(), m_layerThumbnails.end(),
                                    [&](const auto& entry) { return entry.id == p.id; });
                                presetThumbnails.push_back(thumbnail != m_layerThumbnails.end() && thumbnail->ready
                                    ? static_cast<ImTextureID>(thumbnail->texture.srv.gpu.ptr) : 0);
                            }
                            if (!presetIds.empty() && ui::PropertyCombo("マテリアル", &presetIndex, presetNames.data(), static_cast<int>(presetNames.size()), presetIndex,
                                "この区間のレイヤーマテリアル。幅と高さは維持する", presetThumbnails.data())) {
                                materialChanged |= graph::AssignLayerMaterial(roadsideEdit, span, presetIds[presetIndex]);
                            }
                            if (graph::SurfaceId dropped = 0; !presetIds.empty() && AcceptComboDrop(kLayerMaterialDragDropType, dropped) &&
                                std::find(presetIds.begin(), presetIds.end(), dropped) != presetIds.end()) {
                                materialChanged |= graph::AssignLayerMaterial(roadsideEdit, span, dropped);
                            }
                            auto shape = std::find_if(roadsideEdit.presets.begin(), roadsideEdit.presets.end(), [&](const auto& p) { return p.id == span.preset; });
                            float width = 0, height = 0;
                            if (shape != roadsideEdit.presets.end() && graph::GetSimpleRoadsideDimensions(*shape, width, height)) {
                                ui::PropertyValue("形状", "%s", shape->role == graph::SurfaceRole::Sidewalk ? "歩道" : "路肩");
                                const graph::SimpleRoadsideDefaults defaults;
                                bool dimensionsChanged = ui::PropertyFloat("幅", &width, 0.1f, 10, defaults.width,
                                    "この区間だけの幅", "%.2f m");
                                dimensionsChanged |= ui::PropertyFloat(shape->section.size() == 3 ? "段差の高さ" : "外端の高さ", &height,
                                    shape->section.size() == 3 ? 0.01f : -2.0f, 2,
                                    shape->role == graph::SurfaceRole::Sidewalk ? defaults.sidewalkHeight : defaults.groundHeight,
                                    "路面からの高さ。マテリアルを共有する他の区間には影響しない", "%.2f m");
                                if (dimensionsChanged) {
                                    size_t uses = 0;
                                    for (const auto& l : roadsideEdit.layouts) for (const auto& b : l.bands)
                                        for (const auto& s : b.spans) uses += s.preset == span.preset;
                                    if (uses <= 1 || graph::DuplicateSurfacePreset(roadsideEdit, candidate, i)) {
                                        shape = std::find_if(roadsideEdit.presets.begin(), roadsideEdit.presets.end(), [&](const auto& p) { return p.id == span.preset; });
                                        materialChanged |= graph::SetSimpleRoadsideDimensions(*shape, width, height);
                                    }
                                }
                            }
                            ui::EndPropertyTable();
                        }
                        if (ui::Button("マテリアルを編集", ui::kWideButtonWidth)) {
                            m_editSurfacePreset = graph::PresetLayerMaterial(roadsideEdit, candidate.spans[i].preset); m_surfacePresetError.clear();
                        }
                        ImGui::SameLine();
                        if (ui::Button("複製して編集", ui::kWideButtonWidth)) {
                            materialChanged |= graph::DuplicateLayerMaterial(roadsideEdit, candidate.spans[i]);
                            if (materialChanged) { m_editSurfacePreset = graph::PresetLayerMaterial(roadsideEdit, candidate.spans[i].preset); m_surfacePresetError.clear(); }
                        }
                        ui::HintText("マテリアルのレイヤー合成は「マテリアルを編集」で設定します");
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            if (rangeChanged) {
                graph::EnsureRoadsideTransitions(candidate);
                renderer::MeshData mesh;
                if (graph::BuildSurfaceBandGeometry(road, roadsideEdit, candidate, mesh, error)) {
                    m_surfaceLayouts = std::move(roadsideEdit); m_previewSurfaceBands = true; return true;
                }
                TG_LOG_ERROR("沿道区間: %s", error.c_str());
            } else if (materialChanged) {
                graph::EnsureRoadsideTransitions(candidate);
                // 共有プリセットの変更が別の道路・沿道を壊さないことも確認する。
                const auto preset = candidate.spans[static_cast<size_t>(m_surfaceBandSpan)].preset;
                for (const auto& checkLayout : roadsideEdit.layouts) for (const auto& checkBand : checkLayout.bands) {
                    if (checkBand.side == graph::SurfaceSide::Road || checkBand.spans.empty() || !error.empty()) continue;
                    if (std::none_of(checkBand.spans.begin(), checkBand.spans.end(), [&](const auto& checkSpan) {
                        return checkSpan.preset == preset;
                    })) continue;
                    const auto preview = graph::CompileSurfaceBandPreview(m_graph, roadsideEdit, checkLayout.roadNode, checkBand.id);
                    error = preview.error;
                    std::unordered_set<graph::SurfaceId> unique;
                    for (const auto& checkSpan : checkBand.spans) unique.insert(checkSpan.preset);
                    if (error.empty() && m_connectSurfaceBands && unique.size() > 2) error = "横接続中は沿道各2種類までです";
                }
                if (error.empty()) { m_surfaceLayouts = std::move(roadsideEdit); m_previewSurfaceBands = true; return true; }
                m_editSurfacePreset = 0; m_surfacePresetError = error;
                TG_LOG_ERROR("沿道のマテリアルと形状: %s", error.c_str());
            }
            return false;
        }
    }
    return false;
}

// 路面の区間。無ければ区間編集の入口、あれば区間ごとのタブ。
bool Application::DrawRoadSpanTab(graph::GraphId roadId) {
    auto edited = m_surfaceLayouts;
    auto* band = graph::FindRoadBand(edited, roadId);
    bool changed = false;
    if (!band || band->spans.empty()) {
        ui::HintText("道路マテリアルを保存して、区間ごとに切り替える");
        if (ui::Button("区間編集を始める", ui::kWideButtonWidth)) {
            std::string error;
            if (graph::CreateRoadLayout(edited, m_graph, roadId, error)) changed = true;
            else TG_LOG_ERROR("路面区間: %s", error.c_str());
        }
    } else {
        ui::HintText("マテリアルとマスクはレイヤーマテリアルで編集します");
        graph::RoadGeometry road;
        std::string error;
        if (!graph::EvaluateRoad(m_graph, roadId, road, error)) { ui::HintText("先に道路の形状を修正してください"); return false; }
        const float length = road.rowDistances.back();
        if (std::abs(band->spans.back().endMeters - length) > 0.001f) {
            ui::HintText("区間の範囲と道路長が異なります");
            if (ui::Button("道路長に合わせる", ui::kWideButtonWidth)) changed |= graph::ResizeSurfaceBand(*band, length);
        }
        if (ImGui::BeginTabBar("roadSpanTabs", SpanTabBarFlags(band->spans.size()))) {
            for (size_t i = 0; i < band->spans.size(); ++i) {
                char label[64];
                SpanTabLabel(label, sizeof(label), "rd", band->spans[i]);
                if (!ImGui::BeginTabItem(label)) continue;
                const int selected = static_cast<int>(i);
                m_surfaceLayoutSpan = band->spans[i].id;
                auto& span = band->spans[i];
                if (ui::BeginPropertyTable("surfaceSpanRows")) {
                    std::vector<graph::SurfaceId> presetIds;
                    std::vector<const char*> presetNames;
                    std::vector<ImTextureID> presetThumbnails;
                    int presetIndex = 0;
                    for (const auto& preset : edited.layerMaterials) {
                        if (preset.id == graph::PresetLayerMaterial(edited, span.preset)) presetIndex = static_cast<int>(presetIds.size());
                        presetIds.push_back(preset.id); presetNames.push_back(preset.name.c_str());
                        const auto thumbnail = std::find_if(m_layerThumbnails.begin(), m_layerThumbnails.end(),
                            [&](const auto& entry) { return entry.id == preset.id; });
                        presetThumbnails.push_back(thumbnail != m_layerThumbnails.end() && thumbnail->ready
                            ? static_cast<ImTextureID>(thumbnail->texture.srv.gpu.ptr) : 0);
                    }
                    if (ui::PropertyCombo("マテリアル", &presetIndex, presetNames.data(), static_cast<int>(presetNames.size()), presetIndex,
                                          "この区間へ割り当てるマテリアル。1本の道路で同時に3種類まで使用できる", presetThumbnails.data())) {
                        changed |= graph::AssignLayerMaterial(edited, span, presetIds[presetIndex]);
                    }
                    if (graph::SurfaceId dropped = 0; AcceptComboDrop(kLayerMaterialDragDropType, dropped) &&
                        std::find(presetIds.begin(), presetIds.end(), dropped) != presetIds.end()) {
                        changed |= graph::AssignLayerMaterial(edited, span, dropped);
                    }
                    ui::PropertyValue("始点", "%.2f m", span.startMeters);
                    if (i + 1 < band->spans.size() && band->spans[i + 1].endMeters - span.startMeters > 0.02f) {
                        auto& next = band->spans[i + 1];
                        if (ui::PropertyFloat("終点", &span.endMeters, span.startMeters + 0.01f, next.endMeters - 0.01f,
                                              (span.startMeters + next.endMeters) * 0.5f, "次の区間との境界。隙間を作らず一緒に動かす", "%.2f m")) {
                            next.startMeters = span.endMeters; graph::ClampSpanBlends(span); graph::ClampSpanBlends(next); changed = true;
                        }
                    } else ui::PropertyValue("終点", "%.2f m", span.endMeters);
                    const graph::SurfaceSpan defaults;
                    if (selected > 0) changed |= ui::PropertyFloat("始端の移行", &span.blendInMeters, 0, span.endMeters - span.startMeters,
                        defaults.blendInMeters, "前のマテリアルから移る距離。実際の移行は区間長の半分まで", "%.2f m");
                    if (i + 1 < band->spans.size()) changed |= ui::PropertyFloat("終端の移行", &span.blendOutMeters, 0,
                        span.endMeters - span.startMeters, defaults.blendOutMeters, "次のマテリアルへ移る距離。大きいほど緩やかに変わる", "%.2f m");
                    ui::EndPropertyTable();
                }
                if (ui::Button("分割")) changed |= graph::SplitSurfaceSpan(edited, *band, i);
                ImGui::SameLine();
                ImGui::BeginDisabled(band->spans.size() < 2);
                if (ui::Button("削除")) { changed |= graph::RemoveSurfaceSpan(*band, i); m_surfaceLayoutSpan = 0; }
                ImGui::EndDisabled();
                // 構造を変更したフレームは、選択中の参照を次フレームに取り直す。
                if (!changed) {
                    auto preset = std::find_if(edited.presets.begin(), edited.presets.end(), [&](const auto& p) { return p.id == span.preset; });
                    if (preset != edited.presets.end()) {
                        if (ui::Button("マテリアルを編集##road", ui::kWideButtonWidth)) {
                            m_editSurfacePreset = preset->layerMaterial; m_surfacePresetError.clear();
                        }
                        ImGui::SameLine();
                        if (ui::Button("複製して編集##road", ui::kWideButtonWidth)) {
                            changed |= graph::DuplicateLayerMaterial(edited, span);
                            if (changed) { m_editSurfacePreset = graph::PresetLayerMaterial(edited, span.preset); m_surfacePresetError.clear(); }
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (ui::Button("区間編集を解除", ui::kWideButtonWidth)) {
            band->spans.clear(); // 沿道の記述とプリセットは保持する。Roadの元の材質へ戻す。
            changed = true;
        }
    }
    if (changed) {
        std::string error;
        if (graph::ValidateSurfaceLayouts(edited, error)) {
            if (const auto* active = graph::FindRoadBand(edited, roadId); active && !active->spans.empty())
                error = graph::CompileSurfaceLayoutPreview(m_graph, edited, roadId).error;
            if (error.empty()) { m_surfaceLayouts = std::move(edited); return true; }
        }
        m_editSurfacePreset = 0; m_surfacePresetError = error;
        TG_LOG_ERROR("路面区間: %s", error.c_str());
    }
    return false;
}

}  // namespace tg
