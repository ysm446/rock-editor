#pragma once

#include "renderer/MeshData.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <array>

namespace tg::renderer {

// 開発用の明示的な計測。読み戻しはフェンス完了後だけ行い、通常表示では確保しない。
class PreviewDiagnostics {
public:
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    void Invalidate() { m_dirty = true; }
    void ResetScene(rhi::Device& device);
    void Shutdown(rhi::Device& device);
    void Begin(rhi::Device& device, ID3D12GraphicsCommandList* commands);
    void End(rhi::Device& device, ID3D12GraphicsCommandList* commands, bool ready,
             uint32_t width, uint32_t height, bool tessellation);
    void Probe(rhi::Device& device, rhi::PipelineCache& pipelines, ID3D12GraphicsCommandList* commands,
               const SceneMesh& mesh, D3D12_GPU_VIRTUAL_ADDRESS constants);

private:
    bool Initialize(rhi::Device& device);
    bool PrepareProbe(rhi::Device& device, ID3D12GraphicsCommandList* commands, const SceneMesh& mesh);
    void CollectProbe(rhi::Device& device);
    struct Frame {
        uint64_t fence = 0;
        bool valid = false;
    };
    bool m_enabled = false;
    bool m_dirty = false;
    bool m_recording = false;
    rhi::ComPtr<ID3D12QueryHeap> m_queries;
    rhi::GpuBuffer m_timestamps;
    uint64_t m_frequency = 0;
    std::array<Frame, rhi::kFrameCount> m_frames{};
    uint32_t m_warmup = 0, m_samples = 0;
    double m_totalMs = 0, m_minMs = 0, m_maxMs = 0;
    rhi::GpuTexture m_probeInput, m_probeOutput;
    rhi::GpuBuffer m_probeReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_probeFootprint{};
    uint64_t m_probeBytes = 0, m_probeFence = 0;
    uint32_t m_probeEdges = 0;
    bool m_probeAttempted = false;
};

}  // namespace tg::renderer
