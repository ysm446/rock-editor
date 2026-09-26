// アンドゥ用の文書スナップショット。写し取り / 書き戻し / 変更の記録と、
// 参照されなくなったペイントマスクの回収。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace rock {

compositor::TextureId Application::ValidTexture(compositor::TextureId id) const {
    return (m_textureLibrary.Find(id) != nullptr) ? id : compositor::kNoTexture;
}

DocumentSnapshot Application::CaptureDocument() const {
    DocumentSnapshot snapshot;
    snapshot.graphNodes = m_graph.Nodes();
    snapshot.graphLinks = m_graph.Links();
    snapshot.selectedGraphNode = m_selectedGraphNode;
    snapshot.selectedMaterial = m_selectedMaterial;
    snapshot.models = m_models;

    snapshot.materials.reserve(m_materialLibrary.Entries().size());
    for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
        MaterialSnapshot material;
        material.id = asset.id;
        material.name = asset.name;
        material.layerMaterial = asset.layerMaterial;
        material.assetPath = asset.assetPath;
        material.assetUid = asset.assetUid;
        material.baseColor = asset.baseColor;
        material.normal = asset.normal;
        material.roughness = asset.roughness;
        material.metallic = asset.metallic;
        material.ambientOcclusion = asset.ambientOcclusion;
        material.height = asset.height;
        material.opacity = asset.opacity;
        material.opacityValue = asset.opacityValue;
        material.blendMode = asset.blendMode;
        material.maskThreshold = asset.maskThreshold;
        material.baseColorTint = asset.baseColorTint;
        material.hueShiftDegrees = asset.hueShiftDegrees;
        material.saturation = asset.saturation;
        material.brightness = asset.brightness;
        material.flipNormalGreen = asset.flipNormalGreen;
        material.mapUvSets = asset.mapUvSets;
        material.roughnessValue = asset.roughnessValue;
        material.metallicValue = asset.metallicValue;
        material.ambientOcclusionValue = asset.ambientOcclusionValue;
        snapshot.materials.push_back(std::move(material));
    }
    return snapshot;
}

// 写し取った文書を書き戻す。
//
// **参照している ID は、いま実在するものだけ残す。** テクスチャとペイントマスクは
// 履歴の対象外なので、写し取った後に消えていることがある。
// 宙に浮いた ID を残すと、次に同じ番号が払い出されたとき別の画像が現れる。
void Application::ApplyDocument(const DocumentSnapshot& snapshot) {
    // --- マテリアル ---------------------------------------------------------
    // 写し取った時点に無かったものを消す。破棄は GPU 待機を伴う。
    std::vector<compositor::MaterialAssetId> removed;
    for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
        const bool kept = std::any_of(
            snapshot.materials.begin(), snapshot.materials.end(),
            [&asset](const MaterialSnapshot& m) { return m.id == asset.id; });
        if (!kept) {
            removed.push_back(asset.id);
        }
    }
    for (const compositor::MaterialAssetId id : removed) {
        m_materialLibrary.Remove(m_device, id);
    }

    for (const MaterialSnapshot& material : snapshot.materials) {
        // 消えていれば ID を保ったまま作り直す。残っていれば中身を上書きする。
        compositor::MaterialAsset& asset =
            m_materialLibrary.RestoreAsset(material.id, material.name);
        asset.name = material.name;
        asset.layerMaterial = material.layerMaterial;
        // 初回保存で付いた永続 ID は、保存前に作った段へ戻っても保持する。
        if (!material.assetUid.empty() || asset.assetUid.empty()) {
            asset.assetPath = material.assetPath;
            asset.assetUid = material.assetUid;
        }
        asset.baseColor = ValidTexture(material.baseColor);
        asset.normal = ValidTexture(material.normal);
        asset.roughness = material.roughness;
        asset.metallic = material.metallic;
        asset.ambientOcclusion = material.ambientOcclusion;
        asset.height = material.height;
        asset.opacity = material.opacity;
        asset.opacity.texture = ValidTexture(asset.opacity.texture);
        asset.opacityValue = material.opacityValue;
        asset.blendMode = material.blendMode;
        asset.maskThreshold = material.maskThreshold;
        asset.roughness.texture = ValidTexture(asset.roughness.texture);
        asset.metallic.texture = ValidTexture(asset.metallic.texture);
        asset.ambientOcclusion.texture = ValidTexture(asset.ambientOcclusion.texture);
        asset.height.texture = ValidTexture(asset.height.texture);
        asset.baseColorTint = material.baseColorTint;
        asset.hueShiftDegrees = material.hueShiftDegrees;
        asset.saturation = material.saturation;
        asset.brightness = material.brightness;
        asset.flipNormalGreen = material.flipNormalGreen;
        asset.mapUvSets = material.mapUvSets;
        asset.roughnessValue = material.roughnessValue;
        asset.metallicValue = material.metallicValue;
        asset.ambientOcclusionValue = material.ambientOcclusionValue;
        asset.thumbnailDirty = true;
    }

    // --- モデル -------------------------------------------------------------
    // 初回保存で付いた永続 ID は、保存前に作った段へ戻っても保持する（マテリアルと同じ）。
    std::vector<renderer::ModelAsset> models = snapshot.models;
    for (renderer::ModelAsset& model : models) {
        if (model.assetUid.empty()) {
            if (const renderer::ModelAsset* current = FindModel(model.id); current != nullptr) {
                model.assetUid = current->assetUid;
                model.assetPath = current->assetPath;
            }
        }
        for (compositor::MaterialAssetId& slot : model.materials) {
            if (m_materialLibrary.Find(slot) == nullptr) slot = compositor::kNoMaterialAsset;
        }
    }
    m_models = std::move(models);
    m_renderedModelThumbnails.clear();
    m_modelInstanceDrag = {};

    // --- グラフ -------------------------------------------------------------
    std::vector<graph::Node> nodes = snapshot.graphNodes;
    for (graph::Node& node : nodes) {
        if (auto* mask = std::get_if<graph::MaterialMaskSettings>(&node.settings)) mask->texture = ValidTexture(mask->texture);
        graph::VisitNodeMaterialLayers(node, [&](compositor::MaterialLayer& layer) {
            if (m_materialLibrary.Find(layer.material) == nullptr) layer.material = compositor::kNoMaterialAsset;
        });
    }
    // 戻した先で外されていたモデルを指す Model ノードは「なし」にする。
    for (graph::Node& node : nodes) {
        if (auto* model = std::get_if<graph::ModelNodeSettings>(&node.settings); model && !FindModel(model->model))
            model->model = 0;
    }
    m_graph.Replace(std::move(nodes), snapshot.graphLinks);
    m_layerPreviewDirty = true;
    // ノードの位置も一緒に戻すので、エディタへ流し込み直す。視点は動かさない。
    RequestGraphNodePlacement(false);

    m_selectedGraphNode =
        (m_graph.FindNode(snapshot.selectedGraphNode) != nullptr) ? snapshot.selectedGraphNode
                                                                  : 0;
    const auto materialCount = static_cast<int>(m_materialLibrary.Entries().size());
    m_selectedMaterial = std::clamp(snapshot.selectedMaterial, 0, std::max(0, materialCount - 1));
}

void Application::MarkDocumentChanged() {
    ++m_pieceEpoch;
    m_meshGraphRevision = 0; // 材質の変更でもベイクの有効性を再判定する。
    m_layerPreviewDirty = true;
    m_documentDirty = true;
    // マテリアルの変更はモデルの見た目にも効くので、サムネイルを描き直す。
    m_renderedModelThumbnails.clear();
    // マテリアルの編集はグラフの改版に映らないので、シーンの材質を直接再評価させる
    // （グラフ自体の編集は Revision の変化でメッシュシーンが作り直される）。
    m_renderer.InvalidateSceneMaterials();
}

}  // namespace rock
