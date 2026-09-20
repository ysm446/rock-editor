// ビューポートパネルと、その上の入力（軌道 / ライトドラッグ / パス編集 / メッシュの選択）、
// 重ねて描くギズモ類。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXCollision.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace rock {

namespace {

// メッシュの軸平行境界ボックス。頂点が無ければ偽。
bool MeshBounds(const renderer::MeshData& data, DirectX::BoundingBox& outBounds) {
    if (data.vertices.empty()) return false;
    DirectX::XMFLOAT3 lo = data.vertices.front().position;
    DirectX::XMFLOAT3 hi = lo;
    for (const auto& vertex : data.vertices) {
        const auto& p = vertex.position;
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
    }
    DirectX::BoundingBox::CreateFromPoints(outBounds, DirectX::XMLoadFloat3(&lo), DirectX::XMLoadFloat3(&hi));
    return true;
}

}  // namespace

// カーソル直下のメッシュ。境界ボックスでふるいにかけてから三角形と交差を取り、最も手前を採る。
// 判定は分割前・変位前の形なので、凹凸のある路面では数 cm ずれるが、ホバー表示には足りる。
void Application::HandleMeshHover(bool itemHovered, const ImVec2& viewportMin, const ImVec2& viewportMax) {
    using namespace DirectX;
    auto& state = m_meshHighlight;
    const auto& meshes = m_renderer.Scene().meshes;
    std::erase_if(state.selected, [&](int index) { return index < 0 || static_cast<size_t>(index) >= meshes.size(); });
    state.hovered = -1;
    const ImGuiIO& io = ImGui::GetIO();
    const auto contains = [](const std::vector<int>& list, int index) {
        return std::find(list.begin(), list.end(), index) != list.end();
    };

    // 空からのドラッグは画面上の矩形で複数選択。頂点が 1 つでも矩形に入れば選ぶ。
    // 形状とアンドゥ履歴は変更しない。
    if (state.boxPending) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.selected = state.boxPrevious;
            state.boxPending = state.boxSelecting = false;
            return;
        }
        state.boxEnd = {std::clamp(io.MousePos.x, viewportMin.x, viewportMax.x),
                        std::clamp(io.MousePos.y, viewportMin.y, viewportMax.y)};
        state.boxSelecting |= ImGui::IsMouseDragging(ImGuiMouseButton_Left, ui::Scaled(3.0f));
        if (state.boxSelecting) {
            state.selected = state.boxAdditive ? state.boxPrevious : std::vector<int>{};
            const ImVec2 lo(std::min(state.boxStart.x, state.boxEnd.x), std::min(state.boxStart.y, state.boxEnd.y));
            const ImVec2 hi(std::max(state.boxStart.x, state.boxEnd.x), std::max(state.boxStart.y, state.boxEnd.y));
            const auto& camera = m_renderer.GetCamera();
            const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
            const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
            for (size_t index = 0; index < meshes.size(); ++index) {
                const auto& mesh = meshes[index];
                if (mesh.materialOnly || contains(state.selected, static_cast<int>(index))) continue;
                for (const auto& vertex : mesh.geometry.vertices) {
                    const auto point = ProjectToViewport(viewProjection, vertex.position, viewportMin, size);
                    if (point.visible && point.screen.x >= lo.x && point.screen.x <= hi.x &&
                        point.screen.y >= lo.y && point.screen.y <= hi.y) {
                        state.selected.push_back(static_cast<int>(index));
                        break;
                    }
                }
            }
            auto* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(viewportMin, viewportMax, true);
            draw->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.4f));
            draw->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_PlotLinesHovered));
            draw->PopClipRect();
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.boxPending = state.boxSelecting = false;
        if (state.boxSelecting) return;  // 矩形の最中はホバーもクリックも見ない
    }

    if (!itemHovered) return;

    XMFLOAT3 origin;
    XMFLOAT3 direction;
    if (ViewportRay(io.MousePos, viewportMin, viewportMax, origin, direction)) {
        const XMVECTOR rayOrigin = XMLoadFloat3(&origin);
        const XMVECTOR rayDirection = XMLoadFloat3(&direction);
        float nearest = FLT_MAX;
        for (size_t index = 0; index < meshes.size(); ++index) {
            const auto& mesh = meshes[index];
            if (mesh.materialOnly) continue;
            BoundingBox bounds;
            float boxDistance = 0.0f;
            if (!MeshBounds(mesh.geometry, bounds) || !bounds.Intersects(rayOrigin, rayDirection, boxDistance) ||
                boxDistance >= nearest) {
                continue;
            }
            const auto& vertices = mesh.geometry.vertices;
            const auto& indices = mesh.geometry.indices;
            for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                const XMVECTOR a = XMLoadFloat3(&vertices[indices[t]].position);
                const XMVECTOR b = XMLoadFloat3(&vertices[indices[t + 1]].position);
                const XMVECTOR c = XMLoadFloat3(&vertices[indices[t + 2]].position);
                float distance = 0.0f;
                if (TriangleTests::Intersects(rayOrigin, rayDirection, a, b, c, distance) && distance < nearest) {
                    nearest = distance;
                    state.hovered = static_cast<int>(index);
                }
            }
        }
    }

    // 押した瞬間に矩形選択の待機へ入る。動かさずに離せばクリック選択（Shift で切り替え）。
    // 空を押せば解除。Esc でも解除。選択は文書を変えない。
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.boxPending = true;
        state.boxSelecting = false;
        state.boxAdditive = io.KeyShift;
        state.boxStart = state.boxEnd = io.MousePos;
        state.boxPrevious = state.selected;
        if (state.hovered < 0) {
            if (!io.KeyShift) state.selected.clear();
        } else if (io.KeyShift) {
            if (contains(state.selected, state.hovered)) std::erase(state.selected, state.hovered);
            else state.selected.push_back(state.hovered);
        } else {
            state.selected = {state.hovered};
            if (static_cast<size_t>(state.hovered) < m_rockMeshReferences.size()) {
                const auto& ref = m_rockMeshReferences[static_cast<size_t>(state.hovered)];
                if (ref.chunk != 0) {
                    m_selectedChunk = ref.chunk;
                    m_selectedGraphNode = ref.source;
                    m_graphSelectionRequest = ref.source;
                }
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) state.selected.clear();
}

bool Application::SelectedMeshFocusTarget(DirectX::XMFLOAT3& target) const {
    using namespace DirectX;
    const auto& meshes = m_renderer.Scene().meshes;
    BoundingBox total;
    bool found = false;
    for (const int selected : m_meshHighlight.selected) {
        if (selected < 0 || static_cast<size_t>(selected) >= meshes.size()) continue;
        BoundingBox bounds;
        if (!MeshBounds(meshes[static_cast<size_t>(selected)].geometry, bounds)) continue;
        if (!found) { total = bounds; found = true; }
        else BoundingBox::CreateMerged(total, total, bounds);
    }
    if (!found) return false;
    target = total.Center;
    return true;
}

// 3 桁ごとに区切る。**桁数の多い数はそのままだと読めない。**
std::string GroupDigits(uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return digits;
}

// ビューポートに重ねる操作。表示モードの切り替えと、重ねる情報の切り替え。
//
// トップメニューではなくビューポートの中に置く。見ている場所から目を離さずに
// 切り替えられ、いまどの表示なのかも常に見える。
void Application::DrawViewportOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const float margin = ui::Scaled(10.0f);
    ImGui::SetCursorScreenPos(ImVec2(viewportMin.x + margin, viewportMin.y + margin));

    renderer::DebugView& current = m_renderer.Debug();
    const char* label = kDebugViewLabels[static_cast<size_t>(current)];

    // 既定以外の表示は見落としやすいので、ボタンの文字を強調する。
    const bool highlighted = (current != renderer::DebugView::Shaded);
    if (highlighted) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
    }
    if (ImGui::Button(label)) {
        ImGui::OpenPopup("##viewportViewMenu");
    }
    if (highlighted) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ビューポートに何を表示するか");
    }

    if (ImGui::BeginPopup("##viewportViewMenu")) {
        for (int i = 0; i < IM_ARRAYSIZE(kDebugViewLabels); ++i) {
            const auto view = static_cast<renderer::DebugView>(i);
            if (ImGui::Selectable(kDebugViewLabels[i], current == view)) {
                current = view;
            }
        }
        ImGui::EndPopup();
    }

    // --- 重ねる情報の切り替え ------------------------------------------------
    // FPS / 統計 / グリッド。どれもビューポートに重ねて出すものなので、
    // トップメニューではなくここに置く。切り替えたその場で設定に覚える。
    ImGui::SameLine();
    if (ImGui::Button("表示")) {
        ImGui::OpenPopup("##viewportDisplayMenu");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ビューポートに重ねる情報");
    }
    if (ImGui::BeginPopup("##viewportDisplayMenu")) {
        io::DisplaySettings& settings = m_settings.Display();
        bool changed = false;
        changed |= ImGui::MenuItem("FPS", nullptr, &settings.showFps);
        changed |= ImGui::MenuItem("統計", nullptr, &settings.showStats);
        changed |= ImGui::MenuItem("グリッド（50 m × 50 m / 1 m間隔）", nullptr, &settings.showReferenceGrid);
        changed |= ImGui::MenuItem("ワイヤーフレーム（分割前）", nullptr, &settings.showRoadGrid);
        changed |= ImGui::MenuItem("UVチェッカー", nullptr, &settings.showUvChecker);
        changed |= ImGui::MenuItem("ワイヤーフレーム（分割後）", nullptr, &settings.showWireframe);
        if (changed) {
            m_settings.Save();
        }
        ImGui::EndPopup();
    }

    // --- FPS と描画の量 ------------------------------------------------------
    // **右上へ置く。** 左上は表示モードの切り替えとライトの数値で埋まっている。
    // ボタンではなく描き込みにする。押すものではないので、枠を持たせない。
    const io::DisplaySettings& display = m_settings.Display();
    if (!display.showFps && !display.showStats) {
        return;
    }

    // 行を組み立ててから 1 つの下地にまとめて描く。**枠を 2 つ並べない。**
    // FPS と統計で別々の箱にすると、片方だけ出したときに位置が揃わない。
    std::vector<std::string> lines;
    if (display.showFps) {
        // 1 桁台では小数まで出す。整数だけだと 0.8fps が「0 FPS」になり、
        // 止まっているのか極端に遅いのかが読めない。
        const float framerate = ImGui::GetIO().Framerate;
        char text[32] = {};
        std::snprintf(text, sizeof(text), (framerate < 10.0f) ? "%.1f FPS" : "%.0f FPS",
                      framerate);
        lines.emplace_back(text);
    }
    if (display.showStats) {
        const renderer::RenderStats& stats = m_renderer.Stats();
        char text[96] = {};
        std::snprintf(text, sizeof(text), "ドローコール %u", stats.drawCalls);
        lines.emplace_back(text);
        std::snprintf(text, sizeof(text), "頂点 %s", GroupDigits(stats.vertices).c_str());
        lines.emplace_back(text);
        // テセレーション中は、三角形はドメインシェーダが決めるので CPU では分からない。
        // **数えられないものを数えたふりをしない。** 投入したパッチ数と上限を出す。
        if (stats.tessellation) {
            std::snprintf(text, sizeof(text), "パッチ %s (x%.0f まで)",
                          GroupDigits(stats.patches).c_str(), stats.tessellationFactor);
        } else {
            std::snprintf(text, sizeof(text), "三角形 %s", GroupDigits(stats.triangles).c_str());
        }
        lines.emplace_back(text);
        // VRAM はプロセス全体の使用量とバジェット。合成の解像度を上げたときに
        // どれだけ余裕が残っているかを、その場で見えるようにする。
        const rhi::Device::VideoMemory vram = m_device.QueryVideoMemory();
        constexpr double kMegaBytes = 1024.0 * 1024.0;
        std::snprintf(text, sizeof(text), "VRAM %.0f / %.0f MB",
                      static_cast<double>(vram.usage) / kMegaBytes,
                      static_cast<double>(vram.budget) / kMegaBytes);
        lines.emplace_back(text);
    }

    const ImVec2 padding(ui::Scaled(8.0f), ui::Scaled(4.0f));
    const float lineHeight = ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.y * 0.5f;
    float widest = 0.0f;
    for (const std::string& line : lines) {
        widest = std::max(widest, ImGui::CalcTextSize(line.c_str()).x);
    }
    const float height = lineHeight * static_cast<float>(lines.size()) +
                         spacing * static_cast<float>(lines.size() - 1);

    const ImVec2 boxMax(viewportMax.x - margin,
                        viewportMin.y + margin + height + padding.y * 2.0f);
    const ImVec2 boxMin(boxMax.x - widest - padding.x * 2.0f, viewportMin.y + margin);

    // 明るい素材の上でも読めるように、暗い下地を敷く。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(8, 10, 12, 190), ui::Scaled(4.0f));

    float y = boxMin.y + padding.y;
    for (const std::string& line : lines) {
        // **右へ揃える。** 桁数が変わるたびに数字の頭が動くと、目で追えない。
        const float width = ImGui::CalcTextSize(line.c_str()).x;
        drawList->AddText(ImVec2(boxMax.x - padding.x - width, y),
                          IM_COL32(235, 235, 235, 255), line.c_str());
        y += lineHeight + spacing;
    }
}

// L + 左ドラッグでライトの向きを変える。
//
// 修飾キー（Ctrl / Shift / Alt）は付けない。Alt は軌道、Ctrl は数値の直接入力に
// 使っているので、それらと重ならないようにする。
bool Application::HandleLightDrag(renderer::LightSettings& light, LightInteraction& interaction, bool itemActive) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool shortcut =
        ImGui::IsKeyDown(ImGuiKey_L) && !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
    if (!shortcut || !itemActive || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        interaction.dragging = false;
        return false;
    }

    interaction.dragging = true;
    interaction.gizmoUntil = ImGui::GetTime() + kLightGizmoFadeSeconds;

    const float step = DegreesToRadians(kLightDegreesPerPixel);
    // 方位角は一周させる。仰角は UI のスライダーと同じ範囲に収める。
    light.azimuth = WrapAngle(light.azimuth + io.MouseDelta.x * step);
    // 真下からの光も見たいので、下は -89 度まで許す。
    light.elevation = std::clamp(light.elevation - io.MouseDelta.y * step,
                                 DegreesToRadians(-89.0f), DegreesToRadians(89.0f));
    return true;
}

// F で選択中のモデル / メッシュへ注視点を移す。
// 選択が無ければ原点へ戻し、A は全体が収まる距離まで引く。
// DCC の「選択をフレーム / 全体をフレーム」に倣った割り当て。
//
// 修飾キーは付けない（Ctrl は数値の直接入力、Alt は軌道に使っている）。
// カーソルがビューポートの上にあるときだけ効かせ、
// **テキスト入力中は無視する**。レイヤー名を打っている最中に視点が飛ぶのを防ぐ。
void Application::HandleCameraInput(renderer::PreviewRenderer& preview, bool itemActive, bool itemHovered,
                                    bool includeReferenceGrid) {
    const ImGuiIO& io = ImGui::GetIO();
    renderer::Camera& camera = preview.GetCamera();
    if (itemActive && io.KeyAlt) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            camera.Orbit(io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            camera.Pan(io.MouseDelta.x, io.MouseDelta.y);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            // 右へ引くと寄る。縦は見ない（斜めに引いたときに暴れるため）。
            camera.Dolly(io.MouseDelta.x);
        }
    }

    if (itemHovered && io.MouseWheel != 0.0f) {
        camera.Zoom(io.MouseWheel);
    }

    if (!itemHovered || io.WantTextInput || io.KeyCtrl || io.KeyShift || io.KeyAlt) {
        return;
    }

    // プレビューのメッシュはどれも原点中心（モデル行列は単位行列）。
    constexpr DirectX::XMFLOAT3 kMeshCenter{0.0f, 0.0f, 0.0f};

    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        // 選択中のモデル、無ければ選択中のメッシュ、それも無ければ原点。
        DirectX::XMFLOAT3 target = kMeshCenter;
        if (&preview == &m_renderer && !SelectedModelInstanceFocusTarget(target))
            SelectedMeshFocusTarget(target);
        camera.Focus(target);
    } else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        camera.Frame(kMeshCenter, includeReferenceGrid
            ? std::max(preview.BoundingRadius(), renderer::PreviewRenderer::kReferenceGridRadius)
            : preview.BoundingRadius());
    }
}

// ライトの向きを示すギズモ。地面のリング、水平方向、仰角の弧、光が来る向きの矢印。
//
// 色はテーマから引かない。座標軸ギズモと同じく「意味を持つ色」として固定する。
void Application::DrawLightGizmo(const renderer::LightSettings& light, const LightInteraction& interaction,
                                const renderer::Camera& camera, const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const double now = ImGui::GetTime();
    if (!interaction.dragging && now >= interaction.gizmoUntil) {
        return;
    }
    const float fade =
        interaction.dragging
            ? 1.0f
            : static_cast<float>(std::clamp((interaction.gizmoUntil - now) / kLightGizmoFadeSeconds,
                                            0.0, 1.0));
    if (fade <= 0.001f) {
        return;
    }

    using namespace DirectX;
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }

    const XMFLOAT3 direction = light.Direction();
    // **カメラの注視点に、画面へ収まる大きさで置く。** 原点固定・実寸固定だと、
    // パンやズームで注視点を移した先で見えなくなったり、画面からはみ出したりする。
    // 半径は注視点までの距離と縦画角から決め、リングと矢印が縦の視野の中に収まる比にする。
    const XMFLOAT3 origin = camera.Target();
    const XMFLOAT3 eye = camera.Position();
    const float distance = std::sqrt((eye.x - origin.x) * (eye.x - origin.x) +
                                     (eye.y - origin.y) * (eye.y - origin.y) +
                                     (eye.z - origin.z) * (eye.z - origin.z));
    const float gizmoRadius = std::max(distance * std::tan(camera.FovY() * 0.5f) * 0.45f, 1e-3f);
    const XMFLOAT3 horizontal{std::sin(light.azimuth), 0.0f, std::cos(light.azimuth)};

    const auto color = [fade](int r, int g, int b, int a) {
        return IM_COL32(r, g, b, static_cast<int>(static_cast<float>(a) * fade));
    };
    const auto offset = [](const XMFLOAT3& base, const XMFLOAT3& dir, float amount) {
        return XMFLOAT3{base.x + dir.x * amount, base.y + dir.y * amount,
                        base.z + dir.z * amount};
    };
    const auto project = [&](const XMFLOAT3& world) {
        return ProjectToViewport(viewProjection, world, viewportMin, size);
    };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);

    const auto drawWorldLine = [&](const XMFLOAT3& a, const XMFLOAT3& b, ImU32 lineColor,
                                   float thickness) {
        const ProjectedPoint pa = project(a);
        const ProjectedPoint pb = project(b);
        if (pa.visible && pb.visible) {
            drawList->AddLine(pa.screen, pb.screen, lineColor, thickness);
        }
    };

    // 地面のリング。方位角の目安になる。
    constexpr int kRingSegments = 72;
    ProjectedPoint previous;
    for (int i = 0; i <= kRingSegments; ++i) {
        const float t = (static_cast<float>(i) / kRingSegments) * 2.0f * 3.14159265f;
        const ProjectedPoint current = project(XMFLOAT3{
            origin.x + std::sin(t) * gizmoRadius, origin.y, origin.z + std::cos(t) * gizmoRadius});
        if (i > 0 && previous.visible && current.visible) {
            drawList->AddLine(previous.screen, current.screen, color(150, 160, 175, 130), 1.6f);
        }
        previous = current;
    }

    // 水平方向への投影と、そこから仰角ぶんの弧。
    drawWorldLine(origin, offset(origin, horizontal, gizmoRadius), color(150, 160, 175, 170), 1.8f);

    constexpr int kArcSegments = 32;
    ProjectedPoint previousArc;
    for (int i = 0; i <= kArcSegments; ++i) {
        const float angle = light.elevation * (static_cast<float>(i) / kArcSegments);
        const float ring = std::cos(angle) * gizmoRadius;
        const ProjectedPoint current = project(XMFLOAT3{origin.x + horizontal.x * ring,
                                                        origin.y + std::sin(angle) * gizmoRadius,
                                                        origin.z + horizontal.z * ring});
        if (i > 0 && previousArc.visible && current.visible) {
            drawList->AddLine(previousArc.screen, current.screen, color(255, 206, 112, 150), 1.6f);
        }
        previousArc = current;
    }

    // 光が来る向きの矢印。ライトの位置から原点へ向ける。
    const ProjectedPoint arrowStart = project(offset(origin, direction, gizmoRadius));
    const ProjectedPoint arrowEnd = project(offset(origin, direction, gizmoRadius * 0.22f));
    if (arrowStart.visible && arrowEnd.visible) {
        const ImU32 lightColor = color(255, 188, 76, 245);
        ImVec2 screenDir(arrowEnd.screen.x - arrowStart.screen.x,
                         arrowEnd.screen.y - arrowStart.screen.y);
        const float length = std::sqrt(screenDir.x * screenDir.x + screenDir.y * screenDir.y);
        if (length > 0.001f) {
            screenDir.x /= length;
            screenDir.y /= length;
            const ImVec2 side(-screenDir.y, screenDir.x);
            const float head = ui::Scaled(14.0f);
            const float halfWidth = ui::Scaled(6.0f);
            const ImVec2 base(arrowEnd.screen.x - screenDir.x * head,
                              arrowEnd.screen.y - screenDir.y * head);
            drawList->AddLine(arrowStart.screen, base, lightColor, ui::Scaled(3.5f));
            drawList->AddTriangleFilled(
                arrowEnd.screen, ImVec2(base.x + side.x * halfWidth, base.y + side.y * halfWidth),
                ImVec2(base.x - side.x * halfWidth, base.y - side.y * halfWidth), lightColor);
        }
    }

    if (const ProjectedPoint center = project(origin); center.visible) {
        drawList->AddCircle(center.screen, ui::Scaled(5.0f), color(200, 210, 220, 200), 20, 1.6f);
    }

    // いまの値。掴んだまま数字を確かめられるようにする。
    char text[64] = {};
    std::snprintf(text, sizeof(text), "方位角 %.0f 度   仰角 %.0f 度",
                  RadiansToDegrees(light.azimuth), RadiansToDegrees(light.elevation));
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 padding(ui::Scaled(8.0f), ui::Scaled(5.0f));
    // 左上には表示モードのボタンがあるので、その下へ置く。
    const ImVec2 textMin(viewportMin.x + ui::Scaled(10.0f),
                         viewportMin.y + ui::Scaled(10.0f) + ImGui::GetFrameHeight() +
                             ui::Scaled(6.0f));
    const ImVec2 textMax(textMin.x + textSize.x + padding.x * 2.0f,
                         textMin.y + textSize.y + padding.y * 2.0f);
    drawList->AddRectFilled(textMin, textMax, color(8, 10, 12, 190), ui::Scaled(4.0f));
    drawList->AddText(ImVec2(textMin.x + padding.x, textMin.y + padding.y),
                      color(235, 235, 235, 255), text);

    drawList->PopClipRect();
}

void Application::DrawViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // ホイールでウィンドウがスクロールしないようにする（ズームに使うため）。
    const bool open = ImGui::Begin("ビューポート", nullptr,
                                   ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (open) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        // 8 の倍数に丸め、ドラッグ中の作り直しを減らす。
        const auto snap = [](float value) {
            const int clamped = std::clamp(static_cast<int>(value), 64, 4096);
            return static_cast<uint32_t>((clamped / 8) * 8);
        };
        m_requestedViewportWidth = snap(available.x);
        m_requestedViewportHeight = snap(available.y);

        if (m_renderer.HasOutput()) {
            // テクスチャの実サイズではなくコンテンツ領域に合わせて描く。
            // 実サイズで描くとパネルからはみ出し、スクロールバーの出入りで
            // 要求サイズが振動してしまう。作り直しは 1 フレーム遅れる。
            const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(m_renderer.OutputHandle().ptr), available);

            // ImGui::Image は入力を消費しないため、そのままだと画像上のドラッグが
            // 「ウィンドウの余白のドラッグ」と解釈されてパネルごと動いてしまう。
            // 同じ矩形に不可視ボタンを重ねてドラッグを受け止める。
            ImGui::SetCursorScreenPos(imageOrigin);
            // ビューポート内に重ねるボタン（表示モード）へ入力を譲る。
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##viewportInput", available,
                                   ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonMiddle |
                                       ImGuiButtonFlags_MouseButtonRight);
            // アセットの帯のモデル（.rockmodel / .fbx）を落とすと、その位置へ置く。
            ModelDropTarget(imageOrigin, ImVec2(imageOrigin.x + available.x, imageOrigin.y + available.y));

            const ImGuiIO& io = ImGui::GetIO();
            renderer::Camera& camera = m_renderer.GetCamera();
            const bool itemActive = ImGui::IsItemActive();
            const bool itemHovered = ImGui::IsItemHovered();

            // L + 左ドラッグはライトの向き。軌道より先に見る。
            const bool lightDragging = HandleLightDrag(m_renderer.Light(), m_viewportLightInteraction, itemActive);

            const ImVec2 imageMax(imageOrigin.x + available.x, imageOrigin.y + available.y);

            // 置いたモデル → メッシュの順にカーソル直下を強調し、クリックで選ぶ。
            // モデルを掴んだ入力はメッシュの選択へ渡さない。
            const bool modelInput = !lightDragging && !io.KeyAlt &&
                                    HandleModelInstanceInput(itemActive, itemHovered, imageOrigin, imageMax);
            if (lightDragging || io.KeyAlt) {
                m_hoveredModelNode = 0;
                m_modelInstanceDrag = {};
            }
            if (m_renderer.HasMeshScene() && !lightDragging && !io.KeyAlt && !modelInput) {
                HandleMeshHover(itemHovered, imageOrigin, imageMax);
            } else {
                m_meshHighlight.hovered = -1;
                m_meshHighlight.boxPending = m_meshHighlight.boxSelecting = false;
            }
            m_renderer.SetMeshHighlight(m_meshHighlight.hovered, m_meshHighlight.selected);

            // 視点操作は Alt を押している間だけ受ける（Maya と同じ割り当て）。
            //
            // Alt なしのドラッグはメッシュの矩形選択。
            // Alt を押している間はライトも無効になる（HandleLightDrag が !io.KeyAlt を見る）ので、
            // ここで競合は起きない。
            HandleCameraInput(m_renderer, itemActive, itemHovered, m_settings.Display().showReferenceGrid);

            DrawAxisGizmo(camera, imageOrigin, imageMax);
            DrawLightGizmo(m_renderer.Light(), m_viewportLightInteraction, camera, imageOrigin, imageMax);
            DrawModelInstanceOverlay(imageOrigin, imageMax);

            // ビューポートに重ねる操作。左上に表示モードの切り替え、右上に FPS。
            DrawViewportOverlay(imageOrigin, imageMax);

        }
    }
    ImGui::End();
}

}  // namespace rock
