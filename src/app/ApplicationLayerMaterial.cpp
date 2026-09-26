// 岩用の共有レイヤーマテリアル。下から順に素材を重ねる。
#include "app/Application.h"
#include "ui/UiStyle.h"
#include <imgui.h>
#include "app/ApplicationUiHelpers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rock {
namespace {
// レイヤー一覧の行の並べ替え。中身は層の添字（int）。
constexpr const char* kLayerSlotDragDropType = "ROCK_LAYER_SLOT";
}  // namespace
bool Application::DrawLayerMaterialProperties(compositor::MaterialAsset& asset) {
    auto& data = *asset.layerMaterial;
    bool changed = false;
    char name[128];
    std::snprintf(name, sizeof(name), "%s", asset.name.c_str());
    if (ui::BeginPropertyTable("layerMaterialName")) {
        if (ui::PropertyTextInputCommit("名前", name, sizeof(name), "共有アセットの名前"))
            changed |= RequestAssetNameChange(asset.assetPath, asset.name, name);
        ui::EndPropertyTable();
    }
    if (m_layerEditorAsset != asset.id) {
        m_layerEditorAsset = asset.id;
        m_layerEditorSelection = data.materials.size() > 1 ? 1 : 0;
    }
    m_layerEditorSelection = std::clamp(m_layerEditorSelection, 0, std::max(0, int(data.materials.size()) - 1));
    ui::HintText("上の層ほど手前に重なります（最大4層）。行のドラッグで並べ替え、素材のドロップで割り当て");
    // 1行 = 目のアイコン｜素材のサムネイル｜見える範囲の白黒画像｜名前。上の行が上の層。
    const float rowHeight = ui::Scaled(40.0f);
    const float thumbSize = rowHeight - ui::Scaled(4.0f);
    const auto maskHandle = m_materialLibrary.LayerMaskHandle(asset.id);
    int moveFrom = -1, moveTo = -1;
    for (int i = static_cast<int>(data.materials.size()) - 1; i >= 0; --i) {
        auto& layer = data.materials[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const auto* source = m_materialLibrary.Find(layer.material);
        const std::string layerName = source ? source->name : layer.material ? "リンク切れ" : "単色";
        const ImVec2 rowStart = ImGui::GetCursorPos();
        if (ImGui::Selectable("##row", m_layerEditorSelection == i, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0.0f, rowHeight)))
            m_layerEditorSelection = i;
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kLayerSlotDragDropType, &i, sizeof(int));
            ImGui::TextUnformatted(layerName.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kLayerSlotDragDropType)) {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = i;
            }
            // マテリアル一覧から落とした素材をこの層へ割り当てる。入れ子になる合成素材は受けない。
            if (const auto* payload = ImGui::AcceptDragDropPayload(kMaterialDragDropType)) {
                const auto id = *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                const auto* dropped = m_materialLibrary.Find(id);
                if (dropped && !dropped->layerMaterial && !dropped->transient) {
                    layer.material = id;
                    m_layerEditorSelection = i;
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        const float eyeSize = ImGui::GetFrameHeight();
        ImGui::SetCursorPos(ImVec2(rowStart.x + ui::Scaled(4.0f), rowStart.y + (rowHeight - eyeSize) * 0.5f));
        changed |= ui::EyeToggle("##visible", &layer.enabled, eyeSize);
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowStart.y + (rowHeight - thumbSize) * 0.5f);
        if (source) {
            ui::ThumbnailImage(static_cast<ImTextureID>(m_materialLibrary.ThumbnailHandle(layer.material).ptr), thumbSize);
        } else {
            // 単色の層。色はリニアで持つので、表示用に sRGB 相当へ直して塗る。
            const auto display = [&](float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f); };
            ui::ColorSwatch(ImVec4(display(layer.baseColor[0]), display(layer.baseColor[1]), display(layer.baseColor[2]), 1.0f),
                            thumbSize);
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowStart.y + (rowHeight - thumbSize) * 0.5f);
        if (maskHandle.ptr != 0) {
            const float u0 = static_cast<float>(i) / 4.0f;
            ImGui::Image(static_cast<ImTextureID>(maskHandle.ptr), ImVec2(thumbSize, thumbSize), ImVec2(u0, 0.0f),
                         ImVec2(u0 + 0.25f, 1.0f));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("合成後にこの層が見えている範囲（白）");
        } else {
            ImGui::Dummy(ImVec2(thumbSize, thumbSize));
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowStart.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextUnformatted(layerName.c_str());
        if (i == 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("下地");
        }
        // 行の高さを固定する。中の部品の縦位置をずらしても次の行の位置を変えない。
        ImGui::SetCursorPos(ImVec2(rowStart.x, rowStart.y + rowHeight));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
        auto moved = data.materials[static_cast<size_t>(moveFrom)];
        data.materials.erase(data.materials.begin() + moveFrom);
        data.materials.insert(data.materials.begin() + moveTo, std::move(moved));
        // 下地を上へ動かすと被覆が無く消えてしまう。被覆の無い上層は全面を覆う設定にする。
        for (size_t k = 1; k < data.materials.size(); ++k)
            if (!data.materials[k].mask) {
                graph::LayerMaskSettings full;
                full.shape = graph::LayerMaskShape::Constant;
                data.materials[k].mask = full;
            }
        m_layerEditorSelection = moveTo;
        changed = true;
    }
    ImGui::BeginDisabled(data.materials.size() >= 4);
    if (ui::Button("追加")) {
        data.materials.emplace_back();
        data.materials.back().mask.emplace();
        data.materials.back().blendMode = 1;  // 新しい層は高さで合成する。
        m_layerEditorSelection = static_cast<int>(data.materials.size()) - 1;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(data.materials.size() <= 1);
    if (ui::Button("削除")) {
        data.materials.erase(data.materials.begin() + m_layerEditorSelection);
        m_layerEditorSelection = std::min(m_layerEditorSelection, int(data.materials.size()) - 1);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (!data.materials.empty()) {
        const size_t i = static_cast<size_t>(m_layerEditorSelection);
        auto& layer = data.materials[i];
        ImGui::PushID(static_cast<int>(i));
        const auto* source = m_materialLibrary.Find(layer.material);
        if (ui::BeginPropertyTable("layerRows")) {
            ui::PropertyLabel("素材", "通常のPBRマテリアル、または単色を選ぶ");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##source", source ? source->name.c_str() : layer.material ? "リンク切れ" : "単色")) {
                if (ImGui::Selectable("単色", layer.material == 0)) { layer.material = 0; changed = true; }
                for (const auto& entry : m_materialLibrary.Entries()) {
                    if (entry.layerMaterial || entry.transient) continue;
                    ImGui::PushID(static_cast<int>(entry.id));
                    if (ImGui::Selectable(entry.name.c_str(), layer.material == entry.id)) { layer.material = entry.id; changed = true; }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ui::PropertyEnd();
            if (layer.material == 0) {
                const float defaultColor[3] = {.42f, .4f, .36f};
                changed |= ui::PropertyColorLinear("色", layer.baseColor.data(), defaultColor, "この層の色");
                changed |= ui::PropertyFloat("粗さ", &layer.roughness, 0, 1, .8f, "表面の粗さ");
            } else {
                changed |= ui::PropertyFloat("素材サイズ", &layer.uvRepeatMeters, .01f, 8, 2, "素材座標内の繰り返しサイズ");
            }
            ui::EndPropertyTable();
        }
        if (i > 0) {
            // 未接続だったマスクも、操作するまでは保存データを変更しない。
            auto mask = layer.mask.value_or(graph::LayerMaskSettings{});
            if (!layer.mask) mask.strength = 0;
            int mode = mask.shape == graph::LayerMaskShape::Noise ? 1 : 0;
            if (ui::BeginPropertyTable("maskRows")) {
                const char* modes[] = {"均一", "ムラ"};
                if (ui::PropertyCombo("被覆方式", &mode, modes, 2, 1)) {
                    mask.shape = mode ? graph::LayerMaskShape::Noise : graph::LayerMaskShape::Constant; changed = true;
                }
                changed |= ui::PropertyFloat("被覆量", &mask.strength, 0, 1, 1, "この層を重ねる強さ");
                int blend = layer.blendMode != 0 ? 1 : 0;
                const char* blends[] = {"通常", "高さ"};
                if (ui::PropertyCombo("合成", &blend, blends, 2, 0,
                                      "高さ：下の結果よりハイトが高い所から、この層が現れる。被覆量を下げると高い所だけに残る")) {
                    layer.blendMode = static_cast<uint32_t>(blend);
                    changed = true;
                }
                if (blend)
                    changed |= ui::PropertyFloat("境界の幅", &data.layerBlendRange, .001f, 1, .2f,
                                                 "高さで合成する境界の柔らかさ（全層で共通）");
                if (mode) {
                    changed |= ui::PropertyFloat("ムラのサイズ", &mask.noiseScaleMeters, .05f, 8, 1, "素材座標内のムラの大きさ");
                    changed |= ui::PropertyFloat("しきい値", &mask.threshold, 0, 1, .5f, "高くすると被覆範囲が狭まる");
                    changed |= ui::PropertyFloat("ぼかし", &mask.softness, .001f, 1, .2f, "ムラの境界の柔らかさ");
                }
                ui::EndPropertyTable();
            }
            if (mode && ui::BeginPropertyTable("seedRows")) {
                int seed = static_cast<int>(mask.seed);
                if (ui::PropertyInt("Seed", &seed, 0, 1000000, 1, "同じ値なら同じムラ")) { mask.seed = static_cast<uint32_t>(seed); changed = true; }
                changed |= ui::PropertyBool("反転", &mask.invert, false);
                ui::EndPropertyTable();
            }
            if (ImGui::TreeNode("詳細")) {
                int gate = static_cast<int>(layer.heightGate);
                if (ui::BeginPropertyTable("heightRows")) {
                    const char* gates[] = {"なし", "高い部分", "低い部分"};
                    if (ui::PropertyCombo("高さの条件", &gate, gates, 3, 0, "下地の高さで被覆を絞る")) {
                        layer.heightGate = static_cast<uint32_t>(gate);
                        changed = true;
                    }
                    if (gate) {
                        changed |= ui::PropertyFloat("境界", &layer.heightGateThreshold, 0, 1, .5f, "下地の高さによる被覆の境界");
                        changed |= ui::PropertyFloat("境界のぼかし", &layer.heightGateSoftness, .001f, 1, .2f, "境界の柔らかさ");
                    }
                    ui::EndPropertyTable();
                }
                ImGui::TreePop();
            }
            if (changed) layer.mask = mask;
        }
        ImGui::PopID();
    }
    // 診断は常に一行分。文言の長さでパラメータを動かさない。
    ImGui::BeginChild("layerStatus", ImVec2(0, ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextUnformatted(asset.layerError.empty() ? " " : asset.layerError.c_str());
    ImGui::EndChild();
    if (changed) { data.name = asset.name; m_pendingAssetsSave = true; }
    return changed;
}
}  // namespace rock
