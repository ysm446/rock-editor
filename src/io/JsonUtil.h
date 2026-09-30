#pragma once
// 保存ファイルの JSON を読み書きする小さな道具。ProjectIo と GraphIo が共有する。
// GPU に依存しない（compositor のデータ構造にだけ依存する）。
#include "compositor/MaterialLayer.h"
#include "core/PathUtf8.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <system_error>

namespace rock::io::detail {

namespace fs = std::filesystem;
using nlohmann::json;

// --- 文字列とパス ---------------------------------------------------------
//
// JSON は UTF-8。変換は core/PathUtf8.h に一本化してある。
// 保存する文字列は区切りを '/' に揃える（ToUtf8Portable）。

// baseDir から見た相対パスにする。ドライブが違うなど relative が使えないときは
// 絶対パスのまま書く。プロジェクトごと移動しても壊れないようにするため。
inline std::string RelativePathString(const fs::path& target, const fs::path& baseDir) {
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
inline fs::path ResolvePath(const std::string& text, const fs::path& baseDir) {
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

inline const json* FindMember(const json& node, const char* key) {
    const auto it = node.find(key);
    return (it != node.end()) ? &(*it) : nullptr;
}

inline float ReadFloat(const json& node, const char* key, float fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_number()) ? member->get<float>() : fallback;
}

inline int ReadInt(const json& node, const char* key, int fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_number_integer()) {
        return fallback;
    }
    // int に収まらない値を切り詰めると、別のノードやピンの ID と衝突する。
    const int64_t value = member->get<int64_t>();
    return (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
               ? fallback
               : static_cast<int>(value);
}

inline uint32_t ReadUInt(const json& node, const char* key, uint32_t fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_number_integer()) {
        return fallback;
    }
    const int64_t value = member->get<int64_t>();
    return (value < 0) ? fallback : static_cast<uint32_t>(value);
}

inline bool ReadBool(const json& node, const char* key, bool fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_boolean()) ? member->get<bool>() : fallback;
}

inline std::string ReadString(const json& node, const char* key, const std::string& fallback = {}) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_string()) ? member->get<std::string>() : fallback;
}

// モデルの倍率。0 以下や非有限は 1（そのまま）に落とす。
inline float ReadModelScale(const json& node) {
    const float scale = ReadFloat(node, "scale", 1.0f);
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

inline json WriteFloat3(const DirectX::XMFLOAT3& value) {
    return json::array({value.x, value.y, value.z});
}

inline DirectX::XMFLOAT3 ReadFloat3(const json& node, const char* key,
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

inline constexpr const char* kTextureChannelNames[] = {"r", "g", "b", "a"};
inline constexpr const char* kValueSourceNames[] = {"constant", "noise", "texture"};
inline constexpr const char* kNoiseTypeNames[] = {"fbm",    "ridged", "worley",
                                      "perlin", "billow", "cracks"};
inline constexpr const char* kChannelNames[] = {"baseColor", "normal", "surface", "height"};
inline constexpr const char* kTonemapNames[] = {"none", "reinhard", "aces"};
inline constexpr const char* kSkySourceNames[] = {"procedural", "hdri"};
inline constexpr const char* kApertureShapeNames[] = {"circle", "triangle", "hexagon", "octagon"};

template <size_t N>
const char* EnumName(const char* const (&names)[N], uint32_t value) {
    return (value < N) ? names[value] : names[0];
}

template <size_t N>
inline uint32_t EnumValue(const char* const (&names)[N], const json& node, const char* key,
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

using TextureWriterFn = std::function<json(compositor::TextureId)>;
using TextureReaderFn = std::function<compositor::TextureId(const json&)>;

inline json WriteMapSlot(const compositor::MapSlot& slot, const TextureWriterFn& writeTexture) {
    json node;
    node["texture"] = writeTexture(slot.texture);
    node["channel"] = EnumName(kTextureChannelNames, static_cast<uint32_t>(slot.channel));
    return node;
}

inline compositor::MapSlot ReadMapSlot(const json& node, const char* key,
                                const TextureReaderFn& readTexture) {
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

inline json WriteNoise(const compositor::NoiseParams& noise) {
    json node;
    node["type"] = EnumName(kNoiseTypeNames, static_cast<uint32_t>(noise.type));
    node["scale"] = noise.scale;
    node["amount"] = noise.amount;
    node["octaves"] = noise.octaves;
    node["offset"] = noise.offset;
    return node;
}

inline compositor::NoiseParams ReadNoise(const json& node, const char* key,
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

}  // namespace rock::io::detail
