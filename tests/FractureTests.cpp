#include <cmath>
#include <limits>
#include <random>
#include "TestSupport.h"
#include "app/UndoHistory.h"
#include "fracture/PlaneSplit.h"
#include "graph/RockEvaluator.h"
#include "renderer/RockMesh.h"

void RunFractureTests() {
    using namespace rock;
    using tests::Check;
    tests::Section("完全分割 — 閉包・体積・断面");
    const auto box = geometry::MakeBox({2, 2, 2});
    const auto verify = [&](const fracture::SplitResult& result, double volume) {
        Check(result.error.empty(), result.error.empty() ? "分割成功" : result.error.c_str());
        if (!result.error.empty()) return;
        double sum = 0;
        for (const auto& mesh : result.meshes) {
            geometry::MeshInfo info;
            Check(geometry::InspectMesh(mesh, info) && info.closed && info.components == 1 && info.volume > 0,
                  "各片が外向きの閉じた1連結体");
            sum += info.volume;
            renderer::MeshScene scene;
            renderer::SceneMesh part;
            part.geometry = renderer::MakeRockMeshData(mesh);
            scene.meshes.push_back(part);
            Check(renderer::ValidateMeshScene(scene), "分割片を描画できる");
        }
        Check(std::abs(sum - volume) < volume * 1e-5, "分割前後で体積保存");
    };
    auto result = fracture::SplitByPlane(box, {0, 0, 0}, {0, 0, 1});
    verify(result, 8);
    Check(std::abs(result.sectionArea - 4) < 1e-6, "中央断面は4平方メートル");
    for (int side = 0; side < 2; ++side) {
        double capArea = 0;
        for (const auto& face : result.meshes[side].triangles) {
            const auto& mesh = result.meshes[side];
            const auto a = mesh.positions[face[0]], b = mesh.positions[face[1]], c = mesh.positions[face[2]];
            if (a.z == 0 && b.z == 0 && c.z == 0) {
                Check(geometry::FaceNormal(mesh, face).z == (side == 0 ? 1.0f : -1.0f),
                      "切断面は反対向きの法線");
                capArea += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5;
            }
        }
        Check(std::abs(capArea - 4) < 1e-6, "各側の閉包面が断面全体を覆う");
    }
    const auto reverse = fracture::SplitByPlane(box, {}, {0, 0, -1});
    verify(reverse, 8);
    geometry::MeshInfo reversedInfo;
    geometry::InspectMesh(reverse.meshes[0], reversedInfo);
    Check(reversedInfo.minimum.z == 0 && reversedInfo.maximum.z == 1, "法線反転で Chunk の符号を交換");
    Check(!fracture::SplitByPlane(box, {0, 0, 0.9999999f}, {0, 0, 1}).error.empty(),
          "許容誤差以下の極小片を拒否");
    verify(fracture::SplitByPlane(box, {0, 0, 0}, {1, 1, 0}), 8);  // 既存の辺上
    verify(fracture::SplitByPlane(box, {0, 0, 0}, {1, 1, 2}), 8);  // 既存の頂点上
    verify(fracture::SplitByPlane(box, {0, 0, 0.999f}, {0, 0, 1}), 8);
    Check(!fracture::SplitByPlane(box, {0, 0, 1}, {0, 0, 1}).error.empty(), "面への接触は分割しない");
    Check(!fracture::SplitByPlane(box, {0, 0, 2}, {0, 0, 1}).error.empty(), "外側平面を拒否");
    Check(!fracture::SplitByPlane(box, {}, {}).error.empty(), "ゼロ法線を拒否");
    Check(!fracture::SplitByPlane(box, {}, {std::numeric_limits<float>::quiet_NaN(), 0, 1}).error.empty(),
          "非有限平面を拒否");
    auto far = box;
    for (auto& p : far.positions) {
        p.x += 10000;
        p.y -= 8000;
        p.z += 5000;
    }
    geometry::MeshInfo farInfo;
    geometry::InspectMesh(far, farInfo);
    Check(std::abs(farInfo.volume - 8) < 1e-10, "原点から離れても体積積分は安定");
    auto open = box;
    open.triangles.pop_back();
    Check(!fracture::SplitByPlane(open, {}, {0, 0, 1}).error.empty(), "開いた入力を拒否");
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> direction(-1, 1), size(0.1f, 20);
    for (int i = 0; i < 160; ++i) {
        const std::array<float, 3> dimensions{size(rng), size(rng), size(rng)};
        const geometry::Vec3 normal{direction(rng), direction(rng), direction(rng)};
        const geometry::Vec3 center{direction(rng) * dimensions[0] * 0.2f,
                                    direction(rng) * dimensions[1] * 0.2f,
                                    direction(rng) * dimensions[2] * 0.2f};
        const auto mesh = geometry::MakeBox(dimensions);
        geometry::MeshInfo info;
        geometry::InspectMesh(mesh, info);
        verify(fracture::SplitByPlane(mesh, center, normal), info.volume);
    }
    for (float length : {0.001f, 1000.0f})
        verify(fracture::SplitByPlane(geometry::MakeBox({length, length, length}), {}, {1, 2, 3}),
               double(length) * length * length);
    crack::CrackSettings cut;
    cut.extentV = 1;
    cut.depth = 1.4f;
    cut.persistence = 1;
    cut.aperture = 0.2f;
    auto partial = crack::CutBox({2, 2, 2}, cut);
    geometry::MeshInfo partialInfo;
    geometry::InspectMesh(partial.mesh, partialInfo);
    result = fracture::SplitByPlane(partial.mesh, {}, {0, 0, 1});
    verify(result, partialInfo.volume);
    Check(std::abs(result.sectionArea - 1.2) < 1e-5, "Bridge の残った断面だけ閉じる");
    cut.extentU = 0.65f;
    partial = crack::CutBox({2, 2, 2}, cut);
    geometry::InspectMesh(partial.mesh, partialInfo);
    result = fracture::SplitByPlane(partial.mesh, {}, {0, 0, 1});
    verify(result, partialInfo.volume);  // 凹んだ U 字の断面
    Check(std::abs(result.sectionArea - 2.18) < 1e-5, "有限長の溝を残した凹断面");
    result = fracture::SplitByPlane(partial.mesh, {0, 0.5f, 0}, {0, 1, 0});
    Check(!result.error.empty() && result.meshes[0].triangles.empty(), "穴のある断面を部分出力せず診断");

    tests::Section("完全分割 — Chunk ID・移動・Locked・Undo");
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(graph::NodeKind::BaseRock);
    const auto fracture = graph.CreateNode(graph::NodeKind::Fracture);
    const auto output = graph.CreateNode(graph::NodeKind::MeshOutput);
    graph.CreateLink(graph.FindNode(base)->outputs[0].id, graph.FindNode(fracture)->inputs[0].id);
    graph.CreateLink(graph.FindNode(fracture)->outputs[0].id, graph.FindNode(output)->inputs[0].id);
    auto evaluated = graph::EvaluateRocks(graph);
    Check(evaluated.error.empty() && evaluated.rocks.size() == 2, "2つの Chunk を出力");
    if (evaluated.rocks.size() != 2) return;
    Check(evaluated.rocks[0].source == fracture && evaluated.rocks[0].chunk == 1 &&
              evaluated.rocks[1].chunk == 2 && evaluated.rocks[0].parent == base && evaluated.rocks[0].locked,
          "ID・親・初期 Locked を保持");
    Check(evaluated.fractures.size() == 1 && evaluated.fractures[0].sectionArea == 4, "分割面の関係を保持");
    const auto original = evaluated;
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    auto& settings = std::get<fracture::FractureSettings>(graph.FindMutableNode(fracture)->settings);
    settings.chunks[1].locked = false;
    settings.chunks[1].position = {0, 0, 2};
    settings.chunks[1].rotationDegrees = {0, 30, 0};
    graph.MarkDirty();
    evaluated = graph::EvaluateRocks(graph);
    Check(evaluated.rocks[0].mesh.positions == original.rocks[0].mesh.positions &&
              evaluated.rocks[1].mesh.positions != original.rocks[1].mesh.positions,
          "片方だけ移動・回転");
    Check(evaluated.rocks[1].chunk == 2 && !evaluated.rocks[1].locked, "移動しても ID が不変");
    geometry::MeshInfo info;
    geometry::InspectMesh(evaluated.rocks[1].mesh, info);
    Check(std::abs(info.volume - 4) < 1e-5 && info.minimum.z > 1, "剛体変換で体積を維持");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    const auto undone = history.Undo(after);
    graph.Replace(undone.graphNodes, undone.graphLinks);
    Check(graph::EvaluateRocks(graph).rocks[1].mesh.positions == original.rocks[1].mesh.positions,
          "Undo で配置を復元");
    const auto redone = history.Redo(undone);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(graph::EvaluateRocks(graph).rocks[1].mesh.positions == evaluated.rocks[1].mesh.positions,
          "Redo で配置を再現");
    const auto merge = graph.CreateNode(graph::NodeKind::Merge);
    graph.CreateLink(graph.FindNode(fracture)->outputs[0].id, graph.FindNode(merge)->inputs.back().id);
    graph.CreateLink(graph.FindNode(fracture)->outputs[0].id, graph.FindNode(merge)->inputs.back().id);
    Check(graph::EvaluateRocks(graph, merge).rocks.size() == 2, "Merge は Chunk ID ごとに重複除去");
    const auto again = graph.CreateNode(graph::NodeKind::Fracture);
    graph.CreateLink(graph.FindNode(fracture)->outputs[0].id, graph.FindNode(again)->inputs[0].id);
    Check(!graph::EvaluateRocks(graph, again).error.empty(), "再帰分割は診断");
    const auto crack = graph.CreateNode(graph::NodeKind::Crack);
    auto& crackSettings = std::get<crack::CrackSettings>(graph.FindMutableNode(crack)->settings);
    crackSettings = cut;
    crackSettings.applyCut = true;
    graph.CreateLink(graph.FindNode(base)->outputs[0].id, graph.FindNode(crack)->inputs[0].id);
    graph.CreateLink(graph.FindNode(crack)->outputs[0].id, graph.FindNode(fracture)->inputs[0].id);
    evaluated = graph::EvaluateRocks(graph);
    Check(evaluated.error.empty() && evaluated.rocks.size() == 2 && evaluated.cuts.empty() &&
              evaluated.cracks.empty(),
          "部分亀裂から明示的に分割し、古いガイドを除く");
}
