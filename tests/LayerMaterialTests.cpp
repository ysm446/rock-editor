#include "graph/SurfacePresetGraph.h"
#include "io/LayerMaterialIo.h"
#include "io/ProjectWorkspace.h"
#include <iostream>
#include "TestSupport.h"
#include "app/UndoHistory.h"
#include <limits>
using nlohmann::json;
void RunLayerMaterialTests() {
    int failures = 0;
    const auto check = [&](bool value, const char* label) { if (!value) { ++failures; std::cerr << label << '\n'; } };
    rock::graph::LayerMaterial layer;
    layer.name = "Layer test"; layer.displacementMeters = 0.15f;
    layer.materials.emplace_back(); layer.materials[0].material = 7;
    layer.materialGraph = rock::graph::MakePresetGraph(layer.materials);
    std::string error;
    check(rock::graph::AppendPresetLayer(*layer.materialGraph, error), "append layer");
    std::vector<rock::graph::PresetMaterial> compiled;
    check(rock::graph::CompilePresetMaterials(layer, compiled, error) && compiled.size() == 2, "compile linked layer");
    const auto validGraph = *layer.materialGraph;
    const auto blend = layer.materialGraph->nodes.back().id;
    check(!rock::graph::ConnectPresetNodes(*layer.materialGraph, blend, blend, 0, error), "reject cycle");
    check(layer.materialGraph->nodes.back().inputs == validGraph.nodes.back().inputs, "failed connection is atomic");
    auto body = rock::io::WriteLayerMaterial(layer);
    rock::graph::LayerMaterial restored;
    check(rock::io::ReadLayerMaterial(body, restored, error), "read material");
    check(rock::io::WriteLayerMaterial(restored) == body, "graph and settings roundtrip");
    auto broken = body; broken["materialGraph"]["nodes"][0]["id"] = -1;
    const auto before = rock::io::WriteLayerMaterial(restored);
    check(!rock::io::ReadLayerMaterial(broken, restored, error) && rock::io::WriteLayerMaterial(restored) == before, "bad ID rejected atomically");
    broken = body; broken["displacement"] = "invalid";
    check(!rock::io::ReadLayerMaterial(broken, restored, error), "bad number rejected");
    broken = body; broken["materialGraph"]["nodes"][1]["inputs"] = json::array({999,0,0});
    check(!rock::io::ReadLayerMaterial(broken, restored, error), "dangling link rejected");
    auto withoutMask = layer;
    for (auto& node : withoutMask.materialGraph->nodes) if (node.kind == rock::graph::PresetNodeKind::Blend) node.inputs[2] = 0;
    check(rock::graph::CompilePresetMaterials(withoutMask, compiled, error) && compiled.size() == 1, "unconnected mask adds no coverage");
    check(rock::graph::AppendPresetLayer(*layer.materialGraph, error) && rock::graph::AppendPresetLayer(*layer.materialGraph, error), "four layers");
    check(!rock::graph::AppendPresetLayer(*layer.materialGraph, error), "fifth layer rejected");

    // 編集用変換では、非表示の素材・マスクも再表示できるよう保持する。
    auto hidden = layer;
    for (auto& node : hidden.materialGraph->nodes) if (node.kind == rock::graph::PresetNodeKind::Material) node.settings.enabled = false;
    std::vector<rock::graph::PresetMaterial> editable;
    check(rock::graph::ExtractPresetLayers(hidden, editable, error) && editable.size() == 4, "extract editable layers");
    check(!editable.front().enabled && editable.front().material == 7 && !editable.back().enabled && editable.back().mask.has_value(), "hidden settings retained");
    std::vector<rock::graph::PresetMaterial> beforeConversion, afterConversion;
    check(rock::graph::CompilePresetMaterials(hidden, beforeConversion, error), "compile hidden graph");
    hidden.materialGraph.reset(); hidden.materials = editable;
    check(rock::graph::CompilePresetMaterials(hidden, afterConversion, error), "compile converted stack");
    auto beforeLayer = hidden; beforeLayer.materials = beforeConversion;
    auto afterLayer = hidden; afterLayer.materials = afterConversion;
    check(rock::io::WriteLayerMaterial(beforeLayer) == rock::io::WriteLayerMaterial(afterLayer), "conversion preserves evaluated materials");
    auto stackBody = rock::io::WriteLayerMaterial(hidden);
    check(rock::io::ReadLayerMaterial(stackBody, restored, error) && rock::io::WriteLayerMaterial(restored) == stackBody, "hidden layer stack roundtrip");

    auto invalidMask = stackBody;
    invalidMask["materials"][1]["mask"]["shape"] = 0;
    check(!rock::io::ReadLayerMaterial(invalidMask, restored, error), "road mask shapes are rejected");
    auto invalidCount = hidden;
    invalidCount.materials.emplace_back();
    check(!rock::graph::CompilePresetMaterials(invalidCount, compiled, error), "five stack layers rejected");
    auto invalidScale = hidden;
    invalidScale.materials[1].mask->noiseScaleMeters = .01f;
    check(!rock::graph::CompilePresetMaterials(invalidScale, compiled, error), "invalid mask size rejected before GPU evaluation");
    rock::DocumentSnapshot snapshot;
    snapshot.materials.emplace_back(); snapshot.materials.back().layerMaterial = hidden;
    rock::UndoHistory history; history.Push(snapshot, 0);
    snapshot.materials.back().layerMaterial->materials[1].mask->seed = 99;
    auto undone = history.Undo(snapshot);
    check(undone.materials.back().layerMaterial->materials[1].mask->seed != 99, "layer settings undo independently");
    check(history.Redo(undone).materials.back().layerMaterial->materials[1].mask->seed == 99, "layer settings redo");

    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "rock-editor-tests" / "layer-material-test-data";
    fs::create_directories(root);
    rock::io::ProjectWorkspace workspace;
    check(workspace.Open(root), "open workspace");
    auto sourcePath = workspace.UniquePath(root, "source", ".rockmat");
    json source = {{"name", "Source"}, {"maps", json::object()}};
    check(workspace.SaveAsset(sourcePath, "material-asset", source), "save source");
    const auto ref = workspace.Reference(sourcePath);
    rock::io::MapLayerMaterials(body, [&](const json& id) -> json { return id == 7 ? ref : json(0); });
    auto path = workspace.UniquePath(root, "layer", ".tglayer");
    check(workspace.SaveAsset(path, "layer-material-asset", body), "save layer asset");
    check(workspace.Scan(), "scan layer UID");
    json document = {{"materials", json::array({{{"id", 1}, {"asset", workspace.Reference(path)}}})}};
    check(workspace.Expand(document), "expand shared layer and dependencies");
    check(document["materials"].size() == 2 && document["materials"][0]["materials"][0]["material"] == 2, "dependency IDs remapped");
    check(document["materials"][0]["materialGraph"]["nodes"][0]["settings"]["material"] == 2, "graph refs remapped");
    const auto moved = workspace.UniquePath(root, "moved", ".rockmat");
    std::error_code ec; fs::rename(sourcePath, moved, ec);
    check(!ec && workspace.Scan(), "rename source");
    document = {{"materials", json::array({{{"id", 1}, {"asset", workspace.Reference(path)}}})}};
    check(workspace.Expand(document), "UID survives source rename");
    // 新しい数値IDから保存し直しても依存が通常素材として残る。
    document["textures"] = json::array(); document["models"] = json::array(); document["skies"] = json::array();
    auto scene = workspace.UniquePath(root, "scene", ".rockscene");
    check(workspace.SaveScene(scene, document), "scene save with layer before source");
    check(workspace.ReadScene(scene, document), "scene reload");
    check(document["materials"][0]["materials"][0]["material"] == 2, "scene reference preserved");
    json invalid = body;
    rock::io::MapLayerMaterials(invalid, [&](const json&) -> json { return workspace.Reference(path); });
    auto nested = workspace.UniquePath(root, "nested", ".tglayer");
    check(workspace.SaveAsset(nested, "layer-material-asset", invalid), "save unsupported nesting fixture");
    document = {{"materials", json::array({{{"id", 1}, {"asset", workspace.Reference(nested)}}})}};
    check(!workspace.Expand(document), "nested layer dependency rejected");
    std::cout << "Layer material failures: " << failures << '\n';
    rock::tests::g_failures += failures;
}
