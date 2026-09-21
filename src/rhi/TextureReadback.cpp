#include "rhi/TextureReadback.h"

#include "core/ImageIo.h"
#include "core/Log.h"

#include <pix3.h>

#include <algorithm>
#include <vector>
#include <cstring>

namespace rock::rhi {
bool ReadTextureRgba8(Device& device, GpuTexture& texture, LdrImage& image) {
    image = {};
    if (!texture.IsValid() || texture.format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    const auto desc = texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    GpuBuffer buffer;
    if (!device.Allocator().CreateReadbackBuffer(bytes,L"BakeReadback",buffer)) return false;
    const auto state=texture.state;
    const bool copied=device.ExecuteImmediate([&](auto* list) {
        TransitionIfNeeded(list,texture,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION dst(buffer.resource.Get(),footprint), src(texture.resource.Get(),0);
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        TransitionIfNeeded(list,texture,state);
    });
    void* mapped=nullptr;
    const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
    bool success=copied && SUCCEEDED(buffer.resource->Map(0,&range,&mapped));
    if (success) {
        image.width=texture.width; image.height=texture.height;
        image.pixels.resize(size_t(image.width)*image.height*4);
        for (uint32_t y=0;y<image.height;++y)
            std::memcpy(image.pixels.data()+size_t(y)*image.width*4,
                static_cast<const uint8_t*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,size_t(image.width)*4);
        const D3D12_RANGE written{0,0}; buffer.resource->Unmap(0,&written);
    }
    device.DeferRelease(buffer);
    return success;
}

bool SaveTextureToPng(Device& device, GpuTexture& texture, const std::filesystem::path& path,
                      uint32_t maxSize) {
    if (!texture.IsValid() || texture.format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    const auto desc = texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
    GpuBuffer readback;
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"TextureReadback", readback)) return false;
    const auto previous = texture.state;
    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        PIXBeginEvent(list, PIX_COLOR_DEFAULT, "TextureReadback");
        TransitionIfNeeded(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION src(texture.resource.Get(), 0);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        TransitionIfNeeded(list, texture, previous);
        PIXEndEvent(list);
    });
    if (!executed) { device.DeferRelease(readback); return false; }
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(totalBytes)};
    if (!ROCK_CHECK_HR(readback.resource->Map(0, &range, &mapped))) { device.DeferRelease(readback); return false; }
    const auto* source = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    const float scale = maxSize ? std::min(1.0f, float(maxSize) / float(std::max(texture.width, texture.height))) : 1.0f;
    const uint32_t width = std::max(1u, uint32_t(texture.width * scale));
    const uint32_t height = std::max(1u, uint32_t(texture.height * scale));
    bool saved = false;
    if (width == texture.width && height == texture.height) {
        saved = SaveRgba8Png(path, width, height, footprint.Footprint.RowPitch, source);
    } else {
        // 箱型フィルタで縮小する。サムネイル用なので品質より単純さを優先する。
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            const uint32_t x0 = uint32_t(uint64_t(x) * texture.width / width);
            const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * texture.width / width));
            const uint32_t y0 = uint32_t(uint64_t(y) * texture.height / height);
            const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * texture.height / height));
            uint64_t sum[4]{};
            for (uint32_t sy = y0; sy < y1; ++sy) for (uint32_t sx = x0; sx < x1; ++sx)
                for (size_t c = 0; c < 4; ++c) sum[c] += source[size_t(sy) * footprint.Footprint.RowPitch + sx * 4 + c];
            const uint64_t count = uint64_t(x1 - x0) * (y1 - y0);
            for (size_t c = 0; c < 4; ++c) pixels[(size_t(y) * width + x) * 4 + c] = uint8_t(sum[c] / count);
        }
        saved = SaveRgba8Png(path, width, height, width * 4, pixels.data());
    }
    const D3D12_RANGE written{0, 0};
    readback.resource->Unmap(0, &written);
    device.DeferRelease(readback);
    return saved;
}

}  // namespace rock::rhi
