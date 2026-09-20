// ノードグラフから評価用レイヤー列へのコンパイルを確かめる。
// GPU 評価の前段だけを対象にし、入力を外したときに古い結果を残さない規則を固定する。

#include "graph/NodeGraph.h"

#include "TestSupport.h"

#include <array>
#include <variant>
#include <cmath>

namespace {

using tg::graph::NodeGraph;
using tg::graph::NodeKind;
using tg::tests::Check;
using tg::tests::Section;

bool IsNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.size() != 1) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

}  // namespace

void RunNodeGraphTests() {
    Section("ノードグラフ — 復元時のリンク検証と Merge の ID 保持");
    {
        NodeGraph graph;
        const auto a = graph.CreateNode(NodeKind::RoadMarking);
        const auto b = graph.CreateNode(NodeKind::RoadMarking);
        const auto c = graph.CreateNode(NodeKind::RoadMarking);
        const auto merge = graph.CreateNode(NodeKind::Merge);
        const auto aIn = graph.FindNode(a)->inputs[0].id;
        const auto aOut = graph.FindNode(a)->outputs[0].id;
        const auto bIn = graph.FindNode(b)->inputs[0].id;
        const auto bOut = graph.FindNode(b)->outputs[0].id;
        const auto cOut = graph.FindNode(c)->outputs[0].id;
        const auto spare = graph.FindNode(merge)->inputs[0].id;
        graph.CreateLink(aOut, bIn);
        Check(graph.FindNode(merge)->inputs[0].id == spare,
              "無関係なリンクを作っても Merge の空きピン ID は変わらない");
        const auto nodes = graph.Nodes();
        const auto links = graph.Links();
        graph.Replace(nodes, links);
        Check(graph.FindNode(merge)->inputs[0].id == spare,
              "保存データやアンドゥの復元で Merge の空きピン ID を保持する");
        graph.Replace(nodes, {{1000, aOut, bIn}, {1001, bOut, aIn},
                              {1002, cOut, bIn}, {1003, cOut, graph.FindNode(c)->inputs[0].id}});
        Check(graph.Links().size() == 1 && graph.Links()[0].id == 1000,
              "復元時は循環・入力の重複・自己接続を捨て、最初の有効な接続を保つ");
        Check(graph.FindUpstreamNodeForPin(bIn)->id == a &&
                  graph.FindUpstreamNodeForPin(aIn) == nullptr,
              "不正リンクの除去後も有効な上流を参照できる");
    }

    Section("ノードグラフ — 既定の下地と Surface");
    {
        NodeGraph graph = NodeGraph::CreateDefault();
        Check(graph.Nodes().size() == 1 && graph.Nodes().front().kind == NodeKind::Surface,
              "既定のグラフは Surface 1 つ");
        Check(IsNeutralPlane(graph.CompileLayers()),
              "出力ノードは無いので、既定のレイヤー列は変位 0 の平面になる");

        // Surface は入力を持たず、それ 1 枚がレイヤー列になる。
        const tg::graph::GraphId surfaceId = graph.CreateNode(NodeKind::Surface);
        tg::graph::Node* surface = graph.FindMutableNode(surfaceId);
        Check(surface != nullptr && surface->inputs.empty() && surface->outputs.size() == 1,
              "Surface は入力を持たず、Result だけを出す");
        std::get<tg::graph::LayerNodeSettings>(surface->settings).layer.roughness = 0.31f;
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(surfaceId);
        Check(compiled.layers.size() == 1 && compiled.layers.front().roughness == 0.31f &&
                  compiled.layerSources.size() == 1 && compiled.layerSources[0] == surfaceId,
              "Surface 1 つがそのままレイヤー列になり、元ノードを控える");

        // 無効な Surface は中立平面へ落ちる（古い結果を残さない）。
        std::get<tg::graph::LayerNodeSettings>(surface->settings).layer.enabled = false;
        Check(IsNeutralPlane(graph.CompileLayersTo(surfaceId)),
              "無効な Surface は変位 0 の平面になる");
    }

    Section("パス — 実寸カーブの範囲外編集");
    {
        NodeGraph graph;
        const auto id = graph.CreateNode(NodeKind::Path);
        auto& path = std::get<tg::graph::PathNodeSettings>(graph.FindMutableNode(id)->settings).path;
        Check(path.points.empty() && !path.surfaceSpace, "新規Pathは実寸座標で空");
        const auto a = tg::graph::AddPathPoint(path, -30.0f, -12.0f, 0);
        path.FindPoint(a)->y = 3.0f;
        const auto b = tg::graph::AddPathPoint(path, 40.0f, 15.0f, a);
        Check(path.FindPoint(a)->x == -30.0f && path.FindPoint(b)->x == 40.0f &&
              path.FindPoint(b)->y == 3.0f, "グリッド外の点を追加し起点の高さを継ぐ");
        tg::graph::MovePathPoints(path, {a,b}, 20.0f, -50.0f);
        Check(path.FindPoint(a)->x == -10.0f && path.FindPoint(b)->z == -35.0f,
              "範囲外へまとめて動かしても形を保つ");
        tg::graph::PathClip clip;
        tg::graph::ExtractPathClip(path, {a,b}, {}, clip);
        std::vector<tg::graph::PathElementId> pasted;
        Check(tg::graph::PastePathClip(path, clip, 100.0f, 100.0f, &pasted, nullptr) &&
              pasted.size() == 2 && path.FindPoint(pasted.front())->x == 90.0f,
              "貼り付けでも座標を丸めない");
        const auto strands = tg::graph::BuildPathStrands(path);
        const auto samples = tg::graph::SamplePathStrand(path, strands.front(), 16);
        Check(!samples.empty() && samples.front().y == 3.0f &&
              std::abs(samples.back().x - samples.front().x) == 70.0f,
              "道路の入力となるカーブ標本は実寸と高さを保持");
    }

    Section("パス — XYZカーブの補間");
    {
        using namespace tg::graph;
        PathSettings vertical;
        const auto a = AddPathPoint(vertical, 0.0f, 0.0f, 0);
        const auto b = AddPathPoint(vertical, 0.0f, 0.0f, a);
        vertical.FindPoint(b)->y = 10.0f;
        const auto inserted = InsertPathPointOnEdge(vertical, vertical.edges.front().id, 0.5f);
        Check(inserted != 0 && std::abs(vertical.FindPoint(inserted)->y - 5.0f) < 1e-5f,
              "垂直エッジの中点も3次元の距離で挿入する");
        for (const auto curve : {PathCurve::Quadratic, PathCurve::Cubic}) {
            PathSettings path;
            const auto p0 = AddPathPoint(path, 0.0f, 0.0f, 0);
            const auto p1 = AddPathPoint(path, 2.0f, 3.0f, p0);
            const auto p2 = AddPathPoint(path, 9.0f, 1.0f, p1);
            path.FindPoint(p1)->y = 10.0f;
            path.FindPoint(p2)->y = -2.0f;
            for (auto& edge : path.edges) edge.curve = curve;
            auto rotated = path;
            for (auto& point : rotated.points) std::swap(point.x, point.y);
            const auto samples = SamplePathStrand(path, BuildPathStrands(path).front(), 16);
            const auto transformed = SamplePathStrand(rotated, BuildPathStrands(rotated).front(), 16);
            bool consistent = samples.size() == transformed.size();
            for (size_t i = 0; consistent && i < samples.size(); ++i) {
                consistent = std::abs(samples[i].x - transformed[i].y) < 1e-5f &&
                             std::abs(samples[i].y - transformed[i].x) < 1e-5f &&
                             std::abs(samples[i].z - transformed[i].z) < 1e-5f;
            }
            Check(consistent, "ベジェ/Bスプラインは軸を入れ替えても同じ3次元曲線になる");
        }
    }

    Section("パス — まとめて動かす / コピーと貼り付け");
    {
        using tg::graph::PathClip;
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        PathSettings path;
        const PathElementId a = tg::graph::AddPathPoint(path, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(path, 0.4f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(path, 0.4f, 0.4f, b);
        const PathElementId lone = tg::graph::AddPathPoint(path, 0.9f, 0.9f, 0);
        const tg::graph::PathEdge* ab = path.FindEdgeBetween(a, b);
        const tg::graph::PathEdge* bc = path.FindEdgeBetween(b, c);
        Check(ab != nullptr && bc != nullptr && lone != 0, "3 点の鎖と孤立点を作れる");

        // まとめて動かす。座標は丸めない。
        const bool moved = tg::graph::MovePathPoints(path, {a, b, c}, 0.1f, -0.3f);
        const tg::graph::PathPoint* pa = path.FindPoint(a);
        const tg::graph::PathPoint* pc = path.FindPoint(c);
        Check(moved && pa != nullptr && pc != nullptr && std::abs(pa->x - 0.3f) < 1e-5f &&
                  std::abs(pa->z + 0.1f) < 1e-5f && std::abs(pc->x - 0.5f) < 1e-5f && std::abs(pc->z - 0.1f) < 1e-5f,
              "MovePathPoints は指定した点だけを動かす");
        float cu = 0.0f;
        float cv = 0.0f;
        Check(tg::graph::PathPointsCentroid(path, {a, b, c}, cu, cv) &&
                  std::abs(cu - (0.3f + 0.5f + 0.5f) / 3.0f) < 1e-5f,
              "PathPointsCentroid は重心を返す");

        // 鎖を切り出す。エッジは両端の点を連れていく。
        if (ab != nullptr && bc != nullptr) {
            const_cast<tg::graph::PathEdge*>(ab)->curve = tg::graph::PathCurve::Cubic;
        }
        PathClip clip;
        const bool extracted = tg::graph::ExtractPathClip(path, {}, {ab->id, bc->id}, clip);
        Check(extracted && clip.points.size() == 3 && clip.edges.size() == 2 &&
                  clip.edges.front().curve == tg::graph::PathCurve::Cubic,
              "ExtractPathClip は鎖の点とエッジを切り出し、曲線の性質は残す");

        // 点の集合から切り出すと、その間のエッジだけが付いてくる。
        PathClip pointClip;
        Check(tg::graph::ExtractPathClip(path, {a, b, lone}, {}, pointClip) &&
                  pointClip.points.size() == 3 && pointClip.edges.size() == 1,
              "点の集合の ExtractPathClip は点どうしを結ぶエッジだけを拾う");

        // 貼り付け。ID は振り直され、ずらした位置に同じ形で入る。
        const size_t pointsBefore = path.points.size();
        const size_t edgesBefore = path.edges.size();
        std::vector<PathElementId> pastedPoints;
        std::vector<PathElementId> pastedEdges;
        const bool pasted =
            tg::graph::PastePathClip(path, clip, 0.2f, 0.5f, &pastedPoints, &pastedEdges);
        bool idsFresh = true;
        for (const PathElementId id : pastedPoints) {
            idsFresh &= (id != a && id != b && id != c && id != lone);
        }
        const tg::graph::PathPoint* firstPasted =
            pastedPoints.empty() ? nullptr : path.FindPoint(pastedPoints.front());
        Check(pasted && path.points.size() == pointsBefore + 3 &&
                  path.edges.size() == edgesBefore + 2 && pastedEdges.size() == 2 && idsFresh &&
                  firstPasted != nullptr && std::abs(firstPasted->x - 0.5f) < 1e-5f &&
                  std::abs(firstPasted->z - 0.4f) < 1e-5f &&
                  path.FindEdgeBetween(pastedPoints[0], pastedPoints[1]) != nullptr,
              "PastePathClip は新しい ID で同じ形を、ずらした位置に貼る");
        Check(tg::graph::BuildPathStrands(path).size() == 2,
              "貼った鎖は元の鎖と別の鎖になる");
    }

    Section("ノードグラフ — Path の入力");
    {
        NodeGraph graph;
        const tg::graph::GraphId surfaceId = graph.CreateNode(NodeKind::Surface);
        const tg::graph::GraphId pathId = graph.CreateNode(NodeKind::Path);
        const tg::graph::Node* surface = graph.FindNode(surfaceId);
        const tg::graph::Node* path = graph.FindNode(pathId);
        Check(IsNeutralPlane(graph.CompileLayersTo(pathId)),
              "Path はレイヤー列を持たず、変位 0 の平面になる");

        // Path の入力は Surface（Mesh 型）。材質（Material 型）は繋げない。
        const bool pathRejects =
            surface != nullptr && path != nullptr && !surface->outputs.empty() &&
            !path->inputs.empty() &&
            !graph.CanCreateLink(surface->outputs.front().id, path->inputs.front().id);
        Check(pathRejects && path->inputs.front().valueType == tg::graph::ValueType::Mesh,
              "Path の Surface 入力は Mesh 型で、材質（Material）は繋げない");
    }
}
