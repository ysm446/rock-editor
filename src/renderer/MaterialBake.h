#pragma once
#include "renderer/MeshData.h"
#include "compositor/MaterialEvaluator.h"
#include "core/ImageIo.h"
#include "renderer/BakePadding.h"
#include <array>
namespace rock::renderer {
// フレーム外で実行。照明なしのBaseColor(sRGB)、Normal(DirectX)、R/M/AO、Height。
bool BakeMaterial(rhi::Device &device, rhi::PipelineCache &pipelines, const SceneMesh &mesh,
                  const compositor::TextureLibrary &textures, const compositor::MaterialLibrary &materials,
                  uint32_t width, uint32_t height, std::array<LdrImage, 4> &images, std::string &error);
} // namespace rock::renderer
