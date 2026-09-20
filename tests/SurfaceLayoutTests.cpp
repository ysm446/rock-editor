#include "graph/SurfacePresetGraph.h"
#include "TestSupport.h"
#include "graph/SurfaceBandGeometry.h"
#include "graph/SurfaceLayout.h"
#include "graph/SurfaceLayoutEditing.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "graph/RoadMask.h"
#include "io/SurfaceLayoutIo.h"
#include "app/UndoHistory.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <cmath>

void RunSurfaceLayoutTests() {
    using namespace tg;
    using tests::Check;
    tests::Section("配置記述 — 保存・参照・独立区間・Undo");
    graph::SurfaceLayoutDocument document;
    std::array<graph::SurfaceId, 6> presets{};
    const char* names[] = {"新舗装", "荒れた舗装", "砂利道", "砂利路肩", "草地", "歩道"};
    for (size_t i = 0; i < presets.size(); ++i) {
        graph::SurfacePreset p;
        p.id = document.AllocateId();
        presets[i] = p.id;
        p.name = names[i];
        p.role = i < 3 ? graph::SurfaceRole::Road : i == 5 ? graph::SurfaceRole::Sidewalk : graph::SurfaceRole::Ground;
        p.section = {{document.AllocateId(), 0, 0}, {document.AllocateId(), 2, i == 5 ? 0.15f : 0.0f}};
        p.materials.push_back({static_cast<uint32_t>(i + 1)});
        p.displacementMeters = 0.01f * static_cast<float>(i);
        p.parameters.push_back({document.AllocateId(), "荒れ具合", 0, 1, 0.5f});
        p.boundaries[0].mode = i == 5 ? graph::BoundaryMode::KeepStep : graph::BoundaryMode::Blend;
        p.boundaries[0].preserveOutline = i == 5;
        document.presets.push_back(p);
    }
    graph::RoadLayout layout;
    layout.id = document.AllocateId(); layout.roadNode = 10;
    for (size_t side = 0; side < 2; ++side) {
        graph::SurfaceBand band;
        band.id = document.AllocateId();
        band.side = side == 0 ? graph::SurfaceSide::Road : graph::SurfaceSide::Left;
        const float cuts[2][4] = {{0, 12, 28, 50}, {0, 17, 35, 50}};
        for (size_t i = 0; i < 3; ++i) {
            graph::SurfaceSpan span;
            span.id = document.AllocateId(); span.preset = presets[side * 3 + i];
            span.startMeters = cuts[side][i]; span.endMeters = cuts[side][i+1];
            span.blendInMeters = 2; span.blendOutMeters = 3; span.seed = 17;
            span.parameters.push_back({document.presets[side * 3 + i].parameters[0].id, 0.2f, 0.8f});
            band.spans.push_back(span);
        }
        layout.bands.push_back(band);
    }
    document.layouts.push_back(layout);
    std::string error;
    Check(graph::ValidateSurfaceLayouts(document, error), "道路3種・沿道3種の独立した区間列を保持できる");
    {
        auto cloned = document;
        Check(graph::ExtractLayerMaterials(cloned, error), "複製検証用の独立材質を用意する");
        const auto original = io::WriteSurfaceLayouts(cloned);
        auto& cloneBand = cloned.layouts[0].bands[0];
        const auto parameterId = cloneBand.spans[0].parameters[0].parameter;
        Check(graph::DuplicateSurfacePreset(cloned, cloneBand, 0) &&
              cloneBand.spans[0].parameters[0].parameter != parameterId &&
              cloneBand.spans[0].parameters[0].parameter == cloned.presets.back().parameters[0].id &&
              cloneBand.spans[0].parameters[0].startValue == 0.2f &&
              cloneBand.spans[0].parameters[0].endValue == 0.8f &&
              graph::ValidateSurfaceLayouts(cloned, error), "複製は公開値の始終端値を維持して参照IDを更新する");
        Check(io::WriteSurfaceLayouts(cloned)["presets"][0] == original["presets"][0],
              "複製元のプリセットを変更しない");
        for (const graph::SurfaceId nextId : {graph::SurfaceId{0}, std::numeric_limits<graph::SurfaceId>::max() - 2}) {
            auto exhausted = document;
            Check(graph::ExtractLayerMaterials(exhausted, error), "ID不足検証用の独立材質を用意する");
            exhausted.nextId = nextId;
            const auto before = io::WriteSurfaceLayouts(exhausted);
            Check(!graph::DuplicateSurfacePreset(exhausted, exhausted.layouts[0].bands[0], 0) &&
                  exhausted.nextId == nextId && io::WriteSurfaceLayouts(exhausted) == before,
                  "不正な次ID・ID不足ではプリセットも区間参照も変更しない");
        }
    }
    graph::NodeGraph sceneGraph;
    Check(!graph::ValidateSurfaceLayoutRoads(document, sceneGraph, error), "存在しないRoad参照を保存・読込前に拒否する");
    auto linked = document;
    linked.layouts[0].roadNode = sceneGraph.CreateNode(graph::NodeKind::Road);
    Check(graph::ValidateSurfaceLayoutRoads(linked, sceneGraph, error), "配置は明示したRoadにだけ接続する");
    const auto encoded = io::WriteSurfaceLayouts(document);
    graph::SurfaceLayoutDocument decoded;
    Check(io::ReadSurfaceLayouts(encoded, decoded, error) && io::WriteSurfaceLayouts(decoded) == encoded,
          "安定ID・断面・境界条件・材質・公開値の始終端が完全に往復する");
    auto assertRejected = [&](nlohmann::json broken) {
        Check(!io::ReadSurfaceLayouts(broken, decoded, error) && !error.empty() && io::WriteSurfaceLayouts(decoded) == encoded,
              "不正な配置を理由付きで拒否し、既存データを保持する");
    };
    auto broken = encoded;
    broken["layouts"][0]["bands"][0]["spans"][0]["preset"] = 99999;
    assertRejected(broken);
    broken = encoded; broken["presets"][1]["id"] = document.presets[0].id; assertRejected(broken);
    broken = encoded; broken["layouts"][0]["bands"][0]["spans"][1]["start"] = 4; assertRejected(broken);
    broken = encoded; broken["layouts"][0]["bands"][0]["spans"][0]["parameters"][0]["end"] = 2; assertRejected(broken);
    broken = encoded; broken["nextId"] = -1; assertRejected(broken);
    broken = encoded; broken["nextId"] = uint64_t(1) << 40; assertRejected(broken);
    broken = encoded; broken["version"] = 99; assertRejected(broken);
    broken = encoded; broken["presets"][0]["section"] = "invalid"; assertRejected(broken);
    broken = encoded; broken["presets"][0]["boundaries"][0]["mode"] = 99; assertRejected(broken);
    broken = encoded; broken["layerMaterials"][0].erase("materials"); assertRejected(broken);
    auto invalid = document;
    invalid.layouts[0].bands[0].spans[0].endMeters = std::numeric_limits<float>::quiet_NaN();
    Check(!graph::ValidateSurfaceLayouts(invalid, error), "非有限の区間を保存前に拒否する");
    invalid.nextId = std::numeric_limits<graph::SurfaceId>::max();
    Check(invalid.AllocateId() == 0 && invalid.nextId == std::numeric_limits<graph::SurfaceId>::max(), "ID枯渇時に既存IDを再利用しない");

    DocumentSnapshot before;
    before.surfaceLayouts = document;
    DocumentSnapshot after = before;
    after.surfaceLayouts.layouts[0].bands[0].spans[0].parameters[0].endValue = 0.4f;
    after.surfaceLayouts.presets[5].section.back().height = 0.2f;
    UndoHistory history;
    history.Push(before, 0);
    const auto undone = history.Undo(after);
    Check(io::WriteSurfaceLayouts(undone.surfaceLayouts) == encoded, "区間と断面の編集をIDごとUndoする");
    const auto redone = history.Redo(undone);
    Check(io::WriteSurfaceLayouts(redone.surfaceLayouts) == io::WriteSurfaceLayouts(after.surfaceLayouts), "Redoで公開値と断面を復元する");

    const auto& roadBand = document.layouts[0].bands[0];
    const auto midpoint = graph::SampleSurfaceBand(document, roadBand, 6);
    Check(midpoint.size() == 1 && midpoint[0].preset == presets[0] && std::abs(midpoint[0].parameters[0].value - 0.5f) < 1e-6f,
          "実距離から区間の公開値を補間する");
    const auto blend = graph::SampleSurfaceBand(document, roadBand, 11.5f);
    Check(blend.size() == 2 && std::abs(blend[0].weight - 0.5f) < 1e-6f && blend[1].weight == blend[0].weight,
          "前区間の終端3mと次区間の始端2mで連続移行する");
    const auto side = graph::SampleSurfaceBand(document, document.layouts[0].bands[1], 11.5f);
    Check(side.size() == 1 && side[0].preset == presets[3], "道路の切り替えは沿道の区切りへ影響しない");
    auto shortBand = roadBand;
    shortBand.spans[0].endMeters = shortBand.spans[1].startMeters = 1;
    shortBand.spans[1].endMeters = shortBand.spans[2].startMeters = 2;
    bool partition = true;
    for (int step = 0; step <= 5000; ++step) {
        const auto samples = graph::SampleSurfaceBand(document, shortBand, static_cast<float>(step) / 100);
        float sum = 0;
        for (const auto& sample : samples) { sum += sample.weight; partition &= sample.weight >= 0 && sample.weight <= 1; }
        partition &= samples.size() <= 2 && std::abs(sum - 1) < 1e-6f;
    }
    Check(partition, "短い区間の前後に長い移行を指定しても被覆率の和が1で二重合成しない");
    auto gapBand = roadBand;
    gapBand.spans[1].startMeters = 14;
    Check(graph::SampleSurfaceBand(document, gapBand, 13).empty() &&
          graph::SampleSurfaceBand(document, roadBand, -1).empty() &&
          graph::SampleSurfaceBand(document, roadBand, std::numeric_limits<float>::quiet_NaN()).empty(),
          "空白区間・範囲外・非有限値を隣の素材で埋めない");
    auto hardBand = roadBand;
    hardBand.spans[0].blendOutMeters = hardBand.spans[1].blendInMeters = 0;
    const auto hard = graph::SampleSurfaceBand(document, hardBand, 12);
    Check(hard.size() == 1 && hard[0].preset == presets[1], "移行距離0の境界は次区間に属する");
    auto defaultsBand = roadBand;
    defaultsBand.spans[0].parameters.clear();
    Check(graph::SampleSurfaceBand(document, defaultsBand, 0)[0].parameters[0].value == 0.5f,
          "公開値を指定しない区間はプリセットの既定値を使う");

    const auto pathId = sceneGraph.CreateNode(graph::NodeKind::Path);
    graph::PathSettings path;
    const auto first = graph::AddPathPoint(path, 0, 0, 0);
    graph::AddPathPoint(path, 0, 50, first);
    std::get<graph::PathNodeSettings>(sceneGraph.FindMutableNode(pathId)->settings).path = path;
    sceneGraph.CreateLink(sceneGraph.FindNode(pathId)->outputs[0].id, sceneGraph.FindNode(linked.layouts[0].roadNode)->inputs[0].id);
    const auto preview = graph::CompileSurfaceLayoutPreview(sceneGraph, linked, linked.layouts[0].roadNode);
    Check(preview.error.empty() && preview.scene.meshes.size() == 4 && renderer::ValidateMeshScene(preview.scene),
          "配置から道路と3素材の評価コンテキストを生成する");
    if (preview.scene.meshes.size() == 4) {
        const auto& mask = preview.scene.meshes[0].roadMask;
        Check(mask.IsValid() && mask.rgba[0] == 0 && mask.rgba[1] == 0 && mask.rgba[mask.rgba.size() - 3] == 255,
              "道路始端は第1素材、終端は第3素材の共通マスクになる");
        Check(preview.scene.meshes[3].materialStack->Layers()[0].material == 3 &&
              preview.scene.meshes[3].displacementMeters == linked.presets[2].displacementMeters,
              "プリセットの素材参照と変位量を既存評価器へ渡す");
    }
    auto layered = linked;
    auto& preset = layered.presets[0];
    preset.layerBlendRange = 0.37f;
    for (uint32_t slot = 1; slot < 4; ++slot) {
        graph::PresetMaterial material;
        material.material = slot + 10;
        material.uvRepeatMeters = 1.5f * static_cast<float>(slot);
        material.worldUv = slot == 3;
        material.metallic = 0.1f; material.ambientOcclusion = 0.7f;
        material.mask.emplace();
        material.mask->shape = static_cast<graph::RoadMaskShape>(slot - 1);
        material.mask->seed = slot * 13;
        material.mask->edgeSide = graph::RoadMaskSide::Left;
        material.mask->strength = 0.6f;
        material.heightGate = slot % 3;
        material.heightGateThreshold = 0.3f;
        material.heightGateSoftness = 0.13f;
        material.blendMode = slot % 2;
        preset.materials.push_back(material);
    }
    const auto layeredJson = io::WriteSurfaceLayouts(layered);
    graph::SurfaceLayoutDocument layeredDecoded;
    Check(io::ReadSurfaceLayouts(layeredJson, layeredDecoded, error) && io::WriteSurfaceLayouts(layeredDecoded) == layeredJson,
          "4層の道路マスク・高さ条件・UV・PBR値が保存往復する");
    const auto layeredPreview = graph::CompileSurfaceLayoutPreview(sceneGraph, layeredDecoded, linked.layouts[0].roadNode);
    Check(layeredPreview.error.empty() && renderer::ValidateMeshScene(layeredPreview.scene), "多層プリセットをGPU評価用のシーンへ変換する");
    if (layeredPreview.scene.meshes.size() == 4) {
        const auto& context = layeredPreview.scene.meshes[1];
        graph::RoadGeometry evaluatedRoad;
        graph::EvaluateRoad(sceneGraph, linked.layouts[0].roadNode, evaluatedRoad, error);
        const auto lanes = graph::ComputeRoadLanes(evaluatedRoad.settings, sceneGraph.RoadNetwork().leftHandTraffic);
        const graph::RoadMaskNodeSettings* channels[] = {&*preset.materials[1].mask, &*preset.materials[2].mask, &*preset.materials[3].mask};
        const auto expected = graph::BakeRoadMask(channels, evaluatedRoad.settings.widthMeters, 50, &lanes, &evaluatedRoad);
        Check(context.roadMask.rgba == expected.rgba && context.layerStacks[2].has_value() &&
              context.layerStacks[2]->Layers()[0].material == 13 && context.layerWorldUv[3] &&
              context.layerUvRepeat[2] == 3 && context.layerHeightGate[2] == 2 && context.layerBlendRange == 0.37f,
              "既存Roadと同じマスクを生成し、各層の設定を失わない");
    }
    auto legacyJson = encoded;
    legacyJson["version"] = 1;
    legacyJson["nextId"] = document.nextId;
    for (size_t i = 0; i < document.presets.size(); ++i) {
        auto& shape = legacyJson["presets"][i];
        const auto& material = encoded["layerMaterials"][i];
        for (const auto* key : {"materials", "displacement", "layerBlendRange"}) shape[key] = material[key];
        shape.erase("layerMaterial");
    }
    legacyJson.erase("layerMaterials");
    for (auto& p : legacyJson["presets"]) {
        p.erase("layerBlendRange");
        for (auto& m : p["materials"])
            for (const auto* key : {"mask", "blendMode", "heightGate", "heightGateThreshold", "heightGateSoftness", "metallic", "ambientOcclusion"}) m.erase(key);
    }
    Check(io::ReadSurfaceLayouts(legacyJson, decoded, error) && io::WriteSurfaceLayouts(decoded) == encoded,
          "旧版の単層記述を既定値で移行する");
    for (const auto* field : {"mask", "heightGate", "metallic"}) {
        auto missing = layeredJson;
        missing["layerMaterials"][0]["materials"][1].erase(field);
        Check(!io::ReadSurfaceLayouts(missing, layeredDecoded, error) && io::WriteSurfaceLayouts(layeredDecoded) == layeredJson,
              "新版の必須項目欠落を既存文書を変更せず拒否する");
    }
    auto invalidMask = layeredJson;
    invalidMask["layerMaterials"][0]["materials"][1]["mask"]["shape"] = 99;
    Check(!io::ReadSurfaceLayouts(invalidMask, layeredDecoded, error), "不正な道路マスクを拒否する");
    auto invalidLayer = layered;
    invalidLayer.presets[0].materials[1].heightGateSoftness = std::numeric_limits<float>::quiet_NaN();
    Check(!graph::ValidateSurfaceLayouts(invalidLayer, error), "非有限の層条件をGPUへ渡さない");
    DocumentSnapshot layeredBefore;
    layeredBefore.surfaceLayouts = layered;
    auto layeredAfter = layeredBefore;
    layeredAfter.surfaceLayouts.presets[0].materials[1].mask->strength = 0.1f;
    history.Clear();
    history.Push(layeredBefore, 0);
    Check(io::WriteSurfaceLayouts(history.Undo(layeredAfter).surfaceLayouts) == layeredJson, "層マスクの編集をUndoで復元する");
    graph::SurfaceLayoutDocument editing;
    const auto roadId = linked.layouts[0].roadNode;
    Check(graph::CreateRoadLayout(editing, sceneGraph, roadId, error), "現在の道路から区間と材質プリセットを作る");
    auto* band = graph::FindRoadBand(editing, roadId);
    Check(band && band->spans.size() == 1 && band->spans[0].endMeters == 50, "初期区間は道路の全長を覆う");
    if (band) {
        const auto originalId = band->spans[0].id;
        Check(graph::SplitSurfaceSpan(editing, *band, 0) && band->spans.size() == 2 &&
              band->spans[0].endMeters == band->spans[1].startMeters && band->spans[0].id == originalId,
              "分割で左のIDを保ち、右に新IDを割り当てる");
        Check(graph::DuplicateSurfacePreset(editing, *band, 1) && band->spans[0].preset != band->spans[1].preset,
              "複製した材質は別区間の編集から独立する");
        Check(graph::ValidateSurfaceLayouts(editing, error), "分割と複製後も全ID・参照が有効");
        const auto editingSnapshot = io::WriteSurfaceLayouts(editing);
        Check(graph::RemoveSurfaceSpan(*band, 0) && band->spans[0].startMeters == 0 && band->spans[0].endMeters == 50,
              "区間を削除すると隣の区間が埋める");
        Check(!graph::RemoveSurfaceSpan(*band, 0), "最後の区間は削除しない");
        Check(graph::ResizeSurfaceBand(*band, 24) && band->spans.back().endMeters == 24, "道路長の変更へ区間を合わせる");
        Check(io::ReadSurfaceLayouts(editingSnapshot, editing, error), "操作前の保存記述へ戻せる");
    }
    const auto markingId = sceneGraph.CreateNode(graph::NodeKind::RoadMarking);
    const auto outputId = sceneGraph.CreateNode(graph::NodeKind::MeshOutput);
    sceneGraph.CreateLink(sceneGraph.FindNode(roadId)->outputs[0].id, sceneGraph.FindNode(markingId)->inputs[0].id);
    sceneGraph.CreateLink(sceneGraph.FindNode(markingId)->outputs[0].id, sceneGraph.FindNode(outputId)->inputs[0].id);
    const auto normalScene = graph::CompileMeshGraphWithLayouts(sceneGraph, editing);
    Check(normalScene.error.empty() && renderer::ValidateMeshScene(normalScene.scene) && normalScene.scene.meshes.size() == 4,
          "通常のMesh Outputへ道路2材質と白線を統合する");
    if (normalScene.scene.meshes.size() == 4) {
        Check(normalScene.scene.meshes[0].connectionSources[0] == 2 && normalScene.scene.meshes[0].connectionSources[1] == 3 &&
              normalScene.scene.meshes[1].displacementSource == 0 && normalScene.scene.meshes[1].useBlendMode,
              "白線の材質を保ち、変位元の道路と内部コンテキストの参照を維持する");
    }
    auto badRange = editing;
    graph::FindRoadBand(badRange, roadId)->spans.back().endMeters = 40;
    const auto fallback = graph::CompileMeshGraphWithLayouts(sceneGraph, badRange);
    Check(!fallback.error.empty() && fallback.scene.meshes.size() == 2 && fallback.scene.meshes[0].connectionSources[0] == -1,
          "道路長と区間が合わない間は元の道路を表示し、エラーを残す");

    auto retained = linked;
    const auto roadsideBefore = io::WriteSurfaceLayouts(retained)["layouts"][0]["bands"][1];
    auto* retainedRoad = graph::FindRoadBand(retained, roadId);
    const auto bandId = retainedRoad->id;
    retainedRoad->spans.clear();
    {
        auto lastIds = retained;
        lastIds.nextId = std::numeric_limits<graph::SurfaceId>::max() - 4;
        Check(graph::CreateRoadLayout(lastIds, sceneGraph, roadId, error) &&
              lastIds.nextId == std::numeric_limits<graph::SurfaceId>::max() &&
              graph::FindRoadBand(lastIds, roadId)->id == bandId &&
              graph::ValidateSurfaceLayouts(lastIds, error),
              "既存道路帯の再作成はプリセット・断面2点・区間の4IDだけを使う");
    }
    Check(graph::CompileMeshGraphWithLayouts(sceneGraph, retained).error.empty(), "解除した空の道路帯は元のRoad材質で表示する");
    Check(graph::CreateRoadLayout(retained, sceneGraph, roadId, error) &&
          graph::FindRoadBand(retained, roadId)->id == bandId &&
          io::WriteSurfaceLayouts(retained)["layouts"][0]["bands"][1] == roadsideBefore,
          "解除後に再開しても沿道と道路帯のIDを保持する");
    DocumentSnapshot editBefore, editAfter;
    editBefore.surfaceLayouts = editing;
    editAfter = editBefore;
    graph::SplitSurfaceSpan(editAfter.surfaceLayouts, *graph::FindRoadBand(editAfter.surfaceLayouts, roadId), 0);
    history.Clear(); history.Push(editBefore, 0);
    const auto editUndo = history.Undo(editAfter);
    Check(io::WriteSurfaceLayouts(editUndo.surfaceLayouts) == io::WriteSurfaceLayouts(editBefore.surfaceLayouts) &&
          io::WriteSurfaceLayouts(history.Redo(editUndo).surfaceLayouts) == io::WriteSurfaceLayouts(editAfter.surfaceLayouts),
          "区間分割の全IDをUndo・Redoで復元する");

    tests::Section("沿道断面 — 左右・移行・高さ・保存");
    graph::SurfaceLayoutDocument roadside;
    Check(graph::CreateRoadsideExample(roadside, sceneGraph, roadId, graph::SurfaceSide::Left, error),
          "左側に路肩から歩道へ移る記述を作れる");
    const auto roadsideJson = io::WriteSurfaceLayouts(roadside);
    auto resizedSide = roadside;
    auto& resizedPreset = resizedSide.presets[1];
    const auto originalSection = resizedPreset.section;
    Check(graph::SetSimpleRoadsideDimensions(resizedPreset, 3.5f, 0.3f), "歩道の幅と段差を編集する");
    Check(resizedPreset.section[0].across == 0 && resizedPreset.section[0].height == 0 &&
          resizedPreset.section[1].across == 0 && resizedPreset.section[1].height == 0.3f &&
          resizedPreset.section[2].across == 3.5f && resizedPreset.section[2].height == 0.3f &&
          resizedPreset.section[1].id == originalSection[1].id, "道路端と垂直段差・水平面・点IDを保持する");
    const auto validResized = io::WriteSurfaceLayouts(resizedSide);
    Check(!graph::SetSimpleRoadsideDimensions(resizedPreset, 3, 0) &&
          !graph::SetSimpleRoadsideDimensions(resizedPreset, std::numeric_limits<float>::quiet_NaN(), 0.2f) &&
          io::WriteSurfaceLayouts(resizedSide) == validResized, "潰れる段差や非有限寸法は変更せず拒否する");
    auto complexPreset = resizedPreset; complexPreset.section[1].across = 0.1f;
    Check(!graph::SetSimpleRoadsideDimensions(complexPreset, 2, 0.15f), "複雑な断面を単純形状で上書きしない");
    Check(graph::SetSimpleRoadsideDimensions(resizedSide.presets[0], 1.2f, -0.2f), "路肩の幅と外端の落差を編集する");
    const auto sharedPresetId = resizedSide.layouts[0].bands[1].spans[0].preset;
    Check(graph::DuplicateSurfacePreset(resizedSide, resizedSide.layouts[0].bands[1], 0) &&
          resizedSide.layouts[0].bands[1].spans[0].preset != sharedPresetId, "沿道プリセットを複製して区間へ割り当てる");
    graph::SurfaceLayoutDocument resizedReload;
    Check(io::ReadSurfaceLayouts(io::WriteSurfaceLayouts(resizedSide), resizedReload, error) &&
          io::WriteSurfaceLayouts(resizedReload) == io::WriteSurfaceLayouts(resizedSide), "沿道の寸法・複製・割当が保存往復する");
    Check(!graph::CreateRoadsideExample(roadside, sceneGraph, roadId, graph::SurfaceSide::Left, error) &&
          io::WriteSurfaceLayouts(roadside) == roadsideJson, "既存の沿道は上書きしない");
    graph::RoadGeometry bandRoad;
    graph::EvaluateRoad(sceneGraph, roadId, bandRoad, error);
    auto following = roadside;
    Check(graph::CreateRoadsideExample(following, sceneGraph, roadId, graph::SurfaceSide::Right, error) &&
          graph::CreateRoadLayout(following, sceneGraph, roadId, error), "追従検証用の路面と左右沿道を作る");
    auto shortenedGraph = sceneGraph;
    graph::PathSettings shorterPath;
    const auto shorterFirst = graph::AddPathPoint(shorterPath, 0, 0, 0);
    graph::AddPathPoint(shorterPath, 0, 24, shorterFirst);
    std::get<graph::PathNodeSettings>(shortenedGraph.FindMutableNode(pathId)->settings).path = shorterPath;
    graph::RoadGeometry shorterRoad;
    graph::EvaluateRoad(shortenedGraph, roadId, shorterRoad, error);
    renderer::MeshData followingMesh;
    Check(!graph::BuildSurfaceBandGeometry(shorterRoad, following, following.layouts[0].bands[1], followingMesh, error),
          "パス短縮後の未追従沿道が消える条件を再現する");
    DocumentSnapshot followBefore, followAfter;
    followBefore.surfaceLayouts = following;
    Check(graph::FitSurfaceLayoutsToRoads(following, shortenedGraph), "パス変更へ全帯を自動追従する");
    for (const auto& followingBand : following.layouts[0].bands) {
        Check(followingBand.spans.back().endMeters == 24, "路面と左右沿道が同じ全長へ追従する");
        if (followingBand.side != graph::SurfaceSide::Road)
            Check(followingBand.spans[0].endMeters == 12 &&
                  graph::BuildSurfaceBandGeometry(shorterRoad, following, followingBand, followingMesh, error),
                  "左右の区間割合と沿道表示を保持する");
    }
    Check(!graph::FitSurfaceLayoutsToRoads(following, shortenedGraph), "道路長が同じなら追従処理は変更しない");
    followAfter.surfaceLayouts = following;
    history.Clear(); history.Push(followBefore, 0);
    const auto followUndo = history.Undo(followAfter);
    Check(io::WriteSurfaceLayouts(followUndo.surfaceLayouts) == io::WriteSurfaceLayouts(followBefore.surfaceLayouts) &&
          io::WriteSurfaceLayouts(history.Redo(followUndo).surfaceLayouts) == io::WriteSurfaceLayouts(followAfter.surfaceLayouts),
          "追従した左右沿道と路面の全区間をUndo・Redoで復元する");
    Check(graph::FitSurfaceLayoutsToRoads(following, sceneGraph), "パスを伸ばす方向にも追従する");
    auto splitSide = roadside;
    auto& splitBand = splitSide.layouts[0].bands[1];
    const auto firstSideId = splitBand.spans.front().id;
    renderer::MeshData editedSideMesh;
    Check(graph::SplitSurfaceSpan(splitSide, splitBand, 0) && splitBand.spans.size() == 3 &&
          splitBand.spans[0].id == firstSideId && splitBand.spans[1].id != firstSideId &&
          graph::BuildSurfaceBandGeometry(bandRoad, splitSide, splitBand, editedSideMesh, error),
          "沿道を同じプリセットで分割し、元のIDと連続形状を保持する");
    splitBand.spans[1].preset = splitBand.spans[2].preset;
    splitBand.spans[1].parameters.clear();
    graph::EnsureRoadsideTransitions(splitBand);
    Check(splitBand.spans[0].blendOutMeters > 0 && splitBand.spans[1].blendInMeters > 0 &&
          graph::BuildSurfaceBandGeometry(bandRoad, splitSide, splitBand, editedSideMesh, error),
          "分割後の歩道割当で移行距離を自動補完する");
    for (size_t removed = 0; removed < 3; ++removed) {
        auto deletedSide = splitSide;
        auto& deletedBand = deletedSide.layouts[0].bands[1];
        Check(graph::RemoveSurfaceSpan(deletedBand, removed), "先頭・中間・末尾の沿道区間を削除できる");
        graph::EnsureRoadsideTransitions(deletedBand);
        Check(deletedBand.spans.front().startMeters == 0 &&
              deletedBand.spans.back().endMeters == bandRoad.rowDistances.back() &&
              graph::BuildSurfaceBandGeometry(bandRoad, deletedSide, deletedBand, editedSideMesh, error),
              "削除した範囲を隣接区間で埋めて連続形状を保つ");
        graph::RemoveSurfaceSpan(deletedBand, 0);
        Check(!graph::RemoveSurfaceSpan(deletedBand, 0), "最後の沿道区間は削除しない");
    }
    const auto beforeResize = io::WriteSurfaceLayouts(splitSide);
    const float originalLength = splitBand.spans.back().endMeters;
    const float originalCut = splitBand.spans.front().endMeters;
    Check(graph::ResizeSurfaceBand(splitBand, originalLength * 0.5f) &&
          splitBand.spans.front().endMeters == originalCut * 0.5f &&
          graph::ResizeSurfaceBand(splitBand, originalLength) &&
          io::WriteSurfaceLayouts(splitSide) == beforeResize,
          "道路長への伸縮で区間割合・移行距離・IDを保持する");
    graph::SurfaceLayoutDocument splitReload;
    Check(io::ReadSurfaceLayouts(beforeResize, splitReload, error) &&
          graph::BuildSurfaceBandGeometry(bandRoad, splitReload, splitReload.layouts[0].bands[1], editedSideMesh, error),
          "編集した沿道を保存往復して再生成する");
    renderer::MeshData bandMesh;
    const auto checkBandGrid = [&](const renderer::MeshData& mesh) {
        bool bounded = !mesh.vertices.empty();
        for (size_t i = 0; i + 3 < mesh.vertices.size(); i += 4) {
            for (const auto edge : {std::pair{0, 1}, std::pair{0, 2}, std::pair{1, 3}, std::pair{2, 3}}) {
                const auto& a = mesh.vertices[i + edge.first].position;
                const auto& b = mesh.vertices[i + edge.second].position;
                bounded &= std::sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) + (a.z-b.z)*(a.z-b.z)) <= 1.0001f;
            }
        }
        Check(bounded, "沿道格子の進行方向・断面方向の辺が1m以下である");
    };
    Check(graph::BuildSurfaceBandGeometry(bandRoad, roadside, roadside.layouts[0].bands[1], bandMesh, error),
          "路肩から歩道の連続した断面を生成する");
    checkBandGrid(bandMesh);
    renderer::SceneMesh bandSceneMesh; bandSceneMesh.geometry = bandMesh;
    renderer::MeshScene bandScene; bandScene.meshes.push_back(bandSceneMesh);
    Check(renderer::ValidateMeshScene(bandScene), "沿道の頂点・法線・接線・インデックスが有効");
    if (!bandMesh.vertices.empty()) {
        Check(std::abs(bandMesh.vertices.front().position.x - bandRoad.surface.vertices[bandRoad.stride - 1].position.x) < 1e-5f &&
              std::abs(bandMesh.vertices.back().position.y - 0.15f) < 1e-5f,
              "内端は道路端に一致し、終端の歩道は15 cm上がる");
        bool hasSlope = false;
        for (const auto& v : bandMesh.vertices) if (v.uv.x == 1 && v.position.y > 0 && v.position.y < 0.14f) hasSlope = true;
        Check(hasSlope, "移行区間には中間の高さが存在する");
    }
    auto rightSide = roadside;
    rightSide.layouts[0].bands[1].side = graph::SurfaceSide::Right;
    renderer::MeshData rightMesh;
    Check(graph::BuildSurfaceBandGeometry(bandRoad, rightSide, rightSide.layouts[0].bands[1], rightMesh, error),
          "右側にも同じ断面を生成できる");
    checkBandGrid(rightMesh);
    if (!rightMesh.vertices.empty() && rightMesh.vertices.size() == bandMesh.vertices.size()) {
        bool mirrored = true;
        for (size_t i = 0; i < rightMesh.vertices.size(); ++i)
            mirrored &= std::abs(rightMesh.vertices[i].position.x + bandMesh.vertices[i].position.x) < 1e-5f &&
                        std::abs(rightMesh.vertices[i].normal.y - bandMesh.vertices[i].normal.y) < 1e-5f;
        Check(mirrored, "左右を反転しても面の表裏は反転しない");
        for (const auto* mesh : {&bandMesh, &rightMesh}) {
            const auto& v = mesh->vertices.front();
            const float bitangentZ = (v.normal.x * v.tangent.y - v.normal.y * v.tangent.x) * v.tangent.w;
            Check(bitangentZ > 0, "左右とも法線マップのV方向が道路の進行方向と一致する");
        }
    }
    auto bentRoad = bandRoad;
    for (auto& v : bentRoad.surface.vertices) {
        v.position.x += std::sin(v.position.z * 0.05f);
        v.position.y += v.position.z * 0.02f;
    }
    Check(graph::BuildSurfaceBandGeometry(bentRoad, roadside, roadside.layouts[0].bands[1], rightMesh, error),
          "曲がりと縦断高さを持つ道路格子に沿道が追従する");
    checkBandGrid(rightMesh);
    {
        auto wideBands = roadside;
        for (auto& p : wideBands.presets) if (p.role != graph::SurfaceRole::Road) {
            float width, height;
            if (graph::GetSimpleRoadsideDimensions(p, width, height))
                graph::SetSimpleRoadsideDimensions(p, 9.5f, height);
        }
        renderer::MeshData wideMesh;
        Check(graph::BuildSurfaceBandGeometry(bentRoad, wideBands, wideBands.layouts[0].bands[1], wideMesh, error),
              "幅の広い路肩・歩道も断面の角を残して格子化する");
        checkBandGrid(wideMesh);
        Check(wideMesh.vertices.size() > bandMesh.vertices.size(), "幅を広げると横方向の列も増える");
    }
    if (!rightMesh.vertices.empty()) Check(std::abs(rightMesh.vertices.back().position.y - 1.15f) < 1e-5f,
          "沿道の高さは道路の縦断高さを基準にする");
    auto roundedRoad = bentRoad;
    roundedRoad.rowDistances[1] = 0.25000003f;
    Check(graph::BuildSurfaceBandGeometry(roundedRoad, roadside, roadside.layouts[0].bands[1], rightMesh, error),
          "等間隔点と道路格子の距離が丸め誤差で重なっても沿道を生成できる");
    auto brokenSide = roadside;
    brokenSide.layouts[0].bands[1].spans[0].blendOutMeters = 0;
    brokenSide.layouts[0].bands[1].spans[1].blendInMeters = 0;
    const auto retainedCount = rightMesh.vertices.size();
    Check(!graph::BuildSurfaceBandGeometry(bandRoad, brokenSide, brokenSide.layouts[0].bands[1], rightMesh, error) &&
          !error.empty() && rightMesh.vertices.size() == retainedCount, "移行なしの異種断面は拒否し、出力を保持する");
    graph::SurfaceLayoutDocument roadsideReloaded;
    Check(io::ReadSurfaceLayouts(roadsideJson, roadsideReloaded, error) &&
          graph::BuildSurfaceBandGeometry(bandRoad, roadsideReloaded, roadsideReloaded.layouts[0].bands[1], rightMesh, error) &&
          rightMesh.vertices.size() == bandMesh.vertices.size() && rightMesh.indices == bandMesh.indices,
          "保存した仮沿道から同じ分割の形状を再生成する");

    tests::Section("沿道材質 — 下地と区間混合");
    roadside.presets[0].materials[0].material = 13;
    roadside.presets[0].materials[0].uvRepeatMeters = 3;
    roadside.presets[0].materials[0].worldUv = true;
    roadside.presets[1].materials[0].baseColor = {0.2f, 0.3f, 0.4f};
    roadside.presets[1].materials[0].roughness = 0.67f;
    roadside.presets[1].displacementMeters = 0.2f;
    const auto bandPreview = graph::CompileSurfaceBandPreview(sceneGraph, roadside, roadId, roadside.layouts[0].bands[1].id);
    Check(bandPreview.error.empty() && bandPreview.scene.meshes.size() == 3 && renderer::ValidateMeshScene(bandPreview.scene),
          "沿道を形状1件と材質2件で描画できる構成へ変換する");
    if (bandPreview.scene.meshes.size() == 3) {
        const auto& surface = bandPreview.scene.meshes[0];
        const auto& ground = bandPreview.scene.meshes[1];
        const auto& walk = bandPreview.scene.meshes[2];
        Check(surface.connectionSources == std::array<int, 3>{1, 2, 2} && ground.materialOnly && walk.materialOnly,
              "沿道の素材参照は評価専用コンテキストを指す");
        Check(ground.materialStack->Layers()[0].material == 13 && ground.layerUvRepeat[0] == 3 && ground.layerWorldUv[0] &&
              walk.materialStack->Layers()[0].roughness == 0.67f && walk.materialStack->Layers()[0].baseColor.y == 0.3f,
              "下地の素材参照・反復長・座標・PBR定数を保持する");
        Check(surface.displacementMeters == 0 && walk.displacementMeters == 0, "断面の継ぎ目を独立した材質変位で割らない");
        Check(std::abs(surface.geometry.vertices.back().uv.x - 2.15f) < 1e-5f &&
              surface.geometry.vertices.back().roadUv.y == 50, "垂直面を含む断面長と進行方向の実距離をUVに保持する");
        const auto& mask = surface.roadMask;
        Check(mask.rgba.front() == 0 && mask.rgba[(mask.height - 1) * 4] == 255 &&
              mask.rgba[(mask.height / 2) * 4] >= 127 && mask.rgba[(mask.height / 2) * 4] <= 130,
              "断面と同じ移行位置で下地材質の重みが0から1へ変わる");
        Check(surface.geometry.indices == bandMesh.indices && surface.geometry.vertices.front().position.x == bandMesh.vertices.front().position.x,
              "材質を付けても沿道の形状は変わらない");
    }
    auto multilayerBand = roadside;
    auto& layeredGround = multilayerBand.presets[0];
    layeredGround.materials.resize(4);
    layeredGround.layerBlendRange = 0.37f;
    for (size_t slot = 1; slot < 4; ++slot) {
        auto& layer = layeredGround.materials[slot];
        layer.mask.emplace(); layer.mask->shape = graph::RoadMaskShape::Constant;
        layer.material = 13; layer.blendMode = 1; layer.heightGate = 2;
        layer.uvRepeatMeters = float(slot + 1);
    }
    const auto layeredBandPreview = graph::CompileSurfaceBandPreview(sceneGraph, multilayerBand, roadId, multilayerBand.layouts[0].bands[1].id);
    Check(layeredBandPreview.error.empty() && renderer::ValidateMeshScene(layeredBandPreview.scene), "沿道4層の合成を評価できる");
    if (layeredBandPreview.scene.meshes.size() > 1) {
        const auto& context = layeredBandPreview.scene.meshes[1];
        Check(context.layerStacks[0] && context.layerStacks[1] && context.layerStacks[2] &&
              context.layerBlendMode[3] == 1 && context.layerHeightGate[3] == 2 &&
              context.layerUvRepeat[3] == 4 && context.layerBlendRange == 0.37f &&
              context.roadWidthMeters > 0 && context.roadLengthMeters == 50 && !context.roadMask.rgba.empty(),
              "沿道の上層・ハイト競合・下地条件・反復長・マスク座標を保持する");
    }
    graph::SurfaceLayoutDocument layeredSideReload;
    layeredGround.materials[1].mask->shape = graph::RoadMaskShape::WorldNoise;
    const auto worldBandPreview = graph::CompileSurfaceBandPreview(sceneGraph, multilayerBand, roadId, multilayerBand.layouts[0].bands[1].id);
    Check(worldBandPreview.error.empty(), "沿道のワールドノイズを評価する");
    if (worldBandPreview.scene.meshes.size() > 1) {
        const auto& context = worldBandPreview.scene.meshes[1];
        const auto& mask = context.roadMask;
        const float u = 0.5f / float(mask.width), distance = 25.0f / float(mask.height);
        const auto& origin = bandRoad.surface.vertices[bandRoad.stride - 1].position;
        const float expected = graph::EvaluateRoadMask(*layeredGround.materials[1].mask,
            (u - 0.5f) * context.roadWidthMeters, distance, context.roadWidthMeters * 0.5f, 50,
            nullptr, origin.x + 2 * u, origin.z + distance, true);
        Check(mask.rgba[0] == static_cast<uint8_t>(std::lround(std::clamp(expected, 0.0f, 1.0f) * 255)),
              "ワールドノイズを道路中央ではなく左路肩の実位置で評価する");
    }
    Check(io::ReadSurfaceLayouts(io::WriteSurfaceLayouts(multilayerBand), layeredSideReload, error) &&
          io::WriteSurfaceLayouts(layeredSideReload) == io::WriteSurfaceLayouts(multilayerBand), "沿道4層を保存往復する");
    auto layeredSideScene = graph::CompileMeshGraph(sceneGraph);
    Check(graph::ConnectSurfaceBandMaterials(layeredSideScene, sceneGraph, multilayerBand, roadId,
          multilayerBand.layouts[0].bands[1].id, error, true) && renderer::ValidateMeshScene(layeredSideScene.scene),
          "沿道4層の材質を変位つき横接続に使用する");
    graph::SurfaceLayoutDocument uniformSides;
    Check(graph::CreateUniformRoadside(uniformSides, sceneGraph, roadId, graph::SurfaceSide::Left, graph::SurfaceRole::Ground, error) &&
          graph::CreateUniformRoadside(uniformSides, sceneGraph, roadId, graph::SurfaceSide::Right, graph::SurfaceRole::Ground, error),
          "左右とも全長を路肩で作成する");
    for (const auto& uniformBand : uniformSides.layouts[0].bands) if (uniformBand.side != graph::SurfaceSide::Road)
        Check(uniformBand.spans.size() == 1 && uniformBand.spans[0].startMeters == 0 && uniformBand.spans[0].endMeters == 50 &&
              graph::BuildSurfaceBandGeometry(bandRoad, uniformSides, uniformBand, bandMesh, error), "全長路肩の形状を生成する");
    graph::SurfaceLayoutDocument materialReload;
    Check(io::ReadSurfaceLayouts(io::WriteSurfaceLayouts(roadside), materialReload, error), "沿道の下地材質が保存往復する");
    const auto reloadedBand = graph::CompileSurfaceBandPreview(sceneGraph, materialReload, roadId, materialReload.layouts[0].bands[1].id);
    Check(reloadedBand.error.empty() && reloadedBand.scene.meshes[0].roadMask.rgba == bandPreview.scene.meshes[0].roadMask.rgba,
          "再読込後も沿道の材質の移行が一致する");

    tests::Section("独立したレイヤーマテリアル — 共有・形状保持・移行");
    {
        auto shared = roadside;
        Check(graph::ExtractLayerMaterials(shared, error) && graph::ValidateSurfaceLayouts(shared, error),
              "旧プリセットの材質を独立アセットへ移行する");
        const auto migrated = io::WriteSurfaceLayouts(shared);
        Check(graph::ExtractLayerMaterials(shared, error) && io::WriteSurfaceLayouts(shared) == migrated,
              "移行を繰り返してもIDと材質数が変わらない");
        const auto resolved = graph::ResolveLayerMaterials(shared);
        Check(resolved.presets[0].materials[0].material == roadside.presets[0].materials[0].material &&
              resolved.presets[1].section.back().height == roadside.presets[1].section.back().height &&
              migrated["presets"][0].contains("layerMaterial") && !migrated["presets"][0].contains("materials"),
              "材質参照を形状と分け、旧材質・段差を保つ");
        auto& spans = shared.layouts[0].bands[1].spans;
        const auto groundMaterial = graph::PresetLayerMaterial(shared, spans[0].preset);
        const auto sidewalkHeight = shared.presets[1].section.back().height;
        const auto sidewalkPreset = spans[1].preset;
        Check(graph::AssignLayerMaterial(shared, spans[1], groundMaterial) &&
              spans[1].preset == sidewalkPreset && shared.presets[1].role == graph::SurfaceRole::Sidewalk &&
              shared.presets[1].section.back().height == sidewalkHeight &&
              graph::PresetLayerMaterial(shared, spans[0].preset) == graph::PresetLayerMaterial(shared, spans[1].preset),
              "路肩の材質を歩道へ割り当てても垂直段差は変わらない");
        Check(graph::CreateRoadLayout(shared, sceneGraph, roadId, error) && graph::ExtractLayerMaterials(shared, error),
              "独立材質の文書へ路面区間を追加できる");
        auto* sharedRoad = graph::FindRoadBand(shared, roadId);
        Check(graph::AssignLayerMaterial(shared, sharedRoad->spans[0], groundMaterial) &&
              graph::ValidateSurfaceLayouts(shared, error), "路面と左右沿道で同じ材質IDを使用できる");
        auto& asset = *std::find_if(shared.layerMaterials.begin(), shared.layerMaterials.end(), [&](const auto& m) { return m.id == groundMaterial; });
        asset.materials[0].roughness = 0.23f;
        const auto sharedResolved = graph::ResolveLayerMaterials(shared);
        bool propagated = true;
        for (const auto& p : sharedResolved.presets) if (p.layerMaterial == groundMaterial)
            propagated &= p.materials[0].roughness == 0.23f;
        Check(propagated, "共有材質の編集が路面・路肩・歩道へ反映する");
        auto& sideBand = shared.layouts[0].bands[1];
        Check(graph::SetSimpleRoadsideDimensions(shared.presets[1], 3, 0.2f) && asset.materials[0].roughness == 0.23f,
              "形状の寸法変更は材質アセットを書き換えない");
        Check(graph::DuplicateLayerMaterial(shared, sideBand.spans[0]) &&
              graph::PresetLayerMaterial(shared, sideBand.spans[0].preset) != groundMaterial &&
              graph::PresetLayerMaterial(shared, sideBand.spans[1].preset) == groundMaterial &&
              graph::ValidateSurfaceLayouts(shared, error), "複製して編集は選択区間だけの材質を作る");
        const auto savedShared = io::WriteSurfaceLayouts(shared);
        graph::SurfaceLayoutDocument restored;
        Check(io::ReadSurfaceLayouts(savedShared, restored, error) && io::WriteSurfaceLayouts(restored) == savedShared &&
              graph::CompileMeshGraphWithLayouts(sceneGraph, restored).error.empty(),
              "共有材質の保存往復後も道路と沿道を評価できる");
        for (int failure = 0; failure < 3; ++failure) {
            auto bad = savedShared;
            if (failure == 0) bad["presets"][0]["layerMaterial"] = 999999;
            if (failure == 1) bad["layerMaterials"][0]["id"] = bad["presets"][0]["id"];
            if (failure == 2) bad["presets"][0]["materials"] = nlohmann::json::array();
            Check(!io::ReadSurfaceLayouts(bad, restored, error) && io::WriteSurfaceLayouts(restored) == savedShared,
                  "不正参照・重複ID・二重の材質記述を文書を変えずに拒否する");
        }
        auto exhausted = shared;
        graph::SplitSurfaceSpan(exhausted, exhausted.layouts[0].bands[1], 0);
        exhausted.nextId = std::numeric_limits<graph::SurfaceId>::max() - 1;
        const auto unchanged = io::WriteSurfaceLayouts(exhausted);
        Check(!graph::DuplicateLayerMaterial(exhausted, exhausted.layouts[0].bands[1].spans[0]) &&
              io::WriteSurfaceLayouts(exhausted) == unchanged, "ID枯渇時の材質複製は文書全体を保持する");
        DocumentSnapshot materialBefore, materialAfter;
        materialBefore.surfaceLayouts = shared;
        materialAfter = materialBefore;
        materialAfter.surfaceLayouts.layerMaterials[0].materials[0].roughness = 0.6f;
        history.Clear(); history.Push(materialBefore, 0);
        const auto materialUndo = history.Undo(materialAfter);
        Check(io::WriteSurfaceLayouts(materialUndo.surfaceLayouts) == savedShared &&
              io::WriteSurfaceLayouts(history.Redo(materialUndo).surfaceLayouts) == io::WriteSurfaceLayouts(materialAfter.surfaceLayouts),
              "共有材質の編集を参照関係とともにUndo・Redoする");
    }
    tests::Section("横接続 — 共通境界と段差の保持");
    {
        auto boundaryDoc = roadside;
        compositor::BoundaryMaterial boundary;
        boundary.id = boundaryDoc.AllocateId(); boundary.name = "舗装端";
        boundary.mask = 12; boundary.height = 13; boundary.widthMeters = 0.7f;
        boundaryDoc.boundaryMaterials.push_back(boundary);
        for (auto& span : boundaryDoc.layouts[0].bands[1].spans) span.boundaryMaterial = boundary.id;
        Check(graph::ValidateSurfaceLayouts(boundaryDoc, error), "境界マテリアルを沿道へ割り当てられる");
        const auto saved = io::WriteSurfaceLayouts(boundaryDoc);
        auto legacyBoundary = saved;
        legacyBoundary["version"] = 5;
        for (auto& legacyLayout : legacyBoundary["layouts"]) for (auto& legacyBand : legacyLayout["bands"]) {
            legacyBand["boundaryMaterial"] = legacyBand["spans"].empty() ? nlohmann::json(0) : legacyBand["spans"][0]["boundaryMaterial"];
            for (auto& span : legacyBand["spans"]) span.erase("boundaryMaterial");
        }
        graph::SurfaceLayoutDocument migratedBoundary;
        Check(io::ReadSurfaceLayouts(legacyBoundary, migratedBoundary, error) &&
              io::WriteSurfaceLayouts(migratedBoundary) == saved, "旧全体境界設定を各区間へ移行する");
        if (!error.empty()) std::printf("Boundary migration: %s\n", error.c_str());
        else if (io::WriteSurfaceLayouts(migratedBoundary) != saved)
            std::printf("Boundary migration diff: %s\n", nlohmann::json::diff(saved, io::WriteSurfaceLayouts(migratedBoundary)).dump().c_str());
        graph::SurfaceLayoutDocument restored;
        Check(saved["version"] == 6 && io::ReadSurfaceLayouts(saved, restored, error) && io::WriteSurfaceLayouts(restored) == saved,
              "境界画像・寸法・参照が保存往復する");
        auto connected = graph::CompileMeshGraph(sceneGraph);
        Check(graph::ConnectSurfaceBandMaterials(connected, sceneGraph, boundaryDoc, roadId,
              boundaryDoc.layouts[0].bands[1].id, error, true), "境界マテリアルを両面の描画へ渡せる");
        const auto& control = connected.scene.meshes[0].boundaryControl;
        Check(control.IsValid() && control.rgba == connected.scene.meshes.back().boundaryControl.rgba &&
              control.rgba.front() == 255 && control.rgba[(size_t(control.height) - 1) * 8 * 4] == 0 &&
              connected.scene.meshes[0].boundaries[0].material.mask == boundary.mask &&
              connected.scene.meshes[0].boundaries[0].center == connected.scene.meshes.back().boundaries[0].center,
              "路肩は境界画像を共有し、段差保持の歩道区間では無効にする");
        Check(graph::CreateRoadsideExample(boundaryDoc, sceneGraph, roadId, graph::SurfaceSide::Right, error), "境界の左右検証用に右沿道を追加する");
        for (auto& span : boundaryDoc.layouts[0].bands.back().spans) span.boundaryMaterial = boundary.id;
        auto both = graph::CompileMeshGraph(sceneGraph);
        Check(graph::ConnectBothSurfaceBands(both, sceneGraph, boundaryDoc, roadId,
              boundaryDoc.layouts[0].bands[1].id, boundaryDoc.layouts[0].bands.back().id, error, true) &&
              renderer::ValidateMeshScene(both.scene), "左右で同じ境界アセットを共有できる");
        Check(both.scene.meshes[0].boundaries[1].acrossSign == -1 &&
              both.scene.meshes[0].boundaryControl.rgba == both.scene.meshes.back().boundaryControl.rgba,
              "右沿道の境界方向を反転し、両側とも同じ高さ制御を使う");
        for (int failure = 0; failure < 4; ++failure) {
            auto bad = saved;
            if (failure == 0) bad["boundaryMaterials"][0]["width"] = 0;
            if (failure == 1) bad["boundaryMaterials"][0].erase("mask");
            if (failure == 2) bad["layouts"][0]["bands"][1]["spans"][0]["boundaryMaterial"] = 999999;
            if (failure == 3) bad["boundaryMaterials"][0]["id"] = bad["presets"][0]["id"];
            Check(!io::ReadSurfaceLayouts(bad, restored, error) && io::WriteSurfaceLayouts(restored) == saved,
                  "不正な境界寸法・欠落・参照・ID重複を非破壊で拒否する");
        }
        DocumentSnapshot beforeBoundary, afterBoundary;
        beforeBoundary.surfaceLayouts = restored; afterBoundary = beforeBoundary;
        afterBoundary.surfaceLayouts.boundaryMaterials[0].depthMeters = 0.08f;
        afterBoundary.surfaceLayouts.layouts[0].bands[1].spans[0].boundaryMaterial = 0;
        history.Clear(); history.Push(beforeBoundary, 0);
        const auto undo = history.Undo(afterBoundary);
        Check(io::WriteSurfaceLayouts(undo.surfaceLayouts) == saved &&
              io::WriteSurfaceLayouts(history.Redo(undo).surfaceLayouts) == io::WriteSurfaceLayouts(afterBoundary.surfaceLayouts),
              "共有する境界の深さ変更をUndo・Redoする");
        auto spanDoc = boundaryDoc;
        auto& boundaryBand = spanDoc.layouts[0].bands[1];
        boundaryBand.spans.resize(1);
        boundaryBand.spans[0].endMeters = 50;
        Check(graph::SplitSurfaceSpan(spanDoc, boundaryBand, 0), "境界付き区間を分割する");
        Check(boundaryBand.spans[1].boundaryMaterial == boundary.id, "分割先に境界を引き継ぐ");
        auto alternate = boundary;
        alternate.id = spanDoc.AllocateId(); alternate.name = "別の境界"; alternate.depthMeters = 0.09f;
        spanDoc.boundaryMaterials.push_back(alternate);
        boundaryBand.spans[1].boundaryMaterial = alternate.id;
        graph::EnsureRoadsideTransitions(boundaryBand);
        Check(boundaryBand.spans[0].blendOutMeters > 0 && boundaryBand.spans[1].blendInMeters > 0,
              "同じ路肩素材でも境界変更には移行距離を用意する");
        for (bool none : {false, true}) {
            boundaryBand.spans[1].boundaryMaterial = none ? 0 : alternate.id;
            auto mixed = graph::CompileMeshGraph(sceneGraph);
            Check(graph::ConnectSurfaceBandMaterials(mixed, sceneGraph, spanDoc, roadId, boundaryBand.id, error, true),
                  "区間ごとの境界またはなしを接続する");
            const auto& mask = mixed.scene.meshes[0].boundaryControl;
            if (!mask.IsValid()) { Check(false, "区間の境界制御画像が有効"); continue; }
            const size_t middle = (mask.height / 2) * 8 * 4;
            const auto firstWeight = mask.rgba[middle], secondWeight = mask.rgba[middle + 4];
            Check(firstWeight > 100 && firstWeight < 155 && (none ? secondWeight == 0 : firstWeight + secondWeight >= 254),
                  "切替位置で境界を半分ずつ混ぜ、なしでは溝の重みを減らす");
            Check(mask.rgba == mixed.scene.meshes.back().boundaryControl.rgba,
                  "区間切替中も道路と沿道の制御を一致させる");
            const size_t lastRow = (size_t(mask.height) - 1) * 8 * 4;
            Check(mask.rgba.front() == 255 && mask.rgba[lastRow] == 0 &&
                  mask.rgba[lastRow + 4] == (none ? 0 : 255), "境界の始終端は指定した区間だけに適用する");
        }
        auto capacityDoc = spanDoc;
        auto& capacityBand = capacityDoc.layouts[0].bands[1];
        const auto seedSpan = capacityBand.spans.front();
        capacityBand.spans.clear();
        for (int i = 0; i < 9; ++i) {
            auto asset = boundary; asset.id = capacityDoc.AllocateId();
            capacityDoc.boundaryMaterials.push_back(asset);
            auto part = seedSpan; part.id = capacityDoc.AllocateId(); part.boundaryMaterial = asset.id;
            part.startMeters = float(i * 5); part.endMeters = part.startMeters + 5;
            part.blendInMeters = part.blendOutMeters = 0;
            capacityBand.spans.push_back(part);
            if (i == 7) Check(graph::ValidateSurfaceLayouts(capacityDoc, error), "片側8種類の境界を受け付ける");
        }
        Check(!graph::ValidateSurfaceLayouts(capacityDoc, error), "描画上限を超える9種類目は拒否する");
    }
    auto lateralScene = graph::CompileMeshGraph(sceneGraph);
    const size_t originalMeshes = lateralScene.scene.meshes.size();
    const auto lateralBandId = roadside.layouts[0].bands[1].id;
    Check(graph::ConnectSurfaceBandMaterials(lateralScene, sceneGraph, roadside, roadId, lateralBandId, error) &&
          renderer::ValidateMeshScene(lateralScene.scene), "道路1構成と左沿道2構成の材質を接続する");
    if (lateralScene.scene.meshes.size() == originalMeshes + 4) {
        const auto& roadSurface = lateralScene.scene.meshes[0];
        const auto& sideSurface = lateralScene.scene.meshes.back();
        Check(roadSurface.connectionSources == sideSurface.connectionSources &&
              roadSurface.connectionOrigins[1].x == bandRoad.settings.widthMeters &&
              roadSurface.roadWidthMeters == sideSurface.roadWidthMeters,
              "道路と沿道が同じ材質参照・境界座標・マスク縮尺を使う");
        const auto& a = roadSurface.roadMask; const auto& b = sideSurface.roadMask;
        bool sameGround = true;
        for (uint32_t x = 0; x < a.width * 4; ++x) sameGround &= a.rgba[x] == b.rgba[x];
        Check(sameGround, "馴染ませる路肩区間では両面の境界比率が完全に一致する");
        const uint32_t edge = static_cast<uint32_t>(bandRoad.settings.widthMeters / roadSurface.roadWidthMeters * float(a.width));
        const auto roadEnd = (size_t(a.height - 1) * a.width + edge) * 4;
        Check(a.rgba[roadEnd] == 0 && a.rgba[roadEnd + 1] == 0 && b.rgba[roadEnd + 1] == 255,
              "段差を残す歩道は道路へ滲ませず、歩道側の材質を保持する");
        Check(a.rgba[edge * 4] > 0 && a.rgba[edge * 4] < 255,
              "路肩との境界では両側の材質が混ざる");
        Check(roadSurface.displacementMeters == 0 && sideSurface.displacementMeters == 0 &&
              lateralScene.scene.meshes[originalMeshes].displacementMeters == 0,
              "材質接続の試作では道路と沿道の変位を停止する");
        Check(lateralScene.scene.meshes[1].displacementSource == 0 &&
              lateralScene.scene.meshes[1].geometry.indices == normalScene.scene.meshes[1].geometry.indices,
              "白線の形状と参照先は材質接続後も保持する");
    }
    auto unsupportedScene = normalScene;
    const auto countBefore = unsupportedScene.scene.meshes.size();
    Check(!graph::ConnectSurfaceBandMaterials(unsupportedScene, sceneGraph, roadside, roadId, lateralBandId, error) &&
          unsupportedScene.scene.meshes.size() == countBefore &&
          unsupportedScene.scene.meshes[0].connectionSources == normalScene.scene.meshes[0].connectionSources,
          "複数の道路プリセットは部分的に接続せずシーンを保持する");
    auto wrongSideScene = graph::CompileMeshGraph(sceneGraph);
    Check(graph::ConnectSurfaceBandMaterials(wrongSideScene, sceneGraph, rightSide, roadId,
          rightSide.layouts[0].bands[1].id, error, true), "右側にも材質と変位を接続できる");
    if (wrongSideScene.scene.meshes.size() == originalMeshes + 4) {
        const auto& r = wrongSideScene.scene.meshes[0];
        const auto& rightSurface = wrongSideScene.scene.meshes.back();
        Check(r.connectionAcrossSigns[0] == -1 && r.connectionFrameSign == -1 && rightSurface.connectionFrameSign == 1,
              "右接続は道路の素材座標と接線の反転を明示する");
        Check(r.connectionHeightFade.x == bandRoad.settings.widthMeters && renderer::ValidateMeshScene(wrongSideScene.scene),
              "右接続も同じ境界高さへ変位を減衰する");
        const auto original = graph::CompileMeshGraph(sceneGraph);
        bool preserved = true;
        for (size_t m = 0; m < originalMeshes; ++m) {
            for (size_t i = 0; i < original.scene.meshes[m].geometry.vertices.size(); ++i) {
                const auto a = original.scene.meshes[m].geometry.vertices[i].roadUv;
                const auto b = wrongSideScene.scene.meshes[m].geometry.vertices[i].roadUv;
                preserved &= std::abs((r.connectionOrigins[0].x - b.x) - a.x * original.scene.meshes[0].roadMetersPerUv) < 1e-5f;
            }
        }
        Check(preserved, "右接続後も道路と白線の素材参照位置を保持する");
        Check(rightSurface.geometry.vertices.front().roadUv.x == r.connectionHeightFade.x, "右路肩の内端も共通境界座標に一致する");
    }
    auto axisGraph = sceneGraph;
    std::get<graph::RoadNodeSettings>(axisGraph.FindMutableNode(roadId)->settings).uvAlongU = true;
    auto axisScene = graph::CompileMeshGraph(axisGraph);
    Check(graph::ConnectSurfaceBandMaterials(axisScene, axisGraph, roadside, roadId, lateralBandId, error),
          "道路のUVが長さ方向Uでも横接続を生成できる");
    if (!axisScene.scene.meshes.empty()) {
        const auto& axisRoad = axisScene.scene.meshes[0];
        Check(std::abs(axisRoad.geometry.vertices.back().roadUv.x - bandRoad.settings.widthMeters) < 1e-5f &&
              std::abs(axisRoad.geometry.vertices.back().roadUv.y - 50) < 1e-5f && !axisRoad.roadUvAlongU,
              "元のUV軸に依存せず横距離・進行距離の共通座標へ変換する");
        Check(axisScene.meshSources.size() == axisScene.scene.meshes.size(), "追加後も描画メッシュと由来の配列が対応する");
    }

    tests::Section("左右同時接続 — 独立した区間と共通座標");
    auto bothDocument = roadside;
    Check(graph::CreateRoadsideExample(bothDocument, sceneGraph, roadId, graph::SurfaceSide::Right, error),
          "既存の左沿道を保って右沿道を追加する");
    auto& rightBand = bothDocument.layouts[0].bands.back();
    rightBand.spans[0].endMeters = rightBand.spans[1].startMeters = 18;
    auto bothScene = graph::CompileMeshGraph(sceneGraph);
    Check(graph::ConnectBothSurfaceBands(bothScene, sceneGraph, bothDocument, roadId, lateralBandId, rightBand.id, error, true),
          "左右の異なる区切りを持つ沿道を同時に接続する");
    if (bothScene.scene.meshes.size() == originalMeshes + 7) {
        const auto& bothRoad = bothScene.scene.meshes[0];
        const auto& leftSurface = bothScene.scene.meshes[originalMeshes + 3];
        const auto& rightSurface = bothScene.scene.meshes.back();
        Check(renderer::ValidateMeshScene(bothScene.scene) && bothRoad.connectionExtraSources[0] >= 0,
              "左右同時接続の5材質参照が有効");
        Check(bothRoad.connectionSources == rightSurface.connectionSources &&
              bothRoad.connectionExtraSources == leftSurface.connectionExtraSources &&
              bothRoad.roadWidthMeters == rightSurface.roadWidthMeters,
              "道路と両沿道が同じ材質参照とマスク縮尺を共有する");
        Check(std::abs(leftSurface.geometry.vertices.front().roadUv.x - bothRoad.connectionHeightFade.x) < 1e-5f &&
              std::abs(rightSurface.geometry.vertices.front().roadUv.x - bothRoad.connectionSecondHeightFade.x) < 1e-5f,
              "左右の境界がそれぞれ共通の変位減衰位置に一致する");
        bool sharedGround = true;
        for (size_t i = 0; i < size_t(bothRoad.roadMask.width) * 4; ++i)
            sharedGround &= bothRoad.roadMask.rgba[i] == leftSurface.roadMask.rgba[i] &&
                            bothRoad.roadMask.rgba[i] == rightSurface.roadMask.rgba[i];
        Check(sharedGround, "両側が路肩の区間は3面の混合率が一致する");
        const auto original = graph::CompileMeshGraph(sceneGraph);
        bool whiteUv = true;
        for (size_t i = 0; i < original.scene.meshes[1].geometry.vertices.size(); ++i)
            whiteUv &= std::abs(bothScene.scene.meshes[1].geometry.vertices[i].roadUv.x - bothRoad.connectionOrigins[0].x -
                original.scene.meshes[1].geometry.vertices[i].roadUv.x * original.scene.meshes[0].roadMetersPerUv) < 1e-5f;
        Check(whiteUv, "左右同時接続後も白線が元の道路位置を参照する");
        auto invalidBoth = bothScene.scene;
        invalidBoth.meshes[0].connectionExtraSources[1] = -1;
        Check(!renderer::ValidateMeshScene(invalidBoth), "片方だけ欠けた追加材質参照を拒否する");
    }
    auto failedBoth = graph::CompileMeshGraph(sceneGraph);
    const auto beforeBoth = failedBoth.scene.meshes.size();
    Check(!graph::ConnectBothSurfaceBands(failedBoth, sceneGraph, bothDocument, roadId, lateralBandId, lateralBandId, error, true) &&
          failedBoth.scene.meshes.size() == beforeBoth && failedBoth.scene.meshes[0].connectionSources[0] == -1,
          "左右の指定が不正なら途中の接続を残さない");

    tests::Section("道路の3プリセットと沿道の同時接続");
    auto multiDocument = layered;
    auto& multiBands = multiDocument.layouts[0].bands;
    std::erase_if(multiBands, [](const auto& band) { return band.side != graph::SurfaceSide::Road; });
    Check(graph::CreateRoadsideExample(multiDocument, sceneGraph, roadId, graph::SurfaceSide::Left, error), "複数路面の左沿道を作る");
    const auto multiLeft = multiDocument.layouts[0].bands.back().id;
    Check(graph::CreateRoadsideExample(multiDocument, sceneGraph, roadId, graph::SurfaceSide::Right, error), "複数路面の右沿道を作る");
    const auto multiRight = multiDocument.layouts[0].bands.back().id;
    const auto originalMulti = graph::CompileMeshGraphWithLayouts(sceneGraph, multiDocument);
    for (bool bothSides : {false, true}) {
        auto connectedMulti = originalMulti;
        Check(graph::ConnectSurfaceLayoutBands(connectedMulti, sceneGraph, multiDocument, roadId,
              bothSides ? multiLeft : 0, multiRight, error, true), "道路3構成を右片側・左右両側へ接続する");
        const auto& multiRoad = connectedMulti.scene.meshes[0];
        if (multiRoad.connectionRoadMixSource >= 0) {
            Check(renderer::ValidateMeshScene(connectedMulti.scene), "道路区間付きの全接続参照が有効");
            Check(connectedMulti.scene.meshes[multiRoad.connectionRoadMixSource].roadMask.rgba == originalMulti.scene.meshes[0].roadMask.rgba,
                  "道路の進行方向の混合率を変えずに保持する");
            const auto& originalContext = originalMulti.scene.meshes[originalMulti.scene.meshes[0].connectionSources[1]];
            const auto& connectedContext = connectedMulti.scene.meshes[multiRoad.connectionRoadSources[0]];
            Check(connectedContext.layerUvRepeat == originalContext.layerUvRepeat &&
                  connectedContext.displacementMeters == originalContext.displacementMeters,
                  "追加した道路プリセットのUVと変位量を維持する");
            if (!bothSides) {
                bool emptyExtra = true;
                for (size_t i = 0; i < multiRoad.roadMask.rgba.size(); i += 4)
                    emptyExtra &= multiRoad.roadMask.rgba[i + 2] == 0 && multiRoad.roadMask.rgba[i + 3] == 0;
                Check(emptyExtra, "片側接続の未使用マスクを道路の被覆に混ぜない");
            }
            auto brokenMix = connectedMulti.scene;
            brokenMix.meshes[0].connectionRoadSources[1] = -1;
            Check(!renderer::ValidateMeshScene(brokenMix), "不足した道路プリセット参照を拒否する");
        } else Check(false, "道路の区間混合参照を生成する");
    }

    tests::Section("横接続の変位 — 共通の高さ基準と白線UV");
    auto displacedScene = graph::CompileMeshGraph(axisGraph);
    const auto oldRoad = displacedScene.scene.meshes[0];
    const auto oldMarking = displacedScene.scene.meshes[1];
    Check(graph::ConnectSurfaceBandMaterials(displacedScene, axisGraph, roadside, roadId, lateralBandId, error, true),
          "変位を有効にした横接続を生成できる");
    if (displacedScene.scene.meshes.size() == originalMeshes + 4) {
        const auto& roadMesh = displacedScene.scene.meshes[0];
        const auto& sideMesh = displacedScene.scene.meshes.back();
        Check(roadMesh.displacementMeters == 1 && sideMesh.displacementMeters == 1 &&
              roadMesh.connectionPrototype && sideMesh.connectionPrototype,
              "道路・沿道の変位方向を世界Yに揃える");
        Check(roadMesh.connectionHeightFade.x == bandRoad.settings.widthMeters &&
              roadMesh.connectionHeightFade.x == sideMesh.connectionHeightFade.x &&
              roadMesh.connectionHeightFade.y == sideMesh.connectionHeightFade.y && roadMesh.connectionHeightFade.y > 0,
              "両面に同じ境界位置と変位抑制幅を渡す");
        Check(displacedScene.scene.meshes[originalMeshes + 2].displacementMeters == roadside.presets[1].displacementMeters,
              "境界から離れた歩道の変位量はプリセットから取得する");
        bool uvMatches = true;
        for (size_t i = 0; i < oldMarking.geometry.vertices.size(); ++i) {
            const auto oldUv = oldMarking.geometry.vertices[i].roadUv;
            const auto uv = displacedScene.scene.meshes[1].geometry.vertices[i].roadUv;
            uvMatches &= std::abs(uv.x - oldUv.y * oldRoad.roadMetersPerUv) < 1e-5f &&
                         std::abs(uv.y - oldUv.x * oldRoad.roadMetersPerUv) < 1e-5f;
        }
        Check(uvMatches, "白線も道路と同じ実距離UVから変位を評価する");
        auto invalidScene = displacedScene.scene;
        invalidScene.meshes[0].connectionHeightFade.y = std::numeric_limits<float>::quiet_NaN();
        Check(!renderer::ValidateMeshScene(invalidScene), "不正な変位抑制幅をGPUへ渡さない");
    }

    tests::Section("プリセットグラフ — 移行・結線・保存・Undo");
    {
        auto graphDocument = layered;
        auto& graphPreset = graphDocument.presets.front();
        graphPreset.materialGraph = graph::MakePresetGraph(graphPreset.materials);
        std::vector<graph::PresetMaterial> flattened;
        Check(graph::CompilePresetMaterials(graphPreset, flattened, error), "旧4層をグラフへ変換できる");
        auto flatDocument = graphDocument;
        flatDocument.presets.front().materialGraph.reset();
        flatDocument.presets.front().materials = flattened;
        Check(io::WriteSurfaceLayouts(flatDocument) == io::WriteSurfaceLayouts(layered),
              "旧材質・マスク・ハイト合成条件を変えずにグラフから復元する");
        const auto graphJson = io::WriteSurfaceLayouts(graphDocument);
        graph::SurfaceLayoutDocument restored;
        Check(graphJson["version"] == 6 && io::ReadSurfaceLayouts(graphJson, restored, error) &&
              io::WriteSurfaceLayouts(restored) == graphJson, "ノードID・結線・配置・設定が保存往復する");
        const auto graphPreview = graph::CompileSurfaceLayoutPreview(sceneGraph, graphDocument, linked.layouts[0].roadNode);
        Check(graphPreview.error.empty() && graphPreview.scene.meshes.size() == layeredPreview.scene.meshes.size() &&
              graphPreview.scene.meshes[1].roadMask.rgba == layeredPreview.scene.meshes[1].roadMask.rgba,
              "グラフの道路評価が旧レイヤーと同じマスクを生成する");
        auto& nodes = *graphPreset.materialGraph;
        const auto output = nodes.nodes.back().id;
        const auto lastBlend = nodes.nodes.back().inputs[0];
        Check(!graph::ConnectPresetNodes(nodes, lastBlend, lastBlend, 0, error), "自己循環を拒否する");
        uint32_t mask = 0;
        for (const auto& n : nodes.nodes) if (n.kind == graph::PresetNodeKind::Mask) mask = n.id;
        Check(!graph::ConnectPresetNodes(nodes, mask, output, 0, error), "マスクを面出力へ接続できない");
        const auto extra = graph::AddPresetNode(nodes, graph::PresetNodeKind::Blend, {40, 50});
        Check(graph::ConnectPresetNodes(nodes, lastBlend, extra, 0, error) &&
              graph::ConnectPresetNodes(nodes, nodes.nodes.front().id, extra, 1, error) &&
              !graph::ConnectPresetNodes(nodes, mask, extra, 2, error), "5層目となる接続を拒否する");
        Check(graph::DeletePresetNode(nodes, extra) && !graph::DeletePresetNode(nodes, output),
              "追加ノードは削除でき、出力ノードは保持する");
        Check(graph::ConnectPresetNodes(nodes, nodes.nodes.front().id, output, 0, error) &&
              graph::CompilePresetMaterials(graphPreset, flattened, error) && flattened.size() == 1,
              "出力を下地素材へ結び替えると合成を迂回する");
        auto brokenGraph = graphJson; brokenGraph["layerMaterials"][0]["materialGraph"]["nodes"][0]["id"] = 0;
        Check(!io::ReadSurfaceLayouts(brokenGraph, restored, error) && io::WriteSurfaceLayouts(restored) == graphJson,
              "破損グラフの読込は文書を変更しない");
        DocumentSnapshot graphBefore, graphAfter;
        graphBefore.surfaceLayouts = restored; graphAfter.surfaceLayouts = graphDocument;
        UndoHistory graphHistory; graphHistory.Push(graphBefore, 0);
        const auto undoGraph = graphHistory.Undo(graphAfter);
        Check(io::WriteSurfaceLayouts(undoGraph.surfaceLayouts) == graphJson, "結線変更をUndoする");
        Check(io::WriteSurfaceLayouts(graphHistory.Redo(undoGraph).surfaceLayouts) == io::WriteSurfaceLayouts(graphDocument),
              "結線変更をRedoする");
        auto graphRoadside = multilayerBand;
        for (auto& p : graphRoadside.presets) p.materialGraph = graph::MakePresetGraph(p.materials);
        Check(graph::ValidateSurfaceLayouts(graphRoadside, error), "沿道も同じグラフへ移行できる");
        const auto graphBand = graph::CompileSurfaceBandPreview(sceneGraph, graphRoadside, roadId, graphRoadside.layouts[0].bands[1].id);
        Check(graphBand.error.empty() && graphBand.scene.meshes.size() == worldBandPreview.scene.meshes.size() &&
              graphBand.scene.meshes[1].roadMask.rgba == worldBandPreview.scene.meshes[1].roadMask.rgba,
              "グラフの沿道評価がワールド座標のマスクを維持する");
        auto simple = graph::MakePresetGraph({graph::PresetMaterial{}});
        Check(graph::AppendPresetLayer(simple, error) && graph::AppendPresetLayer(simple, error) &&
              graph::AppendPresetLayer(simple, error) && !graph::AppendPresetLayer(simple, error),
              "層の追加が素材・マスク・合成を結線し、4層で止まる");
    }


    {
        auto visibility = document;
        auto& visibilityPreset = visibility.presets[0];
        auto upper = visibilityPreset.materials[0];
        upper.material = 42; upper.mask.emplace(); upper.mask->shape = graph::RoadMaskShape::WorldNoise;
        upper.mask->strength = 0.7f; upper.enabled = false;
        visibilityPreset.materials.push_back(upper);
        std::vector<graph::PresetMaterial> evaluated;
        auto reordered = visibility;
        std::swap(reordered.presets[0].materials[0], reordered.presets[0].materials[1]);
        reordered.presets[0].materials[0].enabled = true;
        Check(graph::ValidateSurfaceLayouts(reordered, error) &&
              graph::CompilePresetMaterials(reordered.presets[0], evaluated, error) &&
              evaluated[0].material == 42 && !evaluated[0].mask && reordered.presets[0].materials[0].mask,
              "上層を下地へ移動でき、マスクは保存して評価時だけ全面にする");
        std::swap(reordered.presets[0].materials[0], reordered.presets[0].materials[1]);
        Check(graph::CompilePresetMaterials(reordered.presets[0], evaluated, error) && evaluated[1].mask && evaluated[1].mask->strength == 0.7f,
              "下地から上層へ戻した素材は元のマスクを使う");
        Check(graph::CompilePresetMaterials(visibilityPreset, evaluated, error) && evaluated.size() == 2 && !evaluated[1].mask,
              "非表示レイヤーはマスクを評価へ渡さない");
        const auto saved = io::WriteSurfaceLayouts(visibility);
        graph::SurfaceLayoutDocument restored;
        Check(io::ReadSurfaceLayouts(saved, restored, error) && io::WriteSurfaceLayouts(restored) == saved &&
              !restored.layerMaterials[0].materials[1].enabled && restored.layerMaterials[0].materials[1].mask->strength == 0.7f,
              "非表示の素材とマスクを失わず保存往復する");
        restored.layerMaterials[0].materials[1].enabled = true;
        Check(graph::CompilePresetMaterials(restored.layerMaterials[0], evaluated, error) && evaluated[1].mask && evaluated[1].material == 42,
              "再表示すると元の素材とマスクへ戻る");
        visibilityPreset.materials[0].enabled = false;
        Check(graph::CompilePresetMaterials(visibilityPreset, evaluated, error) && evaluated[0].material == 0,
              "下地の非表示は安全な定数材質を使う");
        visibilityPreset.materialGraph = graph::MakePresetGraph(visibilityPreset.materials);
        Check(graph::CompilePresetMaterials(visibilityPreset, evaluated, error) && evaluated[0].material == 0 && !evaluated[1].mask,
              "グラフ形式でもレイヤーの非表示が一致する");
        auto malformed = saved;
        malformed["layerMaterials"][0]["materials"][1]["enabled"] = "false";
        const auto unchanged = io::WriteSurfaceLayouts(restored);
        Check(!io::ReadSurfaceLayouts(malformed, restored, error) && io::WriteSurfaceLayouts(restored) == unchanged,
              "表示フラグの型が壊れた保存データは非破壊で拒否する");
    }
}
