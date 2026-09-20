#include "io/AssetRelations.h"

#include "core/Log.h"
#include "core/PathUtf8.h"
#include "io/ThumbnailStore.h"

#include <algorithm>
#include <functional>

namespace rock::io {
namespace fs = std::filesystem;
namespace {

bool SamePath(const fs::path& a, const fs::path& b) {
    std::error_code ea, eb;
    const auto ca = fs::weakly_canonical(a, ea), cb = fs::weakly_canonical(b, eb);
    return !ea && !eb && _wcsicmp(ca.c_str(), cb.c_str()) == 0;
}

bool IsDocument(const fs::path& path) {
    const auto ext = path.extension().wstring();
    for (const auto* value : {L".rockscene", L".rockmat", L".rocksky", L".tglayer", L".tgboundary", L".rockmodel", L".reproj", L".mmproj", L".mmmat"})
        if (_wcsicmp(ext.c_str(), value) == 0) return true;
    return false;
}

void Unique(std::vector<fs::path>& paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

}  // namespace

AssetRelations InspectAssetRelations(ProjectWorkspace& workspace, const fs::path& target) {
    AssetRelations result;
    result.target = target;
    std::error_code error;
    if (!workspace.Contains(target) || SamePath(target, workspace.Root() / L"project.reproj") ||
        target.lexically_relative(workspace.Root()).wstring().starts_with(L".") ||
        fs::is_symlink(target, error) || !fs::is_regular_file(target, error)) return result;
    result.modified = fs::last_write_time(target, error);
    if (error) return result;
    result.size = fs::file_size(target, error);
    if (error || !workspace.Scan()) return result;
    result.complete = true;
    nlohmann::json header;
    if (IsDocument(target) && !ProjectWorkspace::ReadJson(target, header)) result.complete = false;
    auto uid = ProjectWorkspace::String(header, "uid");
    const fs::path meta = target.wstring() + L".meta";
    if (fs::exists(meta, error)) {
        if (!workspace.Contains(meta) || fs::is_symlink(meta, error)) {
            result.complete = false;
        } else {
            nlohmann::json metadata;
            if (!ProjectWorkspace::ReadJson(meta, metadata)) result.complete = false;
            else uid = ProjectWorkspace::String(metadata, "uid");
            result.companions.push_back(meta);
            const auto modified = fs::last_write_time(meta, error);
            if (error) result.complete = false;
            const auto size = fs::file_size(meta, error);
            if (error) result.complete = false;
            result.companionVersions.push_back(std::to_string(modified.time_since_epoch().count()) + ":" + std::to_string(size));
        }
    }
    const auto references = [&](const nlohmann::json& document, const fs::path& owner, bool outgoing) {
        bool hit = false;
        const auto visit = [&](auto&& self, const nlohmann::json& value) -> void {
            if (value.is_object() && value.contains("uid") && value.contains("path")) {
                const auto reference = workspace.Resolve(value);
                if (outgoing) {
                    if (!reference.empty()) result.related.push_back(reference);
                } else if ((!uid.empty() && ProjectWorkspace::String(value, "uid") == uid) ||
                           (!reference.empty() && SamePath(reference, target))) {
                    hit = true;
                }
                return;
            }
            if (value.is_structured()) {
                for (const auto& child : value) self(self, child);
            } else if (value.is_string()) {
                const auto text = value.get<std::string>();
                if (text.empty()) return;
                const auto path = FromUtf8(text);
                for (const auto& candidate : {owner.parent_path() / path, workspace.Root() / path}) {
                    if (outgoing) {
                        std::error_code existsError;
                        if (fs::is_regular_file(candidate, existsError) && !SamePath(candidate, owner))
                            result.related.push_back(candidate.lexically_normal());
                    } else if (SamePath(candidate, target)) {
                        hit = true;
                    }
                }
            }
        };
        visit(visit, document);
        return hit;
    };
    if (header.is_object()) references(header, target, true);
    if (_wcsicmp(target.extension().c_str(), L".rockscene") == 0) {
        const auto thumbnail = SceneThumbnailPath(workspace, target);
        if (!thumbnail.empty() && fs::exists(thumbnail, error)) result.related.push_back(thumbnail);
    }
    fs::recursive_directory_iterator it(workspace.Root(), fs::directory_options::none, error), end;
    for (; it != end && !error; it.increment(error)) {
        if (it->is_symlink(error)) { it.disable_recursion_pending(); result.complete = false; continue; }
        if (it->is_directory(error)) {
            if (it->path().filename().wstring().starts_with(L".")) it.disable_recursion_pending();
            continue;
        }
        const auto path = it->path();
        if (SamePath(path, target) || !IsDocument(path)) continue;
        nlohmann::json document;
        if (!ProjectWorkspace::ReadJson(path, document)) { result.complete = false; continue; }
        // ルートの目印ファイル（startupScene）は自動読み込みに使わないので参照元に数えない。
        if (ProjectWorkspace::String(document, "format") == "rock-editor.workspace") continue;
        if (references(document, path, false)) result.referencers.push_back(path);
    }
    if (error) result.complete = false;
    Unique(result.related);
    Unique(result.referencers);
    Unique(result.companions);
    return result;
}

bool RetireAsset(ProjectWorkspace& workspace, const AssetRelations& approved) {
    const auto current = InspectAssetRelations(workspace, approved.target);
    if (!approved.complete || !current.complete || current.modified != approved.modified ||
        current.size != approved.size || current.referencers != approved.referencers ||
        current.related != approved.related || current.companions != approved.companions ||
        current.companionVersions != approved.companionVersions) {
        ROCK_LOG_WARN("削除対象または参照関係が変わりました。もう一度確認してください");
        return false;
    }
    const auto directory = workspace.UniquePath(workspace.Root() / L".rock-editor" / L"trash", "DeletedAsset", "");
    if (directory.empty() || !workspace.Contains(directory)) return false;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) return false;
    auto files = current.companions;
    files.insert(files.begin(), current.target);
    nlohmann::json manifest;
    manifest["files"] = nlohmann::json::array();
    for (const auto& file : files) {
        if (!workspace.Contains(file) || !workspace.Contains(directory / file.filename())) return false;
        manifest["files"].push_back({{"original", ToUtf8Portable(file.lexically_relative(workspace.Root()))},
                                     {"stored", ToUtf8Portable(file.filename())}});
    }
    if (!ProjectWorkspace::WriteJson(directory / L"restore.json", manifest)) return false;
    size_t moved = 0;
    for (; moved < files.size(); ++moved) {
        fs::rename(files[moved], directory / files[moved].filename(), error);
        if (error) break;
    }
    if (error) {
        while (moved > 0) {
            --moved;
            std::error_code rollback;
            fs::rename(directory / files[moved].filename(), files[moved], rollback);
            if (rollback) ROCK_LOG_ERROR("退避ファイルを元に戻せません: %s", ToUtf8Display(directory).c_str());
        }
        return false;
    }
    workspace.Scan();
    ROCK_LOG_INFO("ファイルを退避しました: %s", ToUtf8Display(directory).c_str());
    return true;
}

namespace {

// 本体を destination へ置き換え、.meta も新しい名前へ揃える。
fs::path RelocateAsset(ProjectWorkspace& workspace, const fs::path& target, const fs::path& destination) {
    std::error_code error;
    if (!workspace.Contains(target) || !workspace.Contains(destination) ||
        SamePath(target, workspace.Root() / L"project.reproj") ||
        target.lexically_relative(workspace.Root()).wstring().starts_with(L".") ||
        destination.filename().wstring().starts_with(L".") ||
        fs::is_symlink(target, error) || !fs::is_regular_file(target, error) ||
        !fs::is_directory(destination.parent_path(), error)) {
        ROCK_LOG_WARN("名前を変更できないファイルです: %s", ToUtf8Display(target).c_str());
        return {};
    }
    if (SamePath(target, destination)) return target;
    if (fs::exists(destination, error) || error || fs::exists(destination.wstring() + L".meta", error) || error) {
        ROCK_LOG_WARN("同じ名前のファイルがあります: %s", ToUtf8Display(destination).c_str());
        return {};
    }
    // 画像はまだ .meta が無ければ先に ID を確定し、古いパスを持つ参照からも追えるようにする。
    if (!IsDocument(target) && workspace.Reference(target).is_null()) return {};
    std::vector<std::pair<fs::path, fs::path>> files{{target, destination}};
    const fs::path meta = target.wstring() + L".meta";
    if (fs::exists(meta, error)) files.emplace_back(meta, destination.wstring() + L".meta");
    size_t moved = 0;
    for (; moved < files.size(); ++moved) {
        fs::rename(files[moved].first, files[moved].second, error);
        if (error) break;
    }
    if (error) {
        ROCK_LOG_ERROR("名前を変更できませんでした: %s", ToUtf8Display(files[moved].first).c_str());
        while (moved > 0) {
            --moved;
            std::error_code rollback;
            fs::rename(files[moved].second, files[moved].first, rollback);
            if (rollback) ROCK_LOG_ERROR("改名したファイルを元に戻せません: %s", ToUtf8Display(files[moved].first).c_str());
        }
        return {};
    }
    workspace.Scan();
    return destination;
}

}  // namespace

fs::path RenameAsset(ProjectWorkspace& workspace, const fs::path& target, const std::string& newName) {
    const fs::path name = FromUtf8(newName);
    if (name.empty() || name.has_parent_path() || name.wstring().starts_with(L".") ||
        name.wstring().find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) {
        ROCK_LOG_WARN("使えない名前です: %s", newName.c_str());
        return {};
    }
    const auto destination = target.parent_path() / name;
    std::error_code error;
    if (fs::is_directory(target, error) && !fs::is_symlink(target, error)) {
        if (!workspace.Contains(target) || !workspace.Contains(destination) || SamePath(target, workspace.Root()) ||
            target.lexically_relative(workspace.Root()).wstring().starts_with(L".")) {
            ROCK_LOG_WARN("このフォルダは改名できません");
            return {};
        }
        if (SamePath(target, destination)) return target;
        if (fs::exists(destination, error)) {
            ROCK_LOG_WARN("同じ名前のフォルダがあります: %s", ToUtf8Display(destination).c_str());
            return {};
        }
        fs::rename(target, destination, error);
        if (error) {
            ROCK_LOG_ERROR("フォルダを改名できませんでした: %s", ToUtf8Display(target).c_str());
            return {};
        }
        workspace.Scan();
        return destination;
    }
    return RelocateAsset(workspace, target, destination);
}

fs::path MoveAsset(ProjectWorkspace& workspace, const fs::path& target, const fs::path& directory) {
    std::error_code error;
    if (!workspace.Contains(directory) || !fs::is_directory(directory, error) ||
        directory.lexically_relative(workspace.Root()).wstring().starts_with(L".")) {
        ROCK_LOG_WARN("移動先のフォルダが使えません: %s", ToUtf8Display(directory).c_str());
        return {};
    }
    if (fs::is_directory(target, error) && !fs::is_symlink(target, error)) {
        // フォルダは中身ごと移す。ルート・内部フォルダは動かさず、自分自身やその配下へは移さない。
        if (!workspace.Contains(target) || SamePath(target, workspace.Root()) ||
            target.lexically_relative(workspace.Root()).wstring().starts_with(L".")) {
            ROCK_LOG_WARN("このフォルダは移動できません: %s", ToUtf8Display(target).c_str());
            return {};
        }
        const auto source = fs::weakly_canonical(target, error);
        const auto into = error ? fs::path{} : fs::weakly_canonical(directory, error);
        const auto inside = into.lexically_relative(source);
        if (error || into.empty() || (!inside.empty() && *inside.begin() != L"..")) {
            ROCK_LOG_WARN("フォルダを自分自身の中へは移動できません: %s", ToUtf8Display(target).c_str());
            return {};
        }
        const auto destination = directory / target.filename();
        if (SamePath(target, destination)) return target;
        if (fs::exists(destination, error) || error) {
            ROCK_LOG_WARN("移動先に同じ名前のフォルダがあります: %s", ToUtf8Display(destination).c_str());
            return {};
        }
        fs::rename(target, destination, error);
        if (error) {
            ROCK_LOG_ERROR("フォルダを移動できませんでした: %s", ToUtf8Display(target).c_str());
            return {};
        }
        workspace.Scan();
        return destination;
    }
    return RelocateAsset(workspace, target, directory / target.filename());
}

}  // namespace rock::io
