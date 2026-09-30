#include "TestSupport.h"
#include "geometry/BaseRock.h"
#include "geometry/DetailTransfer.h"
#include "geometry/UvUnwrap.h"
#include "graph/RockEvaluator.h"

#include <cmath>

using namespace rock::tests;
using namespace rock;

namespace {
geometry::Mesh Sphere(float diameter, int subdivisions) {
    geometry::BaseRockSettings settings;
    settings.shape = geometry::BaseShape::Sphere;
    settings.size = {diameter, diameter, diameter};
    settings.subdivisions = subdivisions;
    std::string error;
    return geometry::MakeBaseRock(settings, error);
}
}  // namespace

void RunDetailTransferTests() {
    Section("High → Low の転写");
    // ローポリ: 直径 2 m の粗い球を UV 展開したもの。
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), unwrap = g.CreateNode(graph::NodeKind::UvUnwrap);
    auto& shapeSettings = std::get<graph::BaseRockNodeSettings>(g.FindMutableNode(shape)->settings);
    shapeSettings.shape = geometry::BaseShape::Sphere;
    shapeSettings.subdivisions = 8;
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(unwrap)->settings).resolution = 128;
    g.CreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(unwrap)->inputs[0].id);
    const auto lowResult = graph::EvaluateRocks(g, unwrap);
    Check(lowResult.error.empty() && lowResult.rocks.size() == 1 && geometry::HasValidUvs(lowResult.rocks[0].mesh),
          "ローポリ（UV 展開した粗い球）を用意できる");
    if (lowResult.rocks.empty()) return;
    const auto& low = lowResult.rocks[0].mesh;

    // ハイポリ: 同じ中心で半径が 5 cm 大きい細かい球。
    const auto high = Sphere(2.1f, 32);
    geometry::DetailTransferImage image;
    std::string error;
    Check(geometry::TransferDetail(low, {}, high, 0.1f, 128, 128, image, error) && error.empty(), "転写できる");
    const size_t covered = image.CoveredCount(), hits = image.HitCount();
    Check(covered > 128 * 128 / 4 && hits >= covered * 99 / 100, "UV の島の画素はほぼ全てハイポリに当たる");
    // ローポリは多面体なので、面の中央では球まで少し遠い。高さは 5 cm 前後（外向きが正）、法線はほぼ真上を向く。
    double minHeight = 1e9, maxHeight = -1e9, minUp = 1;
    for (size_t i = 0; i < image.hit.size(); ++i) {
        if (!image.hit[i]) continue;
        minHeight = std::min(minHeight, double(image.heights[i]));
        maxHeight = std::max(maxHeight, double(image.heights[i]));
        minUp = std::min(minUp, double(image.normals[i].z));
    }
    Check(minHeight > 0.03 && maxHeight < 0.12, "外側のハイポリまでの距離を正の高さとして書く");
    Check(minUp > 0.9, "滑らかな球どうしなら、転写した法線はローポリの接線空間でほぼ真上");

    // 同じ形なら高さは 0、法線は真上。
    geometry::DetailTransferImage same;
    Check(geometry::TransferDetail(low, {}, low, 0.1f, 128, 128, same, error), "自分自身へも転写できる");
    double worstHeight = 0;
    for (size_t i = 0; i < same.hit.size(); ++i)
        if (same.hit[i]) worstHeight = std::max(worstHeight, std::abs(double(same.heights[i])));
    Check(same.HitCount() >= same.CoveredCount() * 99 / 100 && worstHeight < 1e-4, "同じ形なら高さはほぼ 0");

    // ケージ距離がずれより小さいと、ハイポリに届かない。
    geometry::DetailTransferImage tooShort;
    Check(geometry::TransferDetail(low, {}, high, 0.01f, 128, 128, tooShort, error) && tooShort.HitCount() == 0,
          "ケージ距離がずれより小さければ当たらない（素材の値が残る）");

    // 内側のハイポリは負の高さ。
    const auto inner = Sphere(1.9f, 32);
    geometry::DetailTransferImage inside;
    Check(geometry::TransferDetail(low, {}, inner, 0.2f, 128, 128, inside, error) && inside.HitCount() > 0, "内側へも転写できる");
    double maxInside = -1e9;
    for (size_t i = 0; i < inside.hit.size(); ++i)
        if (inside.hit[i]) maxInside = std::max(maxInside, double(inside.heights[i]));
    Check(maxInside < 0, "内側のハイポリは負の高さ");

    // 入力の検査。
    Check(!geometry::TransferDetail(high, {}, high, 0.1f, 64, 64, image, error) && !error.empty(), "UV の無いローポリは断る");
    Check(!geometry::TransferDetail(low, {}, geometry::Mesh{}, 0.1f, 64, 64, image, error), "空のハイポリは断る");
    Check(!geometry::TransferDetail(low, {}, high, 0.0f, 64, 64, image, error), "ケージ距離 0 は断る");
    Check(!geometry::TransferDetail(low, std::vector<std::array<geometry::CornerFrame, 3>>(1), high, 0.1f, 64, 64, image, error),
          "向きの数が合わなければ断る");

    Section("テクスチャの転写（LOD の段ごとのテクスチャ）");
    {
        // 転写元の画像。BaseColor は画素の位置（x, y）を色にし、法線は真上、ほかは一定。
        constexpr uint32_t size = 128;
        std::array<std::vector<uint8_t>, 4> pixels;
        for (auto& p : pixels) p.assign(size_t(size) * size * 4, 255);
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                const size_t i = (size_t(y) * size + x) * 4;
                pixels[0][i] = uint8_t(x * 2); pixels[0][i + 1] = uint8_t(y * 2); pixels[0][i + 2] = 77;
                pixels[1][i] = 128; pixels[1][i + 1] = 128; pixels[1][i + 2] = 255;
                pixels[2][i] = 200; pixels[2][i + 1] = 0; pixels[2][i + 2] = 180;
                pixels[3][i] = pixels[3][i + 1] = pixels[3][i + 2] = 90;
            }
        std::array<geometry::TextureView, 4> views;
        for (size_t c = 0; c < 4; ++c) views[c] = {size, size, pixels[c].data()};

        // 同じメッシュ・同じ UV へ転写すると、画像はほぼそのまま写る。
        geometry::TextureTransferResult identity;
        Check(geometry::TransferTextures(low, {}, low, {}, views, 0.1f, size, size, identity, error) && error.empty() &&
                  identity.width == size && identity.images[0].size() == size_t(size) * size * 4,
              "自分自身へテクスチャを転写できる");
        size_t pixelsIn = 0, off = 0, bent = 0;
        for (size_t i = 0; i < identity.covered.size(); ++i) {
            if (!identity.covered[i]) continue;
            ++pixelsIn;
            const uint8_t* color = &identity.images[0][i * 4];
            const uint8_t* normal = &identity.images[1][i * 4];
            const uint32_t x = uint32_t(i % size), y = uint32_t(i / size);
            if (color[3] != 255 || std::abs(int(color[0]) - int(x * 2)) > 2 || std::abs(int(color[1]) - int(y * 2)) > 2 || color[2] != 77) ++off;
            if (std::abs(int(normal[0]) - 128) > 3 || std::abs(int(normal[1]) - 128) > 3 || normal[2] < 250) ++bent;
        }
        Check(pixelsIn > 0 && identity.hits >= pixelsIn * 99 / 100, "UV の画素はほぼ全て転写元に当たる");
        Check(off <= pixelsIn / 100, "同じ UV なら色はその画素の色のまま");
        Check(bent <= pixelsIn / 100, "同じ形なら真上の法線は真上のまま");

        // 別の UV・細かさの違う形（UV を展開し直した LOD の段にあたる）。一定の色は一定のまま、法線はほぼ真上。
        geometry::UvUnwrapSettings unwrapSettings;
        unwrapSettings.resolution = 128;
        const auto other = geometry::UnwrapMesh(Sphere(2.0f, 16), unwrapSettings, error);
        geometry::TextureTransferResult moved;
        Check(error.empty() && geometry::TransferTextures(other, {}, low, {}, views, 0.2f, 64, 64, moved, error) && error.empty(),
              "別の UV の形へ転写できる");
        size_t otherCovered = 0, wrong = 0, tilted = 0;
        for (size_t i = 0; i < moved.covered.size(); ++i) {
            if (!moved.covered[i] || !moved.images[2][i * 4 + 3]) continue;
            ++otherCovered;
            const uint8_t* rma = &moved.images[2][i * 4];
            if (std::abs(int(rma[0]) - 200) > 1 || rma[1] > 1 || std::abs(int(rma[2]) - 180) > 1 || moved.images[3][i * 4] != 90) ++wrong;
            if (moved.images[1][i * 4 + 2] < 230) ++tilted;
        }
        Check(otherCovered > 0 && moved.hits >= otherCovered, "転写元に当たった画素だけ不透明になる");
        Check(wrong == 0, "一定の値はそのまま写る");
        Check(tilted <= otherCovered / 50, "滑らかな球どうしなら、法線は転写先の接線空間でもほぼ真上");

        geometry::TextureTransferResult rejected;
        Check(!geometry::TransferTextures(low, {}, high, {}, views, 0.1f, 64, 64, rejected, error) && !error.empty(),
              "UV の無い転写元は断る");
        auto missing = views;
        missing[3] = {};
        Check(!geometry::TransferTextures(low, {}, low, {}, missing, 0.1f, 64, 64, rejected, error), "画像が欠けていれば断る");
    }

    Section("Material Bake の High 入力");
    const auto bake = g.CreateNode(graph::NodeKind::MaterialBake), surface = g.CreateNode(graph::NodeKind::Surface),
               highShape = g.CreateNode(graph::NodeKind::BaseRock);
    Check(g.FindNode(bake)->inputs.size() == 3 && g.FindNode(bake)->inputs[2].label == "High" &&
              g.FindNode(bake)->inputs[2].valueType == graph::ValueType::Mesh,
          "Material Bake は 3 本目の入力 High（Mesh）を持つ");
    g.CreateLink(g.FindNode(unwrap)->outputs[0].id, g.FindNode(bake)->inputs[0].id);
    g.CreateLink(g.FindNode(surface)->outputs[0].id, g.FindNode(bake)->inputs[1].id);
    const auto withoutHigh = graph::EvaluateRocks(g, bake);
    Check(withoutHigh.error.empty() && withoutHigh.rocks.size() == 1 && withoutHigh.rocks[0].bakeDetail == 0,
          "High 未接続なら転写しない（従来どおり）");
    auto& highSettings = std::get<graph::BaseRockNodeSettings>(g.FindMutableNode(highShape)->settings);
    highSettings.shape = geometry::BaseShape::Sphere;
    highSettings.subdivisions = 32;
    Check(g.CreateLink(g.FindNode(highShape)->outputs[0].id, g.FindNode(bake)->inputs[2].id), "High にメッシュを繋げる");
    const auto withHigh = graph::EvaluateRocks(g, bake);
    Check(withHigh.error.empty() && withHigh.rocks.size() == 1 && withHigh.rocks[0].bakeDetail != 0 &&
              withHigh.rocks[0].mesh.triangles.size() == low.triangles.size(),
          "High を繋ぐと転写の対象になり、出力はローポリのまま");
    highSettings.subdivisions = 16;
    const auto changed = graph::EvaluateRocks(g, bake);
    Check(changed.error.empty() && changed.rocks[0].bakeDetail != withHigh.rocks[0].bakeDetail,
          "ハイポリの形が変わると、焼き直しが要ると分かる");
}
