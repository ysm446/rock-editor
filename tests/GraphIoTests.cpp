#include "TestSupport.h"

#include "io/GraphIo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

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

// Base Shape → To Volume → Volume to Mesh → Mesh Output。
graph::NodeGraph MakeChain() {
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(graph::NodeKind::BaseRock);
    const auto volume = graph.CreateNode(graph::NodeKind::ToVolume);
    const auto mesh = graph.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto output = graph.CreateNode(graph::NodeKind::MeshOutput);
    graph.CreateLink(graph.FindNode(base)->outputs[0].id, graph.FindNode(volume)->inputs[0].id);
    graph.CreateLink(graph.FindNode(volume)->outputs[0].id, graph.FindNode(mesh)->inputs[0].id);
    graph.CreateLink(graph.FindNode(mesh)->outputs[0].id, graph.FindNode(output)->inputs[0].id);
    return graph;
}

bool HasIssue(const std::vector<io::GraphReadIssue>& issues, graph::GraphId node, graph::GraphId link,
              const std::string& fragment) {
    return std::any_of(issues.begin(), issues.end(), [&](const io::GraphReadIssue& issue) {
        return issue.node == node && issue.link == link && issue.message.find(fragment) != std::string::npos;
    });
}

}  // namespace

void RunGraphIoTests() {
    rock::tests::Section("GraphIo（グラフの読み書きと読み込みの診断）");
    const std::filesystem::path baseDir = std::filesystem::current_path();

    const graph::NodeGraph chain = MakeChain();
    const json written = io::WriteGraph(chain, kWriteMaterial, kWriteModel, kWriteTexture, baseDir);
    graph::NodeGraph reread;
    std::vector<io::GraphReadIssue> issues;
    const bool read = io::ReadGraph(written, reread, kReadMaterial, kReadModel, kReadTexture, baseDir, &issues);
    Check(read && reread.Nodes().size() == 4 && reread.Links().size() == 3, "保存して読み直すとノード4つ・リンク3本");
    Check(issues.empty(), "正しいグラフでは診断が出ない");
    Check(io::WriteGraph(reread, kWriteMaterial, kWriteModel, kWriteTexture, baseDir) == written,
          "読み直したグラフを書くと同じ JSON");

    // 知らない種類・無いピン・型の合わない接続・入力への 2 本目を混ぜる。
    json broken = written;
    const graph::Node* volume = nullptr;
    const graph::Node* mesh = nullptr;
    for (const graph::Node& node : chain.Nodes()) {
        if (node.kind == graph::NodeKind::ToVolume) volume = &node;
        if (node.kind == graph::NodeKind::VolumeToMesh) mesh = &node;
    }
    broken["nodes"].push_back({{"id", 900}, {"kind", "volumeBlur"}, {"inputs", {901}}, {"outputs", {902}}});
    broken["links"].push_back({{"id", 910}, {"start", 12345}, {"end", mesh->inputs[0].id}});
    // Volume to Mesh の出力（Mesh）を To Volume の入力（Mesh）へ戻すと循環になる。
    broken["links"].push_back({{"id", 911}, {"start", mesh->outputs[0].id}, {"end", volume->inputs[0].id}});
    broken["links"].push_back({{"id", 912}, {"start", 0}, {"end", volume->inputs[0].id}});
    graph::NodeGraph partial;
    issues.clear();
    io::ReadGraph(broken, partial, kReadMaterial, kReadModel, kReadTexture, baseDir, &issues);
    Check(partial.Nodes().size() == 4 && partial.Links().size() == 3, "壊れた部分を捨てて残りは読む");
    Check(HasIssue(issues, 900, 0, "volumeBlur"), "知らない種類をノード ID と名前つきで報告");
    Check(HasIssue(issues, mesh->id, 910, "start のピン 12345"), "無いピンへのリンクを報告");
    Check(HasIssue(issues, volume->id, 911, "既に別のリンク") || HasIssue(issues, volume->id, 911, "繋げません"),
          "繋げないリンクを入力側のノードつきで報告");
    Check(HasIssue(issues, 0, 912, "id / start / end"), "欠けたリンクを報告");
    Check(issues.size() == 4, "診断はちょうど4件");

    // 設定のキーの打ち間違い（入れ子も）。読み込みは無視して既定値で開くので、診断で知らせる。
    json typo = written;
    for (json& item : typo["nodes"]) {
        if (item["kind"] == "toVolume") item["toVolume"]["resolutoin"] = 64;
        if (item["kind"] == "baseRock") item["note"] = "メモは条件つきのキーなので診断しない";
    }
    graph::NodeGraph typed;
    issues.clear();
    io::ReadGraph(typo, typed, kReadMaterial, kReadModel, kReadTexture, baseDir, &issues);
    Check(issues.size() == 1 && HasIssue(issues, volume->id, 0, "toVolume.resolutoin"),
          "知らない設定のキーをパスつきで報告（メモは報告しない）");
    json surface = io::WriteDefaultNodeSettings(graph::NodeKind::Surface);
    Check(surface.contains("layer") && surface["layer"].contains("mapping"), "既定値のノードの設定を書ける（Surface）");

    graph::NodeGraph silent;
    Check(io::ReadGraph(broken, silent, kReadMaterial, kReadModel, kReadTexture, baseDir) &&
              silent.Links().size() == 3,
          "診断を受け取らなくても同じ結果で読む（アプリの読み込み）");
}
