#include "TestSupport.h"
#include "geometry/BaseRock.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using namespace rock::tests;
using namespace rock;

namespace {
bool Measure(const geometry::VolumeGrid& grid, geometry::MeshInfo& info,
             geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra) {
    std::string error;
    const auto mesh = geometry::VolumeSurface(grid, error, method);
    return error.empty() && geometry::InspectMesh(mesh, info) && info.closed;
}
float Dot(geometry::Vec3 a, geometry::Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// 右手系 Z → X → Y（Volume Transform と同じ規約）で軸を回す。
geometry::Vec3 Rotate(geometry::Vec3 p, const std::array<float, 3>& degrees) {
    const float x = degrees[0] * std::numbers::pi_v<float> / 180, y = degrees[1] * std::numbers::pi_v<float> / 180,
                z = degrees[2] * std::numbers::pi_v<float> / 180;
    const geometry::Vec3 rz{std::cos(z) * p.x - std::sin(z) * p.y, std::sin(z) * p.x + std::cos(z) * p.y, p.z};
    const geometry::Vec3 rx{rz.x, std::cos(x) * rz.y - std::sin(x) * rz.z, std::sin(x) * rz.y + std::cos(x) * rz.z};
    return {std::cos(y) * rx.x + std::sin(y) * rx.z, rx.y, -std::sin(y) * rx.x + std::cos(y) * rx.z};
}
}  // namespace

void RunPlaneCutsTests() {
    Section("Plane Cuts");
    std::string error;
    // 半径 1 m の球。どの向きでも幅は 2 m なので、深さの比から切り落とす体積を解析的に求められる。
    geometry::BaseRockSettings sphereSettings;
    sphereSettings.shape = geometry::BaseShape::Sphere;
    sphereSettings.size = {2, 2, 2};
    sphereSettings.subdivisions = 32;
    const auto sphere = geometry::MeshToVolume(geometry::MakeBaseRock(sphereSettings, error), {48}, error);
    Check(error.empty(), "球のボリューム");
    geometry::MeshInfo sphereInfo;
    Check(Measure(sphere, sphereInfo), "入力は閉じた表面を持つ");

    geometry::PlaneCutsSettings one;
    one.count = 1;
    one.depthMin = one.depthMax = .25f;
    const auto planes = geometry::MakeCutPlanes(sphere, one, error);
    Check(error.empty() && planes.size() == 1 &&
              std::abs(Dot(planes[0].normal, planes[0].normal) - 1) < 1e-5f &&
              std::abs(planes[0].offset - .5f) < sphere.spacing * 1.5f,
          "平面は単位法線を持ち、幅 2 m の 0.25 だけ内側（中心から 0.5 m）に置かれる");
    const auto capped = geometry::CutVolume(sphere, one, error);
    geometry::MeshInfo cappedInfo;
    // 球冠（高さ h = 0.5）の体積は π h² (3 − h) / 3。
    const double cap = std::numbers::pi * .25 * 2.5 / 3;
    Check(error.empty() && Measure(capped, cappedInfo) && cappedInfo.components == 1 &&
              std::abs((sphereInfo.volume - cappedInfo.volume) - cap) < .08,
          "1枚の平面は球冠の体積だけを切り落とす");
    Check(capped.dimensions == sphere.dimensions && capped.spacing == sphere.spacing &&
              capped.origin.x == sphere.origin.x && capped.values.size() == sphere.values.size(),
          "格子は入力のまま変わらない");
    bool neverGrows = true, keepsFarSide = true;
    for (size_t i = 0; i < sphere.values.size(); ++i) neverGrows &= capped.values[i] >= sphere.values[i];
    for (uint32_t z = 0; z < sphere.dimensions[2]; ++z)
        for (uint32_t y = 0; y < sphere.dimensions[1]; ++y)
            for (uint32_t x = 0; x < sphere.dimensions[0]; ++x) {
                // 平面から十分に内側で、表面に近い点は入力と同じ値のまま。
                const auto p = sphere.Position(x, y, z);
                const float value = sphere.values[sphere.Index(x, y, z)];
                if (Dot(planes[0].normal, p) - planes[0].offset < value - 1e-4f)
                    keepsFarSide &= capped.values[capped.Index(x, y, z)] == value;
            }
    Check(neverGrows, "切り落としは形を広げない（どの格子点の距離も減らない）");
    Check(keepsFarSide, "平面の影響がない格子点は入力の値と完全に一致する");

    // 深さ 0 は何も切らない。
    geometry::PlaneCutsSettings nothing;
    nothing.depthMin = nothing.depthMax = 0;
    const auto untouched = geometry::CutVolume(sphere, nothing, error);
    geometry::MeshInfo untouchedInfo;
    Check(error.empty() && Measure(untouched, untouchedInfo) &&
              std::abs(untouchedInfo.volume - sphereInfo.volume) < sphereInfo.volume * .03,
          "深さ 0 では形がほぼ変わらない");

    // 再現性と Seed。
    geometry::PlaneCutsSettings many;
    many.count = 24;
    const auto first = geometry::CutVolume(sphere, many, error);
    const auto again = geometry::CutVolume(sphere, many, error);
    Check(error.empty() && first.values == again.values, "同じ入力と Seed から同じ結果を得る");
    auto reseeded = many;
    reseeded.seed = 2;
    Check(geometry::CutVolume(sphere, reseeded, error).values != first.values, "Seed を変えると形が変わる");
    geometry::MeshInfo manyInfo, manyDual;
    Check(Measure(first, manyInfo) && manyInfo.components == 1 && manyInfo.volume < sphereInfo.volume * .95 &&
              manyInfo.volume > sphereInfo.volume * .3,
          "24枚の切り落としは1つの閉じた塊を残す");
    Check(Measure(first, manyDual, geometry::VolumeMeshingMethod::DualContouring) &&
              std::abs(manyDual.volume - manyInfo.volume) < manyInfo.volume * .05,
          "Dual Contouring でも閉じた表面にできる");
    // 枚数を増やしても先頭の平面は変わらない（後から足した平面だけが加わる）。
    auto more = many;
    more.count = 25;
    const auto planes24 = geometry::MakeCutPlanes(sphere, many, error);
    const auto planes25 = geometry::MakeCutPlanes(sphere, more, error);
    bool prefix = planes25.size() == 25;
    for (size_t i = 0; prefix && i < planes24.size(); ++i)
        prefix = planes24[i].offset == planes25[i].offset && planes24[i].normal.x == planes25[i].normal.x &&
                 planes24[i].normal.y == planes25[i].normal.y && planes24[i].normal.z == planes25[i].normal.z;
    Check(prefix, "枚数を増やしても既存の平面は変わらない");

    // 等方。法線が偏らず、深さが範囲に収まる。
    geometry::PlaneCutsSettings spreadAll;
    spreadAll.count = geometry::MaxPlaneCuts;
    spreadAll.depthMin = .1f;
    spreadAll.depthMax = .2f;
    const auto isotropic = geometry::MakeCutPlanes(sphere, spreadAll, error);
    geometry::Vec3 mean{};
    bool depthInRange = true;
    for (const auto& plane : isotropic) {
        mean = {mean.x + plane.normal.x, mean.y + plane.normal.y, mean.z + plane.normal.z};
        // 半径 1 m の球なので、中心からの距離は 1 − 2 × 深さ。
        depthInRange &= plane.offset > .6f - sphere.spacing * 1.5f && plane.offset < .8f + sphere.spacing * 1.5f;
    }
    Check(error.empty() && isotropic.size() == size_t(geometry::MaxPlaneCuts) &&
              std::sqrt(Dot(mean, mean)) / isotropic.size() < .15f,
          "等方の法線は特定の向きへ偏らない");
    Check(depthInRange, "切り込みの深さは最小と最大の間に収まる");
    const auto maximum = geometry::CutVolume(sphere, spreadAll, error);
    geometry::MeshInfo maximumInfo;
    Check(error.empty() && Measure(maximum, maximumInfo) && maximumInfo.components == 1,
          "最大枚数でも閉じた1つの塊になる");

    // 主方向。法線は回した座標系の軸の円錐内に収まり、系統数より多い軸は使わない。
    geometry::PlaneCutsSettings directional;
    directional.count = 96;
    directional.distribution = geometry::PlaneCutsDistribution::Directional;
    directional.systems = 2;
    directional.rotationDegrees = {10, 35, -20};
    directional.spreadDegrees = 8;
    const auto jointed = geometry::MakeCutPlanes(sphere, directional, error);
    const geometry::Vec3 axisX = Rotate({1, 0, 0}, directional.rotationDegrees),
                         axisY = Rotate({0, 1, 0}, directional.rotationDegrees);
    const float cone = std::cos((directional.spreadDegrees + .01f) * std::numbers::pi_v<float> / 180);
    bool inCone = true;
    int alongX = 0, alongY = 0, positive = 0;
    for (const auto& plane : jointed) {
        const float x = Dot(plane.normal, axisX), y = Dot(plane.normal, axisY);
        inCone &= std::max(std::abs(x), std::abs(y)) >= cone;
        (std::abs(x) > std::abs(y) ? alongX : alongY)++;
        positive += std::max(std::abs(x), std::abs(y)) == std::max(x, y) ? 1 : 0;
    }
    Check(error.empty() && inCone, "主方向の法線は軸からのばらつきの円錐に収まる");
    Check(alongX > 20 && alongY > 20 && positive > 20 && positive < 76, "2系統と表裏の両方を使う");
    directional.spreadDegrees = 0;
    directional.systems = 1;
    const auto slabPlanes = geometry::MakeCutPlanes(sphere, directional, error);
    bool parallel = true;
    for (const auto& plane : slabPlanes) parallel &= std::abs(std::abs(Dot(plane.normal, axisX)) - 1) < 1e-4f;
    Check(parallel, "1系統・ばらつき 0 では平面がすべて平行になる");
    const auto slab = geometry::CutVolume(sphere, directional, error);
    geometry::MeshInfo slabInfo;
    Check(error.empty() && Measure(slab, slabInfo) && slabInfo.components == 1, "平行な平面で板状に切り落とせる");

    // なめらかさ。稜線が丸く削れ、体積が減る。
    auto rounded = many;
    rounded.blend = .15f;
    const auto roundedVolume = geometry::CutVolume(sphere, rounded, error);
    geometry::MeshInfo roundedInfo;
    Check(error.empty() && Measure(roundedVolume, roundedInfo) && roundedInfo.volume < manyInfo.volume - .005,
          "なめらかさは稜線を丸く削る");

    // 不正な設定と入力。
    const auto rejects = [&](const char* name, auto change) {
        geometry::PlaneCutsSettings bad;
        change(bad);
        const auto result = geometry::CutVolume(sphere, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejects("枚数 0 を拒否する", [](auto& s) { s.count = 0; });
    rejects("上限を超える枚数を拒否する", [](auto& s) { s.count = geometry::MaxPlaneCuts + 1; });
    rejects("深すぎる切り込みを拒否する", [](auto& s) { s.depthMax = .5f; });
    rejects("最小が最大を超える深さを拒否する", [](auto& s) { s.depthMin = .3f; s.depthMax = .1f; });
    rejects("負の深さを拒否する", [](auto& s) { s.depthMin = -.1f; });
    rejects("系統数 4 を拒否する", [](auto& s) { s.systems = 4; });
    rejects("範囲外のばらつきを拒否する", [](auto& s) { s.spreadDegrees = 120; });
    rejects("非有限の向きを拒否する", [](auto& s) { s.rotationDegrees[1] = std::numeric_limits<float>::infinity(); });
    rejects("負のなめらかさを拒否する", [](auto& s) { s.blend = -1; });
    rejects("不明な分布を拒否する", [](auto& s) { s.distribution = static_cast<geometry::PlaneCutsDistribution>(9); });
    geometry::CutVolume({}, {}, error);
    Check(!error.empty(), "空のボリュームを拒否する");
    Check(geometry::ParsePlaneCutsDistribution(geometry::PlaneCutsDistributionName(
              geometry::PlaneCutsDistribution::Directional)) == geometry::PlaneCutsDistribution::Directional &&
              geometry::ParsePlaneCutsDistribution("?") == geometry::PlaneCutsDistribution::Isotropic,
          "分布の保存名を往復でき、不明な名前は等方として読む");

    // 薄い板。幅に対する比なので、薄い向きでも貫通せずに内部が残る。
    const auto plate = geometry::MeshToVolume(geometry::MakeBox({3, .4f, 2}), {64}, error);
    geometry::PlaneCutsSettings deep;
    deep.count = 40;
    deep.depthMin = deep.depthMax = geometry::MaxPlaneCutDepth;
    const auto plateCut = geometry::CutVolume(plate, deep, error);
    geometry::MeshInfo plateInfo;
    Check(error.empty() && Measure(plateCut, plateInfo) && plateInfo.components == 1,
          "薄い板を最大の深さで切っても内部が残る");

    // 局所。表面の凸な点まわりの球の中だけを切る。球の壁が形に当たる欠けは除かれる。
    geometry::PlaneCutsSettings chip;
    chip.count = 96;
    chip.scope = geometry::PlaneCutsScope::Local;
    chip.radius = .2f;
    chip.depthMin = chip.depthMax = .25f;
    const auto chipPlanes = geometry::MakeCutPlanes(sphere, chip, error);
    Check(error.empty() && !chipPlanes.empty() && chipPlanes.size() < 96,
          "滑らかな球では、壁が残る欠けを除いた一部だけを使う");
    bool onSurface = true, outwardFacing = true, depthHalved = true;
    for (const auto& local : chipPlanes) {
        onSurface &= std::abs(std::sqrt(Dot(local.center, local.center)) - 1) < sphere.spacing * 2.5f &&
                     std::abs(local.radius - .4f) < sphere.spacing;
        outwardFacing &= Dot(local.normal, local.center) > 0;
        // 球の縁に材料が掛かる深さは半分ずつ浅くされる。
        const float chipDepth = Dot(local.normal, local.center) - local.offset;
        bool matches = false;
        for (float expected = .25f * local.radius; expected > .25f * local.radius / 10; expected *= .5f)
            matches |= std::abs(chipDepth - expected) < 1e-4f;
        depthHalved &= matches;
    }
    Check(onSurface, "中心は表面にあり、半径は最長辺に対する比で決まる");
    Check(outwardFacing, "法線は中心の点で外を向く");
    Check(depthHalved, "深さは欠けの半径に対する比で決まり、必要なら半分ずつ浅くなる");
    const auto chipped = geometry::CutVolume(sphere, chip, error);
    geometry::MeshInfo chippedInfo;
    // 欠けの外で内外が変わるのは、切り離されて除かれた浮いた小片だけ。
    size_t changedPoints = 0, uncovered = 0;
    for (uint32_t z = 0; z < sphere.dimensions[2]; ++z)
        for (uint32_t y = 0; y < sphere.dimensions[1]; ++y)
            for (uint32_t x = 0; x < sphere.dimensions[0]; ++x) {
                const size_t i = sphere.Index(x, y, z);
                if ((sphere.values[i] < 0) == (chipped.values[i] < 0)) continue;
                const auto p = sphere.Position(x, y, z);
                bool covered = false;
                for (const auto& local : chipPlanes) {
                    const geometry::Vec3 d{p.x - local.center.x, p.y - local.center.y, p.z - local.center.z};
                    covered |= std::sqrt(Dot(d, d)) < local.radius && Dot(local.normal, p) > local.offset;
                }
                ++changedPoints;
                uncovered += covered ? 0 : 1;
            }
    Check(error.empty() && Measure(chipped, chippedInfo) && chippedInfo.components == 1 &&
              chippedInfo.volume < sphereInfo.volume - .005 && chippedInfo.volume > sphereInfo.volume * .8,
          "局所の欠けは形を少しだけ削る");
    Check(changedPoints > 0 && uncovered * 50 <= changedPoints,
          "内外が変わるのは欠けの球の中で平面の外側にある点（と、除かれたわずかな小片）だけ");
    // 凹みのある形。全体は凹みを埋めるように凸へ近づき、局所は凹みを残す。
    auto cross = geometry::MakeBox({3, 1, 1});
    {
        const auto bar = geometry::MakeBox({1, 3, 1});
        const auto offset = static_cast<uint32_t>(cross.positions.size());
        cross.positions.insert(cross.positions.end(), bar.positions.begin(), bar.positions.end());
        for (auto face : bar.triangles) {
            for (auto& index : face) index += offset;
            cross.triangles.push_back(face);
        }
    }
    const auto crossVolume = geometry::MeshToVolume(cross, {64}, error);
    Check(error.empty(), "十字形のボリューム");
    geometry::PlaneCutsSettings chips;
    chips.count = 80;
    chips.depthMin = .05f;
    chips.depthMax = .2f;
    auto localChips = chips;
    localChips.scope = geometry::PlaneCutsScope::Local;
    localChips.radius = .12f;
    geometry::MeshInfo crossInfo, globalInfo, localInfo;
    const auto globalCross = geometry::CutVolume(crossVolume, chips, error);
    const auto localCross = geometry::CutVolume(crossVolume, localChips, error);
    Check(error.empty() && Measure(crossVolume, crossInfo) && Measure(globalCross, globalInfo) &&
              Measure(localCross, localInfo) && localInfo.components == 1,
          "十字形を全体と局所で切り落とせる");
    // 腕の先（中心から 1.5 m）が残るかで比べる。
    Check(localInfo.maximum.x > 1.3f && localInfo.maximum.y > 1.3f && localInfo.minimum.x < -1.3f &&
              localInfo.minimum.y < -1.3f,
          "局所は4本の腕の張り出しを残す");
    Check(globalInfo.maximum.x < 1.3f && globalInfo.maximum.y < 1.3f, "全体は腕の先を切り落とす");
    Check(globalInfo.volume < localInfo.volume, "同じ枚数なら全体のほうが大きく削る");
    // 重なった欠けが角を切り離しても、浮いた小片は残さない。
    auto heavy = localChips;
    heavy.count = geometry::MaxPlaneCuts;
    heavy.radius = .15f;
    heavy.depthMax = .4f;
    geometry::MeshInfo heavyInfo;
    const auto heavyCross = geometry::CutVolume(crossVolume, heavy, error);
    Check(error.empty() && Measure(heavyCross, heavyInfo) && heavyInfo.components == 1,
          "欠けを重ねても1つの塊だけを残す");
    Check(geometry::CutVolume(crossVolume, localChips, error).values == localCross.values,
          "局所も同じ入力と Seed から同じ結果を得る");
    rejects("範囲外の半径を拒否する", [](auto& s) { s.radius = 0; });
    rejects("不明な適用範囲を拒否する", [](auto& s) { s.scope = static_cast<geometry::PlaneCutsScope>(7); });
    Check(geometry::ParsePlaneCutsScope(geometry::PlaneCutsScopeName(geometry::PlaneCutsScope::Local)) ==
                  geometry::PlaneCutsScope::Local &&
              geometry::ParsePlaneCutsScope("?") == geometry::PlaneCutsScope::Global,
          "適用範囲の保存名を往復でき、不明な名前は全体として読む");

    Section("Plane Cuts のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               coarse = g.CreateNode(graph::NodeKind::PlaneCuts), fine = g.CreateNode(graph::NodeKind::PlaneCuts),
               surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(g.FindNode(coarse)->inputs.size() == 1 && g.FindNode(coarse)->outputs.size() == 1 &&
              std::holds_alternative<geometry::PlaneCutsSettings>(g.FindNode(coarse)->settings),
          "ノードは Volume の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(coarse)->inputs[0].id),
          "Mesh 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, coarse).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 32;
    Check(link(shape, volume) && link(volume, coarse) && link(coarse, fine) && link(fine, surface),
          "To Volume → Plane Cuts → Plane Cuts → Volume to Mesh を接続できる");
    auto& fineSettings = std::get<geometry::PlaneCutsSettings>(g.FindMutableNode(fine)->settings);
    fineSettings.count = 40;
    fineSettings.seed = 5;
    fineSettings.depthMin = .02f;
    fineSettings.depthMax = .08f;
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 &&
              geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) && graphInfo.closed &&
              graphInfo.volume > 1 && graphInfo.volume < 8,
          "2段の切り落としを評価して閉じたメッシュを得る");
    const auto coarseFirst = graph::EvaluateRocks(g, coarse, &cache);
    const auto fineFirst = graph::EvaluateRocks(g, fine, &cache);
    ++std::get<geometry::PlaneCutsSettings>(g.FindMutableNode(fine)->settings).seed;
    const auto fineSecond = graph::EvaluateRocks(g, fine, &cache);
    const auto coarseSecond = graph::EvaluateRocks(g, coarse, &cache);
    Check(fineSecond.error.empty() && fineSecond.rocks[0].volume != fineFirst.rocks[0].volume,
          "設定の変更で作り直す");
    Check(coarseSecond.rocks[0].volume == coarseFirst.rocks[0].volume, "変更していない上段は再利用する");
    std::get<geometry::PlaneCutsSettings>(g.FindMutableNode(coarse)->settings).depthMax = .3f;
    const auto fineThird = graph::EvaluateRocks(g, fine, &cache);
    Check(fineThird.error.empty() && fineThird.rocks[0].volume != fineSecond.rocks[0].volume,
          "上段の変更は下段へ伝わる");
}
