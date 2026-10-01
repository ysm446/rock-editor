#include "TestSupport.h"

#include "graph/NodeParams.h"
#include "graph/RockEvaluator.h"
#include "io/GraphIo.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

// ノードの項目表（graph/NodeParams）が、保存形式と評価のコードに合っているかを確かめる。
using namespace rock;
using rock::tests::Check;
using nlohmann::json;

namespace {

const io::MaterialWriter kWriteMaterial = [](compositor::MaterialAssetId id) -> json { return id ? json(id) : json(nullptr); };
const io::ModelWriter kWriteModel = [](uint64_t) -> json { return nullptr; };
const io::TextureWriter kWriteTexture = [](compositor::TextureId) -> json { return nullptr; };
const io::MaterialReader kReadMaterial = [](const json& value) -> compositor::MaterialAssetId {
    return value.is_number_unsigned() ? value.get<compositor::MaterialAssetId>() : compositor::kNoMaterialAsset;
};
const io::ModelReader kReadModel = [](const json&) -> uint64_t { return 0; };
const io::TextureReader kReadTexture = [](const json&) -> compositor::TextureId { return compositor::kNoTexture; };

const json* FindPath(const json& root, const std::string& path) {
    const json* current = &root;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!current->is_object() || !current->contains(key)) return nullptr;
        current = &(*current)[key];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return current;
}

json* FindPath(json& root, const std::string& path) {
    return const_cast<json*>(FindPath(static_cast<const json&>(root), path));
}

bool InRange(const graph::ParamDefinition& param, double value) {
    return (std::isnan(param.minimum) || value >= param.minimum - 1e-6) &&
           (std::isnan(param.maximum) || value <= param.maximum + 1e-6);
}

// 既定値が範囲内か。配列はすべての要素を見る。
bool DefaultInRange(const graph::ParamDefinition& param, const json& value) {
    if (value.is_number()) return InRange(param, value.get<double>());
    if (value.is_array()) {
        for (const json& element : value)
            if (element.is_number() && !InRange(param, element.get<double>())) return false;
    }
    return true;
}

// 対象のノードを評価できるように上流を組んだグラフ。対象ノードの ID と、評価するノードの ID を返す。
struct Host {
    graph::NodeGraph graph;
    graph::GraphId target = 0;
    graph::GraphId evaluate = 0;
};

graph::GraphId Add(graph::NodeGraph& graph, graph::NodeKind kind) { return graph.CreateNode(kind); }
void Connect(graph::NodeGraph& graph, graph::GraphId from, graph::GraphId to, size_t input, size_t output = 0) {
    graph.CreateLink(graph.FindNode(from)->outputs[output].id, graph.FindNode(to)->inputs[input].id);
}

// 小さな Box（1 m）と粗いボリューム（24）。境界値の評価を速く済ませるため。
graph::GraphId SmallBox(graph::NodeGraph& graph) {
    const auto box = Add(graph, graph::NodeKind::BaseRock);
    std::get<graph::BaseRockNodeSettings>(graph.FindMutableNode(box)->settings).size = {1, 1, 1};
    return box;
}
graph::GraphId SmallVolume(graph::NodeGraph& graph) {
    const auto volume = Add(graph, graph::NodeKind::ToVolume);
    std::get<geometry::VolumeSettings>(graph.FindMutableNode(volume)->settings).resolution = 24;
    Connect(graph, SmallBox(graph), volume, 0);
    return volume;
}

std::optional<Host> MakeHost(graph::NodeKind kind, bool bareCrack = false) {
    using K = graph::NodeKind;
    Host host;
    auto& g = host.graph;
    switch (kind) {
    case K::BaseRock: case K::RandomBoxes: case K::Heightmap: case K::LayeredBoxes:
        host.target = host.evaluate = Add(g, kind);
        return host;
    case K::ToVolume: case K::Decimate: case K::Remesh: case K::Subdivide: case K::UvUnwrap: case K::RockAsset:
        host.target = host.evaluate = Add(g, kind);
        Connect(g, SmallBox(g), host.target, 0);
        return host;
    case K::VolumeTransform: case K::PlaneCuts: case K::VolumeNoise: case K::VolumeSmooth: case K::VolumeTerrace:
    case K::VolumeClose: case K::VolumeEdgeWear: case K::VolumeClip: case K::VolumeToMesh: case K::VolumeScatter: case K::VolumeUndercut:
        host.target = host.evaluate = Add(g, kind);
        Connect(g, SmallVolume(g), host.target, 0);
        return host;
    case K::VolumeBoolean:
        host.target = host.evaluate = Add(g, kind);
        Connect(g, SmallVolume(g), host.target, 0);
        Connect(g, SmallVolume(g), host.target, 1);
        return host;
    case K::VolumeCrack: case K::ParallelPlanes: {
        const auto crack = Add(g, K::VolumeCrack);
        const auto planes = Add(g, K::ParallelPlanes);
        Connect(g, SmallVolume(g), crack, 0);
        if (!bareCrack) Connect(g, planes, crack, 2);
        host.target = kind == K::VolumeCrack ? crack : planes;
        host.evaluate = crack;
        return host;
    }
    case K::VolumeDiffMask: {
        const auto uv = Add(g, K::UvUnwrap);
        Connect(g, SmallBox(g), uv, 0);
        host.target = host.evaluate = Add(g, kind);
        Connect(g, uv, host.target, 0);
        Connect(g, SmallVolume(g), host.target, 1);
        return host;
    }
    case K::ShapeMask: case K::NoiseMask: case K::DepositionMask: case K::StructureMask: {
        const auto uv = Add(g, K::UvUnwrap);
        Connect(g, SmallBox(g), uv, 0);
        host.target = host.evaluate = Add(g, kind);
        Connect(g, uv, host.target, 0);
        if (kind == K::StructureMask) {
            // 縞（既定）は構造面が要る。間隔 0.1 m で 1 m の箱に 10 枚ほど。
            const auto planes = Add(g, K::ParallelPlanes);
            std::get<geometry::ParallelPlanesSettings>(g.FindMutableNode(planes)->settings).spacing = .1f;
            Connect(g, planes, host.target, 1);
        }
        return host;
    }
    case K::ScatterPoints: case K::VoronoiFracture: case K::PieceSelect: case K::PieceFilter: case K::PiecesToMesh: {
        const auto plates = Add(g, K::LayeredBoxes);
        const auto points = Add(g, K::ScatterPoints);
        const auto fracture = Add(g, K::VoronoiFracture);
        const auto select = Add(g, K::PieceSelect);
        const auto filter = Add(g, K::PieceFilter);
        const auto mesh = Add(g, K::PiecesToMesh);
        Connect(g, plates, points, 0);
        Connect(g, plates, fracture, 0);
        Connect(g, points, fracture, 1);
        Connect(g, fracture, select, 0);
        Connect(g, fracture, filter, 0);
        Connect(g, select, filter, 1);
        Connect(g, filter, mesh, 0);
        host.target = kind == K::ScatterPoints ? points : kind == K::VoronoiFracture ? fracture
                    : kind == K::PieceSelect ? select : kind == K::PieceFilter ? filter : mesh;
        host.evaluate = mesh;
        return host;
    }
    default:
        return std::nullopt;  // 評価に GPU・外部ファイルが要る種類（Displace、Rock Scatter など）は範囲の評価を見ない
    }
}

// 範囲がほかの項目との組み合わせで決まる・端の値が重すぎる項目の扱い。
struct EdgeRule {
    const char* name;   // "種類:パス"
    json extras;        // 同じノードに先に書く値（パス → 値）
    bool skipMinimum = false, skipMaximum = false;
    const char* reason = "";
    bool bareCrack = false;  // Volume Crack に Planes を繋がない（割り方が「殻」のとき）
};
const std::vector<EdgeRule>& EdgeRules() {
    static const std::vector<EdgeRule> rules = {
        {"pieceSelect:pieces.fraction", {{"pieces.mode", 4}}},
        {"pieceSelect:pieces.rimLayers", {{"pieces.mode", 5}}},
        {"pieceSelect:pieces.rimFalloff", {{"pieces.mode", 5}}},
        {"pieceSelect:pieces.peelNoise", {{"pieces.mode", 6}}},
        {"pieceSelect:pieces.peelSize", {{"pieces.mode", 6}}},
        {"pieceSelect:pieces.peelRetreat", {{"pieces.mode", 6}}},
        {"pieceSelect:pieces.stability", {{"pieces.mode", 6}, {"pieces.grounded", true}}},
        {"pieceSelect:pieces.minVolume", {{"pieces.mode", 3}}},
        {"pieceSelect:pieces.maxVolume", {{"pieces.mode", 3}, {"pieces.minVolume", 0}}},
        {"volumeClose:volumeClose.width", {{"volumeClose.mode", "width"}}},
        {"volumeUndercut:volumeUndercut.depth", json::object(), false, true, "小さな形では削り切って上下に切り離される"},
        {"volumeCrack:volumeCrack.shellSpacing", {{"volumeCrack.source", "shells"}}, false, false, "", true},
        {"volumeCrack:volumeCrack.shellCount", {{"volumeCrack.source", "shells"}}, false, false, "", true},
        {"volumeCrack:volumeCrack.shellSmoothing", {{"volumeCrack.source", "shells"}}, false, false, "", true},
        {"volumeCrack:volumeCrack.shellPeel", {{"volumeCrack.source", "shells"}}, false, false, "", true},
        {"planeCuts:planeCuts.blend", json::object(), false, true, "大きいと小さな形では中身が残らない"},
        {"volumeClip:volumeClip.height", json::object(), false, true, "形より上で切ると中身が残らない"},
        {"planeCuts:planeCuts.depthMin", {{"planeCuts.depthMax", 0.45}}},
        {"volumeScatter:volumeScatter.radiusMin", {{"volumeScatter.radiusMax", 0.3}}},
        {"volumeScatter:volumeScatter.radiusMax", {{"volumeScatter.radiusMin", 0.005}}},
        {"volumeScatter:volumeScatter.depthMin", {{"volumeScatter.depthMax", 4}}},
        {"volumeScatter:volumeScatter.depthMax", {{"volumeScatter.depthMin", -1}}},
        {"planeCuts:planeCuts.depthMax", {{"planeCuts.depthMin", 0}}},
        {"heightmap:heightmap.minHeight", {{"heightmap.maxHeight", 10000}}, false, true, "最高より低くなければならない"},
        {"heightmap:heightmap.maxHeight", {{"heightmap.minHeight", -10000}}, true, false, "最低より高くなければならない"},
        {"heightmap:heightmap.textureResolution", json::object(), false, true, "重い"},
        {"scatterPoints:pieces.count", json::object(), false, true, "片の数との積が 1024 以下"},
        {"parallelPlanes:parallelPlanes.spacing", json::object(), true, false, "評価範囲の面は 512 枚まで"},
        {"subdivide:subdivide.levels", json::object(), false, true, "重い"},
        {"depositionMask:depositionMask.resolution", json::object(), false, true, "重い"},
        {"structureMask:structureMask.resolution", json::object(), false, true, "重い"},
        {"depositionMask:depositionMask.samples", json::object(), false, true, "重い"},
    };
    return rules;
}
const EdgeRule* FindEdgeRule(const std::string& name) {
    for (const EdgeRule& rule : EdgeRules())
        if (name == rule.name) return &rule;
    return nullptr;
}

// host の対象ノードの項目 path を value にして評価し、エラー文を返す（成功なら空）。
std::string EvaluateWith(const Host& host, const std::string& path, const json& value, const json& extras) {
    json document = io::WriteGraph(host.graph, kWriteMaterial, kWriteModel, kWriteTexture, {});
    for (json& item : document["nodes"]) {
        if (item["id"] != host.target) continue;
        for (const auto& [extraPath, extraValue] : extras.items())
            if (json* slot = FindPath(item, extraPath)) *slot = extraValue;
        if (json* slot = FindPath(item, path)) *slot = value;
    }
    graph::NodeGraph graph;
    io::ReadGraph(document, graph, kReadMaterial, kReadModel, kReadTexture, {});
    return graph::EvaluateRocks(graph, host.evaluate).error;
}

// 範囲の外側の値。整数は 1 つ外、実数は幅の 1%（最低 0.001）外。
double Outside(const graph::ParamDefinition& param, bool below) {
    const double width = param.maximum - param.minimum;
    if (param.type == graph::ParamType::Int) return below ? param.minimum - 1 : param.maximum + 1;
    const double step = std::max(0.001, std::isfinite(width) ? width * 0.01 : 0.001);
    return below ? param.minimum - step : param.maximum + step;
}

}  // namespace

void RunNodeParamsTests() {
    rock::tests::Section("NodeParams（ノードの項目表と、保存形式・評価の一致）");

    std::set<std::string> seen;
    bool pathsExist = true, defaultsInRange = true, unique = true, optionsListed = true;
    for (const graph::ParamDefinition& param : graph::NodeParams()) {
        const graph::NodeDefinition* definition = graph::FindNodeDefinition(param.kind);
        const std::string name = std::string(definition ? definition->name : "?") + ":" + param.path;
        if (!seen.insert(name).second) {
            unique = false;
            std::printf("    重複: %s\n", name.c_str());
        }
        const json defaults = io::WriteDefaultNodeSettings(param.kind);
        const json* value = FindPath(defaults, param.path);
        if (!value) {
            pathsExist = false;
            std::printf("    保存形式に無いパス: %s\n", name.c_str());
            continue;
        }
        if (!DefaultInRange(param, *value)) {
            defaultsInRange = false;
            std::printf("    既定値が範囲外: %s = %s\n", name.c_str(), value->dump().c_str());
        }
        if ((param.type == graph::ParamType::Enum || param.type == graph::ParamType::IntEnum) && param.options.empty()) {
            optionsListed = false;
            std::printf("    列挙の候補が無い: %s\n", name.c_str());
        }
        if (param.type == graph::ParamType::Enum && value->is_string()) {
            bool listed = false;
            for (const graph::ParamOption& option : param.options) listed |= *value == option.name;
            if (!listed) {
                optionsListed = false;
                std::printf("    既定値が列挙の候補に無い: %s = %s\n", name.c_str(), value->dump().c_str());
            }
        }
    }
    Check(!graph::NodeParams().empty() && unique, "項目表があり、同じ項目が重複しない");
    Check(pathsExist, "すべての項目のパスが保存形式（既定値のノード）にある");
    Check(defaultsInRange, "既定値が範囲内");
    Check(optionsListed, "列挙の項目は候補を持ち、既定値が候補にある");

    // 設定を持つ種類は、保存するキーがすべて表に載っている（内部の管理用も internal で載せる）。
    bool covered = true;
    for (uint32_t value = 0; value < 256; ++value) {
        const graph::NodeDefinition* definition = graph::FindNodeDefinition(graph::NodeKind(value));
        if (!definition || uint32_t(definition->kind) != value) continue;
        const json defaults = io::WriteDefaultNodeSettings(definition->kind);
        for (const auto& [section, body] : defaults.items()) {
            if (!body.is_object()) continue;
            for (const auto& [key, unused] : body.items()) {
                const std::string path = section + "." + key;
                bool listed = false;
                for (const graph::ParamDefinition& param : graph::NodeParams())
                    if (param.kind == definition->kind &&
                        (path == param.path || std::string(param.path).starts_with(path + ".")))
                        listed = true;
                if (!listed) {
                    covered = false;
                    std::printf("    表に無いキー: %s:%s\n", definition->name, path.c_str());
                }
            }
        }
        if (!graph::FindNodeSummary(definition->kind)) {
            covered = false;
            std::printf("    概要が無い種類: %s\n", definition->name);
        }
    }
    Check(covered, "保存するキーと種類がすべて表に載っている");

    // 範囲外でエラーになる項目は、端の値で評価が通り、外側の値で評価が失敗する。
    bool boundaries = true, outside = true;
    int evaluated = 0;
    for (const graph::ParamDefinition& param : graph::NodeParams()) {
        if (param.check != graph::ParamCheck::Error || param.internal) continue;
        if (param.type != graph::ParamType::Float && param.type != graph::ParamType::Int) continue;
        const std::string name = std::string(graph::FindNodeDefinition(param.kind)->name) + ":" + param.path;
        const EdgeRule* rule = FindEdgeRule(name);
        const auto host = MakeHost(param.kind, rule && rule->bareCrack);
        if (!host) continue;
        const auto number = [&](double v) { return param.type == graph::ParamType::Int ? json(int64_t(std::llround(v))) : json(v); };
        const json extras = rule ? rule->extras : json::object();
        for (const bool atMinimum : {true, false}) {
            const double edge = atMinimum ? param.minimum : param.maximum;
            if (std::isnan(edge) || (rule && (atMinimum ? rule->skipMinimum : rule->skipMaximum))) continue;
            if (const std::string error = EvaluateWith(*host, param.path, number(edge), extras); !error.empty()) {
                boundaries = false;
                std::printf("    端の値で失敗: %s = %g: %s\n", name.c_str(), edge, error.c_str());
            }
        }
        for (const bool below : {true, false}) {
            const double edge = below ? param.minimum : param.maximum;
            if (std::isnan(edge)) continue;
            const double value = Outside(param, below);
            if (EvaluateWith(*host, param.path, number(value), extras).empty()) {
                outside = false;
                std::printf("    範囲外でも通る: %s = %g\n", name.c_str(), value);
            }
        }
        ++evaluated;
    }
    std::printf("    範囲を評価で確かめた項目: %d\n", evaluated);
    Check(boundaries, "範囲の端の値では評価が通る");
    Check(outside, "範囲の外の値では評価が失敗する");
}
