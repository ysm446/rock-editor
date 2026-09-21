#pragma once
#include <string>
#include <memory>
#include <map>

#include "crack/PartialCut.h"
#include "geometry/Mesh.h"
#include "graph/NodeGraph.h"
namespace rock::graph {
struct GeneratedRock {
    GraphId source = 0;
    geometry::Mesh mesh;
    std::optional<std::array<float, 3>> uncutBox;
    // (source, chunk) は表示用 ID。0 は未分割。単一平面では1が負側、2が正側。
    int chunk = 0;
    GraphId parent = 0;
    bool locked = false;
    geometry::Vec3 pivot;
    std::string key;  // 多片の変換設定を結び付ける半空間のキー。
    std::shared_ptr<const std::vector<geometry::OrientedBox>> boxes;
    std::shared_ptr<const geometry::VolumeGrid> volume;
};
struct GeneratedCrack {
    GraphId source = 0;
    crack::CrackPatch patch;
    int index = 0;  // 同じ Joint Set 内のパッチ番号。Crack は0。
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
    int negative = 1, positive = 2;
    GraphId jointSource = 0;
    int patchIndex = 0;
};
struct RockEvaluation {
    std::vector<GeneratedFracture> fractures;
    std::vector<GeneratedCut> cuts;
    bool hasModels = false;
    std::vector<GeneratedCrack> cracks;
    std::vector<GeneratedCrack> jointPlanes;  // showGuide に依存しない節理の定義。
    std::vector<GeneratedRock> rocks;
    std::string error;
};
// ボリューム系の枝を設定と接続の内容で再利用する。アプリ単位で保持する。
struct RockEvaluationCache {
    struct Entry { std::string key; RockEvaluation result; };
    std::map<GraphId, Entry> entries;
    struct Surface {
        std::shared_ptr<const geometry::VolumeGrid> volume;
        geometry::Mesh mesh;
    };
    std::map<GraphId, Surface> surfaces;
};
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview = 0,
                            RockEvaluationCache* persistent = nullptr);
}  // namespace rock::graph
