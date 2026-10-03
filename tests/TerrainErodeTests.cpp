#include "TestSupport.h"
#include "geometry/RockScatter.h"
#include "geometry/Terrain.h"
#include "geometry/TerrainErode.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <cstdio>
#include <numeric>

using namespace rock::tests;
using namespace rock;

namespace {
// 格子の最大の勾配（度）。
double MaxSlopeDegrees(const geometry::HeightGrid& g, const geometry::HeightmapSettings& t) {
    const double dx = t.width / double(g.width - 1), dz = t.depth / double(g.height - 1), range = t.maxHeight - t.minHeight;
    double worst = 0;
    for (uint32_t y = 0; y < g.height; ++y)
        for (uint32_t x = 0; x < g.width; ++x) {
            const double h = g.values[size_t(y) * g.width + x] * range;
            if (x + 1 < g.width) worst = std::max(worst, std::abs(h - g.values[size_t(y) * g.width + x + 1] * range) / dx);
            if (y + 1 < g.height) worst = std::max(worst, std::abs(h - g.values[size_t(y + 1) * g.width + x] * range) / dz);
        }
    return std::atan(worst) * 180.0 / 3.14159265358979;
}
double Sum(const geometry::HeightGrid& g) { return std::accumulate(g.values.begin(), g.values.end(), 0.0); }
}  // namespace

void RunTerrainErodeTests() {
    Section("Terrain Erode（熱侵食）");
    std::string error;
    geometry::HeightmapSettings terrain;
    terrain.width = terrain.depth = 64;
    terrain.minHeight = 0;
    terrain.maxHeight = 20;
    terrain.resolution = 32;
    // 64 m 四方、左半分が 20 m 高い崖。
    geometry::HeightGrid cliff;
    cliff.width = cliff.height = 65;
    cliff.values.resize(65 * 65);
    for (uint32_t y = 0; y < 65; ++y)
        for (uint32_t x = 0; x < 65; ++x) cliff.values[size_t(y) * 65 + x] = x < 32 ? 1.0f : 0.0f;
    geometry::TerrainErodeSettings thermal;
    thermal.rillDepth = 0;
    thermal.thermalIterations = 500;  // 20 m の崖が 35° に落ち着くには裾が 28 セル広がる必要があり、1 回に 1 セルしか進まない
    thermal.talusAngle = 35;
    const auto before = MaxSlopeDegrees(cliff, terrain);
    const auto eroded = geometry::ErodeTerrain(cliff, terrain, thermal, error);
    Check(error.empty() && eroded.width == 65 && eroded.height == 65, "崖を崩せる（格子の寸法は入力のまま）");
    const auto after = MaxSlopeDegrees(eroded, terrain);
    std::printf("    崩れる前 %.1f 度 → 後 %.1f 度\n", before, after);
    Check(before > 80 && after < 45, "崩れた後の最大の勾配は安息角の近くまで下がる");
    Check(std::abs(Sum(eroded) - Sum(cliff)) < 1e-3, "土は移すだけで量は変わらない");
    bool talus = eroded.values[32 * 65 + 36] > 0.05f && eroded.values[32 * 65 + 28] < 0.95f;
    Check(talus, "崖の下に土が溜まり、崖の上は削れる");
    auto none = thermal;
    none.thermalIterations = 0;
    Check(geometry::ErodeTerrain(cliff, terrain, none, error).values == cliff.values, "回数 0 では変えない");
    auto steep = thermal;
    steep.talusAngle = 80;
    Check(MaxSlopeDegrees(geometry::ErodeTerrain(cliff, terrain, steep, error), terrain) > after, "安息角が大きいほど崩れない");
    auto bad = thermal;
    bad.talusAngle = 5;
    Check(geometry::ErodeTerrain(cliff, terrain, bad, error).values.empty() && !error.empty(), "範囲外の設定は診断する");

    Section("Terrain Erode（流路）");
    // 奥（v 小）が高い斜面に、中央（u = 0.5）が低い谷。
    geometry::HeightGrid valley;
    valley.width = valley.height = 65;
    valley.values.resize(65 * 65);
    for (uint32_t y = 0; y < 65; ++y)
        for (uint32_t x = 0; x < 65; ++x) {
            const float u = x / 64.0f, v = y / 64.0f;
            valley.values[size_t(y) * 65 + x] = 0.2f + (1 - v) * 0.6f + std::abs(u - 0.5f) * 0.3f;
        }
    geometry::TerrainErodeSettings rills;
    rills.thermalIterations = 0;
    rills.rillDepth = 2;
    rills.rillWidth = 0;
    const auto carved = geometry::ErodeTerrain(valley, terrain, rills, error);
    Check(error.empty() && carved.values.size() == valley.values.size(), "流路を彫れる");
    const auto drop = [&](uint32_t x, uint32_t y) { return (valley.values[size_t(y) * 65 + x] - carved.values[size_t(y) * 65 + x]) * 20; };
    Check(drop(32, 60) > drop(10, 60) + 0.3 && drop(32, 60) > drop(32, 4) + 0.3, "谷の下流ほど深く彫れ、尾根と上流は浅い");
    bool neverRaises = true;
    for (size_t i = 0; i < valley.values.size(); ++i) neverRaises &= carved.values[i] <= valley.values[i] + 1e-6f;
    Check(neverRaises, "流路は彫るだけで盛らない");
    auto wide = rills;
    wide.rillWidth = 6;
    const auto smooth = geometry::ErodeTerrain(valley, terrain, wide, error);
    Check(error.empty() && (valley.values[60 * 65 + 28] - smooth.values[60 * 65 + 28]) > (valley.values[60 * 65 + 28] - carved.values[60 * 65 + 28]),
          "幅を広げると筋の脇も彫れる");

    Section("Terrain Deform");
    geometry::MaskImage mask;
    mask.width = mask.height = 64;
    mask.pixels.assign(64 * 64, 0);
    for (uint32_t y = 24; y < 40; ++y)
        for (uint32_t x = 24; x < 40; ++x) mask.pixels[size_t(y) * 64 + x] = 255;  // 中央の 16 × 16 px（16 m 四方）
    geometry::HeightGrid flat;
    flat.width = flat.height = 65;
    flat.values.assign(65 * 65, 0.5f);
    geometry::TerrainDeformSettings scoop;
    scoop.amount = -2;
    scoop.blur = 0;
    const auto scooped = geometry::DeformTerrain(flat, terrain, mask, false, scoop, error);
    Check(error.empty() && std::abs(scooped.values[32 * 65 + 32] - 0.4f) < 1e-3f && std::abs(scooped.values[5 * 65 + 5] - 0.5f) < 1e-6f,
          "マスクの白い所だけ 2 m えぐる（20 m の 0.1）");
    geometry::TerrainDeformSettings mound = scoop;
    mound.amount = 2;
    Check(std::abs(geometry::DeformTerrain(flat, terrain, mask, false, mound, error).values[32 * 65 + 32] - 0.6f) < 1e-3f, "正の量で盛る");
    const auto inverted = geometry::DeformTerrain(flat, terrain, mask, true, scoop, error);
    Check(std::abs(inverted.values[32 * 65 + 32] - 0.5f) < 1e-6f && std::abs(inverted.values[5 * 65 + 5] - 0.4f) < 1e-3f, "反転したマスクでは外側をえぐる");
    geometry::TerrainDeformSettings soft = scoop;
    soft.blur = 6;
    const auto blurred = geometry::DeformTerrain(flat, terrain, mask, false, soft, error);
    Check(blurred.values[32 * 65 + 32] > scooped.values[32 * 65 + 32] - 1e-6f && blurred.values[32 * 65 + 21] < 0.5f - 1e-4f,
          "ぼかすと縁の外も少しえぐれ、中心は浅くなる");
    geometry::TerrainDeformSettings big = scoop;
    big.amount = -30;
    Check(geometry::DeformTerrain(flat, terrain, mask, false, big, error).values.empty() && !error.empty(), "量の範囲外は診断する");

    Section("Terrain Erode / Deform のグラフ");
    graph::NodeGraph g;
    const auto height = g.CreateNode(graph::NodeKind::Heightmap), erode = g.CreateNode(graph::NodeKind::TerrainErode),
               shape = g.CreateNode(graph::NodeKind::ShapeMask), deform = g.CreateNode(graph::NodeKind::TerrainDeform),
               scatter = g.CreateNode(graph::NodeKind::RockScatter), rock = g.CreateNode(graph::NodeKind::Rock),
               output = g.CreateNode(graph::NodeKind::MeshOutput), ground = g.CreateNode(graph::NodeKind::MeshOutput);
    auto& hs = std::get<geometry::HeightmapSettings>(g.FindMutableNode(height)->settings);
    hs.resolution = 32;
    hs.textureResolution = 128;
    std::get<geometry::TerrainErodeSettings>(g.FindMutableNode(erode)->settings).thermalIterations = 5;
    std::get<graph::RockNodeSettings>(g.FindMutableNode(rock)->settings).scene = "C:/rocks/a.rockgraph";
    auto& ss = std::get<geometry::ShapeMaskSettings>(g.FindMutableNode(shape)->settings);
    ss.type = geometry::ShapeMaskType::Height;
    ss.resolution = 128;
    const auto pin = [&](graph::GraphId node, size_t index) { return g.FindNode(node)->inputs[index].id; };
    const auto out = [&](graph::GraphId node) { return g.FindNode(node)->outputs[0].id; };
    Check(g.CreateLink(out(height), pin(erode, 0)) && g.CreateLink(out(erode), pin(shape, 0)) && g.CreateLink(out(erode), pin(deform, 0)) &&
              g.CreateLink(out(shape), pin(deform, 1)) && g.CreateLink(out(deform), pin(ground, 0)) &&
              g.CreateLink(out(erode), pin(scatter, 0)) && g.CreateLink(out(rock), pin(scatter, 2)) && g.CreateLink(out(scatter), pin(output, 0)),
          "Heightmap → Erode → Deform（Mask = Shape Mask）→ Mesh Output、Erode → Rock Scatter とつなげる");
    const auto erodedResult = graph::EvaluateRocks(g, erode);
    Check(erodedResult.error.empty() && erodedResult.rocks.size() == 1 && erodedResult.rocks[0].terrain &&
              erodedResult.rocks[0].mesh.triangles.size() == 32 * 32 * 2 && erodedResult.rocks[0].source == erode,
          "Terrain Erode は格子を持つ地形のメッシュを出す");
    const auto deformed = graph::EvaluateRocks(g, ground);
    Check(deformed.error.empty() && deformed.rocks.size() == 1 && deformed.rocks[0].terrain, "Terrain Deform が Shape Mask を受けて評価できる");
    const auto scattered = graph::EvaluateRocks(g, output);
    Check(scattered.error.empty() && scattered.rockInstances.size() == 1 && !scattered.rockInstances[0].instances.empty(),
          "侵食した地形に Rock Scatter で撒ける");
    graph::NodeGraph badGraph;
    const auto box = badGraph.CreateNode(graph::NodeKind::BaseRock), erodeBad = badGraph.CreateNode(graph::NodeKind::TerrainErode);
    badGraph.CreateLink(badGraph.FindNode(box)->outputs[0].id, badGraph.FindNode(erodeBad)->inputs[0].id);
    Check(!graph::EvaluateRocks(badGraph, erodeBad).error.empty(), "格子を持たないメッシュ（Base Shape）は診断する");
}
