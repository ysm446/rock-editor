#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

namespace rock::io {

// プロジェクトのルートフォルダ、永続ID、共有アセットの入出力。GPU には依存しない。
//
// ルート直下の `project.reproj`（rock-editor.workspace 版1）がプロジェクトの目印。
// シーン (.rockscene) はルート内の任意の場所に置き、マテリアル (.rockmat)・天球 (.rocksky)・モデル (.rockmodel) などは
// 個別ファイルとして共有する。参照は `{"uid", "path"}` で、走査した ID から現在のパスを引く。
// 仕様は docs/design/project-workspace.md。
class ProjectWorkspace {
public:
    bool Open(const std::filesystem::path& root);
    bool Scan();
    const std::filesystem::path& Root() const { return m_root; }
    bool IsOpen() const { return !m_root.empty(); }
    std::filesystem::path StartupScene() const;
    bool SetStartupScene(const std::filesystem::path& scene);
    bool Contains(const std::filesystem::path& path) const;
    // ルート外のファイルを directory へコピーして、その先を返す。ルート内ならそのまま返す。
    std::filesystem::path Import(const std::filesystem::path& source,
                                 const std::filesystem::path& directory);
    std::filesystem::path UniquePath(const std::filesystem::path& directory,
                                     const std::string& name, const char* extension) const;
    std::filesystem::path Resolve(const nlohmann::json& reference) const;
    nlohmann::json Reference(const std::filesystem::path& path);
    // 中身が変わらなければ書き込まない（更新日時を保ち、サムネイルのキャッシュを無効にしない）。
    bool SaveAsset(std::filesystem::path& path, const char* kind, nlohmann::json& body);
    // ID を持たない本文（旧 .reproj の埋め込みなど）と同じ中身の既存アセットの ID を探す。
    // 無ければ空。claimedUids の ID は、同じ保存で別のアセットが使うので対象にしない。
    std::string FindIdenticalAsset(const char* kind, const nlohmann::json& body,
                                   const std::unordered_set<std::string>& claimedUids) const;
    bool ReadAsset(const std::filesystem::path& path, const char* kind, nlohmann::json& body) const;
    // 既存の保存器が作った文書（埋め込みのマテリアル・天球、相対パスの画像）を
    // 共有アセットへ分離してシーンを書く。
    bool SaveScene(const std::filesystem::path& path, nlohmann::json& document);
    bool ReadScene(const std::filesystem::path& path, nlohmann::json& document);
    // 共有アセットの参照を、既存の読み込み器が扱う埋め込み文書へ展開する。
    bool Expand(nlohmann::json& document);
    static bool ReadJson(const std::filesystem::path& path, nlohmann::json& document);
    static bool WriteJson(const std::filesystem::path& path, const nlohmann::json& document);
    static std::string String(const nlohmann::json& value, const char* key);
    // ルートの目印ファイルか（ルートを指定する経路で使う）。
    static bool IsWorkspaceFile(const std::filesystem::path& path);

private:
    std::filesystem::path m_root;
    nlohmann::json m_project;
    std::unordered_map<std::string, std::filesystem::path> m_paths;
    std::unordered_map<std::string, std::filesystem::path> m_imports;
    std::unordered_map<std::string, std::string> m_knownUids;
};

}  // namespace rock::io
