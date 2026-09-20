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
    // (source, chunk) が生成片の ID。0 は未分割、1 は負側、2 は正側。
    int chunk = 0;
    GraphId parent = 0;
    bool locked = false;
    geometry::Vec3 pivot;
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
struct GeneratedFracture {
    GraphId source = 0, parent = 0;
    // この2片は元の分割面を共有する。現在の接触・拘束を意味しない。
    geometry::Vec3 center, normal;
    double sectionArea = 0;
};
struct RockEvaluation {
    std::vector<GeneratedFracture> fractures;
    std::vector<GeneratedCut> cuts;
    bool hasModels = false;
    std::vector<GeneratedCrack> cracks;
    std::vector<GeneratedRock> rocks;
    std::string error;
};
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview = 0);
}  // namespace rock::graph
