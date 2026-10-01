// 「テンプレートから作成」。岩のテンプレート（examples/ai-recipes/ のレシピ）を一覧から選び、
// 未保存の文書として開く（中身を見るだけのことが多いので、保存するまでファイルを作らない）。一覧とサムネイルは研究ページ（docs/research/rocks.md）と共有する。

#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"

#include "core/Log.h"
#include "core/PathUtf8.h"
#include "ui/UiStyle.h"

#include <imgui.h>

namespace rock {

// サムネイルは研究ページの画像（4 方向を 2×2 にまとめた横長 2:1）を一辺 512 の正方形に縮めて中央に置いたもの。
// 左上の 1 方向（u 0〜0.5、v 0.25〜0.5）のうち、岩のある中央付近（u 0.1〜0.4、v 0.27〜0.42）を見せる。
constexpr uint32_t kTemplateThumbnailSize = 512;

void Application::OpenTemplateWindow(const std::filesystem::path& directory) {
    m_templateWindow = true;
    m_templateDirectory = directory;
}

void Application::ProcessTemplateWork() {
    // 一覧とサムネイルは、初めて開いたときに 1 回だけ読む（GPU の待機を伴うのでフレームの外で）。
    if (!m_templateWindow || m_templatesLoaded) return;
    m_templatesLoaded = true;
    std::string error;
    m_rockTemplates = io::LoadRockTemplates(io::FindRockTemplateFolders(), error);
    if (!error.empty()) ROCK_LOG_WARN("岩のテンプレート: %s", error.c_str());
    m_templateThumbnails.resize(m_rockTemplates.size());
    for (size_t i = 0; i < m_rockTemplates.size(); ++i) {
        if (m_rockTemplates[i].image.empty()) continue;
        if (!AssetThumbnailCache::BuildImage(m_device, m_rockTemplates[i].image, m_templateThumbnails[i], kTemplateThumbnailSize))
            m_device.DeferRelease(m_templateThumbnails[i]);
    }
}

void Application::DestroyTemplateThumbnails() {
    for (auto& texture : m_templateThumbnails) m_device.DeferRelease(texture);
    m_templateThumbnails.clear();
}

void Application::OpenTemplate(const io::RockTemplate& source) {
    // 開く。今の文書の保存の確認（シーンの切り替え）を通る。読み込めたら未保存の文書にする（ProcessPendingFileWork）。
    m_pendingProjectOpen = source.graph;
    m_pendingTemplateOpen = true;
    m_untitledName = source.name;
    m_untitledDirectory = m_templateDirectory.empty() ? m_workspace.Root() : m_templateDirectory;
    m_templateWindow = false;
}

void Application::DrawTemplateWindow() {
    if (!m_templateWindow) return;
    ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("テンプレートから作成", &m_templateWindow)) {
        ImGui::End();
        return;
    }
    const std::filesystem::path directory = m_templateDirectory.empty() ? m_workspace.Root() : m_templateDirectory;
    if (m_workspace.Root().empty()) {
        ui::HintText("ルートフォルダを開いてから作成してください（「ファイル」→「ルートフォルダを開く…」）。");
        ImGui::End();
        return;
    }
    std::error_code error;
    const auto relative = std::filesystem::relative(directory, m_workspace.Root(), error);
    ui::HintText(("保存先: " + (error || relative.empty() || relative == "." ? std::string("ルート") : ToUtf8Display(relative)) +
                  "（選ぶと未保存のまま開き、保存したときに初めてファイルを作ります。状態 ○ はそれらしく作れる、△ は改善中）").c_str());
    if (!m_templatesLoaded) {
        ImGui::TextDisabled("読み込み中…");
        ImGui::End();
        return;
    }
    if (m_rockTemplates.empty()) {
        ui::HintText("テンプレートが見つかりません（実行ファイルの横の templates/ か、examples/ai-recipes/）。");
        ImGui::End();
        return;
    }

    const float thumbnail = 168.0f;
    const float cell = thumbnail + ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / cell));
    std::string category;
    int column = 0;
    const io::RockTemplate* chosen = nullptr;
    for (size_t i = 0; i < m_rockTemplates.size(); ++i) {
        const auto& entry = m_rockTemplates[i];
        if (entry.category != category) {
            category = entry.category;
            column = 0;
            ImGui::Spacing();
            ImGui::SeparatorText(category.c_str());
        }
        if (column > 0) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(int(i));
        const bool hasImage = i < m_templateThumbnails.size() && m_templateThumbnails[i].IsValid();
        bool clicked = false;
        if (hasImage) {
            clicked = ImGui::ImageButton("##thumbnail", static_cast<ImTextureID>(m_templateThumbnails[i].srv.gpu.ptr),
                                         ImVec2(thumbnail, thumbnail * .5f), ImVec2(.1f, .27f), ImVec2(.4f, .42f));
        } else {
            clicked = ImGui::Button("##thumbnail", ImVec2(thumbnail, thumbnail * .5f));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", entry.name.c_str(), entry.description.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumbnail);
        ImGui::TextWrapped("%s %s", entry.name.c_str(), entry.status.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        ImGui::EndGroup();
        if (clicked) chosen = &entry;
        column = (column + 1) % columns;
        if (column == 0) ImGui::Spacing();
    }
    ImGui::End();
    if (chosen) OpenTemplate(*chosen);
}

}  // namespace rock
