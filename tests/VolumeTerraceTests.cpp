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
// +X の面の近く（x ≈ 0.98）で、高さ y の格子点の値。層の段差はここに出る。
float NearFaceAt(const geometry::VolumeGrid& g, float y) {
    const uint32_t ix = uint32_t(std::lround((.98f - g.origin.x) / g.spacing));
    const uint32_t iy = uint32_t(std::lround((y - g.origin.y) / g.spacing));
    const uint32_t iz = uint32_t(std::lround((0 - g.origin.z) / g.spacing));
    return g.values[g.Index(ix, iy, iz)];
}
}  // namespace

void RunVolumeTerraceTests() {
    Section("Volume Terrace");
    std::string error;
    // 2 m の立方体。最長辺が 2 m なので、間隔 0.25 は 0.5 m、深さ 0.05 は 0.1 m。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    Check(error.empty(), "立方体のボリューム");
    geometry::MeshInfo boxInfo;
    Check(Measure(box, boxInfo), "入力は閉じた表面を持つ");

    geometry::VolumeTerraceSettings off;
    off.depth = 0;
    const auto same = geometry::TerraceVolume(box, off, error);
    Check(error.empty() && same.values == box.values && same.dimensions == box.dimensions, "深さが 0 なら入力と完全に同じ結果になる");

    geometry::VolumeTerraceSettings flat;
    flat.step = .25f;
    flat.depth = .05f;
    flat.ratio = .5f;
    flat.softness = 0;
    flat.variation = 0;
    flat.noise = 0;
    const auto stepped = geometry::TerraceVolume(box, flat, error);
    geometry::MeshInfo info, dual;
    Check(error.empty() && Measure(stepped, info) && info.components == 1, "平らな層：閉じた1つの塊になる");
    Check(Measure(stepped, dual, geometry::VolumeMeshingMethod::DualContouring), "平らな層：Dual Contouring でも閉じた表面にできる");
    Check(stepped.dimensions == box.dimensions && stepped.origin.x == box.origin.x && stepped.spacing == box.spacing, "格子は入力のまま");
    bool neverGrows = true, bounded = true;
    for (size_t i = 0; i < box.values.size(); ++i) {
        neverGrows &= stepped.values[i] >= box.values[i];
        if (stepped.values[i] < 0 || box.values[i] >= 0) bounded &= stepped.values[i] - box.values[i] <= .1f + 1e-4f;
    }
    Check(neverGrows, "削る方向にだけ効き、形を広げない");
    Check(bounded, "削る深さは設定の深さを超えない");
    // 層の位相は中心（y = 0）が基準。y = 0〜0.25 がへこみ、0.25〜0.5 が残る（間隔 0.5 m、割合 0.5）。
    Check(NearFaceAt(stepped, .12f) > NearFaceAt(box, .12f) + .09f && NearFaceAt(stepped, .62f) > NearFaceAt(box, .62f) + .09f,
          "へこませる層では面の近くの値が深さぶん増える");
    Check(std::abs(NearFaceAt(stepped, .37f) - NearFaceAt(box, .37f)) < 1e-5f && std::abs(NearFaceAt(stepped, -.12f) - NearFaceAt(box, -.12f)) < 1e-5f,
          "残す層では面の近くの値が変わらない");
    Check(NearFaceAt(stepped, .12f) == NearFaceAt(stepped, .62f), "ばらつき 0 なら層ごとの深さは同じ");
    // 半分の面積を 0.1 m ずつ削る。完全な段なら 6 面 × 4 m² × 0.5 × 0.1 m = 1.2 m³ より少し少ない（上下の面は層に平行）。
    Check(info.volume < boxInfo.volume - .3 && info.volume > boxInfo.volume - 1.3, "削る体積は深さと割合に見合う");
    Check(geometry::TerraceVolume(box, flat, error).values == stepped.values, "同じ入力と Seed から同じ結果を得る");
    Check(std::abs(info.maximum.y - boxInfo.maximum.y) < box.spacing * 1.5f, "水平な層は上面を動かさない");

    auto varied = flat;
    varied.variation = .8f;
    const auto variedGrid = geometry::TerraceVolume(box, varied, error);
    Check(error.empty() && NearFaceAt(variedGrid, .12f) != NearFaceAt(variedGrid, .62f), "ばらつきで層ごとの深さが変わる");
    auto reseeded = varied;
    reseeded.seed = 7;
    Check(geometry::TerraceVolume(box, reseeded, error).values != variedGrid.values, "Seed を変えるとばらつきが変わる");

    auto soft = flat;
    soft.softness = .25f;
    const auto softened = geometry::TerraceVolume(box, soft, error);
    const float edge = NearFaceAt(softened, .25f + .06f);
    Check(error.empty() && edge > NearFaceAt(box, .31f) + .005f && edge < NearFaceAt(box, .31f) + .095f, "なだらかさで段の縁が斜面になる");

    auto wavy = flat;
    wavy.noise = .5f;
    const auto waved = geometry::TerraceVolume(box, wavy, error);
    geometry::MeshInfo wavyInfo;
    Check(error.empty() && Measure(waved, wavyInfo) && wavyInfo.components == 1, "ゆらぎ：閉じた1つの塊になる");
    // 同じ高さの格子点でも、x / z によって層の境がずれる。
    float lowest = std::numeric_limits<float>::max(), highest = std::numeric_limits<float>::lowest();
    const uint32_t row = uint32_t(std::lround((.25f - waved.origin.y) / waved.spacing));
    const uint32_t face = uint32_t(std::lround((.98f - waved.origin.x) / waved.spacing));
    // 面の内側（|z| < 0.9）を見る。ゆらぎの細かさは 2（1 m に山1つ）なので、面全体で見ないと境の移動が出ない。
    for (uint32_t z = waved.dimensions[2] / 8; z < waved.dimensions[2] * 7 / 8; ++z) {
        lowest = std::min(lowest, waved.values[waved.Index(face, row, z)]);
        highest = std::max(highest, waved.values[waved.Index(face, row, z)]);
    }
    Check(highest - lowest > .02f, "ゆらぎは層の境を波打たせる");

    // 向き。X 軸まわりに 90 度回すと層は Z 方向に積み重なる。上面 (y = 0.98) に段が出る。
    auto turned = flat;
    turned.rotationDegrees = {90, 0, 0};
    const auto turnedGrid = geometry::TerraceVolume(box, turned, error);
    const auto topAt = [&](const geometry::VolumeGrid& g, float z) {
        const uint32_t ix = uint32_t(std::lround((0 - g.origin.x) / g.spacing));
        const uint32_t iy = uint32_t(std::lround((.98f - g.origin.y) / g.spacing));
        const uint32_t iz = uint32_t(std::lround((z - g.origin.z) / g.spacing));
        return g.values[g.Index(ix, iy, iz)];
    };
    bool zLayered = error.empty() && (std::abs(topAt(turnedGrid, .12f) - topAt(box, .12f)) > .09f) != (std::abs(topAt(turnedGrid, .37f) - topAt(box, .37f)) > .09f);
    Check(zLayered, "向きを回すと層の重なる方向が変わる");
    Check(std::abs(NearFaceAt(turnedGrid, .12f) - NearFaceAt(box, .12f)) < 1e-5f || std::abs(NearFaceAt(turnedGrid, .37f) - NearFaceAt(box, .37f)) < 1e-5f,
          "回した層は元の方向には段を作らない");

    // 最も細かい間隔と深い設定でも小片と空洞を残さない。
    auto fine = flat;
    fine.step = .02f;
    fine.depth = .2f;
    fine.noise = 1;
    fine.variation = 1;
    geometry::MeshInfo fineInfo;
    const auto fineGrid = geometry::TerraceVolume(box, fine, error);
    Check(error.empty() && Measure(fineGrid, fineInfo) && fineInfo.components == 1, "最も細かく深い設定でも、浮いた小片と閉じた空洞を残さない");

    const auto rejects = [&](const char* name, auto change) {
        geometry::VolumeTerraceSettings bad;
        change(bad);
        const auto result = geometry::TerraceVolume(box, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejects("小さすぎる間隔を拒否する", [](auto& s) { s.step = .01f; });
    rejects("大きすぎる間隔を拒否する", [](auto& s) { s.step = 2; });
    rejects("負の深さを拒否する", [](auto& s) { s.depth = -.1f; });
    rejects("大きすぎる深さを拒否する", [](auto& s) { s.depth = .5f; });
    rejects("範囲外の割合を拒否する", [](auto& s) { s.ratio = 1; });
    rejects("範囲外のなだらかさを拒否する", [](auto& s) { s.softness = 1; });
    rejects("範囲外のばらつきを拒否する", [](auto& s) { s.variation = 2; });
    rejects("範囲外のゆらぎを拒否する", [](auto& s) { s.noise = -1; });
    rejects("範囲外のゆらぎの細かさを拒否する", [](auto& s) { s.noiseScale = 100; });
    rejects("非有限の回転を拒否する", [](auto& s) { s.rotationDegrees[1] = std::numeric_limits<float>::infinity(); });
    geometry::TerraceVolume({}, {}, error);
    Check(!error.empty(), "空のボリュームを拒否する");

    Section("Volume Terrace のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeTerrace), surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(graph::FindNodeDefinitionByName("volumeTerrace") && g.FindNode(node)->inputs.size() == 1 &&
              g.FindNode(node)->outputs.size() == 1 && std::holds_alternative<geometry::VolumeTerraceSettings>(g.FindNode(node)->settings),
          "ノードは Volume の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(node)->inputs[0].id), "Mesh 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    Check(link(shape, volume) && link(volume, node) && link(node, surface), "To Volume → Volume Terrace → Volume to Mesh を接続できる");
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) &&
              graphInfo.closed && graphInfo.volume > 4,
          "グラフの評価で閉じたメッシュを得る");
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && graph::EvaluateRocks(g, node, &cache).rocks[0].volume == first.rocks[0].volume, "変更がなければボリュームを再利用する");
    std::get<geometry::VolumeTerraceSettings>(g.FindMutableNode(node)->settings).rotationDegrees = {0, 0, 30};
    const auto changed = graph::EvaluateRocks(g, node, &cache);
    Check(changed.error.empty() && changed.rocks[0].volume != first.rocks[0].volume, "向きの変更で作り直す");
    std::get<geometry::VolumeTerraceSettings>(g.FindMutableNode(node)->settings).depth = 1;
    Check(!graph::EvaluateRocks(g, node, &cache).error.empty(), "不正な設定はノードで診断する");
}
