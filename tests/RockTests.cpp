#include <cmath>
#include <limits>

#include "TestSupport.h"
#include "app/UndoHistory.h"
#include "geometry/Mesh.h"
#include "graph/RockEvaluator.h"
#include "renderer/RockMesh.h"

void RunRockTests() {
    using namespace rock;
    using graph::NodeKind;
    using tests::Check;
    tests::Section("岩生成 — Box トポロジーと描画");
    auto box = geometry::MakeBox({2, 4, 6});
    geometry::MeshInfo info;
    Check(geometry::InspectMesh(box, info) && info.closed && info.components == 1, "Box は閉じた1連結体");
    Check(std::abs(info.volume - 48) < 1e-6 && info.minimum.x == -1 && info.maximum.y == 2 &&
              info.maximum.z == 3,
          "体積と寸法が一致");
    for (const auto& f : box.triangles) {
        const auto n = geometry::FaceNormal(box, f), p = box.positions[f[0]];
        Check(n.x * p.x + n.y * p.y + n.z * p.z > 0, "面法線は外向き");
    }
    renderer::MeshScene scene;
    renderer::SceneMesh mesh;
    mesh.geometry = renderer::MakeRockMeshData(box);
    scene.meshes.push_back(mesh);
    Check(renderer::ValidateMeshScene(scene), "描画データは有効な法線・接線・index を持つ");
    auto open = box;
    open.triangles.pop_back();
    Check(geometry::InspectMesh(open, info) && !info.closed, "穴を検出");
    auto reversed = box;
    std::swap(reversed.triangles[0][0], reversed.triangles[0][1]);
    Check(geometry::InspectMesh(reversed, info) && !info.closed, "逆向きの共有辺を検出");
    auto invalid = box;
    invalid.triangles[0][0] = 999;
    Check(!geometry::InspectMesh(invalid, info), "不正 index を拒否");
    invalid = box;
    invalid.triangles[0][0] = invalid.triangles[0][1];
    Check(!geometry::InspectMesh(invalid, info), "縮退面を拒否");
    invalid = box;
    invalid.positions[0].x = std::numeric_limits<float>::quiet_NaN();
    Check(!geometry::InspectMesh(invalid, info), "非有限座標を拒否");
    Check(geometry::MakeBox({0, 2, 2}).positions.empty() && geometry::MakeBox({-1, 2, 2}).positions.empty(),
          "無効な寸法を拒否");
    auto tiny = geometry::MakeBox({0.001f, 0.001f, 0.001f});
    Check(geometry::InspectMesh(tiny, info) && info.closed && info.volume > 0, "最小寸法でも閉じた Box");
    auto two = box;
    for (auto p : box.positions) {
        p.x += 10;
        two.positions.push_back(p);
    }
    for (auto f : box.triangles) {
        for (auto& i : f) i += 8;
        two.triangles.push_back(f);
    }
    Check(geometry::InspectMesh(two, info) && info.components == 2, "離れた2成分を区別");

    tests::Section("岩生成 — グラフ、プレビュー、再評価、Undo");
    graph::NodeGraph graph;
    const auto rock = graph.CreateNode(NodeKind::BaseRock), output = graph.CreateNode(NodeKind::MeshOutput);
    const auto outputPin = graph.FindNode(rock)->outputs[0].id;
    Check(graph.CreateLink(outputPin, graph.FindNode(output)->inputs[0].id),
          "Base Rock を Mesh Output へ接続");
    Check(graph::EvaluateRocks(graph).rocks.size() == 1, "接続した岩を生成");
    const auto transform = graph.CreateNode(NodeKind::Transform);
    Check(!graph.CanCreateLink(outputPin, graph.FindNode(transform)->inputs[0].id),
          "モデル専用 Transform への誤接続を拒否");
    const auto other = graph.CreateNode(NodeKind::BaseRock);
    Check(graph::EvaluateRocks(graph).rocks.size() == 1, "未接続の岩は出力しない");
    Check(graph::EvaluateRocks(graph, other).rocks[0].source == other, "未接続の岩も途中プレビューできる");
    Check(graph::EvaluateRocks(graph, transform).rocks.empty(), "モデルのプレビューには岩を混ぜない");
    const auto merge = graph.CreateNode(NodeKind::Merge);
    graph.CreateLink(outputPin, graph.FindNode(merge)->inputs.back().id);
    graph.CreateLink(outputPin, graph.FindNode(merge)->inputs.back().id);
    graph.CreateLink(graph.FindNode(other)->outputs[0].id, graph.FindNode(merge)->inputs.back().id);
    graph.CreateLink(graph.FindNode(merge)->outputs[0].id, graph.FindNode(output)->inputs[0].id);
    Check(graph::EvaluateRocks(graph).rocks.size() == 2, "Merge は異なる岩を集め、同じ岩は重複しない");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    auto& settings = std::get<graph::BaseRockNodeSettings>(graph.FindMutableNode(rock)->settings);
    settings.size = {3, 4, 5};
    settings.seed = 123;
    graph.MarkDirty();
    const auto evaluated = graph::EvaluateRocks(graph, rock);
    geometry::InspectMesh(evaluated.rocks[0].mesh, info);
    Check(info.volume == 60, "寸法変更を再評価");
    const auto repeated = graph::EvaluateRocks(graph, rock);
    Check(repeated.rocks[0].mesh.triangles == evaluated.rocks[0].mesh.triangles &&
              repeated.rocks[0].mesh.positions == evaluated.rocks[0].mesh.positions,
          "同じ設定で再現");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    const auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(std::get<graph::BaseRockNodeSettings>(graph.FindNode(rock)->settings).size[0] == 2,
          "Undo で寸法を復元");
    const auto redone = history.Redo(restored);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(std::get<graph::BaseRockNodeSettings>(graph.FindNode(rock)->settings).seed == 123,
          "Redo で seed を復元");
    std::get<graph::BaseRockNodeSettings>(graph.FindMutableNode(rock)->settings).size[0] = -1;
    const auto failed = graph::EvaluateRocks(graph);
    Check(failed.rocks.empty() && !failed.error.empty(), "無効な岩は部分出力せず診断する");
}
