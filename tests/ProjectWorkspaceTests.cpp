// プロジェクトルートと共有アセットのテスト。GPU を使わない io 層だけを対象にする。
//
//   - 永続 ID と参照の解決（改名・移動しても追える、ID が無ければ付け替えない）
//   - シーンの分離保存と展開（埋め込み文書 → .rockmat / .rocksky / Imported/ → 埋め込み文書）
//   - リンク切れの画像を保存・読み込みで失わない
//   - ルートごとのシーン履歴と旧履歴の移行
//   - サムネイルのディスクキャッシュの有効判定
//   - ファイルの削除（退避）前の参照検査

#include "TestSupport.h"

#include "core/PathUtf8.h"
#include "io/AssetRelations.h"
#include "io/ProjectWorkspace.h"
#include "io/RecentFiles.h"
#include "io/ThumbnailStore.h"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;
using rock::io::ProjectWorkspace;

// AppSettings.cpp はテストに入れない（Windows の設定フォルダへ触らせない）。
namespace rock::io {
fs::path AppDataDirectory() { return {}; }
}  // namespace rock::io

namespace {

using rock::tests::Check;
using rock::tests::Section;

// 検証用のファイルは data/test/ に作る（AGENTS.md）。各ケースのルートは project.reproj を持つ
// 入れ子のルートなので、data/ をルートに開いたときの走査には入らない。
fs::path FreshDirectory(const char* name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path directory = fs::path(ROCK_DATA_DIR) / "test" / ("rock-editor-" + std::string(name) + "-" + std::to_string(stamp));
    std::error_code error;
    fs::create_directories(directory, error);
    return directory;
}

void Touch(const fs::path& path, char content = 'x') {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary).put(content);
}

void TestWorkspace() {
    Section("ProjectWorkspace: 永続 ID と参照");
    const fs::path root = FreshDirectory("workspace");
    ProjectWorkspace workspace;
    Check(workspace.Open(root), "ルートを開く（project.reproj を作る）");
    Check(fs::exists(root / "project.reproj"), "目印ファイルができる");
    Check(ProjectWorkspace::IsWorkspaceFile(root / "project.reproj"), "目印ファイルの判定");
    Check(!workspace.Contains(root / ".." / "outside"), "ルート外（..）は含まない");
    Check(!workspace.Contains(root.wstring() + L"-sibling/file"), "名前が前方一致するだけの隣は含まない");

    const fs::path image = workspace.UniquePath(root, "source", ".png");
    Touch(image);
    const json source = workspace.Reference(image);
    Check(!source.is_null() && fs::exists(fs::path(image.wstring() + L".meta")), "画像の参照で .meta ができる");
    fs::path materialPath = workspace.UniquePath(root, "material", ".rockmat");
    json material = {{"name", "material"}, {"roughness", 0.25}, {"maps", {{"baseColor", source}}}};
    Check(workspace.SaveAsset(materialPath, "material-asset", material), "マテリアルを保存する");
    const json materialRef = workspace.Reference(materialPath);

    json packed = {{"materials", json::array({{{"id", 7}, {"asset", materialRef}}})}};
    Check(workspace.Expand(packed), "共有マテリアルを展開する");
    Check(packed["textures"].size() == 1 && packed["materials"][0]["maps"]["baseColor"] == 1, "画像の参照をシーンの番号へ写す");

    std::error_code error;
    const fs::path moved = workspace.UniquePath(root / "Moved", "renamed", ".rockmat");
    fs::create_directories(moved.parent_path(), error);
    fs::rename(materialPath, moved, error);
    Check(!error && workspace.Scan() && workspace.Resolve(materialRef) == moved, "改名・移動しても ID で追える");
    const fs::path movedImage = workspace.UniquePath(root / "Moved", "source", ".png");
    fs::rename(image, movedImage, error);
    fs::rename(fs::path(image.wstring() + L".meta"), fs::path(movedImage.wstring() + L".meta"), error);
    Check(!error && workspace.Scan() && workspace.Resolve(source) == movedImage, ".meta と一緒に動かした画像を追える");

    json changed;
    Check(workspace.ReadAsset(moved, "material-asset", changed), "改名したマテリアルを読む");
    changed["roughness"] = 0.75;
    fs::path target = moved;
    Check(workspace.SaveAsset(target, "material-asset", changed), "共有マテリアルを更新する");
    const auto writtenAt = fs::last_write_time(target, error);
    json same = changed;
    fs::path sameTarget = target;
    Check(workspace.SaveAsset(sameTarget, "material-asset", same) && fs::last_write_time(target, error) == writtenAt,
          "中身が同じなら書き直さない（サムネイルのキャッシュを無効にしない）");
    json second = {{"materials", json::array({{{"id", 2}, {"asset", materialRef}}})}};
    Check(workspace.Expand(second) && second["materials"][0]["roughness"] == 0.75, "別のシーンから編集後の値が見える");

    Section("ProjectWorkspace: シーンの分離保存と展開");
    const fs::path outside = FreshDirectory("outside") / "external.png";
    Touch(outside);
    const fs::path missing = root / "gone.png";
    const fs::path scene = workspace.UniquePath(root / "Scenes", "scene", ".rockscene");
    json legacy = {{"version", 26},
                   {"textures", json::array({{{"id", 1}, {"name", "a"}, {"path", rock::ToUtf8Portable(movedImage)}},
                                             {{"id", 2}, {"name", "b"}, {"path", rock::ToUtf8Portable(outside)}},
                                             {{"id", 3}, {"name", "c"}, {"path", rock::ToUtf8Portable(missing)}}})},
                   {"materials", json::array({{{"id", 1}, {"name", "embedded"}, {"maps", {{"baseColor", 2}}},
                                               {"mapUvSets", {{"baseColor", 1}, {"ambientOcclusion", 2}}}}})},
                   {"skies", json::array({{{"id", 1}, {"name", "sky"}, {"hdri", nullptr}}})},
                   {"graph", {{"nodes", json::array()}}}};
    const json legacyCopy = legacy;
    Check(workspace.SaveScene(scene, legacy), "シーンを保存する");
    Check(fs::exists(root / "Materials" / "embedded.rockmat") && fs::exists(root / "Skies" / "sky.rocksky"), "埋め込みを .rockmat / .rocksky へ分ける");
    Check(fs::exists(root / "Imported" / "external.png"), "ルート外の画像を Imported/ へ取り込む");
    json loaded;
    Check(workspace.ReadScene(scene, loaded), "シーンを読む");
    Check(loaded["version"] == 26, "既存の保存器の版を戻す");
    Check(loaded["textures"][0]["path"] == rock::ToUtf8Portable(movedImage), "ルート内の画像は元の場所のまま");
    Check(loaded["textures"][1]["path"] == rock::ToUtf8Portable(root / "Imported" / "external.png"), "取り込んだ画像を指す");
    Check(loaded["textures"][2]["path"] == rock::ToUtf8Portable(missing), "リンク切れの画像はパスのまま残る");
    Check(loaded["materials"][0]["maps"]["baseColor"] == 2 && loaded["materials"][0]["name"] == "embedded", "マテリアルの画像参照を番号へ戻す");
    Check(loaded["materials"][0]["mapUvSets"]["ambientOcclusion"] == 2, "マップごとの UV が分離保存と展開で残る");
    Check(workspace.StartupScene() == scene, "開始シーンを覚える");
    json resaved = legacyCopy;
    Check(workspace.SaveScene(workspace.UniquePath(root / "Scenes", "resaved", ".rockscene"), resaved), "同じ旧文書をもう一度シーンへ保存する");
    Check(!fs::exists(root / "Materials" / "embedded_1.rockmat") && !fs::exists(root / "Skies" / "sky_1.rocksky"),
          "同じ中身の埋め込みは連番の複製を作らない");
    Check(resaved["materials"][0]["asset"]["uid"] == legacy["materials"][0]["asset"]["uid"], "既存のマテリアルの ID を指す");
    json edited = legacyCopy;
    edited["materials"][0]["roughness"] = 0.5;
    Check(workspace.SaveScene(workspace.UniquePath(root / "Scenes", "edited", ".rockscene"), edited) &&
              fs::exists(root / "Materials" / "embedded_1.rockmat"),
          "中身が違えば別のアセットにする");
    const std::string uid = loaded["sceneUid"];
    json again = loaded;
    Check(workspace.SaveScene(scene, again) && again["sceneUid"] == uid, "同じ保存先はシーン ID を保つ");
    json other = loaded;
    Check(workspace.SaveScene(root / "Scenes" / "copy.rockscene", other) && other["sceneUid"] != uid, "名前を付けて保存は別 ID");

    Section("ProjectWorkspace: モデル（.rockmodel）の分離保存と展開");
    const fs::path fbx = workspace.UniquePath(root / "Models", "plane", ".fbx");
    fs::create_directories(fbx.parent_path(), error);
    Touch(fbx);
    json withModel = {{"version", 27},
                      {"textures", json::array()},
                      {"materials", json::array({{{"id", 1}, {"name", "paint"}, {"maps", json::object()}}})},
                      {"skies", json::array()},
                      {"models", json::array({{{"id", 1}, {"name", "plane"},
                                               {"path", rock::ToUtf8Portable(fbx.lexically_relative(root / "Scenes"))},
                                               {"scale", 100.0},
                                               {"materials", json::array({1, nullptr})}}})}};
    const fs::path modelScene = workspace.UniquePath(root / "Scenes", "model", ".rockscene");
    Check(workspace.SaveScene(modelScene, withModel), "モデルを含むシーンを保存する");
    json modelBody;
    Check(workspace.ReadAsset(root / "Models" / "plane.rockmodel", "model-asset", modelBody) &&
              modelBody["source"]["uid"].is_string() && modelBody["materials"][0]["uid"].is_string() &&
              modelBody["materials"][1].is_null() && modelBody["scale"] == 100.0,
          "モデルを .rockmodel へ分け、FBX とスロットを固定 ID で参照する");
    json modelLoaded;
    Check(workspace.ReadScene(modelScene, modelLoaded) && modelLoaded["models"].size() == 1 &&
              modelLoaded["models"][0]["path"] == rock::ToUtf8Portable(fbx) &&
              modelLoaded["models"][0]["materials"][0].is_number_integer() &&
              modelLoaded["models"][0]["materials"][1].is_null(),
          "モデルを展開し、FBX を絶対パス・スロットをシーンの番号へ戻す");
    // 以降のルート移動の確認は画像を持つシーンで行うので、開始シーンを戻しておく。
    workspace.SetStartupScene(root / "Scenes" / "copy.rockscene");

    json broken = {{"materials", json::array({{{"id", 1}, {"asset", {{"uid", "missing"}, {"path", "Moved/renamed.rockmat"}}}}})}};
    // 無いマテリアルはシーン全体を失敗にせず、その項目だけ外す（同名のファイルへ勝手に付け替えない）。
    Check(workspace.Expand(broken) && broken["materials"].empty(), "ID が見つからなければ同名へ付け替えず、その項目を外す");
    const fs::path duplicate = workspace.UniquePath(root, "duplicate", ".rockmat");
    fs::copy_file(moved, duplicate, error);
    Check(!workspace.Scan(), "ID の重複を検出する");
    fs::remove(duplicate, error);
    Check(workspace.Scan(), "重複を消せば戻る");
    const fs::path nestedRoot = root / "test" / "nested";
    fs::create_directories(nestedRoot, error);
    ProjectWorkspace nested;
    Check(nested.Open(nestedRoot), "入れ子の検証用ルートを開く");
    fs::copy_file(moved, nestedRoot / "copy.rockmat", error);
    Check(!error && workspace.Scan(), "入れ子のルートの中は親の走査に含めない");
    fs::remove_all(root / "test", error);
    json malformed = {{"materials", json::array({42})}};
    Check(!workspace.Expand(malformed), "壊れたアセット表を拒否する");

    Section("ProjectWorkspace: ルートごと移動");
    fs::path relocated;
    for (unsigned i = 0;; ++i) {
        relocated = root.parent_path() / ("rock-editor-relocated-" + std::to_string(i));
        if (!fs::exists(relocated, error)) break;
    }
    fs::rename(root, relocated, error);
    ProjectWorkspace movedWorkspace;
    Check(!error && movedWorkspace.Open(relocated), "移動したルートを開く");
    json movedScene;
    Check(movedWorkspace.ReadScene(movedWorkspace.StartupScene(), movedScene), "移動後にシーンを読む");
    Check(movedScene["textures"][0]["path"] == rock::ToUtf8Portable(relocated / movedImage.lexically_relative(root)),
          "画像の参照が新しいルートの中を指す");
    fs::remove_all(relocated, error);
    fs::remove_all(outside.parent_path(), error);
}

void TestHistoryAndThumbnails() {
    Section("RecentFiles: ルートごとのシーン履歴");
    const fs::path directory = FreshDirectory("history");
    std::error_code error;
    rock::io::RecentFiles history;
    const fs::path storage = directory / "recent.json", a = directory / "A", b = directory / "B";
    history.Load(storage);
    history.Add(a, a / "same.rockscene");
    history.Add(b, b / "same.rockscene");
    Check(history.Entries(a).size() == 1 && history.Entries(b).size() == 1, "ルートごとに分かれる");
    history.Add(a, a / "SAME.rockscene");
    Check(history.Entries(a).size() == 1 && history.Roots().front().path == a, "大文字小文字を同一視し、ルートが先頭へ来る");
    for (int i = 0; i < 12; ++i) history.Add(a, a / (std::to_string(i) + ".rockscene"));
    Check(history.Entries(a).size() == 10 && history.Entries(a).front().filename() == "11.rockscene", "上限 10 件、新しい順");
    rock::io::RecentFiles loaded;
    loaded.Load(storage);
    Check(loaded.Entries(a) == history.Entries(a) && loaded.Entries(b).size() == 1, "保存と読み込み");
    loaded.Clear(a);
    Check(loaded.Entries(a).empty() && loaded.Entries(b).size() == 1, "ルート単位で消す");
    for (int i = 0; i < 12; ++i) loaded.AddRoot(directory / std::to_string(i));
    Check(loaded.Roots().size() == 10, "ルートも上限 10 件");
    ProjectWorkspace::WriteJson(storage, {{"format", "rock-editor.recent"}, {"version", 1},
        {"projects", {rock::ToUtf8Portable(a / "legacy.reproj"), rock::ToUtf8Portable(b / "legacy.reproj")}}});
    loaded.Load(storage);
    loaded.AddRoot(a);
    Check(loaded.Entries(a).size() == 1 && loaded.Entries(b).empty(), "旧履歴は所属ルートを開いたときだけ移る");
    loaded.Load(storage);
    loaded.AddRoot(b);
    Check(loaded.Entries(a).size() == 1 && loaded.Entries(b).size() == 1, "未移行の旧履歴は残る");
    loaded.ClearRoots();
    loaded.Load(storage);
    Check(loaded.Roots().empty(), "全消去が保存される");
    ProjectWorkspace workspace;
    fs::create_directories(a, error);
    Check(workspace.Open(a), "履歴用のルートを開く");
    ProjectWorkspace::WriteJson(storage, {{"format", "rock-editor.recent"}, {"projects", {rock::ToUtf8Portable(a / "nested.rockscene")}}});
    loaded.Load(storage);
    loaded.AddRoot(directory);
    Check(loaded.Entries(directory).empty(), "入れ子のルートの履歴を親へ取り込まない");
    loaded.AddRoot(a);
    Check(loaded.Entries(a).size() == 1, "入れ子のルート自身を開けば移る");

    Section("ThumbnailStore: ディスクキャッシュの有効判定");
    const fs::path image = a / "source.png";
    Touch(image, 'a');
    const json source = workspace.Reference(image);
    fs::path material = a / "material.rockmat";
    json materialBody = {{"maps", {{"baseColor", source}}}};
    Check(workspace.SaveAsset(material, "material-asset", materialBody), "検証用のマテリアル");
    const auto original = rock::io::AssetThumbnailRecord(workspace, material);
    Touch(original.image);
    Check(rock::io::CommitThumbnail(original) && rock::io::ThumbnailIsCurrent(original), "記録した直後は有効");
    Touch(a / "unrelated.png", 'z');
    Check(rock::io::ThumbnailIsCurrent(rock::io::AssetThumbnailRecord(workspace, material)), "無関係なファイルでは無効にならない");
    std::ofstream(image, std::ios::app | std::ios::binary).put('b');
    const auto changed = rock::io::AssetThumbnailRecord(workspace, material);
    Check(original.image == changed.image && !rock::io::ThumbnailIsCurrent(changed), "参照先の画像が変わると同じ枠が無効になる");
    fs::remove(image, error);
    Check(changed.stamp != rock::io::AssetThumbnailRecord(workspace, material).stamp, "参照先が消えても無効になる");
    const fs::path scene = a / "scene.rockscene";
    json sceneDocument = {{"textures", json::array()}, {"materials", json::array()}, {"skies", json::array()}};
    Check(workspace.SaveScene(scene, sceneDocument), "シーン ID 付きで保存する");
    const auto thumbnail = rock::io::SceneThumbnailPath(workspace, scene);
    Check(thumbnail.parent_path() == a / ".rock-editor" / "scene-thumbnails", "シーンの画像はルート内の一か所へ");
    const fs::path renamedScene = a / "renamed.rockscene";
    fs::rename(scene, renamedScene, error);
    Check(rock::io::SceneThumbnailPath(workspace, renamedScene) == thumbnail, "改名しても同じ画像を指す");

    Section("AssetRelations: 削除前の参照検査");
    ProjectWorkspace deletion;
    const fs::path deleteRoot = directory / "delete-root";
    fs::create_directories(deleteRoot, error);
    Check(deletion.Open(deleteRoot), "削除検査用のルート");
    const fs::path texture = deleteRoot / "image.png";
    Touch(texture, 't');
    const json textureRef = deletion.Reference(texture);
    fs::path materialFile = deleteRoot / "shared.rockmat";
    json dependency = {{"maps", {{"baseColor", textureRef}}}};
    Check(deletion.SaveAsset(materialFile, "material-asset", dependency), "参照元のマテリアル");
    auto report = rock::io::InspectAssetRelations(deletion, texture);
    Check(report.complete && report.referencers.size() == 1 && report.referencers[0] == materialFile && report.companions.size() == 1,
          "参照元と .meta を列挙する");
    std::ofstream(texture.wstring() + L".meta", std::ios::app | std::ios::binary).put(' ');
    Check(!rock::io::RetireAsset(deletion, report) && fs::exists(texture), ".meta が変わっていれば再確認を求める");
    report = rock::io::InspectAssetRelations(deletion, texture);
    const auto materialReport = rock::io::InspectAssetRelations(deletion, materialFile);
    Check(materialReport.related.size() == 1 && materialReport.related[0] == texture, "参照先の素材を関連として出す");
    fs::path secondMaterial = deleteRoot / "second.rockmat";
    dependency.erase("uid");
    Check(deletion.SaveAsset(secondMaterial, "material-asset", dependency), "確認後に参照元が増える");
    Check(!rock::io::RetireAsset(deletion, report) && fs::exists(texture), "参照関係が変わっていれば再確認を求める");
    const auto updated = rock::io::InspectAssetRelations(deletion, texture);
    Check(rock::io::RetireAsset(deletion, updated), "確認どおりなら退避する");
    Check(!fs::exists(texture) && !fs::exists(texture.wstring() + L".meta") && fs::exists(materialFile), "参照元は消さない");
    bool recoverable = false;
    for (const auto& entry : fs::recursive_directory_iterator(deleteRoot / ".rock-editor" / "trash", error))
        if (entry.path().filename() == "image.png") recoverable = true;
    Check(recoverable, "退避した元ファイルが残る");
    std::ofstream(deleteRoot / "broken.rockmat", std::ios::binary) << "{broken";
    Check(!rock::io::InspectAssetRelations(deletion, materialFile).complete, "読めない文書があれば不完全");
    Check(!rock::io::InspectAssetRelations(deletion, deleteRoot / "project.reproj").complete, "目印ファイルは削除できない");
    fs::remove_all(directory, error);
}

// 同じファイルか（表記の揺れ、正規化の有無を問わない）。
bool Same(const fs::path& a, const fs::path& b) {
    std::error_code error;
    return fs::equivalent(a, b, error) && !error;
}

void TestSurfaceAssets() {
    Section("ProjectWorkspace: レイヤーマテリアル・境界マテリアルの共有アセット");
    const fs::path root = FreshDirectory("surface-assets");
    std::error_code error;
    ProjectWorkspace workspace;
    Check(workspace.Open(root), "ルートを開く");
    const fs::path image = root / "mask.png";
    Touch(image);
    const json layer = {{"id", 5}, {"name", "gravel"}, {"displacement", 0.02}, {"layerBlendRange", 0.2},
                        {"materials", json::array({{{"material", 1}, {"uvRepeat", 2.0}}, {{"material", 0}, {"uvRepeat", 1.0}}})}};
    const json boundary = {{"id", 6}, {"name", "edge"}, {"mask", 1}, {"height", 0}, {"width", 0.5}};
    json scene = {{"version", 26},
                  {"textures", json::array({{{"id", 1}, {"name", "mask"}, {"path", rock::ToUtf8Portable(image)}}})},
                  {"materials", json::array({{{"id", 1}, {"name", "stone"}, {"maps", json::object()}}})},
                  {"skies", json::array()},
                  {"surfaceLayouts", {{"version", 6}, {"layerMaterials", json::array({layer})},
                                      {"boundaryMaterials", json::array({boundary})}}}};
    const json original = scene;
    const fs::path scenePath = root / "Scenes" / "a.rockscene";
    const fs::path layerPath = root / "LayerMaterials" / "gravel.tglayer";
    const fs::path boundaryPath = root / "BoundaryMaterials" / "edge.tgboundary";
    Check(workspace.SaveScene(scenePath, scene), "レイヤー・境界を含むシーンを保存する");
    Check(fs::exists(layerPath) && fs::exists(boundaryPath), "LayerMaterials/ と BoundaryMaterials/ へ分ける");
    const json savedLayer = scene["surfaceLayouts"]["layerMaterials"][0];
    Check(savedLayer["id"] == 5 && savedLayer.contains("asset") && !savedLayer.contains("materials"),
          "シーンの配置データには番号と参照だけを残す");
    json layerFile, boundaryFile;
    Check(workspace.ReadAsset(layerPath, "layer-material-asset", layerFile) &&
              layerFile["materials"][0]["material"] == scene["materials"][0]["asset"] &&
              layerFile["materials"][1]["material"].is_null(),
          "レイヤーのマテリアル参照を固定 ID にする（なしは null）");
    Check(workspace.ReadAsset(boundaryPath, "boundary-material-asset", boundaryFile) &&
              boundaryFile["mask"]["uid"] == scene["textures"][0]["source"]["uid"] && boundaryFile["height"].is_null(),
          "境界の画像を固定 ID で参照する");

    json loaded;
    Check(workspace.ReadScene(scenePath, loaded), "シーンを読む");
    const json expandedLayer = loaded["surfaceLayouts"]["layerMaterials"][0];
    Check(expandedLayer["id"] == 5 && expandedLayer["name"] == "gravel" &&
              expandedLayer["materials"][0]["material"] == 1 && expandedLayer["materials"][1]["material"] == 0,
          "レイヤーの参照をシーンの番号へ戻す");
    Check(Same(rock::FromUtf8(expandedLayer.value("_assetPath", std::string())), layerPath) && expandedLayer["uid"] == savedLayer["asset"]["uid"],
          "展開したレイヤーはファイルと固定 ID を持つ");
    const json expandedBoundary = loaded["surfaceLayouts"]["boundaryMaterials"][0];
    Check(expandedBoundary["id"] == 6 && expandedBoundary["mask"] == 1 && expandedBoundary["height"] == 0 &&
              expandedBoundary["width"] == 0.5,
          "境界の画像参照をシーンの番号へ戻す");

    json resaved = original;
    Check(workspace.SaveScene(root / "Scenes" / "b.rockscene", resaved) &&
              !fs::exists(root / "LayerMaterials" / "gravel_1.tglayer") &&
              !fs::exists(root / "BoundaryMaterials" / "edge_1.tgboundary") &&
              resaved["surfaceLayouts"]["layerMaterials"][0]["asset"]["uid"] == savedLayer["asset"]["uid"],
          "同じ中身の埋め込みは連番の複製を作らない");

    json other = {{"surfaceLayouts", {{"layerMaterials", json::array({{{"id", 9}, {"asset", savedLayer["asset"]}}})}}}};
    Check(workspace.Expand(other) && other["materials"].size() == 1 && other["materials"][0]["name"] == "stone" &&
              other["surfaceLayouts"]["layerMaterials"][0]["materials"][0]["material"] == other["materials"][0]["id"],
          "レイヤーが参照するマテリアルが表に無ければ足す（他のシーンで作ったもの）");
    json legacyScene = {{"surfaceLayouts", {{"layerMaterials", json::array({layer})}}}};
    Check(workspace.Expand(legacyScene) && legacyScene["surfaceLayouts"]["layerMaterials"][0] == layer,
          "共有化前のシーンの埋め込みはそのまま読む");

    const auto relations = rock::io::InspectAssetRelations(workspace, root / "Materials" / "stone.rockmat");
    Check(relations.complete && std::any_of(relations.referencers.begin(), relations.referencers.end(),
                                            [&](const fs::path& path) { return Same(path, layerPath); }),
          "マテリアルを参照するレイヤーを参照元として数える");
    const auto record = rock::io::AssetThumbnailRecord(workspace, layerPath);
    Touch(record.image);
    Check(rock::io::CommitThumbnail(record) && rock::io::ThumbnailIsCurrent(record), "レイヤーのサムネイルを記録する");
    std::ofstream(root / "Materials" / "stone.rockmat", std::ios::app | std::ios::binary).put(' ');
    Check(!rock::io::ThumbnailIsCurrent(rock::io::AssetThumbnailRecord(workspace, layerPath)),
          "参照するマテリアルが変わるとレイヤーのサムネイルは無効になる");
    fs::remove_all(root, error);
}

void TestRename() {
    Section("AssetRelations: 名前の変更");
    const fs::path root = FreshDirectory("rename");
    std::error_code error;
    ProjectWorkspace workspace;
    Check(workspace.Open(root), "ルートを開く");
    const fs::path image = root / "Textures" / "source.png";
    Touch(image);
    const json imageRef = workspace.Reference(image);
    fs::path material = root / "Textures" / "stone.rockmat";
    json body = {{"maps", {{"baseColor", imageRef}}}};
    Check(workspace.SaveAsset(material, "material-asset", body), "参照元のマテリアル");
    const json materialRef = workspace.Reference(material);
    const auto renamedImage = rock::io::RenameAsset(workspace, image, "ground.png");
    Check(Same(renamedImage, root / "Textures" / "ground.png") && fs::exists(renamedImage.wstring() + L".meta") &&
              !fs::exists(image.wstring() + L".meta"),
          "画像の改名は .meta も新しい名前へ揃える");
    Check(Same(workspace.Resolve(imageRef), renamedImage), "参照は改名後の画像を指す");
    Check(rock::io::RenameAsset(workspace, renamedImage, "bad/name.png").empty() &&
              rock::io::RenameAsset(workspace, renamedImage, "").empty() &&
              rock::io::RenameAsset(workspace, renamedImage, ".hidden.png").empty(),
          "使えない名前は拒否する");
    Touch(root / "Textures" / "taken.png");
    Check(rock::io::RenameAsset(workspace, renamedImage, "taken.png").empty() && fs::exists(renamedImage),
          "同じ名前のファイルがあれば変えない");
    const auto folder = rock::io::RenameAsset(workspace, root / "Textures", "Images");
    Check(Same(folder, root / "Images") && Same(workspace.Resolve(materialRef), root / "Images" / "stone.rockmat") &&
              Same(workspace.Resolve(imageRef), root / "Images" / "ground.png"),
          "フォルダの改名でも参照は切れない");
    Check(rock::io::RenameAsset(workspace, workspace.Root(), "Other").empty() && fs::exists(root / "project.reproj"),
          "ルートは改名できない");
    Check(rock::io::RenameAsset(workspace, root / "project.reproj", "other.reproj").empty(), "目印ファイルは改名できない");

    Section("AssetRelations: フォルダへの移動");
    fs::create_directories(root / "Other", error);
    const auto moved = rock::io::MoveAsset(workspace, root / "Images" / "ground.png", root / "Other");
    Check(Same(moved, root / "Other" / "ground.png") && fs::exists(moved.wstring() + L".meta") &&
              !fs::exists(root / "Images" / "ground.png.meta") && Same(workspace.Resolve(imageRef), moved),
          "移動しても .meta と参照が付いてくる");
    Touch(root / "Images" / "ground.png");
    Check(rock::io::MoveAsset(workspace, root / "Images" / "ground.png", root / "Other").empty() &&
              fs::exists(root / "Images" / "ground.png"),
          "移動先に同じ名前があれば動かさない");
    fs::create_directories(root / ".rock-editor", error);
    Check(rock::io::MoveAsset(workspace, root / "Images" / "stone.rockmat", root / ".rock-editor").empty(),
          "内部フォルダへは移さない");
    const auto movedFolder = rock::io::MoveAsset(workspace, root / "Images", root / "Other");
    Check(Same(movedFolder, root / "Other" / "Images") &&
              Same(workspace.Resolve(materialRef), root / "Other" / "Images" / "stone.rockmat"),
          "フォルダを中身ごと移し、参照は切れない");
    Check(rock::io::MoveAsset(workspace, root / "Other", root / "Other" / "Images").empty() &&
              rock::io::MoveAsset(workspace, root / "Other", root / "Other").empty() && fs::exists(root / "Other" / "Images"),
          "フォルダを自分自身やその配下へは移さない");
    Check(rock::io::MoveAsset(workspace, workspace.Root(), root / "Other").empty(), "ルートは移動できない");
    fs::remove_all(root, error);
}

}  // namespace

void RunProjectWorkspaceTests() {
    TestWorkspace();
    TestHistoryAndThumbnails();
    TestSurfaceAssets();
    TestRename();
}
