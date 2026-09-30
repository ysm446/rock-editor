#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <functional>
#include <string>

using namespace rock::tests;
using namespace rock;

namespace {
bool Measure(const geometry::VolumeGrid& grid, geometry::MeshInfo& info,
             geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra) {
    std::string error;
    const geometry::Mesh mesh = geometry::VolumeSurface(grid, error, method);
    return error.empty() && geometry::InspectMesh(mesh, info) && info.closed;
}
}  // namespace

void RunVolumeScatterTests() {
    Section("Volume Scatter");
    std::string error;
    // 2 m の立方体。最長辺 2 m なので、半径 0.05 は 0.1 m。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    geometry::MeshInfo boxInfo;
    Check(error.empty() && Measure(box, boxInfo), "立方体のボリューム");

    geometry::VolumeScatterSettings clasts;
    clasts.count = 40;
    clasts.radiusMin = .04f;
    clasts.radiusMax = .07f;
    const auto added = geometry::ScatterVolume(box, clasts, error);
    geometry::MeshInfo addedInfo, dual;
    Check(error.empty() && Measure(added, addedInfo) && addedInfo.components == 1, "和: 閉じた1つの塊になる");
    Check(Measure(added, dual, geometry::VolumeMeshingMethod::DualContouring), "和: Dual Contouring でも閉じた表面にできる");
    Check(addedInfo.volume > boxInfo.volume && added.dimensions[0] > box.dimensions[0] && added.spacing == box.spacing,
          "和: 礫が突き出して体積が増え、突き出す分だけ格子が広がる");
    Check(addedInfo.maximum.x > boxInfo.maximum.x + box.spacing, "和: 礫が元の表面より外へ出る");

    geometry::VolumeScatterSettings holes = clasts;
    holes.operation = geometry::VolumeScatterOperation::Difference;
    holes.depthMin = 0;
    holes.depthMax = 1;
    const auto removed = geometry::ScatterVolume(box, holes, error);
    geometry::MeshInfo removedInfo;
    Check(error.empty() && Measure(removed, removedInfo) && removedInfo.components == 1, "差: 閉じた1つの塊になる");
    Check(removedInfo.volume < boxInfo.volume && removed.dimensions == box.dimensions, "差: 穴で体積が減り、格子は入力のまま");
    bool neverGrows = true;
    for (size_t i = 0; i < box.values.size(); ++i) neverGrows &= removed.values[i] >= box.values[i] - 1e-5f;
    Check(neverGrows, "差: 形を広げない");

    for (const auto shape : {geometry::VolumeScatterShape::Ellipsoid, geometry::VolumeScatterShape::Box}) {
        geometry::VolumeScatterSettings s = clasts;
        s.shape = shape;
        const auto shaped = geometry::ScatterVolume(box, s, error);
        geometry::MeshInfo info;
        Check(error.empty() && Measure(shaped, info) && info.volume > boxInfo.volume,
              (std::string(geometry::VolumeScatterShapeName(shape)) + ": 礫を足した閉じた塊になる").c_str());
    }

    const auto again = geometry::ScatterVolume(box, clasts, error);
    Check(error.empty() && again.values == added.values, "同じ入力と Seed から同じ結果を得る");
    geometry::VolumeScatterSettings other = clasts;
    other.seed = 7;
    Check(geometry::ScatterVolume(box, other, error).values != added.values, "Seed を変えると形が変わる");

    // 中心を表面から大きく外へ置くと、礫は浮いて入力の塊と重ならない。浮いた形は除く。
    geometry::VolumeScatterSettings floating = clasts;
    floating.depthMin = -1;
    floating.depthMax = -1;
    floating.blend = 0;
    floating.radiusMin = floating.radiusMax = .03f;
    const auto floated = geometry::ScatterVolume(box, floating, error);
    geometry::MeshInfo floatedInfo;
    Check(error.empty() && Measure(floated, floatedInfo) && floatedInfo.components == 1, "浮いた形は除き、1つの塊のまま");

    const auto rejects = [&](const char* name, const std::function<void(geometry::VolumeScatterSettings&)>& change) {
        geometry::VolumeScatterSettings bad;
        change(bad);
        geometry::ScatterVolume(box, bad, error);
        Check(!error.empty(), name);
    };
    rejects("数が範囲外なら診断する", [](auto& s) { s.count = 0; });
    rejects("半径の最小が最大より大きければ診断する", [](auto& s) { s.radiusMin = .2f; s.radiusMax = .1f; });
    rejects("深さが範囲外なら診断する", [](auto& s) { s.depthMax = 5; });
    rejects("細長さが範囲外なら診断する", [](auto& s) { s.elongation = .5f; });
    rejects("不明な形を診断する", [](auto& s) { s.shape = static_cast<geometry::VolumeScatterShape>(9); });
    Check(geometry::ParseVolumeScatterShape(geometry::VolumeScatterShapeName(geometry::VolumeScatterShape::Box)) ==
                  geometry::VolumeScatterShape::Box &&
              geometry::ParseVolumeScatterOperation("difference") == geometry::VolumeScatterOperation::Difference &&
              geometry::ParseVolumeScatterShape("?") == geometry::VolumeScatterShape::Sphere,
          "保存名を往復できる");

    // グラフ: Base Shape → To Volume → Volume Scatter → Volume to Mesh。
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeScatter), surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(graph::FindNodeDefinitionByName("volumeScatter") &&
              std::holds_alternative<geometry::VolumeScatterSettings>(g.FindNode(node)->settings),
          "ノードは保存名と既定の設定を持つ");
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    Check(link(shape, volume) && link(volume, node) && link(node, surface), "To Volume → Volume Scatter → Volume to Mesh を接続できる");
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) &&
              graphInfo.closed,
          "グラフの評価で閉じたメッシュを得る");
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && graph::EvaluateRocks(g, node, &cache).rocks[0].volume == first.rocks[0].volume, "変更がなければボリュームを再利用する");
    std::get<geometry::VolumeScatterSettings>(g.FindMutableNode(node)->settings).seed = 9;
    Check(graph::EvaluateRocks(g, node, &cache).rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
}
