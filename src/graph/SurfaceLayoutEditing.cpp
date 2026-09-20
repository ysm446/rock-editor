#include "graph/SurfaceLayoutEditing.h"
#include "graph/Road.h"
#include "graph/SurfaceBandGeometry.h"
#include <algorithm>
#include <cmath>

namespace tg::graph {
SurfaceBand* FindRoadBand(SurfaceLayoutDocument& document, GraphId roadId) {
    for (auto& layout : document.layouts) if (layout.roadNode == roadId)
        for (auto& band : layout.bands) if (band.side == SurfaceSide::Road) return &band;
    return nullptr;
}
bool CreateUniformRoadside(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId,
                          SurfaceSide side, SurfaceRole role, std::string& error) {
    if (role != SurfaceRole::Ground && role != SurfaceRole::Sidewalk) { error = "路肩または歩道を選んでください"; return false; }
    auto next = document;
    if (!CreateRoadsideExample(next, graph, roadId, side, error)) return false;
    for (auto& layout : next.layouts) if (layout.roadNode == roadId)
        for (auto& band : layout.bands) if (band.side == side) {
            auto span = role == SurfaceRole::Ground ? band.spans.front() : band.spans.back();
            span.startMeters = 0; span.endMeters = band.spans.back().endMeters;
            span.blendInMeters = span.blendOutMeters = 0;
            band.spans = {span};
        }
    document = std::move(next);
    return true;
}
void ClampSpanBlends(SurfaceSpan& span) {
    const float length = span.endMeters - span.startMeters;
    span.blendInMeters = std::clamp(span.blendInMeters, 0.0f, length);
    span.blendOutMeters = std::clamp(span.blendOutMeters, 0.0f, length);
}
bool CreateRoadLayout(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId, std::string& error) {
    if (const auto* existing = FindRoadBand(document, roadId); existing && !existing->spans.empty()) {
        error = "この道路には既に区間があります"; return false;
    }
    RoadGeometry road;
    if (!EvaluateRoad(graph, roadId, road, error)) return false;
    const float length = road.rowDistances.back();
    if (length < 0.1f || length > 50) { error = "区間編集は長さ0.1〜50 mの道路に対応します"; return false; }
    auto next = document;
    SurfacePreset preset;
    preset.id = next.AllocateId(); preset.name = "道路マテリアル"; preset.role = SurfaceRole::Road;
    preset.displacementMeters = road.settings.displacementMeters;
    preset.layerBlendRange = road.settings.layerBlendRange;
    preset.section = {{next.AllocateId(), 0, 0}, {next.AllocateId(), road.settings.widthMeters, 0}};
    const auto* node = graph.FindNode(roadId);
    std::vector<const Pin*> materials, masks;
    for (const auto& pin : node->inputs) {
        if (pin.valueType == ValueType::Material) materials.push_back(&pin);
        if (pin.valueType == ValueType::RoadMask) masks.push_back(&pin);
    }
    for (size_t slot = 0; slot < 4; ++slot) {
        PresetMaterial material;
        if (!slot) { material.baseColor = {0.18f, 0.18f, 0.18f}; material.roughness = 0.85f; }
        material.uvRepeatMeters = slot ? road.settings.layerUvRepeatMeters[slot] : road.settings.uvRepeatMeters;
        material.worldUv = road.settings.layerWorldUv[slot];
        material.blendMode = slot ? road.settings.layerBlendMode[slot] : 0;
        material.heightGate = slot ? road.settings.layerHeightGate[slot] : 0;
        material.heightGateThreshold = road.settings.layerHeightGateThreshold[slot];
        material.heightGateSoftness = road.settings.layerHeightGateSoftness[slot];
        if (slot < materials.size()) {
            const auto* upstream = graph.FindUpstreamNodeForPin(materials[slot]->id);
            if (upstream) {
                const auto layers = graph.CompileLayersTo(upstream->id).layers;
                if (layers.size() > 1 || (!layers.empty() && layers.back().channelMask != compositor::kAllChannelBits)) {
                    error = "マテリアルの取込は各スロット1層・全チャンネルに対応します"; return false;
                }
                if (!layers.empty()) {
                    const auto& layer = layers.back();
                    material.material = layer.material;
                    material.baseColor = {layer.baseColor.x, layer.baseColor.y, layer.baseColor.z};
                    material.roughness = layer.roughness; material.metallic = layer.metallic;
                    material.ambientOcclusion = layer.ambientOcclusion;
                    if (slot && slot - 1 < masks.size()) {
                        const auto* mask = graph.FindUpstreamNodeForPin(masks[slot - 1]->id);
                        if (mask) if (const auto* settings = std::get_if<RoadMaskNodeSettings>(&mask->settings)) material.mask = *settings;
                    }
                }
            }
        }
        preset.materials.push_back(material);
    }
    SurfaceSpan span;
    span.id = next.AllocateId(); span.preset = preset.id; span.endMeters = length;
    next.presets.push_back(std::move(preset));
    if (auto* existing = FindRoadBand(next, roadId)) {
        existing->spans = {span};
    } else {
        SurfaceBand band;
        band.id = next.AllocateId(); band.side = SurfaceSide::Road;
        band.spans.push_back(span);
        RoadLayout layout;
        layout.id = next.AllocateId(); layout.roadNode = roadId;
        layout.bands.push_back(std::move(band));
        next.layouts.push_back(std::move(layout));
    }
    if (!ValidateSurfaceLayouts(next, error)) return false;
    document = std::move(next);
    return true;
}
bool SplitSurfaceSpan(SurfaceLayoutDocument& document, SurfaceBand& band, size_t index) {
    if (index >= band.spans.size()) return false;
    auto& span = band.spans[index];
    if (span.endMeters - span.startMeters < 0.1f) return false;
    const auto id = document.AllocateId();
    if (!id) return false;
    auto right = span;
    right.id = id;
    right.startMeters = (span.startMeters + span.endMeters) * 0.5f;
    span.endMeters = right.startMeters;
    for (size_t i = 0; i < span.parameters.size(); ++i) {
        const float middle = std::lerp(span.parameters[i].startValue, span.parameters[i].endValue, 0.5f);
        span.parameters[i].endValue = middle; right.parameters[i].startValue = middle;
    }
    span.blendOutMeters = right.blendInMeters = 0;
    ClampSpanBlends(span); ClampSpanBlends(right);
    band.spans.insert(band.spans.begin() + index + 1, std::move(right));
    return true;
}
bool RemoveSurfaceSpan(SurfaceBand& band, size_t index) {
    if (index >= band.spans.size() || band.spans.size() < 2) return false;
    if (index) band.spans[index - 1].endMeters = band.spans[index].endMeters;
    else band.spans[1].startMeters = band.spans[0].startMeters;
    band.spans.erase(band.spans.begin() + index);
    return true;
}
bool ResizeSurfaceBand(SurfaceBand& band, float length) {
    if (band.spans.empty() || !std::isfinite(length) || length < 0.1f || length > 50 || band.spans.back().endMeters <= 0) return false;
    const float ratio = length / band.spans.back().endMeters;
    for (auto& span : band.spans) {
        span.startMeters *= ratio; span.endMeters *= ratio;
        span.blendInMeters *= ratio; span.blendOutMeters *= ratio;
        ClampSpanBlends(span);
    }
    band.spans.back().endMeters = length;
    return true;
}
bool FitSurfaceLayoutsToRoads(SurfaceLayoutDocument& document, const NodeGraph& graph) {
    bool changed = false;
    for (auto& layout : document.layouts) {
        RoadGeometry road; std::string error;
        if (!EvaluateRoad(graph, layout.roadNode, road, error) || road.rowDistances.empty()) continue;
        const float length = road.rowDistances.back();
        for (auto& band : layout.bands) {
            if (band.spans.empty() || std::abs(band.spans.back().endMeters - length) <= 0.00001f) continue;
            changed |= ResizeSurfaceBand(band, length);
        }
    }
    return changed;
}
bool DuplicateLayerMaterial(SurfaceLayoutDocument& document, SurfaceSpan& span) {
    const auto id = PresetLayerMaterial(document, span.preset);
    const auto found = std::find_if(document.layerMaterials.begin(), document.layerMaterials.end(), [&](const auto& m) { return m.id == id; });
    if (found == document.layerMaterials.end()) return false;
    auto material = *found;
    const auto previousNextId = document.nextId;
    material.id = document.AllocateId();
    if (!material.id) return false;
    material.name += " コピー";
    // 複製は別の共有アセットになる（保存時に新しいファイルと ID を持つ）。
    material.assetPath.clear();
    material.assetUid.clear();
    document.layerMaterials.push_back(material);
    if (AssignLayerMaterial(document, span, material.id)) return true;
    document.layerMaterials.pop_back();
    document.nextId = previousNextId;
    return false;
}
bool DuplicateSurfacePreset(SurfaceLayoutDocument& document, SurfaceBand& band, size_t index) {
    if (index >= band.spans.size()) return false;
    if (!CloneSurfacePresetForSpan(document, band.spans[index])) return false;
    document.presets.back().name += " コピー";
    return true;
}
void EnsureRoadsideTransitions(SurfaceBand& band) {
    if (band.side == SurfaceSide::Road) return;
    for (auto& span : band.spans) ClampSpanBlends(span);
    for (size_t i = 1; i < band.spans.size(); ++i) {
        auto& left = band.spans[i - 1]; auto& right = band.spans[i];
        if ((left.preset == right.preset && left.boundaryMaterial == right.boundaryMaterial) ||
            left.blendOutMeters + right.blendInMeters > 0) continue;
        const BoundaryContract defaults;
        left.blendOutMeters = right.blendInMeters = std::min({defaults.transitionMeters,
            (left.endMeters - left.startMeters) * 0.5f, (right.endMeters - right.startMeters) * 0.5f});
    }
}
bool GetSimpleRoadsideDimensions(const SurfacePreset& preset, float& width, float& height) {
    const auto& points = preset.section;
    if (preset.role == SurfaceRole::Road || (points.size() != 2 && points.size() != 3) ||
        points.front().across != 0 || points.front().height != 0 || points.back().across <= 0) return false;
    if (points.size() == 3 && (points[1].across != 0 || points[1].height <= 0 || points[1].height != points[2].height)) return false;
    width = points.back().across; height = points.back().height;
    return std::isfinite(width) && std::isfinite(height);
}
bool SetSimpleRoadsideDimensions(SurfacePreset& preset, float width, float height) {
    float oldWidth, oldHeight;
    if (!GetSimpleRoadsideDimensions(preset, oldWidth, oldHeight) || !std::isfinite(width) || !std::isfinite(height) ||
        width < 0.1f || width > 10 || height < -2 || height > 2 || (preset.section.size() == 3 && height < 0.01f)) return false;
    if (width == oldWidth && height == oldHeight) return false;
    preset.section.back().across = width; preset.section.back().height = height;
    if (preset.section.size() == 3) preset.section[1].height = height;
    return true;
}
}  // namespace tg::graph
