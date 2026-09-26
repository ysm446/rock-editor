#include "io/LayerMaterialIo.h"
#include "graph/SurfacePresetGraph.h"
#include "io/PieceSettings.h"
#include "io/ProjectIo.h"

#include "core/PathUtf8.h"

#include "core/ImageIo.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rock::io {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kProjectFormat = "rock-editor.project";
constexpr const char* kMaterialFormat = "rock-editor.material";
// 形式を変えたら上げる。読み込み側は「これ以下なら読める」として扱う。
//
// 1: rock-editor としての最初の形式。road-editor 時代の terrain-graph.* とは
//    互換を持たない（形式の識別子が違うので、そもそも読み込みで弾かれる）。
constexpr int kProjectFormatVersion = 1;
// マテリアル単体 (.rockmat) の版。
constexpr int kMaterialFormatVersion = 1;

// --- 文字列とパス ---------------------------------------------------------
//
// JSON は UTF-8。変換は core/PathUtf8.h に一本化してある。
// 保存する文字列は区切りを '/' に揃える（ToUtf8Portable）。

// baseDir から見た相対パスにする。ドライブが違うなど relative が使えないときは
// 絶対パスのまま書く。プロジェクトごと移動しても壊れないようにするため。
std::string RelativePathString(const fs::path& target, const fs::path& baseDir) {
    if (target.empty()) {
        return {};
    }
    std::error_code error;
    const fs::path absolute = fs::absolute(target, error);
    const fs::path& source = error ? target : absolute;

    std::error_code relativeError;
    const fs::path relative = fs::relative(source, baseDir, relativeError);
    if (relativeError || relative.empty()) {
        return ToUtf8Portable(source);
    }
    return ToUtf8Portable(relative);
}

// 相対パスなら baseDir から解決する。絶対パスならそのまま。
fs::path ResolvePath(const std::string& text, const fs::path& baseDir) {
    if (text.empty()) {
        return {};
    }
    const fs::path path = FromUtf8(text);
    if (path.is_absolute()) {
        return path;
    }
    return (baseDir / path).lexically_normal();
}

// --- JSON の読み書き（例外を投げない） ------------------------------------
//
// 型が食い違っていたら既定値に落とす。手で編集されたファイルでも落ちないようにする。

const json* FindMember(const json& node, const char* key) {
    const auto it = node.find(key);
    return (it != node.end()) ? &(*it) : nullptr;
}

float ReadFloat(const json& node, const char* key, float fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_number()) ? member->get<float>() : fallback;
}

int ReadInt(const json& node, const char* key, int fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_number_integer()) ? member->get<int>() : fallback;
}

uint32_t ReadUInt(const json& node, const char* key, uint32_t fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_number_integer()) {
        return fallback;
    }
    const int64_t value = member->get<int64_t>();
    return (value < 0) ? fallback : static_cast<uint32_t>(value);
}

bool ReadBool(const json& node, const char* key, bool fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_boolean()) ? member->get<bool>() : fallback;
}

std::string ReadString(const json& node, const char* key, const std::string& fallback = {}) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_string()) ? member->get<std::string>() : fallback;
}

// モデルの倍率。0 以下や非有限は 1（そのまま）に落とす。
float ReadModelScale(const json& node) {
    const float scale = ReadFloat(node, "scale", 1.0f);
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

json WriteFloat3(const DirectX::XMFLOAT3& value) {
    return json::array({value.x, value.y, value.z});
}

DirectX::XMFLOAT3 ReadFloat3(const json& node, const char* key,
                             const DirectX::XMFLOAT3& fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_array() || member->size() != 3) {
        return fallback;
    }
    DirectX::XMFLOAT3 value = fallback;
    float* components[3] = {&value.x, &value.y, &value.z};
    for (size_t i = 0; i < 3; ++i) {
        const json& element = (*member)[i];
        if (element.is_number()) {
            *components[i] = element.get<float>();
        }
    }
    return value;
}

// --- 列挙 -----------------------------------------------------------------
//
// 数値ではなく名前で書く。ファイルを直接読んだときに意味が分かるようにするため。
// 名前の並びは enum の値と一致させること。

const char* const kTextureChannelNames[] = {"r", "g", "b", "a"};
const char* const kValueSourceNames[] = {"constant", "noise", "texture"};
const char* const kNoiseTypeNames[] = {"fbm",    "ridged", "worley",
                                      "perlin", "billow", "cracks"};
const char* const kChannelNames[] = {"baseColor", "normal", "surface", "height"};
const char* const kTonemapNames[] = {"none", "reinhard", "aces"};
const char* const kSkySourceNames[] = {"procedural", "hdri"};
const char* const kApertureShapeNames[] = {"circle", "triangle", "hexagon", "octagon"};

template <size_t N>
const char* EnumName(const char* const (&names)[N], uint32_t value) {
    return (value < N) ? names[value] : names[0];
}

template <size_t N>
uint32_t EnumValue(const char* const (&names)[N], const json& node, const char* key,
                   uint32_t fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_string()) {
        return fallback;
    }
    const std::string text = member->get<std::string>();
    for (uint32_t i = 0; i < N; ++i) {
        if (text == names[i]) {
            return i;
        }
    }
    return fallback;
}

// --- テクスチャ参照 -------------------------------------------------------
//
// 参照の書き方は用途で変わる。
//   プロジェクト:   textures 配列の通し番号
//   マテリアル単体: 画像ファイルへの相対パス
// どちらも「参照が無い」は null で表す。

using TextureWriter = std::function<json(compositor::TextureId)>;
using TextureReader = std::function<compositor::TextureId(const json&)>;

json WriteMapSlot(const compositor::MapSlot& slot, const TextureWriter& writeTexture) {
    json node;
    node["texture"] = writeTexture(slot.texture);
    node["channel"] = EnumName(kTextureChannelNames, static_cast<uint32_t>(slot.channel));
    return node;
}

compositor::MapSlot ReadMapSlot(const json& node, const char* key,
                                const TextureReader& readTexture) {
    compositor::MapSlot slot;
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_object()) {
        return slot;
    }
    const json* texture = FindMember(*member, "texture");
    slot.texture = (texture != nullptr) ? readTexture(*texture) : compositor::kNoTexture;
    slot.channel = static_cast<compositor::TextureChannel>(
        EnumValue(kTextureChannelNames, *member, "channel", 0));
    return slot;
}

// --- ノイズ ---------------------------------------------------------------

json WriteNoise(const compositor::NoiseParams& noise) {
    json node;
    node["type"] = EnumName(kNoiseTypeNames, static_cast<uint32_t>(noise.type));
    node["scale"] = noise.scale;
    node["amount"] = noise.amount;
    node["octaves"] = noise.octaves;
    node["offset"] = noise.offset;
    return node;
}

compositor::NoiseParams ReadNoise(const json& node, const char* key,
                                  const compositor::NoiseParams& fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_object()) {
        return fallback;
    }
    compositor::NoiseParams noise;
    noise.type = static_cast<compositor::NoiseType>(
        EnumValue(kNoiseTypeNames, *member, "type", static_cast<uint32_t>(fallback.type)));
    noise.scale = ReadFloat(*member, "scale", fallback.scale);
    noise.amount = ReadFloat(*member, "amount", fallback.amount);
    noise.octaves = ReadInt(*member, "octaves", fallback.octaves);
    noise.offset = ReadFloat(*member, "offset", fallback.offset);
    return noise;
}

// --- マテリアル -----------------------------------------------------------
//
// プロジェクトへの埋め込みと .rockmat で同じ形を使う。違うのはテクスチャ参照の書き方だけ。

// compositor::MaterialMap の並び。maps のキーと同じ名前。
const char* const kMaterialMapNames[] = {"baseColor", "normal", "roughness", "metallic",
                                         "ambientOcclusion", "height", "opacity"};
static_assert(std::size(kMaterialMapNames) == static_cast<size_t>(compositor::MaterialMap::Count));

void RemapLayerReferences(compositor::MaterialAsset& asset, const std::unordered_map<int, compositor::MaterialAssetId>& ids) {
    if (!asset.layerMaterial) return;
    auto body = WriteLayerMaterial(*asset.layerMaterial);
    MapLayerMaterials(body, [&](const json& value) -> json {
        const auto it = ids.find(value.is_number_integer() ? value.get<int>() : 0);
        return it == ids.end() ? json(0) : json(it->second);
    });
    std::string error;
    ReadLayerMaterial(body, *asset.layerMaterial, error);
}

json WriteMaterialBody(const compositor::MaterialAsset& asset, const TextureWriter& writeTexture) {
    if (asset.layerMaterial) {
        auto node = WriteLayerMaterial(*asset.layerMaterial);
        node["name"] = asset.name;
        return node;
    }
    json node;
    node["name"] = asset.name;
    node["baseColorTint"] = WriteFloat3(asset.baseColorTint);
    node["hueShift"] = asset.hueShiftDegrees;
    node["saturation"] = asset.saturation;
    node["brightness"] = asset.brightness;
    node["roughness"] = asset.roughnessValue;
    node["metallic"] = asset.metallicValue;
    node["ambientOcclusion"] = asset.ambientOcclusionValue;
    // 不透明度と合成モード。材質の属性。
    static const char* const kBlendModeNames[] = {"opaque", "masked", "translucent"};
    node["opacity"] = asset.opacityValue;
    node["blendMode"] = EnumName(kBlendModeNames, static_cast<uint32_t>(asset.blendMode));
    node["maskThreshold"] = asset.maskThreshold;

    json maps;
    maps["baseColor"] = writeTexture(asset.baseColor);
    maps["normal"] = writeTexture(asset.normal);
    // 法線マップの規約（緑の向き）。既定は OpenGL（反転して読む）。
    node["flipNormalGreen"] = asset.flipNormalGreen;
    maps["roughness"] = WriteMapSlot(asset.roughness, writeTexture);
    maps["metallic"] = WriteMapSlot(asset.metallic, writeTexture);
    maps["ambientOcclusion"] = WriteMapSlot(asset.ambientOcclusion, writeTexture);
    maps["height"] = WriteMapSlot(asset.height, writeTexture);
    maps["opacity"] = WriteMapSlot(asset.opacity, writeTexture);
    node["maps"] = std::move(maps);
    // マップごとの UV（1 か 2）。maps の中はテクスチャの参照だけにしておく（ProjectWorkspace が書き換えるため）。
    json uvSets;
    for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::MaterialMap::Count); ++i) {
        uvSets[kMaterialMapNames[i]] = compositor::UsesSecondUv(asset, static_cast<compositor::MaterialMap>(i)) ? 2 : 1;
    }
    node["mapUvSets"] = std::move(uvSets);
    return node;
}

void ReadMaterialBody(const json& node, compositor::MaterialAsset& asset,
                      const TextureReader& readTexture) {
    if (node.contains("materials") || node.contains("materialGraph")) {
        graph::LayerMaterial layer;
        std::string error;
        if (ReadLayerMaterial(node, layer, error)) {
            if (layer.materialGraph) {
                graph::ExtractPresetLayers(layer, layer.materials, error);
                layer.materialGraph.reset();
            }
            asset.layerMaterial = std::move(layer); asset.name = asset.layerMaterial->name;
        }
        else { asset.layerError = error; ROCK_LOG_ERROR("レイヤーマテリアル: %s", error.c_str()); }
        return;
    }
    const compositor::MaterialAsset defaults;
    asset.name = ReadString(node, "name", defaults.name);
    asset.baseColorTint = ReadFloat3(node, "baseColorTint", defaults.baseColorTint);
    asset.hueShiftDegrees = ReadFloat(node, "hueShift", defaults.hueShiftDegrees);
    asset.saturation = ReadFloat(node, "saturation", defaults.saturation);
    asset.brightness = std::clamp(ReadFloat(node, "brightness", defaults.brightness), 0.0f, 8.0f);
    asset.roughnessValue = ReadFloat(node, "roughness", defaults.roughnessValue);
    asset.metallicValue = ReadFloat(node, "metallic", defaults.metallicValue);
    asset.ambientOcclusionValue =
        ReadFloat(node, "ambientOcclusion", defaults.ambientOcclusionValue);
    {
        static const char* const kBlendModeNames[] = {"opaque", "masked", "translucent"};
        asset.opacityValue = std::clamp(ReadFloat(node, "opacity", defaults.opacityValue), 0.0f, 1.0f);
        asset.blendMode = static_cast<compositor::BlendMode>(
            EnumValue(kBlendModeNames, node, "blendMode", static_cast<uint32_t>(defaults.blendMode)));
        asset.maskThreshold = std::clamp(ReadFloat(node, "maskThreshold", defaults.maskThreshold), 0.0f, 1.0f);
    }

    const json* maps = FindMember(node, "maps");
    if (maps == nullptr || !maps->is_object()) {
        return;
    }
    const json* baseColor = FindMember(*maps, "baseColor");
    asset.baseColor = (baseColor != nullptr) ? readTexture(*baseColor) : compositor::kNoTexture;
    asset.flipNormalGreen = ReadBool(node, "flipNormalGreen", defaults.flipNormalGreen);
    const json* normal = FindMember(*maps, "normal");
    asset.normal = (normal != nullptr) ? readTexture(*normal) : compositor::kNoTexture;
    asset.roughness = ReadMapSlot(*maps, "roughness", readTexture);
    asset.metallic = ReadMapSlot(*maps, "metallic", readTexture);
    asset.ambientOcclusion = ReadMapSlot(*maps, "ambientOcclusion", readTexture);
    asset.height = ReadMapSlot(*maps, "height", readTexture);
    asset.opacity = ReadMapSlot(*maps, "opacity", readTexture);
    // 無ければ（版の古いファイル）すべて 1 つ目の UV。
    asset.mapUvSets = defaults.mapUvSets;
    if (const json* uvSets = FindMember(node, "mapUvSets"); uvSets != nullptr && uvSets->is_object()) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::MaterialMap::Count); ++i) {
            if (ReadInt(*uvSets, kMaterialMapNames[i], 1) == 2)
                asset.mapUvSets |= compositor::MaterialMapBit(static_cast<compositor::MaterialMap>(i));
        }
    }
}

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

// --- ノードグラフ ---------------------------------------------------------
//
// ノードの kind は enum の数値ではなく定義テーブルの名前で書く
// （「列挙は名前で書く」）。ピンはノードの定義から再生成するので、
// ファイルには ID の並びだけを持つ（リンクがピン ID を参照するため）。

// writeModel は Model ノードのモデル（実行中の ID）を文書内の番号へ写す。無ければ null を書く。
json WriteGraph(const graph::NodeGraph& graphData,
                const std::function<json(compositor::MaterialAssetId)>& writeMaterial,
                const std::function<json(uint64_t)>& writeModel, const TextureWriter& writeTexture) {
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
        } else if (const auto* uv = std::get_if<geometry::UvUnwrapSettings>(&node.settings)) {
            item["uvUnwrap"] = {{"resolution", uv->resolution}, {"padding", uv->padding}, {"quality", uv->quality}};
        } else if (const auto* mask = std::get_if<graph::MaterialMaskSettings>(&node.settings)) {
            item["materialMask"] = {{"texture", writeTexture(mask->texture)}, {"value", mask->value},
                {"repeatMeters", mask->repeatMeters}, {"invert", mask->invert}, {"triplanar", mask->triplanar}};
        } else if (const auto* apply = std::get_if<graph::ApplyMaterialSettings>(&node.settings)) {
            item["applyMaterial"] = {{"heightBlend", apply->heightBlend}, {"heightBlendRange", apply->heightBlendRange}};
        } else if (const auto* deposition = std::get_if<geometry::DepositionMaskSettings>(&node.settings)) {
            item["depositionMask"] = {{"amount", deposition->amount}, {"distance", deposition->distance},
                {"maxSlopeDegrees", deposition->maxSlopeDegrees}, {"recessPreference", deposition->recessPreference},
                {"resolution", deposition->resolution}, {"samples", deposition->samples}, {"invert", deposition->invert}};
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
        } else if (const auto* bake = std::get_if<graph::MaterialBakeSettings>(&node.settings)) {
            // ベイク結果は一時的なもので、保存しない。開き直したら未ベイクへ戻る。指紋だけ残すと、結果が無いのに
            // 「ベイク済み」と判定されるので、材質を書けないときは指紋も書かない。
            // 旧版が Bakes/ へ保存した結果（一時でない材質）は、従来どおり書く。
            const bool keepBake = !writeMaterial(bake->bakedLayer.material).is_null();
            item["materialBake"] = {{"geometryAo", bake->geometryAo}, {"aoDistance", bake->aoDistance}, {"aoStrength", bake->aoStrength}, {"aoSamples", bake->aoSamples}};
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
                                    {"upwardFocus", smooth->upwardFocus}};
        } else if (const auto* wear = std::get_if<geometry::VolumeEdgeWearSettings>(&node.settings)) {
            item["volumeEdgeWear"] = {{"radius", wear->radius}, {"amount", wear->amount}, {"noise", wear->noise},
                                      {"noiseScale", wear->noiseScale}, {"upwardFocus", wear->upwardFocus}, {"seed", wear->seed}};
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
                                   {"noiseScale", crack->noiseScale}, {"seed", crack->seed}};
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
                                 {"blend", cuts->blend}};
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

// 戻り値はノードを 1 つ以上読めたか。空のグラフ節は「グラフ未使用」とみなし、
// 呼び出し側が旧 layers からの移行に切り替える。
// readModel は Model ノードの文書内の番号を実行中のモデル ID へ写す（0 = なし）。
bool ReadGraph(const json& node, graph::NodeGraph& graphData,
               const std::function<compositor::MaterialAssetId(const json&)>& readMaterial,
               const std::function<uint64_t(const json&)>& readModel, const TextureReader& readTexture) {
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
        for (const json& item : *items) {
            if (!item.is_object()) {
                continue;
            }
            const graph::NodeDefinition* definition =
                graph::FindNodeDefinitionByName(ReadString(item, "kind"));
            const int id = ReadInt(item, "id", 0);
            if (definition == nullptr || id <= 0) {
                // 知らない種類は捨てる（将来のビルドで増えた種類を古いビルドで開いた場合）。
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
                    extra.id = (*inputIds)[inputIndex].get<int>();
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
            } else if (created.kind == graph::NodeKind::MaterialBake) {
                graph::MaterialBakeSettings settings;
                if (const json* v = FindMember(item, "materialBake"); v && v->is_object()) {
                    if (const json* layer = FindMember(*v, "layer"); layer && layer->is_object()) settings.bakedLayer = ReadLayer(*layer, readMaterial);
                    settings.fingerprint = ReadString(*v, "fingerprint");
                    settings.geometryAo = ReadBool(*v, "geometryAo", false);
                    settings.aoDistance = std::clamp(ReadFloat(*v, "aoDistance", .5f), .001f, 1000.f);
                    settings.aoStrength = std::clamp(ReadFloat(*v, "aoStrength", 1), 0.f, 1.f);
                    settings.aoSamples = std::clamp(ReadInt(*v, "aoSamples", 32), 8, 128);
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

    // ID が欠けていたピンへ新しい番号を振る（0 のままだと ID が衝突する）。
    for (graph::Node& created : nodes) {
        for (graph::Pin& pin : created.inputs) {
            if (pin.id <= 0) {
                pin.id = ++maxId;
            }
        }
        for (graph::Pin& pin : created.outputs) {
            if (pin.id <= 0) {
                pin.id = ++maxId;
            }
        }
    }

    if (const json* items = FindMember(node, "links"); items != nullptr && items->is_array()) {
        for (const json& item : *items) {
            if (!item.is_object()) {
                continue;
            }
            graph::Link link;
            link.id = ReadInt(item, "id", 0);
            link.startPin = ReadInt(item, "start", 0);
            link.endPin = ReadInt(item, "end", 0);
            if (link.id <= 0 || link.startPin <= 0 || link.endPin <= 0) {
                continue;
            }
            links.push_back(link);
        }
    }

    if (nodes.empty()) {
        return false;
    }
    // Replace が壊れたリンクの除去と次の採番の再構築を行う。
    graphData.Replace(std::move(nodes), std::move(links));
    return true;
}

// --- プレビューの設定 -----------------------------------------------------

// 天球アセット（M5b-2）で HDRI のパスが preview から抜けたため、パスの解決は不要になった。
json WritePreview(renderer::PreviewRenderer& renderer) {
    json node;
    node["tonemap"] = EnumName(kTonemapNames, static_cast<uint32_t>(renderer.Tonemap()));
    node["tessellation"] = renderer.TessellationEnabled();
    node["tessellationFactor"] = renderer.TessellationFactor();
    node["tessellationTargetPixels"] = renderer.TessellationTargetPixels();
    node["materialResolution"] = renderer.MaterialResolution();
    node["showSkybox"] = renderer.ShowSkybox();
    node["skyboxBlur"] = renderer.SkyboxBlur();
    node["screenSpaceAo"] = {{"enabled", renderer.Ssao().enabled}, {"radius", renderer.Ssao().radius}, {"strength", renderer.Ssao().strength}};
    node["shadow"] = renderer.ShadowEnabled();
    node["shadowResolution"] = renderer.ShadowResolution();
    node["shadowCascadeCount"] = renderer.ShadowCascadeCount();

    // 被写界深度。見え方だけの設定だが、プロジェクトごとに変えるものなので残す。
    const renderer::DofSettings& dof = renderer.Dof();
    json dofNode;
    dofNode["enabled"] = dof.enabled;
    dofNode["focusOnTarget"] = dof.focusOnTarget;
    dofNode["focusDistance"] = dof.focusDistance;
    dofNode["blurScale"] = dof.blurScale;
    dofNode["miniatureScale"] = dof.miniatureScale;
    dofNode["maxBlurPixels"] = dof.maxBlurPixels;
    dofNode["shape"] = EnumName(kApertureShapeNames, static_cast<uint32_t>(dof.shape));
    dofNode["rotationDegrees"] = dof.rotationDegrees;
    node["depthOfField"] = std::move(dofNode);
    // 環境そのもの（HDRI・較正値・空のパラメータ）は天球アセットが持つ。
    // ここには「見え方」だけを書く。

    const renderer::CameraState camera = renderer.GetCamera().State();
    json cameraNode;
    cameraNode["target"] = WriteFloat3(camera.target);
    cameraNode["distance"] = camera.distance;
    cameraNode["yaw"] = camera.yaw;
    cameraNode["pitch"] = camera.pitch;
    cameraNode["fovY"] = camera.fovY;
    node["camera"] = std::move(cameraNode);

    // light は作業用のライト（作業用IBL）。シーンの空の太陽と大気は別に持つ。
    const renderer::LightSettings& light = renderer.WorkLight();
    json lightNode;
    lightNode["azimuth"] = light.azimuth;
    lightNode["elevation"] = light.elevation;
    lightNode["illuminance"] = light.illuminance;
    lightNode["color"] = WriteFloat3(light.color);
    node["light"] = std::move(lightNode);

    // シーンの空（大気散乱）。lightingMode は表示環境（ibl = 作業用IBL、atmospheric = シーンの空）。
    node["lightingMode"] = renderer.AtmosphericMode() ? "atmospheric" : "ibl";
    {
        const renderer::LightSettings& sun = renderer.SceneSunLight();
        const renderer::AtmosphereSettings& sky = renderer.AtmosphericSettings();
        node["atmosphere"] = {{"azimuth", sun.azimuth},
                              {"elevation", sun.elevation},
                              {"illuminance", sun.illuminance},
                              {"density", sky.density},
                              {"mie", sky.mie},
                              {"eccentricity", sky.eccentricity},
                              {"altitude", sky.altitude},
                              {"groundAlbedo", sky.groundAlbedo},
                              {"lowerHemisphere", sky.lowerHemisphere != 0 ? "ground" : "sky"},
                              {"skylightIntensity", renderer.SkylightIntensity()}};
    }

    const renderer::ExposureSettings& exposure = renderer.Exposure();
    json exposureNode;
    exposureNode["automatic"] = exposure.automatic;
    exposureNode["compensation"] = exposure.compensation;
    exposureNode["minEv100"] = exposure.minEv100;
    exposureNode["maxEv100"] = exposure.maxEv100;
    exposureNode["adaptationSpeed"] = exposure.adaptationSpeed;
    exposureNode["useManualEv"] = exposure.useManualEv;
    exposureNode["manualEv100"] = exposure.manualEv100;
    exposureNode["aperture"] = exposure.aperture;
    exposureNode["shutterSpeed"] = exposure.shutterSpeed;
    exposureNode["iso"] = exposure.iso;
    node["exposure"] = std::move(exposureNode);
    return node;
}

void ReadPreview(const json& node, renderer::PreviewRenderer& renderer) {
    // 既定値は renderer::kPreviewDefaults の一択。数値を直接書かない。
    // 名前は各節ローカルの defaults（LightSettings など）と衝突させない。
    const renderer::PreviewDefaults& previewDefaults = renderer::kPreviewDefaults;
    renderer.Tonemap() = static_cast<renderer::TonemapMode>(
        EnumValue(kTonemapNames, node, "tonemap", static_cast<uint32_t>(previewDefaults.tonemap)));
    // 旧ファイルの平面プレビューの設定（useMaterialTextures / displacementScale / planeSize /
    // meshSubdivisions / flatMaterial）は読まない（平面のプレビューは無くなった）。
    renderer.TessellationEnabled() =
        ReadBool(node, "tessellation", previewDefaults.tessellationEnabled);
    renderer.TessellationFactor() =
        std::clamp(ReadFloat(node, "tessellationFactor", previewDefaults.tessellationFactor), 1.0f, 64.0f);
    renderer.TessellationTargetPixels() = std::clamp(
        ReadFloat(node, "tessellationTargetPixels", previewDefaults.tessellationTargetPixels), 2.0f, 64.0f);
    renderer.RequestMaterialResolution(
        ReadUInt(node, "materialResolution", previewDefaults.materialResolution));
    renderer.ShowSkybox() = ReadBool(node, "showSkybox", previewDefaults.showSkybox);
    renderer.SkyboxBlur() = ReadBool(node, "skyboxBlur", previewDefaults.skyboxBlur);
    renderer.ShadowEnabled() = ReadBool(node, "shadow", previewDefaults.shadowEnabled);
    renderer.RequestShadowResolution(ReadUInt(node, "shadowResolution", previewDefaults.shadowResolution));
    renderer.RequestShadowCascadeCount(ReadUInt(node, "shadowCascadeCount", previewDefaults.shadowCascadeCount));

    // 節が丸ごと欠けていても既定値で埋める。file-format.md の「欠けているキーは
    // 既定値で埋める」に合わせる（節ごと飛ばすと前のプロジェクトの値が残る）。
    const json emptySection = json::object();
    const auto section = [&node, &emptySection](const char* key) -> const json& {
        const json* member = FindMember(node, key);
        return (member != nullptr && member->is_object()) ? *member : emptySection;
    };

    {
        const auto& ao = section("screenSpaceAo");
        const renderer::SsaoSettings defaults;
        renderer.Ssao().enabled = ReadBool(ao, "enabled", defaults.enabled);
        renderer.Ssao().radius = std::clamp(ReadFloat(ao, "radius", defaults.radius), 0.001f, 10.0f);
        renderer.Ssao().strength = std::clamp(ReadFloat(ao, "strength", defaults.strength), 0.0f, 3.0f);
    }
    {
        const json& camera = section("camera");
        renderer::CameraState state;
        state.target = ReadFloat3(camera, "target", state.target);
        state.distance = ReadFloat(camera, "distance", state.distance);
        state.yaw = ReadFloat(camera, "yaw", state.yaw);
        state.pitch = ReadFloat(camera, "pitch", state.pitch);
        state.fovY = ReadFloat(camera, "fovY", state.fovY);
        renderer.GetCamera().SetState(state);
    }

    {
        const json& light = section("light");
        renderer::LightSettings& target = renderer.WorkLight();
        const renderer::LightSettings defaults;
        target.azimuth = ReadFloat(light, "azimuth", defaults.azimuth);
        target.elevation = ReadFloat(light, "elevation", defaults.elevation);
        target.illuminance = ReadFloat(light, "illuminance", defaults.illuminance);
        target.color = ReadFloat3(light, "color", defaults.color);
    }

    {
        // シーンの空。無ければ（古いファイル）既定値で、表示環境は作業用IBL。
        renderer.AtmosphericMode() = ReadString(node, "lightingMode", "ibl") == "atmospheric";
        const json& atmosphere = section("atmosphere");
        const renderer::AtmosphereSettings defaults;
        renderer::LightSettings& sun = renderer.SceneSunLight();
        sun.azimuth = ReadFloat(atmosphere, "azimuth", defaults.azimuth);
        sun.elevation = std::clamp(ReadFloat(atmosphere, "elevation", defaults.elevation), -1.5f, 1.5f);
        sun.illuminance = std::max(ReadFloat(atmosphere, "illuminance", defaults.illuminance), 0.0f);
        sun.color = {1.0f, 1.0f, 1.0f};
        renderer::AtmosphereSettings& sky = renderer.AtmosphericSettings();
        sky = defaults;
        sky.density = std::clamp(ReadFloat(atmosphere, "density", defaults.density), 0.1f, 3.0f);
        sky.mie = std::clamp(ReadFloat(atmosphere, "mie", defaults.mie), 0.0f, 2.0f);
        sky.eccentricity = std::clamp(ReadFloat(atmosphere, "eccentricity", defaults.eccentricity), 0.0f, 0.95f);
        sky.altitude = std::clamp(ReadFloat(atmosphere, "altitude", defaults.altitude), 0.0f, 10000.0f);
        sky.groundAlbedo = std::clamp(ReadFloat(atmosphere, "groundAlbedo", defaults.groundAlbedo), 0.0f, 1.0f);
        sky.lowerHemisphere =
            ReadString(atmosphere, "lowerHemisphere", defaults.lowerHemisphere != 0 ? "ground" : "sky") == "sky" ? 0u : 1u;
        renderer.SkylightIntensity() = std::clamp(
            ReadFloat(atmosphere, "skylightIntensity", renderer::PreviewRenderer::kDefaultSkylightIntensity), 0.0f, 8.0f);
    }

    {
        const json& exposure = section("exposure");
        renderer::ExposureSettings& target = renderer.Exposure();
        const renderer::ExposureSettings defaults;
        target.automatic = ReadBool(exposure, "automatic", defaults.automatic);
        target.compensation = std::clamp(ReadFloat(exposure, "compensation", defaults.compensation), -5.0f, 5.0f);
        target.minEv100 = std::clamp(ReadFloat(exposure, "minEv100", defaults.minEv100), -10.0f, 20.0f);
        target.maxEv100 = std::clamp(ReadFloat(exposure, "maxEv100", defaults.maxEv100), -10.0f, 20.0f);
        target.adaptationSpeed = std::clamp(ReadFloat(exposure, "adaptationSpeed", defaults.adaptationSpeed), 0.1f, 20.0f);
        target.useManualEv = ReadBool(exposure, "useManualEv", defaults.useManualEv);
        target.manualEv100 = ReadFloat(exposure, "manualEv100", defaults.manualEv100);
        target.aperture = ReadFloat(exposure, "aperture", defaults.aperture);
        target.shutterSpeed = ReadFloat(exposure, "shutterSpeed", defaults.shutterSpeed);
        target.iso = ReadFloat(exposure, "iso", defaults.iso);
    }

    {
        const json& dofNode = section("depthOfField");
        renderer::DofSettings& target = renderer.Dof();
        const renderer::DofSettings defaults;
        target.enabled = ReadBool(dofNode, "enabled", defaults.enabled);
        target.focusOnTarget = ReadBool(dofNode, "focusOnTarget", defaults.focusOnTarget);
        target.focusDistance = ReadFloat(dofNode, "focusDistance", defaults.focusDistance);
        target.blurScale = ReadFloat(dofNode, "blurScale", defaults.blurScale);
        target.miniatureScale =
            ReadFloat(dofNode, "miniatureScale", defaults.miniatureScale);
        target.maxBlurPixels = ReadFloat(dofNode, "maxBlurPixels", defaults.maxBlurPixels);
        target.shape = static_cast<renderer::ApertureShape>(EnumValue(
            kApertureShapeNames, dofNode, "shape", static_cast<uint32_t>(defaults.shape)));
        target.rotationDegrees =
            ReadFloat(dofNode, "rotationDegrees", defaults.rotationDegrees);
    }
}

// --- 天球 -----------------------------------------------------------------

json WriteSky(const renderer::SkyAsset& asset, const fs::path& baseDir) {
    json node;
    node["name"] = asset.name;
    node["source"] = EnumName(kSkySourceNames, static_cast<uint32_t>(asset.sky.source));
    // 画像はテクスチャと同じく相対パスの参照で持つ。使っていなければ null。
    node["hdri"] = asset.sky.hdriPath.empty()
                       ? json()
                       : json(RelativePathString(asset.sky.hdriPath, baseDir));
    node["skyLuminance"] = asset.sky.skyLuminance;
    node["iblIntensity"] = asset.sky.iblIntensity;

    const renderer::SkySettings& procedural = asset.sky.procedural;
    json proceduralNode;
    proceduralNode["zenithColor"] = WriteFloat3(procedural.zenithColor);
    proceduralNode["horizonColor"] = WriteFloat3(procedural.horizonColor);
    proceduralNode["groundColor"] = WriteFloat3(procedural.groundColor);
    proceduralNode["intensity"] = procedural.intensity;
    node["procedural"] = std::move(proceduralNode);
    return node;
}

// 天球 1 つを読み込んでライブラリへ足す。
renderer::SkyAssetId ReadSky(const json& node, renderer::SkyLibrary& skies,
                             const fs::path& baseDir) {
    const renderer::SkyDefinition defaults;
    std::string name = ReadString(node, "name");
    if (name.empty()) {
        name = "天球";
    }
    const renderer::SkyAssetId id = skies.Add(name);
    renderer::SkyAsset* asset = skies.FindMutable(id);
    if (asset == nullptr) {
        return renderer::kNoSkyAsset;
    }

    // 共有アセットから来たものは置き場所と永続 ID を持つ（埋め込みの旧形式では空）。
    asset->assetPath = FromUtf8(ReadString(node, "_assetPath"));
    asset->assetUid = ReadString(node, "uid");
    asset->sky.source = static_cast<renderer::SkySource>(
        EnumValue(kSkySourceNames, node, "source", static_cast<uint32_t>(defaults.source)));
    if (const std::string hdri = ReadString(node, "hdri"); !hdri.empty()) {
        asset->sky.hdriPath = ResolvePath(hdri, baseDir);
    }
    asset->sky.skyLuminance = ReadFloat(node, "skyLuminance", defaults.skyLuminance);
    asset->sky.iblIntensity = ReadFloat(node, "iblIntensity", defaults.iblIntensity);

    if (const json* procedural = FindMember(node, "procedural");
        procedural != nullptr && procedural->is_object()) {
        renderer::SkySettings& target = asset->sky.procedural;
        const renderer::SkySettings proceduralDefaults;
        target.zenithColor = ReadFloat3(*procedural, "zenithColor", proceduralDefaults.zenithColor);
        target.horizonColor =
            ReadFloat3(*procedural, "horizonColor", proceduralDefaults.horizonColor);
        target.groundColor = ReadFloat3(*procedural, "groundColor", proceduralDefaults.groundColor);
        target.intensity = ReadFloat(*procedural, "intensity", proceduralDefaults.intensity);
    }
    return id;
}

// 天球アセットが無いプロジェクト（天球を入れる前の形式）から 1 つ作る。
// 当時は環境がビューポートに 1 つしか無く、preview 節に直接書かれていた。
void MigrateSkyFromPreview(const json& preview, renderer::SkyLibrary& skies,
                           const fs::path& baseDir) {
    const renderer::SkyDefinition defaults;
    const std::string hdri = ReadString(preview, "hdri");
    const renderer::SkyAssetId id = skies.Add("既定の空");
    renderer::SkyAsset* asset = skies.FindMutable(id);
    if (asset == nullptr) {
        return;
    }
    if (!hdri.empty()) {
        asset->sky.source = renderer::SkySource::Hdri;
        asset->sky.hdriPath = ResolvePath(hdri, baseDir);
        asset->name = ToUtf8Display(asset->sky.hdriPath.stem());
    }
    asset->sky.skyLuminance = ReadFloat(preview, "hdriSkyLuminance", defaults.skyLuminance);
    asset->sky.iblIntensity = ReadFloat(preview, "iblIntensity", defaults.iblIntensity);

    if (const json* sky = FindMember(preview, "sky"); sky != nullptr && sky->is_object()) {
        renderer::SkySettings& target = asset->sky.procedural;
        const renderer::SkySettings proceduralDefaults;
        target.zenithColor = ReadFloat3(*sky, "zenithColor", proceduralDefaults.zenithColor);
        target.horizonColor = ReadFloat3(*sky, "horizonColor", proceduralDefaults.horizonColor);
        target.groundColor = ReadFloat3(*sky, "groundColor", proceduralDefaults.groundColor);
        target.intensity = ReadFloat(*sky, "intensity", proceduralDefaults.intensity);
    }
    skies.SetActive(id);
}

// --- ファイル入出力 -------------------------------------------------------

bool WriteJsonFile(const fs::path& path, const json& document) {
    std::error_code error;
    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, error);
    }

    // いきなり上書きすると、ディスクフルなどで途中失敗したときに元のファイルが
    // 壊れたまま残る。一時ファイルへ書き切ってから rename で差し替える。
    const fs::path tempPath = path.wstring() + L".tmp";
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            ROCK_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
        // 人が読める形で書く。差分も取りやすい。壊れた文字列が混ざっていても
        // 例外を出さない（不正な UTF-8 は置換文字にする）。
        stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        // バッファの最終書き込み・closeの失敗も、元ファイルの差し替え前に検出する。
        stream.close();
        if (!stream.good()) {
            ROCK_LOG_ERROR("ファイルの書き込みに失敗しました: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
    }

    std::error_code renameError;
    fs::rename(tempPath, path, renameError);
    if (renameError) {
        ROCK_LOG_ERROR("ファイルを差し替えられませんでした: %s", ToUtf8Portable(path).c_str());
        std::error_code removeError;
        fs::remove(tempPath, removeError);
        return false;
    }
    return true;
}

bool ReadJsonFile(const fs::path& path, const char* expectedFormat, int maxVersion,
                  json& outDocument) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        ROCK_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // 例外は使わない方針なので、パース失敗は discarded で受ける。
    outDocument = json::parse(stream, nullptr, false);
    if (outDocument.is_discarded() || !outDocument.is_object()) {
        ROCK_LOG_ERROR("JSON として読めませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // material-mixer 時代のファイルは "material-mixer.project" などの形式名を持つ。
    // 中身は同じなので、旧形式名は新形式名へ読み替えて受け付ける（書くのは新形式名のみ）。
    std::string format = ReadString(outDocument, "format");
    if (format.rfind("material-mixer.", 0) == 0) {
        format = "rock-editor." + format.substr(std::string("material-mixer.").size());
    }
    if (format != expectedFormat) {
        ROCK_LOG_ERROR("形式が違います（%s ではなく %s）: %s", expectedFormat, format.c_str(),
                     ToUtf8Portable(path).c_str());
        return false;
    }
    const int version = ReadInt(outDocument, "version", 0);
    if (version > maxVersion) {
        ROCK_LOG_ERROR("このバージョンでは読めません（ファイル %d > 対応 %d）: %s", version,
                     maxVersion, ToUtf8Portable(path).c_str());
        return false;
    }
    return true;
}

}  // namespace

bool SaveProject(const std::filesystem::path& path, const ProjectRefs& refs,
                 ProjectWorkspace* workspace) {
    // 裸のファイル名（親ディレクトリ無し）で保存すると相対パスが作れず、
    // 全参照が絶対パスで書かれてしまう。先に絶対化してから基準を取る。
    std::error_code absoluteError;
    const fs::path absolutePath = fs::absolute(path, absoluteError);
    const fs::path& savePath = absoluteError ? path : absolutePath;
    const fs::path baseDir = savePath.parent_path();

    // シーンとして保存するときは、先に共有アセットを各ファイルへ書く。
    // ここで失敗したら文書には触らない（片方だけ新しい状態を作らない）。
    if (workspace != nullptr) {
        if (_wcsicmp(savePath.extension().c_str(), L".rockscene") != 0 || !workspace->Contains(savePath)) {
            ROCK_LOG_ERROR("シーンはプロジェクトルート内の .rockscene へ保存してください: %s",
                         ToUtf8Display(savePath).c_str());
            return false;
        }
        if (!SaveSharedAssets(*workspace, refs)) {
            ROCK_LOG_ERROR("共有アセットを保存できないため、シーンの保存を中止しました");
            return false;
        }
    }

    json document;
    document["format"] = kProjectFormat;
    document["version"] = kProjectFormatVersion;
    document["app"] = ROCK_APP_VERSION;

    // --- テクスチャ（画像は参照。パスはプロジェクトからの相対） -----------
    // ファイルの中では通し番号で参照する。実行中の ID をそのまま書くと、
    // 削除して番号が飛んだときにファイルが読みにくくなる。
    std::unordered_map<compositor::TextureId, int> textureIndex;
    json textures = json::array();
    for (const compositor::LibraryTexture& entry : refs.textures.Entries()) {
        // 一時的なテクスチャ（Material Bake の結果）はファイルを持たない。参照は「なし」として書かれる。
        if (entry.transient) continue;
        const int index = static_cast<int>(textures.size()) + 1;
        textureIndex[entry.id] = index;

        json node;
        node["id"] = index;
        node["name"] = entry.name;
        node["path"] = RelativePathString(entry.path, baseDir);
        textures.push_back(std::move(node));
    }
    document["textures"] = std::move(textures);

    const TextureWriter writeTexture = [&textureIndex](compositor::TextureId id) {
        const auto it = textureIndex.find(id);
        return (it != textureIndex.end()) ? json(it->second) : json();
    };

    // --- マテリアル（構造ごと埋め込む） -----------------------------------
    std::unordered_map<compositor::MaterialAssetId, int> materialIndex;
    json materials = json::array();
    for (const compositor::MaterialAsset& asset : refs.materials.Entries()) {
        if (asset.transient) continue;
        const int index = static_cast<int>(materials.size()) + 1;
        materialIndex[asset.id] = index;

        json node = WriteMaterialBody(asset, writeTexture);
        node["id"] = index;
        if (workspace != nullptr) {
            node["_assetPath"] = ToUtf8Portable(asset.assetPath);
            node["uid"] = asset.assetUid;
        }
        materials.push_back(std::move(node));
    }
    for (auto& node : materials) MapLayerMaterials(node, [&](const json& value) -> json {
        const auto it = materialIndex.find(value.is_number_integer() ? value.get<uint32_t>() : 0);
        return it == materialIndex.end() ? json(0) : json(it->second);
    });
    document["materials"] = std::move(materials);

    // --- モデル（FBX は参照。スロットのマテリアルは文書内の番号） ----------
    std::unordered_map<uint64_t, int> modelIndex;
    if (refs.models != nullptr) {
        json models = json::array();
        for (const renderer::ModelAsset& asset : *refs.models) {
            modelIndex[asset.id] = static_cast<int>(models.size()) + 1;
            json slots = json::array();
            for (const compositor::MaterialAssetId id : asset.materials) {
                const auto found = materialIndex.find(id);
                slots.push_back(found == materialIndex.end() ? json() : json(found->second));
            }
            json node = {{"id", static_cast<int>(models.size()) + 1},
                         {"name", asset.name},
                         {"path", RelativePathString(asset.path, baseDir)},
                         {"scale", asset.scale},
                         {"materials", std::move(slots)}};
            if (workspace != nullptr) {
                node["_assetPath"] = ToUtf8Portable(asset.assetPath);
                node["uid"] = asset.assetUid;
            }
            models.push_back(std::move(node));
        }
        document["models"] = std::move(models);
    }

    // --- ノードグラフ -----------------------------------------------------
    // 版 4 から layers 節は書かない。合成の構造はグラフだけが持つ。
    const std::function<json(compositor::MaterialAssetId)> writeMaterial =
        [&materialIndex](compositor::MaterialAssetId id) {
            const auto it = materialIndex.find(id);
            return (it != materialIndex.end()) ? json(it->second) : json();
        };
    const std::function<json(uint64_t)> writeModel = [&modelIndex](uint64_t id) {
        const auto found = modelIndex.find(id);
        return found != modelIndex.end() ? json(found->second) : json();
    };
    document["graph"] = WriteGraph(refs.graph, writeMaterial, writeModel, writeTexture);

    // 天球はマテリアルと同じく、構造ごと埋め込む（画像だけ相対パスの参照）。
    json skies = json::array();
    int activeSkyIndex = 0;
    for (const renderer::SkyAsset& asset : refs.skies.Entries()) {
        if (asset.id == refs.skies.ActiveId()) {
            activeSkyIndex = static_cast<int>(skies.size());
        }
        skies.push_back(WriteSky(asset, baseDir));
        if (workspace != nullptr) {
            skies.back()["_assetPath"] = ToUtf8Portable(asset.assetPath);
            skies.back()["uid"] = asset.assetUid;
        }
    }
    document["skies"] = std::move(skies);
    document["activeSky"] = activeSkyIndex;

    document["preview"] = WritePreview(refs.renderer);

    if (workspace != nullptr) {
        if (!workspace->SaveScene(savePath, document)) {
            ROCK_LOG_ERROR("シーンを保存できませんでした: %s", ToUtf8Display(savePath).c_str());
            return false;
        }
        ROCK_LOG_INFO("シーンを保存しました: %s", ToUtf8Display(savePath).c_str());
        return true;
    }
    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    ROCK_LOG_INFO("プロジェクトを保存しました: %s", ToUtf8Portable(savePath).c_str());
    return true;
}

bool LoadProject(const std::filesystem::path& path, rhi::Device& device,
                 rhi::PipelineCache& pipelineCache, const ProjectRefs& refs,
                 ProjectWorkspace* workspace) {
    json document;
    if (workspace != nullptr) {
        // シーンは参照する共有アセットを展開してから、従来の読み込み器に渡す。
        // 欠けたアセットがあればここで止まり、現在の文書は保持される。
        if (!workspace->ReadScene(path, document)) {
            ROCK_LOG_ERROR("シーンまたは参照アセットを開けません: %s", ToUtf8Display(path).c_str());
            return false;
        }
        if (const int version = ReadInt(document, "version", 0); version > kProjectFormatVersion) {
            ROCK_LOG_ERROR("このバージョンでは読めません（シーン %d > 対応 %d）: %s", version,
                         kProjectFormatVersion, ToUtf8Display(path).c_str());
            return false;
        }
    } else if (!ReadJsonFile(path, kProjectFormat, kProjectFormatVersion, document)) {
        return false;
    }

    const fs::path baseDir = path.parent_path();

    // 旧ファイルの手入力メッシュシーン（scene）は読まない。表示するメッシュは
    // グラフの Mesh Output から生成する。
    if (FindMember(document, "scene") != nullptr) {
        ROCK_LOG_WARN("旧形式の手入力メッシュシーン（scene）は読み飛ばしました");
    }
    refs.renderer.ClearMeshScene(device);

    // ここから先は現在の中身を捨てて入れ替える。読み込みは GPU 待機を伴うため、
    // 呼び出し側がフレームの外で呼んでいること。
    refs.materials.Clear(device);
    refs.skies.Clear(device);
    refs.textures.Clear(device);

    // --- テクスチャ -------------------------------------------------------
    std::unordered_map<int, compositor::TextureId> textureIds;
    if (const json* textures = FindMember(document, "textures");
        textures != nullptr && textures->is_array()) {
        for (const json& node : *textures) {
            if (!node.is_object()) {
                continue;
            }
            const int index = ReadInt(node, "id", 0);
            const fs::path texturePath = ResolvePath(ReadString(node, "path"), baseDir);
            if (index <= 0 || texturePath.empty()) {
                continue;
            }
            const std::string name = ReadString(node, "name");
            compositor::TextureId id = refs.textures.Load(device, pipelineCache, texturePath);
            if (id == compositor::kNoTexture) {
                // 画像が見つからなくても、残りは読み込む。**参照は捨てない。**
                // パスと名前だけの「リンク切れ」として登録し、マテリアルやノードの
                // 割り当てはそこへ繋いでおく。消してしまうと、次に保存した時点で
                // どのファイルを指していたかが失われ、繋ぎ直せなくなる。
                ROCK_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
                            ToUtf8Portable(texturePath).c_str());
                id = refs.textures.AddMissing(texturePath, name);
                if (id == compositor::kNoTexture) {
                    continue;
                }
            }
            textureIds[index] = id;
            if (compositor::LibraryTexture* entry = refs.textures.FindMutable(id);
                entry != nullptr && !name.empty()) {
                entry->name = name;
            }
        }
    }
    const TextureReader readTexture = [&textureIds](const json& node) {
        if (!node.is_number_integer()) {
            return compositor::kNoTexture;
        }
        const auto it = textureIds.find(node.get<int>());
        return (it != textureIds.end()) ? it->second : compositor::kNoTexture;
    };

    // --- マテリアル -------------------------------------------------------
    std::unordered_map<int, compositor::MaterialAssetId> materialIds;
    if (const json* materials = FindMember(document, "materials");
        materials != nullptr && materials->is_array()) {
        for (const json& node : *materials) {
            if (!node.is_object()) {
                continue;
            }
            const int index = ReadInt(node, "id", 0);
            const compositor::MaterialAssetId id = refs.materials.Add("マテリアル");
            if (compositor::MaterialAsset* asset = refs.materials.FindMutable(id);
                asset != nullptr) {
                ReadMaterialBody(node, *asset, readTexture);
                asset->assetPath = FromUtf8(ReadString(node, "_assetPath"));
                asset->assetUid = ReadString(node, "uid");
                asset->thumbnailDirty = true;
            }
            if (index > 0) {
                materialIds[index] = id;
            }
        }
    }

    for (const auto& [index, id] : materialIds) RemapLayerReferences(*refs.materials.FindMutable(id), materialIds);

    // --- モデル -----------------------------------------------------------
    // 欠けた FBX も参照を残す（リンク切れとして表示し、別の場所へ保存し直しても割り当てを失わない）。
    std::unordered_map<int, uint64_t> modelIds;
    if (refs.models != nullptr) {
        refs.models->clear();
        if (const json* models = FindMember(document, "models"); models != nullptr && models->is_array()) {
            for (const json& node : *models) {
                if (!node.is_object()) {
                    continue;
                }
                renderer::ModelAsset asset;
                asset.id = refs.models->size() + 1;
                asset.name = ReadString(node, "name");
                asset.assetPath = FromUtf8(ReadString(node, "_assetPath"));
                asset.assetUid = ReadString(node, "uid");
                asset.path = ResolvePath(ReadString(node, "path"), baseDir);
                asset.scale = ReadModelScale(node);
                if (!renderer::LoadModel(asset.path, asset)) {
                    ROCK_LOG_WARN("モデルを読み込めません（%s）: %s", asset.error.c_str(),
                                ToUtf8Display(asset.path).c_str());
                }
                if (const json* slots = FindMember(node, "materials"); slots != nullptr && slots->is_array()) {
                    asset.materials.resize(std::max(asset.materials.size(), slots->size()),
                                           compositor::kNoMaterialAsset);
                    for (size_t i = 0; i < slots->size(); ++i) {
                        const auto found = (*slots)[i].is_number_integer()
                                               ? materialIds.find((*slots)[i].get<int>())
                                               : materialIds.end();
                        if (found != materialIds.end()) asset.materials[i] = found->second;
                    }
                }
                modelIds[ReadInt(node, "id", 0)] = asset.id;
                refs.models->push_back(std::move(asset));
            }
        }
    }

    // --- ノードグラフ（旧形式は layers[] から移行） -----------------------
    const std::function<compositor::MaterialAssetId(const json&)> readMaterial =
        [&materialIds](const json& value) {
            if (!value.is_number_integer()) {
                return compositor::kNoMaterialAsset;
            }
            const auto it = materialIds.find(value.get<int>());
            return (it != materialIds.end()) ? it->second : compositor::kNoMaterialAsset;
        };
    // graph 節が唯一の合成。無ければ既定（Surface 1 つ）へ戻す。
    const json* graphNode = FindMember(document, "graph");
    bool graphLoaded = false;
    if (graphNode != nullptr && graphNode->is_object()) {
        const std::function<uint64_t(const json&)> readModel = [&modelIds](const json& value) -> uint64_t {
            if (!value.is_number_integer()) return 0;
            const auto found = modelIds.find(value.get<int>());
            return found != modelIds.end() ? found->second : 0;
        };
        graphLoaded = ReadGraph(*graphNode, refs.graph, readMaterial, readModel, readTexture);
    }
    if (!graphLoaded) {
        refs.graph = graph::NodeGraph::CreateDefault();
    }

    // preview が無い（または壊れている）プロジェクトでも必ず既定値で埋める。
    // 呼ばないと、前のプロジェクトのカメラ・ライト・露出が残ってしまう。
    const json* preview = FindMember(document, "preview");
    const json emptyPreview = json::object();
    const json& previewNode =
        (preview != nullptr && preview->is_object()) ? *preview : emptyPreview;
    ReadPreview(previewNode, refs.renderer);

    // 天球。無ければ preview 節から 1 つ作る（天球を入れる前のプロジェクト）。
    if (const json* skies = FindMember(document, "skies");
        skies != nullptr && skies->is_array() && !skies->empty()) {
        std::vector<renderer::SkyAssetId> ids;
        for (const json& sky : *skies) {
            if (!sky.is_object()) {
                continue;
            }
            ids.push_back(ReadSky(sky, refs.skies, baseDir));
        }
        const auto activeIndex = static_cast<size_t>(ReadUInt(document, "activeSky", 0));
        if (activeIndex < ids.size()) {
            refs.skies.SetActive(ids[activeIndex]);
        }
    } else {
        MigrateSkyFromPreview(previewNode, refs.skies, baseDir);
    }
    refs.skies.EnsureDefault();

    ROCK_LOG_INFO("プロジェクトを開きました: %s", ToUtf8Portable(path).c_str());
    return true;
}

bool SaveSharedAssets(ProjectWorkspace& workspace, const ProjectRefs& refs) {
    if (!workspace.Scan()) {
        return false;
    }
    bool valid = true;
    // 画像の参照。ルート外の実在ファイルは Imported/ へ取り込む。
    // リンク切れ（ファイルが無い）はパスだけを残し、参照を失わない。
    const auto source = [&](const fs::path& path) -> json {
        if (path.empty()) {
            return nullptr;
        }
        std::error_code error;
        if (!fs::is_regular_file(path, error)) {
            return {{"path", ToUtf8Portable(path.lexically_normal())}};
        }
        const fs::path target = workspace.Import(path, workspace.Root() / L"Imported");
        const json result = target.empty() ? json() : workspace.Reference(target);
        if (result.is_null()) {
            valid = false;
        }
        return result;
    };
    const TextureWriter writeTexture = [&](compositor::TextureId id) -> json {
        const compositor::LibraryTexture* entry = refs.textures.Find(id);
        return (entry != nullptr) ? source(entry->path) : json();
    };
    // この保存で ID を持つアセットが使う ID。ID の無いものを既存のファイルへ寄せるときに奪わない。
    std::unordered_set<std::string> claimedUids;
    for (const compositor::MaterialAsset& entry : refs.materials.Entries()) {
        if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
    }
    for (const renderer::SkyAsset& entry : refs.skies.Entries()) {
        if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
    }
    if (refs.models != nullptr) {
        for (const renderer::ModelAsset& entry : *refs.models) {
            if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
        }
    }
    // 置き場所が未定のものの保存先。ID の無いもの（旧 .reproj・単体 .rockmat から来たもの）は、
    // 同じ中身の既存アセットがあればそれを使う。無ければ名前から連番で作る。
    const auto placement = [&](json& body, const fs::path& current, const char* kind, const wchar_t* folder,
                               const std::string& name, const char* extension) -> fs::path {
        if (!current.empty() && workspace.Contains(current)) {
            return current;
        }
        if (ReadString(body, "uid").empty()) {
            const std::string uid = workspace.FindIdenticalAsset(kind, body, claimedUids);
            if (!uid.empty()) {
                body["uid"] = uid;
                return workspace.Resolve({{"uid", uid}});
            }
        }
        return workspace.UniquePath(workspace.Root() / folder, name, extension);
    };
    for (int pass = 0; pass < 2; ++pass) for (const compositor::MaterialAsset& entry : refs.materials.Entries()) {
        if (entry.transient || bool(entry.layerMaterial) != (pass == 1)) continue;
        compositor::MaterialAsset* asset = refs.materials.FindMutable(entry.id);
        json body = WriteMaterialBody(*asset, writeTexture);
        body["uid"] = asset->assetUid;
        if (asset->layerMaterial) MapLayerMaterials(body, [&](const json& value) -> json {
            const auto* source = refs.materials.Find(value.is_number_integer() ? value.get<uint32_t>() : 0);
            if (!source) return 0;
            if (source->layerMaterial || source->assetPath.empty()) { valid = false; return 0; }
            return workspace.Reference(source->assetPath);
        });
        const char* kind = asset->layerMaterial ? "layer-material-asset" : "material-asset";
        fs::path assetPath = placement(body, asset->assetPath, kind, L"Materials", asset->name, asset->layerMaterial ? ".tglayer" : ".rockmat");
        if (!valid || !workspace.SaveAsset(assetPath, kind, body)) {
            ROCK_LOG_ERROR("マテリアルを保存できません: %s", asset->name.c_str());
            return false;
        }
        asset->assetPath = assetPath;
        asset->assetUid = ReadString(body, "uid");
        claimedUids.insert(asset->assetUid);
    }
    for (const renderer::SkyAsset& entry : refs.skies.Entries()) {
        renderer::SkyAsset* asset = refs.skies.FindMutable(entry.id);
        json body = WriteSky(*asset, workspace.Root());
        body["hdri"] = source(asset->sky.hdriPath);
        body["uid"] = asset->assetUid;
        fs::path assetPath = placement(body, asset->assetPath, "sky-asset", L"Skies", asset->name, ".rocksky");
        if (!valid || !workspace.SaveAsset(assetPath, "sky-asset", body)) {
            ROCK_LOG_ERROR("天球を保存できません: %s", asset->name.c_str());
            return false;
        }
        asset->assetPath = assetPath;
        asset->assetUid = ReadString(body, "uid");
        claimedUids.insert(asset->assetUid);
    }
    // モデル。FBX は元ファイルの固定 ID、スロットは上で保存した .rockmat の固定 ID で参照する
    // （SaveScene が作る本文と一致させ、中身が変わらなければ書き直さない）。
    if (refs.models != nullptr) {
        for (renderer::ModelAsset& model : *refs.models) {
            json slots = json::array();
            for (const compositor::MaterialAssetId id : model.materials) {
                const compositor::MaterialAsset* asset = refs.materials.Find(id);
                json value = (asset != nullptr && !asset->assetPath.empty()) ? workspace.Reference(asset->assetPath) : json();
                if (asset != nullptr && value.is_null()) valid = false;
                slots.push_back(std::move(value));
            }
            json body = {{"name", model.name}, {"source", source(model.path)}, {"scale", model.scale},
                         {"materials", std::move(slots)}};
            body["uid"] = model.assetUid;
            fs::path assetPath = placement(body, model.assetPath, "model-asset", L"Models", model.name, ".rockmodel");
            if (!valid || !workspace.SaveAsset(assetPath, "model-asset", body)) {
                ROCK_LOG_ERROR("モデルを保存できません: %s", model.name.c_str());
                return false;
            }
            model.assetPath = assetPath;
            model.assetUid = ReadString(body, "uid");
            claimedUids.insert(model.assetUid);
        }
    }
    return valid;
}

namespace {

// 展開済みの文書（ProjectWorkspace::Expand の結果）の画像とマテリアルをライブラリへ足す。
// 同じ固定 ID のマテリアルが読み込み済みなら足さずにそれを使う。文書内の番号 → ライブラリの ID を返す。
void AddExpandedLibraries(const json& document, rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          compositor::TextureLibrary& textures, compositor::MaterialLibrary& materials,
                          std::unordered_map<int, compositor::TextureId>& textureIds,
                          std::unordered_map<int, compositor::MaterialAssetId>& materialIds) {
    for (const json& node : document.at("textures")) {
        const fs::path texturePath = FromUtf8(ReadString(node, "path"));
        compositor::TextureId id = textures.Load(device, pipelineCache, texturePath);
        if (id == compositor::kNoTexture) {
            ROCK_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s", ToUtf8Display(texturePath).c_str());
            id = textures.AddMissing(texturePath, ReadString(node, "name"));
        }
        textureIds[ReadInt(node, "id", 0)] = id;
    }
    const TextureReader readTexture = [&textureIds](const json& value) {
        const auto it = textureIds.find(value.is_number_integer() ? value.get<int>() : 0);
        return (it != textureIds.end()) ? it->second : compositor::kNoTexture;
    };
    std::vector<compositor::MaterialAssetId> added;
    for (const json& node : document.at("materials")) {
        const std::string uid = ReadString(node, "uid");
        const auto& entries = materials.Entries();
        const auto existing = std::find_if(entries.begin(), entries.end(),
                                           [&uid](const compositor::MaterialAsset& a) { return a.assetUid == uid; });
        if (!uid.empty() && existing != entries.end()) {
            materialIds[ReadInt(node, "id", 0)] = existing->id;
            continue;
        }
        const compositor::MaterialAssetId id = materials.Add(ReadString(node, "name"));
        compositor::MaterialAsset* asset = materials.FindMutable(id);
        ReadMaterialBody(node, *asset, readTexture);
        added.push_back(id);
        asset->assetUid = uid;
        asset->assetPath = FromUtf8(ReadString(node, "_assetPath"));
        asset->thumbnailDirty = true;
        materialIds[ReadInt(node, "id", 0)] = id;
    }
    for (const auto id : added) RemapLayerReferences(*materials.FindMutable(id), materialIds);
}

}  // namespace

bool LoadSharedAsset(ProjectWorkspace& workspace, const std::filesystem::path& path,
                     rhi::Device& device, rhi::PipelineCache& pipelineCache,
                     compositor::TextureLibrary& textures, compositor::MaterialLibrary& materials,
                     renderer::SkyLibrary& skies, bool rescan,
                     std::vector<renderer::ModelAsset>* models) {
    if (rescan && !workspace.Scan()) {
        return false;
    }
    // 読み込みではアセットの原本を書き換えない（Reference は .meta を作ることがある）。
    json header;
    if (!workspace.Contains(path) || !ProjectWorkspace::ReadJson(path, header)) {
        return false;
    }
    const std::string assetUid = ReadString(header, "uid");
    if (assetUid.empty()) {
        return false;
    }
    const json reference = {{"uid", assetUid}, {"path", RelativePathString(path, workspace.Root())}};
    const bool isMaterial = _wcsicmp(path.extension().c_str(), L".rockmat") == 0 || _wcsicmp(path.extension().c_str(), L".tglayer") == 0;
    const bool isModel = _wcsicmp(path.extension().c_str(), L".rockmodel") == 0;
    if (isModel && models == nullptr) {
        return false;
    }
    json document;
    document[isMaterial ? "materials" : isModel ? "models" : "skies"] = json::array({{{"id", 1}, {"asset", reference}}});
    if (!workspace.Expand(document)) {
        return false;
    }
    std::unordered_map<int, compositor::TextureId> textureIds;
    std::unordered_map<int, compositor::MaterialAssetId> materialIds;
    AddExpandedLibraries(document, device, pipelineCache, textures, materials, textureIds, materialIds);
    for (const json& node : document["models"]) {
        const std::string uid = ReadString(node, "uid");
        if (std::any_of(models->begin(), models->end(), [&uid](const renderer::ModelAsset& a) { return a.assetUid == uid; })) {
            continue;
        }
        renderer::ModelAsset asset;
        asset.id = 1;
        for (const renderer::ModelAsset& existing : *models) asset.id = std::max(asset.id, existing.id + 1);
        asset.assetUid = uid;
        asset.assetPath = FromUtf8(ReadString(node, "_assetPath"));
        asset.name = ReadString(node, "name");
        asset.path = FromUtf8(ReadString(node, "path"));
        asset.scale = ReadModelScale(node);
        if (!renderer::LoadModel(asset.path, asset)) {
            ROCK_LOG_WARN("モデルを読み込めません（%s）: %s", asset.error.c_str(), ToUtf8Display(asset.path).c_str());
        }
        const json& slots = node.at("materials");
        asset.materials.resize(std::max(asset.materials.size(), slots.size()), compositor::kNoMaterialAsset);
        for (size_t i = 0; i < slots.size(); ++i) {
            const auto found = slots[i].is_number_integer() ? materialIds.find(slots[i].get<int>()) : materialIds.end();
            if (found != materialIds.end()) asset.materials[i] = found->second;
        }
        models->push_back(std::move(asset));
    }
    for (const json& node : document["skies"]) {
        const std::string uid = ReadString(node, "uid");
        const auto& entries = skies.Entries();
        const auto existing = std::find_if(entries.begin(), entries.end(),
                                           [&uid](const renderer::SkyAsset& a) { return a.assetUid == uid; });
        if (existing == entries.end()) {
            skies.SetActive(ReadSky(node, skies, workspace.Root()));
        } else {
            skies.SetActive(existing->id);
        }
    }
    ROCK_LOG_INFO("アセットを読み込みました: %s", ToUtf8Display(path).c_str());
    return true;
}

bool SaveMaterial(const std::filesystem::path& path, const compositor::MaterialAsset& asset,
                  const compositor::TextureLibrary& textures) {
    if (asset.layerMaterial) { ROCK_LOG_ERROR("レイヤーマテリアルは共有アセットとして保存してください"); return false; }
    // SaveProject と同じく、裸のファイル名でも相対パスが作れるよう絶対化する。
    std::error_code absoluteError;
    const fs::path absolutePath = fs::absolute(path, absoluteError);
    const fs::path& savePath = absoluteError ? path : absolutePath;
    const fs::path baseDir = savePath.parent_path();

    // 単体ファイルでは、テクスチャをこのファイルからの相対パスで参照する。
    const TextureWriter writeTexture = [&textures, &baseDir](compositor::TextureId id) {
        const compositor::LibraryTexture* entry = textures.Find(id);
        if (entry == nullptr) {
            return json();
        }
        return json(RelativePathString(entry->path, baseDir));
    };

    json document = WriteMaterialBody(asset, writeTexture);
    document["format"] = kMaterialFormat;
    document["version"] = kMaterialFormatVersion;
    document["app"] = ROCK_APP_VERSION;

    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    ROCK_LOG_INFO("マテリアルを書き出しました: %s", ToUtf8Portable(savePath).c_str());
    return true;
}

compositor::MaterialAssetId LoadMaterial(const std::filesystem::path& path, rhi::Device& device,
                                         rhi::PipelineCache& pipelineCache,
                                         compositor::TextureLibrary& textures,
                                         compositor::MaterialLibrary& materials) {
    json document;
    if (!ReadJsonFile(path, kMaterialFormat, kMaterialFormatVersion, document)) {
        return compositor::kNoMaterialAsset;
    }

    const fs::path baseDir = path.parent_path();
    // 参照している画像はその場で読み込む。すでに同じ画像があれば読み直さない。
    const TextureReader readTexture = [&](const json& node) {
        if (!node.is_string()) {
            return compositor::kNoTexture;
        }
        const fs::path texturePath = ResolvePath(node.get<std::string>(), baseDir);
        if (texturePath.empty()) {
            return compositor::kNoTexture;
        }
        const compositor::TextureId id = textures.Load(device, pipelineCache, texturePath);
        if (id == compositor::kNoTexture) {
            // プロジェクトと同じく、見つからない画像はリンク切れとして残す。
            ROCK_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
                        ToUtf8Portable(texturePath).c_str());
            return textures.AddMissing(texturePath, std::string());
        }
        return id;
    };

    const compositor::MaterialAssetId id = materials.Add("マテリアル");
    compositor::MaterialAsset* asset = materials.FindMutable(id);
    if (asset == nullptr) {
        return compositor::kNoMaterialAsset;
    }
    ReadMaterialBody(document, *asset, readTexture);
    asset->thumbnailDirty = true;

    ROCK_LOG_INFO("マテリアルを読み込みました: %s", ToUtf8Portable(path).c_str());
    return id;
}

}  // namespace rock::io
