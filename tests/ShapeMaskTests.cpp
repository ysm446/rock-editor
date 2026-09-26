#include "TestSupport.h"
#include "geometry/ShapeMask.h"
#include "geometry/UvUnwrap.h"
#include "graph/RockEvaluator.h"
#include "app/UndoHistory.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using namespace rock;
// 上向きの床（x = 0〜2、UVの左半分）と、x = 0 に立つ壁（UVの右半分）。床は壁に近いほど遮蔽される。
// 面は共有しないが、向きは揃えてあり InspectMesh を通る。
geometry::Mesh FloorAndWall() {
    geometry::Mesh mesh;
    mesh.positions = {{0, 0, -4}, {2, 0, -4}, {2, 0, 4}, {0, 0, 4}, {0, 0, -4}, {0, 0, 4}, {0, 4, 4}, {0, 4, -4}};
    mesh.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}};
    // 床: u = x / 4（0〜0.5）、v = (z + 4) / 8。壁: u = 0.5〜1。
    mesh.cornerUvs = {{{{0, 0}, {.5f, 1}, {.5f, 0}}}, {{{0, 0}, {0, 1}, {.5f, 1}}},
                      {{{.5f, 0}, {.5f, 1}, {1, 1}}}, {{{.5f, 0}, {1, 1}, {1, 0}}}};
    mesh.uvCharts = {0, 0, 1, 1};
    mesh.uvWidth = mesh.uvHeight = 128;
    return mesh;
}
}  // namespace

void RunShapeMaskTests() {
    using tests::Check;
    tests::Section("Shape Mask");
    std::string error;
    geometry::ShapeMaskSettings raw;
    raw.low = 0; raw.high = 1; raw.distance = 1; raw.samples = 64; raw.resolution = 128;

    const auto scene = FloorAndWall();
    geometry::MeshInfo info;
    Check(geometry::HasValidUvs(scene) && geometry::InspectMesh(scene, info), "test scene is a valid UV mesh");
    auto image = geometry::ShapeMask(scene, raw, error);
    Check(error.empty() && image.width == 128 && image.height == 128 && image.pixels.size() == 128 * 128,
          "mask image uses the requested resolution");
    // 床の画素 x は、壁からの距離 (x + 0.5) / 128 * 4 m にある。
    const auto floorAt = [&](const geometry::MaskImage& m, int x) { return m.pixels[size_t(64) * 128 + x] / 255.f; };
    // 壁の足元では半球の半分が壁に当たる。コサイン重みなので遮蔽率は 0.5。
    Check(std::abs(floorAt(image, 0) - .5f) < .06f, "texel at the foot of a wall is half occluded");
    Check(floorAt(image, 8) < floorAt(image, 0) && floorAt(image, 16) < floorAt(image, 8) && floorAt(image, 40) == 0,
          "occlusion falls off with distance and vanishes beyond the search distance");
    Check(image.pixels == geometry::ShapeMask(scene, raw, error).pixels, "deterministic across parallel runs");
    auto near = raw; near.distance = .1f;
    const auto nearImage = geometry::ShapeMask(scene, near, error);
    Check(floorAt(nearImage, 8) == 0 && floorAt(nearImage, 0) > .3f, "short distance keeps only the closest texels");

    auto levels = raw; levels.low = .2f; levels.high = .4f;
    const auto leveled = geometry::ShapeMask(scene, levels, error);
    Check(floorAt(leveled, 0) == 1 && floorAt(leveled, 40) == 0 && floorAt(leveled, 8) <= 1, "low and high stretch the value and clamp");
    auto curved = levels; curved.gamma = 2;
    const auto curvedImage = geometry::ShapeMask(scene, curved, error);
    bool gammaOk = error.empty() && curvedImage.pixels[size_t(64) * 128 + 0] == leveled.pixels[size_t(64) * 128 + 0];
    for (int x = 0; x < 128 && gammaOk; ++x) {
        const float a = leveled.pixels[size_t(64) * 128 + x] / 255.f, b = curvedImage.pixels[size_t(64) * 128 + x] / 255.f;
        gammaOk &= std::abs(b - a * a) < 1.5f / 255;
    }
    Check(gammaOk, "gamma curves the stretched value and keeps 0 and 1");
    auto inverted = raw; inverted.invert = true;
    Check(geometry::ShapeMask(scene, inverted, error).pixels == image.pixels, "invert is applied by the consumer, not baked into the image");
    auto large = raw; large.resolution = 256;
    Check(geometry::ShapeMask(scene, large, error).width == 256, "resolution is independent of the UV atlas size");

    // 島の無い画素は最も近い島の値で埋まる。UVの上半分だけを使う形で確かめる。
    auto half = scene;
    for (auto& face : half.cornerUvs) for (auto& uv : face) uv.v *= .5f;
    auto direction = raw; direction.type = geometry::ShapeMaskType::Direction;
    const auto padded = geometry::ShapeMask(half, direction, error);
    Check(error.empty() && padded.pixels[size_t(10) * 128 + 10] == 255 && padded.pixels[size_t(120) * 128 + 10] == 255 &&
              padded.pixels[size_t(120) * 128 + 100] == 128,
          "texels outside the islands take the nearest island value");

    const auto facing = geometry::ShapeMask(scene, direction, error);
    Check(error.empty() && facing.pixels[size_t(64) * 128 + 20] == 255 && facing.pixels[size_t(64) * 128 + 100] == 128,
          "direction: up-facing floor is white, vertical wall is mid grey");
    auto heightSettings = raw; heightSettings.type = geometry::ShapeMaskType::Height;
    const auto heights = geometry::ShapeMask(scene, heightSettings, error);
    // 壁の画素 u は高さ (u - 0.5) * 2 * 4 m。外接箱の高さは 4 m。
    Check(error.empty() && heights.pixels[size_t(64) * 128 + 20] == 0 &&
              std::abs(heights.pixels[size_t(64) * 128 + 96] / 255.f - (96.5f / 128 - .5f) * 2) < .02f &&
              heights.pixels[size_t(64) * 128 + 127] > 250,
          "height: floor is black, wall rises linearly to white");
    Check(std::abs(image.Sample(.5f / 128, 64.5f / 128) - floorAt(image, 0)) < 1e-6f &&
              std::abs(facing.Sample(.2f, .5f) - 1) < 1e-6f && image.Sample(NAN, 0) == 0,
          "image sampling reads texel centres and rejects non-finite coordinates");

    for (auto bad : {[](auto s) { s.distance = 0; return s; }(raw), [](auto s) { s.samples = 4; return s; }(raw),
                     [](auto s) { s.samples = 500; return s; }(raw), [](auto s) { s.low = .5f; s.high = .5f; return s; }(raw),
                     [](auto s) { s.high = 1.5f; return s; }(raw), [](auto s) { s.resolution = 100; return s; }(raw),
                     [](auto s) { s.resolution = 8192; return s; }(raw), [](auto s) { s.distance = NAN; return s; }(raw),
                     [](auto s) { s.type = geometry::ShapeMaskType(9); return s; }(raw),
                     [](auto s) { s.gamma = 0; return s; }(raw), [](auto s) { s.gamma = 11; return s; }(raw)}) {
        Check(geometry::ShapeMask(scene, bad, error).pixels.empty() && !error.empty(), "invalid setting rejected");
    }
    Check(geometry::ShapeMask(geometry::MakeBox({2, 2, 2}), raw, error).pixels.empty() && !error.empty(), "mesh without UVs rejected");
    std::stop_source stop; stop.request_stop();
    Check(geometry::ShapeMask(scene, raw, error, stop.get_token()).pixels.empty() && !error.empty(), "cancellation returns no image");
    int lastProgress = -1;
    geometry::ShapeMask(scene, raw, error, {}, [&](int p) { lastProgress = p; });
    Check(lastProgress == 100, "progress reaches 100");

    // --- グラフ ---
    graph::NodeGraph g;
    const auto base = g.CreateNode(graph::NodeKind::BaseRock), uv = g.CreateNode(graph::NodeKind::UvUnwrap),
               apply = g.CreateNode(graph::NodeKind::ApplyMaterial), surface = g.CreateNode(graph::NodeKind::Surface),
               mask = g.CreateNode(graph::NodeKind::ShapeMask);
    const auto link = [&](int from, int to, int pin = 0) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[pin].id);
    };
    const auto maskSettings = [&]() -> geometry::ShapeMaskSettings& { return std::get<geometry::ShapeMaskSettings>(g.FindMutableNode(mask)->settings); };
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(uv)->settings).resolution = 128;
    maskSettings().resolution = 128;
    const auto* definition = graph::FindNodeDefinitionByName("shapeMask");
    Check(definition && definition->kind == graph::NodeKind::ShapeMask && g.FindNode(mask)->inputs.size() == 1 &&
              g.FindNode(mask)->inputs[0].valueType == graph::ValueType::Mesh && g.FindNode(mask)->outputs.size() == 1 &&
              g.FindNode(mask)->outputs[0].valueType == graph::ValueType::Mask &&
              maskSettings().type == geometry::ShapeMaskType::Occlusion && graph::IsPreviewableNodeKind(graph::NodeKind::ShapeMask),
          "node has a Mesh input and a Mask output, defaults to occlusion, and can be previewed");
    Check(!g.CanCreateLink(g.FindNode(mask)->outputs[0].id, g.FindNode(apply)->inputs[0].id) &&
              !g.CanCreateLink(g.FindNode(mask)->outputs[0].id, g.FindNode(apply)->inputs[1].id),
          "Mask output does not connect to Mesh or Material");
    Check(link(base, uv) && link(uv, apply) && link(surface, apply, 1) && link(mask, apply, 2), "Mask output connects to Apply Material");
    graph::RockEvaluationCache cache;
    auto r = graph::EvaluateRocks(g, apply, &cache);
    Check(!r.error.empty() && r.error.find("Shape Mask") != std::string::npos, "unconnected Mesh input is diagnosed");
    Check(link(base, mask), "Mesh output connects to the mask");
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(!r.error.empty() && r.error.find("UV") != std::string::npos, "mesh without UVs is diagnosed");
    Check(link(uv, mask), "UV Unwrap output connects to the mask");

    r = graph::EvaluateRocks(g, mask, &cache);
    Check(r.error.empty() && r.rocks.size() == 1 && r.rocks[0].previewMask && r.rocks[0].previewMask->width == 128 &&
              geometry::HasValidUvs(r.rocks[0].mesh) && r.rocks[0].materials.empty(),
          "previewing the mask node yields its input mesh with the mask image");
    const auto preview = r.rocks[0].previewMask;
    const auto computed = cache.computations[mask];  // 失敗した評価も数えるので、ここからの差で見る。
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && r.rocks.size() == 1 && r.rocks[0].maskImages.contains(mask) &&
              r.rocks[0].maskImages.at(mask) == preview && !r.rocks[0].previewMask && cache.computations[mask] == computed,
          "Apply Material carries the same image and computes it once");
    maskSettings().invert = true;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(cache.computations[mask] == computed && r.rocks[0].maskImages.at(mask) == preview, "invert does not recompute the image");
    maskSettings().type = geometry::ShapeMaskType::Height;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && cache.computations[mask] == computed + 1 && r.rocks[0].maskImages.at(mask) != preview, "type change recomputes");
    std::get<graph::BaseRockNodeSettings>(g.FindMutableNode(base)->settings).size = {1, 3, 1};
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && cache.computations[mask] == computed + 2, "upstream shape change recomputes");

    // 下流のキャッシュ（Subdivide）にも新しい画像が届く。
    const auto subdivide = g.CreateNode(graph::NodeKind::Subdivide);
    Check(link(apply, subdivide), "Subdivide after a shape-masked material");
    r = graph::EvaluateRocks(g, subdivide, &cache);
    const auto carried = r.error.empty() && !r.rocks.empty() ? r.rocks[0].maskImages.at(mask) : nullptr;
    maskSettings().low = .1f;
    r = graph::EvaluateRocks(g, subdivide, &cache);
    Check(r.error.empty() && carried && r.rocks[0].maskImages.at(mask) != carried, "mask change invalidates cached downstream results");

    // UV を作り直すとマスクの画像と合わなくなる。
    const auto again = g.CreateNode(graph::NodeKind::UvUnwrap);
    link(apply, again);
    Check(!graph::EvaluateRocks(g, again, &cache).error.empty(), "UV Unwrap after a shape-masked material is diagnosed");
    // UV の無いメッシュへは貼れない。
    const auto bare = g.CreateNode(graph::NodeKind::ApplyMaterial);
    link(base, bare); link(surface, bare, 1); link(mask, bare, 2);
    Check(!graph::EvaluateRocks(g, bare, &cache).error.empty(), "applying to a mesh without the mask's UVs is diagnosed");
    // マスクなしの素材で全面を置き換えると、画像も外れる。
    const auto replace = g.CreateNode(graph::NodeKind::ApplyMaterial);
    link(apply, replace); link(surface, replace, 1);
    r = graph::EvaluateRocks(g, replace, &cache);
    Check(r.error.empty() && r.rocks[0].maskImages.empty(), "full replacement drops the shape mask");

    maskSettings().samples = 2;
    maskSettings().type = geometry::ShapeMaskType::Occlusion;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(!r.error.empty() && r.error.find("Shape Mask") != std::string::npos, "invalid settings are diagnosed on the mask node");
    maskSettings().samples = 32;

    // Displace はマスクの画像をUVで読み、ハイトの混合に使う。高さのマスクなら下は動かず、上は動く。
    const auto displace = g.CreateNode(graph::NodeKind::Displace);
    Check(link(apply, displace), "Displace after a shape-masked material");
    maskSettings() = {};
    maskSettings().type = geometry::ShapeMaskType::Height;
    maskSettings().resolution = 128; maskSettings().low = .45f; maskSettings().high = .55f;
    graph::MaterialHeight materialHeights;
    auto& material = materialHeights.surfaces[surface];
    material.field = {1, 1, {1}};
    material.mapping.method = compositor::MappingMethod::Triplanar;
    material.axes = {geometry::Vec3{1, 0, 0}, geometry::Vec3{0, 1, 0}, geometry::Vec3{0, 0, 1}};
    const auto evaluateWithHeights = [&](int id) {
        return graph::EvaluateRocks(g, id, &cache, geometry::VolumeMeshingMethod::MarchingTetrahedra, {}, nullptr, &materialHeights);
    };
    const auto flat = evaluateWithHeights(apply);
    auto moved = evaluateWithHeights(displace);
    bool lowerFixed = moved.error.empty() && moved.rocks[0].mesh.positions.size() == flat.rocks[0].mesh.positions.size(), upperMoved = false;
    if (lowerFixed)
        for (size_t i = 0; i < flat.rocks[0].mesh.positions.size(); ++i) {
            const auto a = flat.rocks[0].mesh.positions[i], b = moved.rocks[0].mesh.positions[i];
            if (a.y < -1) lowerFixed &= a == b;
            if (a.y > 1) upperMoved |= !(a == b);
        }
    Check(lowerFixed && upperMoved, "height mask displaces the top and leaves the bottom in place");
    const auto before = cache.computations[displace];
    maskSettings().invert = true;
    moved = evaluateWithHeights(displace);
    Check(moved.error.empty() && cache.computations[displace] == before + 1, "invert is part of the Displace cache key");
    moved = evaluateWithHeights(displace);
    Check(cache.computations[displace] == before + 1, "Displace result is reused when nothing changed");

    // Undo / Redo は既存のスナップショットで戻る。
    DocumentSnapshot snapshotBefore;
    snapshotBefore.graphNodes = g.Nodes();
    snapshotBefore.graphLinks = g.Links();
    maskSettings().type = geometry::ShapeMaskType::Direction;
    DocumentSnapshot snapshotAfter;
    snapshotAfter.graphNodes = g.Nodes();
    snapshotAfter.graphLinks = g.Links();
    UndoHistory history;
    history.Push(snapshotBefore, 0);
    const auto undone = history.Undo(snapshotAfter);
    g.Replace(undone.graphNodes, undone.graphLinks);
    Check(maskSettings().type == geometry::ShapeMaskType::Height, "settings undo");
    const auto redone = history.Redo(undone);
    g.Replace(redone.graphNodes, redone.graphLinks);
    Check(maskSettings().type == geometry::ShapeMaskType::Direction, "settings redo");
}

void RunMaskCombineTests() {
    using tests::Check;
    tests::Section("Mask Combine");
    std::string error;
    // --- 画像の合成 ---
    const auto ramp = [](uint32_t size, bool vertical) {
        geometry::MaskImage m; m.width = m.height = size; m.pixels.resize(size_t(size) * size);
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) m.pixels[size_t(y) * size + x] = uint8_t((vertical ? y : x) * 255 / (size - 1));
        return m;
    };
    const auto a = ramp(64, false), b = ramp(64, true);
    const auto at = [](const geometry::MaskImage& m, uint32_t x, uint32_t y) { return m.pixels[size_t(y) * m.width + x] / 255.f; };
    geometry::MaskCombineSettings settings;
    const auto multiplied = geometry::CombineMasks(a, false, b, false, settings, error);
    Check(error.empty() && multiplied.width == 64 && multiplied.height == 64 && multiplied.pixels.size() == 64 * 64 &&
              std::abs(at(multiplied, 63, 63) - 1) < 1e-6f && at(multiplied, 0, 63) == 0 && at(multiplied, 63, 0) == 0 &&
              std::abs(at(multiplied, 32, 32) - at(a, 32, 32) * at(b, 32, 32)) < 1.5f / 255,
          "multiply: white only where both are white");
    settings.operation = geometry::MaskCombineOperation::Maximum;
    const auto maximum = geometry::CombineMasks(a, false, b, false, settings, error);
    Check(error.empty() && at(maximum, 0, 63) == 1 && at(maximum, 63, 0) == 1 && at(maximum, 0, 0) == 0 &&
              std::abs(at(maximum, 10, 40) - std::max(at(a, 10, 40), at(b, 10, 40))) < 1.5f / 255,
          "maximum: white where either is white");
    settings.operation = geometry::MaskCombineOperation::Minimum;
    const auto minimum = geometry::CombineMasks(a, false, b, false, settings, error);
    Check(error.empty() && at(minimum, 0, 63) == 0 && at(minimum, 63, 63) == 1 &&
              std::abs(at(minimum, 10, 40) - std::min(at(a, 10, 40), at(b, 10, 40))) < 1.5f / 255,
          "minimum: white only where both are white, without darkening greys");
    settings.operation = geometry::MaskCombineOperation::Subtract;
    const auto subtracted = geometry::CombineMasks(a, false, b, false, settings, error);
    Check(error.empty() && at(subtracted, 63, 0) == 1 && at(subtracted, 63, 63) == 0 && at(subtracted, 0, 63) == 0 &&
              std::abs(at(subtracted, 40, 10) - (at(a, 40, 10) - at(b, 40, 10))) < 1.5f / 255,
          "subtract: A minus B, clamped at black");
    settings.operation = geometry::MaskCombineOperation::Mix; settings.mix = .25f;
    const auto mixed = geometry::CombineMasks(a, false, b, false, settings, error);
    Check(error.empty() && std::abs(at(mixed, 63, 0) - .75f) < 1.5f / 255 && std::abs(at(mixed, 0, 63) - .25f) < 1.5f / 255,
          "mix: interpolates A toward B by the mix amount");
    settings = {};
    settings.operation = geometry::MaskCombineOperation::Maximum;
    const auto invertedInput = geometry::CombineMasks(a, true, b, false, settings, error);
    Check(error.empty() && at(invertedInput, 0, 0) == 1 && at(invertedInput, 63, 0) == 0,
          "input invert flags are applied before combining");
    settings.invert = true;
    const auto invertedOutput = geometry::CombineMasks(a, false, b, false, settings, error);
    Check(error.empty() && at(invertedOutput, 63, 63) == 0 && at(invertedOutput, 0, 0) == 1, "invert is baked into the output");
    settings = {};
    settings.operation = geometry::MaskCombineOperation::Maximum; settings.low = .5f; settings.high = 1; settings.gamma = 2;
    const auto leveled = geometry::CombineMasks(a, false, b, false, settings, error);
    Check(error.empty() && at(leveled, 0, 20) == 0 && at(leveled, 63, 0) == 1 &&
              std::abs(at(leveled, 48, 0) - std::pow((at(a, 48, 0) - .5f) / .5f, 2.f)) < 1.5f / 255,
          "low / high / gamma shape the combined value");
    settings = {};
    const auto small = ramp(32, true);
    const auto upscaled = geometry::CombineMasks(a, false, small, false, settings, error);
    Check(error.empty() && upscaled.width == 64 && upscaled.height == 64 && upscaled.pixels.size() == 64 * 64 &&
              at(upscaled, 63, 63) == 1 && at(upscaled, 63, 0) == 0 && std::abs(at(upscaled, 63, 32) - .5f) < .04f,
          "different resolutions: output uses the larger and samples the smaller");
    Check(geometry::CombineMasks(small, false, a, false, settings, error).width == 64, "larger B also sets the output size");
    for (auto bad : {[](auto s) { s.mix = 2; return s; }(settings), [](auto s) { s.low = .5f; s.high = .5f; return s; }(settings),
                     [](auto s) { s.gamma = 0; return s; }(settings), [](auto s) { s.operation = geometry::MaskCombineOperation(9); return s; }(settings)})
        Check(geometry::CombineMasks(a, false, b, false, bad, error).pixels.empty() && !error.empty(), "invalid setting rejected");
    Check(geometry::CombineMasks({}, false, b, false, settings, error).pixels.empty() && !error.empty(), "empty input rejected");

    // --- グラフ ---
    graph::NodeGraph g;
    const auto base = g.CreateNode(graph::NodeKind::BaseRock), uv = g.CreateNode(graph::NodeKind::UvUnwrap),
               apply = g.CreateNode(graph::NodeKind::ApplyMaterial), surface = g.CreateNode(graph::NodeKind::Surface),
               up = g.CreateNode(graph::NodeKind::ShapeMask), high = g.CreateNode(graph::NodeKind::ShapeMask),
               combine = g.CreateNode(graph::NodeKind::MaskCombine), constant = g.CreateNode(graph::NodeKind::MaterialMask);
    const auto link = [&](int from, int to, int pin = 0) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[pin].id);
    };
    const auto shape = [&](int id) -> geometry::ShapeMaskSettings& { return std::get<geometry::ShapeMaskSettings>(g.FindMutableNode(id)->settings); };
    const auto combineSettings = [&]() -> geometry::MaskCombineSettings& { return std::get<geometry::MaskCombineSettings>(g.FindMutableNode(combine)->settings); };
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(uv)->settings).resolution = 128;
    std::get<graph::BaseRockNodeSettings>(g.FindMutableNode(base)->settings).size = {1, 3, 1};
    shape(up).type = geometry::ShapeMaskType::Direction; shape(up).resolution = 128; shape(up).low = 0; shape(up).high = 1;
    shape(high).type = geometry::ShapeMaskType::Height; shape(high).resolution = 128; shape(high).low = 0; shape(high).high = 1;
    const auto* definition = graph::FindNodeDefinitionByName("maskCombine");
    Check(definition && definition->kind == graph::NodeKind::MaskCombine && g.FindNode(combine)->inputs.size() == 2 &&
              g.FindNode(combine)->inputs[0].valueType == graph::ValueType::Mask && g.FindNode(combine)->inputs[1].valueType == graph::ValueType::Mask &&
              g.FindNode(combine)->outputs.size() == 1 && g.FindNode(combine)->outputs[0].valueType == graph::ValueType::Mask &&
              combineSettings().operation == geometry::MaskCombineOperation::Multiply && graph::IsPreviewableNodeKind(graph::NodeKind::MaskCombine) &&
              graph::IsImageMaskNodeKind(graph::NodeKind::MaskCombine) && !graph::ImageMaskInvert(*g.FindNode(combine)),
          "node has two Mask inputs and a Mask output, defaults to multiply, and can be previewed");
    Check(!g.CanCreateLink(g.FindNode(combine)->outputs[0].id, g.FindNode(apply)->inputs[0].id) &&
              !g.CanCreateLink(g.FindNode(uv)->outputs[0].id, g.FindNode(combine)->inputs[0].id),
          "Mask pins only connect to Mask pins");
    Check(link(base, uv) && link(uv, up) && link(uv, high) && link(uv, apply) && link(surface, apply, 1) && link(combine, apply, 2),
          "chain connects");
    graph::RockEvaluationCache cache;
    auto r = graph::EvaluateRocks(g, apply, &cache);
    Check(!r.error.empty() && r.error.find("Mask Combine") != std::string::npos, "unconnected inputs are diagnosed");
    Check(link(constant, combine) && link(high, combine, 1), "Material Mask connects by type");
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(!r.error.empty() && r.error.find("Material Mask") != std::string::npos, "Material Mask input is diagnosed");
    for (const auto& l : g.Links()) if (l.endPin == g.FindNode(combine)->inputs[0].id) { g.DeleteLink(l.id); break; }
    Check(link(up, combine), "Shape Mask connects to A");

    r = graph::EvaluateRocks(g, combine, &cache);
    Check(r.error.empty() && r.rocks.size() == 1 && r.rocks[0].previewMask && r.rocks[0].previewMask->width == 128 &&
              !r.rocks[0].previewMaskInvert && geometry::HasValidUvs(r.rocks[0].mesh) && r.rocks[0].materials.empty(),
          "previewing the combine node yields the A mesh with the combined image");
    const auto preview = r.rocks[0].previewMask;
    // 乗算：上向き × 高さ。画素ごとに入力の積になる。
    const auto upImage = graph::EvaluateRocks(g, up, &cache).rocks[0].previewMask;
    const auto highImage = graph::EvaluateRocks(g, high, &cache).rocks[0].previewMask;
    bool productOk = upImage && highImage && upImage->pixels.size() == preview->pixels.size();
    int white = 0;
    for (size_t i = 0; productOk && i < preview->pixels.size(); ++i) {
        const float expect = upImage->pixels[i] / 255.f * (highImage->pixels[i] / 255.f);
        productOk &= std::abs(preview->pixels[i] / 255.f - expect) < 1.5f / 255;
        white += preview->pixels[i] == 255;
    }
    Check(productOk && white > 0, "combined image is the pixel-wise product of its inputs");
    const auto computed = cache.computations[combine];
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && r.rocks.size() == 1 && r.rocks[0].maskImages.contains(combine) && r.rocks[0].maskImages.at(combine) == preview &&
              !r.rocks[0].maskImages.contains(up) && !r.rocks[0].previewMask && cache.computations[combine] == computed,
          "Apply Material carries the combined image under the combine node and reuses it");
    combineSettings().operation = geometry::MaskCombineOperation::Maximum;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && cache.computations[combine] == computed + 1 && r.rocks[0].maskImages.at(combine) != preview, "operation change recomputes");
    const auto maxImage = r.rocks[0].maskImages.at(combine);
    combineSettings().invert = true;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && cache.computations[combine] == computed + 2 && r.rocks[0].maskImages.at(combine)->pixels != maxImage->pixels,
          "invert is baked into the image and part of the cache key");
    combineSettings().invert = false;
    const auto shapeComputed = cache.computations[up];
    shape(up).invert = true;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && cache.computations[up] == shapeComputed, "input invert does not recompute the Shape Mask");
    Check(r.error.empty() && cache.computations[combine] == computed + 3, "input invert recomputes the combine");
    Check(r.error.empty() && r.rocks[0].maskImages.at(combine)->pixels != maxImage->pixels, "input invert changes the combined image");
    shape(up).invert = false;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && r.rocks[0].maskImages.at(combine)->pixels == maxImage->pixels, "restoring the input gives the same image");
    shape(high).low = .3f;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(r.error.empty() && cache.computations[combine] == computed + 5, "input Shape Mask setting change recomputes the combine");

    // 直列：合成の結果をさらに合成する。
    const auto second = g.CreateNode(graph::NodeKind::MaskCombine), coarse = g.CreateNode(graph::NodeKind::ShapeMask);
    shape(coarse).type = geometry::ShapeMaskType::Height; shape(coarse).resolution = 256;
    Check(link(uv, coarse) && link(combine, second) && link(coarse, second, 1), "combine output connects to another combine");
    std::get<geometry::MaskCombineSettings>(g.FindMutableNode(second)->settings).operation = geometry::MaskCombineOperation::Subtract;
    r = graph::EvaluateRocks(g, second, &cache);
    Check(r.error.empty() && r.rocks[0].previewMask && r.rocks[0].previewMask->width == 256, "chained combine follows the larger input resolution");
    const auto chained = g.CreateNode(graph::NodeKind::ApplyMaterial);
    link(apply, chained); link(surface, chained, 1); link(second, chained, 2);
    r = graph::EvaluateRocks(g, chained, &cache);
    Check(r.error.empty() && r.rocks[0].maskImages.contains(combine) && r.rocks[0].maskImages.contains(second) && r.rocks[0].materials.size() == 2,
          "two masked materials carry both images");

    // 下流の Subdivide も合成マスクで面を選べる。
    const auto subdivide = g.CreateNode(graph::NodeKind::Subdivide);
    Check(link(apply, subdivide) && link(combine, subdivide, 1), "combine connects to Subdivide's Mask");
    r = graph::EvaluateRocks(g, subdivide, &cache);
    Check(r.error.empty() && !r.rocks.empty(), "Subdivide accepts the combined mask");

    // 別のメッシュから作ったマスクとは合成できない。
    const auto other = g.CreateNode(graph::NodeKind::BaseRock), otherUv = g.CreateNode(graph::NodeKind::UvUnwrap), otherMask = g.CreateNode(graph::NodeKind::ShapeMask),
               mismatch = g.CreateNode(graph::NodeKind::MaskCombine);
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(otherUv)->settings).resolution = 256;
    shape(otherMask).type = geometry::ShapeMaskType::Height; shape(otherMask).resolution = 128;
    link(other, otherUv); link(otherUv, otherMask); link(up, mismatch); link(otherMask, mismatch, 1);
    r = graph::EvaluateRocks(g, mismatch, &cache);
    Check(!r.error.empty() && r.error.find("UV") != std::string::npos, "masks from different UV atlases are diagnosed");

    combineSettings().mix = 3;
    combineSettings().operation = geometry::MaskCombineOperation::Mix;
    r = graph::EvaluateRocks(g, apply, &cache);
    Check(!r.error.empty() && r.error.find("Mask Combine") != std::string::npos, "invalid settings are diagnosed on the combine node");
}

void RunNoiseMaskTests() {
    using tests::Check;
    tests::Section("Noise Mask");
    std::string error;
    geometry::NoiseMaskSettings settings;settings.resolution=128;settings.size=.7f;
    // 同じ3Dの面を離れたUVの島へ置く。テクセルが同じ位置を指せば同じ値になる。
    geometry::Mesh mesh;
    mesh.positions={{0,0,0},{2,0,0},{2,0,2},{0,0,2},{0,0,0},{2,0,0},{2,0,2},{0,0,2}};
    mesh.triangles={{0,2,1},{0,3,2},{4,6,5},{4,7,6}};
    mesh.cornerUvs={{{{0,0},{.5f,1},{.5f,0}}},{{{0,0},{0,1},{.5f,1}}},
                    {{{.5f,0},{1,1},{1,0}}},{{{.5f,0},{.5f,1},{1,1}}}};
    mesh.uvCharts={0,0,1,1};mesh.uvWidth=mesh.uvHeight=128;
    auto image=geometry::NoiseMask(mesh,settings,error);
    Check(error.empty() && image.pixels.size()==128*128,"UV付きメッシュからマスクを生成");
    if (image.pixels.size()!=128*128) return;
    const auto range=std::minmax_element(image.pixels.begin(),image.pixels.end());
    Check(*range.second-*range.first>64,"一定値ではなくムラを生成");
    bool samePosition=true;
    for (size_t y=0;y<128;++y) for (size_t x=0;x<64;++x)
        samePosition &= image.pixels[y*128+x]==image.pixels[y*128+x+64];
    Check(samePosition,"離れたUVの島でも同じ3D位置には同じ模様");
    Check(geometry::NoiseMask(mesh,settings,error).pixels==image.pixels,"並列計算でもSeedから再現");
    auto edited=settings;edited.seed=123;
    Check(geometry::NoiseMask(mesh,edited,error).pixels!=image.pixels,"Seedで模様を変更");
    edited=settings;edited.size=1.7f;
    Check(geometry::NoiseMask(mesh,edited,error).pixels!=image.pixels,"ムラの大きさで模様を変更");
    edited=settings;edited.invert=true;
    Check(geometry::NoiseMask(mesh,edited,error).pixels==image.pixels,"反転は消費側で適用し画像を再生成しない");
    auto lifted=mesh;for (auto& p:lifted.positions) p.y+=.37f;
    Check(geometry::NoiseMask(lifted,settings,error).pixels!=image.pixels,"XZだけでなくY座標も模様に影響する");
    auto flipped=mesh;for (auto& face:flipped.cornerUvs) for (auto& uv:face) uv.v=1-uv.v;
    auto flipImage=geometry::NoiseMask(flipped,settings,error);
    bool followsUv=flipImage.pixels.size()==image.pixels.size();
    if (followsUv) for (size_t y=0;y<128;++y) for (size_t x=0;x<128;++x)
        followsUv &= std::abs(int(image.pixels[y*128+x])-int(flipImage.pixels[(127-y)*128+x]))<=1;
    Check(followsUv,"UVを反転すると画像も反転し表面の模様を維持");
    edited=settings;edited.contrast=1;
    auto high=geometry::NoiseMask(mesh,edited,error);
    const auto extremes=[](const auto& pixels) {return std::count_if(pixels.begin(),pixels.end(),[](auto v){return v==0 || v==255;});};
    Check(extremes(high.pixels)>extremes(image.pixels),"コントラストを上げると白黒が明確になる");
    for (auto bad:{[](auto s){s.size=0;return s;}(settings),
                  [](auto s){s.warp=std::numeric_limits<float>::quiet_NaN();return s;}(settings),
                  [](auto s){s.detail=2;return s;}(settings),[](auto s){s.resolution=127;return s;}(settings)})
        Check(geometry::NoiseMask(mesh,bad,error).pixels.empty() && !error.empty(),"不正なノイズ設定を診断");
    Check(geometry::NoiseMask(geometry::MakeBox({1,1,1}),settings,error).pixels.empty() && !error.empty(),"UVのないメッシュを診断");
    std::stop_source stop;stop.request_stop();
    Check(geometry::NoiseMask(mesh,settings,error,stop.get_token()).pixels.empty() && !error.empty(),"ノイズ生成のキャンセル");

    graph::NodeGraph g;
    const auto base=g.CreateNode(graph::NodeKind::BaseRock),uv=g.CreateNode(graph::NodeKind::UvUnwrap),
        noise=g.CreateNode(graph::NodeKind::NoiseMask),shape=g.CreateNode(graph::NodeKind::ShapeMask),
        combine=g.CreateNode(graph::NodeKind::MaskCombine),apply=g.CreateNode(graph::NodeKind::ApplyMaterial),
        surface=g.CreateNode(graph::NodeKind::Surface),sub=g.CreateNode(graph::NodeKind::Subdivide);
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(uv)->settings).resolution=128;
    std::get<geometry::NoiseMaskSettings>(g.FindMutableNode(noise)->settings)=settings;
    auto& shapeSettings=std::get<geometry::ShapeMaskSettings>(g.FindMutableNode(shape)->settings);
    shapeSettings.type=geometry::ShapeMaskType::Height;shapeSettings.resolution=128;
    const auto link=[&](int from,int to,int pin=0){return g.CreateLink(g.FindNode(from)->outputs[0].id,g.FindNode(to)->inputs[pin].id);};
    Check(link(base,uv) && link(uv,noise) && link(uv,shape) && link(noise,combine) && link(shape,combine,1) &&
          link(uv,apply) && link(surface,apply,1) && link(noise,apply,2) && link(apply,sub),"Noise Maskの接続と既存経路への統合");
    const auto* definition=graph::FindNodeDefinitionByName("noiseMask");
    Check(definition && definition->kind==graph::NodeKind::NoiseMask && graph::IsPreviewableNodeKind(definition->kind),"保存名と白黒プレビュー対象を登録");
    graph::RockEvaluationCache cache;
    auto result=graph::EvaluateRocks(g,noise,&cache);
    Check(result.error.empty() && result.rocks.size()==1 && result.rocks[0].previewMask,"ノード評価でプレビュー画像を返す");
    if (!result.error.empty() || result.rocks.empty() || !result.rocks[0].previewMask) return;
    const auto preview=result.rocks[0].previewMask;
    auto applied=graph::EvaluateRocks(g,sub,&cache);
    Check(applied.error.empty() && applied.rocks[0].maskImages.at(noise)==preview,"素材適用とSubdivideにマスク画像を渡す");
    const auto count=cache.computations[noise];
    auto mixed=graph::EvaluateRocks(g,combine,&cache);
    Check(mixed.error.empty() && mixed.rocks[0].previewMask && cache.computations[noise]==count,"Shape Maskとの合成で入力キャッシュを再利用");
    auto& ns=std::get<geometry::NoiseMaskSettings>(g.FindMutableNode(noise)->settings);
    ns.invert=true;
    Check(graph::ImageMaskInvert(*g.FindNode(noise)),"描画・ベイク・Displace共通の反転値");
    auto inverted=graph::EvaluateRocks(g,combine,&cache);
    Check(inverted.error.empty() && inverted.rocks[0].previewMask->pixels!=mixed.rocks[0].previewMask->pixels &&
          cache.computations[noise]==count,"反転はノイズを再生成せず合成結果へ反映");
    DocumentSnapshot before;before.graphNodes=g.Nodes();before.graphLinks=g.Links();
    ns.seed+=1;
    auto updated=graph::EvaluateRocks(g,sub,&cache);
    Check(updated.error.empty() && updated.rocks[0].maskImages.at(noise)!=preview && cache.computations[noise]==count+1,
          "Seed変更でノイズと下流のキャッシュを更新");
    DocumentSnapshot after;after.graphNodes=g.Nodes();after.graphLinks=g.Links();
    UndoHistory history;history.Push(before,0);auto undo=history.Undo(after);g.Replace(undo.graphNodes,undo.graphLinks);
    Check(std::get<geometry::NoiseMaskSettings>(g.FindNode(noise)->settings).seed==settings.seed,"Noise Mask設定のUndo");
    auto redo=history.Redo(undo);g.Replace(redo.graphNodes,redo.graphLinks);
    Check(std::get<geometry::NoiseMaskSettings>(g.FindNode(noise)->settings).seed==settings.seed+1,"Noise Mask設定のRedo");
}

void RunDepositionMaskTests() {
    using namespace rock;
    using tests::Check;
    using tests::Section;
    Section("Deposition Mask — 受け面・隙間・上方の開口");
    geometry::Mesh mesh;
    mesh.positions = {{-1,0,-1},{1,0,-1},{1,0,1},{-1,0,1}};
    mesh.triangles = {{0,2,1},{0,3,2}};
    mesh.cornerUvs = {{{{0,0},{1,1},{1,0}}},{{{0,0},{0,1},{1,1}}}};
    mesh.uvWidth=128; mesh.uvHeight=128;
    geometry::DepositionMaskSettings settings; settings.resolution=128; settings.recessPreference=0;
    std::string error;
    auto image=geometry::DepositionMask(mesh,settings,error);
    Check(error.empty() && image.Sample(.5f,.5f)>.99f,"開いた水平面は土を受ける");
    auto underside=mesh;
    for(auto& face:underside.triangles) std::swap(face[1],face[2]);
    Check(geometry::DepositionMask(underside,settings,error).Sample(.5f,.5f)==0,"下向き面には堆積しない");
    auto wall=mesh; for(auto& p:wall.positions) {p.y=p.x;p.x=0;}
    Check(geometry::DepositionMask(wall,settings,error).Sample(.5f,.5f)==0,"垂直面には堆積しない");
    auto slope=mesh;for(auto& p:slope.positions) p.y=p.x*2;
    Check(geometry::DepositionMask(slope,settings,error).Sample(.5f,.5f)==0,"許容角度を超えた斜面を除く");
    // レイに使う天井は別のUV島。隙間の探索距離より遠くても降下を遮る。
    auto roof=mesh;
    roof.positions.insert(roof.positions.end(),{{-2,1,-2},{2,1,-2},{2,1,2},{-2,1,2}});
    roof.triangles.push_back({4,5,6});roof.triangles.push_back({4,6,7});
    roof.cornerUvs.push_back({{{0,0},{.001f,0},{0,.001f}}});roof.cornerUvs.push_back({{{0,0},{.001f,0},{0,.001f}}});
    Check(geometry::DepositionMask(roof,settings,error).Sample(.5f,.5f)==0,"近傍範囲より遠い天井も上からの堆積を遮る");
    auto pocket=mesh;
    pocket.positions.insert(pocket.positions.end(),{{-1,1,-1},{-1,1,1}});
    pocket.triangles.push_back({0,4,5});pocket.triangles.push_back({0,5,3});
    pocket.cornerUvs.push_back({{{0,0},{.001f,0},{0,.001f}}});pocket.cornerUvs.push_back({{{0,0},{.001f,0},{0,.001f}}});
    settings.recessPreference=1;settings.distance=.8f;
    auto cavity=geometry::DepositionMask(pocket,settings,error);
    Check(error.empty() && cavity.Sample(.06f,.5f)>cavity.Sample(.8f,.5f)+.2f,"上に開いた入隅を平面の中央より優先");
    auto shorter=settings;shorter.distance=.01f;
    Check(geometry::DepositionMask(pocket,shorter,error).Sample(.06f,.5f)<cavity.Sample(.06f,.5f),"距離で対象にする隙間の大きさが変わる");
    Check(geometry::DepositionMask(pocket,settings,error).pixels==cavity.pixels,"並列計算でも結果は再現可能");
    auto half=settings;half.amount=.5f;
    Check(geometry::DepositionMask(pocket,half,error).Sample(.06f,.5f)<cavity.Sample(.06f,.5f),"堆積量で被覆が減る");
    half.amount=0;
    auto zero=geometry::DepositionMask(pocket,half,error);
    Check(std::all_of(zero.pixels.begin(),zero.pixels.end(),[](auto v){return v==0;}),"堆積量0は全面黒");
    half=settings;half.invert=true;
    Check(geometry::DepositionMask(pocket,half,error).pixels==cavity.pixels,"反転は消費側で適用");
    for(auto bad:{[](auto s){s.distance=0;return s;}(settings),
                 [](auto s){s.amount=std::numeric_limits<float>::quiet_NaN();return s;}(settings),
                 [](auto s){s.maxSlopeDegrees=90;return s;}(settings),
                 [](auto s){s.resolution=127;return s;}(settings),
                 [](auto s){s.recessPreference=2;return s;}(settings)})
        Check(geometry::DepositionMask(mesh,bad,error).pixels.empty() && !error.empty(),"不正な堆積設定を拒否");
    Check(geometry::DepositionMask(geometry::MakeBox({1,1,1}),settings,error).pixels.empty() && !error.empty(),"UVなしを診断");
    std::stop_source stop;stop.request_stop();
    Check(geometry::DepositionMask(mesh,settings,error,stop.get_token()).pixels.empty() && !error.empty(),"堆積生成のキャンセル");
    graph::NodeGraph g;
    const auto base=g.CreateNode(graph::NodeKind::BaseRock),uv=g.CreateNode(graph::NodeKind::UvUnwrap),
        noise=g.CreateNode(graph::NodeKind::DepositionMask),shape=g.CreateNode(graph::NodeKind::ShapeMask),
        combine=g.CreateNode(graph::NodeKind::MaskCombine),apply=g.CreateNode(graph::NodeKind::ApplyMaterial),
        surface=g.CreateNode(graph::NodeKind::Surface),sub=g.CreateNode(graph::NodeKind::Subdivide);
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(uv)->settings).resolution=128;
    std::get<geometry::DepositionMaskSettings>(g.FindMutableNode(noise)->settings)=settings;
    auto& shapeSettings=std::get<geometry::ShapeMaskSettings>(g.FindMutableNode(shape)->settings);
    shapeSettings.type=geometry::ShapeMaskType::Height;shapeSettings.resolution=128;
    const auto link=[&](int from,int to,int pin=0){return g.CreateLink(g.FindNode(from)->outputs[0].id,g.FindNode(to)->inputs[pin].id);};
    Check(link(base,uv) && link(uv,noise) && link(uv,shape) && link(noise,combine) && link(shape,combine,1) &&
          link(uv,apply) && link(surface,apply,1) && link(noise,apply,2) && link(apply,sub),"Deposition Maskの接続と既存経路への統合");
    const auto* definition=graph::FindNodeDefinitionByName("depositionMask");
    Check(definition && definition->kind==graph::NodeKind::DepositionMask && graph::IsPreviewableNodeKind(definition->kind),"保存名と白黒プレビュー対象を登録");
    graph::RockEvaluationCache cache;
    auto result=graph::EvaluateRocks(g,noise,&cache);
    Check(result.error.empty() && result.rocks.size()==1 && result.rocks[0].previewMask,"ノード評価でプレビュー画像を返す");
    if (!result.error.empty() || result.rocks.empty() || !result.rocks[0].previewMask) return;
    const auto preview=result.rocks[0].previewMask;
    auto applied=graph::EvaluateRocks(g,sub,&cache);
    Check(applied.error.empty() && applied.rocks[0].maskImages.at(noise)==preview,"素材適用とSubdivideにマスク画像を渡す");
    const auto count=cache.computations[noise];
    auto mixed=graph::EvaluateRocks(g,combine,&cache);
    Check(mixed.error.empty() && mixed.rocks[0].previewMask && cache.computations[noise]==count,"Shape Maskとの合成で入力キャッシュを再利用");
    auto& ns=std::get<geometry::DepositionMaskSettings>(g.FindMutableNode(noise)->settings);
    ns.invert=true;
    Check(graph::ImageMaskInvert(*g.FindNode(noise)),"描画・ベイク・Displace共通の反転値");
    auto inverted=graph::EvaluateRocks(g,combine,&cache);
    Check(inverted.error.empty() && inverted.rocks[0].previewMask->pixels!=mixed.rocks[0].previewMask->pixels &&
          cache.computations[noise]==count,"反転は堆積マスクを再生成せず合成結果へ反映");
    DocumentSnapshot before;before.graphNodes=g.Nodes();before.graphLinks=g.Links();
    ns.amount=.4f;
    auto updated=graph::EvaluateRocks(g,sub,&cache);
    Check(updated.error.empty() && updated.rocks[0].maskImages.at(noise)!=preview && cache.computations[noise]==count+1,
          "堆積量変更で堆積マスクと下流のキャッシュを更新");
    DocumentSnapshot after;after.graphNodes=g.Nodes();after.graphLinks=g.Links();
    UndoHistory history;history.Push(before,0);auto undo=history.Undo(after);g.Replace(undo.graphNodes,undo.graphLinks);
    Check(std::get<geometry::DepositionMaskSettings>(g.FindNode(noise)->settings).amount==settings.amount,"Deposition Mask設定のUndo");
    auto redo=history.Redo(undo);g.Replace(redo.graphNodes,redo.graphLinks);
    Check(std::get<geometry::DepositionMaskSettings>(g.FindNode(noise)->settings).amount==.4f,"Deposition Mask設定のRedo");
}
