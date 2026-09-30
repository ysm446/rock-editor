#include "TestSupport.h"
#include "geometry/BaseRock.h"
#include "geometry/UvUnwrap.h"
#include "graph/RockEvaluator.h"

using namespace rock::tests;
using namespace rock;

void RunRockAssetTests() {
    Section("Rock Asset");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), unwrap = g.CreateNode(graph::NodeKind::UvUnwrap),
               asset = g.CreateNode(graph::NodeKind::RockAsset);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(g.FindNode(asset)->inputs.size() == 1 && g.FindNode(asset)->outputs.size() == 1 &&
              std::holds_alternative<graph::RockAssetSettings>(g.FindNode(asset)->settings) &&
              graph::IsMeshNodeKind(graph::NodeKind::RockAsset),
          "ノードは Mesh の入出力と既定の設定を持ち、プレビューできる");
    Check(!graph::EvaluateRocks(g, asset).error.empty(), "未接続なら診断する");

    // 丸めた箱（16 分割）を UV 展開して、4 段の LOD を作る。
    auto& shapeSettings = std::get<graph::BaseRockNodeSettings>(g.FindMutableNode(shape)->settings);
    shapeSettings.shape = geometry::BaseShape::RoundedBox;
    shapeSettings.subdivisions = 16;
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(unwrap)->settings).resolution = 256;
    Check(link(shape, unwrap) && link(unwrap, asset), "Base Shape → UV Unwrap → Rock Asset を接続できる");
    graph::RockEvaluationCache cache;
    const auto input = graph::EvaluateRocks(g, unwrap, &cache);
    const auto evaluated = graph::EvaluateRocks(g, asset, &cache);
    Check(input.error.empty() && evaluated.error.empty() && evaluated.rocks.size() == 1 && evaluated.rocks[0].lods &&
              evaluated.rocks[0].lods->size() == 4 && evaluated.rocks[0].source == asset,
          "既定で 4 段の LOD を作る");
    if (evaluated.rocks.empty() || !evaluated.rocks[0].lods || evaluated.rocks[0].lods->size() != 4) return;
    const auto& rock = evaluated.rocks[0];
    const auto& lods = *rock.lods;
    Check(lods[0].triangles.size() == input.rocks[0].mesh.triangles.size() &&
              rock.mesh.triangles.size() == lods[0].triangles.size(),
          "上限以下の入力なら LOD0 は入力そのまま（出力も LOD0）");
    bool shrinking = true, sharedUvs = true, ownUvs = true, closed = true, reached = true;
    for (size_t level = 0; level < lods.size(); ++level) {
        if (level > 0) shrinking &= lods[level].triangles.size() < lods[level - 1].triangles.size();
        // 既定では LOD0〜1 が LOD0 の UV を共有し、LOD2 以降は自分の UV（LOD0 のアトラスを段ごとに半分、最小 128）を持つ。
        if (level < 2)
            sharedUvs &= geometry::HasValidUvs(lods[level]) && lods[level].uvWidth == lods[0].uvWidth &&
                         lods[level].uvHeight == lods[0].uvHeight;
        else
            ownUvs &= geometry::HasValidUvs(lods[level]) && lods[level].uvWidth == 128 && lods[level].uvHeight == 128 &&
                      lods[level].cornerUvs != lods[level - 1].cornerUvs;
        const double target = std::max(64.0, double(lods[0].triangles.size()) *
                                                 graph::RockAssetSettings{}.trianglePercent[level] / 100.0);
        if (level >= 2) reached &= double(lods[level].triangles.size()) <= target * 1.05;
        geometry::MeshInfo info;
        closed &= geometry::InspectMesh(lods[level], info) && info.closed && info.components == 1;
    }
    Check(rock.sharedUvLods == 2, "既定では LOD0〜1 が LOD0 の UV を共有する");
    Check(shrinking, "段が進むごとに三角形が減る");
    Check(sharedUvs, "UV を共有する段は LOD0 と同じアトラスの有効な UV を持つ（同じテクスチャを使える）");
    Check(ownUvs, "UV を共有しない段は、自分のアトラスの有効な UV を持つ");
    Check(reached, "UV を共有しない段は目標の三角形数まで減る");
    Check(closed, "どの段も閉じた 1 つのメッシュのまま");

    // 全ての段で共有すると、全段が同じアトラスの UV を持つ（作り直す）。
    {
        const auto before = cache.computations[asset];
        auto& shareSettings = std::get<graph::RockAssetSettings>(g.FindMutableNode(asset)->settings);
        shareSettings.shareUv.fill(true);
        const auto shared = graph::EvaluateRocks(g, asset, &cache);
        bool same = shared.error.empty() && shared.rocks.size() == 1 && shared.rocks[0].lods && shared.rocks[0].sharedUvLods == 4;
        for (size_t level = 0; same && level < shared.rocks[0].lods->size(); ++level)
            same &= geometry::HasValidUvs((*shared.rocks[0].lods)[level]) &&
                    (*shared.rocks[0].lods)[level].uvWidth == lods[0].uvWidth;
        Check(same && cache.computations[asset] == before + 1, "全ての段で UV を共有すると作り直し、全段が同じアトラスを持つ");
        // 上の段が共有しないなら、下の段のチェックは効かない。
        shareSettings.shareUv = {true, false, true, true, true, true};
        Check(graph::RockAssetSharedUvLods(shareSettings) == 1, "共有しない段より下は共有しない");
        shareSettings.shareUv = graph::RockAssetSettings{}.shareUv;
        graph::EvaluateRocks(g, asset, &cache);
    }

    // 表示だけの設定（切り替えの大きさ）は作り直さない。形に効く設定は作り直す。
    const auto runs = cache.computations[asset];
    std::get<graph::RockAssetSettings>(g.FindMutableNode(asset)->settings).screenSize[1] = 0.3f;
    const auto sameShape = graph::EvaluateRocks(g, asset, &cache);
    Check(sameShape.error.empty() && cache.computations[asset] == runs, "切り替えの大きさを変えても LOD を作り直さない");
    auto& settings = std::get<graph::RockAssetSettings>(g.FindMutableNode(asset)->settings);
    settings.lodCount = 2;
    settings.maxTriangles = int(lods[0].triangles.size() / 2);
    const auto fewer = graph::EvaluateRocks(g, asset, &cache);
    Check(fewer.error.empty() && cache.computations[asset] == runs + 1 && fewer.rocks[0].lods &&
              fewer.rocks[0].lods->size() == 2 &&
              fewer.rocks[0].lods->front().triangles.size() < lods[0].triangles.size() &&
              geometry::HasValidUvs(fewer.rocks[0].lods->front()),
          "段数と LOD0 の上限を変えると作り直し、LOD0 も UV を保って減らす");

    Check(!fewer.rocks[0].bakeMesh, "ベイクしていない入力には照合用のメッシュを持たない");

    // Material Bake の結果を LOD で使えるよう、ベイク元のノードと、減らす前のメッシュを残す。
    const auto bake = g.CreateNode(graph::NodeKind::MaterialBake), surface = g.CreateNode(graph::NodeKind::Surface);
    Check(link(unwrap, bake) && g.CreateLink(g.FindNode(surface)->outputs[0].id, g.FindNode(bake)->inputs[1].id) &&
              link(bake, asset),
          "UV Unwrap → Material Bake → Rock Asset を接続できる");
    const auto withBake = graph::EvaluateRocks(g, asset, &cache);
    Check(withBake.error.empty() && withBake.rocks.size() == 1 && withBake.rocks[0].bakeSource == bake &&
              withBake.rocks[0].bakeMesh &&
              withBake.rocks[0].bakeMesh->triangles.size() == input.rocks[0].mesh.triangles.size() &&
              withBake.rocks[0].mesh.triangles.size() < withBake.rocks[0].bakeMesh->triangles.size(),
          "ベイク元を引き継ぎ、焼いたときのメッシュを照合用に残す");

    graph::NodeGraph models;
    const auto model = models.CreateNode(graph::NodeKind::Model), modelAsset = models.CreateNode(graph::NodeKind::RockAsset);
    Check(!models.CanCreateLink(models.FindNode(model)->outputs[0].id, models.FindNode(modelAsset)->inputs[0].id),
          "Model は岩アセットに繋げない");
}
