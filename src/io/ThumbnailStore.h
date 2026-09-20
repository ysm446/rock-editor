#pragma once

#include "io/ProjectWorkspace.h"

namespace rock::io {

// 未読み込みアセットのサムネイルのディスクキャッシュ（`<ルート>/.rock-editor/thumbnails/`）。
// 画像と検証用 JSON の対で、stamp が一致するときだけ有効とみなす。
struct ThumbnailRecord {
    std::filesystem::path image;
    std::filesystem::path metadata;
    std::string stamp;
};

ThumbnailRecord AssetThumbnailRecord(ProjectWorkspace& workspace, const std::filesystem::path& path);
bool ThumbnailIsCurrent(const ThumbnailRecord& record);
bool CommitThumbnail(const ThumbnailRecord& record);
// シーンのプレビュー画像（`<ルート>/.rock-editor/scene-thumbnails/`）。sceneUid から名前を決める。
std::filesystem::path SceneThumbnailPath(const ProjectWorkspace& workspace, const std::filesystem::path& scene);

}  // namespace rock::io
