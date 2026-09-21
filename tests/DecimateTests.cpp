#include "TestSupport.h"
#include "geometry/BaseRock.h"
#include "geometry/BoxCluster.h"
#include "geometry/Decimate.h"
#include "geometry/UvUnwrap.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <chrono>
#include <cmath>
#include <limits>

using namespace rock::tests;
using namespace rock;

namespace {
// 出力の頂点が、入力の表面からどれだけ離れたか。入力が球なら、中心からの距離で測れる。
double MaxRadialError(const geometry::Mesh& mesh, double radius) {
    double worst = 0;
    for (const auto& p : mesh.positions)
        worst = std::max(worst, std::abs(std::sqrt(double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z) - radius));
    return worst;
}
}  // namespace

void RunDecimateTests() {
    Section("Decimate");
    std::string error;
    // 半径 1 m の球（12,288 三角形）。
    geometry::BaseRockSettings sphereSettings;
    sphereSettings.shape = geometry::BaseShape::Sphere;
    sphereSettings.size = {2, 2, 2};
    sphereSettings.subdivisions = 32;
    const auto sphere = geometry::MakeBaseRock(sphereSettings, error);
    geometry::MeshInfo sphereInfo;
    Check(error.empty() && geometry::InspectMesh(sphere, sphereInfo) && sphere.triangles.size() == 12288, "球のメッシュ");

    geometry::DecimateSettings s;
    s.targetTriangles = 2000;
    s.maxError = 0;
    const auto reduced = geometry::DecimateMesh(sphere, s, error);
    geometry::MeshInfo reducedInfo;
    Check(error.empty() && geometry::InspectMesh(reduced, reducedInfo) && reducedInfo.closed &&
              reducedInfo.components == 1 && reducedInfo.volume > 0,
          "減らした結果も閉じた外向きの1つの塊");
    Check(reduced.triangles.size() <= 2000 && reduced.triangles.size() >= 1990, "目標の三角形数まで減る");
    Check(reduced.triangles.size() % 2 == 0 && reduced.positions.size() == reduced.triangles.size() / 2 + 2,
          "閉じた球面の頂点数と面数の関係（V = F/2 + 2）を保つ");
    Check(std::abs(reducedInfo.volume - sphereInfo.volume) < sphereInfo.volume * .02 && MaxRadialError(reduced, 1) < .02,
          "1/6 に減らしても体積と表面の位置をほぼ保つ");
    Check(reduced.cornerUvs.empty() && reduced.uvCharts.empty() && reduced.uvWidth == 0, "UV は引き継がない");
    const auto again = geometry::DecimateMesh(sphere, s, error);
    Check(again.positions == reduced.positions && again.triangles == reduced.triangles, "同じ入力から同じ結果を得る");

    // 入力が目標以下なら形を変えない。
    s.targetTriangles = 20000;
    const auto untouched = geometry::DecimateMesh(sphere, s, error);
    Check(error.empty() && untouched.positions == sphere.positions && untouched.triangles == sphere.triangles,
          "入力が目標以下なら何もしない");

    // 形のずれの上限。小さくすると、目標に届く前に止まる。
    s.targetTriangles = 200;
    s.maxError = .0005f;
    const auto guarded = geometry::DecimateMesh(sphere, s, error);
    s.maxError = 0;
    const auto unguarded = geometry::DecimateMesh(sphere, s, error);
    Check(error.empty() && guarded.triangles.size() > unguarded.triangles.size() * 2 &&
              unguarded.triangles.size() <= 200,
          "形のずれの上限は、目標より手前で減らすのを止める");
    Check(MaxRadialError(guarded, 1) < MaxRadialError(unguarded, 1), "上限があるほうが形のずれが小さい");

    // 平らな面は大きく減り、角は残る。分割した立方体は12枚まで減らせる。
    const auto grid = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {32}, error);
    const auto gridMesh = geometry::VolumeSurface(grid, error, geometry::VolumeMeshingMethod::DualContouring);
    geometry::MeshInfo gridInfo;
    Check(error.empty() && geometry::InspectMesh(gridMesh, gridInfo) && gridMesh.triangles.size() > 5000,
          "格子から作った立方体は細かい三角形を持つ");
    geometry::DecimateSettings flat;
    flat.targetTriangles = 64;
    flat.maxError = .002f;
    const auto flatBox = geometry::DecimateMesh(gridMesh, flat, error);
    geometry::MeshInfo flatInfo;
    Check(error.empty() && geometry::InspectMesh(flatBox, flatInfo) && flatInfo.closed &&
              flatBox.triangles.size() < gridMesh.triangles.size() / 20,
          "平らな面は、形のずれの上限の中で 1/20 未満まで減る");
    Check(std::abs(flatInfo.volume - gridInfo.volume) < gridInfo.volume * .01 &&
              std::abs(flatInfo.maximum.x - gridInfo.maximum.x) < .01f &&
              std::abs(flatInfo.minimum.y - gridInfo.minimum.y) < .01f,
          "立方体の体積と外接範囲を保つ");

    // 稜線の保護。保護なしより、角の位置がよく残る。
    geometry::DecimateSettings crease;
    crease.targetTriangles = 64;
    crease.maxError = 0;
    crease.creaseWeight = 0;
    const auto rounded = geometry::DecimateMesh(gridMesh, crease, error);
    crease.creaseWeight = 4;
    const auto sharp = geometry::DecimateMesh(gridMesh, crease, error);
    geometry::MeshInfo roundedInfo, sharpInfo;
    Check(geometry::InspectMesh(rounded, roundedInfo) && geometry::InspectMesh(sharp, sharpInfo) && roundedInfo.closed &&
              sharpInfo.closed,
          "稜線の保護の有無によらず閉じた形になる");
    // 立方体の角は QEM だけでも残る。保護の有無によらず、64枚まで減らしても体積は変わらない。
    Check(std::abs(sharpInfo.volume - gridInfo.volume) < gridInfo.volume * 1e-3 &&
              std::abs(roundedInfo.volume - gridInfo.volume) < gridInfo.volume * 1e-3 && sharp.triangles.size() <= 64,
          "64枚まで減らしても立方体の角と体積を保つ");
    // 平らな面でも、針のように細長い三角形を作らない（UV 展開で縮退する）。
    double worstQuality = 1;
    for (const auto& face : sharp.triangles) {
        const auto &a = sharp.positions[face[0]], &b = sharp.positions[face[1]], &c = sharp.positions[face[2]];
        const double ab[3] = {double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z},
                     ac[3] = {double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z},
                     bc[3] = {double(c.x) - b.x, double(c.y) - b.y, double(c.z) - b.z};
        const double cross[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                                 ab[0] * ac[1] - ab[1] * ac[0]};
        const auto squared = [](const double* v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; };
        worstQuality =
            std::min(worstQuality, std::sqrt(squared(cross)) / std::max({squared(ab), squared(ac), squared(bc)}));
    }
    Check(worstQuality > .04, "平らな面でも針のような三角形を作らない");

    // 複数の成分。どの成分も消さない。
    auto twin = geometry::MakeBaseRock(sphereSettings, error);
    {
        const auto offset = static_cast<uint32_t>(twin.positions.size());
        for (auto p : sphere.positions) twin.positions.push_back({p.x + 3, p.y, p.z});
        for (auto face : sphere.triangles) {
            for (auto& index : face) index += offset;
            twin.triangles.push_back(face);
        }
    }
    geometry::DecimateSettings few;
    few.targetTriangles = 64;
    few.maxError = 0;
    const auto twinReduced = geometry::DecimateMesh(twin, few, error);
    geometry::MeshInfo twinInfo;
    Check(error.empty() && geometry::InspectMesh(twinReduced, twinInfo) && twinInfo.closed && twinInfo.components == 2,
          "目標が小さくても、成分を消さずに閉じた形を保つ");

    // 最小の閉じた形（四面体）は、それ以上減らさない。
    geometry::Mesh tetra;
    tetra.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    tetra.triangles = {{{0, 2, 1}}, {{0, 1, 3}}, {{1, 2, 3}}, {{0, 3, 2}}};
    // 目標の下限（64）より少ないので、そのまま返る。
    const auto tetraOut = geometry::DecimateMesh(tetra, few, error);
    Check(error.empty() && tetraOut.triangles == tetra.triangles, "目標より少ない入力はそのまま返す");

    // 進捗とキャンセル。
    int calls = 0, last = -1;
    bool monotone = true;
    geometry::DecimateSettings progressSettings;
    progressSettings.targetTriangles = 500;
    progressSettings.maxError = 0;
    const auto reported = geometry::DecimateMesh(sphere, progressSettings, error, {}, [&](int percent) {
        ++calls;
        monotone &= percent >= last && percent >= 0 && percent <= 100;
        last = percent;
    });
    Check(error.empty() && calls > 1 && monotone && last == 100, "進み具合を 0〜100 の単調な値で通知する");
    Check(reported.triangles == geometry::DecimateMesh(sphere, progressSettings, error).triangles,
          "進捗を受け取っても結果は変わらない");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = geometry::DecimateMesh(sphere, progressSettings, error, stop.get_token());
    Check(!error.empty() && cancelled.triangles.empty(), "キャンセルすると結果を返さない");

    // 不正な入力と設定。
    const auto rejects = [&](const char* name, const geometry::Mesh& mesh, auto change) {
        geometry::DecimateSettings bad;
        change(bad);
        const auto result = geometry::DecimateMesh(mesh, bad, error);
        Check(!error.empty() && result.triangles.empty(), name);
    };
    rejects("小さすぎる目標を拒否する", sphere, [](auto& v) { v.targetTriangles = 10; });
    rejects("大きすぎる目標を拒否する", sphere, [](auto& v) { v.targetTriangles = 1000000; });
    rejects("負の形のずれの上限を拒否する", sphere, [](auto& v) { v.maxError = -1; });
    rejects("非有限の稜線の保護を拒否する", sphere, [](auto& v) { v.creaseWeight = std::numeric_limits<float>::infinity(); });
    auto open = sphere;
    open.triangles.pop_back();
    rejects("開いたメッシュを拒否する", open, [](auto&) {});
    auto inverted = sphere;
    for (auto& face : inverted.triangles) std::swap(face[1], face[2]);
    rejects("内向きのメッシュを拒否する", inverted, [](auto&) {});
    rejects("空のメッシュを拒否する", geometry::Mesh{}, [](auto&) {});

    // 実際の用途。ノイズ付きの塊（Dual Contouring）を減らして、UV 展開と再ボリューム化へ渡せること。
    Section("Decimate の下流への接続");
    geometry::BoxClusterSettings cluster;
    cluster.count = 6;
    cluster.seed = 11;
    auto volume = geometry::BoxesToVolume(geometry::MakeBoxCluster(cluster, error), {64}, error);
    geometry::VolumeNoiseSettings noise;
    noise.warp = .02f;
    volume = geometry::NoiseVolume(volume, noise, error);
    const auto rock = geometry::VolumeSurface(volume, error, geometry::VolumeMeshingMethod::DualContouring);
    geometry::MeshInfo rockInfo;
    Check(error.empty() && geometry::InspectMesh(rock, rockInfo) && rock.triangles.size() > 20000, "ノイズ付きの塊のメッシュ");
    geometry::DecimateSettings practical;
    practical.targetTriangles = 6000;
    const auto start = std::chrono::steady_clock::now();
    const auto rockReduced = geometry::DecimateMesh(rock, practical, error);
    const double decimateMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    geometry::MeshInfo rockReducedInfo;
    Check(error.empty() && geometry::InspectMesh(rockReduced, rockReducedInfo) && rockReducedInfo.closed &&
              rockReducedInfo.components == rockInfo.components &&
              rockReduced.triangles.size() < rock.triangles.size() / 2 &&
              std::abs(rockReducedInfo.volume - rockInfo.volume) < rockInfo.volume * .02,
          "岩の形を保ったまま半分未満に減る");
    std::printf("  Decimate: %zu -> %zu triangles, %.1f ms\n", rock.triangles.size(), rockReduced.triangles.size(), decimateMs);
    geometry::UvUnwrapSettings uv;
    uv.resolution = 512;
    const auto unwrapped = geometry::UnwrapMesh(rockReduced, uv, error);
    Check(error.empty() && geometry::HasValidUvs(unwrapped), "減らしたメッシュを UV 展開できる");
    const auto revoxelized = geometry::MeshToVolume(rockReduced, {32}, error);
    Check(error.empty() && !revoxelized.values.empty(), "減らしたメッシュを再びボリューム化できる（閉じた向き付きのまま）");

    Section("Decimate のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), toVolume = g.CreateNode(graph::NodeKind::ToVolume),
               toMesh = g.CreateNode(graph::NodeKind::VolumeToMesh), decimate = g.CreateNode(graph::NodeKind::Decimate),
               unwrap = g.CreateNode(graph::NodeKind::UvUnwrap);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(g.FindNode(decimate)->inputs.size() == 1 && g.FindNode(decimate)->outputs.size() == 1 &&
              std::holds_alternative<geometry::DecimateSettings>(g.FindNode(decimate)->settings),
          "ノードは Mesh の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(toVolume)->outputs[0].id, g.FindNode(decimate)->inputs[0].id),
          "Volume 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, decimate).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(toVolume)->settings).resolution = 40;
    std::get<geometry::DecimateSettings>(g.FindMutableNode(decimate)->settings).targetTriangles = 1000;
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(unwrap)->settings).resolution = 256;
    Check(link(shape, toVolume) && link(toVolume, toMesh) && link(toMesh, decimate) && link(decimate, unwrap),
          "Volume to Mesh → Decimate → UV Unwrap を接続できる");
    graph::RockEvaluationCache cache;
    graph::RockEvaluationProgress progress;
    const auto full = graph::EvaluateRocks(g, toMesh, &cache);
    const auto evaluated = graph::EvaluateRocks(g, decimate, &cache, geometry::VolumeMeshingMethod::MarchingTetrahedra, {},
                                                &progress);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && evaluated.rocks[0].source == decimate &&
              geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) && graphInfo.closed &&
              evaluated.rocks[0].mesh.triangles.size() < full.rocks[0].mesh.triangles.size() / 2,
          "グラフの評価で三角形が減る");
    Check(progress.node.load() == 0, "評価が終わると、計算中のノードは無しへ戻る");
    const auto final = graph::EvaluateRocks(g, unwrap, &cache);
    Check(final.error.empty() && final.rocks.size() == 1 && geometry::HasValidUvs(final.rocks[0].mesh) &&
              final.rocks[0].mesh.triangles.size() == evaluated.rocks[0].mesh.triangles.size(),
          "減らしたメッシュが UV Unwrap へ渡る");
    std::get<geometry::DecimateSettings>(g.FindMutableNode(decimate)->settings).targetTriangles = 400;
    const auto fewer = graph::EvaluateRocks(g, decimate, &cache);
    Check(fewer.error.empty() && fewer.rocks[0].mesh.triangles.size() < evaluated.rocks[0].mesh.triangles.size(),
          "設定の変更で作り直す");
}
