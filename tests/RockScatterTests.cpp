#include "TestSupport.h"
#include "core/PathUtf8.h"
#include "geometry/BaseRock.h"
#include "geometry/RockScatter.h"
#include "geometry/Terrain.h"
#include "graph/RockEvaluator.h"
#include "io/RockAssetIo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>

using namespace rock::tests;
using namespace rock;

namespace {
std::vector<geometry::RockScatterSource> Weights(std::initializer_list<float> weights) {
    std::vector<geometry::RockScatterSource> sources;
    for (const float weight : weights) {
        geometry::RockScatterSource source;
        source.weight = weight;
        sources.push_back(source);
    }
    return sources;
}
// 焼いた岩アセットの範囲を持つ岩（底面の中心が原点、幅 × 高さ × 奥行き）。
geometry::RockScatterSource Boulder(float width, float height, float depth, float weight = 1.0f) {
    geometry::RockScatterSource source;
    source.weight = weight;
    source.hasBounds = true;
    source.minimum = {-width / 2, 0, -depth / 2};
    source.maximum = {width / 2, height, depth / 2};
    return source;
}
}  // namespace

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
    const auto placed = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), settings, error).instances;
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
    const auto again = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), settings, error).instances;
    Check(again.size() == placed.size() && again[0].position.x == placed[0].position.x, "同じ Seed なら同じ配置");
    auto limited = settings;
    limited.maxCount = 20;
    Check(geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), limited, error).instances.size() == 20, "上限の数で止まる");

    // マスク: 左半分（u < 0.5）が白、右半分が黒。
    geometry::MaskImage mask;
    mask.width = mask.height = 64;
    mask.pixels.resize(64 * 64);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) mask.pixels[y * 64 + x] = x < 32 ? 255 : 0;
    const auto masked = geometry::ScatterRocks(surface, &mask, false, Weights({1.0f}), settings, error).instances;
    size_t left = 0, right = 0;
    for (const auto& p : masked) (p.position.x < -2 ? left : p.position.x > 2 ? right : left) += 1;
    Check(error.empty() && left > 50 && right == 0, "マスクの白い所にだけ置く");
    const auto inverted = geometry::ScatterRocks(surface, &mask, true, Weights({1.0f}), settings, error).instances;
    size_t invertedLeft = 0;
    for (const auto& p : inverted) invertedLeft += p.position.x < -2;
    Check(invertedLeft == 0 && !inverted.empty(), "反転したマスクでは黒い所にだけ置く");

    // 重み: 0 の岩は選ばない。
    const auto weighted = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f, 0.0f, 3.0f}), settings, error).instances;
    size_t counts[3] = {};
    for (const auto& p : weighted) ++counts[p.rock];
    Check(counts[1] == 0 && counts[2] > counts[0] * 2, "重みの割合で岩を選ぶ（0 は選ばない）");
    Check(geometry::ScatterRocks(surface, nullptr, false, Weights({0.0f}), settings, error).instances.empty() && !error.empty(),
          "重みの合計が 0 なら診断する");
    auto aligned = settings;
    aligned.alignToNormal = 1;
    aligned.embed = 0.3f;
    const auto alignedRocks = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), aligned, error).instances;
    Check(!alignedRocks.empty() && alignedRocks[0].embed == 0.3f, "沈める量を岩ごとに持つ");

    Section("Rock Scatter: 向きをそろえる");
    {
        auto aligned = settings;
        aligned.yaw = 30;
        aligned.yawVariation = 10;
        const auto rocks = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), aligned, error).instances;
        bool within = !rocks.empty();
        for (const auto& p : rocks) {
            const float degrees = p.yaw * 180.0f / float(std::numbers::pi);
            within &= degrees >= 20.0f - 1e-3f && degrees <= 40.0f + 1e-3f;
        }
        Check(error.empty() && within, "向き ± ばらつきの範囲で回す");
        Check(rocks.size() == placed.size() && rocks[0].position.x == placed[0].position.x, "向きをそろえても配置は変わらない");
        auto bad = aligned;
        bad.yawVariation = 200;
        Check(geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), bad, error).instances.empty() && !error.empty(),
              "向きのばらつきの範囲外は診断する");
    }

    Section("Rock Scatter: 沈める量のばらつき");
    {
        auto varied = settings;
        varied.embed = 0.3f;
        varied.embedVariation = 0.5f;
        const auto rocks = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), varied, error).instances;
        float lo = 1, hi = 0;
        for (const auto& p : rocks) { lo = std::min(lo, p.embed); hi = std::max(hi, p.embed); }
        Check(error.empty() && !rocks.empty() && lo >= 0.15f - 1e-5f && hi <= 0.45f + 1e-5f && hi - lo > 0.15f,
              "沈める量が embed × (1 ± ばらつき) の範囲で岩ごとに変わる");
        Check(rocks.size() == placed.size() && rocks[0].position.x == placed[0].position.x && rocks[0].yaw == placed[0].yaw,
              "ばらつきを付けても配置と向きは変わらない");
        auto bad = varied;
        bad.embedVariation = 2;
        Check(geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), bad, error).instances.empty() && !error.empty(),
              "沈める量のばらつきの範囲外は診断する");
    }

    Section("Rock Scatter: 大きさの間隔");
    {
        // 足元の半径 4 m（幅 8 × 奥行き 0 → 半径 4）の岩を、間隔 1 m・大きさの間隔 1 で撒く。中心どうしは 8 m 以上離れる。
        auto sized = settings;
        sized.spacing = 1;
        sized.scaleMin = sized.scaleMax = 1;
        sized.sizeSpacing = 1;
        sized.settle = 0;
        const auto big = geometry::ScatterRocks(surface, nullptr, false, {Boulder(8, 3, 0)}, sized, error).instances;
        double nearest = 1e9;
        for (size_t i = 0; i < big.size(); ++i)
            for (size_t j = i + 1; j < big.size(); ++j) {
                const double dx = big[i].position.x - big[j].position.x, dz = big[i].position.z - big[j].position.z;
                nearest = std::min(nearest, std::sqrt(dx * dx + dz * dz));
            }
        Check(error.empty() && big.size() > 50 && nearest >= 8.0 - 1e-3, "足元の半径の和より近づけない（間隔 1 m でも 8 m 離れる）");
        // 大きさの違う 2 種類。小さい岩どうしは近く、大きい岩どうしは遠い。
        const auto mixed = geometry::ScatterRocks(surface, nullptr, false, {Boulder(8, 3, 0), Boulder(2, 1, 0)}, sized, error).instances;
        bool pairsOk = true;
        for (size_t i = 0; i < mixed.size() && pairsOk; ++i)
            for (size_t j = i + 1; j < mixed.size(); ++j) {
                const double dx = mixed[i].position.x - mixed[j].position.x, dz = mixed[i].position.z - mixed[j].position.z;
                const double required = (mixed[i].rock == 0 ? 4.0 : 1.0) + (mixed[j].rock == 0 ? 4.0 : 1.0);
                if (std::sqrt(dx * dx + dz * dz) < required - 1e-3) { pairsOk = false; break; }
            }
        size_t small = 0;
        for (const auto& p : mixed) small += p.rock == 1;
        Check(error.empty() && pairsOk && small > big.size(), "大きさの違う岩を混ぜても、組ごとの半径の和で判定する");
        // 未焼成（範囲なし）の岩には効かず、間隔だけで判定する。
        const auto unbaked = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), sized, error).instances;
        Check(error.empty() && unbaked.size() > big.size() * 10, "範囲の無い岩は間隔だけで判定する");
        auto off = sized;
        off.sizeSpacing = 0;
        Check(geometry::ScatterRocks(surface, nullptr, false, {Boulder(8, 3, 0)}, off, error).instances.size() > big.size() * 10,
              "大きさの間隔 0 で無効になる");
        // 足元。倍率と向きを含む 4 隅。
        geometry::RockInstance instance;
        instance.position = {10, 2, -5};
        instance.yaw = 0;
        instance.scale = 2;
        std::array<geometry::Vec3, 4> corners;
        Check(geometry::RockFootprint(instance, Boulder(4, 3, 2), corners) && std::abs(corners[0].x - 6) < 1e-4f &&
                  std::abs(corners[0].z - (-7)) < 1e-4f && std::abs(corners[2].x - 14) < 1e-4f && std::abs(corners[2].z - (-3)) < 1e-4f &&
                  std::abs(corners[0].y - 2) < 1e-4f,
              "足元の 4 隅は倍率を掛けた底の長方形（沈めていなければ地面の高さ）");
        instance.yaw = float(std::numbers::pi / 2);
        instance.embed = 0.5f;
        Check(geometry::RockFootprint(instance, Boulder(4, 3, 2), corners) && std::abs(std::abs(corners[0].x - 10) - 2) < 1e-4f &&
                  std::abs(std::abs(corners[0].z + 5) - 4) < 1e-4f && std::abs(corners[0].y - (2 - 3)) < 1e-4f,
              "90° 回すと幅と奥行きが入れ替わり、沈めた分だけ隅が下がる");
        Check(!geometry::RockFootprint(instance, Weights({1.0f})[0], corners), "範囲の無い岩には足元が無い");
    }

    Section("Rock Scatter: 浮きの補正");
    {
        // 45° の斜面（X が増えると高さが増える）。幅 4 m の岩の底は、谷側の隅が 2 m 浮く。
        geometry::HeightmapSettings slope;
        slope.width = slope.depth = 100;
        slope.resolution = 16;
        slope.minHeight = 0;
        slope.maxHeight = 100;
        geometry::HeightGrid ramp;
        ramp.width = ramp.height = 2;
        ramp.values = {0, 1, 0, 1};  // 列 0（-X）が 0、列 1（+X）が 1
        const auto hill = geometry::MakeTerrainMesh(ramp, slope, error);
        auto settled = settings;
        settled.spacing = 8;
        settled.scaleMin = settled.scaleMax = 1;
        settled.alignToNormal = 0;  // 真上に立てる
        settled.embed = 0;
        settled.settle = 1;
        settled.settleMax = 0.9f;
        settled.sizeSpacing = 0;
        const auto rocks = geometry::ScatterRocks(surface, nullptr, false, {Boulder(4, 4, 4)}, settled, error).instances;
        bool flatUntouched = true;
        for (const auto& p : rocks) flatUntouched &= p.embed == 0;
        Check(error.empty() && !rocks.empty() && flatUntouched, "平らな地形では補正しない");
        const auto onSlope = geometry::ScatterRocks(hill, nullptr, false, {Boulder(4, 4, 4)}, settled, error).instances;
        bool sunk = true, bounded = true;
        for (const auto& p : onSlope) {
            // 縁の岩は隅が地形の外に出て読めないので除く。
            if (std::abs(p.position.x) > 47 || std::abs(p.position.z) > 47) continue;
            // 底の 4 隅のうち最も浮く隅は、中心から水平に最大 2√2 m 谷側（回転による）。45° なので浮きは 2〜2.83 m、高さ 4 m の比で 0.5〜0.71。
            sunk &= p.embed >= 0.5f - 1e-3f;
            bounded &= p.embed <= 0.9f + 1e-6f;
        }
        Check(error.empty() && !onSlope.empty() && sunk && bounded, "45° の斜面では谷側の隅が浮かない深さまで沈める");
        auto capped = settled;
        capped.settleMax = 0.2f;
        const auto limitedSink = geometry::ScatterRocks(hill, nullptr, false, {Boulder(4, 4, 4)}, capped, error).instances;
        bool atCap = !limitedSink.empty();
        for (const auto& p : limitedSink)
            if (std::abs(p.position.x) <= 47 && std::abs(p.position.z) <= 47) atCap &= std::abs(p.embed - 0.2f) < 1e-5f;
        Check(atCap, "補正の上限で止まる");
        auto noSettle = settled;
        noSettle.settle = 0;
        const auto floating = geometry::ScatterRocks(hill, nullptr, false, {Boulder(4, 4, 4)}, noSettle, error).instances;
        bool none = !floating.empty();
        for (const auto& p : floating) none &= p.embed == 0;
        Check(none, "浮きの補正 0 では沈めない");
        // 足元の 4 隅で確かめる: 補正後はどの隅も地形より下（または地表）。
        bool grounded = true;
        for (const auto& p : onSlope) {
            std::array<geometry::Vec3, 4> corners;
            geometry::RockFootprint(p, Boulder(4, 4, 4), corners);
            for (const auto& c : corners) {
                const float ground = (c.x + 50) / 100 * 100;  // 斜面の高さ
                if (std::abs(c.x) <= 50 && std::abs(c.z) <= 50 && c.y > ground + 1e-3f) grounded = false;
            }
        }
        Check(grounded, "補正後は底のどの隅も地形から浮かない");
    }

    Section("Rock Scatter: 被覆マスクと目標の被覆率");
    {
        auto covering = settings;
        covering.spacing = 6;
        covering.scaleMin = covering.scaleMax = 1;
        covering.settle = 0;
        covering.sizeSpacing = 0;
        const auto result = geometry::ScatterRocks(surface, nullptr, false, {Boulder(4, 2, 4)}, covering, error);
        const double expected = double(result.instances.size()) * 16.0 / 10000.0;
        Check(error.empty() && std::abs(result.coverage - expected) < 1e-3, "被覆率は足元の面積の合計 ÷ 地形の面積");
        const auto image = geometry::RasterizeRockCoverage(surface, result.instances, {Boulder(4, 2, 4)}, 256, error);
        size_t white = 0;
        for (const auto v : image.pixels) white += v == 255;
        const double painted = double(white) / double(image.pixels.size());
        Check(error.empty() && image.width == surface.uvWidth && image.height == surface.uvHeight && painted > expected * 0.8 &&
                  painted < expected * 1.2,
              "被覆マスクは地形の UV の大きさで、足元の分だけ白い");
        // 置いた岩の中心は白、遠い所は黒。
        bool centersWhite = true;
        for (const auto& p : result.instances) {
            const float u = (p.position.x + 50) / 100, v = (p.position.z + 50) / 100;
            centersWhite &= image.Sample(u, v) > 0.99f;
        }
        Check(centersWhite, "置いた岩の中心は被覆マスクで白");
        auto target = covering;
        target.coverageTarget = 0.05f;
        const auto stopped = geometry::ScatterRocks(surface, nullptr, false, {Boulder(4, 2, 4)}, target, error);
        Check(error.empty() && stopped.coverage >= 0.05f && stopped.coverage < 0.06f && stopped.instances.size() < result.instances.size(),
              "目標の被覆率に達したら止める");
        const auto noBounds = geometry::ScatterRocks(surface, nullptr, false, Weights({1.0f}), target, error);
        Check(error.empty() && noBounds.coverage == 0 && noBounds.instances.size() > stopped.instances.size(),
              "範囲の無い岩では被覆率が測れず、目標では止まらない");
        geometry::Mesh noUv = surface;
        noUv.cornerUvs.clear();
        geometry::RasterizeRockCoverage(noUv, result.instances, {Boulder(4, 2, 4)}, 256, error);
        Check(!error.empty(), "UV の無い地形では被覆マスクを作れないと診断する");
    }

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
    Check(g.FindNode(scatter)->outputs.size() == 2 && g.FindNode(scatter)->outputs[1].valueType == graph::ValueType::Mask &&
              g.FindNode(scatter)->outputs[1].label == "Coverage",
          "Rock Scatter は Instances と Coverage（被覆マスク）を出す");
    Check(g.CreateLink(g.FindNode(height)->outputs[0].id, pin(scatter, 0)) &&
              g.CreateLink(g.FindNode(rockA)->outputs[0].id, pin(scatter, 2)),
          "地形と Rock をつなげる");
    Check(g.FindNode(scatter)->inputs.size() == 4 && g.FindNode(scatter)->inputs[3].label == "Rock 2",
          "Rock をつなぐと、次の Rock の空きが増える");
    Check(g.CreateLink(g.FindNode(rockB)->outputs[0].id, pin(scatter, 3)) &&
              g.CreateLink(g.FindNode(scatter)->outputs[0].id, pin(output, 0)),
          "2 つ目の Rock と、Mesh Output へつなげる");
    Check(!g.CanCreateLink(g.FindNode(rockA)->outputs[0].id, pin(output, 0)), "Rock は Mesh Output へは直接つなげない");
    Check(!g.CanCreateLink(g.FindNode(scatter)->outputs[1].id, pin(output, 0)), "Coverage は Mesh Output へはつなげない");
    const auto result = graph::EvaluateRocks(g, output);
    size_t total = 0;
    bool scaled = false;
    for (const auto& set : result.rockInstances) {
        total += set.instances.size();
        // Rock の倍率はインスタンスの倍率に畳まれる（組の倍率は 1）。
        if (set.scene == "C:/rocks/b.rockgraph")
            scaled = set.scale == 1.0f && !set.instances.empty() &&
                     std::all_of(set.instances.begin(), set.instances.end(), [](const auto& i) { return i.scale >= 2.0f * 0.8f - 1e-4f; });
    }
    Check(result.error.empty() && result.rockInstances.size() == 2 && total > 100 && scaled && result.rocks.empty(),
          "撒いた岩を岩グラフごとにまとめる（Mesh Output へは地形を通さない。地形は別に出す）");
    const auto selected = graph::EvaluateRocks(g, scatter);
    Check(selected.error.empty() && selected.rocks.size() == 1 && selected.rockInstances.size() == 2 && selected.rocks[0].previewMask &&
              selected.rocks[0].previewMask->width == uint32_t(std::get<geometry::HeightmapSettings>(g.FindNode(height)->settings).textureResolution),
          "Rock Scatter を選んで見るときは地形も一緒に出し、被覆マスクを載せる");
    // 2 段目: Coverage を Mask Filter（反転）で次の Rock Scatter の Mask につなぐ。
    const auto filter = g.CreateNode(graph::NodeKind::MaskFilter), second = g.CreateNode(graph::NodeKind::RockScatter),
               output2 = g.CreateNode(graph::NodeKind::MeshOutput);
    Check(g.CreateLink(g.FindNode(scatter)->outputs[1].id, pin(filter, 0)) && g.CreateLink(g.FindNode(filter)->outputs[0].id, pin(second, 1)) &&
              g.CreateLink(g.FindNode(height)->outputs[0].id, pin(second, 0)) && g.CreateLink(g.FindNode(rockA)->outputs[0].id, pin(second, 2)) &&
              g.CreateLink(g.FindNode(second)->outputs[0].id, pin(output2, 0)),
          "Coverage を Mask Filter を通して次の Rock Scatter の Mask につなげる");
    // 未焼成の岩には足元が無いので被覆は全部黒。そのまま Mask につなぐと何も置けない。
    const auto blocked = graph::EvaluateRocks(g, output2);
    Check(blocked.error.empty() && blocked.rockInstances.empty(), "未焼成の岩の被覆マスクは黒で、そのまま Mask に通すと何も置かない");
    std::get<geometry::MaskFilterSettings>(g.FindMutableNode(filter)->settings).invert = true;
    const auto chained = graph::EvaluateRocks(g, output2);
    Check(chained.error.empty() && chained.rockInstances.size() == 1 && !chained.rockInstances[0].instances.empty() && chained.rocks.empty(),
          "反転した被覆マスクを受けた Rock Scatter が評価できる（地形は通さない）");
    const auto preview = graph::EvaluateRocks(g, rockA);
    Check(preview.rockInstances.size() == 1 && preview.rockInstances[0].instances.size() == 1 &&
              preview.rockReferences.size() == 1 && !preview.rockReferences[0].hasBounds,
          "Rock を選ぶと、原点に 1 つ置いて見せる（未焼成なら範囲なし）");
    // 焼いた岩アセット（目録に範囲がある）を参照すると、範囲が読まれて被覆が付く。
    {
        namespace fs = std::filesystem;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path root = fs::path(ROCK_DATA_DIR) / "test" / ("rock-scatter-" + std::to_string(stamp));
        std::error_code fsError;
        fs::create_directories(root, fsError);
        const fs::path baked = root / "baked.rockgraph";
        std::ofstream(baked) << "{}";
        geometry::BaseRockSettings base;
        base.shape = geometry::BaseShape::Sphere;
        base.subdivisions = 4;
        std::string message;
        io::RockAssetData asset;
        asset.lods = {{geometry::MakeBaseRock(base, message), 1.0f}};
        asset.hash = io::RockAssetHash(asset.lods, "");
        asset.minimum = {-3, 0, -3};
        asset.maximum = {3, 4, 3};
        Check(io::SaveRockAsset(baked, asset, message), "テスト用の岩アセットを焼く");
        std::get<graph::RockNodeSettings>(g.FindMutableNode(rockA)->settings).scene = ToUtf8Portable(baked);
        const auto bakedPreview = graph::EvaluateRocks(g, rockA);
        Check(bakedPreview.error.empty() && bakedPreview.rockReferences.size() == 1 && bakedPreview.rockReferences[0].hasBounds &&
                  bakedPreview.rockReferences[0].maximum.y == 4.0f,
              "焼いた岩アセットの範囲を目録から読む");
        const auto covered = graph::EvaluateRocks(g, scatter);
        size_t white = 0;
        if (covered.rocks.size() == 1 && covered.rocks[0].previewMask)
            for (const auto v : covered.rocks[0].previewMask->pixels) white += v == 255;
        size_t bakedCount = 0;
        for (const auto& set : covered.rockInstances)
            if (set.scene == ToUtf8Portable(baked)) bakedCount = set.instances.size();
        Check(covered.error.empty() && bakedCount > 0 && white > bakedCount * 100, "焼いた岩の足元が被覆マスクに白く描かれる");
        fs::remove_all(root, fsError);
    }
    std::get<graph::RockNodeSettings>(g.FindMutableNode(rockA)->settings).scene.clear();
    Check(!graph::EvaluateRocks(g, output).error.empty(), "岩グラフを選んでいない Rock は診断する");
}
