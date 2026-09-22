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
// 格子点 (x, y, z) の値。
float At(const geometry::VolumeGrid& g, uint32_t x, uint32_t y, uint32_t z) {
    return g.values[g.Index(x, y, z)];
}
}  // namespace

void RunVolumeSmoothTests() {
    Section("Volume Smooth");
    std::string error;
    // 2 m の立方体。最長辺が 2 m なので、半径 0.05 は σ = 0.1 m。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    Check(error.empty(), "立方体のボリューム");
    geometry::MeshInfo boxInfo;
    Check(Measure(box, boxInfo), "入力は閉じた表面を持つ");

    geometry::VolumeSmoothSettings off;
    off.amount = 0;
    const auto same = geometry::SmoothVolume(box, off, error);
    Check(error.empty() && same.values == box.values && same.dimensions == box.dimensions, "量が 0 なら入力と完全に同じ結果になる");

    geometry::VolumeSmoothSettings smooth;
    smooth.radius = .05f;
    const auto rounded = geometry::SmoothVolume(box, smooth, error);
    geometry::MeshInfo roundedInfo, dual;
    Check(error.empty() && Measure(rounded, roundedInfo) && roundedInfo.components == 1, "なめらか：閉じた1つの塊になる");
    Check(Measure(rounded, dual, geometry::VolumeMeshingMethod::DualContouring), "なめらか：Dual Contouring でも閉じた表面にできる");
    Check(rounded.dimensions == box.dimensions && rounded.origin.x == box.origin.x && rounded.spacing == box.spacing,
          "格子は入力のまま");
    // 立方体の角は削れて内側へ入り、面の中央はほぼ動かない。外接箱の大きさは変わらない（面の中央が残る）。
    const auto corner = [&](const geometry::VolumeGrid& g) {
        // (0.95, 0.95, 0.95) に最も近い格子点。入力では内部（負）。
        const uint32_t i = uint32_t(std::lround((.95f - g.origin.x) / g.spacing));
        return At(g, i, i, i);
    };
    Check(At(box, uint32_t(std::lround((.95f - box.origin.x) / box.spacing)),
             uint32_t(std::lround((.95f - box.origin.y) / box.spacing)), uint32_t(std::lround((.95f - box.origin.z) / box.spacing))) < 0 &&
              corner(rounded) > corner(box),
          "なめらか：凸な角の近くの値は増え（外部へ寄り）、角が丸くなる");
    Check(roundedInfo.volume < boxInfo.volume && roundedInfo.volume > boxInfo.volume * .85, "なめらか：体積はわずかに減るだけ");
    Check(std::abs(roundedInfo.maximum.x - boxInfo.maximum.x) < box.spacing * 1.5f &&
              std::abs(roundedInfo.minimum.y - boxInfo.minimum.y) < box.spacing * 1.5f,
          "なめらか：面の中央は残り、外接箱はほぼ変わらない");
    bool edgeKept = true;
    for (uint32_t y = 0; y < box.dimensions[1] && edgeKept; ++y)
        for (uint32_t x = 0; x < box.dimensions[0]; ++x)
            edgeKept &= At(rounded, x, y, 0) == At(box, x, y, 0) && At(rounded, x, y, box.dimensions[2] - 1) > 0;
    Check(edgeKept, "外周の1点は入力のまま（外周は空のまま）");
    Check(geometry::SmoothVolume(box, smooth, error).values == rounded.values, "同じ入力から同じ結果を得る");
    auto half = smooth;
    half.amount = .5f;
    const auto halfRounded = geometry::SmoothVolume(box, half, error);
    Check(error.empty() && corner(halfRounded) > corner(box) && corner(halfRounded) < corner(rounded), "量は入力とぼかしの混合比になる");
    auto wide = smooth;
    wide.radius = .1f;
    Check(corner(geometry::SmoothVolume(box, wide, error)) > corner(rounded), "半径が大きいほど角が深く削れる");

    // 上向きの面に集中。上の角は丸くなり、下の角は残る。
    auto top = smooth;
    top.upwardFocus = 1;
    const auto topRounded = geometry::SmoothVolume(box, top, error);
    const uint32_t hi = uint32_t(std::lround((.95f - box.origin.x) / box.spacing)), lo = uint32_t(std::lround((-.95f - box.origin.x) / box.spacing));
    Check(error.empty() && At(topRounded, hi, hi, hi) > At(box, hi, hi, hi) + 1e-4f, "上向きの集中：上の角は丸くなる");
    Check(std::abs(At(topRounded, hi, lo, hi) - At(box, hi, lo, hi)) < 1e-4f, "上向きの集中：下の角は残る");
    Check(At(topRounded, hi, hi, hi) < At(rounded, hi, hi, hi) + 1e-4f, "上向きの集中：上の角の削れは全面のときと同じかそれ以下");
    geometry::MeshInfo topInfo;
    Check(Measure(topRounded, topInfo) && topInfo.components == 1, "上向きの集中でも閉じた1つの塊になる");

    // シャープ。なめらかにした形へ掛けると角が戻る方向に動く。
    geometry::VolumeSmoothSettings sharpen;
    sharpen.mode = geometry::VolumeSmoothMode::Sharpen;
    sharpen.radius = .05f;
    sharpen.amount = .5f;
    const auto sharpened = geometry::SmoothVolume(rounded, sharpen, error);
    geometry::MeshInfo sharpInfo;
    Check(error.empty() && Measure(sharpened, sharpInfo) && sharpInfo.components == 1, "シャープ：閉じた1つの塊になる");
    Check(corner(sharpened) < corner(rounded), "シャープ：丸めた角の値が減り、角が立つ");
    Check(sharpInfo.volume > roundedInfo.volume - 1e-3, "シャープ：体積は減らない");
    Check(geometry::ParseVolumeSmoothMode(geometry::VolumeSmoothModeName(geometry::VolumeSmoothMode::Sharpen)) ==
                  geometry::VolumeSmoothMode::Sharpen &&
              geometry::ParseVolumeSmoothMode("?") == geometry::VolumeSmoothMode::Smooth,
          "モードの保存名を往復でき、不明な名前はなめらかとして読む");

    // 不正な設定。
    const auto rejects = [&](const char* name, auto change) {
        geometry::VolumeSmoothSettings bad;
        change(bad);
        const auto result = geometry::SmoothVolume(box, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejects("小さすぎる半径を拒否する", [](auto& s) { s.radius = 0; });
    rejects("大きすぎる半径を拒否する", [](auto& s) { s.radius = .5f; });
    rejects("負の量を拒否する", [](auto& s) { s.amount = -1; });
    rejects("範囲外の集中を拒否する", [](auto& s) { s.upwardFocus = 2; });
    rejects("非有限の半径を拒否する", [](auto& s) { s.radius = std::numeric_limits<float>::quiet_NaN(); });
    rejects("不明なモードを拒否する", [](auto& s) { s.mode = static_cast<geometry::VolumeSmoothMode>(9); });
    geometry::SmoothVolume({}, {}, error);
    Check(!error.empty(), "空のボリュームを拒否する");

    Section("Volume Smooth のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeSmooth), surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(graph::FindNodeDefinitionByName("volumeSmooth") && g.FindNode(node)->inputs.size() == 1 &&
              g.FindNode(node)->outputs.size() == 1 && std::holds_alternative<geometry::VolumeSmoothSettings>(g.FindNode(node)->settings),
          "ノードは Volume の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(node)->inputs[0].id), "Mesh 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    Check(link(shape, volume) && link(volume, node) && link(node, surface), "To Volume → Volume Smooth → Volume to Mesh を接続できる");
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) &&
              graphInfo.closed && graphInfo.volume > 4,
          "グラフの評価で閉じたメッシュを得る");
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && graph::EvaluateRocks(g, node, &cache).rocks[0].volume == first.rocks[0].volume, "変更がなければボリュームを再利用する");
    std::get<geometry::VolumeSmoothSettings>(g.FindMutableNode(node)->settings).mode = geometry::VolumeSmoothMode::Sharpen;
    const auto changed = graph::EvaluateRocks(g, node, &cache);
    Check(changed.error.empty() && changed.rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
    std::get<geometry::VolumeSmoothSettings>(g.FindMutableNode(node)->settings).radius = 5;
    Check(!graph::EvaluateRocks(g, node, &cache).error.empty(), "不正な設定はノードで診断する");
}
