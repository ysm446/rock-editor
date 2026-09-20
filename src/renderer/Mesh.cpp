#include "renderer/Mesh.h"

#include "core/Log.h"

#include <cstring>
#include <pix3.h>

using namespace DirectX;

namespace tg::renderer {

bool Mesh::Create(rhi::Device& device, const MeshData& data, const wchar_t* debugName) {
    if (data.vertices.empty() || data.indices.empty()) {
        return false;
    }

    const uint64_t vertexBytes = data.vertices.size() * sizeof(MeshVertex);
    const uint64_t indexBytes = data.indices.size() * sizeof(uint32_t);
    // 外周の辺。頂点バッファを共有し、インデックスだけ別に持つ。
    const std::vector<uint32_t> outline = MeshOutlineEdges(data);
    const uint64_t outlineBytes = outline.size() * sizeof(uint32_t);

    rhi::ResourceAllocator& allocator = device.Allocator();
    if (!allocator.CreateDefaultBuffer(vertexBytes, D3D12_RESOURCE_STATE_COMMON, debugName,
                                       m_vertexBuffer)) {
        return false;
    }
    if (!allocator.CreateDefaultBuffer(indexBytes, D3D12_RESOURCE_STATE_COMMON, debugName,
                                       m_indexBuffer)) {
        return false;
    }
    if (outlineBytes > 0 && !allocator.CreateDefaultBuffer(outlineBytes, D3D12_RESOURCE_STATE_COMMON,
                                                           debugName, m_outlineIndexBuffer)) {
        return false;
    }

    // 初期化時の一度きりの転送なので、専用のステージングバッファを使って即実行する。
    rhi::GpuBuffer staging;
    if (!allocator.CreateUploadBuffer(vertexBytes + indexBytes + outlineBytes, L"MeshStaging", staging)) {
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &readRange, &mapped))) {
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(mapped);
    std::memcpy(bytes, data.vertices.data(), vertexBytes);
    std::memcpy(bytes + vertexBytes, data.indices.data(), indexBytes);
    if (outlineBytes > 0) std::memcpy(bytes + vertexBytes + indexBytes, outline.data(), outlineBytes);
    staging.resource->Unmap(0, nullptr);

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(80, 160, 220), "UploadMesh");
        const D3D12_RESOURCE_BARRIER toCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffer.resource.Get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(m_indexBuffer.resource.Get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        commandList->ResourceBarrier(_countof(toCopy), toCopy);
        commandList->CopyBufferRegion(m_vertexBuffer.resource.Get(), 0, staging.resource.Get(), 0,
                                      vertexBytes);
        commandList->CopyBufferRegion(m_indexBuffer.resource.Get(), 0, staging.resource.Get(),
                                      vertexBytes, indexBytes);

        const D3D12_RESOURCE_BARRIER barriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffer.resource.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            CD3DX12_RESOURCE_BARRIER::Transition(m_indexBuffer.resource.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_INDEX_BUFFER),
        };
        commandList->ResourceBarrier(_countof(barriers), barriers);
        if (outlineBytes > 0) {
            const auto toCopyOutline = CD3DX12_RESOURCE_BARRIER::Transition(
                m_outlineIndexBuffer.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            commandList->ResourceBarrier(1, &toCopyOutline);
            commandList->CopyBufferRegion(m_outlineIndexBuffer.resource.Get(), 0, staging.resource.Get(),
                                          vertexBytes + indexBytes, outlineBytes);
            const auto toIndex = CD3DX12_RESOURCE_BARRIER::Transition(
                m_outlineIndexBuffer.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
            commandList->ResourceBarrier(1, &toIndex);
        }
        PIXEndEvent(commandList);
    });
    if (!executed) {
        return false;
    }

    m_vertexBuffer.state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    m_indexBuffer.state = D3D12_RESOURCE_STATE_INDEX_BUFFER;

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GpuAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
    m_vertexBufferView.StrideInBytes = sizeof(MeshVertex);

    m_indexBufferView.BufferLocation = m_indexBuffer.GpuAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    if (outlineBytes > 0) {
        m_outlineIndexBuffer.state = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        m_outlineIndexBufferView.BufferLocation = m_outlineIndexBuffer.GpuAddress();
        m_outlineIndexBufferView.SizeInBytes = static_cast<UINT>(outlineBytes);
        m_outlineIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    }

    m_vertexCount = static_cast<uint32_t>(data.vertices.size());
    m_indexCount = static_cast<uint32_t>(data.indices.size());
    m_outlineIndexCount = static_cast<uint32_t>(outline.size());
    return true;
}

void Mesh::Release(rhi::Device& device) {
    if (m_vertexBuffer.IsValid()) {
        device.Defer(m_vertexBuffer.resource);
        device.Defer(m_vertexBuffer.allocation);
    }
    if (m_indexBuffer.IsValid()) {
        device.Defer(m_indexBuffer.resource);
        device.Defer(m_indexBuffer.allocation);
    }
    if (m_outlineIndexBuffer.IsValid()) {
        device.Defer(m_outlineIndexBuffer.resource);
        device.Defer(m_outlineIndexBuffer.allocation);
    }
    m_vertexBuffer = rhi::GpuBuffer{};
    m_indexBuffer = rhi::GpuBuffer{};
    m_outlineIndexBuffer = rhi::GpuBuffer{};
    m_indexCount = 0;
    m_outlineIndexCount = 0;
    m_vertexCount = 0;
}

void Mesh::Draw(ID3D12GraphicsCommandList* commandList, bool asPatches) const {
    if (m_indexCount == 0) {
        return;
    }
    commandList->IASetPrimitiveTopology(asPatches
                                            ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                            : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    commandList->IASetIndexBuffer(&m_indexBufferView);
    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}

void Mesh::DrawOutline(ID3D12GraphicsCommandList* commandList) const {
    if (m_outlineIndexCount == 0) {
        return;
    }
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    commandList->IASetIndexBuffer(&m_outlineIndexBufferView);
    commandList->DrawIndexedInstanced(m_outlineIndexCount, 1, 0, 0, 0);
}

}  // namespace tg::renderer
