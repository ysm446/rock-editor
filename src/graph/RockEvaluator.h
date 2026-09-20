#pragma once
#include <string>

#include "geometry/Mesh.h"
#include "graph/NodeGraph.h"
namespace rock::graph {
struct GeneratedRock {
    GraphId source = 0;
    geometry::Mesh mesh;
};
struct GeneratedCrack {
    GraphId source = 0;
    crack::CrackPatch patch;
};
struct RockEvaluation {
    std::vector<GeneratedCrack> cracks;
    std::vector<GeneratedRock> rocks;
    std::string error;
};
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview = 0);
}  // namespace rock::graph
