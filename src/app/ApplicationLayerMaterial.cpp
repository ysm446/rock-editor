// 岩用の共有レイヤーマテリアル。下から順に素材を重ねる。
#include "app/Application.h"
#include "ui/UiStyle.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace rock {
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
    ui::HintText("下地から順に重ねます（最大4層）");
    for (size_t i = 0; i < data.materials.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        auto& layer = data.materials[i];
        changed |= ImGui::Checkbox("##visible", &layer.enabled);
        ImGui::SameLine();
        const auto* source = m_materialLibrary.Find(layer.material);
        const std::string label = (i == 0 ? "下地  " : "層 " + std::to_string(i) + "  ") +
            std::string(source ? source->name : layer.material ? "リンク切れ" : "単色");
        if (ImGui::Selectable(label.c_str(), m_layerEditorSelection == int(i))) m_layerEditorSelection = static_cast<int>(i);
        ImGui::PopID();
    }
    ImGui::BeginDisabled(data.materials.size() >= 4);
    if (ImGui::Button("追加")) {
        data.materials.emplace_back(); data.materials.back().mask.emplace();
        m_layerEditorSelection = static_cast<int>(data.materials.size()) - 1; changed = true;
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(data.materials.size() <= 1);
    if (ImGui::Button("削除")) {
        data.materials.erase(data.materials.begin() + m_layerEditorSelection);
        m_layerEditorSelection = std::min(m_layerEditorSelection, int(data.materials.size()) - 1); changed = true;
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(m_layerEditorSelection <= 0);
    if (ImGui::Button("上へ")) {
        std::swap(data.materials[m_layerEditorSelection], data.materials[m_layerEditorSelection - 1]);
        --m_layerEditorSelection; changed = true;
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(m_layerEditorSelection + 1 >= int(data.materials.size()));
    if (ImGui::Button("下へ")) {
        std::swap(data.materials[m_layerEditorSelection], data.materials[m_layerEditorSelection + 1]);
        ++m_layerEditorSelection; changed = true;
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
                int blend = static_cast<int>(layer.blendMode), gate = static_cast<int>(layer.heightGate);
                if (ImGui::Combo("合成", &blend, "通常\0高さで合成\0")) { layer.blendMode = blend; changed = true; }
                if (ImGui::Combo("高さの条件", &gate, "なし\0高い部分\0低い部分\0")) { layer.heightGate = gate; changed = true; }
                if (gate && ui::BeginPropertyTable("heightRows")) {
                    changed |= ui::PropertyFloat("境界", &layer.heightGateThreshold, 0, 1, .5f, "下地の高さによる被覆の境界");
                    changed |= ui::PropertyFloat("境界のぼかし", &layer.heightGateSoftness, .001f, 1, .2f, "境界の柔らかさ");
                    ui::EndPropertyTable();
                }
                if (ui::BeginPropertyTable("blendWidth")) {
                    changed |= ui::PropertyFloat("高さ合成の幅", &data.layerBlendRange, .001f, 1, .2f, "高さで合成する層の境界幅");
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
