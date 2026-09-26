#include "TestSupport.h"
#include "graph/RockEvaluator.h"
#include "geometry/BakeOcclusion.h"
#include "app/UndoHistory.h"
#include <algorithm>
void RunApplyMaterialTests() {
    using namespace rock;
    using tests::Check;
    tests::Section("Apply Material / geometry AO");
    graph::NodeGraph g;
    auto base = g.CreateNode(graph::NodeKind::BaseRock), a = g.CreateNode(graph::NodeKind::ApplyMaterial),
         b = g.CreateNode(graph::NodeKind::ApplyMaterial), s = g.CreateNode(graph::NodeKind::Surface),
         t = g.CreateNode(graph::NodeKind::Surface), mask = g.CreateNode(graph::NodeKind::MaterialMask),
         uv = g.CreateNode(graph::NodeKind::UvUnwrap), bake = g.CreateNode(graph::NodeKind::MaterialBake);
    auto link = [&](int from, int to, int pin = 0) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[pin].id);
    };
    Check(link(base, a) && link(s, a, 1) && link(a, b) && link(t, b, 1), "Mesh + Material connect");
    auto r = graph::EvaluateRocks(g, b);
    Check(r.error.empty() && r.rocks[0].materials.size() == 1 && r.rocks[0].materials[0].surface == t,
          "unmasked material replaces upstream");
    Check(!g.CanCreateLink(g.FindNode(s)->outputs[0].id, g.FindNode(b)->inputs[2].id) && link(mask, b, 2),
          "Mask has distinct pin type");
    r = graph::EvaluateRocks(g, b);
    Check(r.error.empty() && r.rocks[0].materials.size() == 2 && r.rocks[0].materials[0].surface == s &&
              r.rocks[0].materials[1].mask == mask,
          "masked material retains ordered upstream stack");
    link(b, uv);
    link(uv, bake);
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(uv)->settings).resolution = 128;
    graph::RockEvaluationCache cache;
    r = graph::EvaluateRocks(g, bake, &cache);
    Check(r.error.empty() && r.rocks[0].materials.size() == 2 && r.rocks[0].bakeSource == bake,
          "UV and Bake inherit applied material without Material pin");
    link(s, bake, 1);
    r = graph::EvaluateRocks(g, bake, &cache);
    Check(r.error.empty() && r.rocks[0].materials.empty() && r.rocks[0].materialSource == s,
          "legacy explicit bake material overrides stack");
    DocumentSnapshot before;
    before.graphNodes = g.Nodes();
    before.graphLinks = g.Links();
    std::get<graph::MaterialMaskSettings>(g.FindMutableNode(mask)->settings).value = .25f;
    std::get<graph::MaterialBakeSettings>(g.FindMutableNode(bake)->settings).geometryAo = true;
    DocumentSnapshot after;
    after.graphNodes = g.Nodes();
    after.graphLinks = g.Links();
    UndoHistory history;
    history.Push(before, 0);
    auto undone = history.Undo(after);
    g.Replace(undone.graphNodes, undone.graphLinks);
    Check(std::get<graph::MaterialMaskSettings>(g.FindNode(mask)->settings).value == 1 &&
              !std::get<graph::MaterialBakeSettings>(g.FindNode(bake)->settings).geometryAo,
          "mask and AO undo");
    auto redone = history.Redo(undone);
    g.Replace(redone.graphNodes, redone.graphLinks);
    Check(std::get<graph::MaterialMaskSettings>(g.FindNode(mask)->settings).value == .25f &&
              std::get<graph::MaterialBakeSettings>(g.FindNode(bake)->settings).geometryAo,
          "mask and AO redo");
    std::get<graph::LayerNodeSettings>(g.FindMutableNode(t)->settings).layer.enabled = false;
    r = graph::EvaluateRocks(g, b);
    Check(r.error.empty() && r.rocks[0].materials.size() == 1, "disabled surface preserves upstream material");
    std::get<graph::LayerNodeSettings>(g.FindMutableNode(t)->settings).layer.enabled = true;
    int last = b;
    for (int i = 0; i < 6; ++i) {
        auto next = g.CreateNode(graph::NodeKind::ApplyMaterial);
        link(last, next); link(s, next, 1); link(mask, next, 2); last = next;
    }
    r = graph::EvaluateRocks(g, last);
    Check(r.error.empty() && r.rocks[0].materials.size() == 8, "eight material stages retained");
    auto excess = g.CreateNode(graph::NodeKind::ApplyMaterial);
    link(last, excess); link(s, excess, 1); link(mask, excess, 2);
    Check(!graph::EvaluateRocks(g, excess).error.empty(), "excess material stages diagnosed");
    // ハイトで合成。設定が束へ届き、重みの式がマスク 0 / 1 を保つ。
    auto& applySettings = std::get<graph::ApplyMaterialSettings>(g.FindMutableNode(b)->settings);
    applySettings.heightBlend = true; applySettings.heightBlendRange = .1f;
    r = graph::EvaluateRocks(g, b);
    Check(r.error.empty() && r.rocks[0].materials.size() == 2 && r.rocks[0].materials[1].heightBlend &&
              r.rocks[0].materials[1].heightBlendRange == .1f && !r.rocks[0].materials[0].heightBlend,
          "height blend settings reach the material binding of that stage");
    using graph::MaterialHeight;
    Check(MaterialHeight::HeightBlendWeight(0, 1, 0, .2f) == 0 && MaterialHeight::HeightBlendWeight(1, 0, 1, .2f) == 1,
          "mask 0 and 1 stay fully transparent / opaque regardless of height");
    Check(MaterialHeight::HeightBlendWeight(.5f, .9f, .1f, .2f) == 1 && MaterialHeight::HeightBlendWeight(.5f, .1f, .9f, .2f) == 0 &&
              std::abs(MaterialHeight::HeightBlendWeight(.5f, .5f, .5f, .2f) - .5f) < 1e-6f,
          "at half mask the higher material wins, equal heights blend evenly");
    Check(MaterialHeight::HeightBlendWeight(.5f, .6f, .5f, 1.f) < MaterialHeight::HeightBlendWeight(.5f, .6f, .5f, .1f),
          "smaller range makes the transition sharper");
    applySettings.heightBlend = false;
    // 不透明度。束へ届き、Mask 未接続でも 1 未満なら上流を残して重ねる。
    applySettings.opacity = .3f;
    r = graph::EvaluateRocks(g, b);
    Check(r.error.empty() && r.rocks[0].materials.size() == 2 && r.rocks[0].materials[1].opacity == .3f &&
              r.rocks[0].materials[0].opacity == 1,
          "opacity reaches the material binding of that stage");
    auto thin = g.CreateNode(graph::NodeKind::ApplyMaterial);
    link(b, thin); link(s, thin, 1);
    std::get<graph::ApplyMaterialSettings>(g.FindMutableNode(thin)->settings).opacity = .5f;
    r = graph::EvaluateRocks(g, thin);
    Check(r.error.empty() && r.rocks[0].materials.size() == 3 && r.rocks[0].materials[2].mask == 0,
          "translucent stage without mask keeps upstream materials");
    std::get<graph::ApplyMaterialSettings>(g.FindMutableNode(thin)->settings).opacity = 1;
    r = graph::EvaluateRocks(g, thin);
    Check(r.error.empty() && r.rocks[0].materials.size() == 1, "opaque stage without mask replaces everything");
    applySettings.opacity = 1;
    geometry::Mesh plane;
    plane.positions = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};
    plane.triangles = {{0, 2, 1}, {0, 3, 2}};
    plane.cornerUvs = {{{{0, 0}, {.5f, 1}, {.5f, 0}}}, {{{0, 0}, {0, 1}, {.5f, 1}}}};
    plane.uvWidth = plane.uvHeight = 32;
    std::vector<uint8_t> ao;
    std::string error;
    Check(geometry::BakeOcclusion(plane, 1, 32, 1, ao, error) &&
              std::all_of(ao.begin(), ao.end(), [](auto v) { return v == 255; }),
          "unoccluded plane stays white");
    for (int i = 0; i < 4; ++i) {
        auto p = plane.positions[i];
        p.y = .2f;
        plane.positions.push_back(p);
    }
    plane.triangles.push_back({4, 5, 6});
    plane.triangles.push_back({4, 6, 7});
    plane.cornerUvs.push_back({{{.5f, 0}, {1, 0}, {1, 1}}});
    plane.cornerUvs.push_back({{{.5f, 0}, {1, 1}, {.5f, 1}}});
    Check(geometry::BakeOcclusion(plane, 1, 32, 1, ao, error) && ao[16 * 32 + 8] < 100,
          "nearby surface darkens geometry AO");
    auto repeated = ao;
    geometry::BakeOcclusion(plane, 1, 32, 1, ao, error);
    Check(ao == repeated, "AO is deterministic");
    Check(geometry::BakeOcclusion(plane, .1f, 32, 1, ao, error) && ao[16 * 32 + 8] == 255,
          "AO distance excludes far occluder");
    Check(geometry::BakeOcclusion(plane, 1, 32, 0, ao, error) && ao[16 * 32 + 8] == 255,
          "zero AO strength preserves material AO");
    Check(!geometry::BakeOcclusion(plane, 1, 0, 1, ao, error), "invalid AO settings rejected");
}
