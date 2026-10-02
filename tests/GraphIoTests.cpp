#include "TestSupport.h"

#include "io/GraphIo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
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

    // 書きやすい表記（L3）。ピン・リンクの ID と位置を省き、リンクはノード ID とピン名（か番号）で書く。
    const json authored = json::parse(R"({
      "nodes": [
        {"id": 1, "kind": "baseRock"},
        {"id": 2, "kind": "toVolume", "toVolume": {"resolution": 32}},
        {"id": 3, "kind": "parallelPlanes"},
        {"id": 4, "kind": "volumeCrack"},
        {"id": 5, "kind": "volumeToMesh"},
        {"id": 6, "kind": "meshOutput"},
        {"id": 7, "kind": "layeredBoxes"},
        {"id": 8, "kind": "pieceSelect", "pieces": {"mode": "peel", "rimSide": "top"}}
      ],
      "links": [
        {"from": "1", "to": "2"},
        {"from": "2:volume", "to": "4:Volume"},
        {"from": {"node": 3, "pin": "Planes"}, "to": {"node": 4, "pin": 2}},
        {"from": 4, "to": "5"},
        {"from": "5", "to": "6:Geometry"},
        {"from": "5", "to": "6:Surface"},
        {"from": "9", "to": "6"}
      ]
    })");
    graph::NodeGraph written2;
    issues.clear();
    Check(io::ReadGraph(authored, written2, kReadMaterial, kReadModel, kReadTexture, baseDir, &issues),
          "ピン・リンクの ID と位置を省いたグラフを読める");
    Check(written2.Links().size() == 5, "名前・番号・オブジェクトで書いたリンクを繋ぐ（大文字小文字は区別しない）");
    const graph::Node* crack = written2.FindNode(4);
    Check(crack && written2.FindUpstreamNodeForPin(crack->inputs[2].id) == written2.FindNode(3),
          "Planes は Volume Crack の 3 本目に繋がる");
    Check(HasIssue(issues, 0, 0, "\"Surface\" がありません（Geometry / Material）"), "無いピン名を候補つきで報告");
    Check(HasIssue(issues, 0, 0, "ノード 9 がありません"), "無いノードへのリンクを報告");
    const graph::Node* select = written2.FindNode(8);
    const auto* selection = select ? std::get_if<geometry::PieceSelectSettings>(&select->settings) : nullptr;
    Check(selection && selection->mode == geometry::PieceSelectMode::Peel && selection->rimSide == 1,
          "数値の列挙を名前で書ける（mode: peel、rimSide: top）");
    std::set<graph::GraphId> ids;
    bool unique = true, placed = true;
    for (const graph::Node& node : written2.Nodes()) {
        unique &= ids.insert(node.id).second;
        placed &= node.positionValid;
        for (const auto* pins : {&node.inputs, &node.outputs})
            for (const graph::Pin& pin : *pins) unique &= pin.id > 0 && ids.insert(pin.id).second;
    }
    for (const graph::Link& link : written2.Links()) unique &= ids.insert(link.id).second;
    Check(unique, "省いたピン・リンクの ID を重ならないように振る");
    Check(placed && written2.FindNode(5)->posX > written2.FindNode(4)->posX &&
              written2.FindNode(4)->posX > written2.FindNode(2)->posX,
          "位置の無いノードを上流からの深さで左から並べる");

    // 入力数が可変のノードは、名前の番号（"Rock 3"、"Input 2"）や番号指定の分だけ入力を足して繋ぐ。
    const json variable = json::parse(R"({
      "nodes": [
        {"id": 1, "kind": "heightmap"},
        {"id": 2, "kind": "rock", "rock": {"scene": "a.rockgraph"}},
        {"id": 3, "kind": "rock", "rock": {"scene": "b.rockgraph"}},
        {"id": 4, "kind": "rock", "rock": {"scene": "c.rockgraph"}},
        {"id": 5, "kind": "rockScatter"},
        {"id": 6, "kind": "baseRock"},
        {"id": 7, "kind": "baseRock"},
        {"id": 8, "kind": "merge"}
      ],
      "links": [
        {"from": "1", "to": "5:Terrain"},
        {"from": "2", "to": "5:Rock 1"}, {"from": "3", "to": "5:rock 3"}, {"from": "4", "to": "5:Rock 2"},
        {"from": "6", "to": "8:Input 1"}, {"from": "7", "to": "8:1"}
      ]
    })");
    graph::NodeGraph written3;
    issues.clear();
    Check(io::ReadGraph(variable, written3, kReadMaterial, kReadModel, kReadTexture, baseDir, &issues) && issues.empty() &&
              written3.Links().size() == 6,
          "可変の入力は名前の番号の分だけ足して繋ぐ（Rock 2 / rock 3 / Input 1 / 番号）");
    const graph::Node* scatter3 = written3.FindNode(5);
    Check(scatter3 && scatter3->inputs.size() == 6 && scatter3->inputs[5].label == "Rock 4" &&
              written3.FindUpstreamNodeForPin(scatter3->inputs[4].id) == written3.FindNode(3) &&
              written3.FindUpstreamNodeForPin(scatter3->inputs[3].id) == written3.FindNode(4),
          "Rock 1〜3 が繋がり、空きの Rock 4 が 1 本残る");
}
