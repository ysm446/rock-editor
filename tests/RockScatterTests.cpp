#include "TestSupport.h"
#include "core/PathUtf8.h"
#include "geometry/RockScatter.h"
#include "geometry/Terrain.h"
#include "graph/RockEvaluator.h"

#include <cmath>

using namespace rock::tests;
using namespace rock;

void RunRockScatterTests() {
    Section("Rock Scatter（岩を撒く）");
    // 100 m 四方の平らな地形。
    geometry::HeightmapSettings terrain;
    terrain.width = terrain.depth = 100;
    terrain.resolution = 16;
    terrain.minHeight = 0;
    terrain.maxHeight = 0.001f;
    geometry::HeightGrid flat;
    flat.width = flat.height = 2;
    flat.values = {0, 0, 0, 0};
    std::string error;
    const auto surface = geometry::MakeTerrainMesh(flat, terrain, error);
    geometry::RockScatterSettings settings;
    settings.spacing = 5;
    settings.maxCount = 10000;
    const auto placed = geometry::ScatterRocks(surface, nullptr, false, {1.0f}, settings, error);
    Check(error.empty() && placed.size() > 200 && placed.size() < 600, "100 m 四方に間隔 5 m で数百個を置く");
    double closest = 1e9;
    for (size_t i = 0; i < placed.size(); ++i)
        for (size_t j = i + 1; j < placed.size(); ++j) {
            const double dx = placed[i].position.x - placed[j].position.x, dz = placed[i].position.z - placed[j].position.z;
            closest = std::min(closest, std::sqrt(dx * dx + dz * dz));
        }
    Check(closest >= 5.0 - 1e-4, "どの 2 つも間隔より離れている");
    bool inside = true, upright = true, scales = true;
    for (const auto& p : placed) {
        inside &= std::abs(p.position.x) <= 50.001f && std::abs(p.position.z) <= 50.001f;
        upright &= p.up.y > 0.999f;
        scales &= p.scale >= settings.scaleMin - 1e-5f && p.scale <= settings.scaleMax + 1e-5f;
    }
    Check(inside && upright && scales, "地形の上に、上向きで、倍率の範囲内で置く");
    const auto again = geometry::ScatterRocks(surface, nullptr, false, {1.0f}, settings, error);
    Check(again.size() == placed.size() && again[0].position.x == placed[0].position.x, "同じ Seed なら同じ配置");
    auto limited = settings;
    limited.maxCount = 20;
    Check(geometry::ScatterRocks(surface, nullptr, false, {1.0f}, limited, error).size() == 20, "上限の数で止まる");

    // マスク: 左半分（u < 0.5）が白、右半分が黒。
    geometry::MaskImage mask;
    mask.width = mask.height = 64;
    mask.pixels.resize(64 * 64);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) mask.pixels[y * 64 + x] = x < 32 ? 255 : 0;
    const auto masked = geometry::ScatterRocks(surface, &mask, false, {1.0f}, settings, error);
    size_t left = 0, right = 0;
    for (const auto& p : masked) (p.position.x < -2 ? left : p.position.x > 2 ? right : left) += 1;
    Check(error.empty() && left > 50 && right == 0, "マスクの白い所にだけ置く");
    const auto inverted = geometry::ScatterRocks(surface, &mask, true, {1.0f}, settings, error);
    size_t invertedLeft = 0;
    for (const auto& p : inverted) invertedLeft += p.position.x < -2;
    Check(invertedLeft == 0 && !inverted.empty(), "反転したマスクでは黒い所にだけ置く");

    // 重み: 0 の岩は選ばない。
    const auto weighted = geometry::ScatterRocks(surface, nullptr, false, {1.0f, 0.0f, 3.0f}, settings, error);
    size_t counts[3] = {};
    for (const auto& p : weighted) ++counts[p.rock];
    Check(counts[1] == 0 && counts[2] > counts[0] * 2, "重みの割合で岩を選ぶ（0 は選ばない）");
    Check(geometry::ScatterRocks(surface, nullptr, false, {0.0f}, settings, error).empty() && !error.empty(),
          "重みの合計が 0 なら診断する");
    auto aligned = settings;
    aligned.alignToNormal = 1;
    aligned.embed = 0.3f;
    const auto alignedRocks = geometry::ScatterRocks(surface, nullptr, false, {1.0f}, aligned, error);
    Check(!alignedRocks.empty() && alignedRocks[0].embed == 0.3f, "沈める量を岩ごとに持つ");

    Section("Rock / Rock Scatter のグラフ");
    graph::NodeGraph g;
    const auto height = g.CreateNode(graph::NodeKind::Heightmap), scatter = g.CreateNode(graph::NodeKind::RockScatter),
               rockA = g.CreateNode(graph::NodeKind::Rock), rockB = g.CreateNode(graph::NodeKind::Rock),
               output = g.CreateNode(graph::NodeKind::MeshOutput);
    std::get<geometry::HeightmapSettings>(g.FindMutableNode(height)->settings).resolution = 32;
    std::get<graph::RockNodeSettings>(g.FindMutableNode(rockA)->settings).scene = "C:/rocks/a.rockgraph";
    auto& b = std::get<graph::RockNodeSettings>(g.FindMutableNode(rockB)->settings);
    b.scene = "C:/rocks/b.rockgraph";
    b.scale = 2.0f;
    std::get<geometry::RockScatterSettings>(g.FindMutableNode(scatter)->settings).spacing = 10;
    const auto pin = [&](graph::GraphId node, size_t index) { return g.FindNode(node)->inputs[index].id; };
    Check(g.FindNode(scatter)->inputs.size() == 3 && g.FindNode(scatter)->inputs[2].valueType == graph::ValueType::Rock,
          "Rock Scatter は Terrain・Mask・Rock を受ける");
    Check(g.CreateLink(g.FindNode(height)->outputs[0].id, pin(scatter, 0)) &&
              g.CreateLink(g.FindNode(rockA)->outputs[0].id, pin(scatter, 2)),
          "地形と Rock をつなげる");
    Check(g.FindNode(scatter)->inputs.size() == 4 && g.FindNode(scatter)->inputs[3].label == "Rock 2",
          "Rock をつなぐと、次の Rock の空きが増える");
    Check(g.CreateLink(g.FindNode(rockB)->outputs[0].id, pin(scatter, 3)) &&
              g.CreateLink(g.FindNode(scatter)->outputs[0].id, pin(output, 0)),
          "2 つ目の Rock と、Mesh Output へつなげる");
    Check(!g.CanCreateLink(g.FindNode(rockA)->outputs[0].id, pin(output, 0)), "Rock は Mesh Output へは直接つなげない");
    const auto result = graph::EvaluateRocks(g, output);
    size_t total = 0;
    bool scaled = false;
    for (const auto& set : result.rockInstances) {
        total += set.instances.size();
        if (set.scene == "C:/rocks/b.rockgraph") scaled = set.scale == 2.0f;
    }
    Check(result.error.empty() && result.rockInstances.size() == 2 && total > 100 && scaled && result.rocks.empty(),
          "撒いた岩を岩グラフごとにまとめる（Mesh Output へは地形を通さない。地形は別に出す）");
    const auto selected = graph::EvaluateRocks(g, scatter);
    Check(selected.error.empty() && selected.rocks.size() == 1 && selected.rockInstances.size() == 2,
          "Rock Scatter を選んで見るときは地形も一緒に出す");
    const auto preview = graph::EvaluateRocks(g, rockA);
    Check(preview.rockInstances.size() == 1 && preview.rockInstances[0].instances.size() == 1 &&
              preview.rockReferences.size() == 1,
          "Rock を選ぶと、原点に 1 つ置いて見せる");
    std::get<graph::RockNodeSettings>(g.FindMutableNode(rockA)->settings).scene.clear();
    Check(!graph::EvaluateRocks(g, output).error.empty(), "岩グラフを選んでいない Rock は診断する");
}
