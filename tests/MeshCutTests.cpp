#include "TestSupport.h"
#include "crack/MeshCut.h"
#include "geometry/BaseRock.h"
#include <cmath>
#include "graph/RockEvaluator.h"
#include "app/UndoHistory.h"
#include "fracture/PlaneSplit.h"
void RunMeshCutTests() {
    using namespace rock;
    using tests::Check;
    tests::Section("曲面の全幅・有限長部分切断");
    for (auto shape : {geometry::BaseShape::Box, geometry::BaseShape::RoundedBox, geometry::BaseShape::Sphere,
                       geometry::BaseShape::Ellipsoid}) {
        geometry::BaseRockSettings base;
        base.shape = shape;
        base.size = {2, 2.5f, 1.7f};
        base.subdivisions = 8;
        std::string error;
        const auto original = geometry::MakeBaseRock(base, error);
        geometry::MeshInfo before;
        geometry::InspectMesh(original, before);
        for (float extent : {3.f, .4f})
            for (auto rotation : {std::array<float, 3>{0, 0, 0}, std::array<float, 3>{12, 25, 8}}) {
                crack::CrackSettings s;
                s.extentU = extent;
                s.extentV = 3;
                s.depth = 3;
                s.persistence = 1;
                s.aperture = .15f;
                s.rotationDegrees = rotation;
                auto cut = crack::CutMesh(original, s);
                Check(cut.error.empty(), cut.error.empty() ? "曲面の切り込みが成功" : cut.error.c_str());
                geometry::MeshInfo after;
                Check(geometry::InspectMesh(cut.mesh, after) && after.closed && after.components == 1 &&
                          after.volume > 0 && cut.removedVolume > 0,
                      "閉じた1連結体と除去体積");
                Check(std::abs(after.volume + cut.removedVolume - before.volume) < before.volume * 1e-4,
                      "切断前後の体積保存");
            }
    }
    crack::CrackSettings slot;
    slot.extentU = slot.extentV = 3;
    slot.depth = 3;
    slot.persistence = 1;
    slot.aperture = .15f;
    geometry::BaseRockSettings base;
    base.shape = geometry::BaseShape::Ellipsoid;
    base.subdivisions = 16;
    base.noiseStrength = .08f;
    base.seed = 17;
    std::string error;
    auto noisy = geometry::MakeBaseRock(base, error);
    auto cut = crack::CutMesh(noisy, slot);
    Check(cut.error.empty() && cut.removedVolume > 0, "ノイズ付き曲面・3072三角形の切断");
    const auto repeated = crack::CutMesh(noisy, slot);
    Check(cut.mesh.positions == repeated.mesh.positions && cut.mesh.triangles == repeated.mesh.triangles,
          "再評価を再現");
    if (cut.error.empty()) {
        auto split = fracture::SplitByPlane(cut.mesh, {0, 0, 0}, {0, 0, 1});
        Check(split.error.empty(), "部分切断から残存部の完全分割へ接続");
    }
    const auto reject = [&](const auto& settings) {
        const auto r = crack::CutMesh(noisy, settings);
        Check(!r.error.empty() && r.mesh.positions.empty(), "対応外で途中結果を返さない");
    };
    auto invalid = slot;
    invalid.extentU = -1;
    reject(invalid);
    invalid = slot;
    invalid.extentV = .5f;
    reject(invalid);
    invalid = slot;
    invalid.depth = 6;
    reject(invalid);
    invalid = slot;
    invalid.aperture = 3;
    reject(invalid);
    invalid = slot;
    invalid.aperture = 1e-7f;
    reject(invalid);
    invalid = slot;
    invalid.depth = -1;
    reject(invalid);
    base.subdivisions = 32;
    const auto dense = geometry::MakeBaseRock(base, error);
    Check(!crack::CutMesh(dense, slot).error.empty(), "4096三角形上限を診断");
    for (int mode = 0; mode < 3; ++mode) {
        auto empty = slot;
        if (mode == 0) empty.aperture = 0;
        if (mode == 1) empty.persistence = 0;
        if (mode == 2) empty.center = {0, 0, 10};
        const auto r = crack::CutMesh(noisy, empty);
        Check(r.error.empty() && r.mesh.positions == noisy.positions && r.mesh.triangles == noisy.triangles &&
                  r.removedVolume == 0,
              "ゼロ幅・ゼロ深さ・非交差は入力を維持");
    }
    const auto box = geometry::MakeBox({2, 2, 2});
    auto a = crack::CutMesh(box, slot);
    auto b = crack::CutBox({2, 2, 2}, slot);
    Check(a.error.empty() && b.error.empty() && std::abs(a.removedVolume - b.removedVolume) < 1e-5,
          "Box 専用方式と除去体積が一致");
    slot.extentU = .4f;
    a = crack::CutMesh(box, slot);
    b = crack::CutBox({2, 2, 2}, slot);
    Check(a.error.empty() && b.error.empty() && std::abs(a.removedVolume - b.removedVolume) < 1e-5,
          "有限長溝の除去体積が Box の解析解と一致");
    cut = crack::CutMesh(noisy, slot);
    Check(cut.error.empty() && cut.removedVolume > 0, "ノイズ付き曲面の有限長溝");
    {
        auto obliqueBase = base;
        obliqueBase.subdivisions = 16;
        obliqueBase.size = {2, 2.5f, 1.7f};
        auto obliqueSlot = slot;
        obliqueSlot.rotationDegrees = {12, 25, 8};
        const auto oblique = crack::CutMesh(geometry::MakeBaseRock(obliqueBase, error), obliqueSlot);
        Check(oblique.error.find("tip") != std::string::npos && oblique.mesh.positions.empty(),
              "既知の不安定な斜め断面は診断し、壊れた形状を出力しない（成功対応は保留）");
    }
    const auto finiteRepeated = crack::CutMesh(noisy, slot);
    Check(cut.mesh.positions == finiteRepeated.mesh.positions &&
              cut.mesh.triangles == finiteRepeated.mesh.triangles,
          "有限長溝の再評価を再現");
    for (float offset : {-.8f, 0.f, .8f, 3.f}) {
        auto finite = slot;
        finite.center[0] = offset;
        a = crack::CutMesh(box, finite);
        b = crack::CutBox({2, 2, 2}, finite);
        Check(a.error.empty() && b.error.empty() && std::abs(a.removedVolume - b.removedVolume) < 1e-5,
              "両端・片端・非交差の有限長溝が解析解と一致");
        if (offset == 3.f)
            Check(a.mesh.positions == box.positions && a.mesh.triangles == box.triangles,
                  "U 方向の非交差は入力を変更しない");
        else {
            bool endWall = false;
            for (const auto& t : a.mesh.triangles) {
                const auto p = a.mesh.positions[t[0]], q = a.mesh.positions[t[1]], r = a.mesh.positions[t[2]];
                const float x = (p.x + q.x + r.x) / 3, y = (p.y + q.y + r.y) / 3, z = (p.z + q.z + r.z) / 3;
                const auto normal = geometry::FaceNormal(a.mesh, t);
                if (std::abs(std::abs(x - offset) - finite.extentU) < 1e-5 && y > 0 &&
                    std::abs(z) < finite.aperture * .5f) {
                    endWall = true;
                    Check(normal.x * (x - offset) < 0, "長さの終端壁は溝の内側を向く");
                }
            }
            Check(endWall, "有限長の終端壁が存在する");
        }
    }

    tests::Section("Mesh 有限溝 — グラフと Undo");
    graph::NodeGraph graph;
    const auto mother = graph.CreateNode(graph::NodeKind::BaseRock),
               crack = graph.CreateNode(graph::NodeKind::Crack),
               output = graph.CreateNode(graph::NodeKind::MeshOutput);
    const auto link = [&](auto from, auto to) {
        return graph.CreateLink(graph.FindNode(from)->outputs[0].id, graph.FindNode(to)->inputs[0].id);
    };
    Check(link(mother, crack) && link(crack, output), "母岩→Crack→出力");
    base.subdivisions = 8;
    graph.FindMutableNode(mother)->settings = base;
    slot.applyCut = true;
    slot.meshCut = true;
    graph.FindMutableNode(crack)->settings = slot;
    auto evaluated = graph::EvaluateRocks(graph);
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && evaluated.cuts.size() == 1 &&
              evaluated.cuts[0].removedVolume > 0,
          "曲面の部分切断を描画評価へ反映");
    Check(evaluated.cuts.size() == 1 && !evaluated.cuts[0].bridge, "未実装の Bridge 計測値を表示しない");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    std::get<crack::CrackSettings>(graph.FindMutableNode(crack)->settings).applyCut = false;
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(graph::EvaluateRocks(graph).cuts.size() == 1, "Undo で Mesh 部分切断を復元");
    auto redone = history.Redo(restored);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(graph::EvaluateRocks(graph).cuts.empty(), "Redo でガイドのみへ復元");
    graph.Replace(before.graphNodes, before.graphLinks);
    const auto second = graph.CreateNode(graph::NodeKind::Crack);
    graph.FindMutableNode(second)->settings = slot;
    Check(link(crack, second) && link(second, output), "2回目の部分切断を接続");
    Check(!graph::EvaluateRocks(graph).error.empty(), "交差する複数亀裂は明示診断");
}
