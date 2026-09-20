#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

#include "TestSupport.h"
#include "app/UndoHistory.h"
#include "crack/PartialCut.h"
#include "graph/RockEvaluator.h"
#include "renderer/CrackGuide.h"
#include "renderer/RockMesh.h"

namespace {
// 閉じた辺だけでなく、各頂点の周囲の面が一つの環になっていることを調べる。
bool ManifoldVertices(const rock::geometry::Mesh& mesh) {
    for (uint32_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
        std::map<uint32_t, std::vector<uint32_t>> link;
        for (const auto& f : mesh.triangles)
            for (int i = 0; i < 3; ++i)
                if (f[i] == vertex) {
                    const auto a = f[(i + 1) % 3], b = f[(i + 2) % 3];
                    link[a].push_back(b);
                    link[b].push_back(a);
                }
        if (link.empty()) return false;
        for (const auto& [id, neighbors] : link)
            if (neighbors.size() != 2) return false;
        std::set<uint32_t> visited;
        std::vector<uint32_t> pending{link.begin()->first};
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!visited.insert(id).second) continue;
            for (auto next : link[id]) pending.push_back(next);
        }
        if (visited.size() != link.size()) return false;
    }
    return true;
}
}  // namespace
void RunPartialCutTests() {
    using namespace rock;
    using graph::NodeKind;
    using tests::Check;
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-5; };
    tests::Section("部分切断 — 閉包、切断壁、Rock Bridge");
    crack::CrackSettings s;
    s.applyCut = true;
    s.showGuide = false;
    s.showBridge = false;
    s.extentU = 1.2f;
    s.extentV = 1;
    s.depth = 1.5f;
    s.persistence = 1;
    s.aperture = 0.2f;
    auto result = crack::CutBox({2, 2, 2}, s);
    geometry::MeshInfo info;
    Check(result.error.empty() && result.bridge.has_value(), "実切断と Bridge を生成");
    Check(geometry::InspectMesh(result.mesh, info) && info.closed && info.components == 1 &&
              ManifoldVertices(result.mesh),
          "閉じた1連結体、全頂点は manifold");
    Check(near(info.volume, 7.4) && near(result.removedVolume, 0.6), "除去体積と残存体積が一致");
    Check(
        near(result.penetration, 1.5) && near(result.bridge->thickness, 0.5) && near(result.bridge->area, 1),
        "Bridge の厚さと断面積が実形状に一致");
    renderer::MeshScene scene;
    renderer::SceneMesh drawn;
    drawn.geometry = renderer::MakeRockMeshData(result.mesh);
    scene.meshes.push_back(drawn);
    Check(renderer::ValidateMeshScene(scene), "切断面も描画可能な法線・接線を持つ");
    Check(renderer::MakeBridgeGuides(*result.bridge).size() == 2, "Bridge の断面を面と輪郭で可視化");
    bool boundaryCorrect = true, hasFloor = false, hasWalls = false;
    const auto inside = [](geometry::Vec3 p) {
        return std::abs(p.x) < 1 && std::abs(p.y) < 1 && std::abs(p.z) < 1 &&
               !(p.y > -0.5f && std::abs(p.z) < 0.1f);
    };
    for (const auto& f : result.mesh.triangles) {
        const auto a = result.mesh.positions[f[0]], b = result.mesh.positions[f[1]],
                   c = result.mesh.positions[f[2]], n = geometry::FaceNormal(result.mesh, f);
        geometry::Vec3 p{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
        boundaryCorrect &= !inside({p.x + n.x * 1e-4f, p.y + n.y * 1e-4f, p.z + n.z * 1e-4f}) &&
                           inside({p.x - n.x * 1e-4f, p.y - n.y * 1e-4f, p.z - n.z * 1e-4f});
        hasFloor |= near(p.y, -0.5) && n.y > 0.9;
        hasWalls |= near(std::abs(p.z), 0.1) && p.y > -0.5;
    }
    Check(boundaryCorrect && hasFloor && hasWalls, "全三角形は正しい固体境界。内部面なし、亀裂壁と終端あり");
    auto original = s;
    s.extentU = 0.45f;
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && info.closed &&
              ManifoldVertices(result.mesh) && near(result.removedVolume, 0.27),
          "有限 U の両端にも壁を生成");
    s.center = {0.8f, 0, 0.3f};
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && info.closed &&
              ManifoldVertices(result.mesh),
          "移動し外面をまたぐ有限範囲も閉じる");
    s = original;
    s.rotationDegrees = {0, 90, 0};
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && near(info.volume, 7.4),
          "軸を入れ替えた回転でも切断");
    s.rotationDegrees = {90, 0, 0};
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && info.closed &&
              ManifoldVertices(result.mesh),
          "Z 外面からの切り込み");
    s.rotationDegrees = {0, 0, 180};
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && info.closed &&
              near(result.bridge->thickness, 0.5),
          "反対面からの切り込み");

    tests::Section("部分切断 — 制約と境界値");
    s = original;
    s.aperture = 0;
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.error.empty() && !result.bridge &&
              result.mesh.positions == geometry::MakeBox({2, 2, 2}).positions,
          "幅ゼロは元の形状");
    s = original;
    s.persistence = 0;
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.error.empty() && !result.bridge, "深さゼロは切断しない");
    s = original;
    s.center[2] = 10;
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.error.empty() && !result.bridge && !result.status.empty(), "非交差は形状を保って通知");
    s = original;
    s.depth = 2;
    result = crack::CutBox({2, 2, 2}, s);
    Check(!result.error.empty() && result.mesh.positions.empty(), "Bridge 消失を拒否");
    s = original;
    s.rotationDegrees[1] = 20;
    result = crack::CutBox({2, 2, 2}, s);
    Check(!result.error.empty(), "任意角度は診断し、軸へ勝手に丸めない");
    s = original;
    s.extentV = 0.5f;
    result = crack::CutBox({2, 2, 2}, s);
    Check(!result.error.empty(), "表面に届かない内部空洞を拒否");
    s = original;
    s.aperture = 2;
    result = crack::CutBox({2, 2, 2}, s);
    Check(!result.error.empty(), "両側の岩が消える開口を拒否");
    s = original;
    s.extentV = 1.2f;
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.bridge && near(result.penetration, 1.3) && near(result.bridge->thickness, 0.7),
          "母岩外の区間を実切込深さに含めない");
    s = original;
    s.depth = 1.9999f;
    result = crack::CutBox({2, 2, 2}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && info.closed &&
              result.bridge->thickness > 0,
          "薄い Bridge も正の厚さを保つ");

    std::mt19937 random(31415);
    std::uniform_real_distribution<float> unit(0, 1);
    bool randomValid = true;
    for (int i = 0; i < 160; ++i) {
        const std::array<float, 3> size{0.1f + unit(random) * 5, 0.1f + unit(random) * 5,
                                        0.1f + unit(random) * 5};
        s = {};
        s.extentU = size[0] * (0.1f + unit(random));
        s.extentV = size[1] * 0.5f;
        s.depth = size[1] * (0.05f + 0.9f * unit(random));
        s.persistence = 1;
        s.aperture = size[2] * (0.02f + 0.4f * unit(random));
        result = crack::CutBox(size, s);
        randomValid &= result.error.empty() && result.bridge && geometry::InspectMesh(result.mesh, info) &&
                       info.closed && info.components == 1 && ManifoldVertices(result.mesh);
    }
    Check(randomValid, "160 組の寸法・有限範囲・深さで閉包と連結性を維持");

    bool orientationsValid = true;
    for (float x : {0.0f, 90.0f, 180.0f, 270.0f})
        for (float y : {0.0f, 90.0f, 180.0f, 270.0f})
            for (float z : {0.0f, 90.0f, 180.0f, 270.0f}) {
                s = original;
                s.rotationDegrees = {x, y, z};
                result = crack::CutBox({2, 2, 2}, s);
                orientationsValid &= result.bridge && geometry::InspectMesh(result.mesh, info) &&
                                     info.closed && near(info.volume, 7.4);
            }
    Check(orientationsValid, "90 度単位の64組の回転でも閉包・体積を維持");
    s = original;
    s.extentU = 0.001f;
    s.extentV = 0.001f;
    s.depth = 0.0013f;
    s.aperture = 0.0001f;
    result = crack::CutBox({0.001f, 0.001f, 0.001f}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && info.closed &&
              ManifoldVertices(result.mesh),
          "最小寸法の Box に部分切断");
    s = original;
    s.extentU = 600;
    s.extentV = 500;
    s.depth = 750;
    s.aperture = 50;
    result = crack::CutBox({1000, 1000, 1000}, s);
    Check(result.bridge && geometry::InspectMesh(result.mesh, info) && info.closed && info.components == 1,
          "最大寸法の Box に部分切断");

    tests::Section("部分切断 — 非破壊グラフと Undo");
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(NodeKind::BaseRock), cut = graph.CreateNode(NodeKind::Crack),
               output = graph.CreateNode(NodeKind::MeshOutput);
    graph.CreateLink(graph.FindNode(base)->outputs[0].id, graph.FindNode(cut)->inputs[0].id);
    graph.CreateLink(graph.FindNode(cut)->outputs[0].id, graph.FindNode(output)->inputs[0].id);
    std::get<crack::CrackSettings>(graph.FindMutableNode(cut)->settings) = original;
    const auto evaluated = graph::EvaluateRocks(graph), upstream = graph::EvaluateRocks(graph, base);
    Check(evaluated.error.empty() && evaluated.cuts.size() == 1 && evaluated.cuts[0].bridge &&
              evaluated.cracks.empty(),
          "ガイド非表示でも実形状と Bridge 情報を生成");
    geometry::InspectMesh(evaluated.rocks[0].mesh, info);
    const auto cutVolume = info.volume;
    geometry::InspectMesh(upstream.rocks[0].mesh, info);
    Check(near(cutVolume, 7.4) && near(info.volume, 8), "上流 Box を変更せず加工枝だけを変更");
    const auto out2 = graph.CreateNode(NodeKind::MeshOutput);
    graph.CreateLink(graph.FindNode(base)->outputs[0].id, graph.FindNode(out2)->inputs[0].id);
    Check(graph::EvaluateRocks(graph).rocks.size() == 2, "加工前と加工後の枝を別結果として保持");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    std::get<crack::CrackSettings>(graph.FindMutableNode(cut)->settings).applyCut = false;
    graph.MarkDirty();
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    const auto undone = history.Undo(after);
    graph.Replace(undone.graphNodes, undone.graphLinks);
    Check(graph::EvaluateRocks(graph, cut).cuts[0].bridge.has_value(), "Undo で実切断を復元");
    const auto redone = history.Redo(undone);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(graph::EvaluateRocks(graph, cut).cuts.empty(), "Redo でガイドのみへ戻す");
    std::get<crack::CrackSettings>(graph.FindMutableNode(cut)->settings) = original;
    const auto next = graph.CreateNode(NodeKind::Crack);
    std::get<crack::CrackSettings>(graph.FindMutableNode(next)->settings) = original;
    graph.CreateLink(graph.FindNode(cut)->outputs[0].id, graph.FindNode(next)->inputs[0].id);
    const auto rejected = graph::EvaluateRocks(graph, next);
    Check(!rejected.error.empty() && rejected.rocks.empty() && rejected.cuts.empty(),
          "複数切断は古い結果を返さず診断");
}
