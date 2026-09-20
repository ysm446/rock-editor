#include "io/ThumbnailStore.h"

#include "core/Log.h"
#include "core/PathUtf8.h"

#include <functional>
#include <unordered_set>

namespace rock::io {
namespace fs = std::filesystem;
namespace {

uint64_t Hash(std::string_view text, uint64_t hash = 14695981039346656037ull) {
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; }
    return hash;
}

}  // namespace

fs::path SceneThumbnailPath(const ProjectWorkspace& workspace, const fs::path& scene) {
    if (!workspace.Contains(scene)) return {};
    nlohmann::json document;
    const auto uid = ProjectWorkspace::ReadJson(scene, document) ? ProjectWorkspace::String(document, "sceneUid") : "";
    const auto key = uid.empty()
        ? "legacy-" + std::to_string(Hash(ToUtf8Portable(scene.lexically_relative(workspace.Root()))))
        : "scene-" + std::to_string(Hash(uid));
    const auto target = workspace.Root() / L".rock-editor" / L"scene-thumbnails" / FromUtf8(key + ".png");
    return workspace.Contains(target) ? target : fs::path{};
}

ThumbnailRecord AssetThumbnailRecord(ProjectWorkspace& workspace, const fs::path& path) {
    const auto relative = ToUtf8Portable(path.lexically_relative(workspace.Root()));
    const auto directory = workspace.Root() / L".rock-editor" / L"thumbnails";
    const auto key = std::to_wstring(Hash(relative));
    // 形式・描画条件の変更時に版を上げて古いキャッシュを無効化する。
    uint64_t stamp = Hash("thumbnail-v1");
    std::unordered_set<std::string> visited;
    std::function<void(const fs::path&)> visit;
    visit = [&](const fs::path& file) {
        const auto name = ToUtf8Portable(file.lexically_normal());
        if (!visited.insert(name).second) return;
        std::error_code error;
        const auto time = fs::last_write_time(file, error);
        stamp = Hash(name, stamp);
        stamp = Hash(error ? "missing" : std::to_string(time.time_since_epoch().count()), stamp);
        const auto size = fs::file_size(file, error);
        if (!error) stamp = Hash(std::to_string(size), stamp);
        const auto extension = file.extension().wstring();
        if (_wcsicmp(extension.c_str(), L".rockmat") && _wcsicmp(extension.c_str(), L".rocksky") &&
            _wcsicmp(extension.c_str(), L".tglayer") && _wcsicmp(extension.c_str(), L".tgboundary")) return;
        nlohmann::json document;
        if (!ProjectWorkspace::ReadJson(file, document)) return;
        const auto refs = [&](auto&& self, const nlohmann::json& value) -> void {
            if (value.is_object() && value.contains("uid") && value.contains("path")) {
                const auto dependency = workspace.Resolve(value);
                if (!dependency.empty()) visit(dependency);
                else stamp = Hash("missing-reference", stamp);
                return;
            }
            if (value.is_structured()) {
                for (const auto& child : value) self(self, child);
            } else if (value.is_string()) {
                // 旧単体マテリアルの相対パス参照。
                const auto candidate = file.parent_path() / FromUtf8(value.get<std::string>());
                std::error_code existsError;
                if (fs::is_regular_file(candidate, existsError)) visit(candidate);
            }
        };
        refs(refs, document);
    };
    visit(path);
    return {directory / (key + L".png"), directory / (key + L".json"), std::to_string(stamp)};
}

bool ThumbnailIsCurrent(const ThumbnailRecord& record) {
    std::error_code error;
    if (!fs::is_regular_file(record.image, error) || !fs::is_regular_file(record.metadata, error)) return false;
    nlohmann::json metadata;
    return ProjectWorkspace::ReadJson(record.metadata, metadata) &&
           ProjectWorkspace::String(metadata, "stamp") == record.stamp;
}

bool CommitThumbnail(const ThumbnailRecord& record) {
    return ProjectWorkspace::WriteJson(record.metadata, {{"stamp", record.stamp}});
}

}  // namespace rock::io
