#include <cmath>
#include <limits>
#include "TestSupport.h"
#include "app/UndoHistory.h"
#include "graph/RockEvaluator.h"
#include "renderer/CrackGuide.h"

void RunJointSetTests() {
    using namespace rock;
    using graph::NodeKind;
    using tests::Check;
    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    const auto dot = [](geometry::Vec3 a, geometry::Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto equal = [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].boundary != b[i].boundary || a[i].reached != b[i].reached ||
                a[i].normal != b[i].normal || a[i].aperture != b[i].aperture)
                return false;
        return true;
    };
    tests::Section("Joint Set — 配列・方向・決定性");
    crack::JointSetSettings s;
    std::vector<crack::CrackPatch> patches, repeated;
    std::string error;
    s.spacingVariance = s.angleVariance = 0;
    s.count = 4;
    s.spacing = 0.5f;
    s.offset = 0.2f;
    Check(crack::BuildJointSet(s, patches, error) && patches.size() == 4, "偶数の本数を生成");
    for (int i = 0; i < 4; ++i) {
        Check(near(patches[i].center.z, (i - 1.5f) * 0.5f + 0.2f) &&
                  patches[i].normal == geometry::Vec3{0, 0, 1},
              "中心対称の列と法線方向オフセット");
        Check(near(patches[i].effectiveDepth, 0.96f) && renderer::MakeCrackGuides(patches[i]).size() == 5,
              "有限範囲・到達深さ・開口をガイドへ接続");
    }
    s.seed = 123;
    Check(crack::BuildJointSet(s, repeated, error) && equal(patches, repeated),
          "ばらつき0は seed に依存しない");
    s.rotationDegrees = {0, 90, 0};
    s.center = {2, 3, 4};
    Check(crack::BuildJointSet(s, patches, error), "方向を回転");
    Check(near(patches[0].center.x, 1.45f) && near(patches[0].center.z, 4) && near(patches[0].normal.x, 1),
          "列全体を中心と基底で回転・移動");
    s = {};
    s.count = 64;
    s.spacingVariance = 0.49f;
    s.angleVariance = 30;
    for (int seed : {0, 1, 42, -1, 1000000000}) {
        s.seed = seed;
        Check(crack::BuildJointSet(s, patches, error) && patches.size() == 64, "上限本数・最大ばらつき");
        Check(crack::BuildJointSet(s, repeated, error) && equal(patches, repeated),
              "seed で頂点列を厳密に再現");
        for (size_t i = 0; i < patches.size(); ++i) {
            const auto& p = patches[i];
            Check(near(dot(p.normal, p.normal), 1) && near(dot(p.normal, p.tangentU), 0) &&
                      near(dot(p.tangentU, p.tangentV), 0),
                  "傾いたパッチも正規直交基底");
            const float ideal = (static_cast<float>(i) - 31.5f) * s.spacing;
            Check(std::abs(p.center.z - ideal) <= s.spacing * s.spacingVariance + 1e-5f, "各中心の変位上限");
            if (i) Check(p.center.z > patches[i - 1].center.z, "基準法線への射影順序を保つ");
            Check(p.normal.z >= 0.7499f, "U/V 各30度以内の傾き");
        }
    }
    s.seed = 43;
    Check(crack::BuildJointSet(s, repeated, error) && !equal(patches, repeated),
          "seed 違いで位置と方向が変わる");
    s = {};
    s.count = 1;
    s.persistence = 0;
    Check(crack::BuildJointSet(s, patches, error) && patches.size() == 1 && patches[0].effectiveDepth == 0,
          "1本と進行なしを許可");
    const auto invalid = [&](crack::JointSetSettings bad) {
        Check(!crack::BuildJointSet(bad, patches, error) && patches.empty() && !error.empty(),
              "不正設定で部分的な出力を残さない");
    };
    s.count = 0;
    invalid(s);
    s.count = 65;
    invalid(s);
    s = {};
    s.spacing = 0;
    invalid(s);
    s = {};
    s.angleVariance = 31;
    invalid(s);
    s = {};
    s.spacingVariance = 0.5f;
    invalid(s);
    s = {};
    s.offset = std::numeric_limits<float>::quiet_NaN();
    invalid(s);
    s = {};
    s.rotationDegrees[0] = 361;
    invalid(s);
    s = {};
    s.extentU = 0;
    invalid(s);
    s = {};
    s.depth = -1;
    invalid(s);
    s = {};
    s.center = {0, 0, 10000};
    s.spacing = 1000;
    invalid(s);

    tests::Section("Joint Set — グラフ・合流・Undo");
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(NodeKind::BaseRock), a = graph.CreateNode(NodeKind::JointSet),
               b = graph.CreateNode(NodeKind::JointSet), output = graph.CreateNode(NodeKind::MeshOutput);
    const auto link = [&](auto from, auto to) {
        return graph.CreateLink(graph.FindNode(from)->outputs[0].id, graph.FindNode(to)->inputs[0].id);
    };
    Check(link(base, a) && link(a, b) && link(b, output), "Joint Set を直列接続");
    auto& bSettings = std::get<crack::JointSetSettings>(graph.FindMutableNode(b)->settings);
    bSettings.rotationDegrees = {0, 90, 0};
    bSettings.count = 5;
    auto result = graph::EvaluateRocks(graph);
    const auto original = graph::EvaluateRocks(graph, base);
    Check(result.error.empty() && result.cracks.size() == 8 && result.rocks.size() == 1,
          "2系統の全パッチを表示");
    Check(result.rocks[0].mesh.positions == original.rocks[0].mesh.positions &&
              result.rocks[0].mesh.triangles == original.rocks[0].mesh.triangles,
          "母岩の実形状を保持");
    Check(graph::EvaluateRocks(graph, a).cracks.size() == 3 && original.cracks.empty(),
          "途中プレビューの範囲");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    bSettings.showGuide = false;
    Check(graph::EvaluateRocks(graph).cracks.size() == 3, "系統ごとに表示を切り替える");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(graph::EvaluateRocks(graph).cracks.size() == 8, "Undo で設定を復元");
    auto redone = history.Redo(restored);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(graph::EvaluateRocks(graph).cracks.size() == 3, "Redo で非表示を復元");
    graph.Replace(before.graphNodes, before.graphLinks);
    const auto merge = graph.CreateNode(NodeKind::Merge);
    Check(link(a, merge), "共有上流の枝を合流");
    const auto* mergeNode = graph.FindNode(merge);
    Check(graph.CreateLink(graph.FindNode(b)->outputs[0].id, mergeNode->inputs.back().id),
          "別方向を含む枝を合流");
    Check(link(merge, output), "合流を出力");
    result = graph::EvaluateRocks(graph);
    Check(result.cracks.size() == 8 && result.rocks.size() == 1, "合流時はパッチ番号ごとに重複排除");
    const auto fracture = graph.CreateNode(NodeKind::Fracture);
    Check(link(b, fracture), "Fracture への接続");
    result = graph::EvaluateRocks(graph, fracture);
    Check(result.error.empty() && result.rocks.size() == 2 && result.cracks.empty(),
          "Fracture は従来の単一平面で分割し上流ガイドを消す");
    graph.DeleteNode(base);
    result = graph::EvaluateRocks(graph);
    Check(!result.error.empty() && result.cracks.empty() && result.rocks.empty(),
          "入力欠落で古い表示を残さない");
}
