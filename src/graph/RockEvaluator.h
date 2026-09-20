#pragma once
#include <string>

#include "crack/PartialCut.h"
#include "geometry/Mesh.h"
#include "graph/NodeGraph.h"
namespace rock::graph {
struct GeneratedRock {
    GraphId source = 0;
    geometry::Mesh mesh;
    std::optional<std::array<float, 3>> uncutBox;
};
struct GeneratedCrack {
    GraphId source = 0;
    crack::CrackPatch patch;
};
struct GeneratedCut {
    GraphId source = 0;
    std::optional<crack::RockBridge> bridge;
    float penetration = 0;
    double removedVolume = 0;
    bool showBridge = true;
    std::string status;
};
struct RockEvaluation {
    std::vector<GeneratedCut> cuts;
    bool hasModels = false;
    std::vector<GeneratedCrack> cracks;
    std::vector<GeneratedRock> rocks;
    std::string error;
};
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview = 0);
}  // namespace rock::graph
