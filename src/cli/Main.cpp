// rock_cli — GPU とウィンドウを使わずに岩グラフを扱うコンソールの道具。
//
// LLM（アプリの外で動くエージェント）がグラフを書き、結果を数値で確かめるための入口。
// アプリと同じ評価器（graph::EvaluateRocks）と保存形式の読み込み（io::ReadGraph）を使う。
// 出力は標準出力への JSON 1 つ。ログは標準エラーへ出る。
//
//   rock_cli eval <graph.rockgraph> [--node <id>] [--mesher dc|mt] [--pretty]
//   rock_cli catalog [--pretty]
//
// 終了コード: 0 = 成功、1 = 評価エラーか、読み込みで捨てたノード・リンクがある、2 = 読み込めない・引数の誤り。
// 仕様は docs/reference/ai-authoring.md。
#include "core/PathUtf8.h"
#include "geometry/Mesh.h"
#include "graph/NodeGraph.h"
#include "graph/RockEvaluator.h"
#include "io/GraphIo.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using nlohmann::json;
namespace fs = std::filesystem;
using namespace rock;

constexpr int kExitOk = 0;
constexpr int kExitEvaluationError = 1;
constexpr int kExitUsage = 2;

void WriteJson(const json& document, bool pretty) {
    const std::string text = document.dump(pretty ? 2 : -1, ' ', false, json::error_handler_t::replace) + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

int Fail(const std::string& message, bool pretty) {
    WriteJson({{"ok", false}, {"error", {{"message", message}}}}, pretty);
    return kExitUsage;
}

const char* KindName(graph::NodeKind kind) {
    const graph::NodeDefinition* definition = graph::FindNodeDefinition(kind);
    return definition ? definition->name : "?";
}

const char* ValueTypeName(graph::ValueType type) {
    switch (type) {
    case graph::ValueType::MeshOrPieces: return "meshOrPieces";
    case graph::ValueType::Planes: return "planes";
    case graph::ValueType::Mask: return "mask";
    case graph::ValueType::Points: return "points";
    case graph::ValueType::Pieces: return "pieces";
    case graph::ValueType::Selection: return "selection";
    case graph::ValueType::Material: return "material";
    case graph::ValueType::Mesh: return "mesh";
    case graph::ValueType::Model: return "model";
    case graph::ValueType::Any: return "any";
    case graph::ValueType::Boxes: return "boxes";
    case graph::ValueType::Volume: return "volume";
    case graph::ValueType::Preview: return "preview";
    case graph::ValueType::Rock: return "rock";
    case graph::ValueType::Instances: return "instances";
    }
    return "?";
}

// GPU で作るデータ（素材のハイト）が無いと評価できない種類。CLI では評価に失敗する。
bool NeedsGpu(const graph::Node& node) {
    return node.kind == graph::NodeKind::Displace;
}

// target から上流を辿り、GPU が要るノードを集める。
std::vector<graph::GraphId> UpstreamGpuNodes(const graph::NodeGraph& graph, graph::GraphId target) {
    std::vector<graph::GraphId> found, stack{target};
    std::unordered_set<graph::GraphId> seen;
    while (!stack.empty()) {
        const graph::GraphId id = stack.back();
        stack.pop_back();
        if (!seen.insert(id).second) continue;
        const graph::Node* node = graph.FindNode(id);
        if (!node) continue;
        if (NeedsGpu(*node)) found.push_back(id);
        for (const graph::Pin& pin : node->inputs)
            if (const graph::Node* upstream = graph.FindUpstreamNodeForPin(pin.id)) stack.push_back(upstream->id);
    }
    std::sort(found.begin(), found.end());
    return found;
}

json Vec3Json(const geometry::Vec3& v) { return json::array({v.x, v.y, v.z}); }

// メッシュの連結成分を、外向きの殻（中身の詰まった塊）と内向きの殻（閉じた空洞）に分ける。
// 符号付き体積が正なら塊、負なら空洞。InspectMesh の components は両方を数えるので、これで分けて返す。
json ShellsJson(const geometry::Mesh& mesh) {
    std::vector<uint32_t> parent(mesh.positions.size());
    for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto find = [&](uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (const auto& triangle : mesh.triangles) {
        const uint32_t a = find(triangle[0]);
        parent[find(triangle[1])] = a;
        parent[find(triangle[2])] = a;
    }
    std::unordered_map<uint32_t, double> volumes;
    for (const auto& triangle : mesh.triangles) {
        const geometry::Vec3& p = mesh.positions[triangle[0]];
        const geometry::Vec3& q = mesh.positions[triangle[1]];
        const geometry::Vec3& r = mesh.positions[triangle[2]];
        volumes[find(triangle[0])] += (double(p.x) * (double(q.y) * r.z - double(q.z) * r.y) -
                                       double(p.y) * (double(q.x) * r.z - double(q.z) * r.x) +
                                       double(p.z) * (double(q.x) * r.y - double(q.y) * r.x)) / 6.0;
    }
    std::vector<double> solids;
    size_t cavities = 0;
    double cavityVolume = 0;
    for (const auto& [root, volume] : volumes) {
        if (volume >= 0) {
            solids.push_back(volume);
        } else {
            ++cavities;
            cavityVolume -= volume;
        }
    }
    std::sort(solids.rbegin(), solids.rend());
    double total = 0;
    for (double volume : solids) total += volume;
    json largest = json::array();
    for (size_t i = 0; i < std::min<size_t>(solids.size(), 5); ++i) largest.push_back(solids[i]);
    return {{"solids", solids.size()},
            {"largestSolidVolumes", std::move(largest)},
            {"largestSolidShare", total > 0 && !solids.empty() ? solids.front() / total : 0.0},
            {"cavities", cavities},
            {"cavityVolume", cavityVolume}};
}

json MeshJson(const graph::NodeGraph& graph, const graph::GeneratedRock& rock) {
    const geometry::Mesh& mesh = rock.mesh;
    json item;
    item["source"] = rock.source;
    if (const graph::Node* node = graph.FindNode(rock.source)) item["kind"] = KindName(node->kind);
    item["triangles"] = mesh.triangles.size();
    item["vertices"] = mesh.positions.size();
    item["hasUv"] = !mesh.cornerUvs.empty();
    geometry::MeshInfo info;
    const bool valid = geometry::InspectMesh(mesh, info);
    item["valid"] = valid;
    if (valid && !mesh.positions.empty()) {
        item["bounds"] = {{"min", Vec3Json(info.minimum)}, {"max", Vec3Json(info.maximum)}};
        item["size"] = json::array({info.maximum.x - info.minimum.x, info.maximum.y - info.minimum.y,
                                    info.maximum.z - info.minimum.z});
        item["closed"] = info.closed;
        item["volume"] = info.volume;
        item["shells"] = ShellsJson(mesh);
    }
    if (rock.volume) {
        const auto& grid = *rock.volume;
        item["volumeGrid"] = {{"dimensions", grid.dimensions}, {"spacing", grid.spacing}};
    }
    if (!rock.materials.empty()) item["materials"] = rock.materials.size();
    if (rock.lods) {
        json lods = json::array();
        for (const geometry::Mesh& lod : *rock.lods) lods.push_back(lod.triangles.size());
        item["lodTriangles"] = std::move(lods);
    }
    return item;
}

struct LoadedGraph {
    graph::NodeGraph graph;
    std::vector<io::GraphReadIssue> issues;
};

std::optional<LoadedGraph> LoadGraphFile(const fs::path& path, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "ファイルを開けません: " + ToUtf8Display(path);
        return std::nullopt;
    }
    json document = json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = "JSON として読めません: " + ToUtf8Display(path);
        return std::nullopt;
    }
    const auto graphNode = document.find("graph");
    if (graphNode == document.end() || !graphNode->is_object()) {
        error = "\"graph\" 節がありません";
        return std::nullopt;
    }
    LoadedGraph loaded;
    // マテリアル・テクスチャ・モデルは形の評価に使わない。マテリアルは文書内の番号をそのまま ID にする。
    const auto readMaterial = [](const json& value) -> compositor::MaterialAssetId {
        return value.is_number_unsigned() ? value.get<compositor::MaterialAssetId>() : compositor::kNoMaterialAsset;
    };
    const auto readModel = [](const json&) -> uint64_t { return 0; };
    const auto readTexture = [](const json&) -> compositor::TextureId { return compositor::kNoTexture; };
    if (!io::ReadGraph(*graphNode, loaded.graph, readMaterial, readModel, readTexture, path.parent_path(),
                       &loaded.issues)) {
        error = "グラフにノードがありません";
        return std::nullopt;
    }
    return loaded;
}

json IssuesJson(const std::vector<io::GraphReadIssue>& issues) {
    json out = json::array();
    for (const io::GraphReadIssue& issue : issues) {
        json item{{"message", issue.message}};
        if (issue.node) item["node"] = issue.node;
        if (issue.link) item["link"] = issue.link;
        out.push_back(std::move(item));
    }
    return out;
}

int RunEval(const fs::path& path, std::optional<graph::GraphId> node, geometry::VolumeMeshingMethod mesher, bool pretty) {
    std::string error;
    auto loaded = LoadGraphFile(path, error);
    if (!loaded) return Fail(error, pretty);
    const graph::NodeGraph& graph = loaded->graph;

    graph::GraphId target = 0;
    if (node) {
        if (!graph.FindNode(*node)) return Fail("ノード " + std::to_string(*node) + " がありません", pretty);
        target = *node;
    } else {
        for (const graph::Node& candidate : graph.Nodes())
            if (candidate.kind == graph::NodeKind::MeshOutput) target = candidate.id;
        if (!target) return Fail("Mesh Output がありません。--node で評価するノードを指定してください", pretty);
    }

    json result;
    result["file"] = ToUtf8Portable(path);
    result["target"] = {{"node", target}, {"kind", KindName(graph.FindNode(target)->kind)}};
    result["diagnostics"] = IssuesJson(loaded->issues);
    const std::vector<graph::GraphId> gpuNodes = UpstreamGpuNodes(graph, target);
    if (!gpuNodes.empty()) result["requiresGpu"] = gpuNodes;

    const auto started = std::chrono::steady_clock::now();
    // Mesh Output はプレビュー無し（0）で評価する。アプリの表示と同じ。
    const graph::GraphId preview = graph.FindNode(target)->kind == graph::NodeKind::MeshOutput ? 0 : target;
    const graph::RockEvaluation evaluation = graph::EvaluateRocks(graph, preview, nullptr, mesher);
    result["elapsedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    if (!evaluation.error.empty()) {
        json errorJson{{"message", evaluation.error}};
        // 評価器のエラーは「種類 #ID: 内容」。ID を取り出して機械的に辿れるようにする。
        std::smatch match;
        if (std::regex_search(evaluation.error, match, std::regex("#(\\d+)"))) {
            const graph::GraphId id = std::stoi(match[1].str());
            errorJson["node"] = id;
            if (const graph::Node* failed = graph.FindNode(id)) errorJson["kind"] = KindName(failed->kind);
        }
        if (!gpuNodes.empty())
            errorJson["hint"] = "上流に GPU が要るノード（requiresGpu）があります。rock_cli では評価できないので、"
                                "その手前のノードを --node で評価してください";
        result["ok"] = false;
        result["error"] = std::move(errorJson);
        WriteJson(result, pretty);
        return kExitEvaluationError;
    }

    // 読み込みで捨てたノード・リンクがあれば、評価できても失敗として返す（書いたグラフと評価したグラフが違う）。
    result["ok"] = loaded->issues.empty();
    json meshes = json::array();
    for (const graph::GeneratedRock& rock : evaluation.rocks) meshes.push_back(MeshJson(graph, rock));
    result["meshes"] = std::move(meshes);
    if (evaluation.pieces) result["pieces"] = evaluation.pieces->pieces.size();
    if (evaluation.selection) result["selected"] = evaluation.selection->ids.size();
    if (evaluation.points) result["points"] = evaluation.points->positions.size();
    if (!evaluation.rockInstances.empty()) {
        size_t instances = 0;
        for (const auto& set : evaluation.rockInstances) instances += set.instances.size();
        result["rockInstances"] = instances;
    }
    WriteJson(result, pretty);
    return loaded->issues.empty() ? kExitOk : kExitEvaluationError;
}

// 全ノードの保存名・表示名・ピン・既定の設定。既定の設定は保存処理で書いた JSON そのもの。
int RunCatalog(bool pretty) {
    json kinds = json::array();
    for (uint32_t kindValue = 0; kindValue < 256; ++kindValue) {
        const graph::NodeDefinition* definition = graph::FindNodeDefinition(graph::NodeKind(kindValue));
        if (!definition || uint32_t(definition->kind) != kindValue) continue;
        graph::NodeGraph single;
        const graph::GraphId id = single.CreateNode(definition->kind);
        const auto writeMaterial = [](compositor::MaterialAssetId material) -> json {
            return material ? json(material) : json(nullptr);
        };
        const auto writeModel = [](uint64_t) -> json { return nullptr; };
        const auto writeTexture = [](compositor::TextureId) -> json { return nullptr; };
        const json written = io::WriteGraph(single, writeMaterial, writeModel, writeTexture, fs::current_path());
        json item{{"kind", definition->name}, {"title", definition->title}};
        if (graph::IsMountainNodeKind(definition->kind)) item["graph"] = "mountain";
        json inputs = json::array(), outputs = json::array();
        const graph::Node* created = single.FindNode(id);
        for (const graph::Pin& pin : created->inputs)
            inputs.push_back({{"index", inputs.size()}, {"label", pin.label}, {"type", ValueTypeName(pin.valueType)}});
        for (const graph::Pin& pin : created->outputs)
            outputs.push_back({{"index", outputs.size()}, {"label", pin.label}, {"type", ValueTypeName(pin.valueType)}});
        item["inputs"] = std::move(inputs);
        item["outputs"] = std::move(outputs);
        if (graph::IsVariableInputNodeKind(definition->kind)) item["variableInputs"] = true;
        json settings = json::object();
        if (written.contains("nodes") && !written["nodes"].empty()) {
            for (const auto& [key, value] : written["nodes"][0].items())
                if (key != "id" && key != "kind" && key != "inputs" && key != "outputs" && key != "position")
                    settings[key] = value;
        }
        item["defaults"] = std::move(settings);
        kinds.push_back(std::move(item));
    }
    WriteJson({{"ok", true}, {"nodes", std::move(kinds)}}, pretty);
    return kExitOk;
}

void PrintUsage() {
    std::fputs("使い方:\n"
               "  rock_cli eval <graph.rockgraph> [--node <id>] [--mesher dc|mt] [--pretty]\n"
               "  rock_cli catalog [--pretty]\n",
               stderr);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(ToUtf8Display(fs::path(argv[i])));
    const bool pretty = std::find(args.begin(), args.end(), "--pretty") != args.end();
    if (args.empty()) {
        PrintUsage();
        return kExitUsage;
    }
    const std::string& command = args[0];
    if (command == "catalog") return RunCatalog(pretty);
    if (command == "eval") {
        if (args.size() < 2 || args[1].starts_with("--")) {
            PrintUsage();
            return Fail("eval にはグラフのファイルが要ります", pretty);
        }
        std::optional<graph::GraphId> node;
        auto mesher = geometry::VolumeMeshingMethod::DualContouring;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--node" && i + 1 < args.size()) {
                try {
                    node = std::stoi(args[++i]);
                } catch (...) {
                    return Fail("--node には整数を指定してください", pretty);
                }
            } else if (args[i] == "--mesher" && i + 1 < args.size()) {
                const std::string& name = args[++i];
                if (name == "mt") mesher = geometry::VolumeMeshingMethod::MarchingTetrahedra;
                else if (name != "dc") return Fail("--mesher は dc か mt です", pretty);
            } else if (args[i] != "--pretty") {
                return Fail("知らない引数: " + args[i], pretty);
            }
        }
        return RunEval(FromUtf8(args[1]), node, mesher, pretty);
    }
    PrintUsage();
    return Fail("知らないコマンド: " + command, pretty);
}
