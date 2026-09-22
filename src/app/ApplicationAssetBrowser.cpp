// アセットの帯。プロジェクトのルートフォルダの階層と、フォルダの中身（画像・マテリアル・
// 天球・モデル・シーン）をサムネイルの格子で出す。
// 旧「テクスチャ / マテリアル / 天球」のタブの代わり。
//
// 一覧はファイルそのもの。**シーンへ読み込んでいないものも見える。** ダブルクリックで
// 読み込み（画像 → テクスチャ、.rockmat → マテリアル、.rocksky → 天球、.rockmodel → モデル、
// .fbx → モデルを作る、.rockscene → シーン）。
// 読み込み済みのものはライブラリのサムネイルとドラッグ元（ROCK_TEXTURE / ROCK_MATERIAL）を使い、
// 未読み込みのものは AssetThumbnailCache が
// 別領域で作ったサムネイルを出す。クリック / Ctrl / Shift で複数選択し、フォルダへドラッグで移動、
// F2 か右クリックでその場で改名する。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "core/Shell.h"
#include "io/AssetRelations.h"
#include "io/ProjectIo.h"
#include "io/ThumbnailStore.h"
#include "rhi/TextureReadback.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <unordered_map>
#include <functional>
#include <utility>

namespace rock {
namespace fs = std::filesystem;
namespace {

std::string Extension(const fs::path& path) {
    auto ext = ToUtf8Portable(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

bool IsImage(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".exr" || ext == ".tga" || ext == ".bmp";
}

// 比較用のキー。**ファイルシステムには触らない。** 帯は毎フレーム全ファイル × 読み込み済み全件を
// 突き合わせるので、weakly_canonical のような問い合わせを挟むとシーンを開いた途端に描画が落ちる。
// ライブラリとルートのパスはどちらも絶対パスなので、正規化と大文字小文字の同一視で足りる。
std::wstring PathKey(const fs::path& path) {
    std::wstring key = path.lexically_normal().wstring();
    for (wchar_t& c : key) {
        if (c == L'/') c = L'\\';
        else c = static_cast<wchar_t>(std::towlower(c));
    }
    return key;
}

bool SameFile(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    return PathKey(a) == PathKey(b);
}

// inner が outer の配下（outer 自身は含まない）か。
bool IsInside(const fs::path& inner, const fs::path& outer) {
    return !outer.empty() && PathKey(inner).starts_with(PathKey(outer) + L"\\");
}

// サムネイル枠の中央へ、タブ付きフォルダの輪郭だけを描く（字形ではなく図形で描く）。
void DrawFolderIcon(const ImVec2& min, const ImVec2& max) {
    const float size = std::min(max.x - min.x, max.y - min.y);
    const float left = (min.x + max.x) * 0.5f - size * 0.34f;
    const float top = (min.y + max.y) * 0.5f - size * 0.23f;
    const auto point = [&](float x, float y) { return ImVec2(left + size * x, top + size * y); };
    auto* draw = ImGui::GetWindowDrawList();
    // 閉じる位置は上辺の途中に置き、終点と始点の重複による線の歪みを避ける。
    draw->PathLineTo(point(0.12f, 0.0f));
    draw->PathLineTo(point(0.23f, 0.0f));
    draw->PathLineTo(point(0.31f, 0.08f));
    draw->PathLineTo(point(0.64f, 0.08f));
    draw->PathBezierCubicCurveTo(point(0.67f, 0.08f), point(0.68f, 0.09f), point(0.68f, 0.12f));
    draw->PathLineTo(point(0.68f, 0.44f));
    draw->PathBezierCubicCurveTo(point(0.68f, 0.47f), point(0.67f, 0.48f), point(0.64f, 0.48f));
    draw->PathLineTo(point(0.04f, 0.48f));
    draw->PathBezierCubicCurveTo(point(0.01f, 0.48f), point(0.0f, 0.47f), point(0.0f, 0.44f));
    draw->PathLineTo(point(0.0f, 0.04f));
    draw->PathBezierCubicCurveTo(point(0.0f, 0.01f), point(0.01f, 0.0f), point(0.04f, 0.0f));
    const auto color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float thickness = size / 42.0f;
    draw->PathStroke(color, ImDrawFlags_Closed, thickness);
    draw->AddLine(point(0.0f, 0.15f), point(0.68f, 0.15f), color, thickness);
}

}  // namespace

bool Application::IsAssetLoaded(const fs::path& path) const {
    if (SameFile(m_projectPath, path)) return true;
    for (const auto& a : m_textureLibrary.Entries()) if (SameFile(a.path, path)) return true;
    for (const auto& a : m_materialLibrary.Entries()) if (SameFile(a.assetPath, path)) return true;
    for (const auto& a : m_skyLibrary.Entries()) if (SameFile(a.assetPath, path) || SameFile(a.sky.hdriPath, path)) return true;
    for (const auto& a : m_models) if (SameFile(a.assetPath, path) || SameFile(a.path, path)) return true;
    return false;
}

bool Application::IsAssetSelected(const fs::path& path) const {
    return std::find(m_selectedAssets.begin(), m_selectedAssets.end(), path) != m_selectedAssets.end();
}

// toggle（Ctrl）なら追加 / 除外、range（Shift）なら起点から path までを並び順で選ぶ。どちらも無ければ単独。
void Application::SelectAsset(const fs::path& path, bool toggle, bool range) {
    const auto indexOf = [&](const fs::path& target) -> int {
        for (size_t i = 0; i < m_assetEntries.size(); ++i) if (m_assetEntries[i].path() == target) return int(i);
        return -1;
    };
    const int anchor = indexOf(m_assetSelectionAnchor), current = indexOf(path);
    if (range && anchor >= 0 && current >= 0) {
        if (!toggle) m_selectedAssets.clear();
        for (int i = std::min(anchor, current); i <= std::max(anchor, current); ++i)
            if (!IsAssetSelected(m_assetEntries[i].path())) m_selectedAssets.push_back(m_assetEntries[i].path());
        return;
    }
    if (toggle) {
        const auto found = std::find(m_selectedAssets.begin(), m_selectedAssets.end(), path);
        if (found != m_selectedAssets.end()) m_selectedAssets.erase(found);
        else m_selectedAssets.push_back(path);
    } else {
        m_selectedAssets.assign(1, path);
    }
    m_assetSelectionAnchor = path;
}

void Application::OpenAssetRename(const fs::path& path) {
    if (path.empty() || path.filename() == L"project.reproj") return;
    std::error_code error;
    // 拡張子は変えさせない。フォルダは名前全体を編集する。
    const auto name = fs::is_directory(path, error) ? path.filename() : path.stem();
    std::snprintf(m_assetRenameBuffer, sizeof(m_assetRenameBuffer), "%s", ToUtf8Portable(name).c_str());
    m_assetRenameTarget = path;
    m_assetRenameFocus = true;
    m_assetRenameInTree = false;
}

void Application::FinishAssetRename(bool commit) {
    const auto target = m_assetRenameTarget;
    m_assetRenameTarget.clear();
    m_assetRenameFocus = false;
    m_assetRenameInTree = false;
    if (!commit || target.empty() || m_assetRenameBuffer[0] == '\0') return;
    std::error_code error;
    const auto extension = fs::is_directory(target, error) ? fs::path{} : target.extension();
    auto name = std::string(m_assetRenameBuffer) + ToUtf8Portable(extension);
    if (name == ToUtf8Portable(target.filename())) return;
    m_pendingAssetRename = target;
    m_pendingAssetRenameName = std::move(name);
}

void Application::SyncAssetNamesToFiles() {
    const auto sync = [](const fs::path& assetPath, std::string& name) {
        if (assetPath.empty()) return;
        auto stem = ToUtf8Display(assetPath.stem());
        if (!stem.empty() && stem != name) name = std::move(stem);
    };
    for (const auto& a : m_materialLibrary.Entries()) {
        auto* asset = m_materialLibrary.FindMutable(a.id);
        sync(asset->assetPath, asset->name);
    }
    for (const auto& a : m_skyLibrary.Entries()) {
        auto* sky = m_skyLibrary.FindMutable(a.id);
        sync(sky->assetPath, sky->name);
    }
    for (auto& a : m_models) sync(a.assetPath, a.name);
}

bool Application::RequestAssetNameChange(const fs::path& assetPath, std::string& name, const char* newName) {
    if (newName == nullptr || newName[0] == '\0' || name == newName) return false;
    if (assetPath.empty()) {
        name = newName;
        return true;
    }
    std::error_code error;
    if (!fs::exists(assetPath, error)) {
        // まだ書き出していない（保存時にこの名前でファイルができる）。
        name = newName;
        return true;
    }
    m_pendingAssetRename = assetPath;
    m_pendingAssetRenameName = std::string(newName) + ToUtf8Portable(assetPath.extension());
    return false;
}

// 移動・改名したファイル（フォルダなら配下も）を指す、読み込み済みのアセットの絶対パスを付け替える。
// 参照は ID で解決するので、ファイル側はこれだけで追従する。
void Application::RelinkAssetPaths(const fs::path& from, const fs::path& to) {
    const auto source = from.lexically_normal();
    const auto remap = [&](fs::path& path) {
        if (path.empty()) return;
        const auto relative = path.lexically_normal().lexically_relative(source);
        if (relative.empty() || *relative.begin() == L"..") return;
        path = relative == L"." ? to : to / relative;
    };
    for (const auto& a : m_textureLibrary.Entries()) remap(m_textureLibrary.FindMutable(a.id)->path);
    for (const auto& a : m_materialLibrary.Entries()) remap(m_materialLibrary.FindMutable(a.id)->assetPath);
    for (const auto& a : m_skyLibrary.Entries()) {
        auto* sky = m_skyLibrary.FindMutable(a.id);
        remap(sky->assetPath);
        remap(sky->sky.hdriPath);
    }
    for (auto& a : m_models) {
        remap(a.assetPath);
        remap(a.path);
    }
    const auto previousProject = m_projectPath;
    remap(m_projectPath);
    if (m_projectPath != previousProject) {
        m_recentProjects.Remove(m_workspace.Root(), previousProject);
        m_recentProjects.Add(m_workspace.Root(), m_projectPath);
        UpdateWindowTitle();
    }
    for (auto& path : m_selectedAssets) remap(path);
    remap(m_assetSelectionAnchor);
    remap(m_assetDirectory);
    m_assetThumbnails.Invalidate();
}

void Application::AssetFolderDropTarget(const fs::path& directory) {
    if (!ImGui::BeginDragDropTarget()) return;
    // パスは改行区切りで複数運ばれてくる（複数選択）。読み込み済みのものを 1 つ運ぶときは
    // ライブラリの ID で運ばれてくるので、パスへ引き直す。
    std::vector<fs::path> sources;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPathDragDropType);
        payload != nullptr && payload->DataSize >= int(sizeof(wchar_t))) {
        const std::wstring text(static_cast<const wchar_t*>(payload->Data));
        for (size_t begin = 0; begin < text.size();) {
            const size_t end = std::min(text.find(L'\n', begin), text.size());
            if (end > begin) sources.emplace_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
    } else if (const ImGuiPayload* texture = ImGui::AcceptDragDropPayload(kTextureDragDropType);
               texture != nullptr && texture->DataSize == sizeof(compositor::TextureId)) {
        if (const auto* entry = m_textureLibrary.Find(*static_cast<const compositor::TextureId*>(texture->Data)))
            sources.push_back(entry->path);
    } else if (const ImGuiPayload* material = ImGui::AcceptDragDropPayload(kMaterialDragDropType);
               material != nullptr && material->DataSize == sizeof(compositor::MaterialAssetId)) {
        if (const auto* asset = m_materialLibrary.Find(*static_cast<const compositor::MaterialAssetId*>(material->Data)))
            sources.push_back(asset->assetPath);
    }
    bool accepted = false;
    for (const auto& source : sources) {
        // 今と同じフォルダ、フォルダ自身とその配下へは落とさない（その 1 件だけ外す）。
        if (source.empty() || !m_workspace.Contains(source) || SameFile(source.parent_path(), directory) ||
            SameFile(source, directory) || IsInside(directory, source)) continue;
        m_pendingAssetMoves.push_back(source);
        accepted = true;
    }
    if (accepted) m_pendingAssetMoveTarget = directory;
    ImGui::EndDragDropTarget();
}

void Application::DrawAssetDeleteDialog() {
    if (m_assetDeleteDialog && !ImGui::IsPopupOpen("アセットファイルの削除")) ImGui::OpenPopup("アセットファイルの削除");
    if (!ImGui::BeginPopupModal("アセットファイルの削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const auto& report = m_assetDeleteRelations;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::Scaled(520));
    ImGui::TextUnformatted(ToUtf8Display(report.target).c_str());
    ImGui::PopTextWrapPos();
    ui::HintText("元ファイルと付随する .meta を、ルート内の退避フォルダ（.rock-editor/trash）へ移します。");
    const auto list = [&](const char* title, const std::vector<fs::path>& paths) {
        if (paths.empty()) return;
        ImGui::Separator();
        ImGui::TextUnformatted(title);
        const float height = std::min(float(paths.size()), 5.0f) * ImGui::GetTextLineHeightWithSpacing() + ui::Scaled(12);
        if (ImGui::BeginChild(title, ImVec2(ui::Scaled(520), height), ImGuiChildFlags_Borders))
            for (const auto& path : paths)
                ImGui::TextUnformatted(ToUtf8Display(path.lexically_relative(m_workspace.Root())).c_str());
        ImGui::EndChild();
    };
    list("一緒に退避するファイル", report.companions);
    list("直接の参照元（削除すると参照切れになります）", report.referencers);
    list("関連ファイル（削除せず残します）", report.related);
    const bool loaded = IsAssetLoaded(report.target);
    if (!report.complete) ui::HintText("参照関係をすべて確認できませんでした。読めないファイルやリンクを確認してください。");
    if (loaded) ui::HintText("現在のシーンに読み込まれています。新規シーンなどへ切り替えてから削除してください。");
    ImGui::Separator();
    if (!m_assetDeleteQueue.empty()) ui::HintText("残り %zu 件。続けて確認します。", m_assetDeleteQueue.size());
    ImGui::BeginDisabled(!report.complete || loaded);
    if (ImGui::Button("削除する")) {
        m_pendingAssetDelete = true;
        m_assetDeleteDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_assetDeleteDialog = false;
        m_assetDeleteQueue.clear();  // 複数を選んで消している途中の取り消しは、残りも止める。
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::ResumeSceneSwitch() {
    m_pendingRoot = std::move(m_deferredRoot);
    m_deferredRoot.clear();
    m_pendingProjectOpen = std::move(m_deferredScene);
    m_deferredScene.clear();
    m_pendingProjectNew = m_deferredNew;
    m_deferredNew = false;
    m_allowSceneSwitch = true;
    m_sceneSwitchDialog = false;
}

void Application::DrawSceneSwitchDialog() {
    if (m_sceneSwitchDialog && !ImGui::IsPopupOpen("シーンの切り替え")) ImGui::OpenPopup("シーンの切り替え");
    if (!ImGui::BeginPopupModal("シーンの切り替え", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ui::HintText("現在のシーンと共有アセットを保存してから切り替えますか？");
    if (ImGui::Button("保存して切り替え")) {
        RequestSaveProject(false);
        if (!m_pendingProjectSave.empty()) {
            m_saveThenSwitch = true;
            m_sceneSwitchDialog = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("保存せず切り替え")) {
        ResumeSceneSwitch();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル")) {
        m_sceneSwitchDialog = false;
        m_deferredRoot.clear();
        m_deferredScene.clear();
        m_deferredNew = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::RefreshAssetBrowser() {
    m_assetEntries.clear();
    std::error_code error;
    if (!m_workspace.Contains(m_assetDirectory) || !fs::is_directory(m_assetDirectory, error)) {
        m_assetDirectory = m_workspace.Root();
    }
    fs::directory_iterator it(m_assetDirectory, fs::directory_options::skip_permission_denied, error), end;
    for (; it != end && !error; it.increment(error)) {
        const auto& entry = *it;
        // .meta と内部フォルダ（.rock-editor）は出さない。
        if (entry.is_symlink(error) || entry.path().filename().wstring().starts_with(L".")) continue;
        const auto ext = Extension(entry.path());
        if (!entry.is_directory(error) && !IsImage(ext) && ext != ".hdr" && ext != ".rockmat" && ext != ".rocksky" &&
            ext != ".rockmodel" && ext != ".fbx" && ext != ".rockscene") continue;
        m_assetEntries.push_back(entry);
    }
    std::sort(m_assetEntries.begin(), m_assetEntries.end(), [](const auto& a, const auto& b) {
        std::error_code errorA, errorB;
        const bool directoryA = a.is_directory(errorA), directoryB = b.is_directory(errorB);
        if (directoryA != directoryB) return directoryA;
        return a.path().filename() < b.path().filename();
    });
    // フォルダ階層。ドット始まりと symlink は出さない。深さは 32 まで。
    m_assetFolders.clear();
    const auto collect = [&](auto&& self, const fs::path& directory, int depth) -> void {
        if (depth > 32) return;
        auto& children = m_assetFolders[directory.wstring()];
        std::error_code scanError;
        fs::directory_iterator child(directory, fs::directory_options::skip_permission_denied, scanError), childEnd;
        for (; child != childEnd && !scanError; child.increment(scanError))
            if (child->is_directory(scanError) && !child->is_symlink(scanError) &&
                !child->path().filename().wstring().starts_with(L".")) children.push_back(child->path());
        std::sort(children.begin(), children.end());
        for (const auto& path : children) self(self, path, depth + 1);
    };
    collect(collect, m_workspace.Root(), 0);
    std::error_code stampError;
    m_assetDirectoryStamp = fs::last_write_time(m_assetDirectory, stampError);
    m_assetDirectoryChecked = ImGui::GetTime();
    m_assetRefresh = false;
}

// フレームの外で処理するアセット関連の作業（移動・改名、ルートの切り替え、共有アセットの保存、
// ファイルを開く、削除、サムネイルの生成）。ProcessPendingFileWork から呼ぶ。
void Application::ProcessAssetWork() {
    if (!m_pendingAssetMoves.empty()) {
        const auto requested = std::exchange(m_pendingAssetMoves, {});
        const auto directory = std::exchange(m_pendingAssetMoveTarget, {});
        // フォルダと一緒にその中身も選ばれていたら、中身はフォルダごと運ばれるので個別には動かさない。
        std::vector<fs::path> sources;
        for (const auto& source : requested) {
            const bool nested = std::any_of(requested.begin(), requested.end(),
                                            [&](const fs::path& other) { return IsInside(source, other); });
            if (!nested && std::none_of(sources.begin(), sources.end(), [&](const fs::path& p) { return SameFile(p, source); }))
                sources.push_back(source);
        }
        size_t count = 0;
        for (const auto& source : sources) {
            const auto moved = io::MoveAsset(m_workspace, source, directory);
            if (moved.empty() || moved == source) continue;
            // 読み込み済みのアセットは絶対パスを持つので、移動先へ付け替える。
            RelinkAssetPaths(source, moved);
            ++count;
        }
        if (count > 0) {
            ROCK_LOG_INFO("%zu 件を移動しました → %s", count,
                        ToUtf8Display(directory.lexically_relative(m_workspace.Root())).c_str());
        }
        m_assetRefresh = true;
    }
    if (!m_pendingAssetRename.empty()) {
        const auto source = std::exchange(m_pendingAssetRename, {});
        const auto renamed = io::RenameAsset(m_workspace, source, m_pendingAssetRenameName);
        if (!renamed.empty() && renamed != source) {
            RelinkAssetPaths(source, renamed);
            ROCK_LOG_INFO("名前を変更しました: %s → %s", ToUtf8Display(source.filename()).c_str(),
                        ToUtf8Display(renamed.filename()).c_str());
        }
        m_assetRefresh = true;
    }
    if (m_pendingAssetDelete) {
        m_pendingAssetDelete = false;
        const auto path = m_assetDeleteRelations.target;
        if (!IsAssetLoaded(path) && io::RetireAsset(m_workspace, m_assetDeleteRelations)) {
            m_recentProjects.Remove(m_workspace.Root(), path);
            std::erase_if(m_selectedAssets, [&](const fs::path& selected) { return SameFile(selected, path); });
            m_assetRefresh = true;
            m_assetThumbnails.Invalidate();
        } else {
            ROCK_LOG_WARN("削除できませんでした。対象と参照関係を再確認してください");
            m_pendingAssetDeleteInspect = path;
        }
    }
    // Del キーの残り。ダイアログが閉じていて、削除の実行も待っていなければ次を検査する。
    if (!m_assetDeleteQueue.empty() && m_pendingAssetDeleteInspect.empty() && !m_assetDeleteDialog && !m_pendingAssetDelete) {
        m_pendingAssetDeleteInspect = m_assetDeleteQueue.front();
        m_assetDeleteQueue.erase(m_assetDeleteQueue.begin());
    }
    if (!m_pendingAssetDeleteInspect.empty()) {
        std::error_code e;
        if (fs::exists(m_pendingAssetDeleteInspect, e)) {
            m_assetDeleteRelations = io::InspectAssetRelations(m_workspace, m_pendingAssetDeleteInspect);
            m_assetDeleteDialog = true;
        } else {
            // 外で先に消されていた。一覧から外すだけでよい。
            m_assetRefresh = true;
        }
        m_pendingAssetDeleteInspect.clear();
    }
    // 表示中のフォルダが外で変わっていたら（エクスプローラでの削除など）、一覧を作り直す。
    // 消えたファイルが幽霊のように残らないようにする。確認は 1 秒ごと。
    if (!m_assetRefresh && m_workspace.IsOpen() && ImGui::GetTime() - m_assetDirectoryChecked > 1.0) {
        m_assetDirectoryChecked = ImGui::GetTime();
        std::error_code e;
        const auto stamp = fs::last_write_time(m_assetDirectory, e);
        if (e || stamp != m_assetDirectoryStamp) m_assetRefresh = true;
    }
    // --project にはシーンだけでなく、ルートのフォルダや目印ファイル（project.reproj）も渡せる。
    if (!m_pendingProjectOpen.empty()) {
        std::error_code error;
        if (fs::is_directory(m_pendingProjectOpen, error)) {
            m_pendingRoot = m_pendingProjectOpen;
            m_pendingProjectOpen.clear();
        } else if (io::ProjectWorkspace::IsWorkspaceFile(m_pendingProjectOpen)) {
            m_pendingRoot = m_pendingProjectOpen.parent_path();
            m_pendingProjectOpen.clear();
        }
    }
    if (!m_pendingRoot.empty()) {
        const auto root = m_pendingRoot;
        m_pendingRoot.clear();
        io::ProjectWorkspace next;
        if (next.Open(root)) {
            // ルートの選択とシーンの選択を分ける。履歴から指定されたシーンだけ開く。
            if (!m_pendingProjectOpen.empty() && Extension(m_pendingProjectOpen) == ".rockscene") {
                nlohmann::json validation;
                if (!next.ReadScene(m_pendingProjectOpen, validation)) {
                    ROCK_LOG_ERROR("シーンを読み込めません。現在のプロジェクトを保持します");
                    m_pendingProjectOpen.clear();
                    return;
                }
            }
            m_workspace = std::move(next);
            m_assetDirectory = m_workspace.Root();
            m_selectedAssets.clear();
            m_assetSelectionAnchor.clear();
            m_assetRenameTarget.clear();
            m_assetRefresh = true;
            m_assetThumbnails.Invalidate();
            if (!Headless()) m_recentProjects.AddRoot(m_workspace.Root());
            ResetProject();
            m_projectPath.clear();
            UpdateWindowTitle();
            ROCK_LOG_INFO("ルートフォルダを開きました: %s", ToUtf8Display(m_workspace.Root()).c_str());
        } else {
            m_pendingProjectOpen.clear();
        }
    }
    // 作成したマテリアル・天球などの共有アセットをすぐファイルへ書く（シーンの保存でも書かれる）。
    if (m_pendingAssetsSave) {
        m_pendingAssetsSave = false;
        io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_skyLibrary, m_renderer, m_graph, &m_models};
        if (!io::SaveSharedAssets(m_workspace, refs)) {
            ROCK_LOG_ERROR("共有アセットを保存できませんでした");
        }
        m_assetRefresh = true;
        m_assetThumbnails.Invalidate();
    }
    if (!m_pendingAssetOpen.empty()) {
        const auto path = m_pendingAssetOpen;
        m_pendingAssetOpen.clear();
        const auto ext = Extension(path);
        if (ext == ".rockmodel") {
            // シーンのモデルへ足し（読み込み済みならそれを使い）、参照するマテリアルもライブラリへ読む。
            const size_t before = m_models.size();
            if (io::LoadSharedAsset(m_workspace, path, m_device, m_pipelineCache, m_textureLibrary, m_materialLibrary,
                                    m_skyLibrary, true, &m_models)) {
                for (const auto& model : m_models)
                    if (SameFile(model.assetPath, path)) m_selectedModel = model.id;
                m_modelLod = 0;
                m_showModelPreview = true;
                if (m_models.size() != before) MarkDocumentChanged();
            } else {
                ROCK_LOG_ERROR("アセットを開けません: %s", ToUtf8Display(path).c_str());
            }
        } else if (ext == ".fbx") {
            m_pendingModelImports.push_back(path);
        } else if (ext == ".rockmat" || ext == ".rocksky") {
            nlohmann::json header;
            if (ext == ".rockmat" && io::ProjectWorkspace::ReadJson(path, header) &&
                io::ProjectWorkspace::String(header, "format") != "rock-editor.material-asset") {
                // 持ち出し用の旧 .rockmat は従来の読み込み（ライブラリへ 1 つ足す）。
                m_pendingMaterialImport = path;
                return;
            }
            if (io::LoadSharedAsset(m_workspace, path, m_device, m_pipelineCache,
                                    m_textureLibrary, m_materialLibrary, m_skyLibrary)) {
                if (ext == ".rockmat") {
                    const auto& entries = m_materialLibrary.Entries();
                    for (size_t i = 0; i < entries.size(); ++i)
                        if (SameFile(entries[i].assetPath, path)) m_selectedMaterial = static_cast<int>(i);
                    m_showMaterialSphere = true;
                    m_scrollToSelectedMaterial = true;
                } else {
                    m_showSkyPreview = true;
                    m_scrollToSelectedSky = true;
                }
                m_renderer.InvalidateSceneMaterials();
                MarkDocumentChanged();
            } else {
                ROCK_LOG_ERROR("アセットを開けません: %s", ToUtf8Display(path).c_str());
            }
        } else if (IsImage(ext)) {
            const compositor::TextureId id = m_textureLibrary.Load(m_device, m_pipelineCache, path);
            if (id != compositor::kNoTexture) {
                const auto& entries = m_textureLibrary.Entries();
                for (size_t i = 0; i < entries.size(); ++i)
                    if (entries[i].id == id) m_selectedTexture = static_cast<int>(i);
                m_showTexturePreview = true;
                m_scrollToSelectedTexture = true;
            }
        } else {
            HandleDroppedFiles({path});
        }
        m_assetRefresh = true;
    }
    if (m_assetRefresh) RefreshAssetBrowser();
    m_assetThumbnails.SetModelLighting({&m_renderer.GetEnvironment(), m_renderer.EnvironmentIntensity(), m_renderer.EffectiveLight(),
                                        m_renderer.Exposure().Exposure(), m_renderer.Tonemap()});
    m_assetThumbnails.Process(m_device, m_pipelineCache, m_workspace, m_assetDirectory);
}

void Application::DrawAssetBrowser() {
    if (!ImGui::Begin("アセット")) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("ルートを開く…")) RequestOpenProject();
    ImGui::SameLine();
    if (ImGui::Button("更新")) {
        m_assetRefresh = true;
        m_workspace.Scan();
        m_assetThumbnails.Invalidate();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ToUtf8Display(m_assetDirectory).c_str());

    // フォルダを作り、すぐ名前の入力へ入る（エクスプローラと同じ）。一覧は作った先のフォルダの中身を出す。
    const auto createFolder = [&](const fs::path& parent) {
        const auto path = m_workspace.UniquePath(parent, "新しいフォルダー", "");
        std::error_code error;
        if (!path.empty()) fs::create_directory(path, error);
        if (path.empty() || error) {
            ROCK_LOG_ERROR("フォルダを作成できませんでした");
            return;
        }
        m_assetDirectory = parent;
        m_assetRefresh = true;
        m_selectedAssets.assign(1, path);
        m_assetSelectionAnchor = path;
        OpenAssetRename(path);
    };

    const auto available = ImGui::GetContentRegionAvail();
    const float margin = ui::Scaled(ui::kSplitterMargin);
    const float usable = std::max(2.0f, available.x - margin * 2.0f - ui::Scaled(ui::kSplitterGrabWidth));
    const float minimum = std::min(ui::Scaled(120.0f), usable * 0.5f);
    float folderWidth = std::clamp(ui::Scaled(m_settings.Ui().assetFolderWidth), minimum, usable - minimum);
    // --- 左: フォルダ階層 ------------------------------------------------
    if (ImGui::BeginChild("folders", ImVec2(folderWidth, 0), ImGuiChildFlags_Borders)) {
        const auto tree = [&](auto&& self, const fs::path& directory, int depth) -> void {
            if (depth > 32) return;
            const auto label = ToUtf8Display(directory.filename());
            const bool root = directory == m_workspace.Root();
            // 改名中の行は名前の代わりに入力欄を並べる。行いっぱいに広げると入力欄と重なるので広げない。
            const bool renaming = m_assetRenameInTree && directory == m_assetRenameTarget;
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
            if (!renaming) flags |= ImGuiTreeNodeFlags_SpanAvailWidth;
            if (root) flags |= ImGuiTreeNodeFlags_DefaultOpen;
            if (directory == m_assetDirectory) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::PushID(ToUtf8Portable(directory).c_str());
            const bool open = ImGui::TreeNodeEx(renaming ? "##folder" : label.c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                m_assetDirectory = directory;
                m_assetRefresh = true;
            }
            // 行はドラッグ元（フォルダごと移動）とドロップ先を兼ねる。ルートは動かさない。
            if (!root && !renaming && ImGui::BeginDragDropSource()) {
                const std::wstring text = directory.wstring();
                ImGui::SetDragDropPayload(kAssetPathDragDropType, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            AssetFolderDropTarget(directory);
            // 行の右クリックメニュー。ルートは改名できない。
            if (ImGui::BeginPopupContextItem("folderMenu")) {
                if (ImGui::MenuItem("開く")) {
                    m_assetDirectory = directory;
                    m_assetRefresh = true;
                }
                if (ImGui::MenuItem("エクスプローラで表示")) RevealFileInExplorer(directory);
                ImGui::Separator();
                if (ImGui::MenuItem("フォルダを作成")) createFolder(directory);
                if (!root && ImGui::MenuItem("名前を変更…")) {
                    OpenAssetRename(directory);
                    m_assetRenameInTree = true;
                }
                ImGui::EndPopup();
            }
            if (renaming) {
                ImGui::SameLine();
                const float width = std::max(ui::Scaled(60.0f), ImGui::GetContentRegionAvail().x);
                const auto edit = ui::InlineNameInput("##rename", m_assetRenameBuffer, sizeof(m_assetRenameBuffer), width, &m_assetRenameFocus);
                if (edit != ui::CaptionEdit::Editing) FinishAssetRename(edit == ui::CaptionEdit::Commit);
            }
            if (open) {
                if (const auto found = m_assetFolders.find(directory.wstring()); found != m_assetFolders.end())
                    for (const auto& child : found->second) self(self, child, depth + 1);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        tree(tree, m_workspace.Root(), 0);
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, margin);
    const float previousWidth = folderWidth;
    const bool released = ui::VerticalSplitter("assetFolderSplitter", &folderWidth, minimum, usable - minimum, available.y);
    if (folderWidth != previousWidth) m_settings.Ui().assetFolderWidth = folderWidth / ui::Scaled(1.0f);
    if (released) m_settings.Save();
    ImGui::SameLine(0.0f, margin);
    // --- 右: フォルダの中身 ----------------------------------------------
    if (ImGui::BeginChild("contents", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const ImGuiIO& io = ImGui::GetIO();
        if (m_assetDirectory != m_workspace.Root() && ImGui::Button("上のフォルダ")) {
            m_assetDirectory = m_assetDirectory.parent_path();
            m_assetRefresh = true;
        }
        if (m_assetEntries.empty()) ui::HintText("右クリックでアセットを作成、またはファイルを読み込みます");
        // 選択は表示中のフォルダの中だけ。別のフォルダへ移ったら、そこに無いものは外す。
        std::erase_if(m_selectedAssets, [&](const fs::path& path) { return path.parent_path() != m_assetDirectory; });
        // 一覧のキー操作。テキスト入力中・ダイアログ表示中・改名中は効かせない。
        if ((ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) && !io.WantTextInput && !m_assetDeleteDialog &&
            m_assetRenameTarget.empty()) {
            if (m_selectedAssets.size() == 1 && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
                OpenAssetRename(m_selectedAssets.front());
            } else if (!m_selectedAssets.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                // 選んだものを順に削除（退避）する。確認は 1 件ずつ。フォルダは対象外（右クリックからも消せない）。
                m_assetDeleteQueue.clear();
                for (const auto& path : m_selectedAssets) {
                    std::error_code e;
                    if (!fs::is_directory(path, e)) m_assetDeleteQueue.push_back(path);
                }
            } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
                m_selectedAssets.clear();
                for (const auto& entry : m_assetEntries) m_selectedAssets.push_back(entry.path());
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_selectedAssets.clear();
            }
        }
        // 一覧で改名中に別のフォルダへ移ったら、入力欄が描かれなくなるので取り消す。
        if (!m_assetRenameTarget.empty() && !m_assetRenameInTree && m_assetRenameTarget.parent_path() != m_assetDirectory)
            FinishAssetRename(false);
        const float size = ui::Scaled(84);
        const int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / (size + ImGui::GetStyle().ItemSpacing.x)));
        // 読み込み済みのものをパスで引く表。ファイルごとにライブラリを総なめしない。
        struct Loaded {
            ImTextureID handle = 0;
            compositor::TextureId texture = compositor::kNoTexture;
            compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
            bool missing = false;
            uint64_t model = 0;
        };
        std::unordered_map<std::wstring, Loaded> loaded;
        for (const auto& a : m_textureLibrary.Entries()) if (!a.path.empty())
            loaded[PathKey(a.path)] = {static_cast<ImTextureID>(a.PreviewHandle().ptr), a.id, compositor::kNoMaterialAsset, a.missing};
        for (const auto& a : m_materialLibrary.Entries()) if (!a.assetPath.empty())
            loaded[PathKey(a.assetPath)] = {static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr), compositor::kNoTexture, a.id, MaterialHasMissingTexture(a)};
        for (const auto& a : m_skyLibrary.Entries()) if (!a.assetPath.empty())
            loaded[PathKey(a.assetPath)] = {static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr)};
        // モデルは .rockmodel と元の FBX の両方を読み込み済みとして扱う（FBX のダブルクリックでそのモデルを開く）。
        for (const auto& a : m_models) {
            const auto preview = m_modelPreviews.find(a.id);
            Loaded entry;
            entry.handle = preview != m_modelPreviews.end() && m_renderedModelThumbnails.contains(a.id)
                               ? static_cast<ImTextureID>(preview->second->OutputHandle().ptr) : 0;
            entry.missing = !a.geometry;
            entry.model = a.id;
            if (!a.assetPath.empty()) loaded[PathKey(a.assetPath)] = entry;
            if (!a.path.empty()) loaded[PathKey(a.path)] = entry;
        }
        int index = 0;
        for (const auto& entry : m_assetEntries) {
            const auto path = entry.path();
            const auto ext = Extension(path);
            std::error_code error;
            const bool folder = entry.is_directory(error);
            ImTextureID handle = 0;
            compositor::TextureId textureId = compositor::kNoTexture;
            compositor::MaterialAssetId materialId = compositor::kNoMaterialAsset;
            uint64_t modelId = 0;
            bool missing = false;
            if (const auto found = loaded.find(PathKey(path)); found != loaded.end()) {
                handle = found->second.handle;
                textureId = found->second.texture;
                materialId = found->second.material;
                modelId = found->second.model;
                missing = found->second.missing;
            }
            if (!handle && !folder && ImGui::IsRectVisible(ImVec2(size, size)))
                handle = static_cast<ImTextureID>(m_assetThumbnails.Request(path).ptr);
            ImGui::PushID(ToUtf8Portable(path).c_str());
            ImGui::BeginGroup();
            const bool selected = IsAssetSelected(path);
            // 名前の行まで含めて 1 つの当たり判定にする（名前を押しても選べる・掴める・落とせる）。
            const float spacing = ImGui::GetStyle().ItemSpacing.y;
            const auto thumb = ui::ThumbnailButton("##asset", handle, size, selected,
                                                   spacing + ui::GridCaptionHeight());
            const ImVec2 afterTile = ImGui::GetCursorScreenPos();
            if (folder) {
                DrawFolderIcon(thumb.min, thumb.max);
            } else if (!handle) {
                const char* type = ext == ".rockscene" ? "シーン" : ext == ".rockmat" ? "マテリアル" :
                    ext == ".rocksky" ? "天球" :
                    (ext == ".rockmodel" || ext == ".fbx") ? "モデル" :
                    IsImage(ext) || ext == ".hdr" ? "画像" : "ファイル";
                const auto min = thumb.min, max = thumb.max;
                const auto text = ImGui::CalcTextSize(type);
                ImGui::GetWindowDrawList()->AddText(ImVec2((min.x + max.x - text.x) * 0.5f, (min.y + max.y - text.y) * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), type);
            }
            if (missing || (!handle && m_assetThumbnails.Failed(path)))
                ui::MissingBadge(thumb.min, thumb.max);
            // 押した時点で選ぶ（ドラッグの前に選択を決める）。複数選択のうちの 1 つを修飾キーなしで押したときは、
            // 全部をそのままドラッグできるよう選択を残し、ドラッグせずに離したら単独へ絞る。
            const bool plain = !io.KeyCtrl && !io.KeyShift;
            if (thumb.clicked) {
                if (!(plain && selected && m_selectedAssets.size() > 1)) SelectAsset(path, io.KeyCtrl, io.KeyShift);
                if (plain) {
                    // 読み込み済みのものは、一覧での選択もそれへ合わせる（プレビューの窓が追従する）。
                    if (textureId != compositor::kNoTexture) {
                        const auto& entries = m_textureLibrary.Entries();
                        for (size_t i = 0; i < entries.size(); ++i) if (entries[i].id == textureId) m_selectedTexture = int(i);
                    }
                    if (materialId != compositor::kNoMaterialAsset) {
                        const auto& entries = m_materialLibrary.Entries();
                        for (size_t i = 0; i < entries.size(); ++i) if (entries[i].id == materialId) m_selectedMaterial = int(i);
                    }
                    // 編集ウィンドウが開いていれば、選んだものへ内容を切り替える（フォーカスは移さない）。
                    if (modelId && m_selectedModel != modelId) {
                        m_selectedModel = modelId;
                        m_modelLod = 0;
                    }
                }
            }
            if (plain && thumb.hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left) && IsAssetSelected(path) && m_selectedAssets.size() > 1) {
                m_selectedAssets.assign(1, path);
                m_assetSelectionAnchor = path;
            }
            if (thumb.doubleClicked) {
                if (folder) {
                    m_assetDirectory = path;
                    m_assetRefresh = true;
                } else if (textureId != compositor::kNoTexture) {
                    m_showTexturePreview = true;
                } else if (materialId != compositor::kNoMaterialAsset) {
                    m_showMaterialSphere = true;
                } else if (modelId) {
                    m_selectedModel = modelId;
                    m_showModelPreview = true;
                    ImGui::SetWindowFocus("モデルプレビュー");
                } else {
                    m_pendingAssetOpen = path;
                }
            }
            // ドラッグ元。複数選んでいるうちの 1 つを掴んだら、選択全部のパスを改行区切りで運ぶ（フォルダへの移動だけ）。
            // 1 つだけなら、読み込み済みのものは従来と同じ ID のペイロード（割り当ての欄が受ける）で、
            // それ以外のファイル・フォルダはパスを運ぶ。どちらもフォルダへ落とせば移動する（AssetFolderDropTarget）。
            // テクスチャは hold-to-switch を残すため SourceNoHoldToOpenOthers を付けない。
            const bool movable = path.filename() != L"project.reproj";
            if (movable && ImGui::BeginDragDropSource()) {
                std::wstring list;
                size_t count = 0;
                if (IsAssetSelected(path) && m_selectedAssets.size() > 1) {
                    for (const auto& item : m_selectedAssets) {
                        if (item.filename() == L"project.reproj") continue;
                        if (!list.empty()) list += L'\n';
                        list += item.wstring();
                        ++count;
                    }
                }
                if (count > 1) {
                    ImGui::SetDragDropPayload(kAssetPathDragDropType, list.c_str(), (list.size() + 1) * sizeof(wchar_t));
                    ImGui::Text("%zu 件", count);
                } else {
                    if (materialId != compositor::kNoMaterialAsset) {
                        ImGui::SetDragDropPayload(kMaterialDragDropType, &materialId, sizeof(materialId));
                    } else if (textureId != compositor::kNoTexture) {
                        ImGui::SetDragDropPayload(kTextureDragDropType, &textureId, sizeof(textureId));
                    } else {
                        const std::wstring text = path.wstring();
                        ImGui::SetDragDropPayload(kAssetPathDragDropType, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                    }
                    ImGui::TextUnformatted(ToUtf8Display(path.filename()).c_str());
                }
                ImGui::EndDragDropSource();
            }
            if (folder) AssetFolderDropTarget(path);
            // ドラッグ中は ImGui が運んでいるものを出すので、ツールチップを重ねない。
            if (thumb.hovered && ImGui::GetDragDropPayload() == nullptr) {
                const char* hint = folder ? "ダブルクリックで開く / サムネイルを落とすと移動"
                    : modelId ? "ダブルクリックでモデルプレビュー（マテリアルスロットの割り当て）"
                    : ext == ".fbx" ? "ダブルクリックでモデル（.rockmodel）を作ってシーンへ読み込む"
                    : ext == ".rockmodel" ? "ダブルクリックでシーンへ読み込んでプレビュー"
                    : "ダブルクリックで開く";
                ImGui::SetTooltip("%s\n%s\nCtrl / Shift + クリックで複数選択", ToUtf8Display(path).c_str(), hint);
            }
            if (ImGui::BeginPopupContextItem("assetMenu")) {
                if (!IsAssetSelected(path)) SelectAsset(path, false, false);
                if (ImGui::MenuItem("開く")) {
                    if (folder) { m_assetDirectory = path; m_assetRefresh = true; }
                    else m_pendingAssetOpen = path;
                }
                if (ImGui::MenuItem("エクスプローラで表示")) RevealFileInExplorer(path);
                if (path.filename() != L"project.reproj" && ImGui::MenuItem("名前を変更…", "F2")) OpenAssetRename(path);
                if (textureId != compositor::kNoTexture) {
                    ImGui::Separator();
                    DrawTextureContextMenu(textureId);
                } else if (materialId != compositor::kNoMaterialAsset) {
                    ImGui::Separator();
                    DrawMaterialContextMenu(materialId);
                } else if (modelId) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("プレビュー")) {
                        m_selectedModel = modelId;
                        m_showModelPreview = true;
                    }
                    // ファイルは残す。
                    if (ImGui::MenuItem("シーンから外す")) m_pendingModelRemove = modelId;
                }
                if (!folder && path.filename() != L"project.reproj") {
                    ImGui::Separator();
                    if (ImGui::MenuItem("ファイルを削除…")) m_pendingAssetDeleteInspect = path;
                }
                ImGui::EndPopup();
            }
            // 名前は当たり判定のアイテムの上に重ねて描く（文字は操作を奪わない。
            // 改名の入力欄は後から置くアイテムなので、そちらが優先して受ける）。
            ImGui::SetCursorScreenPos(ImVec2(thumb.min.x, thumb.max.y + spacing));
            if (path == m_assetRenameTarget && !m_assetRenameInTree) {
                const auto edit = ui::GridCaptionInput("##rename", m_assetRenameBuffer, sizeof(m_assetRenameBuffer), size, &m_assetRenameFocus);
                if (edit != ui::CaptionEdit::Editing) FinishAssetRename(edit == ui::CaptionEdit::Commit);
            } else {
                ui::GridCaption(ToUtf8Display(path.filename()).c_str(), size);
            }
            // 名前の描画で進んだカーソルをアイテムの下へ揃える。位置を戻すだけだと
            // ImGui が「境界を広げる SetCursorPos」と見なして警告するので、Dummy で確定する。
            const float below = afterTile.y - ImGui::GetCursorScreenPos().y;
            if (below > 0.0f) ImGui::Dummy(ImVec2(size, below));
            ImGui::EndGroup();
            ImGui::PopID();
            if (++index % columns && index < int(m_assetEntries.size())) ImGui::SameLine();
        }
        // 余白をクリックしたら選択を外す（修飾キー付きなら残す）。
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
            !io.KeyCtrl && !io.KeyShift) {
            m_selectedAssets.clear();
        }
        if (ImGui::BeginPopupContextWindow("createAsset", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("フォルダを作成")) createFolder(m_assetDirectory);
            if (ImGui::MenuItem("マテリアルを作成")) {
                const auto id = m_materialLibrary.Add("新規マテリアル");
                auto* asset = m_materialLibrary.FindMutable(id);
                asset->assetPath = m_workspace.UniquePath(m_assetDirectory, asset->name, ".rockmat");
                m_selectedMaterial = static_cast<int>(m_materialLibrary.Entries().size()) - 1;
                m_showMaterialSphere = true;
                m_pendingAssetsSave = true;
                MarkDocumentChanged();
            }
            if (ImGui::MenuItem("天球を作成")) {
                const auto id = m_skyLibrary.Add("新規天球");
                auto* asset = m_skyLibrary.FindMutable(id);
                asset->assetPath = m_workspace.UniquePath(m_assetDirectory, asset->name, ".rocksky");
                m_skyLibrary.SetActive(id);
                m_showSkyPreview = true;
                m_pendingAssetsSave = true;
            }
            if (ImGui::MenuItem("ファイルを読み込む…")) {
                const auto paths = ShowOpenFilesDialog(
                    L"アセットを読み込む",
                    {{L"画像 / HDRI / マテリアル / モデル", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr;*.hdr;*.rockmat;*.fbx"}});
                HandleDroppedFiles(paths);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace rock
