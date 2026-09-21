#pragma once
#include "geometry/Mesh.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"
#include "core/ImageIo.h"

namespace rock::renderer {
// フレーム外で小分けに実行する。レイ判定はGPU、BVH構築はCPU。
class OcclusionBake {
public:
    bool Create(rhi::Device&, const geometry::Mesh&, float distance, int samples, float strength, std::string& error);
    bool Step(rhi::Device&, rhi::PipelineCache&, std::string& error);
    bool Read(rhi::Device&, LdrImage& image);
    void Release(rhi::Device&);
    float Progress() const { return m_tiles ? float(m_completed) / float(m_tiles) : 0; }
    bool Complete() const { return m_tiles && m_completed == m_tiles; }
private:
    rhi::GpuBuffer m_triangles, m_nodes;
    rhi::GpuTexture m_output;
    uint32_t m_uvRoot = 0, m_completed = 0, m_tiles = 0;
    float m_distance = 0, m_strength = 1, m_bias = 0;
    uint32_t m_samples = 32;
};
}
