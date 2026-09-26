#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <limits>

using namespace rock::tests;
using namespace rock;

namespace {
bool Measure(const geometry::VolumeGrid& grid, geometry::MeshInfo& info,
             geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra) {
    std::string error;
    const auto mesh = geometry::VolumeSurface(grid, error, method);
    return error.empty() && geometry::InspectMesh(mesh, info) && info.closed;
}
}  // namespace

void RunVolumeClipTests() {
    Section("Volume Clip");
    std::string error;
    // 原点を中心とする 2 m の立方体。高さ 0 で切ると下半分が消え、Y は 0～1 になる。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    Check(error.empty(), "立方体のボリューム");
    geometry::MeshInfo boxInfo;
    Check(Measure(box, boxInfo), "入力は閉じた表面を持つ");

    const auto clipped = geometry::ClipVolume(box, {}, error);
    geometry::MeshInfo info, dual;
    Check(error.empty() && Measure(clipped, info) && info.components == 1, "既定の設定で閉じた1つの塊になる");
    Check(Measure(clipped, dual, geometry::VolumeMeshingMethod::DualContouring), "Dual Contouring でも閉じた表面にできる");
    Check(clipped.dimensions == box.dimensions && clipped.origin.x == box.origin.x && clipped.spacing == box.spacing,
          "格子は入力のまま");
    Check(std::abs(info.minimum.y) < box.spacing * .5f && std::abs(info.maximum.y - boxInfo.maximum.y) < 1e-4f,
          "原点から下が消え、上面は動かない");
    Check(std::abs(info.volume - boxInfo.volume * .5) < boxInfo.volume * .02, "体積はほぼ半分になる");
    Check(std::abs(info.maximum.x - boxInfo.maximum.x) < 1e-4f && std::abs(info.minimum.z - boxInfo.minimum.z) < 1e-4f,
          "横の広がりは変わらない");

    geometry::VolumeClipSettings raised;
    raised.height = .5f;
    const auto upper = geometry::ClipVolume(box, raised, error);
    geometry::MeshInfo upperInfo;
    Check(error.empty() && Measure(upper, upperInfo) && std::abs(upperInfo.minimum.y - .5f) < box.spacing * .5f,
          "高さを上げると切り口も上がる");

    geometry::VolumeClipSettings inverted;
    inverted.invert = true;
    const auto lower = geometry::ClipVolume(box, inverted, error);
    geometry::MeshInfo lowerInfo;
    Check(error.empty() && Measure(lower, lowerInfo) && std::abs(lowerInfo.maximum.y) < box.spacing * .5f &&
              std::abs(lowerInfo.minimum.y - boxInfo.minimum.y) < 1e-4f,
          "反転すると上が消える");

    geometry::VolumeClipSettings below;
    below.height = -5;
    const auto whole = geometry::ClipVolume(box, below, error);
    geometry::MeshInfo wholeInfo;
    Check(error.empty() && Measure(whole, wholeInfo) && std::abs(wholeInfo.volume - boxInfo.volume) < 1e-3,
          "形より下で切っても形は変わらない");

    // 不正な設定と、何も残らない切り方。
    geometry::VolumeClipSettings above;
    above.height = 5;
    Check(geometry::ClipVolume(box, above, error).values.empty() && !error.empty(), "内部が残らなければ診断する");
    geometry::VolumeClipSettings bad;
    bad.height = std::numeric_limits<float>::quiet_NaN();
    Check(geometry::ClipVolume(box, bad, error).values.empty() && !error.empty(), "非有限の高さを拒否する");
    geometry::ClipVolume({}, {}, error);
    Check(!error.empty(), "空のボリュームを拒否する");

    Section("Volume Clip のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeClip), surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(graph::FindNodeDefinitionByName("volumeClip") && g.FindNode(node)->inputs.size() == 1 &&
              g.FindNode(node)->outputs.size() == 1 && std::holds_alternative<geometry::VolumeClipSettings>(g.FindNode(node)->settings),
          "ノードは Volume の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(node)->inputs[0].id), "Mesh 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    Check(link(shape, volume) && link(volume, node) && link(node, surface), "To Volume → Volume Clip → Volume to Mesh を接続できる");
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) &&
              graphInfo.closed && graphInfo.minimum.y > -.05f,
          "グラフの評価で原点から下を切った閉じたメッシュを得る");
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && graph::EvaluateRocks(g, node, &cache).rocks[0].volume == first.rocks[0].volume, "変更がなければボリュームを再利用する");
    std::get<geometry::VolumeClipSettings>(g.FindMutableNode(node)->settings).invert = true;
    const auto changed = graph::EvaluateRocks(g, node, &cache);
    Check(changed.error.empty() && changed.rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
    std::get<geometry::VolumeClipSettings>(g.FindMutableNode(node)->settings) = {1000, false};
    Check(!graph::EvaluateRocks(g, node, &cache).error.empty(), "何も残らない設定を診断する");
}
