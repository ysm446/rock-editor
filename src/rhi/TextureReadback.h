#pragma once

#include "rhi/Device.h"

#include <filesystem>

namespace rock::rhi {

// RGBA8 のテクスチャを読み戻して PNG へ保存する。GPU 待機を伴うので**フレームの外で呼ぶこと。**
// maxSize を指定すると、縦横比を保ってその大きさ以下へ縮小する（0 で等倍）。
bool SaveTextureToPng(Device& device, GpuTexture& texture, const std::filesystem::path& path,
                      uint32_t maxSize = 0);

}  // namespace rock::rhi
