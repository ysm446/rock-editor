#include "renderer/PreviewDiagnostics.h"

#include "core/Log.h"
#include <pix3.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace tg::renderer {
namespace {
constexpr uint32_t ProbeSamples = 33;
constexpr uint32_t ProbeColumns = 12;
constexpr uint32_t MaxProbeEdges = 8192;
}

void PreviewDiagnostics::ResetScene(rhi::Device& device) {
    m_dirty = false;
    for (auto& frame : m_frames) frame.valid = false;
    m_warmup = m_samples = 0;
    m_totalMs = m_minMs = m_maxMs = 0;
    device.DeferRelease(m_probeInput);
    device.DeferRelease(m_probeOutput);
    device.DeferRelease(m_probeReadback);
    m_probeFence = 0;
    m_probeAttempted = false;
}

void PreviewDiagnostics::Shutdown(rhi::Device& device) {
    ResetScene(device);
    device.Defer(m_queries);
    m_queries.Reset();
    device.DeferRelease(m_timestamps);
}

bool PreviewDiagnostics::Initialize(rhi::Device& device) {
    if (m_queries) return true;
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = rhi::kFrameCount * 2;
    if (!TG_CHECK_HR(device.GetCommandQueue()->GetTimestampFrequency(&m_frequency)) || m_frequency == 0 ||
        !TG_CHECK_HR(device.GetDevice()->CreateQueryHeap(&desc, IID_PPV_ARGS(&m_queries))) ||
        !device.Allocator().CreateReadbackBuffer(sizeof(uint64_t) * desc.Count, L"PreviewTimestamps", m_timestamps)) {
        TG_LOG_ERROR("プレビューGPU計測を初期化できませんでした");
        Shutdown(device);
        m_enabled = false;
        return false;
    }
    return true;
}

void PreviewDiagnostics::Begin(rhi::Device& device, ID3D12GraphicsCommandList* commands) {
    m_recording = false;
    if (!m_enabled || !Initialize(device)) return;
    if (m_dirty) ResetScene(device);
    CollectProbe(device);
    auto& frame = m_frames[device.FrameIndex()];
    if (frame.fence > device.CompletedFenceValue()) return;
    if (frame.valid) {
        const size_t offset = device.FrameIndex() * 2 * sizeof(uint64_t);
        const D3D12_RANGE range{offset, offset + 2 * sizeof(uint64_t)};
        void* mapped = nullptr;
        if (TG_CHECK_HR(m_timestamps.resource->Map(0, &range, &mapped))) {
            uint64_t ticks[2];
            std::memcpy(ticks, static_cast<const uint8_t*>(mapped) + offset, sizeof(ticks));
            const D3D12_RANGE written{0, 0};
            m_timestamps.resource->Unmap(0, &written);
            if (ticks[1] >= ticks[0]) {
                const double ms = static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(m_frequency);
                // 初回のパイプライン準備・材質切替直後の値を定常描画の集計へ入れない。
                if (++m_warmup > 8) {
                    if (m_samples == 0) m_minMs = m_maxMs = ms;
                    m_minMs = std::min(m_minMs, ms);
                    m_maxMs = std::max(m_maxMs, ms);
                    m_totalMs += ms;
                    ++m_samples;
                }
            }
        }
    }
    frame.valid = false;
    PIXBeginEvent(commands, PIX_COLOR(120, 180, 220), "PreviewGpuTiming");
    commands->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, device.FrameIndex() * 2);
    m_recording = true;
}

void PreviewDiagnostics::End(rhi::Device& device, ID3D12GraphicsCommandList* commands, bool ready,
                             uint32_t width, uint32_t height, bool tessellation) {
    if (!m_recording) return;
    const uint32_t slot = device.FrameIndex();
    commands->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    commands->ResolveQueryData(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                              m_timestamps.resource.Get(), slot * 2 * sizeof(uint64_t));
    m_frames[slot] = {device.NextFenceValue(), ready};
    PIXEndEvent(commands);
    m_recording = false;
    if (m_samples >= 30) {
        const auto memory = device.QueryVideoMemory();
        TG_LOG_INFO("プレビューGPU: %u samples, %ux%u, tess=%u, mean=%.3f ms, min=%.3f ms, max=%.3f ms, VRAM usage=%llu allocated=%llu bytes",
                    m_samples, width, height, tessellation ? 1u : 0u, m_totalMs / m_samples, m_minMs, m_maxMs,
                    static_cast<unsigned long long>(memory.usage), static_cast<unsigned long long>(memory.allocated));
        m_samples = 0;
        m_totalMs = 0;
    }
}

bool PreviewDiagnostics::PrepareProbe(rhi::Device& device, ID3D12GraphicsCommandList* commands, const SceneMesh& mesh) {
    m_probeEdges = static_cast<uint32_t>(std::min(mesh.connectionSeams.size(), size_t(MaxProbeEdges)));
    if (m_probeEdges == 0) return false;
    rhi::TextureDesc desc;
    desc.width = ProbeColumns;
    desc.height = m_probeEdges + 1;
    desc.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    desc.debugName = L"ConnectionProbeInput";
    if (!device.Allocator().CreateTexture2D(desc, m_probeInput)) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT inputFootprint{};
    uint64_t inputBytes = 0;
    auto resourceDesc = m_probeInput.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &inputFootprint, nullptr, nullptr, &inputBytes);
    const auto upload = device.Upload().Allocate(inputBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!upload.IsValid()) return false;
    std::memset(upload.cpu, 0, static_cast<size_t>(inputBytes));
    for (uint32_t edge = 0; edge <= m_probeEdges; ++edge) {
        const bool control = edge == m_probeEdges;
        const size_t selected = control ? 0 : size_t(edge) * mesh.connectionSeams.size() / m_probeEdges;
        const auto& seam = mesh.connectionSeams[selected];
        auto* row = reinterpret_cast<DirectX::XMFLOAT4*>(static_cast<uint8_t*>(upload.cpu) + size_t(edge) * inputFootprint.Footprint.RowPitch);
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const auto& v = mesh.geometry.vertices[seam[vertex]];
            row[vertex] = {v.position.x, v.position.y, v.position.z, 0};
            // 最後の一辺だけ入力段階で片側を1mmずらす。両側の読み違いも検出する。
            if (control && vertex >= 2) row[vertex].y += 0.001f;
            row[vertex + 4] = {v.roadUv.x, v.roadUv.y, 0, 0};
            row[vertex + 8] = {v.normal.x, v.normal.y, v.normal.z, 0};
        }
    }
    inputFootprint.Offset = upload.offset;
    const CD3DX12_TEXTURE_COPY_LOCATION source(upload.resource, inputFootprint);
    const CD3DX12_TEXTURE_COPY_LOCATION destination(m_probeInput.resource.Get(), 0);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    rhi::TransitionIfNeeded(commands, m_probeInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    desc.width = ProbeSamples;
    desc.allowUnorderedAccess = true;
    desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    desc.debugName = L"ConnectionProbeOutput";
    if (!device.Allocator().CreateTexture2D(desc, m_probeOutput)) return false;
    resourceDesc = m_probeOutput.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &m_probeFootprint, nullptr, nullptr, &m_probeBytes);
    TG_LOG_INFO("接続GPU検査: %u / %zu edges, %u samples per edge", m_probeEdges, mesh.connectionSeams.size(), ProbeSamples);
    return device.Allocator().CreateReadbackBuffer(m_probeBytes, L"ConnectionProbeReadback", m_probeReadback);
}

void PreviewDiagnostics::Probe(rhi::Device& device, rhi::PipelineCache& pipelines, ID3D12GraphicsCommandList* commands,
                               const SceneMesh& mesh, D3D12_GPU_VIRTUAL_ADDRESS constants) {
    if (!m_enabled || m_probeAttempted || mesh.connectionSeams.empty() || constants == 0) return;
    m_probeAttempted = true;
    auto* pipeline = pipelines.GetCompute(L"MeshPbr.hlsl", L"CsConnectionProbe");
    PIXBeginEvent(commands, PIX_COLOR(220, 180, 100), "ConnectionSeamProbe");
    if (pipeline && PrepareProbe(device, commands, mesh)) {
        const uint32_t parameters[] = {m_probeInput.SrvIndex(), m_probeOutput.UavIndex(), m_probeEdges + 1, ProbeSamples};
        commands->SetComputeRootSignature(pipelines.GlobalRootSignature());
        commands->SetPipelineState(pipeline);
        commands->SetComputeRootConstantBufferView(1, constants);
        commands->SetComputeRoot32BitConstants(0, 4, parameters, 0);
        commands->Dispatch(rhi::DispatchCount(ProbeSamples), rhi::DispatchCount(m_probeEdges + 1), 1);
        rhi::TransitionIfNeeded(commands, m_probeOutput, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION source(m_probeOutput.resource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(m_probeReadback.resource.Get(), m_probeFootprint);
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        m_probeFence = device.NextFenceValue();
    } else {
        TG_LOG_ERROR("接続GPU検査を準備できませんでした（未検証）");
    }
    PIXEndEvent(commands);
}

void PreviewDiagnostics::CollectProbe(rhi::Device& device) {
    if (m_probeFence == 0 || device.CompletedFenceValue() < m_probeFence) return;
    m_probeFence = 0;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<size_t>(m_probeBytes)};
    if (!TG_CHECK_HR(m_probeReadback.resource->Map(0, &range, &mapped))) return;
    double maxError = 0;
    uint32_t failures = 0, invalid = 0;
    for (uint32_t edge = 0; edge <= m_probeEdges; ++edge) {
        const auto* row = reinterpret_cast<const DirectX::XMFLOAT4*>(static_cast<const uint8_t*>(mapped) +
            m_probeFootprint.Offset + size_t(edge) * m_probeFootprint.Footprint.RowPitch);
        for (uint32_t sample = 0; sample < ProbeSamples; ++sample) {
            const auto& value = row[sample];
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) ||
                !std::isfinite(value.w) || value.w < 0.0008f || value.w > 0.0012f) { ++invalid; continue; }
            if (edge == m_probeEdges) {
                if (value.x < 0.0008f || value.x > 0.0012f) ++invalid;
                continue;
            }
            maxError = std::max(maxError, static_cast<double>(value.x));
            failures += value.x > 0.0001f ? 1u : 0u;
        }
    }
    const D3D12_RANGE written{0, 0};
    m_probeReadback.resource->Unmap(0, &written);
    TG_LOG_INFO("接続GPU検査: %u points, max=%.6f mm, over0.1mm=%u, invalid/controlFailures=%u (sampled displacement only)",
                m_probeEdges * ProbeSamples, maxError * 1000, failures, invalid);
    if (failures || invalid) TG_LOG_ERROR("接続GPU検査が許容誤差を超えました");
    // 定常描画のVRAM集計に検査用の一時テクスチャを残さない。
    device.DeferRelease(m_probeInput);
    device.DeferRelease(m_probeOutput);
    device.DeferRelease(m_probeReadback);
}

}  // namespace tg::renderer
