#include "app/Application.h"
#include "ui/UiStyle.h"
#include <algorithm>
#include <cmath>
namespace rock {
void Application::DrawUvPanel() {
    if (ImGui::Begin("UVビュー")) {
        if (ImGui::Button("全体表示")) {
            m_uvZoom = 1;
            m_uvPan = {};
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("ホイール: ズーム / 中ボタンドラッグ: 移動");
        if (m_uvPreviewMesh.cornerUvs.empty()) {
            ui::HintText("UV UnwrapのノードまたはUVを持つ出力を表示してください。");
        } else {
            ImGui::Text("UV: %u x %u / %zu triangles", m_uvPreviewMesh.uvWidth, m_uvPreviewMesh.uvHeight,
                        m_uvPreviewMesh.triangles.size());
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImVec2 size = ImGui::GetContentRegionAvail();
            size.x = std::max(1.0f, size.x);
            size.y = std::max(1.0f, size.y);
            ImGui::InvisibleButton("uvCanvas", size, ImGuiButtonFlags_MouseButtonMiddle);
            const bool hovered = ImGui::IsItemHovered();
            const float side = std::max(1.0f, std::min(size.x, size.y) - 40.0f);
            const ImVec2 center{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f};
            if (hovered && ImGui::GetIO().MouseWheel != 0) {
                const float next =
                    std::clamp(m_uvZoom * std::pow(1.2f, ImGui::GetIO().MouseWheel), 0.1f, 50.0f);
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const float ratio = next / m_uvZoom;
                m_uvPan = {mouse.x - center.x - (mouse.x - center.x - m_uvPan.x) * ratio,
                           mouse.y - center.y - (mouse.y - center.y - m_uvPan.y) * ratio};
                m_uvZoom = next;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                m_uvPan.x += ImGui::GetIO().MouseDelta.x;
                m_uvPan.y += ImGui::GetIO().MouseDelta.y;
            }
            const float scale = side * m_uvZoom;
            const auto point = [&](geometry::Mesh::Uv uv) {
                return ImVec2{center.x + m_uvPan.x + (uv.u - .5f) * scale,
                              center.y + m_uvPan.y + (uv.v - .5f) * scale};
            };
            auto *draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
            draw->AddRectFilled(point({0, 0}), point({1, 1}), IM_COL32(35, 35, 35, 255));
            for (int i = 0; i <= 10; ++i) {
                const float v = i / 10.0f;
                draw->AddLine(point({v, 0}), point({v, 1}), IM_COL32(65, 65, 65, 255));
                draw->AddLine(point({0, v}), point({1, v}), IM_COL32(65, 65, 65, 255));
            }
            for (size_t f = 0; f < m_uvPreviewMesh.cornerUvs.size(); ++f) {
                const auto &uv = m_uvPreviewMesh.cornerUvs[f];
                const uint32_t chart = f < m_uvPreviewMesh.uvCharts.size() ? m_uvPreviewMesh.uvCharts[f] : 0;
                const ImU32 color = ImGui::ColorConvertFloat4ToU32(
                    ImColor::HSV(float((chart * 37) % 255) / 255.0f, 0.45f, 0.95f));
                for (int c = 0; c < 3; ++c)
                    draw->AddLine(point(uv[c]), point(uv[(c + 1) % 3]), color);
            }
            draw->AddRect(point({0, 0}), point({1, 1}), IM_COL32(230, 230, 230, 255));
            draw->PopClipRect();
        }
    }
    ImGui::End();
}
} // namespace rock
