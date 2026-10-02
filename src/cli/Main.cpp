// rock_cli — GPU とウィンドウを使わずに岩グラフを扱うコンソールの道具。
//
// LLM（アプリの外で動くエージェント）がグラフを書き、結果を数値で確かめるための入口。
// アプリと同じ評価器（graph::EvaluateRocks）と保存形式の読み込み（io::ReadGraph）を使う。
// 出力は標準出力への JSON 1 つ。ログは標準エラーへ出る。
//
//   rock_cli eval <graph.rockgraph> [--node <id>] [--mesher dc|mt] [--obj <out.obj>] [--volume <out.bin>] [--cache] [--pretty]
//   rock_cli check <graph.rockgraph> [--pretty]
//   rock_cli catalog [--pretty]
//
// 終了コード: 0 = 成功、1 = 評価エラーか、読み込みで捨てたノード・リンクがある、2 = 読み込めない・引数の誤り。
// 仕様は docs/reference/ai-authoring.md。
#include "core/PathUtf8.h"
#include "geometry/Mesh.h"
#include "graph/NodeGraph.h"
#include "graph/NodeParams.h"
#include "graph/RockEvaluator.h"
#include "io/GraphIo.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

const char* ParamTypeName(graph::ParamType type) {
    switch (type) {
    case graph::ParamType::Float: return "float";
    case graph::ParamType::Int: return "int";
    case graph::ParamType::Bool: return "bool";
    case graph::ParamType::String: return "string";
    case graph::ParamType::Float3: return "float3";
    case graph::ParamType::FloatArray: return "floatArray";
    case graph::ParamType::BoolArray: return "boolArray";
    case graph::ParamType::Enum: return "enum";
    case graph::ParamType::IntEnum: return "intEnum";
    }
    return "?";
}

const char* ParamCheckName(graph::ParamCheck check) {
    switch (check) {
    case graph::ParamCheck::Error: return "error";
    case graph::ParamCheck::Clamp: return "clamp";
    case graph::ParamCheck::None: return "none";
    }
    return "?";
}

json ParamJson(const graph::ParamDefinition& param) {
    json item{{"path", param.path}, {"type", ParamTypeName(param.type)}, {"label", param.label},
              {"meaning", param.meaning}, {"outOfRange", ParamCheckName(param.check)}};
    if (!std::isnan(param.minimum)) item["min"] = param.minimum;
    if (!std::isnan(param.maximum)) item["max"] = param.maximum;
    if (param.unit && *param.unit) item["unit"] = param.unit;
    if (!param.options.empty()) {
        json options = json::array();
        for (const graph::ParamOption& option : param.options) {
            json entry{{"name", option.name}, {"meaning", option.meaning}};
            if (param.type == graph::ParamType::IntEnum) entry["value"] = option.value;
            options.push_back(std::move(entry));
        }
        item["options"] = std::move(options);
    }
    if (param.internal) item["internal"] = true;
    return item;
}

const json* FindJsonPath(const json& root, const std::string& path) {
    const json* current = &root;
    for (size_t start = 0;;) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!current->is_object()) return nullptr;
        const auto found = current->find(key);
        if (found == current->end()) return nullptr;
        current = &*found;
        if (dot == std::string::npos) return current;
        start = dot + 1;
    }
}

// 値の JSON の型が項目の型に合うか。合わない値は読み込みで黙って既定値になる。
bool TypeMatches(const graph::ParamDefinition& param, const json& value) {
    const std::string path = param.path;
    const bool nullable = path.ends_with(".material") || path.ends_with(".model") || path.ends_with(".texture");
    if (value.is_null()) return nullable;
    const auto numbers = [&](size_t count) {
        if (!value.is_array() || (count && value.size() != count)) return false;
        for (const json& element : value)
            if (!element.is_number()) return false;
        return true;
    };
    switch (param.type) {
    case graph::ParamType::Float: return value.is_number();
    case graph::ParamType::Int: return value.is_number_integer() || (nullable && value.is_string());
    case graph::ParamType::Bool: return value.is_boolean();
    case graph::ParamType::String: return value.is_string();
    case graph::ParamType::Float3: return numbers(3);
    case graph::ParamType::FloatArray: return value.is_array();
    case graph::ParamType::BoolArray: return value.is_array();
    case graph::ParamType::Enum: return value.is_string() || value.is_array();
    case graph::ParamType::IntEnum: return value.is_number_integer() || value.is_string();
    }
    return true;
}

// ファイルに書かれた値を項目表の範囲・列挙と照らす。評価より先に、どの項目が悪いかをパスで返す。
void CheckParams(const json& graphNode, std::vector<io::GraphReadIssue>& issues) {
    const json* nodes = graphNode.contains("nodes") ? &graphNode["nodes"] : nullptr;
    if (!nodes || !nodes->is_array()) return;
    for (const json& item : *nodes) {
        if (!item.is_object() || !item.contains("kind") || !item["kind"].is_string()) continue;
        const graph::NodeDefinition* definition = graph::FindNodeDefinitionByName(item["kind"].get<std::string>());
        if (!definition) continue;
        const graph::GraphId id = item.value("id", 0);
        for (const graph::ParamDefinition& param : graph::NodeParams()) {
            if (param.kind != definition->kind) continue;
            const json* value = FindJsonPath(item, param.path);
            if (!value) continue;
            const std::string where = std::string(definition->name) + "#" + std::to_string(id) + " の " + param.path;
            if (!TypeMatches(param, *value)) {
                issues.push_back({id, 0, where + " = " + value->dump() + " の型が違います（" + ParamTypeName(param.type) +
                                             " が要ります。読み込みでは無視されて既定値になります）"});
                continue;
            }
            const auto checkNumber = [&](const json& number) {
                if (!number.is_number()) return;
                const double v = number.get<double>();
                if ((!std::isnan(param.minimum) && v < param.minimum - 1e-6) ||
                    (!std::isnan(param.maximum) && v > param.maximum + 1e-6)) {
                    char range[96];
                    std::snprintf(range, sizeof(range), "%g〜%g", param.minimum, param.maximum);
                    issues.push_back({id, 0, where + " = " + number.dump() + " は範囲外です（" + range +
                                                 (param.check == graph::ParamCheck::Clamp ? "。丸めて使われます）" : "）")});
                }
            };
            if (value->is_array()) {
                for (const json& element : *value) checkNumber(element);
            } else {
                checkNumber(*value);
            }
            if (param.type == graph::ParamType::Enum && value->is_string()) {
                bool listed = false;
                std::string names;
                for (const graph::ParamOption& option : param.options) {
                    listed |= *value == option.name;
                    names += (names.empty() ? "" : " / ") + std::string(option.name);
                }
                if (!listed) issues.push_back({id, 0, where + " = " + value->dump() + " は候補にありません（" + names + "）"});
            }
            if (param.type == graph::ParamType::IntEnum && value->is_number_integer()) {
                bool listed = false;
                for (const graph::ParamOption& option : param.options) listed |= value->get<int>() == option.value;
                if (!listed) issues.push_back({id, 0, where + " = " + value->dump() + " は候補にありません"});
            }
        }
    }
}

// グラフのファイルから上へ project.reproj を探す（アプリの --root と同じ、プロジェクトのルート）。無ければ空。
fs::path FindProjectRoot(const fs::path& file) {
    std::error_code error;
    for (fs::path dir = fs::absolute(file, error).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "project.reproj", error)) return dir;
        if (dir == dir.root_path()) break;
    }
    return {};
}

// マテリアルの参照（表の asset.path と、Surface に直接書いたパス）がルートの中に実在するか。
void CheckAssetPaths(const json& document, const fs::path& file, std::vector<io::GraphReadIssue>& issues) {
    // 見つからなければ、グラフのあるフォルダをルートとみなす（サンプルは開くまで project.reproj を持たない）。
    std::error_code absoluteError;
    fs::path root = FindProjectRoot(file);
    if (root.empty()) root = fs::absolute(file, absoluteError).parent_path();
    const auto check = [&](const std::string& path, graph::GraphId node) {
        std::error_code error;
        if (!path.empty() && !fs::exists(root / FromUtf8(path), error))
            issues.push_back({node, 0, "マテリアルが見つかりません: " + path + "（ルート " + ToUtf8Portable(root) + " からのパス）"});
    };
    if (const auto materials = document.find("materials"); materials != document.end() && materials->is_array())
        for (const json& entry : *materials)
            if (entry.is_object() && entry.contains("asset") && entry["asset"].is_object())
                check(entry["asset"].value("path", std::string()), 0);
    if (const auto graphNode = document.find("graph"); graphNode != document.end() && graphNode->contains("nodes"))
        for (const json& item : (*graphNode)["nodes"])
            if (item.is_object() && item.contains("layer") && item["layer"].is_object() &&
                item["layer"].contains("material") && item["layer"]["material"].is_string())
                check(item["layer"]["material"].get<std::string>(), item.value("id", 0));
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
    CheckParams(*graphNode, loaded.issues);
    CheckAssetPaths(document, path, loaded.issues);
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

int RunEval(const fs::path& path, std::optional<graph::GraphId> node, geometry::VolumeMeshingMethod mesher, bool pretty,
            const fs::path& objPath = {}, const fs::path& volumePath = {}, bool useCache = false) {
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
    // --cache: アプリと同じく永続キャッシュ付きで評価する（アプリとの結果の違いを切り分けるため）。
    graph::RockEvaluationCache cache;
    const graph::RockEvaluation evaluation = graph::EvaluateRocks(graph, preview, useCache ? &cache : nullptr, mesher);
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
    // --volume: 評価したノードの距離場（Volume を持つ結果の最初のもの）を生の float で書く。
    // 先頭に寸法 3 つ（uint32）、原点 3 つ（float）、間隔（float）、続けて値（X が最速、負が内部）。
    if (!volumePath.empty()) {
        const geometry::VolumeGrid* grid = nullptr;
        for (const graph::GeneratedRock& rock : evaluation.rocks)
            if (rock.volume && !rock.volume->values.empty()) { grid = rock.volume.get(); break; }
        if (!grid) {
            result["volumeError"] = "このノードの結果に Volume がありません";
        } else if (FILE* file = _wfopen(volumePath.c_str(), L"wb")) {
            std::fwrite(grid->dimensions.data(), sizeof(uint32_t), 3, file);
            const float header[4] = {grid->origin.x, grid->origin.y, grid->origin.z, grid->spacing};
            std::fwrite(header, sizeof(float), 4, file);
            std::fwrite(grid->values.data(), sizeof(float), grid->values.size(), file);
            std::fclose(file);
            result["volume"] = ToUtf8Display(volumePath);
        } else {
            result["volumeError"] = "Volume を書けません: " + ToUtf8Display(volumePath);
        }
    }
    // --obj: 評価したメッシュ（複数なら全部を一つに）を Wavefront OBJ に書く。外部ツールや Python で形を調べるため。
    if (!objPath.empty()) {
        std::string text;
        size_t base = 0;
        for (const graph::GeneratedRock& rock : evaluation.rocks) {
            for (const auto& p : rock.mesh.positions)
                text += "v " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + "\n";
            for (const auto& t : rock.mesh.triangles)
                text += "f " + std::to_string(base + t[0] + 1) + " " + std::to_string(base + t[1] + 1) + " " + std::to_string(base + t[2] + 1) + "\n";
            base += rock.mesh.positions.size();
        }
        if (FILE* file = _wfopen(objPath.c_str(), L"wb")) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
            result["obj"] = ToUtf8Display(objPath);
        } else {
            result["objError"] = "OBJ を書けません: " + ToUtf8Display(objPath);
        }
    }
    if (evaluation.pieces) result["pieces"] = evaluation.pieces->pieces.size();
    if (evaluation.selection) result["selected"] = evaluation.selection->ids.size();
    if (evaluation.points) result["points"] = evaluation.points->positions.size();
    if (!evaluation.rockInstances.empty()) {
        size_t instances = 0;
        for (const auto& set : evaluation.rockInstances) instances += set.instances.size();
        result["rockInstances"] = instances;
        // 内訳（撒いたノードと岩グラフごと）。山グラフの段ごとの数と推定の被覆率を見るため。
        json sets = json::array();
        for (const auto& set : evaluation.rockInstances) {
            json item{{"source", set.source}, {"scene", set.scene}, {"scale", set.scale},
                      {"instances", set.instances.size()}, {"coverage", set.coverage}};
            // 置いた点の範囲（地形の外や縁に置いていないかを見るため）。
            if (!set.instances.empty()) {
                geometry::Vec3 lo = set.instances[0].position, hi = lo;
                for (const auto& instance : set.instances) {
                    lo = {std::min(lo.x, instance.position.x), std::min(lo.y, instance.position.y), std::min(lo.z, instance.position.z)};
                    hi = {std::max(hi.x, instance.position.x), std::max(hi.y, instance.position.y), std::max(hi.z, instance.position.z)};
                }
                item["positionMin"] = {lo.x, lo.y, lo.z};
                item["positionMax"] = {hi.x, hi.y, hi.z};
                // 少数なら位置も列挙する（大岩の置き場の確認用）。
                if (set.instances.size() <= 8) {
                    json positions = json::array();
                    for (const auto& instance : set.instances)
                        positions.push_back({instance.position.x, instance.position.y, instance.position.z});
                    item["positions"] = std::move(positions);
                }
            }
            sets.push_back(std::move(item));
        }
        result["rockInstanceSets"] = std::move(sets);
    }
    WriteJson(result, pretty);
    return loaded->issues.empty() ? kExitOk : kExitEvaluationError;
}

// 読み込みの診断だけを返す（評価しない）。書いた直後の確認に使う。
int RunCheck(const fs::path& path, bool pretty) {
    std::string error;
    auto loaded = LoadGraphFile(path, error);
    if (!loaded) return Fail(error, pretty);
    json result{{"file", ToUtf8Portable(path)}, {"ok", loaded->issues.empty()},
                {"nodes", loaded->graph.Nodes().size()}, {"links", loaded->graph.Links().size()},
                {"diagnostics", IssuesJson(loaded->issues)}};
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
        item["defaults"] = io::WriteDefaultNodeSettings(definition->kind);
        if (const graph::NodeSummary* summary = graph::FindNodeSummary(definition->kind)) {
            item["purpose"] = summary->purpose;
            if (summary->notes && *summary->notes) item["notes"] = summary->notes;
        }
        json params = json::array();
        for (const graph::ParamDefinition& param : graph::NodeParams())
            if (param.kind == definition->kind) params.push_back(ParamJson(param));
        item["params"] = std::move(params);
        kinds.push_back(std::move(item));
    }
    WriteJson({{"ok", true}, {"nodes", std::move(kinds)}}, pretty);
    return kExitOk;
}

void PrintUsage() {
    std::fputs("使い方:\n"
               "  rock_cli eval <graph.rockgraph> [--node <id>] [--mesher dc|mt] [--obj <out.obj>] [--pretty]\n"
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
    if (command == "check") {
        if (args.size() < 2 || args[1].starts_with("--")) {
            PrintUsage();
            return Fail("check にはグラフのファイルが要ります", pretty);
        }
        return RunCheck(FromUtf8(args[1]), pretty);
    }
    if (command == "eval") {
        if (args.size() < 2 || args[1].starts_with("--")) {
            PrintUsage();
            return Fail("eval にはグラフのファイルが要ります", pretty);
        }
        std::optional<graph::GraphId> node;
        auto mesher = geometry::VolumeMeshingMethod::DualContouring;
        fs::path objPath, volumePath;
        bool useCache = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--obj" && i + 1 < args.size()) {
                objPath = FromUtf8(args[++i]);
            } else if (args[i] == "--volume" && i + 1 < args.size()) {
                volumePath = FromUtf8(args[++i]);
            } else if (args[i] == "--cache") {
                useCache = true;
            } else if (args[i] == "--node" && i + 1 < args.size()) {
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
        return RunEval(FromUtf8(args[1]), node, mesher, pretty, objPath, volumePath, useCache);
    }
    PrintUsage();
    return Fail("知らないコマンド: " + command, pretty);
}
