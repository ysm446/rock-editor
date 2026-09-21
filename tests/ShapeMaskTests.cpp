#include "TestSupport.h"
#include "geometry/ShapeMask.h"
#include "geometry/UvUnwrap.h"
#include "graph/RockEvaluator.h"
#include "app/UndoHistory.h"
#include <algorithm>
#include <cmath>

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
                     [](auto s) { s.type = geometry::ShapeMaskType(9); return s; }(raw)}) {
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
