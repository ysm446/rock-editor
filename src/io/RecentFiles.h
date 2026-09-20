#pragma once

#include <filesystem>
#include <vector>

namespace rock::io {

// 最近使ったルートフォルダと、ルートごとのシーンの履歴。
//
// プロジェクトの中身ではなくアプリ側の状態なので、ルートには入れず
// **`%LOCALAPPDATA%/rock-editor/recent.json`** に置く。
// 版1（プロジェクトファイルの一覧）は、所属するルートを開いたときに移行する。
class RecentFiles {
public:
    // 保持する件数（ルート・シーンとも）。多すぎるとメニューが縦に伸びて選びにくい。
    static constexpr size_t kMaxEntries = 10;

    struct RootEntry {
        std::filesystem::path path;
        std::vector<std::filesystem::path> scenes;  // 新しい順
    };

    // 起動時に 1 回読む。storage が空なら既定の置き場所。
    void Load(const std::filesystem::path& storage = {});
    // ルートを先頭へ。すでにあれば先頭へ引き上げる。
    void AddRoot(const std::filesystem::path& root);
    void Add(const std::filesystem::path& root, const std::filesystem::path& scene);
    void Remove(const std::filesystem::path& root, const std::filesystem::path& scene);
    void Clear(const std::filesystem::path& root);
    void ClearRoots();

    // 新しい順。
    const std::vector<RootEntry>& Roots() const { return m_roots; }
    const std::vector<std::filesystem::path>& Entries(const std::filesystem::path& root) const;

private:
    void Save() const;

    std::filesystem::path m_storage;
    std::vector<RootEntry> m_roots;
    // 版1の履歴でまだどのルートにも属していないもの。
    std::vector<std::filesystem::path> m_legacy;
};

}  // namespace rock::io
