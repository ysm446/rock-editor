#pragma once

#include "graph/SurfaceLayout.h"
#include "graph/Road.h"

namespace tg::graph {
struct SurfaceParameterSample {
    SurfaceId parameter = 0;
    float value = 0;
};
struct SurfaceSpanSample {
    SurfaceId span = 0, preset = 0;
    float weight = 0;
    std::vector<SurfaceParameterSample> parameters;
};
// 検証済み文書の帯を実距離で評価する。空白区間・範囲外は空。最大2区間。
// 移行は両区間の半分までに制限し、短い区間の前後の移行が交差しないようにする。
std::vector<SurfaceSpanSample> SampleSurfaceBand(const SurfaceLayoutDocument& document,
                                                const SurfaceBand& band, float distanceMeters);
// P1の道路本体だけを確認する開発用プレビュー。最大3種・各4素材、道路全長を覆う区間列。
// 沿道断面・公開値の結線は後続。通常のグラフ評価は置き換えない。
CompiledMeshGraph CompileSurfaceLayoutPreview(const NodeGraph& graph, const SurfaceLayoutDocument& document,
                                             GraphId roadId);
CompiledMeshGraph CompileMeshGraphWithLayouts(const NodeGraph& graph, const SurfaceLayoutDocument& document,
                                            GraphId previewNodeId = 0);
}  // namespace tg::graph
