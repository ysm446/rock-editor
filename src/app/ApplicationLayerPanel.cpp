// レイヤー（Surface ノードの設定）のプロパティ行。
// レイヤーパネルは廃止済みで、グラフパネルの下段から使われる。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <cstdio>

namespace rock {

// レイヤー 1 枚ぶんのプロパティ行。グラフパネルの下段から使う。
// 変更の記録（アンドゥ / グラフの再コンパイル）は呼び出し側で行う。
bool Application::DrawLayerSettings(compositor::MaterialLayer& layer) {
    // 既定値マーカーは追加時の初期値と揃える。
    const compositor::MaterialLayer& defaults = kDefaultLayer;
    bool changed = false;

    ui::SectionHeader("基本");
    if (ui::BeginPropertyTable("layerBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", layer.name.c_str());
        if (ui::PropertyTextInput("名前", nameBuffer, sizeof(nameBuffer))) {
            layer.name = nameBuffer;
            // 名前もアンドゥの対象。落とすと、次のアンドゥで改名まで巻き戻る。
            changed = true;
        }
        // マテリアルを割り当てているときは、見た目はマテリアル側の値で決まる。
        // 同じ意味の値を 2 か所に置くと、どちらが効いているのか分からなくなる。
        const bool hasMaterial = (layer.material != compositor::kNoMaterialAsset);
        if (!hasMaterial) {
            changed |= ui::PropertyColorLinear("ベースカラー", &layer.baseColor.x,
                                               &defaults.baseColor.x);
            changed |= ui::PropertyFloat("ラフネス", &layer.roughness, 0.0f, 1.0f,
                                         defaults.roughness, nullptr, "%.2f");
            changed |= ui::PropertyFloat("メタルネス", &layer.metallic, 0.0f, 1.0f,
                                         defaults.metallic, nullptr, "%.2f");
            changed |= ui::PropertyFloat("AO", &layer.ambientOcclusion, 0.0f, 1.0f,
                                         defaults.ambientOcclusion, nullptr, "%.2f");
        }
        // UV スケール（タイル内の反復）は旧地形の合成用。いまはスロットの UV 反復長が決めるので出さない。
        ui::EndPropertyTable();
    }
    if (layer.material != compositor::kNoMaterialAsset) {
        ui::HintText("色とサーフェスの値はマテリアル側で決まる");
    }

    // マテリアルの割り当て。
    ui::SectionHeader("マテリアル");
    if (ui::BeginPropertyTable("layerMaterialRows")) {
        changed |= DrawMaterialSlotRow("マテリアル", layer.material, m_materialLibrary);
        ui::EndPropertyTable();
    }
    if (const compositor::MaterialAsset* material = m_materialLibrary.Find(layer.material);
        material != nullptr && material->thumbnail.IsValid()) {
        ImGui::Image(static_cast<ImTextureID>(material->thumbnail.srv.gpu.ptr),
                     ImVec2(ui::Scaled(72.0f), ui::Scaled(72.0f)));
    } else {
        ui::HintText("マテリアルパネルで作って割り当てる");
    }

    ui::SectionHeader("マッピング");
    if (ui::BeginPropertyTable("surfaceMappingRows")) {
        auto& mapping = layer.mapping;
        int method = static_cast<int>(mapping.method);
        const char* methods[] = {"UV", "Triplanar"};
        if (ui::PropertyCombo("方式", &method, methods, 2, 0)) {
            mapping.method = static_cast<compositor::MappingMethod>(method);
            changed = true;
        }
        if (mapping.method == compositor::MappingMethod::Triplanar) {
            const float zero[3] = {};
            changed |= ui::PropertyFloat("反復サイズ (m)", &mapping.repeatMeters, 0.001f, 10000.0f, 1.0f);
            changed |= ui::PropertyFloat3Input("位置 (m)", &mapping.offset.x, zero) != 0;
            changed |= ui::PropertyFloat3Input("回転 (度)", &mapping.rotationDegrees.x, zero) != 0;
            changed |= ui::PropertyFloat("混合の鋭さ", &mapping.sharpness, 1.0f, 16.0f, 4.0f);
        }
        ui::EndPropertyTable();
    }
    ui::HintText("SurfaceのResultをMesh OutputのMaterialへ接続します。Triplanarはワールド座標で投影します。");
    if (layer.mapping.method == compositor::MappingMethod::UV)
        ui::HintText("UV方式は既存UVを使います。SDFメッシュのUV展開はまだ未対応のため、Triplanarを選んでください。");

    ui::SectionHeader("合成");
    if (ui::BeginPropertyTable("layerBlendRows")) {
        ui::PropertyLabel("書き込み", "このレイヤーが書き込むチャンネル");
        for (uint32_t i = 0; i < IM_ARRAYSIZE(kChannelLabels); ++i) {
            bool enabled = (layer.channelMask & (1u << i)) != 0u;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox(kChannelLabels[i], &enabled)) {
                layer.channelMask = enabled ? (layer.channelMask | (1u << i))
                                            : (layer.channelMask & ~(1u << i));
                changed = true;
            }
            ImGui::PopID();
        }
        ui::PropertyEnd();
        ui::EndPropertyTable();
    }

    return changed;
}

}  // namespace rock
