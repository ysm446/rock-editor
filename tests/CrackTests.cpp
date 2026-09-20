#include <cmath>
#include <limits>

#include "TestSupport.h"
#include "app/UndoHistory.h"
#include "crack/CrackPatch.h"
#include "graph/RockEvaluator.h"
#include "renderer/CrackGuide.h"

void RunCrackTests() {
    using namespace rock;
    using graph::NodeKind;
    using tests::Check;
    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    tests::Section("有限亀裂 — 座標系と深さ");
    crack::CrackSettings settings;
    crack::CrackPatch patch;
    std::string error;
    Check(crack::BuildCrackPatch(settings, patch, error), "既定パッチを生成");
    Check(near(patch.effectiveDepth, 0.96f) && near(patch.reached[2].y, 0.24f),
          "+V 端から指定深さ×Persistence だけ進む");
    Check(patch.boundary[0] == geometry::Vec3{-1.2f, 1.2f, 0} && patch.normal == geometry::Vec3{0, 0, 1},
          "有限範囲と既定の面法線");
    settings.depth = 100;
    settings.persistence = 1;
    Check(crack::BuildCrackPatch(settings, patch, error) && near(patch.effectiveDepth, 2.4f),
          "深さを V 全幅で制限");
    Check(patch.boundary == patch.reached, "Persistence 1 と十分な深さで全域に到達");
    settings.persistence = 0;
    Check(crack::BuildCrackPatch(settings, patch, error) && patch.effectiveDepth == 0,
          "Persistence 0 は進行なし");
    auto guides = renderer::MakeCrackGuides(patch);
    Check(guides.size() == 2, "深さゼロでは候補面と枠だけを表示");
    settings.persistence = 1;
    settings.depth = 0;
    Check(crack::BuildCrackPatch(settings, patch, error) && patch.effectiveDepth == 0, "Depth 0 は進行なし");
    settings.depth = 1;
    settings.rotationDegrees = {0, 90, 0};
    settings.center = {2, 3, 4};
    Check(
        crack::BuildCrackPatch(settings, patch, error) && near(patch.normal.x, 1) && near(patch.normal.z, 0),
        "Y 回転で法線を回転");
    Check(near(patch.boundary[0].x, 2) && near(patch.boundary[0].y, 4.2f) && near(patch.boundary[0].z, 5.2f),
          "中心移動と回転が角へ反映");
    settings.rotationDegrees = {30, 40, 50};
    crack::BuildCrackPatch(settings, patch, error);
    const auto dot = [](auto a, auto b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    Check(near(dot(patch.tangentU, patch.tangentV), 0) && near(dot(patch.normal, patch.tangentU), 0) &&
              near(dot(patch.normal, patch.normal), 1),
          "複合回転後も正規直交基底");
    settings.aperture = 0.2f;
    crack::BuildCrackPatch(settings, patch, error);
    guides = renderer::MakeCrackGuides(patch);
    Check(guides.size() == 5 && guides[0].triangles && !guides[0].depthTest && guides[0].color.w < 1,
          "候補面と到達面を透視・半透明で生成");
    const auto p = guides.back().points[0], q = guides.back().points[1];
    Check(near(std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z)),
               0.2f),
          "開口幅は法線方向に反映");
    settings.aperture = 0;
    crack::BuildCrackPatch(settings, patch, error);
    Check(renderer::MakeCrackGuides(patch).size() == 4, "開口ゼロでは厚みの枠を出さない");
    settings.extentU = 0;
    Check(!crack::BuildCrackPatch(settings, patch, error) && !error.empty(), "ゼロの半幅を拒否");
    settings = {};
    settings.depth = -1;
    Check(!crack::BuildCrackPatch(settings, patch, error), "負の深さを拒否");
    settings = {};
    settings.persistence = 1.01f;
    Check(!crack::BuildCrackPatch(settings, patch, error), "範囲外の Persistence を拒否");
    settings = {};
    settings.rotationDegrees[0] = std::numeric_limits<float>::quiet_NaN();
    Check(!crack::BuildCrackPatch(settings, patch, error), "非有限の回転を拒否");

    tests::Section("有限亀裂 — ノードと保存対象のスナップショット");
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(NodeKind::BaseRock), crack = graph.CreateNode(NodeKind::Crack),
               output = graph.CreateNode(NodeKind::MeshOutput);
    Check(graph.CreateLink(graph.FindNode(base)->outputs[0].id, graph.FindNode(crack)->inputs[0].id),
          "Base Rock → Crack");
    Check(graph.CreateLink(graph.FindNode(crack)->outputs[0].id, graph.FindNode(output)->inputs[0].id),
          "Crack → Mesh Output");
    const auto result = graph::EvaluateRocks(graph), original = graph::EvaluateRocks(graph, base);
    Check(result.error.empty() && result.rocks.size() == 1 && result.cracks.size() == 1,
          "母岩とパッチを評価");
    Check(result.rocks[0].mesh.positions == original.rocks[0].mesh.positions &&
              result.rocks[0].mesh.triangles == original.rocks[0].mesh.triangles,
          "P2 は母岩トポロジーを変更しない");
    Check(original.cracks.empty() && graph::EvaluateRocks(graph, crack).cracks.size() == 1,
          "上流プレビューには下流パッチを表示しない");
    const auto model = graph.CreateNode(NodeKind::Model);
    Check(!graph.CanCreateLink(graph.FindNode(model)->outputs[0].id, graph.FindNode(crack)->inputs[0].id),
          "モデル専用出力を Crack に直結しない");
    const auto next = graph.CreateNode(NodeKind::Crack);
    graph.CreateLink(graph.FindNode(crack)->outputs[0].id, graph.FindNode(next)->inputs[0].id);
    graph.CreateLink(graph.FindNode(next)->outputs[0].id, graph.FindNode(output)->inputs[0].id);
    Check(graph::EvaluateRocks(graph).cracks.size() == 2, "連続する Crack のガイドを保持");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    auto& edit = std::get<crack::CrackSettings>(graph.FindMutableNode(crack)->settings);
    edit.center = {3, 4, 5};
    edit.rotationDegrees = {20, 30, 40};
    edit.depth = 0.8f;
    edit.showGuide = false;
    graph.MarkDirty();
    Check(graph::EvaluateRocks(graph).cracks.size() == 1, "非表示は形状を保ってガイドだけ消す");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(graph::EvaluateRocks(graph).cracks.size() == 2, "Undo でガイド設定を復元");
    auto redone = history.Redo(restored);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    const auto& read = std::get<crack::CrackSettings>(graph.FindNode(crack)->settings);
    Check(read.center[0] == 3 && read.rotationDegrees[2] == 40 && near(read.depth, 0.8f) && !read.showGuide,
          "Redo で位置・回転・深さ・表示を復元");
    graph.DeleteNode(base);
    const auto invalid = graph::EvaluateRocks(graph);
    Check(invalid.rocks.empty() && invalid.cracks.empty() && !invalid.error.empty(),
          "入力削除時に古いパッチを残さず診断");
}
