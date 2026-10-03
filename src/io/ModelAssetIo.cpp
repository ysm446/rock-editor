#include "io/ModelAssetIo.h"

#include "core/PathUtf8.h"

#include <nlohmann/json.hpp>
#include <ufbx.h>

#include <algorithm>
#include <fstream>
#include <memory>

namespace rock::io {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
fs::path Resolve(const std::string& text, const fs::path& root, const fs::path& assetDir) {
    if (text.empty()) return {};
    const fs::path raw = FromUtf8(text);
    std::error_code error;
    if (raw.is_absolute()) return raw;
    if (!root.empty() && fs::exists(root / raw, error)) return (root / raw).lexically_normal();
    // ルートが無い・見つからないときは、.tgmodel のフォルダに同名のファイルを探す（フォルダごと写した場合）。
    if (fs::exists(assetDir / raw.filename(), error)) return assetDir / raw.filename();
    if (!root.empty()) return (root / raw).lexically_normal();
    return assetDir / raw;
}
}  // namespace

bool IsModelAssetPath(const fs::path& path) {
    const auto ext = path.extension().wstring();
    return _wcsicmp(ext.c_str(), L".tgmodel") == 0 || _wcsicmp(ext.c_str(), L".model") == 0;
}

fs::path FindProjectRoot(const fs::path& start) {
    std::error_code error;
    for (fs::path folder = fs::absolute(start, error); !folder.empty(); folder = folder.parent_path()) {
        if (fs::exists(folder / L"project.reproj", error) || fs::exists(folder / L"project.tgproj", error)) return folder;
        if (folder == folder.parent_path()) break;
    }
    return {};
}

bool ReadModelAssetInfo(const fs::path& assetPath, ModelAssetInfo& info, std::string& error) {
    error.clear();
    info = {};
    std::ifstream file(assetPath, std::ios::binary);
    if (!file) {
        error = "モデル資産を開けません: " + ToUtf8Display(assetPath);
        return false;
    }
    json document;
    try {
        document = json::parse(file);
    } catch (const std::exception& e) {
        error = std::string("モデル資産の JSON が壊れています: ") + e.what();
        return false;
    }
    const std::string format = document.value("format", "");
    if (format != "terrain-graph.model-asset" && format != "rock-editor.model-asset") {
        error = "モデル資産の形式が違います: " + format;
        return false;
    }
    const fs::path root = FindProjectRoot(assetPath.parent_path());
    const fs::path assetDir = assetPath.parent_path();
    info.name = document.value("name", ToUtf8Display(assetPath.stem()));
    const auto refPath = [](const json& ref) -> std::string {
        if (ref.is_string()) return ref.get<std::string>();
        if (ref.is_object() && ref.contains("path") && ref["path"].is_string()) return ref["path"].get<std::string>();
        return {};
    };
    info.fbx = Resolve(refPath(document.value("source", json())), root, assetDir);
    if (info.fbx.empty()) {
        error = "モデル資産に FBX の参照がありません";
        return false;
    }
    if (document.contains("materials") && document["materials"].is_array())
        for (const json& slot : document["materials"]) info.materials.push_back(Resolve(refPath(slot), root, assetDir));
    if (document.contains("lodScreenSizes") && document["lodScreenSizes"].is_array())
        for (const json& v : document["lodScreenSizes"])
            if (v.is_number()) info.lodScreenSizes.push_back(v.get<float>());
    // インポスター（terrain-graph が焼いたもの）。画像の参照が切れていれば焼いていない扱い。
    if (document.contains("impostor") && document["impostor"].is_object()) {
        const json& value = document["impostor"];
        if (value.contains("baked") && value["baked"].is_object()) {
            const json& baked = value["baked"];
            ModelImpostorInfo& imp = info.impostor;
            imp.frames = static_cast<uint32_t>(std::clamp(baked.value("frames", 12), 2, 32));
            imp.frameSize = static_cast<uint32_t>(std::clamp(baked.value("frameSize", 256), 32, 2048));
            imp.fullSphere = baked.value("fullSphere", false);
            imp.radius = baked.value("radius", 0.0f);
            if (baked.contains("center") && baked["center"].is_array() && baked["center"].size() == 3)
                imp.center = {baked["center"][0].get<float>(), baked["center"][1].get<float>(), baked["center"][2].get<float>()};
            imp.color = Resolve(refPath(baked.value("color", json())), root, assetDir);
            imp.normal = Resolve(refPath(baked.value("normal", json())), root, assetDir);
            imp.variation = Resolve(refPath(baked.value("variation", json())), root, assetDir);
            std::error_code fsError;
            imp.baked = imp.radius > 0 && !imp.color.empty() && !imp.normal.empty() && fs::exists(imp.color, fsError) &&
                        fs::exists(imp.normal, fsError);
        }
    }
    return true;
}

bool ReadFbxBounds(const fs::path& fbx, geometry::Vec3& minimum, geometry::Vec3& maximum) {
    std::ifstream file(fbx, std::ios::binary);
    if (!file) return false;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.ignore_animation = true;
    opts.ignore_embedded = true;
    ufbx_error loadError{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &opts, &loadError), &ufbx_free_scene);
    if (!scene) return false;
    bool any = false;
    minimum = {1e30f, 1e30f, 1e30f};
    maximum = {-1e30f, -1e30f, -1e30f};
    for (const ufbx_node* node : scene->nodes) {
        if (!node->mesh) continue;
        const ufbx_mesh& mesh = *node->mesh;
        for (size_t i = 0; i < mesh.vertices.count; ++i) {
            const ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, mesh.vertices.data[i]);
            minimum = {std::min(minimum.x, float(p.x)), std::min(minimum.y, float(p.y)), std::min(minimum.z, float(p.z))};
            maximum = {std::max(maximum.x, float(p.x)), std::max(maximum.y, float(p.y)), std::max(maximum.z, float(p.z))};
            any = true;
        }
    }
    return any;
}
}  // namespace rock::io
