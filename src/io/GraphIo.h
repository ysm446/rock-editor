#pragma once

#include "compositor/MaterialLayer.h"
#include "graph/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// ノードグラフの保存形式（シーンの "graph" 節）の読み書き。
//
// GPU・ウィンドウに依存しない。アプリの ProjectIo と、UI なしで評価する rock_cli が共有する。
// マテリアル・モデル・テクスチャの参照は、呼び出し側が文書の中の書き方と実行中の ID を写す。
namespace rock::io {

// 読み込みで捨てたもの。アプリは捨てて開き、rock_cli はこれを報告する。
struct GraphReadIssue {
    graph::GraphId node = 0;  // 関係するノード（分からなければ 0）
    graph::GraphId link = 0;  // 関係するリンク（リンクでなければ 0）
    std::string message;
};

using MaterialWriter = std::function<nlohmann::json(compositor::MaterialAssetId)>;
using MaterialReader = std::function<compositor::MaterialAssetId(const nlohmann::json&)>;
using ModelWriter = std::function<nlohmann::json(uint64_t)>;
using ModelReader = std::function<uint64_t(const nlohmann::json&)>;
using TextureWriter = std::function<nlohmann::json(compositor::TextureId)>;
using TextureReader = std::function<compositor::TextureId(const nlohmann::json&)>;

// writeModel は Model ノードのモデル（実行中の ID）を文書内の番号へ写す。無ければ null を書く。
nlohmann::json WriteGraph(const graph::NodeGraph& graphData, const MaterialWriter& writeMaterial,
                          const ModelWriter& writeModel, const TextureWriter& writeTexture,
                          const std::filesystem::path& baseDir);

// 戻り値はノードを 1 つ以上読めたか。空のグラフ節は「グラフ未使用」とみなし、
// 呼び出し側が旧 layers からの移行に切り替える。
// readModel は Model ノードの文書内の番号を実行中のモデル ID へ写す（0 = なし）。
// issues を渡すと、読めずに捨てたノード・リンクを理由つきで積む。
bool ReadGraph(const nlohmann::json& node, graph::NodeGraph& graphData, const MaterialReader& readMaterial,
               const ModelReader& readModel, const TextureReader& readTexture,
               const std::filesystem::path& baseDir, std::vector<GraphReadIssue>* issues = nullptr);

}  // namespace rock::io
