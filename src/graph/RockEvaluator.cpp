#include "graph/RockEvaluator.h"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_set>
namespace rock::graph {
namespace {
RockEvaluation Failure(GraphId id, const char* kind, const std::string& message) {
    RockEvaluation result;
    result.error = std::string(kind) + " #" + std::to_string(id) + ": " + message;
    return result;
}
// 同じ生成結果へ合流したときだけ重複を除く。元の Box と加工した枝は別の source を持つ。
void Append(RockEvaluation& target, const RockEvaluation& source) {
    const auto append = [](auto& dst, const auto& src) {
        for (const auto& item : src)
            if (std::none_of(dst.begin(), dst.end(),
                             [&](const auto& other) { return other.source == item.source; }))
                dst.push_back(item);
    };
    append(target.rocks, source.rocks);
    append(target.cracks, source.cracks);
    append(target.cuts, source.cuts);
    target.hasModels |= source.hasModels;
}
}  // namespace
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview) {
    // 評価一回の中で共有上流を再利用する。永続的な枝キャッシュは P6。
    std::map<GraphId, RockEvaluation> cache;
    std::unordered_set<GraphId> active;
    std::function<RockEvaluation(GraphId, size_t)> evaluate;
    evaluate = [&](GraphId id, size_t depth) -> RockEvaluation {
        if (const auto found = cache.find(id); found != cache.end()) return found->second;
        if (depth > 256 || active.contains(id))
            return Failure(id, "Graph", "循環または評価深さの上限を検出しました");
        const auto* node = graph.FindNode(id);
        if (!node) return {};
        active.insert(id);
        RockEvaluation result;
        const auto finish = [&](RockEvaluation value) {
            active.erase(id);
            cache[id] = value;
            return value;
        };
        if (node->kind == NodeKind::BaseRock) {
            const auto* settings = std::get_if<BaseRockNodeSettings>(&node->settings);
            auto mesh = settings ? geometry::MakeBox(settings->size) : geometry::Mesh{};
            geometry::MeshInfo info;
            if (!geometry::InspectMesh(mesh, info) || !info.closed || info.components != 1 ||
                info.volume <= 0)
                return finish(Failure(id, "Base Rock", "寸法は有限の 0.001～1000 m にしてください"));
            result.rocks.push_back({id, std::move(mesh), settings->size});
        } else if (node->kind == NodeKind::Crack) {
            const auto* settings = std::get_if<crack::CrackSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            crack::CrackPatch patch;
            std::string error;
            if (!settings || !upstream || !crack::BuildCrackPatch(*settings, patch, error))
                return finish(Failure(id, "Crack", !upstream ? "Mesh 入力を接続してください" : error));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (settings->applyCut) {
                if (result.hasModels || result.rocks.size() != 1 || !result.rocks.front().uncutBox)
                    return finish(
                        Failure(id, "Crack", "部分切断は未加工の Box 1 個・切り込み1回のみ対応します"));
                auto cut = crack::CutBox(*result.rocks.front().uncutBox, *settings);
                if (!cut.error.empty()) return finish(Failure(id, "Crack", cut.error));
                if (cut.bridge) {
                    result.rocks.front() = {id, std::move(cut.mesh), std::nullopt};
                }
                result.cuts.push_back(
                    {id, cut.bridge, cut.penetration, cut.removedVolume, settings->showBridge, cut.status});
            }
            if (settings->showGuide) result.cracks.push_back({id, patch});
        } else if (node->kind == NodeKind::Model || node->kind == NodeKind::Transform) {
            result.hasModels = true;
        } else if (node->kind == NodeKind::Merge || node->kind == NodeKind::MeshOutput) {
            for (const auto& pin : node->inputs)
                if (const auto* upstream = graph.FindUpstreamNodeForPin(pin.id)) {
                    const auto input = evaluate(upstream->id, depth + 1);
                    if (!input.error.empty()) return finish(input);
                    Append(result, input);
                }
        }
        return finish(result);
    };
    if (preview != 0) return evaluate(preview, 0);
    RockEvaluation result;
    for (const auto& node : graph.Nodes())
        if (node.kind == NodeKind::MeshOutput) {
            const auto input = evaluate(node.id, 0);
            if (!input.error.empty()) return input;
            Append(result, input);
        }
    return result;
}
}  // namespace rock::graph
