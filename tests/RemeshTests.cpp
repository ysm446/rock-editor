#include "TestSupport.h"
#include "geometry/BaseRock.h"
#include "geometry/Remesh.h"
#include "geometry/UvUnwrap.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <thread>

using namespace rock::tests;
using namespace rock;

namespace {
double MaxRadialError(const geometry::Mesh& mesh, double radius) {
    double worst = 0;
    for (const auto& p : mesh.positions)
        worst = std::max(worst, std::abs(std::sqrt(double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z) - radius));
    return worst;
}
// 辺の長さの一覧（重複なし）。
std::vector<double> EdgeLengths(const geometry::Mesh& mesh) {
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    for (const auto& t : mesh.triangles)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = t[k], b = t[(k + 1) % 3];
            edges.emplace_back(std::min(a, b), std::max(a, b));
        }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<double> lengths;
    for (const auto [a, b] : edges) {
        const auto &p = mesh.positions[a], &q = mesh.positions[b];
        lengths.push_back(std::sqrt(double(p.x - q.x) * (p.x - q.x) + double(p.y - q.y) * (p.y - q.y) + double(p.z - q.z) * (p.z - q.z)));
    }
    return lengths;
}
// 辺の長さのうち、[low, high] に入る割合。
double FractionWithin(const std::vector<double>& lengths, double low, double high) {
    size_t within = 0;
    for (const double l : lengths) within += l >= low && l <= high;
    return lengths.empty() ? 0 : double(within) / double(lengths.size());
}
// 三角形の形の良さ（面積の2倍 ÷ 最長辺の二乗）の最小値と、0.3 未満の割合。
std::pair<double, double> QualityStats(const geometry::Mesh& mesh) {
    double worst = 1;
    size_t poor = 0;
    for (const auto& t : mesh.triangles) {
        const auto &a = mesh.positions[t[0]], &b = mesh.positions[t[1]], &c = mesh.positions[t[2]];
        const double ab[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, ac[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
        const double n[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
        const double twiceArea = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const double bc[3] = {c.x - b.x, c.y - b.y, c.z - b.z};
        const double longest = std::max({ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2],
                                         bc[0] * bc[0] + bc[1] * bc[1] + bc[2] * bc[2]});
        const double q = longest > 0 ? twiceArea / longest : 0;
        worst = std::min(worst, q);
        poor += q < .3;
    }
    return {worst, mesh.triangles.empty() ? 0 : double(poor) / double(mesh.triangles.size())};
}
// 頂点の次数が 5〜7 の割合。
double RegularValenceFraction(const geometry::Mesh& mesh) {
    std::vector<int> valence(mesh.positions.size(), 0);
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    for (const auto& t : mesh.triangles)
        for (int k = 0; k < 3; ++k) edges.emplace_back(std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3]));
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (const auto [a, b] : edges) { ++valence[a]; ++valence[b]; }
    size_t regular = 0;
    for (const int v : valence) regular += v >= 5 && v <= 7;
    return valence.empty() ? 0 : double(regular) / double(valence.size());
}
}  // namespace

void RunRemeshTests() {
    Section("Remesh");
    std::string error;
    // 半径 1 m の球（12,288 三角形）。最長辺 2 m なので、辺の長さ 0.05 は 0.1 m。
    geometry::BaseRockSettings sphereSettings;
    sphereSettings.shape = geometry::BaseShape::Sphere;
    sphereSettings.size = {2, 2, 2};
    sphereSettings.subdivisions = 32;
    const auto sphere = geometry::MakeBaseRock(sphereSettings, error);
    geometry::MeshInfo sphereInfo;
    Check(error.empty() && geometry::InspectMesh(sphere, sphereInfo) && sphere.triangles.size() == 12288, "球のメッシュ");

    geometry::RemeshSettings s;
    s.edgeLength = .05f;
    s.iterations = 5;
    const auto remeshed = geometry::RemeshMesh(sphere, s, error);
    geometry::MeshInfo info;
    Check(error.empty() && geometry::InspectMesh(remeshed, info) && info.closed && info.components == 1 && info.volume > 0,
          "結果は閉じた外向きの1つの塊");
    // 表面積 4π ÷ (√3/4 × 0.01) ≈ 2,900 面。
    Check(remeshed.triangles.size() > 2000 && remeshed.triangles.size() < 4000, "三角形数は辺の長さから見込める数になる");
    Check(remeshed.positions.size() == remeshed.triangles.size() / 2 + 2, "閉じた球面の頂点数と面数の関係（V = F/2 + 2）を保つ");
    Check(std::abs(info.volume - sphereInfo.volume) < sphereInfo.volume * .02 && MaxRadialError(remeshed, 1) < .01,
          "体積と表面の位置をほぼ保つ（元の表面へ投影する）");
    const auto lengths = EdgeLengths(remeshed);
    Check(FractionWithin(lengths, .1 * .6, .1 * 1.4) > .9, "辺の長さの9割以上が目標の ±40% に入る");
    Check(QualityStats(remeshed).second < .02, "形の悪い三角形はほとんど無い");
    Check(RegularValenceFraction(remeshed) > .8, "頂点の次数の8割以上が 5〜7");
    Check(remeshed.cornerUvs.empty() && remeshed.uvCharts.empty() && remeshed.uvWidth == 0, "UVは持たない");
    const auto again = geometry::RemeshMesh(sphere, s, error);
    Check(again.positions == remeshed.positions && again.triangles == remeshed.triangles, "同じ入力から同じ結果を得る");
    auto fine = s;
    fine.edgeLength = .025f;
    const auto finer = geometry::RemeshMesh(sphere, fine, error);
    Check(error.empty() && finer.triangles.size() > remeshed.triangles.size() * 3 && finer.triangles.size() < remeshed.triangles.size() * 5,
          "辺の長さを半分にすると三角形数は約4倍になる");
    auto coarse = s;
    coarse.edgeLength = .2f;
    const auto coarser = geometry::RemeshMesh(sphere, coarse, error);
    geometry::MeshInfo coarseInfo;
    Check(error.empty() && geometry::InspectMesh(coarser, coarseInfo) && coarseInfo.closed && coarser.triangles.size() < 400,
          "粗い辺の長さでも閉じたまま少ない面数になる");
    auto once = s;
    once.iterations = 1;
    const auto rough = geometry::RemeshMesh(sphere, once, error);
    Check(error.empty() && FractionWithin(EdgeLengths(rough), .1 * .6, .1 * 1.4) <= FractionWithin(lengths, .1 * .6, .1 * 1.4),
          "繰り返しが多いほど辺の長さが揃う");

    // 立方体。特徴辺（90度の稜線）と角を保つ。
    const auto box = geometry::MakeBox({2, 1, 1});
    geometry::MeshInfo boxInfo;
    geometry::InspectMesh(box, boxInfo);
    geometry::RemeshSettings boxSettings;
    boxSettings.edgeLength = .05f;
    boxSettings.iterations = 8;
    const auto boxRemeshed = geometry::RemeshMesh(box, boxSettings, error);
    geometry::MeshInfo boxRemeshedInfo;
    Check(error.empty() && geometry::InspectMesh(boxRemeshed, boxRemeshedInfo) && boxRemeshedInfo.closed && boxRemeshedInfo.components == 1,
          "直方体：閉じた1つの塊になる");
    Check(std::abs(boxRemeshedInfo.volume - boxInfo.volume) < boxInfo.volume * .01 &&
              std::abs(boxRemeshedInfo.maximum.x - boxInfo.maximum.x) < 1e-3 && std::abs(boxRemeshedInfo.minimum.y - boxInfo.minimum.y) < 1e-3,
          "直方体：体積と外接箱を保つ（角が丸くならない）");
    // 頂点は全て面上か稜線上にある。どの頂点も、いずれかの面の平面から 1 mm 以内。
    bool onSurface = true;
    for (const auto& p : boxRemeshed.positions) {
        const double dx = 1 - std::abs(p.x), dy = .5 - std::abs(p.y), dz = .5 - std::abs(p.z);
        onSurface &= std::min({dx, dy, dz}) < 1e-3 && std::min({dx, dy, dz}) > -1e-3;
    }
    Check(onSurface, "直方体：頂点は元の面の上にある");
    Check(FractionWithin(EdgeLengths(boxRemeshed), .1 * .6, .1 * 1.4) > .85, "直方体：辺の長さが揃う");
    auto noFeature = boxSettings;
    noFeature.featureAngle = 180;
    const auto rounded = geometry::RemeshMesh(box, noFeature, error);
    geometry::MeshInfo roundedInfo;
    Check(error.empty() && geometry::InspectMesh(rounded, roundedInfo) && roundedInfo.closed && roundedInfo.volume < boxRemeshedInfo.volume,
          "特徴なし：角が丸くなり体積が減る");

    // Marching Tetrahedra の細長い三角形を揃える。
    const auto grid = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {32}, error);
    const auto slivers = geometry::VolumeSurface(grid, error);
    geometry::MeshInfo sliverInfo;
    Check(error.empty() && geometry::InspectMesh(slivers, sliverInfo) && QualityStats(slivers).second > .1, "格子から作ったメッシュには形の悪い三角形が多い");
    geometry::RemeshSettings evenSettings;
    evenSettings.edgeLength = .04f;
    const auto evened = geometry::RemeshMesh(slivers, evenSettings, error);
    geometry::MeshInfo evenInfo;
    Check(error.empty() && geometry::InspectMesh(evened, evenInfo) && evenInfo.closed && evenInfo.components == 1 &&
              QualityStats(evened).second < .02 && std::abs(evenInfo.volume - sliverInfo.volume) < sliverInfo.volume * .02,
          "格子のメッシュを揃えても形の悪い三角形が消え、体積は保たれる");

    // 拒否と取消。
    const auto rejects = [&](const char* name, auto change) {
        geometry::RemeshSettings bad;
        change(bad);
        const auto result = geometry::RemeshMesh(sphere, bad, error);
        Check(!error.empty() && result.triangles.empty(), name);
    };
    rejects("小さすぎる辺の長さを拒否する", [](auto& r) { r.edgeLength = .001f; });
    rejects("大きすぎる辺の長さを拒否する", [](auto& r) { r.edgeLength = .5f; });
    rejects("繰り返し 0 を拒否する", [](auto& r) { r.iterations = 0; });
    rejects("範囲外の特徴辺の角度を拒否する", [](auto& r) { r.featureAngle = 200; });
    geometry::UvUnwrapSettings atlas;
    atlas.resolution = 128;
    const auto unwrapped = geometry::UnwrapMesh(geometry::MakeBox({1, 1, 1}), atlas, error);
    Check(error.empty() && geometry::RemeshMesh(unwrapped, s, error).triangles.empty() && error.find("UV") != std::string::npos,
          "UV付きの入力を診断する");
    geometry::Mesh open = sphere;
    open.triangles.pop_back();
    Check(geometry::RemeshMesh(open, s, error).triangles.empty() && !error.empty(), "閉じていない入力を診断する");
    auto huge = s;
    huge.edgeLength = .002f;
    Check(geometry::RemeshMesh(geometry::MakeBox({100, 100, 100}), huge, error).triangles.empty() && error.find("100万") != std::string::npos,
          "100万面を超える設定を診断する");
    std::stop_source stop;
    stop.request_stop();
    Check(geometry::RemeshMesh(sphere, s, error, stop.get_token()).triangles.empty() && !error.empty(), "取消で空を返す");
    int lastProgress = -1;
    geometry::RemeshMesh(sphere, s, error, {}, [&](int p) { lastProgress = p; });
    Check(lastProgress == 100, "進捗は 100 に達する");

    Section("Remesh のグラフ");
    graph::NodeGraph g;
    const auto base = g.CreateNode(graph::NodeKind::BaseRock), node = g.CreateNode(graph::NodeKind::Remesh),
               uv = g.CreateNode(graph::NodeKind::UvUnwrap);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(graph::FindNodeDefinitionByName("remesh") && g.FindNode(node)->inputs.size() == 1 && g.FindNode(node)->outputs.size() == 1 &&
              g.FindNode(node)->inputs[0].valueType == graph::ValueType::Mesh &&
              std::holds_alternative<geometry::RemeshSettings>(g.FindNode(node)->settings) && graph::IsPreviewableNodeKind(graph::NodeKind::Remesh),
          "ノードは Mesh の入出力と既定の設定を持ち、プレビューできる");
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    std::get<graph::BaseRockNodeSettings>(g.FindMutableNode(base)->settings).size = {2, 1, 1};
    Check(link(base, node) && link(node, uv), "Base Shape → Remesh → UV Unwrap を接続できる");
    std::get<geometry::RemeshSettings>(g.FindMutableNode(node)->settings).edgeLength = .1f;
    graph::RockEvaluationCache cache;
    const auto first = graph::EvaluateRocks(g, node, &cache);
    geometry::MeshInfo graphInfo;
    Check(first.error.empty() && first.rocks.size() == 1 && geometry::InspectMesh(first.rocks[0].mesh, graphInfo) && graphInfo.closed &&
              first.rocks[0].source == node && !first.rocks[0].boxes,
          "グラフの評価で閉じたメッシュを得る");
    const auto again2 = graph::EvaluateRocks(g, node, &cache);
    Check(again2.rocks[0].mesh.positions == first.rocks[0].mesh.positions && cache.computations[node] == 1, "変更がなければ結果を再利用する");
    std::get<geometry::RemeshSettings>(g.FindMutableNode(node)->settings).iterations = 2;
    const auto changed = graph::EvaluateRocks(g, node, &cache);
    Check(changed.error.empty() && cache.computations[node] == 2, "設定の変更で作り直す");
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(uv)->settings).resolution = 128;
    const auto unwrappedGraph = graph::EvaluateRocks(g, uv, &cache);
    Check(unwrappedGraph.error.empty() && geometry::HasValidUvs(unwrappedGraph.rocks[0].mesh), "Remesh の後で UV Unwrap できる");
    const auto after = g.CreateNode(graph::NodeKind::Remesh);
    link(uv, after);
    Check(!graph::EvaluateRocks(g, after, &cache).error.empty(), "UV Unwrap の後に置くと診断する");
}
