#pragma once

#include "graph/Road.h"

namespace tg::graph {

// P0 のみ。直線・水平な道路を共通の断面で置き換え、二方向の素材境界を評価する。
// 編集データや旧 Road / Shoulder は変更しない。接続先は Surface / Road / Shoulder の材質構成を使う。
struct ConnectionPrototypeSettings {
    float shoulderWidth = 2.0f;
    float sidewalkWidth = 2.0f;
    float sidewalkHeight = 0.15f;
    float slabLength = 1.5f;
    float jointWidth = 0.012f;
    float jointDepth = 0.004f;
    float lateralBlend = 0.8f;
    float transitionCenter = 12.0f;
    float transitionLength = 8.0f;
    float sampleSpacing = 0.25f;
    uint32_t seed = 17;
};

// 戻り値は砂利の被覆率。両面に同じ境界関数を使い、残りが舗装になる。
float PrototypeGravelCoverage(float across, float distance, const ConnectionPrototypeSettings& settings);
bool BuildConnectionPrototype(const RoadGeometry& road, const ConnectionPrototypeSettings& settings,
                              renderer::SceneMesh& result, std::string& error);
CompiledMeshGraph CompileConnectionPrototype(const NodeGraph& graph, GraphId roadId,
                                             GraphId gravelSurfaceId, GraphId sidewalkSurfaceId,
                                             bool displacement = true);

}  // namespace tg::graph
