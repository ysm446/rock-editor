#include <cmath>
#include <set>
#include "TestSupport.h"
#include "app/UndoHistory.h"
#include "fracture/MultiSplit.h"
#include "graph/RockEvaluator.h"

void RunMultiSplitTests() {
    using namespace rock;
    using fracture::SplitPlane;
    using graph::NodeKind;
    using tests::Check;
    const auto box = geometry::MakeBox({2, 2, 2});
    const std::vector<SplitPlane> grid = {
        {{0, 0, 0}, {0, 0, 1}, "z"}, {{0, 0, 0}, {1, 0, 0}, "x"}, {{0, 0, 0}, {0, 1, 0}, "y"}};
    const auto validate = [&](const auto& split, double expected) {
        Check(split.error.empty(), split.error.empty() ? "多片分割が成功" : split.error.c_str());
        double volume = 0;
        std::set<std::string> keys;
        for (const auto& piece : split.pieces) {
            geometry::MeshInfo info;
            Check(geometry::InspectMesh(piece.mesh, info) && info.closed && info.components == 1 &&
                      info.volume > 0,
                  "全片が閉じた1連結体で正の体積");
            volume += info.volume;
            Check(keys.insert(piece.key).second, "半空間キーが一意");
        }
        Check(std::abs(volume - expected) <= expected * 1e-4, "多片の体積和を保存");
        for (const auto& c : split.connections)
            Check(c.negative < split.pieces.size() && c.positive < split.pieces.size() &&
                      c.negative != c.positive && c.area > 0,
                  "接続は実在する最終片と正の断面積");
    };
    tests::Section("Joint Set 完全分割 — 複数方向と接続");
    auto split = fracture::SplitByPlanes(box, grid);
    validate(split, 8);
    Check(split.pieces.size() == 8 && split.connections.size() == 12, "直交3平面で8片・12共有断面");
    for (const auto& c : split.connections) Check(std::abs(c.area - 1) < 1e-5, "単位正方形の共有断面");
    auto repeated = fracture::SplitByPlanes(box, grid);
    Check(split.pieces.size() == repeated.pieces.size(), "再評価の片数");
    for (size_t i = 0; i < split.pieces.size(); ++i)
        Check(split.pieces[i].key == repeated.pieces[i].key &&
                  split.pieces[i].mesh.positions == repeated.pieces[i].mesh.positions &&
                  split.pieces[i].mesh.triangles == repeated.pieces[i].mesh.triangles,
              "ID・頂点・面を再現");
    std::vector<SplitPlane> parallel;
    for (int i = 0; i < 3; ++i) parallel.push_back({{0, 0, (i - 1) * 0.5f}, {0, 0, 1}, std::to_string(i)});
    split = fracture::SplitByPlanes(box, parallel);
    validate(split, 8);
    Check(split.pieces.size() == 4 && split.connections.size() == 3, "平行3平面で4片と3接続");
    auto duplicate = grid;
    duplicate.push_back({{0, 0, 0}, {0, 0, -1}, "duplicate"});
    duplicate.push_back({{10, 0, 0}, {1, 0, 0}, "outside"});
    duplicate.push_back({{0, 1, 0}, {0, 1, 0}, "touch"});
    split = fracture::SplitByPlanes(box, duplicate);
    validate(split, 8);
    Check(split.pieces.size() == 8 && split.connections.size() == 12,
          "同一面・逆法線・非交差・接触で余分な片を作らない");
    for (const auto shape :
         {geometry::BaseShape::RoundedBox, geometry::BaseShape::Sphere, geometry::BaseShape::Ellipsoid}) {
        geometry::BaseRockSettings settings;
        settings.shape = shape;
        settings.size = {2, 2.5f, 1.7f};
        settings.subdivisions = 8;
        for (float noise : {0.0f, 0.08f}) {
            settings.noiseStrength = noise;
            settings.seed = 42;
            std::string error;
            const auto mesh = geometry::MakeBaseRock(settings, error);
            geometry::MeshInfo info;
            geometry::InspectMesh(mesh, info);
            split = fracture::SplitByPlanes(mesh, grid);
            validate(split, info.volume);
            Check(split.pieces.size() == 8, "曲面と弱いノイズを3方向で分割");
        }
    }
    // 斜めの節理とばらつき。中心/辺/既存キャップを横切る再分割を含む。
    for (int seed : {0, 1, 42}) {
        crack::JointSetSettings settings;
        settings.count = 3;
        settings.seed = seed;
        std::vector<crack::CrackPatch> patches;
        std::string error;
        crack::BuildJointSet(settings, patches, error);
        std::vector<SplitPlane> planes;
        for (size_t i = 0; i < patches.size(); ++i)
            planes.push_back({patches[i].center, patches[i].normal, "a" + std::to_string(i)});
        settings.rotationDegrees = {12, 80, 8};
        settings.count = 2;
        crack::BuildJointSet(settings, patches, error);
        for (size_t i = 0; i < patches.size(); ++i)
            planes.push_back({patches[i].center, patches[i].normal, "b" + std::to_string(i)});
        split = fracture::SplitByPlanes(box, planes);
        validate(split, 8);
        Check(split.pieces.size() >= 12, "ばらつき付きの2系統を分割");
    }
    const auto reject = [&](const auto& planes) {
        const auto r = fracture::SplitByPlanes(box, planes);
        Check(!r.error.empty() && r.pieces.empty() && r.connections.empty(),
              "不正・上限超過は部分結果を返さない");
    };
    reject(std::vector<SplitPlane>{});
    reject(std::vector<SplitPlane>{{{0, 0, 0}, {0, 0, 0}, "zero"}});
    reject(std::vector<SplitPlane>{{{0, 0, 3}, {0, 0, 1}, "outside"}});
    reject(std::vector<SplitPlane>(33, grid.front()));
    auto many = std::vector<SplitPlane>{};
    for (int axis = 0; axis < 3; ++axis)
        for (int i = 0; i < 5; ++i) {
            geometry::Vec3 center{}, normal{};
            if (axis == 0) {
                center.x = (i - 2) * 0.3f;
                normal.x = 1;
            }
            if (axis == 1) {
                center.y = (i - 2) * 0.3f;
                normal.y = 1;
            }
            if (axis == 2) {
                center.z = (i - 2) * 0.3f;
                normal.z = 1;
            }
            many.push_back({center, normal, std::to_string(axis) + ":" + std::to_string(i)});
        }
    reject(many);  // 6^3 > 128
    Check(fracture::SplitByPlanes(box, many).error.find("128片") != std::string::npos, "128片上限による診断");
    std::vector<SplitPlane> thirtyTwo;
    for (int i = 0; i < 32; ++i)
        thirtyTwo.push_back({{0, 0, -0.9f + i * 0.055f}, {0, 0, 1}, std::to_string(i)});
    split = fracture::SplitByPlanes(box, thirtyTwo);
    validate(split, 8);
    Check(split.pieces.size() == 33 && split.connections.size() == 32, "32平面の上限で33片を生成");

    tests::Section("Joint Set 完全分割 — グラフと編集キー");
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(NodeKind::BaseRock), joint = graph.CreateNode(NodeKind::JointSet),
               fracture = graph.CreateNode(NodeKind::Fracture),
               output = graph.CreateNode(NodeKind::MeshOutput);
    const auto link = [&](auto a, auto b) {
        return graph.CreateLink(graph.FindNode(a)->outputs[0].id, graph.FindNode(b)->inputs[0].id);
    };
    Check(link(base, joint) && link(joint, fracture) && link(fracture, output), "母岩→節理→完全分割→出力");
    auto& j = std::get<crack::JointSetSettings>(graph.FindMutableNode(joint)->settings);
    j.spacingVariance = j.angleVariance = 0;
    j.showGuide = false;
    j.depth = 0;
    j.persistence = 0;
    j.extentU = 0.001f;
    auto& f = std::get<fracture::FractureSettings>(graph.FindMutableNode(fracture)->settings);
    f.useJointSets = true;
    auto result = graph::EvaluateRocks(graph);
    Check(result.error.empty() && result.rocks.size() == 4 && result.fractures.size() == 3 &&
              result.cracks.empty(),
          "非表示・深さ0・有限範囲外でも明示モードは無限平面で完全分割");
    if (result.rocks.size() != 4) return;
    const auto key = result.rocks[3].key;
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    f.jointChunks[key].locked = false;
    f.jointChunks[key].position = {2, 0, 0};
    f.jointChunks[key].rotationDegrees = {0, 20, 0};
    result = graph::EvaluateRocks(graph);
    Check(result.error.empty() && result.rocks[3].key == key && !result.rocks[3].locked,
          "第4片の移動・回転とキーを保持");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(graph::EvaluateRocks(graph).rocks[3].locked, "Undo で4片目の編集前へ復元");
    auto redone = history.Redo(restored);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(!graph::EvaluateRocks(graph).rocks[3].locked, "Redo で4片目の編集を復元");
    std::get<crack::JointSetSettings>(graph.FindMutableNode(joint)->settings).count = 1;
    result = graph::EvaluateRocks(graph);
    Check(
        result.error.empty() && result.rocks.size() == 2 && result.rocks[0].locked && result.rocks[1].locked,
        "平面集合変更時は別キーの片へ古い変換を適用しない");
    std::get<fracture::FractureSettings>(graph.FindMutableNode(fracture)->settings).useJointSets = false;
    Check(graph::EvaluateRocks(graph).rocks.size() == 2, "従来の単一平面モードを維持");
}
