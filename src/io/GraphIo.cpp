#include "io/GraphIo.h"

#include "io/JsonUtil.h"
#include "io/PieceSettings.h"

#include "graph/NodeParams.h"

#include "core/Log.h"
#include "core/PathUtf8.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace rock::io {
using namespace detail;
namespace {

// --- レイヤー -------------------------------------------------------------

json WriteChannelMask(uint32_t channelMask) {
    json channels = json::array();
    for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::Channel::Count); ++i) {
        if ((channelMask & (1u << i)) != 0) {
            channels.push_back(kChannelNames[i]);
        }
    }
    return channels;
}

uint32_t ReadChannelMask(const json& node, const char* key, uint32_t fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_array()) {
        return fallback;
    }
    uint32_t mask = 0;
    for (const json& element : *member) {
        if (!element.is_string()) {
            continue;
        }
        const std::string text = element.get<std::string>();
        for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::Channel::Count); ++i) {
            if (text == kChannelNames[i]) {
                mask |= (1u << i);
            }
        }
    }
    return mask;
}

// パス。座標は position:[X,Y,Z]（m）。worldSpace は旧ビルドとの互換のために常に真で書く
// （旧地形 UV のパスは読まない）。
json WriteLayer(const compositor::MaterialLayer& layer,
                const std::function<json(compositor::MaterialAssetId)>& writeMaterial) {
    json node;
    node["name"] = layer.name;
    node["enabled"] = layer.enabled;
    node["channels"] = WriteChannelMask(layer.channelMask);
    node["material"] = writeMaterial(layer.material);

    // マテリアルを割り当てているレイヤーでは使われない値だが、
    // 「なし」へ戻したときに元の値が消えていると驚くので、そのまま持ち回る。
    node["baseColor"] = WriteFloat3(layer.baseColor);
    node["roughness"] = layer.roughness;
    node["metallic"] = layer.metallic;
    node["ambientOcclusion"] = layer.ambientOcclusion;

    json height;
    height["source"] = EnumName(kValueSourceNames, static_cast<uint32_t>(layer.heightSource));
    height["base"] = layer.heightBase;
    height["gain"] = layer.heightGain;
    height["noise"] = WriteNoise(layer.heightNoise);
    node["height"] = std::move(height);

    node["uvScale"] = layer.uvScale;
    node["mapping"] = {{"method", layer.mapping.method == compositor::MappingMethod::Triplanar ? "triplanar" : "uv"},
                       {"repeatMeters", layer.mapping.repeatMeters}, {"offset", WriteFloat3(layer.mapping.offset)},
                       {"rotationDegrees", WriteFloat3(layer.mapping.rotationDegrees)}, {"sharpness", layer.mapping.sharpness}};
    return node;
}

// 旧地形の種類（kind）・マスク・加工の節は読まない（撤去済み。キーが残っていても無視する）。
compositor::MaterialLayer ReadLayer(
    const json& node,
    const std::function<compositor::MaterialAssetId(const json&)>& readMaterial) {
    const compositor::MaterialLayer defaults;
    compositor::MaterialLayer layer;
    layer.name = ReadString(node, "name", defaults.name);
    layer.enabled = ReadBool(node, "enabled", defaults.enabled);
    layer.channelMask = ReadChannelMask(node, "channels", defaults.channelMask);
    const json* material = FindMember(node, "material");
    layer.material =
        (material != nullptr) ? readMaterial(*material) : compositor::kNoMaterialAsset;

    layer.baseColor = ReadFloat3(node, "baseColor", defaults.baseColor);
    layer.roughness = ReadFloat(node, "roughness", defaults.roughness);
    layer.metallic = ReadFloat(node, "metallic", defaults.metallic);
    layer.ambientOcclusion = ReadFloat(node, "ambientOcclusion", defaults.ambientOcclusion);

    if (const json* height = FindMember(node, "height");
        height != nullptr && height->is_object()) {
        layer.heightSource = static_cast<compositor::ValueSource>(EnumValue(
            kValueSourceNames, *height, "source", static_cast<uint32_t>(defaults.heightSource)));
        layer.heightBase = ReadFloat(*height, "base", defaults.heightBase);
        layer.heightNoise = ReadNoise(*height, "noise", defaults.heightNoise);

        if (FindMember(*height, "gain") != nullptr) {
            layer.heightGain = ReadFloat(*height, "gain", defaults.heightGain);
        } else {
            // gain を分離する前の形式。起伏の強さはノイズの amount が兼ねていて、
            // 式は h = base + src * amount だった。基準面を挟む式へ寄せる。
            //
            //   base + src * gain == base' + (src - kHeightPivot) * gain
            //   ただし base' = base + kHeightPivot * gain
            //
            // これは近似ではなく厳密に同じ値になる。定数は src の項がないので触らない。
            layer.heightGain = layer.heightNoise.amount;
            if (layer.heightSource != compositor::ValueSource::Constant) {
                layer.heightBase += compositor::kHeightPivot * layer.heightGain;
            }
        }
    }

    layer.uvScale = ReadFloat(node, "uvScale", defaults.uvScale);
    if (const json* mapping = FindMember(node, "mapping"); mapping && mapping->is_object()) {
        layer.mapping.method = ReadString(*mapping, "method", "uv") == "triplanar"
            ? compositor::MappingMethod::Triplanar : compositor::MappingMethod::UV;
        layer.mapping.repeatMeters = std::clamp(ReadFloat(*mapping, "repeatMeters", 1.0f), 0.001f, 10000.0f);
        layer.mapping.offset = ReadFloat3(*mapping, "offset", {0, 0, 0});
        layer.mapping.rotationDegrees = ReadFloat3(*mapping, "rotationDegrees", {0, 0, 0});
        layer.mapping.sharpness = std::clamp(ReadFloat(*mapping, "sharpness", 4.0f), 1.0f, 16.0f);
    }
    return layer;
}

// Replace が捨てたリンクに、捨てた理由を付ける。規則は NodeGraph::Replace と同じ
// （ピンが無い・向きが逆・入力に 2 本目・型が合わない・循環）。
std::string DescribePin(const graph::NodeGraph& graphData, const graph::Pin& pin) {
    const graph::Node* owner = graphData.FindNode(pin.nodeId);
    const graph::NodeDefinition* definition = owner ? graph::FindNodeDefinition(owner->kind) : nullptr;
    return std::string(definition ? definition->name : "?") + "#" + std::to_string(pin.nodeId) + " の" +
           (pin.kind == graph::PinKind::Input ? "入力" : "出力") + "「" + pin.label + "」";
}

void ReportDroppedLinks(const graph::NodeGraph& graphData, const std::vector<graph::Link>& requested,
                        std::vector<GraphReadIssue>& issues) {
    std::unordered_set<graph::GraphId> kept;
    for (const graph::Link& link : graphData.Links()) kept.insert(link.id);
    for (const graph::Link& link : requested) {
        if (kept.count(link.id) != 0) continue;
        const graph::Pin* start = graphData.FindPin(link.startPin);
        const graph::Pin* end = graphData.FindPin(link.endPin);
        std::string reason;
        if (start == nullptr) {
            reason = "start のピン " + std::to_string(link.startPin) + " がありません";
        } else if (end == nullptr) {
            reason = "end のピン " + std::to_string(link.endPin) + " がありません";
        } else if (start->kind != graph::PinKind::Output) {
            reason = "start が出力ではありません（" + DescribePin(graphData, *start) + "）";
        } else if (end->kind != graph::PinKind::Input) {
            reason = "end が入力ではありません（" + DescribePin(graphData, *end) + "）";
        } else if (graphData.FindUpstreamPin(link.endPin) != 0) {
            reason = DescribePin(graphData, *end) + " には既に別のリンクがあります";
        } else {
            reason = DescribePin(graphData, *start) + " を " + DescribePin(graphData, *end) +
                     " へ繋げません（型が合わないか、循環になります）";
        }
        issues.push_back({end ? end->nodeId : 0, link.id, "リンクを捨てました: " + reason});
    }
}

}  // namespace

// --- ノードグラフ ---------------------------------------------------------
//
// ノードの kind は enum の数値ではなく定義テーブルの名前で書く
// （「列挙は名前で書く」）。ピンはノードの定義から再生成するので、
// ファイルには ID の並びだけを持つ（リンクがピン ID を参照するため）。

json WriteGraph(const graph::NodeGraph& graphData, const MaterialWriter& writeMaterial,
                const ModelWriter& writeModel, const TextureWriter& writeTexture, const fs::path& baseDir) {
    json out;
    json nodes = json::array();
    for (const graph::Node& node : graphData.Nodes()) {
        const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
        if (definition == nullptr) {
            continue;
        }
        json item;
        item["id"] = node.id;
        item["kind"] = definition->name;
        item["position"] = json::array({node.posX, node.posY});
        if (!node.note.empty()) item["note"] = node.note;
        json inputs = json::array();
        for (const graph::Pin& pin : node.inputs) {
            inputs.push_back(pin.id);
        }
        item["inputs"] = std::move(inputs);
        json outputs = json::array();
        for (const graph::Pin& pin : node.outputs) {
            outputs.push_back(pin.id);
        }
        item["outputs"] = std::move(outputs);
        if (graph::IsPieceNodeKind(node.kind)) {
            item["pieces"] = WritePieceSettings(node);
        } else if (const auto* boxes = std::get_if<geometry::BoxClusterSettings>(&node.settings)) {
            item["randomBoxes"] = {{"count", boxes->count}, {"size", boxes->size},
                                   {"sizeVariation", boxes->sizeVariation}, {"spread", boxes->spread},
                                   {"rotation", boxes->rotation}, {"seed", boxes->seed}};
        } else if (const auto* volume = std::get_if<geometry::VolumeSettings>(&node.settings)) {
            item["toVolume"] = {{"resolution", volume->resolution}};
        } else if (const auto* subdivide = std::get_if<geometry::SubdivideSettings>(&node.settings)) {
            item["subdivide"] = {{"levels",subdivide->levels},{"threshold",subdivide->threshold}};
        } else if (const auto* displace = std::get_if<geometry::DisplaceSettings>(&node.settings)) {
            item["displace"] = {{"amount",displace->amount},{"midpoint",displace->midpoint}};
        } else if (const auto* remesh = std::get_if<geometry::RemeshSettings>(&node.settings)) {
            item["remesh"] = {{"edgeLength", remesh->edgeLength}, {"iterations", remesh->iterations}, {"featureAngle", remesh->featureAngle}};
        } else if (const auto* decimate = std::get_if<geometry::DecimateSettings>(&node.settings)) {
            item["decimate"] = {{"targetTriangles", decimate->targetTriangles},
                                {"maxError", decimate->maxError},
                                {"creaseWeight", decimate->creaseWeight}};
        } else if (const auto* rockNode = std::get_if<graph::RockNodeSettings>(&node.settings)) {
            // 岩グラフはシーンからの相対パスで書く。
            item["rock"] = {{"scene", rockNode->scene.empty() ? std::string() : RelativePathString(FromUtf8(rockNode->scene), baseDir)},
                            {"scale", rockNode->scale}, {"weight", rockNode->weight}};
        } else if (const auto* scatter = std::get_if<geometry::RockScatterSettings>(&node.settings)) {
            item["rockScatter"] = {{"seed", scatter->seed}, {"spacing", scatter->spacing}, {"maxCount", scatter->maxCount},
                                   {"scaleMin", scatter->scaleMin}, {"scaleMax", scatter->scaleMax},
                                   {"alignToNormal", scatter->alignToNormal}, {"embed", scatter->embed}};
        } else if (const auto* terrain = std::get_if<geometry::HeightmapSettings>(&node.settings)) {
            // 画像はシーンからの相対パスで書く（ルートごと動かしても読めるように）。
            item["heightmap"] = {{"source", geometry::HeightmapSourceName(terrain->source)},
                                 {"image", terrain->image.empty() ? std::string() : RelativePathString(FromUtf8(terrain->image), baseDir)},
                                 {"width", terrain->width}, {"depth", terrain->depth},
                                 {"minHeight", terrain->minHeight}, {"maxHeight", terrain->maxHeight},
                                 {"resolution", terrain->resolution}, {"textureResolution", terrain->textureResolution},
                                 {"seed", terrain->seed}, {"featureSize", terrain->featureSize},
                                 {"roughness", terrain->roughness}, {"peak", terrain->peak}};
        } else if (const auto* asset = std::get_if<graph::RockAssetSettings>(&node.settings)) {
            item["rockAsset"] = {{"lodCount", asset->lodCount}, {"maxTriangles", asset->maxTriangles},
                                 {"trianglePercent", asset->trianglePercent}, {"screenSize", asset->screenSize},
                                 {"shareUv", asset->shareUv}};
        } else if (const auto* uv = std::get_if<geometry::UvUnwrapSettings>(&node.settings)) {
            item["uvUnwrap"] = {{"resolution", uv->resolution}, {"padding", uv->padding}, {"quality", uv->quality}};
        } else if (const auto* mask = std::get_if<graph::MaterialMaskSettings>(&node.settings)) {
            item["materialMask"] = {{"texture", writeTexture(mask->texture)}, {"value", mask->value},
                {"repeatMeters", mask->repeatMeters}, {"invert", mask->invert}, {"triplanar", mask->triplanar}};
        } else if (const auto* apply = std::get_if<graph::ApplyMaterialSettings>(&node.settings)) {
            item["applyMaterial"] = {{"heightBlend", apply->heightBlend}, {"heightBlendRange", apply->heightBlendRange},
                                     {"opacity", apply->opacity}};
        } else if (const auto* deposition = std::get_if<geometry::DepositionMaskSettings>(&node.settings)) {
            item["depositionMask"] = {{"amount", deposition->amount}, {"distance", deposition->distance},
                {"maxSlopeDegrees", deposition->maxSlopeDegrees}, {"recessPreference", deposition->recessPreference},
                {"resolution", deposition->resolution}, {"samples", deposition->samples}, {"invert", deposition->invert}};
        } else if (const auto* structure = std::get_if<geometry::StructureMaskSettings>(&node.settings)) {
            item["structureMask"] = {{"type", geometry::StructureMaskTypeName(structure->type)}, {"resolution", structure->resolution},
                                     {"fill", structure->fill}, {"softness", structure->softness}, {"scale", structure->scale},
                                     {"width", structure->width}, {"warp", structure->warp}, {"warpScale", structure->warpScale},
                                     {"seed", structure->seed}, {"invert", structure->invert}};
        } else if (const auto* noiseMask = std::get_if<geometry::NoiseMaskSettings>(&node.settings)) {
            item["noiseMask"] = {{"size", noiseMask->size}, {"contrast", noiseMask->contrast}, {"seed", noiseMask->seed},
                {"detail", noiseMask->detail}, {"warp", noiseMask->warp}, {"resolution", noiseMask->resolution}, {"invert", noiseMask->invert}};
        } else if (const auto* shape = std::get_if<geometry::ShapeMaskSettings>(&node.settings)) {
            const char* type = shape->type == geometry::ShapeMaskType::Direction ? "direction"
                             : shape->type == geometry::ShapeMaskType::Height  ? "height"
                             : shape->type == geometry::ShapeMaskType::ValleyCurvature ? "valleyCurvature"
                             : shape->type == geometry::ShapeMaskType::RidgeCurvature ? "ridgeCurvature" : "occlusion";
            item["shapeMask"] = {{"type", type}, {"resolution", shape->resolution}, {"low", shape->low}, {"high", shape->high}, {"gamma", shape->gamma},
                {"invert", shape->invert}, {"distance", shape->distance}, {"samples", shape->samples}};
        } else if (const auto* combine = std::get_if<geometry::MaskCombineSettings>(&node.settings)) {
            static constexpr const char* kOperations[] = {"multiply", "maximum", "minimum", "subtract", "mix"};
            const auto index = std::min<uint32_t>(static_cast<uint32_t>(combine->operation), 4);
            item["maskCombine"] = {{"operation", kOperations[index]}, {"mix", combine->mix}, {"low", combine->low},
                {"high", combine->high}, {"gamma", combine->gamma}, {"invert", combine->invert}};
        } else if (const auto* filter = std::get_if<geometry::MaskFilterSettings>(&node.settings)) {
            static constexpr const char* kTypes[] = {"blur", "sharpen", "levels"};
            const auto index = std::min<uint32_t>(static_cast<uint32_t>(filter->type), 2);
            item["maskFilter"] = {{"type", kTypes[index]}, {"radius", filter->radius}, {"amount", filter->amount},
                {"inputLow", filter->inputLow}, {"inputHigh", filter->inputHigh}, {"gamma", filter->gamma},
                {"outputLow", filter->outputLow}, {"outputHigh", filter->outputHigh}, {"invert", filter->invert}};
        } else if (const auto* bake = std::get_if<graph::MaterialBakeSettings>(&node.settings)) {
            // ベイク結果は一時的なもので、保存しない。開き直したら未ベイクへ戻る。指紋だけ残すと、結果が無いのに
            // 「ベイク済み」と判定されるので、材質を書けないときは指紋も書かない。
            // 旧版が Bakes/ へ保存した結果（一時でない材質）は、従来どおり書く。
            const bool keepBake = !writeMaterial(bake->bakedLayer.material).is_null();
            item["materialBake"] = {{"geometryAo", bake->geometryAo}, {"aoDistance", bake->aoDistance}, {"aoStrength", bake->aoStrength}, {"aoSamples", bake->aoSamples},
                                    {"cageDistance", bake->cageDistance}};
            if (keepBake) {
                item["materialBake"]["layer"] = WriteLayer(bake->bakedLayer, writeMaterial);
                item["materialBake"]["fingerprint"] = bake->fingerprint;
            }
        } else if (const auto* meshing = std::get_if<geometry::VolumeToMeshSettings>(&node.settings)) {
            item["volumeToMesh"] = {{"method", meshing->method == geometry::VolumeMeshingMethod::DualContouring
                ? "dualContouring" : "marchingTetrahedra"}};
        } else if (const auto* smooth = std::get_if<geometry::VolumeSmoothSettings>(&node.settings)) {
            item["volumeSmooth"] = {{"mode", geometry::VolumeSmoothModeName(smooth->mode)},
                                    {"radius", smooth->radius},
                                    {"amount", smooth->amount},
                                    {"upwardFocus", smooth->upwardFocus}, {"focusDirection", smooth->focusDirection}};
        } else if (const auto* wear = std::get_if<geometry::VolumeEdgeWearSettings>(&node.settings)) {
            item["volumeEdgeWear"] = {{"radius", wear->radius}, {"amount", wear->amount}, {"noise", wear->noise},
                                      {"noiseScale", wear->noiseScale}, {"upwardFocus", wear->upwardFocus}, {"seed", wear->seed},
                                      {"focusDirection", wear->focusDirection}};
        } else if (const auto* clip = std::get_if<geometry::VolumeClipSettings>(&node.settings)) {
            item["volumeClip"] = {{"mode", geometry::VolumeClipModeName(clip->mode)}, {"height", clip->height},
                                  {"invert", clip->invert}, {"embed", clip->embed}};
        } else if (const auto* spread = std::get_if<geometry::VolumeScatterSettings>(&node.settings)) {
            item["volumeScatter"] = {{"shape", geometry::VolumeScatterShapeName(spread->shape)},
                                     {"operation", geometry::VolumeScatterOperationName(spread->operation)},
                                     {"count", spread->count}, {"radiusMin", spread->radiusMin}, {"radiusMax", spread->radiusMax},
                                     {"depthMin", spread->depthMin}, {"depthMax", spread->depthMax},
                                     {"elongation", spread->elongation}, {"blend", spread->blend}, {"seed", spread->seed}};
        } else if (const auto* undercut = std::get_if<geometry::VolumeUndercutSettings>(&node.settings)) {
            item["volumeUndercut"] = {{"height", undercut->height}, {"width", undercut->width}, {"depth", undercut->depth},
                                      {"count", undercut->count}, {"spacing", undercut->spacing}, {"noise", undercut->noise},
                                      {"noiseScale", undercut->noiseScale}, {"seed", undercut->seed}};
        } else if (const auto* close = std::get_if<geometry::VolumeCloseSettings>(&node.settings)) {
            item["volumeClose"] = {{"mode", geometry::VolumeCloseModeName(close->mode)}, {"width", close->width},
                                   {"distance", close->distance}, {"threshold", close->threshold},
                                   {"samples", close->samples}, {"softness", close->softness}};
        } else if (const auto* terrace = std::get_if<geometry::VolumeTerraceSettings>(&node.settings)) {
            item["volumeTerrace"] = {{"step", terrace->step},
                                     {"depth", terrace->depth},
                                     {"ratio", terrace->ratio},
                                     {"softness", terrace->softness},
                                     {"variation", terrace->variation},
                                     {"noise", terrace->noise},
                                     {"noiseScale", terrace->noiseScale},
                                     {"rotation", terrace->rotationDegrees},
                                     {"seed", terrace->seed}};
        } else if (const auto* noise = std::get_if<geometry::VolumeNoiseSettings>(&node.settings)) {
            item["volumeNoise"] = {{"type", geometry::VolumeNoiseTypeName(noise->type)},
                                   {"amount", noise->amount},
                                   {"scale", noise->scale},
                                   {"octaves", noise->octaves},
                                   {"warp", noise->warp},
                                   {"warpScale", noise->warpScale},
                                   {"seed", noise->seed}};
        } else if (const auto* planes = std::get_if<geometry::ParallelPlanesSettings>(&node.settings)) {
            item["parallelPlanes"] = {{"rotation", planes->rotationDegrees}, {"spacing", planes->spacing},
                                       {"offset", planes->offset}, {"variation", planes->variation}, {"seed", planes->seed}};
        } else if (const auto* crack = std::get_if<geometry::VolumeCrackSettings>(&node.settings)) {
            item["volumeCrack"] = {{"width", crack->width},       {"depth", crack->depth},
                                   {"variation", crack->variation}, {"noise", crack->noise},
                                   {"noiseScale", crack->noiseScale}, {"seed", crack->seed},
                                   {"source", geometry::VolumeCrackSourceName(crack->source)},
                                   {"shellSpacing", crack->shellSpacing}, {"shellCount", crack->shellCount},
                                   {"shellSmoothing", crack->shellSmoothing}, {"shellPeel", crack->shellPeel}};
        } else if (const auto* cuts = std::get_if<geometry::PlaneCutsSettings>(&node.settings)) {
            item["planeCuts"] = {{"count", cuts->count},
                                 {"scope", geometry::PlaneCutsScopeName(cuts->scope)},
                                 {"radius", cuts->radius},
                                 {"seed", cuts->seed},
                                 {"depthMin", cuts->depthMin},
                                 {"depthMax", cuts->depthMax},
                                 {"distribution", geometry::PlaneCutsDistributionName(cuts->distribution)},
                                 {"systems", cuts->systems},
                                 {"rotation", cuts->rotationDegrees},
                                 {"spread", cuts->spreadDegrees},
                                 {"blend", cuts->blend}, {"curvature", cuts->curvature}};
        } else if (const auto* boolean = std::get_if<geometry::VolumeBooleanSettings>(&node.settings)) {
            item["volumeBoolean"] = {{"operation", geometry::VolumeBooleanOperationName(boolean->operation)},
                                     {"blend", boolean->blend}};
        } else if (const auto* moved = std::get_if<geometry::VolumeTransformSettings>(&node.settings)) {
            item["volumeTransform"] = {{"position", moved->position},
                                       {"rotation", moved->rotationDegrees},
                                       {"scale", moved->scale}};
        } else if (const auto* rock = std::get_if<graph::BaseRockNodeSettings>(&node.settings)) {
            item["baseRock"] = {{"size", rock->size},
                                {"seed", rock->seed},
                                {"shape", geometry::BaseShapeName(rock->shape)},
                                {"subdivisions", rock->subdivisions},
                                {"roundness", rock->roundness},
                                {"noiseStrength", rock->noiseStrength},
                                {"noiseScale", rock->noiseScale}};
        } else if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
            item["layer"] = WriteLayer(settings->layer, writeMaterial);
        } else if (const auto* model = std::get_if<graph::ModelNodeSettings>(&node.settings)) {
            item["model"] = {{"model", writeModel ? writeModel(model->model) : json()},
                             {"position", json::array({model->position[0], model->position[1], model->position[2]})},
                             {"rotation", json::array({model->rotationDegrees[0], model->rotationDegrees[1],
                                                       model->rotationDegrees[2]})},
                             {"scale", model->scale}};
            // FBX のノードに足す回転。無ければ書かない（読むと空）。
            if (!model->nodeRotations.empty()) {
                json rotations = json::array();
                for (const auto& rotation : model->nodeRotations)
                    rotations.push_back({{"node", rotation.node},
                                         {"rotation", json::array({rotation.rotationDegrees[0], rotation.rotationDegrees[1],
                                                                   rotation.rotationDegrees[2]})}});
                item["model"]["nodeRotations"] = std::move(rotations);
            }
        } else if (const auto* transform = std::get_if<graph::TransformNodeSettings>(&node.settings)) {
            item["transform"] = {
                {"position", json::array({transform->position[0], transform->position[1], transform->position[2]})},
                {"rotation", json::array({transform->rotationDegrees[0], transform->rotationDegrees[1],
                                          transform->rotationDegrees[2]})},
                {"scale", transform->scale}};
        }
        nodes.push_back(std::move(item));
    }
    out["nodes"] = std::move(nodes);

    json links = json::array();
    for (const graph::Link& link : graphData.Links()) {
        json item;
        item["id"] = link.id;
        item["start"] = link.startPin;
        item["end"] = link.endPin;
        links.push_back(std::move(item));
    }
    out["links"] = std::move(links);
    return out;
}

json WriteDefaultNodeSettings(graph::NodeKind kind) {
    graph::NodeGraph single;
    const graph::GraphId id = single.CreateNode(kind);
    if (!single.FindNode(id)) return json::object();
    const json written = WriteGraph(
        single, [](compositor::MaterialAssetId material) { return material ? json(material) : json(nullptr); },
        [](uint64_t) { return json(nullptr); }, [](compositor::TextureId) { return json(nullptr); }, fs::path());
    json settings = json::object();
    if (const json* nodes = FindMember(written, "nodes"); nodes && nodes->is_array() && !nodes->empty()) {
        for (const auto& [key, value] : nodes->front().items())
            if (key != "id" && key != "kind" && key != "inputs" && key != "outputs" && key != "position")
                settings[key] = value;
    }
    return settings;
}

namespace {

// 保存処理が条件つきで書くキー（既定値のノードには現れない）。読み込みの診断で「知らないキー」にしない。
bool IsConditionalKey(const std::string& path) {
    return path == "note" || path == "materialBake.layer" || path == "materialBake.fingerprint" ||
           path == "model.nodeRotations";
}

// ファイルのノードにある設定のキーを、既定値のノードを書いた JSON と比べる。無いキーは読み込みで無視される。
void ReportUnknownKeys(const json& item, const json& defaults, const std::string& prefix, graph::GraphId node,
                       std::vector<GraphReadIssue>& issues) {
    for (const auto& [key, value] : item.items()) {
        if (prefix.empty() && (key == "id" || key == "kind" || key == "inputs" || key == "outputs" || key == "position"))
            continue;
        const std::string path = prefix.empty() ? key : prefix + "." + key;
        if (IsConditionalKey(path)) continue;
        const auto known = defaults.find(key);
        if (known == defaults.end()) {
            issues.push_back({node, 0, "知らない設定のキーです（無視されます）: " + path});
        } else if (value.is_object() && known->is_object()) {
            ReportUnknownKeys(value, *known, path, node, issues);
        }
    }
}

json* FindMutablePath(json& root, const std::string& path) {
    json* current = &root;
    for (size_t start = 0;;) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!current->is_object()) return nullptr;
        const auto found = current->find(key);
        if (found == current->end()) return nullptr;
        current = &*found;
        if (dot == std::string::npos) return current;
        start = dot + 1;
    }
}

// 数値の列挙（Piece Select の mode など）を名前でも書けるようにする。項目表の候補の名前を数値へ写す。
json NormalizeEnumNames(const json& item, graph::NodeKind kind, graph::GraphId id, std::vector<GraphReadIssue>* issues) {
    json normalized = item;
    for (const graph::ParamDefinition& param : graph::NodeParams()) {
        if (param.kind != kind || param.type != graph::ParamType::IntEnum) continue;
        json* value = FindMutablePath(normalized, param.path);
        if (!value || !value->is_string()) continue;
        const std::string name = value->get<std::string>();
        bool found = false;
        for (const graph::ParamOption& option : param.options) {
            if (name == option.name) {
                *value = option.value;
                found = true;
            }
        }
        if (!found && issues) {
            std::string names;
            for (const graph::ParamOption& option : param.options) names += (names.empty() ? "" : " / ") + std::string(option.name);
            issues->push_back({id, 0, std::string(graph::FindNodeDefinition(kind)->name) + "#" + std::to_string(id) + " の " +
                                          param.path + " = \"" + name + "\" は候補にありません（" + names + "）"});
        }
    }
    return normalized;
}

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// 書きやすい表記のリンクの端。"12"、"12:Volume"、"12:2"、{"node": 12, "pin": "Volume"}（pin は名前か番号）。
// 名前は大文字小文字を区別しない。pin を省くと 0 番。
graph::GraphId ResolveEnd(const graph::NodeGraph& graphData, const json& end, graph::PinKind kind, std::string& error) {
    graph::GraphId nodeId = 0;
    std::string pin;
    if (end.is_number_integer()) {
        nodeId = end.get<graph::GraphId>();
    } else if (end.is_string()) {
        const std::string text = end.get<std::string>();
        const size_t colon = text.find(':');
        try {
            nodeId = std::stoi(text.substr(0, colon));
        } catch (...) {
            error = "\"" + text + "\" はノード ID で始まりません";
            return 0;
        }
        if (colon != std::string::npos) pin = text.substr(colon + 1);
    } else if (end.is_object()) {
        nodeId = ReadInt(end, "node", 0);
        if (const json* p = FindMember(end, "pin"))
            pin = p->is_number_integer() ? std::to_string(p->get<int>()) : p->is_string() ? p->get<std::string>() : "";
    }
    const char* side = kind == graph::PinKind::Input ? "入力" : "出力";
    const graph::Node* owner = graphData.FindNode(nodeId);
    if (!owner) {
        error = std::string(side) + "側のノード " + std::to_string(nodeId) + " がありません";
        return 0;
    }
    const auto& pins = kind == graph::PinKind::Input ? owner->inputs : owner->outputs;
    const std::string ownerName = std::string(graph::FindNodeDefinition(owner->kind)->name) + "#" + std::to_string(nodeId);
    if (pins.empty()) {
        error = ownerName + " には" + side + "がありません";
        return 0;
    }
    if (pin.empty()) return pins.front().id;
    if (std::all_of(pin.begin(), pin.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        const size_t index = std::stoul(pin);
        if (index < pins.size()) return pins[index].id;
        error = ownerName + " の" + side + " " + pin + " 番がありません";
        return 0;
    }
    for (const graph::Pin& candidate : pins)
        if (Lower(candidate.label) == Lower(pin)) return candidate.id;
    std::string names;
    for (const graph::Pin& candidate : pins) names += (names.empty() ? "" : " / ") + candidate.label;
    error = ownerName + " の" + side + "に \"" + pin + "\" がありません（" + names + "）";
    return 0;
}

// 位置の無いノードを、上流からの深さで列に並べる（LLM が位置を書かなくてもエディタで重ならない）。
void LayoutUnplacedNodes(graph::NodeGraph& graphData) {
    std::map<graph::GraphId, int> depth;
    bool any = false;
    for (const graph::Node& node : graphData.Nodes()) {
        depth[node.id] = 0;
        any |= !node.positionValid;
    }
    if (!any) return;
    for (size_t pass = 0; pass < graphData.Nodes().size(); ++pass) {
        bool changed = false;
        for (const graph::Link& link : graphData.Links()) {
            const graph::Pin* start = graphData.FindPin(link.startPin);
            const graph::Pin* end = graphData.FindPin(link.endPin);
            if (!start || !end) continue;
            if (depth[end->nodeId] < depth[start->nodeId] + 1) {
                depth[end->nodeId] = depth[start->nodeId] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    std::map<int, int> rows;
    for (graph::Node& node : graphData.MutableNodes()) {
        if (node.positionValid) continue;
        const int column = depth[node.id];
        node.posX = float(column * 320);
        node.posY = float(rows[column]++ * 220);
        node.positionValid = true;
    }
}

}  // namespace

bool ReadGraph(const json& node, graph::NodeGraph& graphData, const MaterialReader& readMaterial,
               const ModelReader& readModel, const TextureReader& readTexture, const fs::path& baseDir,
               std::vector<GraphReadIssue>* issues) {
    std::vector<graph::Node> nodes;
    std::vector<graph::Link> links;
    graph::GraphId maxId = 0;

    // **リンクの ID を先に見ておく。** ノードの種類にピンを足した後で古いファイルを
    // 開くと、足りないピンへ「いまの最大 + 1」を振ることになる。リンクを読む前に
    // 振ると、既にあるリンクの ID とぶつかる（ノード / ピン / リンクは同じ ID 空間）。
    if (const json* items = FindMember(node, "links"); items != nullptr && items->is_array()) {
        for (const json& item : *items) {
            if (!item.is_object()) {
                continue;
            }
            maxId = std::max(maxId, static_cast<graph::GraphId>(ReadInt(item, "id", 0)));
            maxId = std::max(maxId, static_cast<graph::GraphId>(ReadInt(item, "start", 0)));
            maxId = std::max(maxId, static_cast<graph::GraphId>(ReadInt(item, "end", 0)));
        }
    }

    if (const json* items = FindMember(node, "nodes"); items != nullptr && items->is_array()) {
        for (const json& rawItem : *items) {
            if (!rawItem.is_object()) {
                continue;
            }
            const graph::NodeDefinition* definition =
                graph::FindNodeDefinitionByName(ReadString(rawItem, "kind"));
            const json item =
                definition ? NormalizeEnumNames(rawItem, definition->kind, ReadInt(rawItem, "id", 0), issues) : rawItem;
            const int id = ReadInt(item, "id", 0);
            if (definition == nullptr || id <= 0) {
                // 知らない種類は捨てる（将来のビルドで増えた種類を古いビルドで開いた場合）。
                if (issues != nullptr) {
                    issues->push_back({std::max(id, 0), 0,
                                       definition == nullptr
                                           ? "知らない種類のノードを捨てました: \"" + ReadString(item, "kind") + "\""
                                           : std::string("ID が無い（0 以下の）ノードを捨てました")});
                }
                continue;
            }
            graph::Node created;
            created.id = id;
            created.kind = definition->kind;
            maxId = std::max(maxId, created.id);
            if (const json* position = FindMember(item, "position");
                position != nullptr && position->is_array() && position->size() >= 2 &&
                (*position)[0].is_number() && (*position)[1].is_number()) {
                created.posX = (*position)[0].get<float>();
                created.posY = (*position)[1].get<float>();
                // 壊れた座標（非有限・極端に大きい値）は「位置なし」へ落とす。
                // エディタへ流し込むとキャンバスの座標計算が壊れるため、
                // パネル側がビュー中央へ置き直す。
                created.positionValid = std::isfinite(created.posX) &&
                                        std::isfinite(created.posY) &&
                                        std::abs(created.posX) <= 1.0e6f &&
                                        std::abs(created.posY) <= 1.0e6f;
            }
            created.note = ReadString(item, "note", "");
            if (issues != nullptr) ReportUnknownKeys(item, WriteDefaultNodeSettings(created.kind), "", created.id, *issues);

            // ピンは定義から再生成し、ID だけファイルの値を使う。
            // 欠けているぶんは後で maxId から振り直す（リンクは繋がらないまま消える）。
            const json* inputIds = FindMember(item, "inputs");
            const json* outputIds = FindMember(item, "outputs");
            size_t inputIndex = 0;
            size_t outputIndex = 0;
            for (const graph::PinDefinition& pin : definition->pins) {
                graph::Pin createdPin;
                createdPin.nodeId = created.id;
                createdPin.kind = pin.kind;
                createdPin.valueType = pin.valueType;
                createdPin.label = pin.label;
                const json* ids = (pin.kind == graph::PinKind::Input) ? inputIds : outputIds;
                size_t& index = (pin.kind == graph::PinKind::Input) ? inputIndex : outputIndex;
                if (ids != nullptr && ids->is_array() && index < ids->size() &&
                    (*ids)[index].is_number_integer()) {
                    createdPin.id = (*ids)[index].get<int>();
                }
                ++index;
                maxId = std::max(maxId, createdPin.id);
                if (pin.kind == graph::PinKind::Input) {
                    created.inputs.push_back(std::move(createdPin));
                } else {
                    created.outputs.push_back(std::move(createdPin));
                }
            }
            // 入力数が可変のノード（Merge）は、ファイルにある分だけ入力を足す。
            // 型とラベルは最後の入力定義に合わせ、並びは読み込み後に NormalizeVariablePins が整える。
            if (graph::IsVariableInputNodeKind(created.kind) && inputIds != nullptr && inputIds->is_array() &&
                !created.inputs.empty()) {
                for (; inputIndex < inputIds->size(); ++inputIndex) {
                    if (!(*inputIds)[inputIndex].is_number_integer()) continue;
                    graph::Pin extra = created.inputs.back();
                    const int64_t storedId = (*inputIds)[inputIndex].get<int64_t>();
                    // 範囲外は 0 にして、下で新しい番号を振る。
                    extra.id = (storedId > 0 && storedId <= std::numeric_limits<int>::max())
                                   ? static_cast<graph::GraphId>(storedId)
                                   : 0;
                    extra.label = "Input " + std::to_string(created.inputs.size() + 1);
                    maxId = std::max(maxId, extra.id);
                    created.inputs.push_back(std::move(extra));
                }
            }

            if (graph::IsPieceNodeKind(created.kind)) {
                const json* v = FindMember(item, "pieces");
                ReadPieceSettings(created, v && v->is_object() ? *v : json::object());
            } else if (created.kind == graph::NodeKind::Subdivide) {
                geometry::SubdivideSettings settings;
                if (const json* v = FindMember(item, "subdivide"); v && v->is_object()) { settings.levels=ReadInt(*v,"levels",1); settings.threshold=std::clamp(ReadFloat(*v,"threshold",.5f),0.f,1.f); }
                created.settings=settings;
            } else if (created.kind == graph::NodeKind::Displace) {
                geometry::DisplaceSettings settings;
                if (const json* v = FindMember(item, "displace"); v && v->is_object()) {
                    settings.amount=ReadFloat(*v,"amount",.05f); settings.midpoint=ReadFloat(*v,"midpoint",.5f);
                }
                created.settings=settings;
            } else if (created.kind == graph::NodeKind::Decimate) {
                geometry::DecimateSettings settings;
                if (const json* v = FindMember(item, "decimate"); v && v->is_object()) {
                    settings.targetTriangles = ReadInt(*v, "targetTriangles", settings.targetTriangles);
                    settings.maxError = ReadFloat(*v, "maxError", settings.maxError);
                    settings.creaseWeight = ReadFloat(*v, "creaseWeight", settings.creaseWeight);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Rock) {
                graph::RockNodeSettings settings;
                if (const json* v = FindMember(item, "rock"); v && v->is_object()) {
                    const std::string scene = ReadString(*v, "scene");
                    if (!scene.empty()) settings.scene = ToUtf8Portable(ResolvePath(scene, baseDir));
                    settings.scale = std::clamp(ReadFloat(*v, "scale", settings.scale), 0.001f, 1000.0f);
                    settings.weight = std::clamp(ReadFloat(*v, "weight", settings.weight), 0.0f, 1000.0f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::RockScatter) {
                geometry::RockScatterSettings settings;
                if (const json* v = FindMember(item, "rockScatter"); v && v->is_object()) {
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                    settings.spacing = std::clamp(ReadFloat(*v, "spacing", settings.spacing), geometry::kMinScatterSpacing, 10000.0f);
                    settings.maxCount = std::clamp(ReadInt(*v, "maxCount", settings.maxCount), 1, geometry::kMaxScatterCount);
                    settings.scaleMin = std::clamp(ReadFloat(*v, "scaleMin", settings.scaleMin), 0.01f, 100.0f);
                    settings.scaleMax = std::clamp(ReadFloat(*v, "scaleMax", settings.scaleMax), settings.scaleMin, 100.0f);
                    settings.alignToNormal = std::clamp(ReadFloat(*v, "alignToNormal", settings.alignToNormal), 0.0f, 1.0f);
                    settings.embed = std::clamp(ReadFloat(*v, "embed", settings.embed), 0.0f, 0.9f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Heightmap) {
                geometry::HeightmapSettings settings;
                if (const json* v = FindMember(item, "heightmap"); v && v->is_object()) {
                    settings.source = geometry::ParseHeightmapSource(ReadString(*v, "source", "noise"));
                    const std::string image = ReadString(*v, "image");
                    if (!image.empty()) settings.image = ToUtf8Portable(ResolvePath(image, baseDir));
                    settings.width = std::clamp(ReadFloat(*v, "width", settings.width), geometry::kMinTerrainSize, geometry::kMaxTerrainSize);
                    settings.depth = std::clamp(ReadFloat(*v, "depth", settings.depth), geometry::kMinTerrainSize, geometry::kMaxTerrainSize);
                    settings.minHeight = ReadFloat(*v, "minHeight", settings.minHeight);
                    settings.maxHeight = ReadFloat(*v, "maxHeight", settings.maxHeight);
                    settings.resolution = std::clamp(ReadInt(*v, "resolution", settings.resolution),
                                                     geometry::kMinTerrainResolution, geometry::kMaxTerrainResolution);
                    settings.textureResolution = ReadInt(*v, "textureResolution", settings.textureResolution);
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                    settings.featureSize = ReadFloat(*v, "featureSize", settings.featureSize);
                    settings.roughness = std::clamp(ReadFloat(*v, "roughness", settings.roughness), 0.0f, 1.0f);
                    settings.peak = std::clamp(ReadFloat(*v, "peak", settings.peak), 0.0f, 1.0f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::RockAsset) {
                graph::RockAssetSettings settings;
                if (const json* v = FindMember(item, "rockAsset"); v && v->is_object()) {
                    settings.lodCount = std::clamp(ReadInt(*v, "lodCount", settings.lodCount), 1, graph::kMaxRockAssetLods);
                    settings.maxTriangles = std::clamp(ReadInt(*v, "maxTriangles", settings.maxTriangles),
                                                       graph::kMinRockAssetTriangles, graph::kMaxRockAssetTriangles);
                    // 段ごとの配列。足りない段や壊れた値は既定のまま残す。
                    const auto readLevels = [&](const char* name, auto& values, float low, float high) {
                        const json* array = FindMember(*v, name);
                        if (!array || !array->is_array()) return;
                        for (size_t i = 0; i < values.size() && i < array->size(); ++i)
                            if ((*array)[i].is_number()) values[i] = std::clamp((*array)[i].get<float>(), low, high);
                    };
                    readLevels("trianglePercent", settings.trianglePercent, 0.1f, 100.0f);
                    readLevels("screenSize", settings.screenSize, 0.001f, 4.0f);
                    if (const json* share = FindMember(*v, "shareUv"); share && share->is_array())
                        for (size_t i = 0; i < settings.shareUv.size() && i < share->size(); ++i)
                            if ((*share)[i].is_boolean()) settings.shareUv[i] = (*share)[i].get<bool>();
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Remesh) {
                geometry::RemeshSettings settings;
                if (const json* v = FindMember(item, "remesh"); v && v->is_object()) {
                    settings.edgeLength = std::clamp(ReadFloat(*v, "edgeLength", settings.edgeLength), geometry::kMinRemeshEdge, geometry::kMaxRemeshEdge);
                    settings.iterations = std::clamp(ReadInt(*v, "iterations", settings.iterations), 1, geometry::kMaxRemeshIterations);
                    settings.featureAngle = std::clamp(ReadFloat(*v, "featureAngle", settings.featureAngle), 0.f, 180.f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::UvUnwrap) {
                geometry::UvUnwrapSettings settings;
                if (const json* v = FindMember(item, "uvUnwrap"); v && v->is_object()) {
                    settings.resolution = geometry::NormalizeUvResolution(ReadInt(*v, "resolution", 1024));
                    settings.padding = ReadInt(*v, "padding", 4);
                    settings.quality = ReadInt(*v, "quality", 1);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::MaterialMask) {
                graph::MaterialMaskSettings settings;
                if (const json* v = FindMember(item, "materialMask"); v && v->is_object()) {
                    if (const json* t = FindMember(*v, "texture")) settings.texture = readTexture(*t);
                    settings.value = std::clamp(ReadFloat(*v, "value", 1), 0.f, 1.f);
                    settings.repeatMeters = std::clamp(ReadFloat(*v, "repeatMeters", 1), .001f, 10000.f);
                    settings.invert = ReadBool(*v, "invert", false);
                    settings.triplanar = ReadBool(*v, "triplanar", false);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::ApplyMaterial) {
                graph::ApplyMaterialSettings settings;
                if (const json* v = FindMember(item, "applyMaterial"); v && v->is_object()) {
                    settings.heightBlend = ReadBool(*v, "heightBlend", false);
                    settings.heightBlendRange = std::clamp(ReadFloat(*v, "heightBlendRange", .2f), .01f, 1.f);
                    settings.opacity = std::clamp(ReadFloat(*v, "opacity", 1), 0.f, 1.f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::DepositionMask) {
                geometry::DepositionMaskSettings settings;
                if (const json* v = FindMember(item, "depositionMask"); v && v->is_object()) {
                    settings.amount = ReadFloat(*v, "amount", settings.amount);
                    settings.distance = ReadFloat(*v, "distance", settings.distance);
                    settings.maxSlopeDegrees = ReadFloat(*v, "maxSlopeDegrees", settings.maxSlopeDegrees);
                    settings.recessPreference = ReadFloat(*v, "recessPreference", settings.recessPreference);
                    settings.resolution = ReadInt(*v, "resolution", settings.resolution);
                    settings.samples = ReadInt(*v, "samples", settings.samples);
                    settings.invert = ReadBool(*v, "invert", settings.invert);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::StructureMask) {
                geometry::StructureMaskSettings settings;
                if (const json* v = FindMember(item, "structureMask"); v && v->is_object()) {
                    settings.type = geometry::ParseStructureMaskType(ReadString(*v, "type", "bands"));
                    settings.resolution = ReadInt(*v, "resolution", settings.resolution);
                    settings.fill = ReadFloat(*v, "fill", settings.fill);
                    settings.softness = ReadFloat(*v, "softness", settings.softness);
                    settings.scale = ReadFloat(*v, "scale", settings.scale);
                    settings.width = ReadFloat(*v, "width", settings.width);
                    settings.warp = ReadFloat(*v, "warp", settings.warp);
                    settings.warpScale = ReadFloat(*v, "warpScale", settings.warpScale);
                    settings.seed = ReadUInt(*v, "seed", settings.seed);
                    settings.invert = ReadBool(*v, "invert", settings.invert);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::NoiseMask) {
                geometry::NoiseMaskSettings settings;
                if (const json* v = FindMember(item, "noiseMask"); v && v->is_object()) {
                    settings.size = std::clamp(ReadFloat(*v, "size", settings.size), .001f, 1000.f);
                    settings.contrast = std::clamp(ReadFloat(*v, "contrast", settings.contrast), 0.f, 1.f);
                    settings.seed = ReadUInt(*v, "seed", settings.seed);
                    settings.detail = std::clamp(ReadFloat(*v, "detail", settings.detail), 0.f, 1.f);
                    settings.warp = std::clamp(ReadFloat(*v, "warp", settings.warp), 0.f, 1.f);
                    settings.resolution = std::clamp(geometry::NormalizeUvResolution(ReadInt(*v, "resolution", settings.resolution)),
                        geometry::kMinShapeMaskResolution, geometry::kMaxShapeMaskResolution);
                    settings.invert = ReadBool(*v, "invert", false);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::ShapeMask) {
                geometry::ShapeMaskSettings settings;
                if (const json* v = FindMember(item, "shapeMask"); v && v->is_object()) {
                    const std::string type = ReadString(*v, "type");
                    settings.type = type == "direction" ? geometry::ShapeMaskType::Direction
                                  : type == "height"    ? geometry::ShapeMaskType::Height
                                  : type == "valleyCurvature" ? geometry::ShapeMaskType::ValleyCurvature
                                  : type == "ridgeCurvature" ? geometry::ShapeMaskType::RidgeCurvature : geometry::ShapeMaskType::Occlusion;
                    // 2のべき乗へ切り上げる（UV Unwrap の解像度と同じ扱い）。
                    settings.resolution = std::clamp(geometry::NormalizeUvResolution(ReadInt(*v, "resolution", settings.resolution)),
                                                     geometry::kMinShapeMaskResolution, geometry::kMaxShapeMaskResolution);
                    settings.low = std::clamp(ReadFloat(*v, "low", settings.low), 0.f, .999f);
                    settings.high = std::clamp(ReadFloat(*v, "high", settings.high), settings.low + .001f, 1.f);
                    settings.invert = ReadBool(*v, "invert", false);
                    settings.gamma = std::clamp(ReadFloat(*v, "gamma", 1), .1f, 10.f);
                    settings.distance = std::clamp(ReadFloat(*v, "distance", settings.distance), .001f, 1000.f);
                    settings.samples = std::clamp(ReadInt(*v, "samples", settings.samples), geometry::kMinOcclusionSamples, geometry::kMaxOcclusionSamples);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::MaskCombine) {
                geometry::MaskCombineSettings settings;
                if (const json* v = FindMember(item, "maskCombine"); v && v->is_object()) {
                    const std::string operation = ReadString(*v, "operation");
                    settings.operation = operation == "maximum" ? geometry::MaskCombineOperation::Maximum
                                       : operation == "minimum" ? geometry::MaskCombineOperation::Minimum
                                       : operation == "subtract" ? geometry::MaskCombineOperation::Subtract
                                       : operation == "mix"      ? geometry::MaskCombineOperation::Mix
                                                                 : geometry::MaskCombineOperation::Multiply;
                    settings.mix = std::clamp(ReadFloat(*v, "mix", settings.mix), 0.f, 1.f);
                    settings.low = std::clamp(ReadFloat(*v, "low", settings.low), 0.f, .999f);
                    settings.high = std::clamp(ReadFloat(*v, "high", settings.high), settings.low + .001f, 1.f);
                    settings.gamma = std::clamp(ReadFloat(*v, "gamma", 1), .1f, 10.f);
                    settings.invert = ReadBool(*v, "invert", false);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::MaskFilter) {
                geometry::MaskFilterSettings settings;
                if (const json* v = FindMember(item, "maskFilter"); v && v->is_object()) {
                    const std::string type = ReadString(*v, "type");
                    settings.type = type == "sharpen"  ? geometry::MaskFilterType::Sharpen
                                  : type == "levels"   ? geometry::MaskFilterType::Levels
                                                       : geometry::MaskFilterType::Blur;
                    settings.radius = std::clamp(ReadFloat(*v, "radius", settings.radius), geometry::kMinMaskFilterRadius,
                                                 geometry::kMaxMaskFilterRadius);
                    settings.amount = std::clamp(ReadFloat(*v, "amount", settings.amount), 0.f, 4.f);
                    settings.inputLow = std::clamp(ReadFloat(*v, "inputLow", settings.inputLow), 0.f, .999f);
                    settings.inputHigh = std::clamp(ReadFloat(*v, "inputHigh", settings.inputHigh), settings.inputLow + .001f, 1.f);
                    settings.gamma = std::clamp(ReadFloat(*v, "gamma", 1), .1f, 10.f);
                    settings.outputLow = std::clamp(ReadFloat(*v, "outputLow", settings.outputLow), 0.f, 1.f);
                    settings.outputHigh = std::clamp(ReadFloat(*v, "outputHigh", settings.outputHigh), 0.f, 1.f);
                    settings.invert = ReadBool(*v, "invert", false);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::MaterialBake) {
                graph::MaterialBakeSettings settings;
                if (const json* v = FindMember(item, "materialBake"); v && v->is_object()) {
                    if (const json* layer = FindMember(*v, "layer"); layer && layer->is_object()) settings.bakedLayer = ReadLayer(*layer, readMaterial);
                    settings.fingerprint = ReadString(*v, "fingerprint");
                    settings.geometryAo = ReadBool(*v, "geometryAo", false);
                    settings.aoDistance = std::clamp(ReadFloat(*v, "aoDistance", .5f), .001f, 1000.f);
                    settings.aoStrength = std::clamp(ReadFloat(*v, "aoStrength", 1), 0.f, 1.f);
                    settings.aoSamples = std::clamp(ReadInt(*v, "aoSamples", 32), 8, 128);
                    settings.cageDistance = std::clamp(ReadFloat(*v, "cageDistance", settings.cageDistance),
                                                       geometry::kMinCageDistance, geometry::kMaxCageDistance);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeToMesh) {
                geometry::VolumeToMeshSettings settings;
                if (const json* v = FindMember(item, "volumeToMesh"); v && v->is_object()) {
                    if (ReadString(*v, "method", "marchingTetrahedra") == "dualContouring")
                        settings.method = geometry::VolumeMeshingMethod::DualContouring;
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::RandomBoxes) {
                geometry::BoxClusterSettings settings;
                if (const json* v = FindMember(item, "randomBoxes"); v && v->is_object()) {
                    settings.count = ReadInt(*v, "count", settings.count);
                    const auto size = ReadFloat3(*v, "size", {2, 2.4f, 1.8f});
                    settings.size = {size.x, size.y, size.z};
                    settings.sizeVariation = ReadFloat(*v, "sizeVariation", settings.sizeVariation);
                    settings.spread = ReadFloat(*v, "spread", settings.spread);
                    settings.rotation = ReadFloat(*v, "rotation", settings.rotation);
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::ToVolume) {
                geometry::VolumeSettings settings;
                if (const json* v = FindMember(item, "toVolume"); v && v->is_object())
                    settings.resolution = ReadInt(*v, "resolution", settings.resolution);
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeTransform) {
                geometry::VolumeTransformSettings settings;
                if (const json* v = FindMember(item, "volumeTransform"); v && v->is_object()) {
                    const auto position = ReadFloat3(*v, "position", {});
                    const auto rotation = ReadFloat3(*v, "rotation", {});
                    settings.position = {position.x, position.y, position.z};
                    settings.rotationDegrees = {rotation.x, rotation.y, rotation.z};
                    settings.scale = ReadFloat(*v, "scale", settings.scale);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeSmooth) {
                geometry::VolumeSmoothSettings settings;
                if (const json* v = FindMember(item, "volumeSmooth"); v && v->is_object()) {
                    settings.mode = geometry::ParseVolumeSmoothMode(ReadString(*v, "mode", "smooth"));
                    settings.radius = ReadFloat(*v, "radius", settings.radius);
                    settings.amount = ReadFloat(*v, "amount", settings.amount);
                    settings.upwardFocus = ReadFloat(*v, "upwardFocus", settings.upwardFocus);
                    const auto focus = ReadFloat3(*v, "focusDirection", {0, 1, 0});
                    settings.focusDirection = {focus.x, focus.y, focus.z};
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeEdgeWear) {
                geometry::VolumeEdgeWearSettings settings;
                if (const json* v = FindMember(item, "volumeEdgeWear"); v && v->is_object()) {
                    settings.radius = ReadFloat(*v, "radius", settings.radius);
                    settings.amount = ReadFloat(*v, "amount", settings.amount);
                    settings.noise = ReadFloat(*v, "noise", settings.noise);
                    settings.noiseScale = ReadFloat(*v, "noiseScale", settings.noiseScale);
                    settings.upwardFocus = ReadFloat(*v, "upwardFocus", settings.upwardFocus);
                    const auto focus = ReadFloat3(*v, "focusDirection", {0, 1, 0});
                    settings.focusDirection = {focus.x, focus.y, focus.z};
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeClip) {
                geometry::VolumeClipSettings settings;
                if (const json* v = FindMember(item, "volumeClip"); v && v->is_object()) {
                    settings.mode = geometry::ParseVolumeClipMode(ReadString(*v, "mode", "world"));
                    settings.height = ReadFloat(*v, "height", settings.height);
                    settings.invert = ReadBool(*v, "invert", settings.invert);
                    settings.embed = ReadFloat(*v, "embed", settings.embed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeScatter) {
                geometry::VolumeScatterSettings settings;
                if (const json* v = FindMember(item, "volumeScatter"); v && v->is_object()) {
                    settings.shape = geometry::ParseVolumeScatterShape(ReadString(*v, "shape", "sphere"));
                    settings.operation = geometry::ParseVolumeScatterOperation(ReadString(*v, "operation", "union"));
                    settings.count = ReadInt(*v, "count", settings.count);
                    settings.radiusMin = ReadFloat(*v, "radiusMin", settings.radiusMin);
                    settings.radiusMax = ReadFloat(*v, "radiusMax", settings.radiusMax);
                    settings.depthMin = ReadFloat(*v, "depthMin", settings.depthMin);
                    settings.depthMax = ReadFloat(*v, "depthMax", settings.depthMax);
                    settings.elongation = ReadFloat(*v, "elongation", settings.elongation);
                    settings.blend = ReadFloat(*v, "blend", settings.blend);
                    settings.seed = ReadUInt(*v, "seed", settings.seed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeUndercut) {
                geometry::VolumeUndercutSettings settings;
                if (const json* v = FindMember(item, "volumeUndercut"); v && v->is_object()) {
                    settings.height = ReadFloat(*v, "height", settings.height);
                    settings.width = ReadFloat(*v, "width", settings.width);
                    settings.depth = ReadFloat(*v, "depth", settings.depth);
                    settings.count = ReadInt(*v, "count", settings.count);
                    settings.spacing = ReadFloat(*v, "spacing", settings.spacing);
                    settings.noise = ReadFloat(*v, "noise", settings.noise);
                    settings.noiseScale = ReadFloat(*v, "noiseScale", settings.noiseScale);
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeClose) {
                geometry::VolumeCloseSettings settings;
                if (const json* v = FindMember(item, "volumeClose"); v && v->is_object()) {
                    settings.mode = geometry::ParseVolumeCloseMode(ReadString(*v, "mode", "occlusion"));
                    settings.width = ReadFloat(*v, "width", settings.width);
                    settings.distance = ReadFloat(*v, "distance", settings.distance);
                    settings.threshold = ReadFloat(*v, "threshold", settings.threshold);
                    settings.samples = ReadInt(*v, "samples", settings.samples);
                    settings.softness = ReadFloat(*v, "softness", settings.softness);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeTerrace) {
                geometry::VolumeTerraceSettings settings;
                if (const json* v = FindMember(item, "volumeTerrace"); v && v->is_object()) {
                    settings.step = ReadFloat(*v, "step", settings.step);
                    settings.depth = ReadFloat(*v, "depth", settings.depth);
                    settings.ratio = ReadFloat(*v, "ratio", settings.ratio);
                    settings.softness = ReadFloat(*v, "softness", settings.softness);
                    settings.variation = ReadFloat(*v, "variation", settings.variation);
                    settings.noise = ReadFloat(*v, "noise", settings.noise);
                    settings.noiseScale = ReadFloat(*v, "noiseScale", settings.noiseScale);
                    const auto rotation = ReadFloat3(*v, "rotation", {});
                    settings.rotationDegrees = {rotation.x, rotation.y, rotation.z};
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeNoise) {
                geometry::VolumeNoiseSettings settings;
                if (const json* v = FindMember(item, "volumeNoise"); v && v->is_object()) {
                    settings.type = geometry::ParseVolumeNoiseType(ReadString(*v, "type", "facet"));
                    settings.amount = ReadFloat(*v, "amount", settings.amount);
                    settings.scale = ReadFloat(*v, "scale", settings.scale);
                    settings.octaves = ReadInt(*v, "octaves", settings.octaves);
                    settings.warp = ReadFloat(*v, "warp", settings.warp);
                    settings.warpScale = ReadFloat(*v, "warpScale", settings.warpScale);
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::ParallelPlanes) {
                geometry::ParallelPlanesSettings settings;
                if (const json* v = FindMember(item, "parallelPlanes"); v && v->is_object()) {
                    const auto rotation = ReadFloat3(*v, "rotation", {});
                    settings.rotationDegrees = {rotation.x, rotation.y, rotation.z};
                    settings.spacing = ReadFloat(*v, "spacing", settings.spacing);
                    settings.offset = ReadFloat(*v, "offset", settings.offset);
                    settings.variation = ReadFloat(*v, "variation", settings.variation);
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeCrack) {
                geometry::VolumeCrackSettings settings;
                if (const json* v = FindMember(item, "volumeCrack"); v && v->is_object()) {
                    settings.width = ReadFloat(*v, "width", settings.width);
                    settings.depth = ReadFloat(*v, "depth", settings.depth);
                    settings.variation = ReadFloat(*v, "variation", settings.variation);
                    settings.noise = ReadFloat(*v, "noise", settings.noise);
                    settings.noiseScale = ReadFloat(*v, "noiseScale", settings.noiseScale);
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                    settings.source = geometry::ParseVolumeCrackSource(ReadString(*v, "source", "inputs"));
                    settings.shellSpacing = ReadFloat(*v, "shellSpacing", settings.shellSpacing);
                    settings.shellCount = ReadInt(*v, "shellCount", settings.shellCount);
                    settings.shellSmoothing = ReadFloat(*v, "shellSmoothing", settings.shellSmoothing);
                    settings.shellPeel = ReadFloat(*v, "shellPeel", settings.shellPeel);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::PlaneCuts) {
                geometry::PlaneCutsSettings settings;
                if (const json* v = FindMember(item, "planeCuts"); v && v->is_object()) {
                    settings.count = ReadInt(*v, "count", settings.count);
                    settings.seed = ReadInt(*v, "seed", settings.seed);
                    settings.scope = geometry::ParsePlaneCutsScope(ReadString(*v, "scope", "global"));
                    settings.radius = ReadFloat(*v, "radius", settings.radius);
                    settings.depthMin = ReadFloat(*v, "depthMin", settings.depthMin);
                    settings.depthMax = ReadFloat(*v, "depthMax", settings.depthMax);
                    settings.distribution =
                        geometry::ParsePlaneCutsDistribution(ReadString(*v, "distribution", "isotropic"));
                    settings.systems = ReadInt(*v, "systems", settings.systems);
                    const auto rotation = ReadFloat3(*v, "rotation", {});
                    settings.rotationDegrees = {rotation.x, rotation.y, rotation.z};
                    settings.spreadDegrees = ReadFloat(*v, "spread", settings.spreadDegrees);
                    settings.blend = ReadFloat(*v, "blend", settings.blend);
                    settings.curvature = ReadFloat(*v, "curvature", settings.curvature);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::VolumeBoolean) {
                geometry::VolumeBooleanSettings settings;
                if (const json* v = FindMember(item, "volumeBoolean"); v && v->is_object()) {
                    settings.operation =
                        geometry::ParseVolumeBooleanOperation(ReadString(*v, "operation", "union"));
                    settings.blend = ReadFloat(*v, "blend", settings.blend);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::BaseRock) {
                graph::BaseRockNodeSettings settings;
                if (const json* values = FindMember(item, "baseRock"); values && values->is_object()) {
                    const auto size = ReadFloat3(*values, "size", {2, 2, 2});
                    settings.size = {size.x, size.y, size.z};
                    settings.seed = ReadInt(*values, "seed", 0);
                    settings.shape = geometry::ParseBaseShape(ReadString(*values, "shape", "box"));
                    settings.subdivisions = ReadInt(*values, "subdivisions", 8);
                    settings.roundness = ReadFloat(*values, "roundness", 0.25f);
                    settings.noiseStrength = ReadFloat(*values, "noiseStrength", 0);
                    settings.noiseScale = ReadFloat(*values, "noiseScale", 2);
                }
                created.settings = settings;
            } else if (graph::IsLayerNodeKind(created.kind)) {
                graph::LayerNodeSettings settings;
                if (const json* layer = FindMember(item, "layer");
                    layer != nullptr && layer->is_object()) {
                    settings.layer = ReadLayer(*layer, readMaterial);
                }
                created.settings = std::move(settings);
            } else if (created.kind == graph::NodeKind::Model || created.kind == graph::NodeKind::Transform) {
                // 位置・回転（X / Y / Z の度。数値 1 つなら Y だけ）・倍率は Model と Transform で共通。
                float position[3] = {0.0f, 0.0f, 0.0f}, rotation[3] = {0.0f, 0.0f, 0.0f}, scale = 1.0f;
                const bool isModel = created.kind == graph::NodeKind::Model;
                const json* values = FindMember(item, isModel ? "model" : "transform");
                if (values != nullptr && values->is_object()) {
                    const DirectX::XMFLOAT3 p = ReadFloat3(*values, "position", {});
                    position[0] = p.x; position[1] = p.y; position[2] = p.z;
                    if (const json* r = FindMember(*values, "rotation"); r && r->is_number()) {
                        rotation[1] = r->get<float>();
                    } else {
                        const DirectX::XMFLOAT3 value = ReadFloat3(*values, "rotation", {});
                        rotation[0] = value.x; rotation[1] = value.y; rotation[2] = value.z;
                    }
                    const float read = ReadFloat(*values, "scale", 1.0f);
                    scale = std::isfinite(read) && read > 0.0f ? read : 1.0f;
                }
                const auto assign = [&](auto& settings) {
                    std::copy(std::begin(position), std::end(position), settings.position);
                    std::copy(std::begin(rotation), std::end(rotation), settings.rotationDegrees);
                    settings.scale = scale;
                };
                if (isModel) {
                    graph::ModelNodeSettings settings;
                    assign(settings);
                    if (const json* reference = values ? FindMember(*values, "model") : nullptr; reference && readModel)
                        settings.model = readModel(*reference);
                    if (const json* rotations = values ? FindMember(*values, "nodeRotations") : nullptr;
                        rotations && rotations->is_array()) {
                        for (const json& entry : *rotations) {
                            if (!entry.is_object()) continue;
                            renderer::ModelNodeRotation nodeRotation;
                            nodeRotation.node = ReadString(entry, "node", "");
                            const DirectX::XMFLOAT3 value = ReadFloat3(entry, "rotation", {});
                            nodeRotation.rotationDegrees[0] = value.x;
                            nodeRotation.rotationDegrees[1] = value.y;
                            nodeRotation.rotationDegrees[2] = value.z;
                            if (!nodeRotation.node.empty()) settings.nodeRotations.push_back(std::move(nodeRotation));
                        }
                    }
                    created.settings = settings;
                } else {
                    graph::TransformNodeSettings settings;
                    assign(settings);
                    created.settings = settings;
                }
            } else {
                // Mesh Output は設定を持たない。
                created.settings = std::monostate{};
            }
            nodes.push_back(std::move(created));
        }
    }

    std::vector<const json*> namedLinks;
    // ID が欠けていたピン・重複したピンへ新しい番号を振る（衝突するとリンクが別のピンへ付く）。
    std::unordered_set<graph::GraphId> pinIds;
    for (graph::Node& created : nodes) {
        for (auto* pins : {&created.inputs, &created.outputs}) {
            for (graph::Pin& pin : *pins) {
                if (pin.id <= 0 || !pinIds.insert(pin.id).second) {
                    pin.id = ++maxId;
                    pinIds.insert(pin.id);
                }
            }
        }
    }

    if (const json* items = FindMember(node, "links"); items != nullptr && items->is_array()) {
        for (const json& item : *items) {
            if (!item.is_object()) {
                continue;
            }
            if (item.contains("from") || item.contains("to")) {
                namedLinks.push_back(&item);  // ピンの ID が決まってから解決する
                continue;
            }
            graph::Link link;
            link.id = ReadInt(item, "id", 0);
            link.startPin = ReadInt(item, "start", 0);
            link.endPin = ReadInt(item, "end", 0);
            if (link.id <= 0 || link.startPin <= 0 || link.endPin <= 0) {
                if (issues != nullptr) {
                    issues->push_back({0, std::max(link.id, 0), "id / start / end のどれかが無いリンクを捨てました"});
                }
                continue;
            }
            links.push_back(link);
        }
    }

    if (nodes.empty()) {
        return false;
    }
    // 名前で書いたリンク（"from" / "to"）は、ピンの ID が決まったノードの上で解決する。ID は省略できる。
    if (!namedLinks.empty()) {
        graph::NodeGraph pinsOnly;
        pinsOnly.Replace(nodes, {});
        for (const graph::Link& link : links) maxId = std::max(maxId, link.id);
        for (const json* item : namedLinks) maxId = std::max(maxId, ReadInt(*item, "id", 0));
        for (const json* item : namedLinks) {
            std::string error;
            graph::Link link;
            link.startPin = ResolveEnd(pinsOnly, item->value("from", json()), graph::PinKind::Output, error);
            if (link.startPin) link.endPin = ResolveEnd(pinsOnly, item->value("to", json()), graph::PinKind::Input, error);
            if (!link.startPin || !link.endPin) {
                if (issues) issues->push_back({0, ReadInt(*item, "id", 0), "リンクを捨てました: " + error});
                continue;
            }
            link.id = ReadInt(*item, "id", 0);
            if (link.id <= 0) link.id = ++maxId;
            links.push_back(link);
        }
    }

    // Replace が壊れたリンクの除去と次の採番の再構築を行う。
    const std::vector<graph::Link> requested = issues != nullptr ? links : std::vector<graph::Link>{};
    graphData.Replace(std::move(nodes), std::move(links));
    if (issues != nullptr) ReportDroppedLinks(graphData, requested, *issues);
    LayoutUnplacedNodes(graphData);
    return true;
}

}  // namespace rock::io
