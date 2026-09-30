#include "TestSupport.h"
#include "core/ImageIo.h"
#include "core/PathUtf8.h"
#include "geometry/Terrain.h"
#include "geometry/UvUnwrap.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <filesystem>

using namespace rock::tests;
using namespace rock;

void RunTerrainTests() {
    Section("地形（Heightmap）");
    geometry::HeightmapSettings settings;
    settings.resolution = 32;
    std::string error;
    const auto heights = geometry::MakeNoiseHeights(settings, 33);
    float low = 1, high = 0;
    for (const float value : heights.values) {
        low = std::min(low, value);
        high = std::max(high, value);
    }
    Check(heights.width == 33 && heights.height == 33 && std::abs(low) < 1e-5f && std::abs(high - 1) < 1e-5f,
          "ノイズの高さは 0〜1 に伸ばしてある");
    const auto again = geometry::MakeNoiseHeights(settings, 33);
    auto other = settings;
    other.seed = 2;
    Check(again.values == heights.values && geometry::MakeNoiseHeights(other, 33).values != heights.values,
          "同じ Seed なら同じ山、Seed を変えると別の山");
    // 山の形（peak = 1）なら縁より中央が高い。
    other = settings;
    other.peak = 1;
    const auto peak = geometry::MakeNoiseHeights(other, 33);
    Check(peak.Sample(0.5f, 0.5f) > peak.Sample(0.0f, 0.0f) + 0.3f, "山の形では中央が縁より高い");

    const auto mesh = geometry::MakeTerrainMesh(heights, settings, error);
    geometry::MeshInfo info;
    Check(error.empty() && mesh.positions.size() == 33u * 33u && mesh.triangles.size() == 32u * 32u * 2 &&
              geometry::InspectMesh(mesh, info),
          "格子の点と三角形の数");
    Check(std::abs(info.minimum.x + 100) < 1e-3f && std::abs(info.maximum.x - 100) < 1e-3f &&
              std::abs(info.minimum.z + 100) < 1e-3f && std::abs(info.maximum.z - 100) < 1e-3f &&
              std::abs(info.minimum.y) < 1e-3f && std::abs(info.maximum.y - 80) < 1e-3f,
          "実寸（200 m 四方、高さ 0〜80 m）で中心が原点");
    bool up = true;
    for (const auto& face : mesh.triangles) up &= geometry::FaceNormal(mesh, face).y > 0;
    Check(up, "どの面も上を向く");
    Check(geometry::HasValidUvs(mesh) && mesh.uvWidth == 1024 && mesh.uvHeight == 1024 &&
              mesh.uvCharts.size() == mesh.triangles.size(),
          "ハイトマップと同じ向きの UV（1 枚の島）を持つ");

    auto bad = settings;
    bad.maxHeight = bad.minHeight;
    Check(!geometry::ValidateHeightmapSettings(bad, error) && !error.empty(), "高さの範囲が無ければ断る");
    bad = settings;
    bad.textureResolution = 1000;
    Check(!geometry::ValidateHeightmapSettings(bad, error), "テクスチャ解像度は 2 のべき乗");

    // 画像のハイトマップ。EXR に書いて読む（値の範囲が 0〜1 を外れていれば伸ばす）。
    const auto directory = std::filesystem::path(ROCK_DATA_DIR) / "test" / "terrain";
    std::filesystem::create_directories(directory);
    const auto exr = directory / "ramp.exr";
    std::vector<float> ramp(16 * 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) ramp[y * 16 + x] = float(x) * 10.0f;  // 0〜150
    Check(SaveExr(exr, 16, 8, 1, ramp.data(), false), "テスト用の EXR を書ける");
    geometry::HeightGrid image;
    Check(LoadHeightImage(exr, image.width, image.height, image.values) && image.width == 16 && image.height == 8 &&
              std::abs(image.values[0]) < 1e-5f && std::abs(image.values[15] - 1) < 1e-5f,
          "EXR は範囲を 0〜1 へ伸ばして読む");
    const auto png = directory / "ramp.png";
    std::vector<uint8_t> rgba(16 * 8 * 4, 255);
    for (int i = 0; i < 16 * 8; ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = uint8_t((i % 16) * 17);
    Check(SaveRgba8Png(png, 16, 8, 16 * 4, rgba.data()), "テスト用の PNG を書ける");
    Check(LoadHeightImage(png, image.width, image.height, image.values) && std::abs(image.values[15] - 1) < 1e-5f &&
              std::abs(image.values[1] - 17 / 255.0f) < 1e-5f,
          "8bit の PNG は 0〜255 を 0〜1 として読む");

    Section("Heightmap のグラフ");
    graph::NodeGraph g;
    const auto terrain = g.CreateNode(graph::NodeKind::Heightmap);
    Check(g.FindNode(terrain)->inputs.empty() && g.FindNode(terrain)->outputs.size() == 1 &&
              graph::IsMountainNodeKind(graph::NodeKind::Heightmap) && graph::IsMeshNodeKind(graph::NodeKind::Heightmap),
          "Heightmap は Mesh を出す山グラフのノード");
    std::get<geometry::HeightmapSettings>(g.FindMutableNode(terrain)->settings).resolution = 32;
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, terrain, &cache);
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && evaluated.rocks[0].mesh.triangles.size() == 32u * 32u * 2,
          "ノイズの山を評価できる");
    auto& imageSettings = std::get<geometry::HeightmapSettings>(g.FindMutableNode(terrain)->settings);
    imageSettings.source = geometry::HeightmapSource::Image;
    Check(!graph::EvaluateRocks(g, terrain, &cache).error.empty(), "画像を選んでいなければ診断する");
    imageSettings.image = ToUtf8Portable(png);
    const auto fromImage = graph::EvaluateRocks(g, terrain, &cache);
    Check(fromImage.error.empty() && fromImage.rocks.size() == 1, "画像のハイトマップを評価できる");

    // 地形は UV 付きの Mesh なので、Shape Mask（上向き度）がそのまま使える。
    const auto mask = g.CreateNode(graph::NodeKind::ShapeMask);
    auto& maskSettings = std::get<geometry::ShapeMaskSettings>(g.FindMutableNode(mask)->settings);
    maskSettings.type = geometry::ShapeMaskType::Direction;
    maskSettings.resolution = 128;
    g.CreateLink(g.FindNode(terrain)->outputs[0].id, g.FindNode(mask)->inputs[0].id);
    const auto slope = graph::EvaluateRocks(g, mask, &cache);
    Check(slope.error.empty() && slope.rocks.size() == 1 && slope.rocks[0].previewMask, "地形に Shape Mask（上向き度）を使える");
}
