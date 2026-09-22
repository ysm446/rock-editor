#pragma once
#include "graph/NodeGraph.h"
#include <map>
#include <memory>

namespace rock::graph {
// GPU合成済みのリニアハイト。ワーカーへは不変スナップショットとして渡す。
struct ScalarField {
    uint32_t width=0,height=0;
    std::vector<float> pixels;
    float Sample(float u,float v,bool wrap) const;
};
struct MaterialHeight {
    struct Surface {
        ScalarField field;
        compositor::MaterialMapping mapping;
        std::array<geometry::Vec3,3> axes;
        uint32_t channels=compositor::kAllChannelBits;
        std::string error;
    };
    std::map<GraphId,Surface> surfaces;
    std::map<GraphId,ScalarField> masks;
    std::map<GraphId,std::string> maskErrors;
    std::map<GraphId,std::string> CacheKeys() const;
    float Sample(GraphId surface, const compositor::MaterialMask* mask, GraphId maskId,
                 geometry::Vec3 position,geometry::Vec3 normal,geometry::Mesh::Uv uv,float below,bool uvWrap,
                 bool heightBlend=false,float heightBlendRange=.2f) const;
    // マスクの重みを、この素材のハイトと下地のハイトの差で寄せる。シェーダの HeightBlendWeight と同じ式。
    static float HeightBlendWeight(float mask,float height,float below,float range);
};
}
