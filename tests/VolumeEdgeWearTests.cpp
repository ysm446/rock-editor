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
float At(const geometry::VolumeGrid& g, uint32_t x, uint32_t y, uint32_t z) {
    return g.values[g.Index(x, y, z)];
}
// 位置 (px, py, pz) に最も近い格子点の値。
float Near(const geometry::VolumeGrid& g, float px, float py, float pz) {
    const auto index = [&](float v, float origin) { return uint32_t(std::lround((v - origin) / g.spacing)); };
    return At(g, index(px, g.origin.x), index(py, g.origin.y), index(pz, g.origin.z));
}
}  // namespace

void RunVolumeEdgeWearTests() {
    Section("Volume Edge Wear");
    std::string error;
    // 2 m の立方体。最長辺が 2 m なので、半径 0.04 は σ = 0.08 m、量 0.05 は 0.1 m。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    Check(error.empty(), "立方体のボリューム");
    geometry::MeshInfo boxInfo;
    Check(Measure(box, boxInfo), "入力は閉じた表面を持つ");

    geometry::VolumeEdgeWearSettings off;
    off.amount = 0;
    const auto same = geometry::EdgeWearVolume(box, off, error);
    Check(error.empty() && same.values == box.values && same.dimensions == box.dimensions, "量が 0 なら入力と完全に同じ結果になる");

    geometry::VolumeEdgeWearSettings wear;
    wear.radius = .04f;
    wear.amount = .05f;
    wear.noise = 0;
    const auto worn = geometry::EdgeWearVolume(box, wear, error);
    geometry::MeshInfo wornInfo, dual;
    Check(error.empty() && Measure(worn, wornInfo) && wornInfo.components == 1, "閉じた1つの塊になる");
    Check(Measure(worn, dual, geometry::VolumeMeshingMethod::DualContouring), "Dual Contouring でも閉じた表面にできる");
    Check(worn.dimensions == box.dimensions && worn.origin.x == box.origin.x && worn.spacing == box.spacing, "格子は入力のまま");
    // 頂点と稜線の近くの値は増え（外部へ寄り）、面の中央と内部は変わらない。
    Check(Near(box, .97f, .97f, .97f) < 0 && Near(worn, .97f, .97f, .97f) > Near(box, .97f, .97f, .97f) + 1e-3f, "頂点の近くが削れる");
    Check(Near(worn, .97f, .97f, 0) > Near(box, .97f, .97f, 0) + 1e-3f, "稜線の近くが削れる");
    Check(std::abs(Near(worn, .97f, 0, 0) - Near(box, .97f, 0, 0)) < 1e-4f, "面の中央は動かない");
    Check(std::abs(Near(worn, 0, 0, 0) - Near(box, 0, 0, 0)) < 1e-4f, "内部の奥は動かない");
    Check(wornInfo.volume < boxInfo.volume && wornInfo.volume > boxInfo.volume * .9, "体積はわずかに減るだけ");
    Check(std::abs(wornInfo.maximum.x - boxInfo.maximum.x) < box.spacing * 1.5f &&
              std::abs(wornInfo.minimum.y - boxInfo.minimum.y) < box.spacing * 1.5f,
          "面の中央は残り、外接箱はほぼ変わらない");
    // 稜線の削れは量を超えない。稜線（x = y = 1, z = 0）から対角に 0.1 m より内側は内部のまま。
    Check(Near(worn, 1 - .1f, 1 - .1f, 0) < 0, "削る深さは量を超えない");
    bool grown = false;
    for (size_t i = 0; i < box.values.size(); ++i) grown |= worn.values[i] < box.values[i] - 1e-5f;
    Check(!grown, "形は広がらない（値が減る格子点がない）");
    bool edgeKept = true;
    for (uint32_t y = 0; y < box.dimensions[1] && edgeKept; ++y)
        for (uint32_t x = 0; x < box.dimensions[0]; ++x)
            edgeKept &= At(worn, x, y, 0) == At(box, x, y, 0) && At(worn, x, y, box.dimensions[2] - 1) > 0;
    Check(edgeKept, "外周の1点は入力のまま（外周は空のまま）");
    Check(geometry::EdgeWearVolume(box, wear, error).values == worn.values, "同じ入力から同じ結果を得る");
    auto shallow = wear;
    shallow.amount = .02f;
    const auto shallowWorn = geometry::EdgeWearVolume(box, shallow, error);
    Check(error.empty() && Near(shallowWorn, .97f, .97f, .97f) < Near(worn, .97f, .97f, .97f), "量が小さいほど削れ方は浅い");

    // 薄い板（厚さ 0.24 m）。半径 0.03（σ = 0.06 m。厚さの半分が 2σ）なら中心の面（稜線の二等分面）は削れず、板は分かれない。
    // 稜線の強さを格子点で読むと二等分面が削れて板が割れる。最も近い表面の点で読むことの確認。
    const auto slab = geometry::MeshToVolume(geometry::MakeBox({2, .24f, 2}), {64}, error);
    auto slabWear = wear;
    slabWear.radius = .03f;
    slabWear.amount = .05f;
    const auto wornSlab = geometry::EdgeWearVolume(slab, slabWear, error);
    geometry::MeshInfo slabInfo;
    Check(error.empty() && Measure(wornSlab, slabInfo) && slabInfo.components == 1, "薄い板：閉じた1つの塊のまま");
    Check(std::abs(Near(wornSlab, 0, 0, 0) - Near(slab, 0, 0, 0)) < 1e-4f && Near(wornSlab, 0, 0, 0) < 0,
          "薄い板：板の中心は削れない");
    Check(std::abs(Near(wornSlab, .5f, 0, .5f) - Near(slab, .5f, 0, .5f)) < 1e-4f &&
              std::abs(Near(wornSlab, 0, .11f, 0) - Near(slab, 0, .11f, 0)) < 1e-4f,
          "薄い板：面の中ほどは動かない");
    Check(Near(wornSlab, .97f, .11f, 0) > Near(slab, .97f, .11f, 0) + 1e-3f, "薄い板：縁は削れる");
    // 半径が板の厚さに近い（σ = 0.1 m）と、板全体が稜線に見えて面もわずかに削れる。量の 1 割未満に収まる。
    auto wide = slabWear;
    wide.radius = .05f;
    const auto wideWorn = geometry::EdgeWearVolume(slab, wide, error);
    const float shift = Near(wideWorn, .5f, 0, .5f) - Near(slab, .5f, 0, .5f);
    Check(error.empty() && shift >= 0 && shift < .05f * 2 * .1f, "薄い板：半径が厚さに近いと面もわずかに削れるが、量の 1 割未満");

    // ばらつき。削る総量は減り、稜線に沿って削れ方が変わる。
    auto noisy = wear;
    noisy.noise = 1;
    const auto noisyWorn = geometry::EdgeWearVolume(box, noisy, error);
    geometry::MeshInfo noisyInfo;
    Check(error.empty() && Measure(noisyWorn, noisyInfo) && noisyInfo.components == 1, "ばらつき：閉じた1つの塊になる");
    Check(noisyInfo.volume > wornInfo.volume, "ばらつき：削る量が減る");
    auto reseeded = noisy;
    reseeded.seed = 7;
    Check(geometry::EdgeWearVolume(box, reseeded, error).values != noisyWorn.values, "Seed でばらつきが変わる");
    bool uneven = false;
    for (uint32_t z = 4; z + 4 < box.dimensions[2] && !uneven; ++z) {
        const uint32_t i = uint32_t(std::lround((.97f - box.origin.x) / box.spacing));
        uneven |= std::abs(At(noisyWorn, i, i, z) - At(noisyWorn, i, i, z + 1)) > 1e-3f;
    }
    Check(uneven, "ばらつき：稜線に沿って削れ方が変わる");

    // 上向きに集中。上の稜線は削れ、下の稜線は残る。
    auto top = wear;
    top.upwardFocus = 1;
    const auto topWorn = geometry::EdgeWearVolume(box, top, error);
    Check(error.empty() && Near(topWorn, .97f, .97f, 0) > Near(box, .97f, .97f, 0) + 1e-3f, "上向きの集中：上の稜線は削れる");
    Check(std::abs(Near(topWorn, .97f, -.97f, 0) - Near(box, .97f, -.97f, 0)) < 1e-4f, "上向きの集中：下の稜線は残る");
    geometry::MeshInfo topInfo;
    Check(Measure(topWorn, topInfo) && topInfo.components == 1, "上向きの集中でも閉じた1つの塊になる");

    // 不正な設定。
    const auto rejects = [&](const char* name, auto change) {
        geometry::VolumeEdgeWearSettings bad;
        change(bad);
        const auto result = geometry::EdgeWearVolume(box, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejects("小さすぎる半径を拒否する", [](auto& s) { s.radius = 0; });
    rejects("大きすぎる半径を拒否する", [](auto& s) { s.radius = .5f; });
    rejects("負の量を拒否する", [](auto& s) { s.amount = -1; });
    rejects("大きすぎる量を拒否する", [](auto& s) { s.amount = .5f; });
    rejects("範囲外のばらつきを拒否する", [](auto& s) { s.noise = 2; });
    rejects("範囲外の細かさを拒否する", [](auto& s) { s.noiseScale = 0; });
    rejects("範囲外の集中を拒否する", [](auto& s) { s.upwardFocus = -1; });
    rejects("非有限の半径を拒否する", [](auto& s) { s.radius = std::numeric_limits<float>::quiet_NaN(); });
    geometry::EdgeWearVolume({}, {}, error);
    Check(!error.empty(), "空のボリュームを拒否する");

    Section("Volume Edge Wear のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeEdgeWear), surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(graph::FindNodeDefinitionByName("volumeEdgeWear") && g.FindNode(node)->inputs.size() == 1 &&
              g.FindNode(node)->outputs.size() == 1 && std::holds_alternative<geometry::VolumeEdgeWearSettings>(g.FindNode(node)->settings),
          "ノードは Volume の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(node)->inputs[0].id), "Mesh 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    Check(link(shape, volume) && link(volume, node) && link(node, surface), "To Volume → Volume Edge Wear → Volume to Mesh を接続できる");
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) &&
              graphInfo.closed && graphInfo.volume > 4,
          "グラフの評価で閉じたメッシュを得る");
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && graph::EvaluateRocks(g, node, &cache).rocks[0].volume == first.rocks[0].volume, "変更がなければボリュームを再利用する");
    std::get<geometry::VolumeEdgeWearSettings>(g.FindMutableNode(node)->settings).amount = .05f;
    const auto changed = graph::EvaluateRocks(g, node, &cache);
    Check(changed.error.empty() && changed.rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
    std::get<geometry::VolumeEdgeWearSettings>(g.FindMutableNode(node)->settings).amount = 1;
    Check(!graph::EvaluateRocks(g, node, &cache).error.empty(), "不正な設定を診断する");
}
