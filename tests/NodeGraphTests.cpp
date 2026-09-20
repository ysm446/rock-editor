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
        const auto a = graph.CreateNode(NodeKind::Merge);
        const auto b = graph.CreateNode(NodeKind::Merge);
        const auto c = graph.CreateNode(NodeKind::Merge);
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

}
