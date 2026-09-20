#include "io/ProjectWorkspace.h"

#include "core/Log.h"
#include "core/PathUtf8.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <unordered_set>

namespace rock::io {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {

constexpr const char* kWorkspaceFormat = "rock-editor.workspace";
constexpr const char* kSceneFormat = "rock-editor.scene";
constexpr const wchar_t* kWorkspaceFile = L"project.reproj";

std::string NewUid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t buffer[40]{};
    StringFromGUID2(guid, buffer, 40);
    return ToUtf8Portable(fs::path(buffer));
}

fs::path Absolute(const fs::path& path) {
    std::error_code error;
    const auto result = fs::weakly_canonical(path, error);
    return error ? fs::path{} : result;
}

bool IsNative(const fs::path& path) {
    const auto ext = path.extension().wstring();
    for (const auto* native : {L".rockmat", L".rocksky", L".tglayer", L".tgboundary", L".rockmodel"})
        if (_wcsicmp(ext.c_str(), native) == 0) return true;
    return false;
}

// レイヤーマテリアル本体のマテリアル参照を書き換える（レイヤーと、ノード形式のノードの settings）。
void MapLayerMaterials(json& layer, const std::function<json(const json&)>& convert) {
    if (auto layers = layer.find("materials"); layers != layer.end() && layers->is_array())
        for (auto& entry : *layers)
            if (entry.is_object() && entry.contains("material")) entry["material"] = convert(entry["material"]);
    const auto graph = layer.find("materialGraph");
    if (graph == layer.end() || !graph->is_object()) return;
    if (auto nodes = graph->find("nodes"); nodes != graph->end() && nodes->is_array())
        for (auto& node : *nodes)
            if (node.is_object() && node.contains("settings") && node["settings"].is_object() &&
                node["settings"].contains("material"))
                node["settings"]["material"] = convert(node["settings"]["material"]);
}

// マテリアル本体の画像参照を書き換える（baseColor / normal と、スロットの texture）。
void MapTextures(json& material, const std::function<json(const json&)>& convert) {
    auto maps = material.find("maps");
    if (maps == material.end() || !maps->is_object()) return;
    for (auto& [key, value] : maps->items()) {
        if (key == "baseColor" || key == "normal") value = convert(value);
        else if (value.is_object() && value.contains("texture"))
            value["texture"] = convert(value["texture"]);
    }
}

}  // namespace

std::string ProjectWorkspace::String(const json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool ProjectWorkspace::ReadJson(const fs::path& path, json& document) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    document = json::parse(stream, nullptr, false);
    return document.is_object();
}

bool ProjectWorkspace::WriteJson(const fs::path& path, const json& document) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        stream.flush();
        if (!stream) return false;
        stream.close();
        if (stream.fail()) return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ROCK_LOG_ERROR("保存先を更新できません: %s", ToUtf8Display(path).c_str());
        fs::remove(temp, error);
        return false;
    }
    return true;
}

bool ProjectWorkspace::IsWorkspaceFile(const fs::path& path) {
    if (_wcsicmp(path.filename().c_str(), kWorkspaceFile) != 0) return false;
    json project;
    return ReadJson(path, project) && String(project, "format") == kWorkspaceFormat;
}

bool ProjectWorkspace::Contains(const fs::path& path) const {
    if (m_root.empty() || path.empty()) return false;
    const auto target = Absolute(path);
    if (target.empty()) return false;
    auto a = m_root.begin();
    auto b = target.begin();
    for (; a != m_root.end(); ++a, ++b)
        if (b == target.end() || _wcsicmp(a->c_str(), b->c_str()) != 0) return false;
    return true;
}

bool ProjectWorkspace::Open(const fs::path& root) {
    ProjectWorkspace next;
    next.m_root = Absolute(root);
    std::error_code error;
    if (next.m_root.empty() || !fs::is_directory(next.m_root, error)) {
        ROCK_LOG_ERROR("ルートフォルダが見つかりません: %s", ToUtf8Display(root).c_str());
        return false;
    }
    const auto projectPath = next.m_root / kWorkspaceFile;
    if (fs::exists(projectPath, error)) {
        if (!ReadJson(projectPath, next.m_project) ||
            String(next.m_project, "format") != kWorkspaceFormat ||
            !next.m_project.contains("version") || next.m_project["version"] != 1) {
            ROCK_LOG_ERROR("ルートの project.reproj が対応するプロジェクト形式ではありません");
            return false;
        }
    } else {
        next.m_project = {{"format", kWorkspaceFormat}, {"version", 1},
                          {"uid", NewUid()}, {"startupScene", ""}};
        if (!WriteJson(projectPath, next.m_project)) return false;
    }
    if (!next.Scan()) return false;
    *this = std::move(next);
    return true;
}

bool ProjectWorkspace::Scan() {
    m_paths.clear();
    std::error_code error;
    fs::recursive_directory_iterator it(m_root, fs::directory_options::skip_permission_denied, error), end;
    for (; it != end && !error; it.increment(error)) {
        if (it->is_symlink(error)) { it.disable_recursion_pending(); continue; }
        if (it->is_directory(error)) {
            // ドット始まりの内部フォルダと、入れ子の別ルート（data/test/ の検証用ルートなど）には入らない。
            std::error_code nestedError;
            if (it->path().filename().wstring().starts_with(L".") ||
                fs::exists(it->path() / kWorkspaceFile, nestedError)) it.disable_recursion_pending();
            continue;
        }
        const auto path = it->path();
        if (!IsNative(path) && _wcsicmp(path.extension().c_str(), L".meta") != 0) continue;
        json body;
        if (!ReadJson(path, body)) continue;
        const auto uid = String(body, "uid");
        if (uid.empty()) continue;  // 持ち出し用の旧 .rockmat など。ID を持たないものは対象外
        auto target = path;
        if (_wcsicmp(path.extension().c_str(), L".meta") == 0) target.replace_extension();
        if (!m_paths.emplace(uid, target).second) {
            ROCK_LOG_ERROR("アセット ID が重複しています: %s", ToUtf8Display(path).c_str());
            return false;
        }
        m_knownUids[ToUtf8Portable(Absolute(target))] = uid;
    }
    return !error;
}

fs::path ProjectWorkspace::StartupScene() const {
    const auto text = String(m_project, "startupScene");
    const auto path = m_root / FromUtf8(text);
    return !text.empty() && Contains(path) ? path : fs::path{};
}

bool ProjectWorkspace::SetStartupScene(const fs::path& scene) {
    if (!Contains(scene)) return false;
    auto project = m_project;
    project["startupScene"] = ToUtf8Portable(Absolute(scene).lexically_relative(m_root));
    if (!WriteJson(m_root / kWorkspaceFile, project)) return false;
    m_project = std::move(project);
    return true;
}

fs::path ProjectWorkspace::UniquePath(const fs::path& directory, const std::string& name,
                                      const char* extension) const {
    if (!Contains(directory)) return {};
    std::string safe = name.empty() ? "Asset" : name;
    for (char& c : safe)
        if (static_cast<unsigned char>(c) < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
    // 予約デバイス名や末尾の空白・ピリオドも避ける。
    while (!safe.empty() && (safe.back() == '.' || safe.back() == ' ')) safe.pop_back();
    if (safe.empty()) safe = "Asset";
    std::string upper = safe;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
        upper.starts_with("COM") || upper.starts_with("LPT")) safe = "_" + safe;
    for (unsigned i = 0; i < 100000; ++i) {
        const auto path = directory / FromUtf8(safe + (i ? "_" + std::to_string(i) : "") + extension);
        std::error_code error;
        if (!fs::exists(path, error) && !error) return path;
    }
    return {};
}

fs::path ProjectWorkspace::Import(const fs::path& source, const fs::path& directory) {
    if (Contains(source)) return Absolute(source);
    const auto sourcePath = Absolute(source);
    const auto sourceKey = ToUtf8Portable(sourcePath);
    if (const auto found = m_imports.find(sourceKey); found != m_imports.end()) return found->second;
    const auto target = UniquePath(directory, ToUtf8Display(source.stem()),
                                   ToUtf8Portable(source.extension()).c_str());
    std::error_code error;
    if (target.empty()) return {};
    fs::create_directories(directory, error);
    if (error || !fs::copy_file(source, target, fs::copy_options::none, error)) {
        ROCK_LOG_ERROR("素材をコピーできません: %s", ToUtf8Display(source).c_str());
        return {};
    }
    m_imports[sourceKey] = target;
    return target;
}

json ProjectWorkspace::Reference(const fs::path& path) {
    if (!Contains(path)) return nullptr;
    const auto target = Absolute(path);
    if (const auto known = m_knownUids.find(ToUtf8Portable(target)); known != m_knownUids.end()) {
        if (const auto found = m_paths.find(known->second); found != m_paths.end())
            return {{"uid", known->second}, {"path", ToUtf8Portable(found->second.lexically_relative(m_root))}};
    }
    const auto metadata = IsNative(target) ? target : fs::path(target.wstring() + L".meta");
    json body;
    std::error_code error;
    if (fs::exists(metadata, error)) {
        if (!ReadJson(metadata, body)) return nullptr;
    } else {
        body = json::object();
    }
    auto uid = String(body, "uid");
    if (uid.empty()) {
        uid = NewUid();
        if (uid.empty()) return nullptr;
        body["uid"] = uid;
        if (!IsNative(target)) { body["format"] = "rock-editor.source"; body["version"] = 1; }
        if (!WriteJson(metadata, body)) return nullptr;
    }
    if (const auto it = m_paths.find(uid); it != m_paths.end() && Absolute(it->second) != target) return nullptr;
    m_paths[uid] = target;
    m_knownUids[ToUtf8Portable(target)] = uid;
    return {{"uid", uid}, {"path", ToUtf8Portable(target.lexically_relative(m_root))}};
}

fs::path ProjectWorkspace::Resolve(const json& reference) const {
    const auto uid = String(reference, "uid");
    if (!uid.empty()) {
        const auto found = m_paths.find(uid);
        // ID がある参照は同名の別ファイルへ黙って付け替えない。
        return found == m_paths.end() ? fs::path{} : found->second;
    }
    const auto text = String(reference, "path");
    const auto path = m_root / FromUtf8(text);
    return !text.empty() && Contains(path) ? path : fs::path{};
}

bool ProjectWorkspace::SaveAsset(fs::path& path, const char* kind, json& body) {
    if (!Contains(path)) return false;
    auto uid = String(body, "uid");
    if (!uid.empty()) {
        const auto found = m_paths.find(uid);
        if (found != m_paths.end()) path = found->second;
    } else {
        uid = NewUid();
    }
    if (uid.empty()) return false;
    body["format"] = std::string("rock-editor.") + kind;
    body["version"] = 1;
    body["uid"] = uid;
    body.erase("id");
    body.erase("_assetPath");
    json current;
    std::error_code error;
    const bool unchanged = fs::is_regular_file(path, error) && ReadJson(path, current) && current == body;
    if (!unchanged && !WriteJson(path, body)) return false;
    path = Absolute(path);
    m_paths[uid] = path;
    m_knownUids[ToUtf8Portable(path)] = uid;
    return true;
}

std::string ProjectWorkspace::FindIdenticalAsset(const char* kind, const json& body,
                                                 const std::unordered_set<std::string>& claimedUids) const {
    const auto comparable = [](json value) {
        for (const char* key : {"uid", "format", "version", "id", "_assetPath"}) value.erase(key);
        return value;
    };
    const json wanted = comparable(body);
    // 同じ中身が複数あれば、パスの若いもの（連番の付かない元）を選んで結果を安定させる。
    std::string match;
    fs::path matchPath;
    for (const auto& [uid, path] : m_paths) {
        if (!IsNative(path) || claimedUids.contains(uid)) continue;
        json existing;
        if (!ReadAsset(path, kind, existing) || comparable(std::move(existing)) != wanted) continue;
        if (match.empty() || path < matchPath) {
            match = uid;
            matchPath = path;
        }
    }
    return match;
}

bool ProjectWorkspace::ReadAsset(const fs::path& path, const char* kind, json& body) const {
    return Contains(path) && ReadJson(path, body) &&
           String(body, "format") == std::string("rock-editor.") + kind &&
           body.contains("version") && body["version"] == 1;
}

bool ProjectWorkspace::SaveScene(const fs::path& path, json& document) {
    if (_wcsicmp(path.extension().c_str(), L".rockscene") != 0 || !Contains(path) || !Scan()) return false;
    const auto baseDir = Absolute(path).parent_path();
    std::unordered_map<int, json> textures;
    // 画像の参照。ルート外の実在ファイルは Imported/ へ取り込む。
    // **リンク切れは保存を止めない。** パスだけを残し、読み込み時にリンク切れとして復元する。
    const auto sourceRef = [&](const json& value) -> json {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) return nullptr;
        const auto original = baseDir / FromUtf8(value.get<std::string>());
        std::error_code error;
        if (!fs::is_regular_file(original, error)) return {{"path", ToUtf8Portable(original.lexically_normal())}};
        const auto imported = Import(original, m_root / L"Imported");
        return imported.empty() ? json() : Reference(imported);
    };
    for (auto& entry : document["textures"]) {
        auto ref = sourceRef(entry["path"]);
        if (ref.is_null()) return false;
        textures[entry["id"].get<int>()] = ref;
        entry["source"] = ref;
        entry.erase("path");
    }
    // 配置データの中のレイヤーマテリアル・境界マテリアルの一覧。無ければ nullptr。
    const auto surfaceAssets = [&](const char* key) -> json* {
        const auto layouts = document.find("surfaceLayouts");
        if (layouts == document.end() || !layouts->is_object()) return nullptr;
        const auto list = layouts->find(key);
        return list != layouts->end() && list->is_array() ? &*list : nullptr;
    };
    std::unordered_set<std::string> claimedUids;
    // モデルはモデルを入れる前の文書に無い。operator[] で null のキーを足さないよう、無ければ空の配列にする。
    if (!document.contains("models")) document["models"] = json::array();
    for (const char* key : {"materials", "skies", "models"})
        for (const auto& entry : document[key])
            if (const auto uid = String(entry, "uid"); !uid.empty()) claimedUids.insert(uid);
    for (const char* key : {"layerMaterials", "boundaryMaterials"})
        if (const auto* list = surfaceAssets(key))
            for (const auto& entry : *list)
                if (const auto uid = String(entry, "uid"); !uid.empty()) claimedUids.insert(uid);
    const auto save = [&](json& entry, const char* kind, const char* folder, const char* ext) {
        const json id = entry.contains("id") ? entry["id"] : json();
        fs::path assetPath = FromUtf8(String(entry, "_assetPath"));
        if (assetPath.empty() || !Contains(assetPath)) {
            // ID の無い埋め込み（旧 .reproj）を保存し直すたびに連番の複製を作らない。
            if (String(entry, "uid").empty()) {
                if (const auto uid = FindIdenticalAsset(kind, entry, claimedUids); !uid.empty()) entry["uid"] = uid;
            }
            assetPath = String(entry, "uid").empty() ? UniquePath(m_root / folder, String(entry, "name"), ext)
                                                     : Resolve({{"uid", String(entry, "uid")}});
            if (assetPath.empty()) assetPath = UniquePath(m_root / folder, String(entry, "name"), ext);
        }
        if (!SaveAsset(assetPath, kind, entry)) return false;
        claimedUids.insert(String(entry, "uid"));
        entry = {{"asset", Reference(assetPath)}};
        if (!id.is_null()) entry["id"] = id;  // 天球は順番で参照するので番号を持たない
        return !entry["asset"].is_null();
    };
    // シーン内の番号 → 永続 ID の参照。表に無い番号（0 = なし）は null。
    const auto byNumber = [](const std::unordered_map<int, json>& table) {
        return [&table](const json& value) -> json {
            if (!value.is_number_integer()) return nullptr;
            const auto found = table.find(value.get<int>());
            return found == table.end() ? json() : found->second;
        };
    };
    std::unordered_map<int, json> materialRefs;
    for (auto& entry : document["materials"]) {
        MapTextures(entry, byNumber(textures));
        if (!save(entry, "material-asset", "Materials", ".rockmat")) return false;
        if (entry.contains("id") && entry["id"].is_number_integer()) materialRefs[entry["id"].get<int>()] = entry["asset"];
    }
    for (auto& entry : document["skies"]) {
        if (!entry["hdri"].is_null() && entry["hdri"] != "") {
            entry["hdri"] = sourceRef(entry["hdri"]);
            if (entry["hdri"].is_null()) return false;
        }
        if (!save(entry, "sky-asset", "Skies", ".rocksky")) return false;
    }
    // モデル。FBX は元ファイルの固定 ID、スロットは .rockmat の参照にして .rockmodel へ分ける。
    for (auto& entry : document["models"]) {
        if (!entry.is_object()) return false;
        entry["source"] = sourceRef(entry.value("path", json()));
        if (entry["source"].is_null()) return false;
        entry.erase("path");
        if (!entry.contains("materials") || !entry["materials"].is_array()) entry["materials"] = json::array();
        for (auto& slot : entry["materials"]) slot = byNumber(materialRefs)(slot);
        if (!save(entry, "model-asset", "Models", ".rockmodel")) return false;
    }
    // レイヤーマテリアルと境界マテリアル。マテリアル・画像の参照を永続 ID へ写してファイルへ分け、
    // 配置データには番号（SurfaceId）と参照だけを残す。区間やプリセットはその番号で指したまま。
    if (auto* list = surfaceAssets("layerMaterials")) {
        for (auto& entry : *list) {
            if (!entry.is_object()) return false;
            MapLayerMaterials(entry, byNumber(materialRefs));
            if (!save(entry, "layer-material-asset", "LayerMaterials", ".tglayer")) return false;
        }
    }
    if (auto* list = surfaceAssets("boundaryMaterials")) {
        const auto textureRef = byNumber(textures);
        for (auto& entry : *list) {
            if (!entry.is_object()) return false;
            for (const char* slot : {"mask", "height"}) entry[slot] = textureRef(entry.value(slot, json()));
            if (!save(entry, "boundary-material-asset", "BoundaryMaterials", ".tgboundary")) return false;
        }
    }
    // 同じ保存先の ID は維持し、名前を付けて保存では別の ID にする。
    json existing;
    const auto sceneUid = ReadJson(path, existing) ? String(existing, "sceneUid") : "";
    document["sceneUid"] = sceneUid.empty() ? NewUid() : sceneUid;
    if (String(document, "sceneUid").empty()) return false;
    // 既存の保存器の版は projectVersion へ退避し、読み込み時に戻す。
    if (document.contains("version")) document["projectVersion"] = document["version"];
    document["format"] = kSceneFormat;
    document["version"] = 1;
    if (!WriteJson(path, document)) return false;
    return SetStartupScene(path);
}

bool ProjectWorkspace::ReadScene(const fs::path& path, json& document) {
    if (!Contains(path) || !Scan() || !ReadJson(path, document) ||
        String(document, "format") != kSceneFormat || document["version"] != 1) return false;
    return Expand(document);
}

bool ProjectWorkspace::Expand(json& document) {
    for (const char* key : {"textures", "materials", "skies", "models"}) {
        if (!document.contains(key) || document[key].is_null()) document[key] = json::array();
        if (!document[key].is_array()) return false;
        for (const auto& entry : document[key]) if (!entry.is_object()) return false;
    }
    auto& textures = document["textures"];
    auto& materials = document["materials"];
    int nextTexture = 1;
    std::unordered_map<std::string, int> textureIds;
    for (const auto& entry : textures) {
        if (!entry.contains("id") || !entry["id"].is_number_integer()) return false;
        const int id = entry["id"].get<int>();
        nextTexture = std::max(nextTexture, id + 1);
        textureIds[String(entry.value("source", json::object()), "uid")] = id;
    }
    for (const auto& entry : materials)
        if (!entry.contains("id") || !entry["id"].is_number_integer()) return false;
    // マテリアルが参照する画像がシーンの表に無ければ足す（他のシーンで作った共有マテリアル）。
    const auto textureId = [&](const json& ref) -> json {
        if (ref.is_null() || !ref.is_object()) return nullptr;
        const auto uid = String(ref, "uid");
        const auto key = uid.empty() ? "path:" + String(ref, "path") : uid;
        if (const auto found = textureIds.find(key); found != textureIds.end()) return found->second;
        const int id = nextTexture++;
        textureIds[key] = id;
        textures.push_back({{"id", id}, {"source", ref}});
        return id;
    };
    const auto read = [&](json& entry, const char* kind) {
        const auto ref = entry.value("asset", json::object());
        const auto assetPath = Resolve(ref);
        json body;
        if (assetPath.empty() || !ReadAsset(assetPath, kind, body)) {
            ROCK_LOG_ERROR("アセットを読み込めません: %s", String(ref, "path").c_str());
            return false;
        }
        body["id"] = entry.value("id", json());
        body["_assetPath"] = ToUtf8Portable(assetPath);
        entry = std::move(body);
        return true;
    };
    // 配置データのレイヤーマテリアル・境界マテリアル。共有化前のシーンの埋め込み（asset の無いもの）はそのまま。
    // レイヤーが参照するマテリアルがシーンの表に無ければ足す（他のシーンで作った共有レイヤーマテリアル）。
    int nextMaterial = 1;
    std::unordered_map<std::string, int> materialIds;
    for (const auto& entry : materials) {
        const int id = entry["id"].get<int>();
        nextMaterial = std::max(nextMaterial, id + 1);
        if (const auto uid = String(entry.value("asset", json::object()), "uid"); !uid.empty()) materialIds.emplace(uid, id);
    }
    const auto materialId = [&](const json& ref) -> json {
        const auto uid = String(ref, "uid");
        if (uid.empty()) return 0;
        if (const auto found = materialIds.find(uid); found != materialIds.end()) return found->second;
        const int id = nextMaterial++;
        materialIds.emplace(uid, id);
        materials.push_back({{"id", id}, {"asset", ref}});
        return id;
    };
    if (auto layouts = document.find("surfaceLayouts"); layouts != document.end() && layouts->is_object()) {
        for (const std::string key : {"layerMaterials", "boundaryMaterials"}) {
            const auto list = layouts->find(key);
            if (list == layouts->end()) continue;
            if (!list->is_array()) return false;
            for (auto& entry : *list) {
                if (!entry.is_object()) return false;
                if (!entry.contains("asset")) continue;
                if (key == "layerMaterials") {
                    if (!read(entry, "layer-material-asset")) return false;
                    MapLayerMaterials(entry, materialId);
                } else {
                    if (!read(entry, "boundary-material-asset")) return false;
                    for (const char* slot : {"mask", "height"}) {
                        const auto id = textureId(entry.value(slot, json()));
                        entry[slot] = id.is_null() ? json(0) : id;
                    }
                }
            }
        }
    }
    // モデル。スロットのマテリアルがシーンの表に無ければ足す（他のシーンで作った共有モデル）。
    // FBX が見つからなければ元のパスのまま渡し、読み込み器がリンク切れとして残す。
    for (auto& entry : document["models"]) {
        if (!read(entry, "model-asset")) return false;
        const json ref = entry.value("source", json::object());
        auto source = Resolve(ref);
        if (source.empty()) {
            const auto text = String(ref, "path");
            const fs::path raw = FromUtf8(text);
            source = text.empty() ? fs::path{} : raw.is_absolute() ? raw : (m_root / raw).lexically_normal();
        }
        entry["path"] = ToUtf8Portable(source);
        if (!entry.contains("materials") || !entry["materials"].is_array()) entry["materials"] = json::array();
        for (auto& slot : entry["materials"]) {
            const auto id = materialId(slot);
            slot = id.is_number_integer() && id.get<int>() > 0 ? id : json();
        }
    }
    for (auto& entry : materials) {
        if (!read(entry, "material-asset")) return false;
        MapTextures(entry, textureId);
    }
    for (auto& entry : textures) {
        const auto& ref = entry["source"];
        auto source = Resolve(ref);
        // 見つからない画像は元のパスのまま渡し、読み込み器がリンク切れとして登録する。
        if (source.empty()) {
            const auto text = String(ref, "path");
            if (text.empty()) return false;
            const fs::path raw = FromUtf8(text);
            source = raw.is_absolute() ? raw : (m_root / raw).lexically_normal();
        }
        entry["path"] = ToUtf8Portable(source);
    }
    for (auto& entry : document["skies"]) {
        if (!read(entry, "sky-asset")) return false;
        if (entry["hdri"].is_object()) {
            auto source = Resolve(entry["hdri"]);
            if (source.empty()) {
                const auto text = String(entry["hdri"], "path");
                const fs::path raw = FromUtf8(text);
                source = text.empty() ? fs::path{} : raw.is_absolute() ? raw : (m_root / raw).lexically_normal();
            }
            entry["hdri"] = source.empty() ? json() : json(ToUtf8Portable(source));
        }
    }
    document["format"] = "rock-editor.project";
    document["version"] = document.contains("projectVersion") ? document["projectVersion"] : json(4);
    return true;
}

}  // namespace rock::io
