#include "io/RockAssetIo.h"

#include "core/PathUtf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

namespace rock::io {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr const char* kFormat = "rock-editor.rock-asset";
// 版 2 で段ごとのテクスチャ（lods[].textures）を足した。版 1 も読む。
constexpr int kVersion = 2;
// .rockmesh の先頭。版を変えたら末尾の数字を上げる。
constexpr char kMeshMagic[8] = {'R', 'K', 'M', 'E', 'S', 'H', '0', '1'};

struct Hasher {
    uint64_t value = 14695981039346656037ull;
    void Bytes(const void* data, size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            value ^= p[i];
            value *= 1099511628211ull;
        }
    }
    template <class T> void Add(const T& v) { Bytes(&v, sizeof(v)); }
};

template <class T> bool Write(std::ofstream& out, const T* data, size_t count) {
    out.write(reinterpret_cast<const char*>(data), std::streamsize(sizeof(T) * count));
    return bool(out);
}
template <class T> bool Read(std::ifstream& in, T* data, size_t count) {
    in.read(reinterpret_cast<char*>(data), std::streamsize(sizeof(T) * count));
    return bool(in);
}

bool ReadManifest(const fs::path& scene, json& manifest) {
    std::ifstream in(RockAssetFolder(scene) / L"asset.json", std::ios::binary);
    if (!in) return false;
    manifest = json::parse(in, nullptr, false);
    const int version = manifest.is_object() ? manifest.value("version", 0) : 0;
    return manifest.is_object() && manifest.value("format", "") == kFormat && version >= 1 && version <= kVersion;
}
}  // namespace

fs::path RockAssetFolder(const fs::path& scene) {
    return fs::path(scene.wstring() + L".bake");
}

std::string RockAssetLodTextureFolder(size_t level) { return "lod" + std::to_string(level); }

std::string RockAssetHash(const std::vector<RockAssetLod>& lods, const std::string& bakeFingerprint) {
    Hasher h;
    h.Add(uint32_t(kVersion));
    h.Add(uint64_t(lods.size()));
    for (const auto& lod : lods) {
        const auto& mesh = lod.mesh;
        h.Add(lod.screenSize);
        h.Add(uint64_t(mesh.positions.size()));
        h.Bytes(mesh.positions.data(), mesh.positions.size() * sizeof(geometry::Vec3));
        h.Add(uint64_t(mesh.triangles.size()));
        h.Bytes(mesh.triangles.data(), mesh.triangles.size() * sizeof(mesh.triangles[0]));
        h.Add(uint64_t(mesh.cornerUvs.size()));
        h.Bytes(mesh.cornerUvs.data(), mesh.cornerUvs.size() * sizeof(mesh.cornerUvs[0]));
        h.Add(mesh.uvWidth);
        h.Add(mesh.uvHeight);
        h.Add(uint64_t(lod.textures.size()));
        h.Bytes(lod.textures.data(), lod.textures.size());
    }
    h.Bytes(bakeFingerprint.data(), bakeFingerprint.size());
    char text[17] = {};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(h.value));
    return text;
}

bool SaveRockMesh(const fs::path& path, const geometry::Mesh& mesh) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const uint64_t counts[4] = {mesh.positions.size(), mesh.triangles.size(), mesh.cornerUvs.size(), mesh.uvCharts.size()};
    const uint32_t atlas[2] = {mesh.uvWidth, mesh.uvHeight};
    return Write(out, kMeshMagic, 8) && Write(out, counts, 4) && Write(out, atlas, 2) &&
           Write(out, mesh.positions.data(), mesh.positions.size()) && Write(out, mesh.triangles.data(), mesh.triangles.size()) &&
           Write(out, mesh.cornerUvs.data(), mesh.cornerUvs.size()) && Write(out, mesh.uvCharts.data(), mesh.uvCharts.size());
}

bool LoadRockMesh(const fs::path& path, geometry::Mesh& mesh) {
    mesh = {};
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const uint64_t fileSize = uint64_t(in.tellg());
    in.seekg(0);
    char magic[8] = {};
    uint64_t counts[4] = {};
    uint32_t atlas[2] = {};
    if (!Read(in, magic, 8) || std::memcmp(magic, kMeshMagic, 8) != 0 || !Read(in, counts, 4) || !Read(in, atlas, 2)) return false;
    // 壊れた数で巨大な確保をしない。ファイルの大きさと突き合わせる。
    const uint64_t expected = 8 + sizeof(counts) + sizeof(atlas) + counts[0] * sizeof(geometry::Vec3) +
                              counts[1] * sizeof(mesh.triangles[0]) + counts[2] * sizeof(mesh.cornerUvs[0]) + counts[3] * sizeof(uint32_t);
    if (counts[0] > (1ull << 32) || counts[1] > (1ull << 32) || counts[2] > (1ull << 32) || counts[3] > (1ull << 32) ||
        expected != fileSize)
        return false;
    mesh.positions.resize(counts[0]);
    mesh.triangles.resize(counts[1]);
    mesh.cornerUvs.resize(counts[2]);
    mesh.uvCharts.resize(counts[3]);
    mesh.uvWidth = atlas[0];
    mesh.uvHeight = atlas[1];
    if (!Read(in, mesh.positions.data(), mesh.positions.size()) || !Read(in, mesh.triangles.data(), mesh.triangles.size()) ||
        !Read(in, mesh.cornerUvs.data(), mesh.cornerUvs.size()) || !Read(in, mesh.uvCharts.data(), mesh.uvCharts.size())) {
        mesh = {};
        return false;
    }
    for (const auto& face : mesh.triangles)
        for (const uint32_t index : face)
            if (index >= mesh.positions.size()) {
                mesh = {};
                return false;
            }
    return true;
}

bool SaveRockAsset(const fs::path& scene, const RockAssetData& data, std::string& error) {
    error.clear();
    if (data.lods.empty()) {
        error = "焼く段がありません";
        return false;
    }
    const fs::path folder = RockAssetFolder(scene);
    std::error_code fileError;
    fs::create_directories(folder, fileError);
    if (fileError) {
        error = "付属フォルダを作れません: " + ToUtf8Display(folder);
        return false;
    }
    json lods = json::array();
    for (size_t level = 0; level < data.lods.size(); ++level) {
        const std::string name = "lod" + std::to_string(level) + ".rockmesh";
        if (!SaveRockMesh(folder / name, data.lods[level].mesh)) {
            error = "メッシュを書けません: " + name;
            return false;
        }
        const std::string& textures = data.lods[level].textures;
        if (!textures.empty() && textures != RockAssetLodTextureFolder(level)) {
            error = "段のテクスチャのフォルダ名が不正です: " + textures;
            return false;
        }
        lods.push_back({{"mesh", name}, {"triangles", data.lods[level].mesh.triangles.size()},
                        {"screenSize", data.lods[level].screenSize}, {"textures", data.textured ? textures : std::string()}});
    }
    // 前に焼いた余分な段（段数を減らしたとき）と、使わなくなった段のテクスチャのフォルダを消す。
    for (size_t level = data.lods.size(); level < 16; ++level) fs::remove(folder / ("lod" + std::to_string(level) + ".rockmesh"), fileError);
    for (size_t level = 0; level < 16; ++level) {
        const bool used = data.textured && level < data.lods.size() && !data.lods[level].textures.empty();
        if (!used) fs::remove_all(folder / RockAssetLodTextureFolder(level), fileError);
    }
    // 直下の共有テクスチャは、共有する段が 1 つも無いかテクスチャを焼かないなら消す。
    const bool shared = data.textured && std::any_of(data.lods.begin(), data.lods.end(), [](const auto& lod) { return lod.textures.empty(); });
    if (!shared)
        for (const char* texture : kRockAssetTextures) fs::remove(folder / texture, fileError);
    const json manifest = {{"format", kFormat},
                           {"version", kVersion},
                           {"source", ToUtf8Portable(scene.filename())},
                           {"hash", data.hash},
                           {"textured", data.textured},
                           {"textures", data.textured ? json(kRockAssetTextures) : json::array()},
                           {"minimum", {data.minimum.x, data.minimum.y, data.minimum.z}},
                           {"maximum", {data.maximum.x, data.maximum.y, data.maximum.z}},
                           {"lods", std::move(lods)}};
    // 目録は最後に書く（途中で失敗したら古い目録が残り、ハッシュが合わないので「古い」と分かる）。
    const fs::path temporary = folder / L"asset.json.tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << manifest.dump(2);
        if (!out) {
            error = "目録を書けません";
            return false;
        }
    }
    fs::rename(temporary, folder / L"asset.json", fileError);
    if (fileError) {
        error = "目録を置き換えられません";
        return false;
    }
    return true;
}

bool LoadRockAsset(const fs::path& scene, RockAssetData& data, std::string& error) {
    data = {};
    error.clear();
    json manifest;
    if (!ReadManifest(scene, manifest)) {
        error = "焼いた岩アセットがありません（未焼成）";
        return false;
    }
    const fs::path folder = RockAssetFolder(scene);
    const json* lods = manifest.contains("lods") && manifest["lods"].is_array() ? &manifest["lods"] : nullptr;
    if (!lods || lods->empty()) {
        error = "岩アセットの目録に段がありません";
        return false;
    }
    for (const auto& entry : *lods) {
        RockAssetLod lod;
        const std::string name = entry.value("mesh", "");
        if (name.empty() || name.find_first_of("/\\") != std::string::npos || !LoadRockMesh(folder / FromUtf8(name), lod.mesh)) {
            error = "岩アセットのメッシュを読めません: " + name;
            data = {};
            return false;
        }
        lod.screenSize = entry.value("screenSize", 1.0f);
        // 版 1 には無い（全ての段が直下のテクスチャを共有する）。
        lod.textures = entry.contains("textures") && entry["textures"].is_string() ? entry["textures"].get<std::string>() : "";
        if (!lod.textures.empty() && lod.textures != RockAssetLodTextureFolder(data.lods.size())) {
            error = "岩アセットの段のテクスチャのフォルダ名が不正です: " + lod.textures;
            data = {};
            return false;
        }
        data.lods.push_back(std::move(lod));
    }
    data.textured = manifest.value("textured", false);
    if (data.textured) {
        std::error_code fileError;
        for (const auto& lod : data.lods)
            for (const char* texture : kRockAssetTextures)
                if (!fs::is_regular_file(folder / FromUtf8(lod.textures) / texture, fileError)) data.textured = false;
    }
    data.hash = manifest.value("hash", "");
    const auto vec = [&](const char* key) {
        geometry::Vec3 v{};
        if (manifest.contains(key) && manifest[key].is_array() && manifest[key].size() == 3)
            v = {manifest[key][0].get<float>(), manifest[key][1].get<float>(), manifest[key][2].get<float>()};
        return v;
    };
    data.minimum = vec("minimum");
    data.maximum = vec("maximum");
    return true;
}

bool ReadRockAssetHash(const fs::path& scene, std::string& hash) {
    json manifest;
    if (!ReadManifest(scene, manifest)) return false;
    hash = manifest.value("hash", "");
    return true;
}

bool ReadRockAssetBounds(const fs::path& scene, geometry::Vec3& minimum, geometry::Vec3& maximum) {
    json manifest;
    if (!ReadManifest(scene, manifest)) return false;
    const auto vec = [&](const char* key, geometry::Vec3& v) {
        if (!manifest.contains(key) || !manifest[key].is_array() || manifest[key].size() != 3) return false;
        for (const auto& item : manifest[key])
            if (!item.is_number()) return false;
        v = {manifest[key][0].get<float>(), manifest[key][1].get<float>(), manifest[key][2].get<float>()};
        return true;
    };
    return vec("minimum", minimum) && vec("maximum", maximum);
}

}  // namespace rock::io
