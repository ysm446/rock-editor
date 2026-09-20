#include "graph/SurfacePresetGraph.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "graph/RoadMask.h"
#include <algorithm>
#include <cmath>

namespace tg::graph {
CompiledMeshGraph CompileMeshGraphWithLayouts(const NodeGraph& graph, const SurfaceLayoutDocument& sourceDocument,
                                            GraphId previewNodeId) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    auto compiled = CompileMeshGraph(graph, previewNodeId);
    const size_t originalCount = compiled.scene.meshes.size();
    for (size_t i = 0; i < originalCount; ++i) {
        const auto roadId = compiled.meshSources[i];
        if (std::none_of(document.layouts.begin(), document.layouts.end(),
                        [roadId](const auto& layout) {
                            return layout.roadNode == roadId && std::any_of(layout.bands.begin(), layout.bands.end(),
                                [](const auto& band) { return band.side == SurfaceSide::Road && !band.spans.empty(); });
                        })) continue;
        auto layout = CompileSurfaceLayoutPreview(graph, document, roadId);
        if (!layout.error.empty()) {
            if (!compiled.error.empty()) compiled.error += " / ";
            compiled.error += layout.error;
            continue; // 編集中の不完全な配置でも元の道路を表示して修正できるようにする。
        }
        auto mesh = std::move(layout.scene.meshes[0]);
        const int offset = static_cast<int>(compiled.scene.meshes.size()) - 1;
        for (auto& source : mesh.connectionSources) source += offset;
        compiled.scene.meshes[i] = std::move(mesh);
        for (size_t context = 1; context < layout.scene.meshes.size(); ++context) {
            compiled.scene.meshes.push_back(std::move(layout.scene.meshes[context]));
            compiled.meshSources.push_back(0);
        }
    }
    return compiled;
}
namespace {
const SurfacePreset* FindPreset(const SurfaceLayoutDocument& document, SurfaceId id) {
    const auto found = std::find_if(document.presets.begin(), document.presets.end(),
                                  [id](const auto& preset) { return preset.id == id; });
    return found == document.presets.end() ? nullptr : &*found;
}
float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }
}

std::vector<SurfaceSpanSample> SampleSurfaceBand(const SurfaceLayoutDocument& document,
                                                const SurfaceBand& band, float distanceMeters) {
    std::vector<SurfaceSpanSample> result;
    if (!std::isfinite(distanceMeters)) return result;
    const auto append = [&](const SurfaceSpan& span, float weight) {
        if (weight <= 0) return;
        const auto* preset = FindPreset(document, span.preset);
        if (!preset) return;
        SurfaceSpanSample sample{span.id, span.preset, weight, {}};
        const float t = std::clamp((distanceMeters - span.startMeters) / (span.endMeters - span.startMeters), 0.0f, 1.0f);
        for (const auto& parameter : preset->parameters) {
            const auto override = std::find_if(span.parameters.begin(), span.parameters.end(),
                [&](const auto& value) { return value.parameter == parameter.id; });
            sample.parameters.push_back({parameter.id, override == span.parameters.end() ? parameter.defaultValue :
                std::lerp(override->startValue, override->endValue, t)});
        }
        result.push_back(std::move(sample));
    };
    for (size_t i = 1; i < band.spans.size(); ++i) {
        const auto& left = band.spans[i - 1];
        const auto& right = band.spans[i];
        if (left.endMeters != right.startMeters) continue; // 空白を隣の材質で埋めない。
        const float start = left.endMeters - std::min(left.blendOutMeters, (left.endMeters - left.startMeters) * 0.5f);
        const float end = right.startMeters + std::min(right.blendInMeters, (right.endMeters - right.startMeters) * 0.5f);
        if (end <= start || distanceMeters < start || distanceMeters > end) continue;
        const float weight = Smooth((distanceMeters - start) / (end - start));
        append(left, 1 - weight);
        append(right, weight);
        return result;
    }
    for (size_t i = 0; i < band.spans.size(); ++i) {
        const auto& span = band.spans[i];
        if (distanceMeters >= span.startMeters && (distanceMeters < span.endMeters ||
            (i + 1 == band.spans.size() && distanceMeters == span.endMeters))) {
            append(span, 1);
            break;
        }
    }
    return result;
}

CompiledMeshGraph CompileSurfaceLayoutPreview(const NodeGraph& graph, const SurfaceLayoutDocument& sourceDocument,
                                             GraphId roadId) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    CompiledMeshGraph result;
    result.active = true;
    if (!ValidateSurfaceLayouts(document, result.error) || !ValidateSurfaceLayoutRoads(document, graph, result.error)) return result;
    const auto layout = std::find_if(document.layouts.begin(), document.layouts.end(),
                                   [roadId](const auto& value) { return value.roadNode == roadId; });
    if (layout == document.layouts.end()) { result.error = "指定Roadの配置記述がありません"; return result; }
    const auto band = std::find_if(layout->bands.begin(), layout->bands.end(), [](const auto& value) { return value.side == SurfaceSide::Road; });
    RoadGeometry road;
    if (!EvaluateRoad(graph, roadId, road, result.error)) return result;
    const float length = road.rowDistances.back();
    float end = 0;
    std::vector<SurfaceId> presets;
    for (const auto& span : band->spans) {
        if (span.startMeters != end) { result.error = "道路配置プレビューには空白のない区間列が必要です"; return result; }
        end = span.endMeters;
        if (std::find(presets.begin(), presets.end(), span.preset) == presets.end()) presets.push_back(span.preset);
    }
    if (presets.empty() || presets.size() > 3 || std::abs(end - length) > 0.001f) {
        result.error = "道路配置プレビューは全長を覆う最大3種のプリセットに対応します"; return result;
    }
    std::vector<renderer::SceneMesh> contexts;
    for (SurfaceId id : presets) {
        const auto& preset = *FindPreset(document, id);
        renderer::SceneMesh context;
        context.materialOnly = true;
        std::vector<PresetMaterial> materials;
        if (!CompilePresetMaterials(preset, materials, result.error)) return result;
        context.roadMetersPerUv = materials.front().uvRepeatMeters;
        context.roadUvAlongU = road.settings.uvAlongU;
        context.displacementMeters = preset.displacementMeters;
        context.layerBlendRange = preset.layerBlendRange;
        context.roadWidthMeters = road.settings.widthMeters;
        context.roadLengthMeters = length;
        const RoadMaskNodeSettings* masks[3]{};
        for (size_t slot = 0; slot < materials.size(); ++slot) {
            const auto& source = materials[slot];
            context.layerUvRepeat[slot] = source.uvRepeatMeters;
            context.layerWorldUv[slot] = source.worldUv;
            context.layerHeightGate[slot] = source.heightGate;
            context.layerHeightGateThreshold[slot] = source.heightGateThreshold;
            context.layerHeightGateSoftness[slot] = source.heightGateSoftness;
            context.layerBlendMode[slot] = source.blendMode;
            if (slot && !source.mask) continue;
            compositor::MaterialStack stack;
            auto layer = compositor::MaterialStack::MakeBaseLayer();
            layer.name = preset.name;
            layer.material = source.material;
            layer.baseColor = {source.baseColor[0], source.baseColor[1], source.baseColor[2]};
            layer.roughness = source.roughness;
            layer.metallic = source.metallic;
            layer.ambientOcclusion = source.ambientOcclusion;
            layer.heightSource = compositor::ValueSource::Texture;
            layer.heightBase = 0.5f;
            layer.heightGain = 1;
            stack.Layers() = {layer};
            stack.SetTerrainScale(source.uvRepeatMeters, 1);
            if (slot == 0) context.materialStack = std::move(stack);
            else {
                context.layerStacks[slot - 1] = std::move(stack);
                masks[slot - 1] = &*source.mask;
            }
        }
        if (masks[0] || masks[1] || masks[2]) {
            const auto lanes = ComputeRoadLanes(road.settings, graph.RoadNetwork().leftHandTraffic);
            auto mask = BakeRoadMask(masks, road.settings.widthMeters, length, &lanes, &road);
            context.roadMask = {mask.width, mask.height, std::move(mask.rgba)};
        }
        contexts.push_back(std::move(context));
    }
    renderer::SceneMesh mesh;
    mesh.geometry = std::move(road.surface);
    mesh.roadMetersPerUv = road.settings.uvRepeatMeters;
    mesh.roadUvAlongU = road.settings.uvAlongU;
    mesh.roadWidthMeters = road.settings.widthMeters;
    mesh.roadLengthMeters = length;
    mesh.displacementMeters = 1;
    for (size_t i = 0; i < 3; ++i) mesh.connectionSources[i] = static_cast<int>(std::min(i, presets.size() - 1) + 1);
    mesh.roadMask.width = 1;
    mesh.roadMask.height = std::max(16u, static_cast<uint32_t>(std::ceil(length * 64)));
    mesh.roadMask.rgba.resize(size_t(mesh.roadMask.height) * 4);
    for (uint32_t y = 0; y < mesh.roadMask.height; ++y) {
        const float distance = (static_cast<float>(y) + 0.5f) * length / static_cast<float>(mesh.roadMask.height);
        std::array<float, 3> weights{};
        for (const auto& sample : SampleSurfaceBand(document, *band, distance)) {
            const auto index = std::find(presets.begin(), presets.end(), sample.preset) - presets.begin();
            weights[index] += sample.weight;
        }
        auto* pixel = &mesh.roadMask.rgba[size_t(y) * 4];
        pixel[0] = static_cast<uint8_t>(std::lround(weights[1] * 255));
        pixel[1] = static_cast<uint8_t>(std::min(255 - int(pixel[0]), static_cast<int>(std::lround(weights[2] * 255))));
        pixel[2] = 0; pixel[3] = 255;
    }
    result.scene.meshes.push_back(std::move(mesh));
    for (auto& context : contexts) result.scene.meshes.push_back(std::move(context));
    return result;
}
}  // namespace tg::graph
