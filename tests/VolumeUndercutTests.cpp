#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <cmath>
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

void RunVolumeUndercutTests() {
    Section("Volume Undercut");
    std::string error;
    // 2 m の立方体（Y は -1〜1）。帯の中心は高さ 0.5（Y = 0）。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    geometry::MeshInfo boxInfo;
    Check(error.empty() && Measure(box, boxInfo), "立方体のボリューム");

    geometry::VolumeUndercutSettings band;
    band.height = .5f;
    band.width = .2f;
    band.depth = .1f;
    band.noise = 0;
    const auto notched = geometry::UndercutVolume(box, band, error);
    geometry::MeshInfo info, dual;
    Check(error.empty() && Measure(notched, info) && info.components == 1 && info.volume < boxInfo.volume,
          "帯を削って体積が減り、1つの塊のまま");
    Check(Measure(notched, dual, geometry::VolumeMeshingMethod::DualContouring), "Dual Contouring でも閉じた表面にできる");
    bool outsideKept = true, insideCarved = false;
    for (uint32_t z = 0; z < box.dimensions[2]; ++z)
        for (uint32_t y = 0; y < box.dimensions[1]; ++y)
            for (uint32_t x = 0; x < box.dimensions[0]; ++x) {
                const size_t i = box.Index(x, y, z);
                const float h = box.Position(x, y, z).y;
                if (std::abs(h) > .45f) outsideKept &= notched.values[i] == box.values[i];
                if (std::abs(h) < .05f) insideCarved |= notched.values[i] > box.values[i] + .15f;
            }
    Check(outsideKept, "帯の外（高さ ±0.45 m より外）は変わらない");
    Check(insideCarved, "帯の中心で深さ（0.1 × 2 m = 0.2 m）ほど削る");

    // ワールド基準: 立方体は Y が -1〜1 なので、ワールドの高さ 0 は形の高さ 0.5 と同じ帯になる。
    geometry::VolumeUndercutSettings world = band;
    world.reference = geometry::VolumeUndercutReference::World;
    world.level = 0;
    world.height = .9f;
    const auto worldNotched = geometry::UndercutVolume(box, world, error);
    bool worldOutsideKept = true, worldCenterCarved = false;
    for (uint32_t z = 0; z < box.dimensions[2]; ++z)
        for (uint32_t y = 0; y < box.dimensions[1]; ++y)
            for (uint32_t x = 0; x < box.dimensions[0]; ++x) {
                const size_t i = box.Index(x, y, z);
                const float h = box.Position(x, y, z).y;
                if (std::abs(h) > .45f) worldOutsideKept &= worldNotched.values[i] == box.values[i];
                if (std::abs(h) < .05f) worldCenterCarved |= worldNotched.values[i] > box.values[i] + .15f;
            }
    Check(error.empty() && worldOutsideKept && worldCenterCarved, "ワールドの高さ 0 の帯は Y = 0 を中心に削り、height は使わない");
    world.level = .6f;
    const auto raised = geometry::UndercutVolume(box, world, error);
    bool highCarved = false, lowKept = true;
    for (uint32_t z = 0; z < box.dimensions[2]; ++z)
        for (uint32_t y = 0; y < box.dimensions[1]; ++y)
            for (uint32_t x = 0; x < box.dimensions[0]; ++x) {
                const size_t i = box.Index(x, y, z);
                const float h = box.Position(x, y, z).y;
                if (std::abs(h - .6f) < .05f) highCarved |= raised.values[i] > box.values[i] + .15f;
                if (h < .1f) lowKept &= raised.values[i] == box.values[i];
            }
    Check(error.empty() && highCarved && lowKept, "ワールドの高さ 0.6 m に帯を置くと、その高さだけ削る");
    Check(geometry::ParseVolumeUndercutReference(geometry::VolumeUndercutReferenceName(geometry::VolumeUndercutReference::World)) ==
              geometry::VolumeUndercutReference::World && geometry::ParseVolumeUndercutReference("?") == geometry::VolumeUndercutReference::Shape,
          "帯の基準の保存名を往復できる");

    // 向きに集中: -X を向いた面の帯だけを削り、+X の面の帯は変わらない。
    {
        geometry::VolumeUndercutSettings oneSided = band;
        oneSided.upwardFocus = 1;
        oneSided.focusDirection = {-1, 0, 0};
        const auto sided = geometry::UndercutVolume(box, oneSided, error);
        float minusX = 0, plusX = 0;
        for (uint32_t z = 0; z < box.dimensions[2]; ++z)
            for (uint32_t y = 0; y < box.dimensions[1]; ++y)
                for (uint32_t x = 0; x < box.dimensions[0]; ++x) {
                    const auto p = box.Position(x, y, z);
                    if (std::abs(p.y) > .05f || std::abs(p.z) > .4f) continue;
                    const float d = sided.values[box.Index(x, y, z)] - box.values[box.Index(x, y, z)];
                    if (std::abs(p.x + .95f) < .05f) minusX = std::max(minusX, d);
                    if (std::abs(p.x - .95f) < .05f) plusX = std::max(plusX, d);
                }
        Check(error.empty() && minusX > .15f && plusX < 1e-4f, "向きに集中: 集中する向きを向いた側の帯だけ削れ、反対側は変わらない");
    }

    geometry::VolumeUndercutSettings stacked = band;
    stacked.height = .2f;
    stacked.width = .08f;
    stacked.count = 3;
    stacked.spacing = .3f;
    geometry::MeshInfo stackedInfo;
    Check(geometry::UndercutVolume(box, stacked, error).values.size() == box.values.size() && error.empty(), "帯を重ねられる");

    // 厚さ 1 m の板（最長辺 2 m）を中央で 0.8 m 削ると、半分の厚さ 0.5 m を超えて上下に切り離される。
    const auto slab = geometry::MeshToVolume(geometry::MakeBox({2, 2, 1}), {64}, error);
    geometry::VolumeUndercutSettings sever = band;
    sever.depth = .4f;
    sever.width = .1f;
    geometry::UndercutVolume(slab, sever, error);
    Check(!error.empty(), "上下に切り離すほど削ると診断する");
    geometry::VolumeUndercutSettings floating = band;
    floating.height = 0;
    floating.width = .1f;
    floating.depth = .4f;
    geometry::UndercutVolume(box, floating, error);
    Check(!error.empty(), "底を断面ごと削り切って浮くと診断する");
    geometry::VolumeUndercutSettings bad = band;
    bad.count = 0;
    geometry::UndercutVolume(box, bad, error);
    Check(!error.empty(), "帯の数が範囲外なら診断する");

    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeUndercut);
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    g.CreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(volume)->inputs[0].id);
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    g.CreateLink(g.FindNode(volume)->outputs[0].id, g.FindNode(node)->inputs[0].id);
    graph::RockEvaluationCache cache;
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && first.rocks.size() == 1 && first.rocks[0].volume, "グラフで評価できる");
    std::get<geometry::VolumeUndercutSettings>(g.FindMutableNode(node)->settings).seed = 4;
    Check(graph::EvaluateRocks(g, node, &cache).rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
}
