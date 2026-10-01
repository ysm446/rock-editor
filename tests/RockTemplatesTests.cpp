#include "TestSupport.h"

#include "core/PathUtf8.h"
#include "io/GraphIo.h"
#include "io/RockTemplates.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace rock;
using rock::tests::Check;
using nlohmann::json;
namespace fs = std::filesystem;

// 岩のテンプレート（examples/ai-recipes/）。アプリの「テンプレートから作成」が使う。
// 全テンプレートが読み込みの診断なしで開けることを保証する（レシピを直して壊したら、ここで気付く）。
void RunRockTemplatesTests() {
    rock::tests::Section("岩のテンプレート");
    const io::RockTemplateFolders source{FromUtf8(ROCK_TEMPLATE_DIR), FromUtf8(ROCK_TEMPLATE_IMAGE_DIR)};
    std::string error;
    const auto templates = io::LoadRockTemplates(source, error);
    Check(error.empty() && templates.size() >= 20, "templates.json の全項目に岩グラフがある");
    bool named = true, imaged = true, clean = true;
    for (const auto& entry : templates) {
        named &= !entry.name.empty() && !entry.category.empty() && !entry.status.empty() && !entry.description.empty();
        imaged &= !entry.image.empty();
        std::ifstream stream(entry.graph, std::ios::binary);
        const json document = json::parse(stream, nullptr, false);
        graph::NodeGraph graph;
        std::vector<io::GraphReadIssue> issues;
        const bool read = !document.is_discarded() && document.contains("graph") &&
                          io::ReadGraph(document["graph"], graph,
                                        [](const json&) -> compositor::MaterialAssetId { return 0; },
                                        [](const json&) -> uint64_t { return 0; },
                                        [](const json&) -> compositor::TextureId { return 0; }, entry.graph.parent_path(), &issues);
        if (!read || !issues.empty()) {
            clean = false;
            std::printf("    読めないテンプレート: %s（%s）\n", entry.id.c_str(), issues.empty() ? "" : issues.front().message.c_str());
        }
    }
    Check(named, "全項目に日本語名・分類・状態・説明がある");
    Check(imaged, "全項目に研究ページの画像がある");
    Check(clean, "全テンプレートが読み込みの診断なしで開ける");

    const fs::path directory = fs::path(ROCK_DATA_DIR) / "test" / "rock-templates";
    std::error_code fileError;
    fs::remove_all(directory, fileError);
    const fs::path destination = directory / FromUtf8("片理.rockgraph");
    Check(!templates.empty() && io::CopyRockTemplate(templates.front(), destination, error) && fs::exists(destination),
          "テンプレートを複製できる（フォルダも作る）");
    Check(!io::CopyRockTemplate(templates.front(), destination, error) && !error.empty(), "既にあるファイルは上書きしない");
    Check(io::LoadRockTemplates({}, error).empty() && !error.empty(), "フォルダが無ければ理由を返す");
    fs::remove_all(directory, fileError);
}
