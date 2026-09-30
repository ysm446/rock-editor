#include "TestSupport.h"
#include "geometry/BaseRock.h"
#include "io/AssetRelations.h"
#include "io/ProjectWorkspace.h"
#include "io/RockAssetIo.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace rock::tests;
using namespace rock;

void RunRockAssetIoTests() {
    Section("岩アセットの保存と読み込み");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::path(ROCK_DATA_DIR) / "test" / ("rock-asset-io-" + std::to_string(stamp));
    std::error_code error;
    fs::create_directories(root / "Scenes", error);
    const fs::path scene = root / "Scenes" / "boulder.rockgraph";
    std::ofstream(scene) << "{}";
    Check(io::RockAssetFolder(scene).filename() == L"boulder.rockgraph.bake", "付属フォルダは <シーン>.bake");

    // 段の違う 2 つのメッシュ（UV は持たない）。
    geometry::BaseRockSettings settings;
    settings.shape = geometry::BaseShape::Sphere;
    std::string message;
    settings.subdivisions = 16;
    const auto fine = geometry::MakeBaseRock(settings, message);
    settings.subdivisions = 4;
    const auto coarse = geometry::MakeBaseRock(settings, message);
    io::RockAssetData data;
    data.lods = {{fine, 1.0f}, {coarse, 0.25f}};
    data.hash = io::RockAssetHash(data.lods, "");
    data.minimum = {-1, -1, -1};
    data.maximum = {1, 1, 1};
    Check(io::SaveRockAsset(scene, data, message) && message.empty(), "目録と段ごとのメッシュを書ける");
    Check(fs::exists(io::RockAssetFolder(scene) / "asset.json") && fs::exists(io::RockAssetFolder(scene) / "lod1.rockmesh"),
          "asset.json と lodN.rockmesh ができる");

    io::RockAssetData loaded;
    Check(io::LoadRockAsset(scene, loaded, message) && loaded.lods.size() == 2 &&
              loaded.lods[0].mesh.positions == fine.positions && loaded.lods[0].mesh.triangles == fine.triangles &&
              loaded.lods[1].mesh.triangles == coarse.triangles && loaded.lods[1].screenSize == 0.25f &&
              loaded.hash == data.hash && !loaded.textured && loaded.maximum.x == 1.0f,
          "読み直すと同じメッシュ・切り替えの大きさ・ハッシュ");
    std::string hash;
    Check(io::ReadRockAssetHash(scene, hash) && hash == data.hash, "目録のハッシュだけを読める");

    // ハッシュは形・切り替えの大きさ・ベイクの指紋で変わる。
    auto changed = data.lods;
    changed[1].screenSize = 0.3f;
    Check(io::RockAssetHash(changed, "") != data.hash && io::RockAssetHash(data.lods, "baked") != data.hash &&
              io::RockAssetHash(data.lods, "") == data.hash,
          "ハッシュは段・切り替えの大きさ・ベイクの指紋で変わり、同じ内容なら同じ");

    // 自分の UV を持つ段は、段のテクスチャのフォルダ（lodN）を目録に書く。使わなくなったフォルダは消す。
    {
        const auto folder = io::RockAssetFolder(scene);
        io::RockAssetData textured = data;
        textured.textured = true;
        textured.lods[1].textures = io::RockAssetLodTextureFolder(1);
        for (const auto& sub : {fs::path(), fs::path("lod1")}) {
            fs::create_directories(folder / sub, error);
            for (const char* name : io::kRockAssetTextures) std::ofstream(folder / sub / name) << "png";
        }
        Check(io::RockAssetHash(textured.lods, "") != data.hash, "段のテクスチャのフォルダもハッシュに入る");
        Check(io::SaveRockAsset(scene, textured, message) && io::LoadRockAsset(scene, loaded, message) && loaded.textured &&
                  loaded.lods[0].textures.empty() && loaded.lods[1].textures == "lod1",
              "段ごとのテクスチャのフォルダを目録に書き、読み直せる");
        fs::remove(folder / "lod1" / "Height.png", error);
        Check(io::LoadRockAsset(scene, loaded, message) && !loaded.textured, "段のテクスチャが欠けていればテクスチャなしとして読む");
        auto wrongFolder = textured;
        wrongFolder.lods[1].textures = "../elsewhere";
        Check(!io::SaveRockAsset(scene, wrongFolder, message) && !message.empty(), "段のテクスチャのフォルダ名は lodN に限る");
        Check(io::SaveRockAsset(scene, data, message) && !fs::exists(folder / "lod1") && !fs::exists(folder / "BaseColor.png"),
              "テクスチャを焼かないと、段のフォルダと共有のテクスチャを消す");
        // 版 1 の目録（段ごとのテクスチャが無い）も読む。
        std::ifstream in(folder / "asset.json");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const auto at = text.find("\"version\": 2");
        if (at != std::string::npos) text.replace(at, 12, "\"version\": 1");
        std::ofstream(folder / "asset.json", std::ios::trunc) << text;
        Check(at != std::string::npos && io::LoadRockAsset(scene, loaded, message) && loaded.lods.size() == 2 &&
                  loaded.lods[1].textures.empty(),
              "版 1 の目録も読める（全ての段がテクスチャを共有する）");
        Check(io::SaveRockAsset(scene, data, message), "版 2 で書き直す");
    }

    // 段を減らして焼き直すと、余分な段のファイルは消える。
    data.lods.pop_back();
    Check(io::SaveRockAsset(scene, data, message) && !fs::exists(io::RockAssetFolder(scene) / "lod1.rockmesh") &&
              io::LoadRockAsset(scene, loaded, message) && loaded.lods.size() == 1,
          "段を減らして焼き直すと、前の余分な段を消す");

    // 壊れたメッシュは読まない。
    std::ofstream(io::RockAssetFolder(scene) / "lod0.rockmesh", std::ios::binary | std::ios::trunc) << "RKMESH01broken";
    Check(!io::LoadRockAsset(scene, loaded, message) && !message.empty(), "壊れたメッシュは読まない（未焼成として扱う）");
    Check(!io::LoadRockAsset(root / "Scenes" / "none.rockgraph", loaded, message), "付属フォルダが無ければ未焼成");

    Section("岩アセットの付属フォルダの改名・移動");
    io::ProjectWorkspace workspace;
    Check(workspace.Open(root), "ルートを開く");
    Check(io::SaveRockAsset(scene, {{{coarse, 1.0f}}, false, "h", {}, {}}, message), "焼き直す");
    const auto renamed = io::RenameAsset(workspace, scene, "cliff.rockgraph");
    Check(!renamed.empty() && fs::is_directory(io::RockAssetFolder(renamed)) && !fs::exists(io::RockAssetFolder(scene)),
          "シーンの名前を変えると付属フォルダも揃う");
    fs::create_directories(root / "Moved", error);
    workspace.Scan();
    const auto moved = io::MoveAsset(workspace, renamed, root / "Moved");
    Check(!moved.empty() && io::LoadRockAsset(moved, loaded, message), "シーンを移すと付属フォルダも移り、読み直せる");
    const auto relations = io::InspectAssetRelations(workspace, moved);
    Check(std::find(relations.companions.begin(), relations.companions.end(), io::RockAssetFolder(moved)) != relations.companions.end(),
          "シーンを削除（退避）するときは付属フォルダも一緒に退避する");
}
