#pragma once
#include <atomic>
#include <string>
#include <memory>
#include <map>

#include "geometry/Mesh.h"
#include "graph/NodeGraph.h"
namespace rock::graph {
struct GeneratedRock {
    GraphId source = 0;
    geometry::Mesh mesh;
    std::shared_ptr<const std::vector<geometry::OrientedBox>> boxes;
    std::shared_ptr<const geometry::VolumeGrid> volume;
    struct MaterialBinding {
        GraphId surface = 0, mask = 0;
        bool operator==(const MaterialBinding&) const = default;
    };
    std::vector<MaterialBinding> materials;
    GraphId materialSource = 0;
    GraphId bakeSource = 0;
    int pieceId = -1;
    bool pieceSelected = false;
};
struct RockEvaluation {
    std::shared_ptr<const geometry::PointSet> points;
    std::shared_ptr<const geometry::PieceCollection> pieces;
    std::shared_ptr<const geometry::PieceSelection> selection;
    bool hasModels = false;
    std::vector<GeneratedRock> rocks;
    std::string error;
};
// ボリューム系の枝を設定と接続の内容で再利用する。アプリ単位で保持する。
struct RockEvaluationCache {
    struct Entry { std::string key; RockEvaluation result; };
    std::map<GraphId, Entry> entries;
    std::map<GraphId, Entry> pieceEntries;
    struct Surface {
        std::shared_ptr<const geometry::VolumeGrid> volume;
        geometry::Mesh mesh;
        geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra;
    };
    std::map<GraphId, Surface> surfaces;
    struct UvEntry { geometry::Mesh input, output; geometry::UvUnwrapSettings settings; };
    std::map<GraphId, UvEntry> uvs;
};
// 別スレッドで走る評価が、いま計算しているノードと段階を UI へ伝える。UI は読むだけ。
struct RockEvaluationProgress {
    std::atomic<GraphId> node{0};
    // ノードの中の段階（UV Unwrap は geometry::UvUnwrapStage + 1）。段階を持たないノードは 0。
    std::atomic<int> stage{0};
    std::atomic<int> percent{-1};  // 0～100。分からないときは -1。
};
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview = 0,
                            RockEvaluationCache* persistent = nullptr,
                            geometry::VolumeMeshingMethod previewMethod = geometry::VolumeMeshingMethod::MarchingTetrahedra,
                            std::stop_token stop = {}, RockEvaluationProgress* progress = nullptr);
}  // namespace rock::graph
