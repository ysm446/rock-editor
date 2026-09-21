#include "TestSupport.h"
#include "geometry/UvUnwrap.h"
#include "renderer/RockMesh.h"
#include "graph/RockEvaluator.h"
#include "renderer/BakePadding.h"
#include "app/UndoHistory.h"
#include <algorithm>
#include <cmath>
#include <iostream>
void RunUvTests() {
    using namespace rock;
    tests::Section("自動UV展開");
    tests::Check(geometry::NormalizeUvResolution(128) == 128 &&
                     geometry::NormalizeUvResolution(512) == 512 &&
                     geometry::NormalizeUvResolution(4096) == 4096 &&
                     geometry::NormalizeUvResolution(513) == 1024 &&
                     geometry::NormalizeUvResolution(0) == 128 &&
                     geometry::NormalizeUvResolution(2147483647) == 4096,
                 "旧解像度を範囲内の2のべき乗へ切り上げる");
    auto box = geometry::MakeBox({2, 3, 4});
    std::string error;
    geometry::UnwrapMesh(box, {300, 4, 1}, error);
    tests::Check(!error.empty(), "2のべき乗でない解像度を直接評価では拒否する");
    auto uv = geometry::UnwrapMesh(box, {512, 4, 1}, error);
    tests::Check(error.empty() && geometry::HasValidUvs(uv), "Boxを0〜1のUVへ展開");
    tests::Check(uv.positions == box.positions && uv.triangles == box.triangles,
                 "展開で形状と共有トポロジーを変えない");
    tests::Check(uv.uvWidth == 512 && uv.uvHeight == 512, "指定した正方形解像度に配置");
    const auto noOverlap = [](const geometry::Mesh &mesh) {
        std::vector<int> occupied(512 * 512, -1);
        int faceId = -1;
        for (const auto &face : mesh.cornerUvs) {
            ++faceId;
            const double ax = face[0].u * 512, ay = face[0].v * 512, bx = face[1].u * 512,
                         by = face[1].v * 512, cx = face[2].u * 512, cy = face[2].v * 512;
            const double d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            const int x0 = std::max(0, int(std::floor(std::min({ax, bx, cx})))),
                      x1 = std::min(511, int(std::ceil(std::max({ax, bx, cx}))));
            const int y0 = std::max(0, int(std::floor(std::min({ay, by, cy})))),
                      y1 = std::min(511, int(std::ceil(std::max({ay, by, cy}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double u = ((x + .5 - ax) * (cy - ay) - (y + .5 - ay) * (cx - ax)) / d;
                    const double v = ((bx - ax) * (y + .5 - ay) - (by - ay) * (x + .5 - ax)) / d;
                    if (u > 1e-5 && v > 1e-5 && u + v < 1 - 1e-5) {
                        if (occupied[y * 512 + x] >= 0) {
                            const auto previous = occupied[y * 512 + x];
                            std::cout << "  overlap: " << previous << "/" << faceId << " charts "
                                      << mesh.uvCharts[previous] << "/" << mesh.uvCharts[faceId] << " at "
                                      << x << "," << y << " bary " << u << "," << v << '\n';
                            return false;
                        }
                        occupied[y * 512 + x] = faceId;
                    }
                }
        }
        return true;
    };
    tests::Check(noOverlap(uv), "UVの面内部が重ならない");
    const auto render = renderer::MakeRockMeshData(uv, true);
    bool tangent = true;
    for (const auto &v : render.vertices)
        tangent &=
            std::isfinite(v.tangent.x) &&
            std::abs(v.normal.x * v.tangent.x + v.normal.y * v.tangent.y + v.normal.z * v.tangent.z) < 1e-4f;
    tests::Check(!render.vertices.empty() && tangent, "UVに対応する直交接線を生成");
    auto again = geometry::UnwrapMesh(box, {512, 4, 1}, error);
    tests::Check(uv.cornerUvs == again.cornerUvs, "同じ入力のUV展開は再現可能");
    geometry::UnwrapMesh(box, {0, 4, 1}, error);
    tests::Check(!error.empty(), "不正解像度を拒否");
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(graph::NodeKind::BaseRock);
    const auto unwrap = graph.CreateNode(graph::NodeKind::UvUnwrap);
    graph.CreateLink(graph.FindNode(base)->outputs[0].id, graph.FindNode(unwrap)->inputs[0].id);
    graph::RockEvaluationCache cache;
    auto result = graph::EvaluateRocks(graph, unwrap, &cache);
    tests::Check(result.error.empty() && result.rocks.size() == 1 &&
                     geometry::HasValidUvs(result.rocks[0].mesh),
                 "ノード評価にUVを渡す");
    tests::Check(cache.uvs.contains(unwrap), "展開結果をキャッシュする");
    tests::Section("SDF由来の細い三角形のUV");
    geometry::BoxClusterSettings boxes;
    boxes.count = 3;
    const auto cluster = geometry::MakeBoxCluster(boxes, error);
    const auto volume = geometry::BoxesToVolume(cluster, {24}, error);
    for (const auto method :
         {geometry::VolumeMeshingMethod::MarchingTetrahedra, geometry::VolumeMeshingMethod::DualContouring}) {
        const auto mesh = geometry::VolumeSurface(volume, error, method);
        const auto unwrapped = geometry::UnwrapMesh(mesh, {512, 4, 1}, error);
        if (!error.empty())
            std::cout << "  UV error: " << error << '\n';
        tests::Check(error.empty() && geometry::HasValidUvs(unwrapped), "SDF表面の両方式を欠落なく展開");
        tests::Check(noOverlap(unwrapped), "SDF表面のUVの面内部が重ならない");
    }
    tests::Section("ベイクノードとUndo");
    const auto bake = graph.CreateNode(graph::NodeKind::MaterialBake),
               surface = graph.CreateNode(graph::NodeKind::Surface);
    graph.CreateLink(graph.FindNode(unwrap)->outputs[0].id, graph.FindNode(bake)->inputs[0].id);
    tests::Check(!graph::EvaluateRocks(graph, bake, &cache).error.empty(), "材質がないベイクを診断");
    graph.CreateLink(graph.FindNode(surface)->outputs[0].id, graph.FindNode(bake)->inputs[1].id);
    result = graph::EvaluateRocks(graph, bake, &cache);
    tests::Check(result.error.empty() && result.rocks[0].bakeSource == bake &&
                     result.rocks[0].materialSource == surface,
                 "ベイク対象のUVと材質を対応付ける");
    const auto output = graph.CreateNode(graph::NodeKind::MeshOutput);
    graph.CreateLink(graph.FindNode(bake)->outputs[0].id, graph.FindNode(output)->inputs[0].id);
    result = graph::EvaluateRocks(graph, 0, &cache);
    tests::Check(result.error.empty() && result.rocks[0].bakeSource == bake,
                 "Mesh Outputへベイク材質を引き継ぐ");
    graph.CreateLink(graph.FindNode(surface)->outputs[0].id, graph.FindNode(output)->inputs[1].id);
    result = graph::EvaluateRocks(graph, 0, &cache);
    tests::Check(result.error.empty() && result.rocks[0].bakeSource == 0,
                 "出力側で明示したSurface材質はベイク材質を上書きする");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    auto &saved = std::get<graph::MaterialBakeSettings>(graph.FindMutableNode(bake)->settings);
    saved.fingerprint = "test";
    saved.bakedLayer.material = 123;
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    const auto undone = history.Undo(after);
    graph.Replace(undone.graphNodes, undone.graphLinks);
    tests::Check(std::get<graph::MaterialBakeSettings>(graph.FindNode(bake)->settings).fingerprint.empty(),
                 "Undoでベイク前へ戻る");
    const auto redone = history.Redo(undone);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    tests::Check(std::get<graph::MaterialBakeSettings>(graph.FindNode(bake)->settings).bakedLayer.material ==
                     123,
                 "Redoでベイク材質を復元");
    tests::Section("ベイク画像の余白");
    LdrImage image;
    image.width = 7;
    image.height = 7;
    image.pixels.resize(7 * 7 * 4);
    image.pixels[(3 * 7 + 3) * 4] = 200;
    image.pixels[(3 * 7 + 3) * 4 + 3] = 255;
    renderer::DilateBakePixels(image, 2);
    tests::Check(image.pixels[(3 * 7 + 5) * 4] == 200 && image.pixels[(3 * 7 + 6) * 4 + 3] == 0 &&
                     image.pixels[(3 * 7 + 3) * 4] == 200,
                 "指定幅だけ色を伸ばし元の被覆画素を保持する");
}
