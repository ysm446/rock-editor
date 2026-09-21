#pragma once
#include "graph/RockEvaluator.h"
#include <functional>
namespace rock::graph {
RockEvaluation EvaluatePieceNode(const NodeGraph &, const Node &, RockEvaluationCache *,
                                 const std::function<RockEvaluation(GraphId)> &, std::stop_token = {});
void PreparePiecePreview(RockEvaluation &, GraphId);
} // namespace rock::graph
