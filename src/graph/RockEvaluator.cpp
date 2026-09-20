#include "graph/RockEvaluator.h"

#include <unordered_set>
namespace rock::graph {
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview) {
    RockEvaluation result;
    std::unordered_set<GraphId> visited;
    std::vector<GraphId> pending;
    if (preview != 0)
        pending.push_back(preview);
    else
        for (const auto& node : graph.Nodes())
            if (node.kind == NodeKind::MeshOutput) pending.push_back(node.id);
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto* node = graph.FindNode(id);
        if (!node || !visited.insert(id).second) continue;
        if (node->kind == NodeKind::BaseRock) {
            const auto* settings = std::get_if<BaseRockNodeSettings>(&node->settings);
            geometry::Mesh mesh = settings ? geometry::MakeBox(settings->size) : geometry::Mesh{};
            geometry::MeshInfo info;
            if (!geometry::InspectMesh(mesh, info) || !info.closed || info.components != 1 ||
                info.volume <= 0) {
                result.rocks.clear();
                result.cracks.clear();
                result.error =
                    "Base Rock #" + std::to_string(id) + ": 寸法は有限の 0.001～1000 m にしてください";
                return result;
            }
            result.rocks.push_back({id, std::move(mesh)});
        } else if (node->kind == NodeKind::Crack) {
            const auto* settings = std::get_if<crack::CrackSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            crack::CrackPatch patch;
            std::string error;
            if (!settings || !upstream || !crack::BuildCrackPatch(*settings, patch, error)) {
                result.rocks.clear();
                result.cracks.clear();
                result.error = "Crack #" + std::to_string(id) + ": " +
                               (!upstream ? "Mesh 入力を接続してください" : error);
                return result;
            }
            if (settings->showGuide) result.cracks.push_back({id, patch});
            pending.push_back(upstream->id);
        } else if (node->kind == NodeKind::Merge || node->kind == NodeKind::MeshOutput) {
            for (auto it = node->inputs.rbegin(); it != node->inputs.rend(); ++it)
                if (const auto* upstream = graph.FindUpstreamNodeForPin(it->id))
                    pending.push_back(upstream->id);
        }
    }
    return result;
}
}  // namespace rock::graph
