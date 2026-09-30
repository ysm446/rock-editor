#pragma once
#include "geometry/Mesh.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

// 岩アセット（Rock Asset ノードが焼いたもの）の保存と読み込み。
//
// 岩アセットは岩グラフに含まれる。設定はシーン（.rockgraph）に、焼いた結果は同名の付属フォルダ
// （`Foo.rockgraph` の横の `Foo.rockgraph.bake/`）に置く。フォルダの中身:
//   asset.json                 目録（形式・段ごとのメッシュと切り替えの大きさ・範囲・内容のハッシュ）
//   lod0.rockmesh, lod1…       段ごとのメッシュ（独自のバイナリ。geometry::Mesh をそのまま書く）
//   BaseColor.png など 4 枚    Material Bake の結果（ベイクしていれば）。全ての段で共有する
// 仕様は docs/reference/rock-asset.md。
namespace rock::io {

inline constexpr std::array<const char*, 4> kRockAssetTextures = {"BaseColor.png", "Normal.png",
                                                                  "RoughnessMetallicAO.png", "Height.png"};

struct RockAssetLod {
    geometry::Mesh mesh;
    // この段へ切り替える画面上の大きさ（Rock Asset の「LODnの切替」）。LOD0 は 1。
    float screenSize = 1.0f;
};

struct RockAssetData {
    std::vector<RockAssetLod> lods;
    // BaseColor.png など 4 枚を持つか（Material Bake の結果を焼いたか）。
    bool textured = false;
    // 焼いたときの内容のハッシュ（RockAssetHash）。今のグラフの結果と比べて、古くなったかを判定する。
    std::string hash;
    // LOD0 の範囲（m）。
    geometry::Vec3 minimum{}, maximum{};
};

// シーンの付属フォルダ（`<シーンのファイル名>.bake`）。
std::filesystem::path RockAssetFolder(const std::filesystem::path& scene);
// 段ごとのメッシュと切り替えの大きさ、ベイクの指紋（テクスチャを焼かないなら空）から作るハッシュ（16 進）。
std::string RockAssetHash(const std::vector<RockAssetLod>& lods, const std::string& bakeFingerprint);
// 目録とメッシュを書く（テクスチャは呼び出し側が先に書く）。前に焼いた余分な段のファイルは消す。
bool SaveRockAsset(const std::filesystem::path& scene, const RockAssetData& data, std::string& error);
// 目録とメッシュを読む。付属フォルダが無い・壊れているときは false（未焼成として扱う）。
bool LoadRockAsset(const std::filesystem::path& scene, RockAssetData& data, std::string& error);
// 目録のハッシュだけを読む（毎フレームの判定用。メッシュは読まない）。無ければ false。
bool ReadRockAssetHash(const std::filesystem::path& scene, std::string& hash);

// メッシュ 1 つの読み書き（.rockmesh）。
bool SaveRockMesh(const std::filesystem::path& path, const geometry::Mesh& mesh);
bool LoadRockMesh(const std::filesystem::path& path, geometry::Mesh& mesh);

}  // namespace rock::io
