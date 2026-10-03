#pragma once
#include "geometry/Mesh.h"

#include <filesystem>
#include <string>
#include <vector>

// モデル資産（.tgmodel / .model）の目録の読み込みと、FBX の範囲。
//
// terrain-graph と植生アセットを共有するため、terrain-graph の `.tgmodel`（"format": "terrain-graph.model-asset"）を
// そのまま読む。FBX とマテリアル（.tgmat）はプロジェクトのルートからの相対パスで書かれているので、.tgmodel の
// 位置から上へ project.reproj を探してルートを決める（無ければ .tgmodel のフォルダに同名のファイルを探す）。
// 仕様は docs/reference/vegetation.md。
namespace rock::io {

struct ModelAssetInfo {
    std::string name;
    std::filesystem::path fbx;                      // 解決した FBX のパス
    std::vector<std::filesystem::path> materials;   // スロット順のマテリアル（.tgmat / .rockmat）。解決できなければ空
    std::vector<float> lodScreenSizes;              // [i] が LOD i+1 に替わる画面の大きさ（無ければ空）
};

// .tgmodel（か .model）の目録を読む。失敗なら false と理由。
bool ReadModelAssetInfo(const std::filesystem::path& assetPath, ModelAssetInfo& info, std::string& error);
// project.reproj を上へ探し、見つかったフォルダを返す（無ければ空）。
std::filesystem::path FindProjectRoot(const std::filesystem::path& start);
// FBX の範囲（右手系 Y-up、m。読み込み時の変換後）。読めなければ false。
bool ReadFbxBounds(const std::filesystem::path& fbx, geometry::Vec3& minimum, geometry::Vec3& maximum);
// モデル資産のパスか（.tgmodel / .model）。
bool IsModelAssetPath(const std::filesystem::path& path);

}  // namespace rock::io
