#include "io/ProjectIo.h"
#include "io/SurfaceLayoutIo.h"

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

namespace tg::io {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kProjectFormat = "terrain-graph.project";
constexpr const char* kMaterialFormat = "terrain-graph.material";
// 形式を変えたら上げる。読み込み側は「これ以下なら読める」として扱う。
//
// 2: ハイトに gain を追加し、base の意味を変えた（h = base + (src - 0.5) * gain）。
//    キーが増えただけに見えるが base の解釈が変わっているので、古いビルドに
//    読ませると黙って違う絵が出る。それを断れるように版を上げている。
// 3: レイヤーに kind（surface / shape / liquid）を追加した。古いビルドはキーを
//    無視してシェイプや水面をサーフェスとして合成し、黙って違う絵を出すため版を上げる。
//    kind の無い旧ファイルは全レイヤーをサーフェスとして読む。
// プロジェクトの版。4 で `layers` 節を廃止し、グラフ (`graph`) を唯一の合成にした
// （旧ファイルの layers はグラフへ移行して読む）。
// 5: 任意のメッシュシーン（手入力の scene。現在は読み飛ばす）。
// 8: road / meshOutput ノード。9: Road の Material 入力。10: roadMarking ノード。
// 11: Path の縦断ポイント・バンクポイント。旧ビルドが線形を平坦・水平に読むことを防ぐ。
// 12: Road の材質スロット 2〜4 と roadMask ノード。旧ビルドがスロット 2〜4 のリンクを捨てて下地だけを出すことを防ぐ。
// 13: Path の Surface 入力（Mesh 型）と surfaceSpace、decal ノード。
// 14: shoulder ノード。旧ビルドが路肩を読み飛ばして Outer 以降のリンクを失うことを防ぐ。
// 15: merge ノード（入力数が可変）。旧ビルドが Merge を読み飛ばして Mesh Output との接続を失うことを防ぐ。
// 16: crack ノード。
// 17: 埋込プリセットと道路・沿道の配置記述。旧ビルドによる消失を防ぐ。
// 24: 中央線・外側線・車線境界線の線幅を独立させる。
// 25: 白線・Decal・CrackのMaterial入力をプロパティへ移す。
// 26: Decalのハイト加算・画像倍率・帯ワイヤーフレーム。
// 27: モデル（models）。旧ビルドが読み飛ばして保存し直し、モデルとスロットの割り当てを失うことを防ぐ。
// 28: model / transform ノードと、道路・モデルの両方を受ける Merge / Mesh Output。旧ビルドが接続を失うことを防ぐ。
constexpr int kProjectFormatVersion = 28;
// マテリアル単体 (.tgmat) の版。中身は変わっていないので 3 のまま。
constexpr int kMaterialFormatVersion = 3;

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
// プロジェクトへの埋め込みと .tgmat で同じ形を使う。違うのはテクスチャ参照の書き方だけ。

// compositor::MaterialMap の並び。maps のキーと同じ名前。
const char* const kMaterialMapNames[] = {"baseColor", "normal", "roughness", "metallic",
                                         "ambientOcclusion", "height", "opacity"};
static_assert(std::size(kMaterialMapNames) == static_cast<size_t>(compositor::MaterialMap::Count));

json WriteMaterialBody(const compositor::MaterialAsset& asset, const TextureWriter& writeTexture) {
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
json WritePath(const graph::PathSettings& path) {
    json node;
    node["worldSpace"] = true;
    if (path.surfaceSpace) node["surfaceSpace"] = true;
    json points = json::array();
    for (const graph::PathPoint& point : path.points) {
        json item;
        item["id"] = point.id;
        item["position"] = json::array({point.x, point.y, point.z});
        item["width"] = point.widthMeters;
        item["feather"] = point.featherMeters;
        item["intensity"] = point.intensity;
        if (point.stopLine != graph::PathStopLine::None) item["stopLine"] = static_cast<int>(point.stopLine);
        points.push_back(std::move(item));
    }
    node["points"] = std::move(points);
    json edges = json::array();
    static const char* const kPathCurveNames[] = {"line", "quadratic", "cubic", "clothoid"};
    for (const graph::PathEdge& edge : path.edges) {
        json item;
        item["id"] = edge.id;
        item["from"] = edge.from;
        item["to"] = edge.to;
        item["curve"] = EnumName(kPathCurveNames, static_cast<uint32_t>(edge.curve));
        item["rounding"] = edge.rounding;
        item["clothoidRatio"] = edge.clothoidRatio;
        // 幅の上書き。切っているときは書かない（点の値を使う）。
        if (edge.overrideValues) {
            item["overrideValues"] = true;
            item["width"] = edge.widthMeters;
            item["feather"] = edge.featherMeters;
            item["intensity"] = edge.intensity;
        }
        edges.push_back(std::move(item));
    }
    node["edges"] = std::move(edges);
    node["defaultWidth"] = path.defaultWidthMeters;
    node["defaultFeather"] = path.defaultFeatherMeters;
    node["defaultIntensity"] = path.defaultIntensity;
    // 道路線形。縦断ポイントとバンクポイントは無ければ書かない。
    {
        if (!path.verticalPoints.empty()) {
            json vertical = json::array();
            for (const graph::PathVerticalPoint& point : path.verticalPoints) {
                vertical.push_back({{"id", point.id}, {"u", point.u}, {"vcl", point.vclMeters},
                                    {"offset", point.offsetMeters}});
            }
            node["verticalPoints"] = std::move(vertical);
        }
        if (!path.bankPoints.empty()) {
            json bank = json::array();
            for (const graph::PathBankPoint& point : path.bankPoints) {
                bank.push_back({{"id", point.id}, {"u", point.u}, {"designSpeed", point.designSpeedKmh},
                                {"manual", point.manual}, {"angle", point.angleDegrees}});
            }
            node["bankPoints"] = std::move(bank);
        }
        node["bankEnabled"] = path.bankEnabled;
        node["designSpeed"] = path.designSpeedKmh;
        node["friction"] = path.frictionCoefficient;
        node["smoothBank"] = path.smoothBank;
        node["bankSmoothDistance"] = path.bankSmoothMeters;
    }
    node["nextId"] = path.nextId;
    return node;
}

graph::PathSettings ReadPath(const json& parent, const char* key) {
    graph::PathSettings path;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return path;
    }
    // 旧地形 UV のパス（worldSpace が偽）は読まない。地形の平面が無くなったので実寸へ直せない。
    // キーは読んで判定だけに使い、点とエッジを捨てて空の実寸パスにする。
    if (!ReadBool(*node, "worldSpace", false)) {
        TG_LOG_WARN("旧地形 UV の Path は読み込めないため、空の Path にしました");
        return path;
    }
    path.surfaceSpace = ReadBool(*node, "surfaceSpace", false);
    const graph::PathSettings defaults;
    path.defaultWidthMeters = ReadFloat(*node, "defaultWidth", defaults.defaultWidthMeters);
    path.defaultFeatherMeters = ReadFloat(*node, "defaultFeather", defaults.defaultFeatherMeters);
    path.defaultIntensity = ReadFloat(*node, "defaultIntensity", defaults.defaultIntensity);
    graph::PathElementId maxId = 0;
    if (const json* points = FindMember(*node, "points"); points != nullptr && points->is_array()) {
        for (const json& item : *points) {
            if (!item.is_object()) {
                continue;
            }
            graph::PathPoint point;
            point.id = ReadInt(item, "id", 0);
            if (point.id <= 0) {
                continue;
            }
            point.widthMeters = ReadFloat(item, "width", path.defaultWidthMeters);
            point.featherMeters = ReadFloat(item, "feather", path.defaultFeatherMeters);
            point.intensity = ReadFloat(item, "intensity", path.defaultIntensity);
            point.stopLine = static_cast<graph::PathStopLine>(std::clamp(ReadInt(item, "stopLine", 0), 0, 3));
            const auto position = ReadFloat3(item, "position", {0.0f, 0.0f, 0.0f});
            point.x = position.x;
            point.y = position.y;
            point.z = position.z;
            maxId = std::max(maxId, point.id);
            path.points.push_back(point);
        }
    }
    if (const json* edges = FindMember(*node, "edges"); edges != nullptr && edges->is_array()) {
        for (const json& item : *edges) {
            if (!item.is_object()) {
                continue;
            }
            static const char* const kPathCurveNames[] = {"line", "quadratic", "cubic",
                                                          "clothoid"};
            graph::PathEdge edge;
            edge.id = ReadInt(item, "id", 0);
            edge.from = ReadInt(item, "from", 0);
            edge.to = ReadInt(item, "to", 0);
            edge.curve = static_cast<graph::PathCurve>(EnumValue(
                kPathCurveNames, item, "curve", static_cast<uint32_t>(graph::PathCurve::Line)));
            edge.rounding = std::clamp(ReadFloat(item, "rounding", 1.0f), 0.0f, 1.0f);
            edge.clothoidRatio = std::clamp(ReadFloat(item, "clothoidRatio", 0.5f), 0.0f, 1.0f);
            if (const json* override = FindMember(item, "overrideValues");
                override != nullptr && override->is_boolean() && override->get<bool>()) {
                edge.overrideValues = true;
                edge.widthMeters = ReadFloat(item, "width", path.defaultWidthMeters);
                edge.featherMeters = ReadFloat(item, "feather", path.defaultFeatherMeters);
                edge.intensity = ReadFloat(item, "intensity", path.defaultIntensity);
            }
            // 旧ファイルの経路探索（route / waypoints）は読まない（地形が無くなったため）。
            // 端点が無い / 自分へ戻るエッジは捨てる（壊れたファイルの安全網）。
            if (edge.id <= 0 || edge.from == edge.to || path.FindPoint(edge.from) == nullptr ||
                path.FindPoint(edge.to) == nullptr) {
                continue;
            }
            maxId = std::max(maxId, edge.id);
            path.edges.push_back(edge);
        }
    }
    {
        path.bankEnabled = ReadBool(*node, "bankEnabled", defaults.bankEnabled);
        path.designSpeedKmh = std::clamp(ReadFloat(*node, "designSpeed", defaults.designSpeedKmh), 0.0f, 300.0f);
        path.frictionCoefficient = std::clamp(ReadFloat(*node, "friction", defaults.frictionCoefficient), 0.0f, 1.0f);
        path.smoothBank = ReadBool(*node, "smoothBank", defaults.smoothBank);
        path.bankSmoothMeters = std::clamp(ReadFloat(*node, "bankSmoothDistance", defaults.bankSmoothMeters), 0.0f, 500.0f);
        if (const json* vertical = FindMember(*node, "verticalPoints"); vertical != nullptr && vertical->is_array()) {
            for (const json& item : *vertical) {
                if (!item.is_object()) continue;
                graph::PathVerticalPoint point;
                point.id = ReadInt(item, "id", 0);
                if (point.id <= 0) continue;
                point.u = std::clamp(ReadFloat(item, "u", 0.0f), 0.0f, 1.0f);
                point.vclMeters = std::clamp(ReadFloat(item, "vcl", point.vclMeters), 0.0f, 10000.0f);
                point.offsetMeters = std::clamp(ReadFloat(item, "offset", 0.0f), -1000.0f, 1000.0f);
                maxId = std::max(maxId, point.id);
                path.verticalPoints.push_back(point);
            }
        }
        if (const json* bank = FindMember(*node, "bankPoints"); bank != nullptr && bank->is_array()) {
            for (const json& item : *bank) {
                if (!item.is_object()) continue;
                graph::PathBankPoint point;
                point.id = ReadInt(item, "id", 0);
                if (point.id <= 0) continue;
                point.u = std::clamp(ReadFloat(item, "u", 0.0f), 0.0f, 1.0f);
                point.designSpeedKmh = std::clamp(ReadFloat(item, "designSpeed", path.designSpeedKmh), 0.0f, 300.0f);
                point.manual = ReadBool(item, "manual", false);
                point.angleDegrees = std::clamp(ReadFloat(item, "angle", 0.0f), -90.0f, 90.0f);
                maxId = std::max(maxId, point.id);
                path.bankPoints.push_back(point);
            }
        }
    }
    path.nextId = std::max(ReadInt(*node, "nextId", 1), maxId + 1);
    return path;
}

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
                const std::function<json(uint64_t)>& writeModel = {}) {
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
        if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
            item["layer"] = WriteLayer(settings->layer, writeMaterial);
        } else if (const auto* road = std::get_if<graph::RoadNodeSettings>(&node.settings)) {
            item["road"] = {{"width", road->widthMeters}, {"uvRepeat", road->uvRepeatMeters},
                            {"lanesForward", road->lanesForward}, {"lanesBackward", road->lanesBackward},
                            {"displacement", road->displacementMeters}, {"uvAlongU", road->uvAlongU},
                            {"layerWorldUv", json::array({road->layerWorldUv[0], road->layerWorldUv[1],
                                                          road->layerWorldUv[2], road->layerWorldUv[3]})},
                            {"layerUvRepeat", json::array({road->layerUvRepeatMeters[0], road->layerUvRepeatMeters[1],
                                                           road->layerUvRepeatMeters[2], road->layerUvRepeatMeters[3]})},
                            {"layerHeightGate", json::array({road->layerHeightGate[0], road->layerHeightGate[1],
                                                            road->layerHeightGate[2], road->layerHeightGate[3]})},
                            {"layerHeightGateThreshold", json::array({road->layerHeightGateThreshold[0], road->layerHeightGateThreshold[1],
                                                                     road->layerHeightGateThreshold[2], road->layerHeightGateThreshold[3]})},
                            {"layerHeightGateSoftness", json::array({road->layerHeightGateSoftness[0], road->layerHeightGateSoftness[1],
                                                                    road->layerHeightGateSoftness[2], road->layerHeightGateSoftness[3]})},
                            {"layerBlendMode", json::array({road->layerBlendMode[0], road->layerBlendMode[1],
                                                           road->layerBlendMode[2], road->layerBlendMode[3]})},
                            {"layerBlendRange", road->layerBlendRange}};
        } else if (const auto* decal = std::get_if<graph::DecalNodeSettings>(&node.settings)) {
            item["decal"] = {{"material", decal->material ? WriteLayer(*decal->material, writeMaterial) : json()}, {"width", decal->widthMeters}, {"lift", decal->liftMeters},
                             {"uvRepeat", decal->uvRepeatMeters}, {"uvAlongU", decal->uvAlongU},
                             {"heightMeters", decal->heightMeters}, {"imageWidthScale", decal->imageWidthScale},
                             {"imageLengthScale", decal->imageLengthScale}, {"showWireframe", decal->showWireframe}};
        } else if (const auto* crack = std::get_if<graph::CrackNodeSettings>(&node.settings)) {
            static const char* const kCrackOrientationNames[] = {"longitudinal", "transverse", "mixed"};
            static const char* const kCrackPlacementNames[] = {"uniform", "wheelTracks", "edges"};
            item["crack"] = {{"material", crack->material ? WriteLayer(*crack->material, writeMaterial) : json()}, {"seed", crack->seed}, {"density", crack->densityPer100m},
                             {"lengthMin", crack->lengthMinMeters}, {"lengthMax", crack->lengthMaxMeters},
                             {"orientation", EnumName(kCrackOrientationNames, static_cast<uint32_t>(crack->orientation))},
                             {"transverseRatio", crack->transverseRatio}, {"angleJitter", crack->angleJitterDegrees},
                             {"placement", EnumName(kCrackPlacementNames, static_cast<uint32_t>(crack->placement))},
                             {"trunkWidth", crack->trunkWidthMeters},
                             {"branchesMin", crack->branchesMin}, {"branchesMax", crack->branchesMax},
                             {"branchLengthRatio", crack->branchLengthRatio}, {"branchWidthRatio", crack->branchWidthRatio},
                             {"lift", crack->liftMeters}, {"uvRepeat", crack->uvRepeatMeters}, {"uvAlongU", crack->uvAlongU}};
        } else if (const auto* shoulder = std::get_if<graph::ShoulderNodeSettings>(&node.settings)) {
            item["shoulder"] = {{"width", shoulder->widthMeters}, {"crossSlope", shoulder->crossSlopePercent},
                                {"stepHeight", shoulder->stepHeightMeters}, {"stepWidth", shoulder->stepWidthMeters},
                                {"uvRepeat", shoulder->uvRepeatMeters}, {"uvAlongU", shoulder->uvAlongU},
                                {"displacement", shoulder->displacementMeters},
                                {"layerWorldUv", json::array({shoulder->layerWorldUv[0], shoulder->layerWorldUv[1],
                                                              shoulder->layerWorldUv[2], shoulder->layerWorldUv[3]})},
                                {"layerUvRepeat", json::array({shoulder->layerUvRepeatMeters[0], shoulder->layerUvRepeatMeters[1],
                                                               shoulder->layerUvRepeatMeters[2], shoulder->layerUvRepeatMeters[3]})},
                            {"layerHeightGate", json::array({shoulder->layerHeightGate[0], shoulder->layerHeightGate[1],
                                                            shoulder->layerHeightGate[2], shoulder->layerHeightGate[3]})},
                            {"layerHeightGateThreshold", json::array({shoulder->layerHeightGateThreshold[0], shoulder->layerHeightGateThreshold[1],
                                                                     shoulder->layerHeightGateThreshold[2], shoulder->layerHeightGateThreshold[3]})},
                            {"layerHeightGateSoftness", json::array({shoulder->layerHeightGateSoftness[0], shoulder->layerHeightGateSoftness[1],
                                                                    shoulder->layerHeightGateSoftness[2], shoulder->layerHeightGateSoftness[3]})},
                                {"layerBlendMode", json::array({shoulder->layerBlendMode[0], shoulder->layerBlendMode[1],
                                                           shoulder->layerBlendMode[2], shoulder->layerBlendMode[3]})},
                                {"layerBlendRange", shoulder->layerBlendRange}};
        } else if (const auto* roadMaskSettings = std::get_if<graph::RoadMaskNodeSettings>(&node.settings)) {
            static const char* const kRoadMaskShapeNames[] = {"wheelTracks", "edgeFalloff", "lengthNoise", "constant", "worldNoise"};
            static const char* const kRoadMaskSideNames[] = {"both", "left", "right"};
            item["roadMask"] = {{"shape", EnumName(kRoadMaskShapeNames, static_cast<uint32_t>(roadMaskSettings->shape))},
                                {"laneOffset", roadMaskSettings->laneOffsetMeters},
                                {"trackSpacing", roadMaskSettings->trackSpacingMeters},
                                {"trackWidth", roadMaskSettings->trackWidthMeters},
                                {"feather", roadMaskSettings->featherMeters},
                                {"bothLanes", roadMaskSettings->bothLanes},
                                {"tracksFromLanes", roadMaskSettings->tracksFromLanes},
                                {"edgeWidth", roadMaskSettings->edgeWidthMeters},
                                {"edgeSide", EnumName(kRoadMaskSideNames, static_cast<uint32_t>(roadMaskSettings->edgeSide))},
                                {"noiseScale", roadMaskSettings->noiseScaleMeters},
                                {"threshold", roadMaskSettings->threshold},
                                {"softness", roadMaskSettings->softness},
                                {"seed", roadMaskSettings->seed},
                                {"breakupAmount", roadMaskSettings->breakupAmount},
                                {"breakupScale", roadMaskSettings->breakupScaleMeters},
                                {"strength", roadMaskSettings->strength},
                                {"invert", roadMaskSettings->invert}};
        } else if (const auto* marking = std::get_if<graph::RoadMarkingNodeSettings>(&node.settings)) {
            item["roadMarking"] = {{"centerLineWidth", marking->centerLineWidthMeters},
                                   {"edgeLineWidth", marking->edgeLineWidthMeters},
                                   {"laneLineWidth", marking->laneLineWidthMeters},
                                   {"centerLine", marking->centerLine},
                                   {"centerLineDashed", marking->centerLineDashed},
                                   {"edgeLines", marking->edgeLines},
                                   {"edgeInset", marking->edgeInsetMeters},
                                   {"laneLines", marking->laneLines},
                                   {"dashLength", marking->dashLengthMeters},
                                   {"dashGap", marking->dashGapMeters},
                                   {"stopLines", marking->stopLines},
                                   {"stopLineWidth", marking->stopLineWidthMeters},
                                   {"lift", marking->liftMeters},
                                   {"uvRepeat", marking->uvRepeatMeters},
                                   {"arrows", marking->arrows},
                                   {"arrowInterval", marking->arrowIntervalMeters},
                                   {"arrowLength", marking->arrowLengthMeters},
                                   {"uvAlongU", marking->uvAlongU}};
            item["roadMarking"]["materials"] = json::array();
            for (const auto& material : marking->materials)
                item["roadMarking"]["materials"].push_back(material ? WriteLayer(*material, writeMaterial) : json());
        } else if (const auto* path = std::get_if<graph::PathNodeSettings>(&node.settings)) {
            item["path"] = WritePath(path->path);
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
    out["roadNetwork"] = {{"leftHandTraffic", graphData.RoadNetwork().leftHandTraffic}};
    return out;
}

// 戻り値はノードを 1 つ以上読めたか。空のグラフ節は「グラフ未使用」とみなし、
// 呼び出し側が旧 layers からの移行に切り替える。
// readModel は Model ノードの文書内の番号を実行中のモデル ID へ写す（0 = なし）。
bool ReadGraph(const json& node, graph::NodeGraph& graphData,
               const std::function<compositor::MaterialAssetId(const json&)>& readMaterial,
               const std::function<uint64_t(const json&)>& readModel = {}) {
    std::vector<graph::Node> nodes;
    std::vector<graph::Link> links;
    std::vector<std::pair<graph::GraphId, graph::GraphId>> legacyMaterialInputs;
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
            // 旧Materialピンは末尾の入力。リンクを落とす前に接続先のSurface設定を移す。
            const bool decalNode = created.kind == graph::NodeKind::Decal;
            const char* materialKey = created.kind == graph::NodeKind::RoadMarking ? "roadMarking" :
                (created.kind == graph::NodeKind::Crack ? "crack" : (decalNode ? "decal" : nullptr));
            const size_t legacyIndex = decalNode ? 2 : 1;
            const json* materialSettings = materialKey ? FindMember(item, materialKey) : nullptr;
            const char* bindingKey = created.kind == graph::NodeKind::RoadMarking ? "materials" : "material";
            if (materialKey && (!materialSettings || !FindMember(*materialSettings, bindingKey)) &&
                inputIds && inputIds->is_array() && inputIds->size() > legacyIndex && (*inputIds)[legacyIndex].is_number_integer())
                legacyMaterialInputs.emplace_back(created.id, (*inputIds)[legacyIndex].get<int>());

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

            if (graph::IsLayerNodeKind(created.kind)) {
                graph::LayerNodeSettings settings;
                if (const json* layer = FindMember(item, "layer");
                    layer != nullptr && layer->is_object()) {
                    settings.layer = ReadLayer(*layer, readMaterial);
                }
                created.settings = std::move(settings);
            } else if (created.kind == graph::NodeKind::Road) {
                graph::RoadNodeSettings settings;
                if (const json* road = FindMember(item, "road"); road && road->is_object()) {
                    settings.widthMeters = ReadFloat(*road, "width", settings.widthMeters);
                    settings.lanesForward = static_cast<uint32_t>(std::clamp(ReadInt(*road, "lanesForward", static_cast<int>(settings.lanesForward)), 1, 8));
                    settings.lanesBackward = static_cast<uint32_t>(std::clamp(ReadInt(*road, "lanesBackward", static_cast<int>(settings.lanesBackward)), 0, 8));
                    settings.uvRepeatMeters = ReadFloat(*road, "uvRepeat", settings.uvRepeatMeters);
                    settings.displacementMeters = std::clamp(ReadFloat(*road, "displacement", 0.0f), 0.0f, 5.0f);
                    settings.uvAlongU = ReadBool(*road, "uvAlongU", settings.uvAlongU);
                    if (const json* worldUv = FindMember(*road, "layerWorldUv"); worldUv && worldUv->is_array()) {
                        for (size_t i = 0; i < worldUv->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*worldUv)[i].is_boolean()) settings.layerWorldUv[i] = (*worldUv)[i].get<bool>();
                    }
                    if (const json* repeat = FindMember(*road, "layerUvRepeat"); repeat && repeat->is_array()) {
                        for (size_t i = 0; i < repeat->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*repeat)[i].is_number()) settings.layerUvRepeatMeters[i] = std::clamp((*repeat)[i].get<float>(), 0.1f, 100.0f);
                    }
                    if (const json* gate = FindMember(*road, "layerHeightGate"); gate && gate->is_array()) {
                        for (size_t i = 0; i < gate->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*gate)[i].is_number_integer()) settings.layerHeightGate[i] = static_cast<uint32_t>(std::clamp((*gate)[i].get<int>(), 0, 2));
                    }
                    if (const json* gate = FindMember(*road, "layerHeightGateThreshold"); gate && gate->is_array()) {
                        for (size_t i = 0; i < gate->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*gate)[i].is_number()) settings.layerHeightGateThreshold[i] = std::clamp((*gate)[i].get<float>(), 0.0f, 1.0f);
                    }
                    if (const json* gate = FindMember(*road, "layerHeightGateSoftness"); gate && gate->is_array()) {
                        for (size_t i = 0; i < gate->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*gate)[i].is_number()) settings.layerHeightGateSoftness[i] = std::clamp((*gate)[i].get<float>(), 0.001f, 1.0f);
                    }
                    // 混ぜ方。キーが無い旧ファイルは「ハイトで競合」（以前の見た目のまま）。
                    for (auto& mode : settings.layerBlendMode) mode = 1u;
                    if (const json* modes = FindMember(*road, "layerBlendMode"); modes && modes->is_array()) {
                        for (size_t i = 0; i < modes->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*modes)[i].is_number_integer()) settings.layerBlendMode[i] = static_cast<uint32_t>(std::clamp((*modes)[i].get<int>(), 0, 1));
                    }
                    settings.layerBlendRange = std::clamp(ReadFloat(*road, "layerBlendRange", settings.layerBlendRange), 0.0f, 1.0f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Decal) {
                graph::DecalNodeSettings settings;
                if (const json* decal = FindMember(item, "decal"); decal && decal->is_object()) {
                    if (const json* material = FindMember(*decal, "material"); material && material->is_object())
                        settings.material = ReadLayer(*material, readMaterial);
                    settings.heightMeters = ReadFloat(*decal, "heightMeters", settings.heightMeters);
                    settings.imageWidthScale = ReadFloat(*decal, "imageWidthScale", settings.imageWidthScale);
                    settings.imageLengthScale = ReadFloat(*decal, "imageLengthScale", settings.imageLengthScale);
                    settings.showWireframe = ReadBool(*decal, "showWireframe", settings.showWireframe);
                    settings.widthMeters = ReadFloat(*decal, "width", settings.widthMeters);
                    settings.liftMeters = ReadFloat(*decal, "lift", settings.liftMeters);
                    settings.uvRepeatMeters = ReadFloat(*decal, "uvRepeat", settings.uvRepeatMeters);
                    settings.uvAlongU = ReadBool(*decal, "uvAlongU", settings.uvAlongU);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Crack) {
                graph::CrackNodeSettings settings;
                if (const json* crack = FindMember(item, "crack"); crack && crack->is_object()) {
                    if (const json* material = FindMember(*crack, "material"); material && material->is_object())
                        settings.material = ReadLayer(*material, readMaterial);
                    static const char* const kCrackOrientationNames[] = {"longitudinal", "transverse", "mixed"};
                    static const char* const kCrackPlacementNames[] = {"uniform", "wheelTracks", "edges"};
                    settings.seed = static_cast<uint32_t>(std::max(0, ReadInt(*crack, "seed", static_cast<int>(settings.seed))));
                    settings.densityPer100m = ReadFloat(*crack, "density", settings.densityPer100m);
                    settings.lengthMinMeters = ReadFloat(*crack, "lengthMin", settings.lengthMinMeters);
                    settings.lengthMaxMeters = ReadFloat(*crack, "lengthMax", settings.lengthMaxMeters);
                    settings.orientation = static_cast<graph::CrackOrientation>(
                        EnumValue(kCrackOrientationNames, *crack, "orientation", static_cast<uint32_t>(settings.orientation)));
                    settings.transverseRatio = ReadFloat(*crack, "transverseRatio", settings.transverseRatio);
                    settings.angleJitterDegrees = ReadFloat(*crack, "angleJitter", settings.angleJitterDegrees);
                    settings.placement = static_cast<graph::CrackPlacement>(
                        EnumValue(kCrackPlacementNames, *crack, "placement", static_cast<uint32_t>(settings.placement)));
                    settings.trunkWidthMeters = ReadFloat(*crack, "trunkWidth", settings.trunkWidthMeters);
                    settings.branchesMin = static_cast<uint32_t>(std::clamp(ReadInt(*crack, "branchesMin", static_cast<int>(settings.branchesMin)), 0, 12));
                    settings.branchesMax = static_cast<uint32_t>(std::clamp(ReadInt(*crack, "branchesMax", static_cast<int>(settings.branchesMax)), 0, 12));
                    settings.branchLengthRatio = ReadFloat(*crack, "branchLengthRatio", settings.branchLengthRatio);
                    settings.branchWidthRatio = ReadFloat(*crack, "branchWidthRatio", settings.branchWidthRatio);
                    settings.liftMeters = ReadFloat(*crack, "lift", settings.liftMeters);
                    settings.uvRepeatMeters = ReadFloat(*crack, "uvRepeat", settings.uvRepeatMeters);
                    settings.uvAlongU = ReadBool(*crack, "uvAlongU", settings.uvAlongU);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Shoulder) {
                graph::ShoulderNodeSettings settings;
                if (const json* shoulder = FindMember(item, "shoulder"); shoulder && shoulder->is_object()) {
                    settings.widthMeters = ReadFloat(*shoulder, "width", settings.widthMeters);
                    settings.crossSlopePercent = ReadFloat(*shoulder, "crossSlope", settings.crossSlopePercent);
                    settings.stepHeightMeters = std::clamp(ReadFloat(*shoulder, "stepHeight", settings.stepHeightMeters), 0.0f, 0.5f);
                    settings.stepWidthMeters = std::clamp(ReadFloat(*shoulder, "stepWidth", settings.stepWidthMeters), 0.005f, 1.0f);
                    settings.uvRepeatMeters = ReadFloat(*shoulder, "uvRepeat", settings.uvRepeatMeters);
                    settings.uvAlongU = ReadBool(*shoulder, "uvAlongU", settings.uvAlongU);
                    settings.displacementMeters = std::clamp(ReadFloat(*shoulder, "displacement", settings.displacementMeters), 0.0f, 1.0f);
                    if (const json* worldUv = FindMember(*shoulder, "layerWorldUv"); worldUv && worldUv->is_array()) {
                        for (size_t i = 0; i < worldUv->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*worldUv)[i].is_boolean()) settings.layerWorldUv[i] = (*worldUv)[i].get<bool>();
                    }
                    if (const json* repeat = FindMember(*shoulder, "layerUvRepeat"); repeat && repeat->is_array()) {
                        for (size_t i = 0; i < repeat->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*repeat)[i].is_number()) settings.layerUvRepeatMeters[i] = std::clamp((*repeat)[i].get<float>(), 0.1f, 100.0f);
                    }
                    if (const json* gate = FindMember(*shoulder, "layerHeightGate"); gate && gate->is_array()) {
                        for (size_t i = 0; i < gate->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*gate)[i].is_number_integer()) settings.layerHeightGate[i] = static_cast<uint32_t>(std::clamp((*gate)[i].get<int>(), 0, 2));
                    }
                    if (const json* gate = FindMember(*shoulder, "layerHeightGateThreshold"); gate && gate->is_array()) {
                        for (size_t i = 0; i < gate->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*gate)[i].is_number()) settings.layerHeightGateThreshold[i] = std::clamp((*gate)[i].get<float>(), 0.0f, 1.0f);
                    }
                    if (const json* gate = FindMember(*shoulder, "layerHeightGateSoftness"); gate && gate->is_array()) {
                        for (size_t i = 0; i < gate->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*gate)[i].is_number()) settings.layerHeightGateSoftness[i] = std::clamp((*gate)[i].get<float>(), 0.001f, 1.0f);
                    }
                    // 混ぜ方。キーが無い旧ファイルは「ハイトで競合」（以前の見た目のまま）。
                    for (auto& mode : settings.layerBlendMode) mode = 1u;
                    if (const json* modes = FindMember(*shoulder, "layerBlendMode"); modes && modes->is_array()) {
                        for (size_t i = 0; i < modes->size() && i < graph::kRoadMaterialSlots; ++i)
                            if ((*modes)[i].is_number_integer()) settings.layerBlendMode[i] = static_cast<uint32_t>(std::clamp((*modes)[i].get<int>(), 0, 1));
                    }
                    settings.layerBlendRange = std::clamp(ReadFloat(*shoulder, "layerBlendRange", settings.layerBlendRange), 0.0f, 1.0f);
                }
                created.settings = settings;
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

            } else if (created.kind == graph::NodeKind::RoadMask) {
                graph::RoadMaskNodeSettings settings;
                if (const json* mask = FindMember(item, "roadMask"); mask && mask->is_object()) {
                    static const char* const kRoadMaskShapeNames[] = {"wheelTracks", "edgeFalloff", "lengthNoise", "constant", "worldNoise"};
                    settings.shape = static_cast<graph::RoadMaskShape>(
                        EnumValue(kRoadMaskShapeNames, *mask, "shape", static_cast<uint32_t>(settings.shape)));
                    settings.laneOffsetMeters = ReadFloat(*mask, "laneOffset", settings.laneOffsetMeters);
                    settings.trackSpacingMeters = ReadFloat(*mask, "trackSpacing", settings.trackSpacingMeters);
                    settings.trackWidthMeters = ReadFloat(*mask, "trackWidth", settings.trackWidthMeters);
                    settings.featherMeters = ReadFloat(*mask, "feather", settings.featherMeters);
                    settings.bothLanes = ReadBool(*mask, "bothLanes", settings.bothLanes);
                    // キーが無い旧ファイルは手入力のまま（既定の真にすると見た目が変わる）。
                    settings.tracksFromLanes = ReadBool(*mask, "tracksFromLanes", false);
                    settings.edgeWidthMeters = ReadFloat(*mask, "edgeWidth", settings.edgeWidthMeters);
                    static const char* const kRoadMaskSideNames[] = {"both", "left", "right"};
                    settings.edgeSide = static_cast<graph::RoadMaskSide>(
                        EnumValue(kRoadMaskSideNames, *mask, "edgeSide", static_cast<uint32_t>(settings.edgeSide)));
                    settings.noiseScaleMeters = ReadFloat(*mask, "noiseScale", settings.noiseScaleMeters);
                    settings.threshold = ReadFloat(*mask, "threshold", settings.threshold);
                    settings.softness = ReadFloat(*mask, "softness", settings.softness);
                    settings.seed = static_cast<uint32_t>(std::max(0, ReadInt(*mask, "seed", static_cast<int>(settings.seed))));
                    settings.breakupAmount = ReadFloat(*mask, "breakupAmount", settings.breakupAmount);
                    settings.breakupScaleMeters = ReadFloat(*mask, "breakupScale", settings.breakupScaleMeters);
                    settings.strength = ReadFloat(*mask, "strength", settings.strength);
                    settings.invert = ReadBool(*mask, "invert", settings.invert);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::RoadMarking) {
                graph::RoadMarkingNodeSettings settings;
                if (const json* marking = FindMember(item, "roadMarking"); marking && marking->is_object()) {
                    if (const json* materials = FindMember(*marking, "materials"); materials && materials->is_array())
                        for (size_t i = 0; i < settings.materials.size() && i < materials->size(); ++i)
                            if ((*materials)[i].is_object()) settings.materials[i] = ReadLayer((*materials)[i], readMaterial);
                    const float legacyWidth = ReadFloat(*marking, "lineWidth", settings.centerLineWidthMeters);
                    settings.centerLineWidthMeters = ReadFloat(*marking, "centerLineWidth", legacyWidth);
                    settings.edgeLineWidthMeters = ReadFloat(*marking, "edgeLineWidth", legacyWidth);
                    settings.laneLineWidthMeters = ReadFloat(*marking, "laneLineWidth", legacyWidth);
                    settings.centerLine = ReadBool(*marking, "centerLine", settings.centerLine);
                    settings.centerLineDashed = ReadBool(*marking, "centerLineDashed", settings.centerLineDashed);
                    settings.edgeLines = ReadBool(*marking, "edgeLines", settings.edgeLines);
                    settings.edgeInsetMeters = ReadFloat(*marking, "edgeInset", settings.edgeInsetMeters);
                    settings.laneLines = ReadBool(*marking, "laneLines", settings.laneLines);
                    settings.dashLengthMeters = ReadFloat(*marking, "dashLength", settings.dashLengthMeters);
                    settings.dashGapMeters = ReadFloat(*marking, "dashGap", settings.dashGapMeters);
                    settings.stopLines = ReadBool(*marking, "stopLines", settings.stopLines);
                    settings.stopLineWidthMeters = ReadFloat(*marking, "stopLineWidth", settings.stopLineWidthMeters);
                    settings.liftMeters = ReadFloat(*marking, "lift", settings.liftMeters);
                    settings.uvRepeatMeters = ReadFloat(*marking, "uvRepeat", settings.uvRepeatMeters);
                    settings.arrows = ReadBool(*marking, "arrows", settings.arrows);
                    settings.arrowIntervalMeters = ReadFloat(*marking, "arrowInterval", settings.arrowIntervalMeters);
                    settings.arrowLengthMeters = ReadFloat(*marking, "arrowLength", settings.arrowLengthMeters);
                    settings.uvAlongU = ReadBool(*marking, "uvAlongU", settings.uvAlongU);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Path) {
                graph::PathNodeSettings settings;
                settings.path = ReadPath(item, "path");
                created.settings = std::move(settings);
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
    for (const auto& [nodeId, pinId] : legacyMaterialInputs) {
        const auto link = std::find_if(links.begin(), links.end(), [&](const auto& value) { return value.endPin == pinId; });
        if (link == links.end()) continue;
        const compositor::MaterialLayer* layer = nullptr;
        for (const auto& source : nodes) {
            if (std::none_of(source.outputs.begin(), source.outputs.end(), [&](const auto& pin) { return pin.id == link->startPin; })) continue;
            if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&source.settings)) layer = &settings->layer;
            break;
        }
        if (!layer) continue;
        for (auto& target : nodes) {
            if (target.id != nodeId) continue;
            if (auto* settings = std::get_if<graph::RoadMarkingNodeSettings>(&target.settings)) settings->materials.fill(*layer);
            if (auto* settings = std::get_if<graph::DecalNodeSettings>(&target.settings)) settings->material = *layer;
            if (auto* settings = std::get_if<graph::CrackNodeSettings>(&target.settings)) settings->material = *layer;
        }
    }
    // Replace が壊れたリンクの除去と次の採番の再構築を行う。
    graphData.Replace(std::move(nodes), std::move(links));
    {
        graph::RoadNetworkSettings roadNetwork;
        if (const json* network = FindMember(node, "roadNetwork"); network != nullptr && network->is_object()) {
            roadNetwork.leftHandTraffic = ReadBool(*network, "leftHandTraffic", roadNetwork.leftHandTraffic);
        }
        graphData.SetRoadNetwork(roadNetwork);
    }
    return true;
}

// 旧形式（版 3 以前）の layers[] をグラフへ移行する。
// 下から上のレイヤー列を Surface の「下地」チェーンとして繋ぐ。
// 旧地形のノード（Shape / Liquid / Output）は無くなったので、種類はすべて Surface にする。
graph::NodeGraph MigrateLayersToGraph(std::vector<compositor::MaterialLayer> layers) {
    graph::NodeGraph migrated;
    if (layers.empty()) {
        return graph::NodeGraph::CreateDefault();
    }
    graph::GraphId previousOutput = 0;
    float x = 60.0f;
    for (compositor::MaterialLayer& layer : layers) {
        const graph::GraphId nodeId = migrated.CreateNode(graph::NodeKind::Surface);
        graph::Node* node = migrated.FindMutableNode(nodeId);
        if (node == nullptr) {
            continue;
        }
        if (auto* settings = std::get_if<graph::LayerNodeSettings>(&node->settings)) {
            settings->layer = std::move(layer);
        }
        node->posX = x;
        node->posY = 120.0f;
        node->positionValid = true;
        x += 240.0f;
        if (previousOutput != 0 && !node->inputs.empty()) {
            migrated.CreateLink(previousOutput, node->inputs.front().id);
        }
        previousOutput = node->outputs.empty() ? 0 : node->outputs.front().id;
    }
    return migrated;
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
            TG_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
        // 人が読める形で書く。差分も取りやすい。壊れた文字列が混ざっていても
        // 例外を出さない（不正な UTF-8 は置換文字にする）。
        stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        // バッファの最終書き込み・closeの失敗も、元ファイルの差し替え前に検出する。
        stream.close();
        if (!stream.good()) {
            TG_LOG_ERROR("ファイルの書き込みに失敗しました: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
    }

    std::error_code renameError;
    fs::rename(tempPath, path, renameError);
    if (renameError) {
        TG_LOG_ERROR("ファイルを差し替えられませんでした: %s", ToUtf8Portable(path).c_str());
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
        TG_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // 例外は使わない方針なので、パース失敗は discarded で受ける。
    outDocument = json::parse(stream, nullptr, false);
    if (outDocument.is_discarded() || !outDocument.is_object()) {
        TG_LOG_ERROR("JSON として読めませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // material-mixer 時代のファイルは "material-mixer.project" などの形式名を持つ。
    // 中身は同じなので、旧形式名は新形式名へ読み替えて受け付ける（書くのは新形式名のみ）。
    std::string format = ReadString(outDocument, "format");
    if (format.rfind("material-mixer.", 0) == 0) {
        format = "terrain-graph." + format.substr(std::string("material-mixer.").size());
    }
    if (format != expectedFormat) {
        TG_LOG_ERROR("形式が違います（%s ではなく %s）: %s", expectedFormat, format.c_str(),
                     ToUtf8Portable(path).c_str());
        return false;
    }
    const int version = ReadInt(outDocument, "version", 0);
    if (version > maxVersion) {
        TG_LOG_ERROR("このバージョンでは読めません（ファイル %d > 対応 %d）: %s", version,
                     maxVersion, ToUtf8Portable(path).c_str());
        return false;
    }
    return true;
}

}  // namespace

bool SaveProject(const std::filesystem::path& path, const ProjectRefs& refs,
                 ProjectWorkspace* workspace) {
    std::string layoutError;
    if (!graph::ValidateSurfaceLayouts(refs.surfaceLayouts, layoutError) ||
        !graph::ValidateSurfaceLayoutRoads(refs.surfaceLayouts, refs.graph, layoutError)) {
        TG_LOG_ERROR("配置データを保存できません: %s", layoutError.c_str());
        return false;
    }
    // 裸のファイル名（親ディレクトリ無し）で保存すると相対パスが作れず、
    // 全参照が絶対パスで書かれてしまう。先に絶対化してから基準を取る。
    std::error_code absoluteError;
    const fs::path absolutePath = fs::absolute(path, absoluteError);
    const fs::path& savePath = absoluteError ? path : absolutePath;
    const fs::path baseDir = savePath.parent_path();

    // シーンとして保存するときは、先に共有アセットを各ファイルへ書く。
    // ここで失敗したら文書には触らない（片方だけ新しい状態を作らない）。
    if (workspace != nullptr) {
        if (_wcsicmp(savePath.extension().c_str(), L".tgscene") != 0 || !workspace->Contains(savePath)) {
            TG_LOG_ERROR("シーンはプロジェクトルート内の .tgscene へ保存してください: %s",
                         ToUtf8Display(savePath).c_str());
            return false;
        }
        if (!SaveSharedAssets(*workspace, refs)) {
            TG_LOG_ERROR("共有アセットを保存できないため、シーンの保存を中止しました");
            return false;
        }
    }

    json document;
    document["format"] = kProjectFormat;
    document["version"] = kProjectFormatVersion;
    document["app"] = TG_APP_VERSION;

    // --- テクスチャ（画像は参照。パスはプロジェクトからの相対） -----------
    // ファイルの中では通し番号で参照する。実行中の ID をそのまま書くと、
    // 削除して番号が飛んだときにファイルが読みにくくなる。
    std::unordered_map<compositor::TextureId, int> textureIndex;
    json textures = json::array();
    for (const compositor::LibraryTexture& entry : refs.textures.Entries()) {
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
    document["graph"] = WriteGraph(refs.graph, writeMaterial, writeModel);
    auto layouts = WriteSurfaceLayouts(refs.surfaceLayouts);
    if (layouts.is_null()) { TG_LOG_ERROR("レイヤーマテリアルの移行に必要なIDを確保できません"); return false; }
    for (auto& preset : layouts["layerMaterials"]) {
        if (preset.contains("materialGraph")) for (auto& node : preset["materialGraph"]["nodes"]) {
            auto& material = node["settings"]["material"];
            const auto reference = writeMaterial(material.get<uint32_t>());
            material = reference.is_null() ? json(0) : reference;
        }
        for (auto& material : preset["materials"]) {
            const auto reference = writeMaterial(material["material"].get<uint32_t>());
            material["material"] = reference.is_null() ? json(0) : reference;
        }
    }
    for (auto& boundary : layouts["boundaryMaterials"]) for (const auto* key : {"mask", "height"}) {
        const auto reference = writeTexture(boundary[key].get<uint32_t>());
        boundary[key] = reference.is_null() ? json(0) : reference;
    }
    if (workspace != nullptr) {
        // 共有アセットの置き場所と固定 ID（SaveSharedAssets が付けたもの）。SaveScene がこれを見てファイルへ分ける。
        const auto identify = [](json& list, const auto& entries) {
            for (json& value : list)
                for (const auto& entry : entries)
                    if (value["id"] == entry.id) {
                        value["_assetPath"] = ToUtf8Portable(entry.assetPath);
                        value["uid"] = entry.assetUid;
                    }
        };
        identify(layouts["layerMaterials"], refs.surfaceLayouts.layerMaterials);
        identify(layouts["boundaryMaterials"], refs.surfaceLayouts.boundaryMaterials);
    }
    document["surfaceLayouts"] = std::move(layouts);

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
    document["preview"]["surfaceBands"] = refs.previewSurfaceBands;
    document["preview"]["connectSurfaceBands"] = refs.connectSurfaceBands;
    document["preview"]["displaceConnectedBands"] = refs.displaceConnectedBands;

    if (workspace != nullptr) {
        if (!workspace->SaveScene(savePath, document)) {
            TG_LOG_ERROR("シーンを保存できませんでした: %s", ToUtf8Display(savePath).c_str());
            return false;
        }
        TG_LOG_INFO("シーンを保存しました: %s", ToUtf8Display(savePath).c_str());
        return true;
    }
    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    TG_LOG_INFO("プロジェクトを保存しました: %s", ToUtf8Portable(savePath).c_str());
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
            TG_LOG_ERROR("シーンまたは参照アセットを開けません: %s", ToUtf8Display(path).c_str());
            return false;
        }
        if (const int version = ReadInt(document, "version", 0); version > kProjectFormatVersion) {
            TG_LOG_ERROR("このバージョンでは読めません（シーン %d > 対応 %d）: %s", version,
                         kProjectFormatVersion, ToUtf8Display(path).c_str());
            return false;
        }
    } else if (!ReadJsonFile(path, kProjectFormat, kProjectFormatVersion, document)) {
        return false;
    }

    graph::SurfaceLayoutDocument pendingLayouts;
    if (ReadInt(document, "version", 0) >= 17 && FindMember(document, "surfaceLayouts") == nullptr) {
        TG_LOG_ERROR("版17の配置データが欠落しています（現在の文書は保持）");
        return false;
    }
    if (const json* layouts = FindMember(document, "surfaceLayouts")) {
        std::string error;
        if (!ReadSurfaceLayouts(*layouts, pendingLayouts, error) || !graph::ExtractLayerMaterials(pendingLayouts, error)) {
            TG_LOG_ERROR("配置データを読み込めません（現在の文書は保持）: %s", error.c_str());
            return false;
        }
        // 共有アセットの置き場所と固定 ID（シーンの展開で入ったもの）。旧 .tgproj には無い。
        const auto identify = [&](const char* key, auto& entries) {
            const json* list = FindMember(*layouts, key);
            if (list == nullptr || !list->is_array()) return;
            for (const json& value : *list)
                for (auto& entry : entries)
                    if (value.is_object() && value.contains("id") && value["id"] == entry.id) {
                        entry.assetPath = FromUtf8(ReadString(value, "_assetPath"));
                        entry.assetUid = ReadString(value, "uid");
                    }
        };
        identify("layerMaterials", pendingLayouts.layerMaterials);
        identify("boundaryMaterials", pendingLayouts.boundaryMaterials);
        if (!pendingLayouts.layouts.empty()) {
            graph::NodeGraph validationGraph;
            const auto* graphValue = FindMember(document, "graph");
            if (!graphValue || !graphValue->is_object() ||
                !ReadGraph(*graphValue, validationGraph, [](const json&) { return compositor::kNoMaterialAsset; }) ||
                !graph::ValidateSurfaceLayoutRoads(pendingLayouts, validationGraph, error)) {
                TG_LOG_ERROR("配置先Roadを確認できません（現在の文書は保持）: %s", error.c_str());
                return false;
            }
        }
    }
    const fs::path baseDir = path.parent_path();

    // 旧ファイルの手入力メッシュシーン（scene）は読まない。表示するメッシュは
    // グラフの Mesh Output から生成する。
    if (FindMember(document, "scene") != nullptr) {
        TG_LOG_WARN("旧形式の手入力メッシュシーン（scene）は読み飛ばしました");
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
                TG_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
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
                    TG_LOG_WARN("モデルを読み込めません（%s）: %s", asset.error.c_str(),
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
    for (auto& preset : pendingLayouts.layerMaterials) {
        for (auto& material : preset.materials) material.material = readMaterial(json(material.material));
        if (preset.materialGraph) for (auto& node : preset.materialGraph->nodes)
            node.settings.material = readMaterial(json(node.settings.material));
    }
    for (auto& boundary : pendingLayouts.boundaryMaterials) {
        boundary.mask = readTexture(json(boundary.mask)); boundary.height = readTexture(json(boundary.height));
    }
    refs.surfaceLayouts = std::move(pendingLayouts);
    // 旧形式の layers[]（版 3 以前）。移行用に一旦読み込んでおく。
    std::vector<compositor::MaterialLayer> legacyLayers;
    if (const json* layers = FindMember(document, "layers");
        layers != nullptr && layers->is_array()) {
        for (const json& node : *layers) {
            if (!node.is_object()) {
                continue;
            }
            legacyLayers.push_back(ReadLayer(node, readMaterial));
        }
    }

    // グラフの決め方。
    //   版 4 以降: graph 節が唯一の合成（無ければ既定へ戻す）。
    //   版 3 以前: 「プレビューに適用」（apply）がオンで保存されていれば graph 節を、
    //             そうでなければ layers[] をグラフへ移行して使う
    //             （当時プレビューに出ていた側を正とする）。
    const int version = ReadInt(document, "version", 0);
    const json* graphNode = FindMember(document, "graph");
    bool graphLoaded = false;
    if (graphNode != nullptr && graphNode->is_object()) {
        const bool legacyApply = ReadBool(*graphNode, "apply", version >= 4);
        if (version >= 4 || legacyApply || legacyLayers.empty()) {
            const std::function<uint64_t(const json&)> readModel = [&modelIds](const json& value) -> uint64_t {
                if (!value.is_number_integer()) return 0;
                const auto found = modelIds.find(value.get<int>());
                return found != modelIds.end() ? found->second : 0;
            };
            graphLoaded = ReadGraph(*graphNode, refs.graph, readMaterial, readModel);
        }
    }
    if (!graphLoaded) {
        if (!legacyLayers.empty()) {
            TG_LOG_INFO("旧形式のレイヤーをノードグラフへ移行しました（%zu 枚）",
                        legacyLayers.size());
            refs.graph = MigrateLayersToGraph(std::move(legacyLayers));
        } else {
            refs.graph = graph::NodeGraph::CreateDefault();
        }
    }

    // preview が無い（または壊れている）プロジェクトでも必ず既定値で埋める。
    // 呼ばないと、前のプロジェクトのカメラ・ライト・露出が残ってしまう。
    const json* preview = FindMember(document, "preview");
    const json emptyPreview = json::object();
    const json& previewNode =
        (preview != nullptr && preview->is_object()) ? *preview : emptyPreview;
    ReadPreview(previewNode, refs.renderer);
    refs.previewSurfaceBands = ReadBool(previewNode, "surfaceBands", false);
    refs.connectSurfaceBands = ReadBool(previewNode, "connectSurfaceBands", false);
    refs.displaceConnectedBands = ReadBool(previewNode, "displaceConnectedBands", false);

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

    TG_LOG_INFO("プロジェクトを開きました: %s", ToUtf8Portable(path).c_str());
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
    for (const graph::LayerMaterial& entry : refs.surfaceLayouts.layerMaterials) {
        if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
    }
    for (const compositor::BoundaryMaterial& entry : refs.surfaceLayouts.boundaryMaterials) {
        if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
    }
    if (refs.models != nullptr) {
        for (const renderer::ModelAsset& entry : *refs.models) {
            if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
        }
    }
    // 置き場所が未定のものの保存先。ID の無いもの（旧 .tgproj・単体 .tgmat から来たもの）は、
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
    for (const compositor::MaterialAsset& entry : refs.materials.Entries()) {
        compositor::MaterialAsset* asset = refs.materials.FindMutable(entry.id);
        json body = WriteMaterialBody(*asset, writeTexture);
        body["uid"] = asset->assetUid;
        fs::path assetPath = placement(body, asset->assetPath, "material-asset", L"Materials", asset->name, ".tgmat");
        if (!valid || !workspace.SaveAsset(assetPath, "material-asset", body)) {
            TG_LOG_ERROR("マテリアルを保存できません: %s", asset->name.c_str());
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
        fs::path assetPath = placement(body, asset->assetPath, "sky-asset", L"Skies", asset->name, ".tgsky");
        if (!valid || !workspace.SaveAsset(assetPath, "sky-asset", body)) {
            TG_LOG_ERROR("天球を保存できません: %s", asset->name.c_str());
            return false;
        }
        asset->assetPath = assetPath;
        asset->assetUid = ReadString(body, "uid");
        claimedUids.insert(asset->assetUid);
    }
    // レイヤーマテリアル。本文は配置データの保存形式と同じで、マテリアルの参照だけを上で保存した
    // .tgmat の固定 ID にする（SaveScene が作る本文と一致させ、中身が変わらなければ書き直さない）。
    for (graph::LayerMaterial& material : refs.surfaceLayouts.layerMaterials) {
        graph::SurfaceLayoutDocument single;
        single.layerMaterials.push_back(material);
        const json written = WriteSurfaceLayouts(single);
        json body = written["layerMaterials"][0];
        const auto materialRef = [&](json& value) {
            const compositor::MaterialAsset* asset = refs.materials.Find(value.get<uint32_t>());
            value = (asset != nullptr && !asset->assetPath.empty()) ? workspace.Reference(asset->assetPath) : json();
            if (asset != nullptr && value.is_null()) valid = false;
        };
        for (json& layer : body["materials"]) materialRef(layer["material"]);
        if (body.contains("materialGraph")) {
            for (json& node : body["materialGraph"]["nodes"]) materialRef(node["settings"]["material"]);
        }
        body["uid"] = material.assetUid;
        fs::path assetPath = placement(body, material.assetPath, "layer-material-asset", L"LayerMaterials",
                                       material.name, ".tglayer");
        if (!valid || !workspace.SaveAsset(assetPath, "layer-material-asset", body)) {
            TG_LOG_ERROR("レイヤーマテリアルを保存できません: %s", material.name.c_str());
            return false;
        }
        material.assetPath = assetPath;
        material.assetUid = ReadString(body, "uid");
        claimedUids.insert(material.assetUid);
    }
    // モデル。FBX は元ファイルの固定 ID、スロットは上で保存した .tgmat の固定 ID で参照する
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
            fs::path assetPath = placement(body, model.assetPath, "model-asset", L"Models", model.name, ".tgmodel");
            if (!valid || !workspace.SaveAsset(assetPath, "model-asset", body)) {
                TG_LOG_ERROR("モデルを保存できません: %s", model.name.c_str());
                return false;
            }
            model.assetPath = assetPath;
            model.assetUid = ReadString(body, "uid");
            claimedUids.insert(model.assetUid);
        }
    }
    // 境界マテリアル。画像は元ファイルの固定 ID で参照する。
    for (compositor::BoundaryMaterial& material : refs.surfaceLayouts.boundaryMaterials) {
        graph::SurfaceLayoutDocument single;
        single.boundaryMaterials.push_back(material);
        const json written = WriteSurfaceLayouts(single);
        json body = written["boundaryMaterials"][0];
        body["mask"] = writeTexture(material.mask);
        body["height"] = writeTexture(material.height);
        body["uid"] = material.assetUid;
        fs::path assetPath = placement(body, material.assetPath, "boundary-material-asset", L"BoundaryMaterials",
                                       material.name, ".tgboundary");
        if (!valid || !workspace.SaveAsset(assetPath, "boundary-material-asset", body)) {
            TG_LOG_ERROR("境界マテリアルを保存できません: %s", material.name.c_str());
            return false;
        }
        material.assetPath = assetPath;
        material.assetUid = ReadString(body, "uid");
        claimedUids.insert(material.assetUid);
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
            TG_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s", ToUtf8Display(texturePath).c_str());
            id = textures.AddMissing(texturePath, ReadString(node, "name"));
        }
        textureIds[ReadInt(node, "id", 0)] = id;
    }
    const TextureReader readTexture = [&textureIds](const json& value) {
        const auto it = textureIds.find(value.is_number_integer() ? value.get<int>() : 0);
        return (it != textureIds.end()) ? it->second : compositor::kNoTexture;
    };
    for (const json& node : document.at("materials")) {
        const std::string uid = ReadString(node, "uid");
        const auto& entries = materials.Entries();
        const auto existing = std::find_if(entries.begin(), entries.end(),
                                           [&uid](const compositor::MaterialAsset& a) { return a.assetUid == uid; });
        if (existing != entries.end()) {
            materialIds[ReadInt(node, "id", 0)] = existing->id;
            continue;
        }
        const compositor::MaterialAssetId id = materials.Add(ReadString(node, "name"));
        compositor::MaterialAsset* asset = materials.FindMutable(id);
        ReadMaterialBody(node, *asset, readTexture);
        asset->assetUid = uid;
        asset->assetPath = FromUtf8(ReadString(node, "_assetPath"));
        asset->thumbnailDirty = true;
        materialIds[ReadInt(node, "id", 0)] = id;
    }
}

}  // namespace

graph::SurfaceId LoadSharedSurfaceAsset(ProjectWorkspace& workspace, const std::filesystem::path& path,
                                        rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                        compositor::TextureLibrary& textures, compositor::MaterialLibrary& materials,
                                        graph::SurfaceLayoutDocument& layouts) {
    if (!workspace.Scan()) {
        return 0;
    }
    json header;
    if (!workspace.Contains(path) || !ProjectWorkspace::ReadJson(path, header)) {
        return 0;
    }
    const std::string assetUid = ReadString(header, "uid");
    if (assetUid.empty()) {
        return 0;
    }
    const bool isLayer = _wcsicmp(path.extension().c_str(), L".tglayer") == 0;
    // 読み込み済みなら足さずにそれを使う（同じアセットを 2 つの番号で持たない）。
    if (isLayer) {
        for (const graph::LayerMaterial& entry : layouts.layerMaterials) {
            if (entry.assetUid == assetUid) return entry.id;
        }
    } else {
        for (const compositor::BoundaryMaterial& entry : layouts.boundaryMaterials) {
            if (entry.assetUid == assetUid) return entry.id;
        }
    }
    const char* key = isLayer ? "layerMaterials" : "boundaryMaterials";
    json document;
    document["surfaceLayouts"][key] = json::array(
        {{{"id", 1}, {"asset", {{"uid", assetUid}, {"path", RelativePathString(path, workspace.Root())}}}}});
    if (!workspace.Expand(document)) {
        return 0;
    }
    // 本文は配置データの読み込み器で検査する。参照の番号はまだ文書内のもの。
    const json& expanded = document.at("surfaceLayouts").at(key).at(0);
    json value = {{"version", 6}, {"nextId", 2}, {"presets", json::array()}, {"layouts", json::array()},
                  {"layerMaterials", json::array()}, {"boundaryMaterials", json::array()}};
    value[key].push_back(expanded);
    graph::SurfaceLayoutDocument parsed;
    std::string error;
    if (!ReadSurfaceLayouts(value, parsed, error)) {
        TG_LOG_ERROR("アセットを読み込めません（%s）: %s", error.c_str(), ToUtf8Display(path).c_str());
        return 0;
    }
    const graph::SurfaceId id = layouts.AllocateId();
    if (id == 0) {
        return 0;
    }
    std::unordered_map<int, compositor::TextureId> textureIds;
    std::unordered_map<int, compositor::MaterialAssetId> materialIds;
    AddExpandedLibraries(document, device, pipelineCache, textures, materials, textureIds, materialIds);
    if (isLayer) {
        const auto materialId = [&materialIds](uint32_t number) {
            const auto found = materialIds.find(static_cast<int>(number));
            return (found != materialIds.end()) ? found->second : compositor::kNoMaterialAsset;
        };
        graph::LayerMaterial material = parsed.layerMaterials.front();
        for (graph::PresetMaterial& layer : material.materials) layer.material = materialId(layer.material);
        if (material.materialGraph) {
            for (graph::PresetNode& node : material.materialGraph->nodes) node.settings.material = materialId(node.settings.material);
        }
        material.id = id;
        material.assetUid = assetUid;
        material.assetPath = FromUtf8(ReadString(expanded, "_assetPath"));
        layouts.layerMaterials.push_back(std::move(material));
    } else {
        const auto textureId = [&textureIds](uint32_t number) {
            const auto found = textureIds.find(static_cast<int>(number));
            return (found != textureIds.end()) ? found->second : compositor::kNoTexture;
        };
        compositor::BoundaryMaterial material = parsed.boundaryMaterials.front();
        material.mask = textureId(material.mask);
        material.height = textureId(material.height);
        material.id = id;
        material.assetUid = assetUid;
        material.assetPath = FromUtf8(ReadString(expanded, "_assetPath"));
        layouts.boundaryMaterials.push_back(std::move(material));
    }
    TG_LOG_INFO("アセットを読み込みました: %s", ToUtf8Display(path).c_str());
    return id;
}

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
    const bool isMaterial = _wcsicmp(path.extension().c_str(), L".tgmat") == 0;
    const bool isModel = _wcsicmp(path.extension().c_str(), L".tgmodel") == 0;
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
            TG_LOG_WARN("モデルを読み込めません（%s）: %s", asset.error.c_str(), ToUtf8Display(asset.path).c_str());
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
    TG_LOG_INFO("アセットを読み込みました: %s", ToUtf8Display(path).c_str());
    return true;
}

bool SaveMaterial(const std::filesystem::path& path, const compositor::MaterialAsset& asset,
                  const compositor::TextureLibrary& textures) {
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
    document["app"] = TG_APP_VERSION;

    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    TG_LOG_INFO("マテリアルを書き出しました: %s", ToUtf8Portable(savePath).c_str());
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
            TG_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
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

    TG_LOG_INFO("マテリアルを読み込みました: %s", ToUtf8Portable(path).c_str());
    return id;
}

}  // namespace tg::io
