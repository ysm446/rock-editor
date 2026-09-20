#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"
#include "core/Log.h"
#include <algorithm>
#include <cstdio>

namespace tg {
// 境界マテリアルをシーンから外す。ファイル（.tgboundary）は残す。沿道で使っていれば外さない。
bool Application::RemoveBoundaryMaterialFromScene(graph::SurfaceId id) {
    for (const auto& layout : m_surfaceLayouts.layouts) for (const auto& band : layout.bands)
        for (const auto& span : band.spans)
            if (span.boundaryMaterial == id) {
                TG_LOG_WARN("沿道で使用中の境界マテリアルはシーンから外せません。割り当てを解除してください");
                return false;
            }
    if (!std::erase_if(m_surfaceLayouts.boundaryMaterials, [&](const auto& m) { return m.id == id; })) return false;
    if (m_editBoundaryMaterial == id) m_editBoundaryMaterial = 0;
    MarkDocumentChanged();
    return true;
}

// 境界マテリアルの編集ウィンドウ。一覧はアセットの帯（.tgboundary）にあり、ダブルクリックでここを開く。
void Application::DrawBoundaryMaterialEditor() {
    const auto found = std::find_if(m_surfaceLayouts.boundaryMaterials.begin(), m_surfaceLayouts.boundaryMaterials.end(),
        [&](const auto& m) { return m.id == m_editBoundaryMaterial; });
    if (!m_editBoundaryMaterial || found == m_surfaceLayouts.boundaryMaterials.end()) { m_editBoundaryMaterial = 0; return; }
    auto edited = *found;
    bool open = true, changed = false;
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(540), ui::Scaled(600)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("境界マテリアル編集", &open)) {
        ui::HintText("道路端のマスクと溝を共有します。形状は道路ビューポートで確認できます。");
        if (ui::BeginPropertyTable("boundaryProperties")) {
            const compositor::BoundaryMaterial defaults;
            char name[128]; std::snprintf(name, sizeof(name), "%s", edited.name.c_str());
            // 名前は `.tgboundary` のファイル名と同じ。保存済みなら確定でファイルを改名する。
            if (ui::PropertyTextInputCommit("名前", name, sizeof(name), "アセットのファイル名（拡張子なし）。保存済みならファイルも改名する"))
                changed |= RequestAssetNameChange(edited.assetPath, edited.name, name);
            changed |= DrawTextureSlotRow("境界マスク", edited.mask, m_textureLibrary);
            changed |= DrawTextureSlotRow("ハイト", edited.height, m_textureLibrary);
            changed |= ui::PropertyFloat("境界幅", &edited.widthMeters, 0.02f, 2, defaults.widthMeters, "道路端を中心とする帯の幅", "%.2f m");
            changed |= ui::PropertyFloat("繰り返し長", &edited.repeatMeters, 0.05f, 50, defaults.repeatMeters, "道路に沿う画像の繰り返し間隔", "%.2f m");
            changed |= ui::PropertyFloat("溝の深さ", &edited.depthMeters, 0, 0.5f, defaults.depthMeters, "基準0.5、黒0のときの深さ。凹凸接続オンで形状へ反映", "%.3f m");
            changed |= ui::PropertyFloat("ハイト基準", &edited.heightCenter, 0, 1, defaults.heightCenter, "この値を変位ゼロとする");
            const char* axes[] = {"V方向", "U方向"}; int axis = edited.alongU ? 1 : 0;
            if (ui::PropertyCombo("反復方向", &axis, axes, 2, 0, "画像内で道路に沿って繰り返す軸")) { edited.alongU = axis == 1; changed = true; }
            changed |= ui::PropertyBool("マスクを反転", &edited.invertMask, defaults.invertMask, "通常は白が路面、黒が沿道");
            ui::EndPropertyTable();
        }
        ui::HintText("画像はRチャンネルを使用。段差保持の区間には適用しません。");
        for (const auto id : {edited.mask, edited.height}) {
            if (const auto* texture = m_textureLibrary.Find(id)) {
                ImGui::Image(static_cast<ImTextureID>(texture->ChannelHandle(0).ptr), ImVec2(ui::Scaled(180), ui::Scaled(180)));
                ImGui::SameLine();
            }
        }
        ImGui::NewLine();
    }
    ImGui::End();
    if (!open) m_editBoundaryMaterial = 0;
    if (changed) {
        auto proposed = m_surfaceLayouts;
        const auto target = std::find_if(proposed.boundaryMaterials.begin(), proposed.boundaryMaterials.end(),
            [&](const auto& m) { return m.id == edited.id; });
        *target = edited;
        std::string error;
        if (graph::ValidateSurfaceLayouts(proposed, error)) {
            m_surfaceLayouts = std::move(proposed); m_graph.MarkDirty(); MarkDocumentChanged();
        } else TG_LOG_ERROR("境界マテリアル: %s", error.c_str());
    }
}
}
