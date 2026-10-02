#include "io/RockTemplates.h"

#include "core/PathUtf8.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <fstream>
#include <system_error>

namespace rock::io {
namespace fs = std::filesystem;
using nlohmann::json;

RockTemplateFolders FindRockTemplateFolders() {
    std::error_code error;
    std::wstring executable(MAX_PATH, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length > 0 && length < executable.size()) {
        executable.resize(length);
        const fs::path copied = fs::path(executable).parent_path() / L"templates";
        if (fs::exists(copied / L"templates.json", error)) return {copied, copied};
    }
#if defined(ROCK_TEMPLATE_DIR) && defined(ROCK_TEMPLATE_IMAGE_DIR)
    // 開発中（ビルドのコピーが無いとき）はソースのフォルダを使う。
    const fs::path source = FromUtf8(ROCK_TEMPLATE_DIR);
    if (fs::exists(source / L"templates.json", error)) return {source, FromUtf8(ROCK_TEMPLATE_IMAGE_DIR)};
#endif
    return {};
}

std::vector<RockTemplate> LoadRockTemplates(const RockTemplateFolders& folders, std::string& error) {
    error.clear();
    std::vector<RockTemplate> templates;
    if (folders.graphs.empty()) {
        error = "テンプレートのフォルダが見つかりません";
        return templates;
    }
    std::ifstream stream(folders.graphs / L"templates.json", std::ios::binary);
    if (!stream.is_open()) {
        error = "templates.json を開けません";
        return templates;
    }
    const json document = json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.contains("templates") || !document["templates"].is_array()) {
        error = "templates.json を読めません";
        return templates;
    }
    std::error_code fileError;
    for (const json& item : document["templates"]) {
        if (!item.is_object()) continue;
        RockTemplate entry;
        entry.id = item.value("id", std::string());
        entry.name = item.value("name", entry.id);
        entry.category = item.value("category", std::string());
        entry.status = item.value("status", std::string());
        entry.description = item.value("description", std::string());
        if (entry.id.empty()) continue;
        entry.graph = folders.graphs / FromUtf8(entry.id + ".rockgraph");
        if (!fs::exists(entry.graph, fileError)) {
            error += (error.empty() ? "" : "\n") + std::string("岩グラフがありません: ") + entry.id;
            continue;
        }
        const fs::path image = folders.images / FromUtf8(entry.id) / L"latest.jpg";
        if (fs::exists(image, fileError)) entry.image = image;
        templates.push_back(std::move(entry));
    }
    return templates;
}

bool CopyRockTemplate(const RockTemplate& source, const fs::path& destination, std::string& error) {
    error.clear();
    std::error_code fileError;
    if (destination.empty()) {
        error = "複製先がありません（ルートフォルダの中を選んでください）";
        return false;
    }
    if (fs::exists(destination, fileError)) {
        error = "複製先に同じ名前のファイルがあります: " + ToUtf8Display(destination);
        return false;
    }
    fs::create_directories(destination.parent_path(), fileError);
    if (!fs::copy_file(source.graph, destination, fs::copy_options::none, fileError) || fileError) {
        error = "テンプレートを複製できません: " + fileError.message();
        return false;
    }
    return true;
}

}  // namespace rock::io
