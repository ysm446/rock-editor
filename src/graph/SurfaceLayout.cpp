#include "graph/SurfaceLayout.h"
#include "graph/SurfacePresetGraph.h"
#include "graph/NodeGraph.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace tg::graph {
SurfacePreset MaterialPreviewPreset(const LayerMaterial& material) {
    SurfacePreset preset;
    preset.id = material.id; preset.name = material.name; preset.role = SurfaceRole::Road;
    preset.materials = material.materials; preset.materialGraph = material.materialGraph;
    preset.displacementMeters = material.displacementMeters; preset.layerBlendRange = material.layerBlendRange;
    return preset;
}
bool ExtractLayerMaterials(SurfaceLayoutDocument& document, std::string& error) {
    auto next = document;
    for (auto& preset : next.presets) {
        if (preset.layerMaterial) continue;
        LayerMaterial material;
        material.id = next.AllocateId();
        if (!material.id) { error = "マテリアルIDを確保できません"; return false; }
        material.name = preset.name;
        material.materials = std::move(preset.materials); material.materialGraph = std::move(preset.materialGraph);
        material.displacementMeters = preset.displacementMeters; material.layerBlendRange = preset.layerBlendRange;
        preset.layerMaterial = material.id;
        preset.materials.clear(); preset.materialGraph.reset();
        preset.displacementMeters = 0; preset.layerBlendRange = 0.2f;
        next.layerMaterials.push_back(std::move(material));
    }
    document = std::move(next); error.clear(); return true;
}
SurfaceLayoutDocument ResolveLayerMaterials(const SurfaceLayoutDocument& document) {
    auto result = document;
    for (auto& preset : result.presets) {
        const auto found = std::find_if(document.layerMaterials.begin(), document.layerMaterials.end(),
            [&](const auto& material) { return material.id == preset.layerMaterial; });
        if (found == document.layerMaterials.end()) continue;
        preset.materials = found->materials; preset.materialGraph = found->materialGraph;
        preset.displacementMeters = found->displacementMeters; preset.layerBlendRange = found->layerBlendRange;
    }
    return result;
}
SurfaceId PresetLayerMaterial(const SurfaceLayoutDocument& document, SurfaceId id) {
    const auto found = std::find_if(document.presets.begin(), document.presets.end(), [&](const auto& p) { return p.id == id; });
    return found == document.presets.end() ? 0 : found->layerMaterial;
}
bool CloneSurfacePresetForSpan(SurfaceLayoutDocument& document, SurfaceSpan& span) {
    const auto found = std::find_if(document.presets.begin(), document.presets.end(), [&](const auto& p) { return p.id == span.preset; });
    if (found == document.presets.end()) return false;
    auto preset = *found;
    const uint64_t needed = 1 + preset.section.size() + preset.parameters.size();
    if (!document.nextId || uint64_t(document.nextId) + needed > std::numeric_limits<SurfaceId>::max()) return false;
    preset.id = document.AllocateId();
    for (auto& point : preset.section) point.id = document.AllocateId();
    for (auto& parameter : preset.parameters) {
        const auto old = parameter.id;
        parameter.id = document.AllocateId();
        for (auto& value : span.parameters) if (value.parameter == old) value.parameter = parameter.id;
    }
    span.preset = preset.id;
    document.presets.push_back(std::move(preset));
    return true;
}
bool AssignLayerMaterial(SurfaceLayoutDocument& document, SurfaceSpan& span, SurfaceId material) {
    const auto found = std::find_if(document.presets.begin(), document.presets.end(), [&](const auto& p) { return p.id == span.preset; });
    if (found == document.presets.end() || found->layerMaterial == material ||
        std::none_of(document.layerMaterials.begin(), document.layerMaterials.end(), [&](const auto& m) { return m.id == material; })) return false;
    // 単独で使う形状はIDを維持する。共有中だけ複製して他区間への変更を防ぐ。
    size_t uses = 0;
    for (const auto& layout : document.layouts) for (const auto& band : layout.bands)
        for (const auto& candidate : band.spans) uses += candidate.preset == span.preset;
    if (uses == 1) { found->layerMaterial = material; return true; }
    if (!CloneSurfacePresetForSpan(document, span)) return false;
    document.presets.back().layerMaterial = material;
    return true;
}
bool ValidateSurfaceLayoutRoads(const SurfaceLayoutDocument& document, const NodeGraph& graph, std::string& error) {
    for (const auto& layout : document.layouts) {
        const auto* road = graph.FindNode(layout.roadNode);
        if (!road || road->kind != NodeKind::Road) { error = "配置先のRoadが存在しません"; return false; }
    }
    error.clear();
    return true;
}

SurfaceId SurfaceLayoutDocument::AllocateId() {
    if (nextId == 0 || nextId == std::numeric_limits<SurfaceId>::max()) return 0;
    return nextId++;
}

bool ValidatePresetMaterial(const PresetMaterial& material, std::string& error) {
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto range = [](float x, float lo, float hi) { return std::isfinite(x) && x >= lo && x <= hi; };
    if (!range(material.uvRepeatMeters, 0.01f, 100) || !range(material.roughness, 0, 1) ||
        !range(material.metallic, 0, 1) || !range(material.ambientOcclusion, 0, 1) ||
        material.blendMode > 1 || material.heightGate > 2 || !range(material.heightGateThreshold, 0, 1) ||
        !range(material.heightGateSoftness, 0.001f, 1)) return fail("マテリアルの寸法・PBR値・合成条件が不正です");
    for (float channel : material.baseColor) if (!range(channel, 0, 1)) return fail("マテリアル色が不正です");
    if (material.mask) {
        const auto& mask = *material.mask;
        if (static_cast<uint32_t>(mask.shape) > 4 || static_cast<uint32_t>(mask.edgeSide) > 2 ||
            !range(mask.laneOffsetMeters, -100, 100) || !range(mask.trackSpacingMeters, 0, 100) ||
            !range(mask.trackWidthMeters, 0, 100) || !range(mask.featherMeters, 0, 100) ||
            !range(mask.edgeWidthMeters, 0, 100) || !range(mask.noiseScaleMeters, 0.05f, 100) ||
            !range(mask.threshold, 0, 1) || !range(mask.softness, 0.0001f, 1) ||
            !range(mask.breakupAmount, 0, 1) || !range(mask.breakupScaleMeters, 0.05f, 100) ||
            !range(mask.strength, 0, 1)) return fail("プリセットの道路マスクが不正です");
    }
    return true;
}

bool ValidateSurfaceLayouts(const SurfaceLayoutDocument& sourceDocument, std::string& error) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto finite = [](float x) { return std::isfinite(x); };
    const auto range = [&](float x, float lo, float hi) { return finite(x) && x >= lo && x <= hi; };
    std::unordered_set<SurfaceId> ids;
    const auto idValid = [&](SurfaceId id) { return id > 0 && id < document.nextId && ids.insert(id).second; };
    if (document.nextId == 0) return fail("配置データの次IDが不正です");
    for (const auto& material : document.boundaryMaterials) {
        if (!idValid(material.id) || material.name.empty() || !range(material.widthMeters, 0.02f, 2) ||
            !range(material.repeatMeters, 0.05f, 50) || !range(material.depthMeters, 0, 0.5f) ||
            !range(material.heightCenter, 0, 1)) return fail("境界マテリアルのID・名前・寸法が不正です");
    }
    for (const auto& material : document.layerMaterials) {
        if (!idValid(material.id) || material.name.empty() || !range(material.displacementMeters, 0, 10) ||
            !range(material.layerBlendRange, 0, 1) || material.materials.empty() || material.materials.size() > 4)
            return fail("レイヤーマテリアルのID・名前・値が不正です");
        for (const auto& layer : material.materials) if (!ValidatePresetMaterial(layer, error)) return false;
        if (material.materialGraph && !ValidatePresetGraph(*material.materialGraph, error)) return false;
    }
    std::unordered_map<SurfaceId, const SurfacePreset*> presets;
    for (const auto& preset : document.presets) {
        if (preset.layerMaterial && std::none_of(document.layerMaterials.begin(), document.layerMaterials.end(),
            [&](const auto& material) { return material.id == preset.layerMaterial; })) return fail("レイヤーマテリアル参照が不正です");
        if (!idValid(preset.id) || preset.version != 1 || preset.name.empty() || static_cast<uint32_t>(preset.role) > 2)
            return fail("プリセットのID・版・名前・役割が不正です");
        if (!range(preset.displacementMeters, 0, 10) || preset.section.size() < 2 || preset.materials.empty() || preset.materials.size() > 4)
            return fail("プリセットの断面・マテリアル数・変位量が不正です");
        for (size_t i = 0; i < preset.section.size(); ++i) {
            const auto& point = preset.section[i];
            if (!idValid(point.id) || !range(point.across, 0, 100) || !range(point.height, -100, 100))
                return fail("断面点が不正です");
            if (i && (point.across < preset.section[i-1].across ||
                (point.across == preset.section[i-1].across && point.height == preset.section[i-1].height)))
                return fail("断面は横方向の順に並べ、同じ点を連続させないでください");
        }
        for (const auto& boundary : preset.boundaries)
            if (static_cast<uint32_t>(boundary.mode) > 2 || !range(boundary.transitionMeters, 0, 50) ||
                !range(boundary.maxHeightAdjustment, 0, 100)) return fail("境界条件が不正です");
        if (!range(preset.layerBlendRange, 0, 1)) return fail("マテリアルのハイト合成幅が不正です");
        for (const auto& material : preset.materials)
            if (!ValidatePresetMaterial(material, error)) return false;
        if (preset.materialGraph && !ValidatePresetGraph(*preset.materialGraph, error)) return false;
        // 下地の合成設定は移動前の値を保持する。評価時は全面の下地として扱う。
        for (const auto& parameter : preset.parameters)
            if (!idValid(parameter.id) || parameter.name.empty() || !finite(parameter.minimum) || !finite(parameter.maximum) ||
                parameter.minimum > parameter.maximum || !range(parameter.defaultValue, parameter.minimum, parameter.maximum))
                return fail("公開パラメータが不正です");
        presets.emplace(preset.id, &preset);
    }
    std::unordered_set<int32_t> roads;
    for (const auto& layout : document.layouts) {
        if (!idValid(layout.id) || layout.roadNode <= 0 || !roads.insert(layout.roadNode).second) return fail("道路配置のID・接続先が不正または重複しています");
        size_t roadBands = 0;
        for (const auto& band : layout.bands) {
            if (!idValid(band.id) || static_cast<uint32_t>(band.side) > 2) return fail("帯のID・側が不正です");
            roadBands += band.side == SurfaceSide::Road ? 1 : 0;
            float previousEnd = 0;
            std::unordered_set<SurfaceId> boundaryIds;
            for (const auto& span : band.spans) {
                if (span.boundaryMaterial) {
                    if (band.side == SurfaceSide::Road || std::none_of(document.boundaryMaterials.begin(), document.boundaryMaterials.end(),
                        [&](const auto& m) { return m.id == span.boundaryMaterial; })) return fail("区間の境界マテリアル参照が不正です");
                    boundaryIds.insert(span.boundaryMaterial);
                    if (boundaryIds.size() > 8) return fail("境界マテリアルは片側につき8種類までです");
                }
                const auto found = presets.find(span.preset);
                if (!idValid(span.id) || found == presets.end()) return fail("区間のID・プリセット参照が不正です");
                if (!range(span.startMeters, 0, 50) || !range(span.endMeters, 0, 50) || span.startMeters >= span.endMeters ||
                    span.startMeters < previousEnd || !range(span.blendInMeters, 0, span.endMeters - span.startMeters) ||
                    !range(span.blendOutMeters, 0, span.endMeters - span.startMeters)) return fail("区間の範囲・並び・移行距離が不正です");
                previousEnd = span.endMeters;
                if ((band.side == SurfaceSide::Road) != (found->second->role == SurfaceRole::Road)) return fail("区間とプリセットの役割が一致しません");
                std::unordered_set<SurfaceId> used;
                for (const auto& value : span.parameters) {
                    const auto& definitions = found->second->parameters;
                    const auto parameter = std::find_if(definitions.begin(), definitions.end(), [&](const auto& p) { return p.id == value.parameter; });
                    if (parameter == definitions.end() || !used.insert(value.parameter).second ||
                        !range(value.startValue, parameter->minimum, parameter->maximum) || !range(value.endValue, parameter->minimum, parameter->maximum))
                        return fail("区間の公開値参照・始終端値が不正です");
                }
            }
        }
        if (roadBands != 1) return fail("道路配置には道路本体の帯が一つ必要です");
    }
    return true;
}
}  // namespace tg::graph
