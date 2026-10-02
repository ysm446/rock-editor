#pragma once

#include <filesystem>
#include <string>
#include <vector>

// 岩のテンプレート（examples/ai-recipes/ のレシピ）。アプリの「テンプレートから作成」で、
// 表示中のフォルダへ複製して開く。一覧は templates.json（日本語名・分類・状態・説明）。
// GPU・UI に依存しない。
namespace rock::io {

struct RockTemplate {
    std::string id;           // ファイル名の元（<id>.rockgraph / <id>/latest.jpg）
    std::string name;         // 日本語名
    std::string category;     // 分類（形の骨格、風化・侵食の形 など）
    std::string status;       // ○ / △（研究ページの状態）
    std::string description;  // 一行の説明
    std::filesystem::path graph;  // 岩グラフ
    std::filesystem::path image;  // サムネイル（無ければ空）
};

struct RockTemplateFolders {
    std::filesystem::path graphs;  // templates.json と <id>.rockgraph のあるフォルダ
    std::filesystem::path images;  // <id>/latest.jpg のあるフォルダ（docs/research か、コピーした templates/）
};

// 実行ファイルの隣の templates/（ビルドでコピーしたもの）。無ければソースのフォルダ（開発中）。
// どちらにも templates.json が無ければ graphs が空。
RockTemplateFolders FindRockTemplateFolders();

// templates.json を読む。岩グラフの無い項目は飛ばし、理由を error に足す（全部読めれば空）。
std::vector<RockTemplate> LoadRockTemplates(const RockTemplateFolders& folders, std::string& error);

// テンプレートの岩グラフを destination へ複製する（名前は呼び出し側が ProjectWorkspace::UniquePath で決める）。
// 既にあるファイルは上書きしない。
bool CopyRockTemplate(const RockTemplate& source, const std::filesystem::path& destination, std::string& error);

}  // namespace rock::io
