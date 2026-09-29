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
