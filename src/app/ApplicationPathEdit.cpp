#include "renderer/AxisProjection.h"
// ビューポートでのパス（Path ノード）の編集。
//
// 操作は「クリックは選ぶ、Ctrl を押している間だけ伸ばす、ドラッグして重ねれば繋がる」
// の 3 つを軸にする。設計の経緯は docs/design/node-graph.md の「パス」。
//
//   点をクリック            選択する（伸ばす起点になる）
//   エッジをクリック        鎖（分岐から分岐まで）を選ぶ。向きと曲線の種類はここで変える。
//                           エッジ 1 本の選択は無い（曲線も向きも鎖の性質で、1 本だけ変えると
//                           鎖の中で食い違う。1 本だけ消す / 切り離すは右クリックのメニュー）
//   空をクリック            選択を外す
//   Ctrl + 空をクリック     選択した点から新しい点へ線を伸ばす（選択が無ければ新しい線の始点）
//   Ctrl + 点をクリック     選択した点とその点を繋ぐ
//   Ctrl + エッジをクリック そこに点を挿入して選択（そのままドラッグできる。点を選んで
//                           いればその点と繋ぐ）
//   点をドラッグ            作業面（選択した点の高さの水平面 / 道路面）に沿って動かす。
//                           他の点や線に重ねると吸着して結合
//   右クリック              点 / エッジ / 空のメニュー（分離、削除、反転、挿入…）
//   Delete                  選択した点（またはエッジ）を消す。鎖の途中の点なら線は繋ぎ直す
//   R                       選択した点に付くエッジ（または選択した鎖）の向きを反転
//   Esc                     選択を外す
//   Shift                   吸着しない
//
// **ふつうのクリックはデータを変えない**（選ぶだけ）。増やす / 繋ぐは Ctrl、動かすは
// ドラッグ、消す / 分ける / 反転はキーとメニュー。線のそばに点を置きたかったのに
// 挿入されてしまう、という誤操作を無くすため。
//
// **仮のエッジ（選択した点からカーソルへ）は Ctrl を押している間だけ出す。**
// 常に出ていると「まだ終わっていない」と急かされる感じになる。伸ばす意思があるときにだけ
// 現れ、Ctrl を離しても状態は変わらない（選択は残る）。
// 操作の案内はビューポートの下端に出す（プロパティに書くと目を離さないと読めない）。
//
// Path は X/Y/Z を実寸で保持し、クリックは選択点の高さ（未選択なら Y=0）の平面へ投影する。
// 面上のパス（Surface に道路を繋いだ Path）は道路面へ投影し、横位置 / 実距離で持つ。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/Log.h"
#include "graph/Path.h"
#include "graph/RoadProfile.h"
#include "graph/Road.h"
#include "ui/UiStyle.h"

#include <cfloat>
#include <imgui.h>

#include <DirectXMath.h>

#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tg {
namespace {

using namespace DirectX;

// 当たり判定の広さ（96 DPI 基準のピクセル）。
constexpr float kPointHitRadius = 9.0f;
constexpr float kEdgeHitRadius = 6.0f;
constexpr float kSnapRadius = 12.0f;
// 分離 / 切り離しで点を離す距離（画面上）。
constexpr float kDetachPixels = 14.0f;
// これより動いたらクリックではなくドラッグ。
constexpr float kDragThreshold = 3.0f;
// 移動ギズモ。軸の長さ、当たり判定の幅、中央の平面ハンドルの半径（画面上）。
constexpr float kGizmoLength = 64.0f;
constexpr float kGizmoHitRadius = 8.0f;
constexpr float kGizmoCenterRadius = 7.0f;

float Distance(const ImVec2& a, const ImVec2& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// 点 p から線分 ab までの距離と、最寄りの位置 t（0〜1）。
float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b, float& outT) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float lengthSq = abx * abx + aby * aby;
    outT = (lengthSq > 1e-6f) ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / lengthSq,
                                           0.0f, 1.0f)
                              : 0.0f;
    const ImVec2 closest(a.x + abx * outT, a.y + aby * outT);
    return Distance(p, closest);
}

// 画面へ投影したパス。当たり判定と描画の両方がこれを見る。
struct PathScreenPoint {
    graph::PathElementId id = 0;
    ImVec2 screen{};
    bool visible = false;
};

struct PathScreenEdge {
    graph::PathElementId id = 0;
    graph::PathElementId from = 0;
    graph::PathElementId to = 0;
    // 面上のパスが道路面に沿うように細かく割った折れ線。t は from 側が 0。
    std::vector<ImVec2> polyline;
    std::vector<float> t;
    std::vector<bool> visible;
};

// 鎖の曲線（曲線の鎖だけ）。ガイドの折れ線とは別に、本線として描く。
struct PathScreenCurve {
    std::vector<ImVec2> polyline;
    std::vector<bool> visible;
};

struct PathScreenCache {
    std::vector<PathScreenPoint> points;
    std::vector<PathScreenEdge> edges;
    std::vector<graph::PathStrand> strands;
    std::vector<PathScreenCurve> curves;  // strands と同じ並び。直線の鎖は空

    const PathScreenPoint* Find(graph::PathElementId id) const {
        for (const PathScreenPoint& point : points) {
            if (point.id == id) {
                return &point;
            }
        }
        return nullptr;
    }
};

template <typename WorldFn>
PathScreenCache BuildPathScreenCache(const graph::PathSettings& path, const XMMATRIX& viewProjection,
                                     const ImVec2& viewportMin, const ImVec2& size,
                                     const WorldFn& worldOf) {
    PathScreenCache cache;
    cache.points.reserve(path.points.size());
    for (const graph::PathPoint& point : path.points) {
        const ProjectedPoint projected = ProjectToViewport(
            viewProjection, worldOf(point.x, point.z, point.y), viewportMin,
            size);
        cache.points.push_back({point.id, projected.screen, projected.visible});
    }
    // 鎖を先に導出しておく（エッジがガイドか結果かで描き方が変わる）。
    cache.strands = graph::BuildPathStrands(path);
    cache.edges.reserve(path.edges.size());
    for (const graph::PathEdge& edge : path.edges) {
        const graph::PathPoint* a = path.FindPoint(edge.from);
        const graph::PathPoint* b = path.FindPoint(edge.to);
        const PathScreenPoint* sa = cache.Find(edge.from);
        const PathScreenPoint* sb = cache.Find(edge.to);
        if (a == nullptr || b == nullptr || sa == nullptr || sb == nullptr) {
            continue;
        }
        PathScreenEdge screenEdge;
        screenEdge.id = edge.id;
        screenEdge.from = edge.from;
        screenEdge.to = edge.to;
        // **曲線の鎖のエッジはガイド（制御の骨組み）なので、面に沿わせず点と点を
        // 3D の直線で結ぶ。** 結果である曲線のほうが面に沿う。役割の違いが一目で分かる。
        // 直線の鎖のエッジはそれ自体が結果なので面に沿わせる（面上のパスで効く）。
        const graph::PathStrand* strand = graph::FindStrandOfEdge(cache.strands, edge.id);
        const bool guide =
            strand != nullptr &&
            graph::StrandCurve(path, *strand, nullptr, nullptr) != graph::PathCurve::Line;
        // 制御点列（from、to）。t は道のりの割合（点の挿入がこれを使う）。
        const std::vector<graph::PathPoint> control = graph::PathEdgeControlPoints(path, edge);
        if (control.size() < 2) {
            continue;
        }
        std::vector<float> cumulative(control.size(), 0.0f);
        for (size_t c = 1; c < control.size(); ++c) {
            const float du = control[c].x - control[c - 1].x;
            const float dv = control[c].z - control[c - 1].z;
            cumulative[c] = cumulative[c - 1] + std::sqrt(du * du + dv * dv);
        }
        const float total = std::max(cumulative.back(), 1e-9f);
        for (size_t c = 0; c + 1 < control.size(); ++c) {
            const graph::PathPoint& from = control[c];
            const graph::PathPoint& to = control[c + 1];
            const ProjectedPoint pa = ProjectToViewport(
                viewProjection, worldOf(from.x, from.z, from.y), viewportMin,
                size);
            const ProjectedPoint pb = ProjectToViewport(
                viewProjection, worldOf(to.x, to.z, to.y), viewportMin, size);
            // 画面上の長さで割る数を決める。長い線ほど細かく割って面に沿わせる。
            // ガイドは 3D の直線で、画面上でも直線になるので両端だけでよい。
            const float length =
                (pa.visible && pb.visible) ? Distance(pa.screen, pb.screen) : 400.0f;
            const int segments =
                guide ? 1 : std::clamp(static_cast<int>(length / ui::Scaled(14.0f)), 1, 32);
            // 前の区間の終点と重ねない。
            for (int i = (c == 0) ? 0 : 1; i <= segments; ++i) {
                const float s = static_cast<float>(i) / static_cast<float>(segments);
                const float u = from.x + (to.x - from.x) * s;
                const float v = from.z + (to.z - from.z) * s;
                const float offset =
                    from.y + (to.y - from.y) * s;
                const ProjectedPoint projected =
                    ProjectToViewport(viewProjection, worldOf(u, v, offset), viewportMin, size);
                screenEdge.polyline.push_back(projected.screen);
                screenEdge.t.push_back((cumulative[c] + (cumulative[c + 1] - cumulative[c]) * s) /
                                       total);
                screenEdge.visible.push_back(projected.visible);
            }
        }
        cache.edges.push_back(std::move(screenEdge));
    }
    // 曲線の鎖。制御点の区間ごとに割り、各標本を実寸の高さで描く。
    cache.curves.resize(cache.strands.size());
    for (size_t i = 0; i < cache.strands.size(); ++i) {
        if (graph::StrandCurve(path, cache.strands[i], nullptr, nullptr) == graph::PathCurve::Line) {
            continue;
        }
        constexpr int kSamplesPerSpan = 12;
        for (const graph::PathCurveSample& sample :
             graph::SamplePathStrand(path, cache.strands[i], kSamplesPerSpan)) {
            const ProjectedPoint projected = ProjectToViewport(
                viewProjection, worldOf(sample.x, sample.z, sample.y),
                viewportMin, size);
            cache.curves[i].polyline.push_back(projected.screen);
            cache.curves[i].visible.push_back(projected.visible);
        }
    }
    return cache;
}

// 最寄りの点（半径内）。exclude は除外する点。
graph::PathElementId NearestPoint(const PathScreenCache& cache, const ImVec2& mouse, float radius,
                                  graph::PathElementId exclude) {
    graph::PathElementId best = 0;
    float bestDistance = radius;
    for (const PathScreenPoint& point : cache.points) {
        if (!point.visible || point.id == exclude) {
            continue;
        }
        const float distance = Distance(mouse, point.screen);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = point.id;
        }
    }
    return best;
}

// 最寄りのエッジ（半径内）と、その上の位置 t。excludePoint に付くエッジは除外する。
graph::PathElementId NearestEdge(const PathScreenCache& cache, const ImVec2& mouse, float radius,
                                 graph::PathElementId excludePoint, float& outT) {
    graph::PathElementId best = 0;
    float bestDistance = radius;
    outT = 0.0f;
    for (const PathScreenEdge& edge : cache.edges) {
        if (edge.from == excludePoint || edge.to == excludePoint) {
            continue;
        }
        for (size_t i = 0; i + 1 < edge.polyline.size(); ++i) {
            if (!edge.visible[i] || !edge.visible[i + 1]) {
                continue;
            }
            float t = 0.0f;
            const float distance =
                DistanceToSegment(mouse, edge.polyline[i], edge.polyline[i + 1], t);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = edge.id;
                outT = edge.t[i] + (edge.t[i + 1] - edge.t[i]) * t;
            }
        }
    }
    return best;
}

}  // namespace

// 移動ギズモの画面上の形。重心と、X / Z / Y の軸の先端。
// 軸は重心の高さから世界軸の向きに出す（傾いた軸だと向きが読めない）。
struct PathGizmoScreen {
    bool valid = false;
    ImVec2 center{};
    ImVec2 tip[3]{};
    bool axisValid[3]{};
    int axisCount = 3;
    // 軸に沿って画面上を 1px 動いたときの実寸（m）の変化量。
    float uvPerPixel[3]{};
    // 軸の画面上の単位ベクトル。
    ImVec2 direction[3]{};
};

// 選択で動く点（点の集合、または鎖の両端と内側）。
std::vector<graph::PathElementId> PathMovablePoints(const graph::PathSettings& path,
                                                    const std::vector<graph::PathElementId>& selected,
                                                    const std::vector<graph::PathElementId>& selectedEdges,
                                                    const std::vector<graph::PathElementId>& interior) {
    std::vector<graph::PathElementId> ids = selected;
    const auto add = [&ids](graph::PathElementId id) {
        if (id != 0 && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    };
    for (const graph::PathElementId edgeId : selectedEdges) {
        if (const graph::PathEdge* edge = path.FindEdge(edgeId)) {
            add(edge->from);
            add(edge->to);
        }
    }
    for (const graph::PathElementId id : interior) {
        add(id);
    }
    return ids;
}

template <typename WorldFn>
PathGizmoScreen BuildPathGizmo(const graph::PathSettings& path,
                               const std::vector<graph::PathElementId>& movable,
                               const XMMATRIX& viewProjection, const ImVec2& viewportMin,
                               const ImVec2& size, const WorldFn& worldOf, const graph::RoadGeometry* surfaceRoad) {
    PathGizmoScreen gizmo;
    if (path.surfaceSpace && !surfaceRoad) return gizmo;
    float u = 0.0f;
    float v = 0.0f;
    if (movable.empty() || !graph::PathPointsCentroid(path, movable, u, v)) {
        return gizmo;
    }
    float height = 0.0f;
    size_t count = 0;
    for (const auto id : movable) {
        if (const auto* point = path.FindPoint(id)) { height += point->y; ++count; }
    }
    if (count > 0) height /= static_cast<float>(count);
    const XMFLOAT3 center = worldOf(u, v, height);
    const ProjectedPoint projectedCenter =
        ProjectToViewport(viewProjection, center, viewportMin, size);
    if (!projectedCenter.visible) {
        return gizmo;
    }
    gizmo.center = projectedCenter.screen;
    XMFLOAT3 surfaceAxes[3]{};
    if (path.surfaceSpace) {
        gizmo.axisCount = 2;
        // 道路座標を1m動かしたときの世界変位。法線方向の高さは固定する。
        constexpr float Step = 0.01f;
        const float v0 = std::max(0.0f, v - Step);
        const float v1 = std::min(surfaceRoad->rowDistances.back(), v + Step);
        if (v1 <= v0) return gizmo;
        const auto derivative = [](const XMFLOAT3& a, const XMFLOAT3& b, float distance) {
            return XMFLOAT3{(b.x - a.x) / distance, (b.y - a.y) / distance, (b.z - a.z) / distance};
        };
        surfaceAxes[0] = derivative(worldOf(u - Step, v, height), worldOf(u + Step, v, height), 2.0f * Step);
        surfaceAxes[1] = derivative(worldOf(u, v0, height), worldOf(u, v1, height), v1 - v0);
    }
    const auto projectedAxes = renderer::ProjectMoveAxes(
        viewProjection, center, size.x, size.y, ui::Scaled(kGizmoLength), gizmo.axisCount,
        path.surfaceSpace ? surfaceAxes : nullptr);
    for (int axis = 0; axis < gizmo.axisCount; ++axis) {
        const auto delta = projectedAxes.delta[axis];
        const float length = std::hypot(delta.x, delta.y);
        // ほぼ視線を向いた軸は、矢印や中央ハンドルに潰れるので選択対象からも外す。
        if (length < ui::Scaled(kGizmoCenterRadius * 2.0f)) continue;
        gizmo.axisValid[axis] = true;
        gizmo.direction[axis] = ImVec2(delta.x / length, delta.y / length);
        gizmo.tip[axis] = ImVec2(gizmo.center.x + delta.x, gizmo.center.y + delta.y);
        gizmo.uvPerPixel[axis] = 1.0f / projectedAxes.pixelsPerMeter[axis];
    }
    gizmo.valid = true;
    return gizmo;
}

// ギズモのどこにカーソルがあるか。0 = X、1 = Z、2 = Y、3 = 平面（中央）、-1 = 無し。
int PathGizmoHit(const PathGizmoScreen& gizmo, const ImVec2& mouse) {
    if (!gizmo.valid) {
        return -1;
    }
    if (Distance(mouse, gizmo.center) <= ui::Scaled(kGizmoCenterRadius + 2.0f)) {
        return 3;
    }
    for (int axis = 0; axis < gizmo.axisCount; ++axis) {
        if (!gizmo.axisValid[axis]) continue;
        float t = 0.0f;
        if (DistanceToSegment(mouse, gizmo.center, gizmo.tip[axis], t) <= ui::Scaled(kGizmoHitRadius)) {
            return axis;
        }
    }
    return -1;
}

// --- 道路線形（縦断 / バンク）の編集 ----------------------------------------------
//
// 制御点の編集とは別のモード。線形の中心線（縦断反映後）を画面へ落とし、
// その上にポイントのマーカーを置く。ポイントは線に沿って u（0〜1）だけを動かす。
// 手動のバンクポイントは断面の平面にリングを出し、掴んで回す。

namespace {

struct ProfileScreenMarker {
    graph::PathElementId id = 0;
    bool bank = false;
    bool manual = false;
    float u = 0.0f;
    ImVec2 screen{};
    bool visible = false;
    graph::ProfileFrame frame;
};

struct ProfileScreen {
    bool valid = false;
    graph::ProfileCurve centerline;
    std::vector<ImVec2> screen;
    std::vector<bool> visible;
    std::vector<ProfileScreenMarker> markers;
    std::string error;
};

ProfileScreen BuildProfileScreen(const graph::PathSettings& path, const XMMATRIX& viewProjection,
                                 const ImVec2& viewportMin, const ImVec2& size) {
    ProfileScreen result;
    if (!graph::BuildPathCenterline(path, result.centerline, &result.error)) {
        return result;
    }
    result.valid = true;
    result.screen.reserve(result.centerline.points.size());
    result.visible.reserve(result.centerline.points.size());
    for (const XMFLOAT3& point : result.centerline.points) {
        const ProjectedPoint projected = ProjectToViewport(viewProjection, point, viewportMin, size);
        result.screen.push_back(projected.screen);
        result.visible.push_back(projected.visible);
    }
    const auto addMarker = [&](graph::PathElementId id, bool bank, bool manual, float u) {
        ProfileScreenMarker marker;
        marker.id = id;
        marker.bank = bank;
        marker.manual = manual;
        marker.u = u;
        marker.frame = graph::EvaluateProfileFrame(path, result.centerline, u);
        const ProjectedPoint projected =
            ProjectToViewport(viewProjection, marker.frame.position, viewportMin, size);
        marker.screen = projected.screen;
        marker.visible = projected.visible;
        result.markers.push_back(marker);
    };
    for (const graph::PathVerticalPoint& point : path.verticalPoints) {
        addMarker(point.id, false, false, point.u);
    }
    for (const graph::PathBankPoint& point : path.bankPoints) {
        addMarker(point.id, true, point.manual, point.u);
    }
    return result;
}

// 中心線上でカーソルに最も近い位置の u。radius の内側に無ければ偽。
bool NearestProfileU(const ProfileScreen& screen, const ImVec2& mouse, float radius, float& outU) {
    if (!screen.valid || screen.screen.size() < 2) {
        return false;
    }
    float best = radius;
    bool found = false;
    const float total = std::max(screen.centerline.TotalLength(), 1e-6f);
    for (size_t i = 0; i + 1 < screen.screen.size(); ++i) {
        if (!screen.visible[i] || !screen.visible[i + 1]) {
            continue;
        }
        float t = 0.0f;
        const float distance = DistanceToSegment(mouse, screen.screen[i], screen.screen[i + 1], t);
        if (distance < best) {
            best = distance;
            const float a = screen.centerline.arcLengths[i];
            const float b = screen.centerline.arcLengths[i + 1];
            outU = std::clamp((a + (b - a) * t) / total, 0.0f, 1.0f);
            found = true;
        }
    }
    return found;
}

// 最寄りのマーカー。bank で縦断 / バンクのどちらを対象にするか選ぶ。
graph::PathElementId NearestProfileMarker(const ProfileScreen& screen, const ImVec2& mouse,
                                          float radius, bool bank) {
    graph::PathElementId best = 0;
    float bestDistance = radius;
    for (const ProfileScreenMarker& marker : screen.markers) {
        if (!marker.visible || marker.bank != bank) {
            continue;
        }
        const float distance = Distance(mouse, marker.screen);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = marker.id;
        }
    }
    return best;
}

// 回転ギズモのリング。断面の平面（right / up）に置き、θ は right から up へ回る角。
// バンク角 a のとき Right 端は θ = -a に来る（正で Left 側が上がる = Right 側が下がる）。
struct BankRingScreen {
    bool valid = false;
    ImVec2 center{};
    std::vector<ImVec2> ring;
    std::vector<bool> visible;
    std::vector<float> theta;
    ImVec2 barLeft{};
    ImVec2 barRight{};
    bool barVisible = false;
    float radiusWorld = 0.0f;
};

constexpr float kBankRingPixels = 44.0f;
constexpr int kBankRingSegments = 48;

// 画面上の長さ（px）をその位置での実寸へ直す。
float MetersPerPixelAt(const XMMATRIX& viewProjection, const XMFLOAT3& position, const ImVec2& size) {
    const auto axes = renderer::ProjectMoveAxes(viewProjection, position, size.x, size.y, 1.0f);
    const float pixelsPerMeter = std::max({axes.pixelsPerMeter[0], axes.pixelsPerMeter[1], axes.pixelsPerMeter[2]});
    return pixelsPerMeter > 1e-3f ? 1.0f / pixelsPerMeter : 0.0f;
}

BankRingScreen BuildBankRing(const graph::ProfileFrame& frame, float bankRadians,
                             const XMMATRIX& viewProjection, const ImVec2& viewportMin,
                             const ImVec2& size) {
    BankRingScreen ring;
    const float metersPerPixel = MetersPerPixelAt(viewProjection, frame.position, size);
    if (metersPerPixel <= 0.0f) {
        return ring;
    }
    ring.radiusWorld = ui::Scaled(kBankRingPixels) * metersPerPixel;
    const XMVECTOR center = XMLoadFloat3(&frame.position);
    const XMVECTOR right = XMLoadFloat3(&frame.right);
    const XMVECTOR up = XMLoadFloat3(&frame.up);
    const auto project = [&](XMVECTOR world) {
        XMFLOAT3 p;
        XMStoreFloat3(&p, world);
        return ProjectToViewport(viewProjection, p, viewportMin, size);
    };
    const ProjectedPoint projectedCenter = project(center);
    if (!projectedCenter.visible) {
        return ring;
    }
    ring.center = projectedCenter.screen;
    for (int i = 0; i <= kBankRingSegments; ++i) {
        const float theta = static_cast<float>(i) / kBankRingSegments * 2.0f * 3.14159265f;
        const XMVECTOR world = XMVectorAdd(center, XMVectorScale(
            XMVectorAdd(XMVectorScale(right, std::cos(theta)), XMVectorScale(up, std::sin(theta))),
            ring.radiusWorld));
        const ProjectedPoint projected = project(world);
        ring.ring.push_back(projected.screen);
        ring.visible.push_back(projected.visible);
        ring.theta.push_back(theta);
    }
    const XMVECTOR bankRight = XMVectorSubtract(XMVectorScale(right, std::cos(bankRadians)),
                                                XMVectorScale(up, std::sin(bankRadians)));
    const ProjectedPoint left = project(XMVectorSubtract(center, XMVectorScale(bankRight, ring.radiusWorld)));
    const ProjectedPoint rightEnd = project(XMVectorAdd(center, XMVectorScale(bankRight, ring.radiusWorld)));
    ring.barLeft = left.screen;
    ring.barRight = rightEnd.screen;
    ring.barVisible = left.visible && rightEnd.visible;
    ring.valid = true;
    return ring;
}

// リング上でカーソルに最も近い θ。radius の内側に無ければ偽（radius が負なら距離を問わない）。
bool BankRingNearestTheta(const BankRingScreen& ring, const ImVec2& mouse, float radius, float& outTheta) {
    if (!ring.valid) {
        return false;
    }
    float best = radius < 0.0f ? 1e30f : radius;
    bool found = false;
    for (size_t i = 0; i + 1 < ring.ring.size(); ++i) {
        if (!ring.visible[i] || !ring.visible[i + 1]) {
            continue;
        }
        float t = 0.0f;
        const float distance = DistanceToSegment(mouse, ring.ring[i], ring.ring[i + 1], t);
        if (distance < best) {
            best = distance;
            outTheta = ring.theta[i] + (ring.theta[i + 1] - ring.theta[i]) * t;
            found = true;
        }
    }
    return found;
}

float NormalizeAngle(float radians) {
    constexpr float kTwoPi = 2.0f * 3.14159265f;
    while (radians > 3.14159265f) radians -= kTwoPi;
    while (radians < -3.14159265f) radians += kTwoPi;
    return radians;
}

}  // namespace

void Application::HandlePathProfileInput(graph::Node& node, bool itemHovered,
                                         const ImVec2& viewportMin, const ImVec2& viewportMax) {
    auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return;
    }
    graph::PathSettings& path = settings->path;
    PathEditState& state = m_pathEdit;
    const ImGuiIO& io = ImGui::GetIO();

    // 制御点の操作状態は使わない。モードを切り替えた直後に残っていても捨てる。
    state.dragging = false;
    state.gizmoDragging = false;
    state.boxPending = state.boxSelecting = false;
    state.hoverPoint = state.hoverEdge = 0;
    state.gizmoHover = -1;
    if (state.selectedProfile != 0 && graph::FindVerticalPoint(path, state.selectedProfile) == nullptr &&
        graph::FindBankPoint(path, state.selectedProfile) == nullptr) {
        state.selectedProfile = 0;
    }

    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ProfileScreen screen = BuildProfileScreen(path, viewProjection, viewportMin, size);
    const bool bankMode = state.profileMode == PathEditState::kProfileBank;
    const ImVec2 mouse = io.MousePos;
    const bool mouseInside = itemHovered;
    bool changed = false;
    bool released = false;

    // 手動のバンクポイントを選んでいれば回転リングを出す。
    graph::PathBankPoint* selectedBank = bankMode ? graph::FindBankPoint(path, state.selectedProfile) : nullptr;
    BankRingScreen ring;
    if (selectedBank != nullptr && selectedBank->manual && screen.valid && !state.profileDragging) {
        const graph::ProfileFrame frame = graph::EvaluateProfileFrame(path, screen.centerline, selectedBank->u);
        ring = BuildBankRing(frame, XMConvertToRadians(selectedBank->angleDegrees), viewProjection,
                             viewportMin, size);
    }
    state.ringHover = false;
    float ringTheta = 0.0f;
    if (mouseInside && ring.valid && !state.ringDragging && !state.profileDragging) {
        state.ringHover = BankRingNearestTheta(ring, mouse, ui::Scaled(kGizmoHitRadius), ringTheta);
    }

    // --- ホバー ---------------------------------------------------------------
    state.hoverProfile = 0;
    if (mouseInside && !state.ringDragging && !state.profileDragging && !state.ringHover) {
        state.hoverProfile = NearestProfileMarker(screen, mouse, ui::Scaled(kPointHitRadius), bankMode);
    }

    // --- 押した -----------------------------------------------------------------
    if (mouseInside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (state.ringHover && selectedBank != nullptr) {
            state.ringDragging = true;
            state.ringStartTheta = ringTheta;
            state.ringStartAngle = selectedBank->angleDegrees;
        } else if (io.KeyCtrl) {
            // 線の上にポイントを置き、そのまま掴む。
            float u = 0.0f;
            if (NearestProfileU(screen, mouse, ui::Scaled(kSnapRadius), u)) {
                state.selectedProfile = bankMode ? graph::AddBankPoint(path, u) : graph::AddVerticalPoint(path, u);
                state.profileDragging = true;
                state.dragMoved = true;
                state.pressPos = mouse;
                changed = true;
            }
        } else if (state.hoverProfile != 0) {
            state.selectedProfile = state.hoverProfile;
            state.profileDragging = true;
            state.dragMoved = false;
            state.pressPos = mouse;
        } else {
            state.selectedProfile = 0;
        }
    }

    // --- リングのドラッグ（手動の角度） ----------------------------------------------
    if (state.ringDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && selectedBank != nullptr) {
            float theta = 0.0f;
            if (BankRingNearestTheta(ring, mouse, -1.0f, theta)) {
                // Right 端は θ = -a にあるので、リングを θ の正へ回すと角度は減る。
                const float delta = NormalizeAngle(theta - state.ringStartTheta);
                const float angle = std::clamp(state.ringStartAngle - XMConvertToDegrees(delta), -90.0f, 90.0f);
                if (std::abs(angle - selectedBank->angleDegrees) > 1e-4f) {
                    selectedBank->angleDegrees = angle;
                    changed = true;
                }
            }
        } else {
            state.ringDragging = false;
            released = true;
        }
    }

    // --- マーカーのドラッグ（線に沿って u を動かす） ------------------------------------
    if (state.profileDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!state.dragMoved && Distance(mouse, state.pressPos) > ui::Scaled(kDragThreshold)) {
                state.dragMoved = true;
            }
            float u = 0.0f;
            if (state.dragMoved && NearestProfileU(screen, mouse, 1e30f, u)) {
                if (auto* vertical = graph::FindVerticalPoint(path, state.selectedProfile)) {
                    if (vertical->u != u) { vertical->u = u; changed = true; }
                } else if (auto* bank = graph::FindBankPoint(path, state.selectedProfile)) {
                    if (bank->u != u) { bank->u = u; changed = true; }
                }
            }
        } else {
            state.profileDragging = false;
            if (state.dragMoved) {
                released = true;
            }
        }
    }

    // --- キー ---------------------------------------------------------------------
    if (mouseInside && !io.WantTextInput && !state.profileDragging && !state.ringDragging) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.selectedProfile = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && state.selectedProfile != 0) {
            changed |= graph::DeleteProfilePoint(path, state.selectedProfile);
            state.selectedProfile = 0;
        }
    }

    if (changed) {
        m_graph.MarkDirty();
        MarkDocumentChanged();
    }
    if (released) {
        m_documentJoinsEdit = true;
    }
}

void Application::DrawPathProfileOverlay(const graph::Node& node, ImDrawList* drawList,
                                         const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return;
    }
    const graph::PathSettings& path = settings->path;
    const PathEditState& state = m_pathEdit;
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ProfileScreen screen = BuildProfileScreen(path, viewProjection, viewportMin, size);
    const bool bankMode = state.profileMode == PathEditState::kProfileBank;

    const ImU32 shadow = IM_COL32(0, 0, 0, 120);
    const ImU32 centerlineColor = IM_COL32(255, 220, 120, 170);
    const ImU32 verticalColor = IM_COL32(190, 230, 120, 240);
    const ImU32 bankAutoColor = IM_COL32(255, 190, 90, 240);
    const ImU32 bankManualColor = IM_COL32(255, 130, 80, 240);
    const ImU32 hoverColor = IM_COL32(255, 245, 255, 255);
    const ImU32 selectedColor = IM_COL32(255, 255, 255, 255);
    const ImU32 textColor = IM_COL32(235, 235, 235, 255);

    if (!screen.valid) {
        // 線形にできない Path（分岐など）。理由を線の代わりに出す。
        const ImVec2 at(viewportMin.x + ui::Scaled(10.0f),
                        viewportMin.y + ui::Scaled(10.0f) + ImGui::GetFrameHeight() + ui::Scaled(6.0f));
        drawList->AddText(at, IM_COL32(255, 180, 120, 255), screen.error.c_str());
        return;
    }

    // --- 縦断反映後の中心線 -----------------------------------------------------
    for (size_t i = 0; i + 1 < screen.screen.size(); ++i) {
        if (!screen.visible[i] || !screen.visible[i + 1]) {
            continue;
        }
        drawList->AddLine(screen.screen[i], screen.screen[i + 1], shadow, ui::Scaled(3.0f));
        drawList->AddLine(screen.screen[i], screen.screen[i + 1], centerlineColor, ui::Scaled(1.5f));
    }

    // --- マーカー ---------------------------------------------------------------
    const auto project = [&](const XMFLOAT3& world) {
        return ProjectToViewport(viewProjection, world, viewportMin, size);
    };
    for (const ProfileScreenMarker& marker : screen.markers) {
        if (!marker.visible) {
            continue;
        }
        const bool active = marker.bank == bankMode;
        const bool selected = marker.id == state.selectedProfile;
        const bool hovered = marker.id == state.hoverProfile;
        const float radius = ui::Scaled(selected ? 6.5f : 5.5f);
        ImU32 color = marker.bank ? (marker.manual ? bankManualColor : bankAutoColor) : verticalColor;
        if (!active) {
            color = (color & 0x00FFFFFFu) | (110u << 24);
        } else if (hovered) {
            color = hoverColor;
        }
        const ImVec2 c = marker.screen;
        if (marker.bank) {
            // バンクは断面の傾きを短い棒で示し、その中央に四角。
            const float metersPerPixel = MetersPerPixelAt(viewProjection, marker.frame.position, size);
            if (metersPerPixel > 0.0f) {
                const float half = ui::Scaled(16.0f) * metersPerPixel;
                const float a = marker.frame.bankRadians;
                const XMVECTOR right = XMLoadFloat3(&marker.frame.right);
                const XMVECTOR up = XMLoadFloat3(&marker.frame.up);
                const XMVECTOR bankRight = XMVectorSubtract(XMVectorScale(right, std::cos(a)),
                                                            XMVectorScale(up, std::sin(a)));
                const XMVECTOR center = XMLoadFloat3(&marker.frame.position);
                XMFLOAT3 left, rightEnd;
                XMStoreFloat3(&left, XMVectorSubtract(center, XMVectorScale(bankRight, half)));
                XMStoreFloat3(&rightEnd, XMVectorAdd(center, XMVectorScale(bankRight, half)));
                const ProjectedPoint pl = project(left);
                const ProjectedPoint pr = project(rightEnd);
                if (pl.visible && pr.visible) {
                    drawList->AddLine(pl.screen, pr.screen, shadow, ui::Scaled(4.0f));
                    drawList->AddLine(pl.screen, pr.screen, color, ui::Scaled(2.0f));
                    drawList->AddCircleFilled(pr.screen, ui::Scaled(2.5f), color, 10);
                }
            }
            drawList->AddRectFilled(ImVec2(c.x - radius - 1.5f, c.y - radius - 1.5f),
                                    ImVec2(c.x + radius + 1.5f, c.y + radius + 1.5f), shadow, ui::Scaled(2.0f));
            if (marker.manual) {
                drawList->AddRectFilled(ImVec2(c.x - radius, c.y - radius), ImVec2(c.x + radius, c.y + radius),
                                        color, ui::Scaled(2.0f));
            } else {
                drawList->AddRectFilled(ImVec2(c.x - radius, c.y - radius), ImVec2(c.x + radius, c.y + radius),
                                        IM_COL32(20, 22, 26, 230), ui::Scaled(2.0f));
                drawList->AddRect(ImVec2(c.x - radius, c.y - radius), ImVec2(c.x + radius, c.y + radius),
                                  color, ui::Scaled(2.0f), 0, ui::Scaled(2.0f));
            }
        } else {
            // 縦断はひし形。
            const ImVec2 diamond[4] = {ImVec2(c.x, c.y - radius), ImVec2(c.x + radius, c.y),
                                       ImVec2(c.x, c.y + radius), ImVec2(c.x - radius, c.y)};
            const float pad = ui::Scaled(1.5f);
            const ImVec2 shadowDiamond[4] = {ImVec2(c.x, c.y - radius - pad), ImVec2(c.x + radius + pad, c.y),
                                             ImVec2(c.x, c.y + radius + pad), ImVec2(c.x - radius - pad, c.y)};
            drawList->AddConvexPolyFilled(shadowDiamond, 4, shadow);
            drawList->AddConvexPolyFilled(diamond, 4, color);
        }
        if (selected) {
            drawList->AddCircle(c, radius + ui::Scaled(4.0f), selectedColor, 24, ui::Scaled(1.5f));
        }
        // 値のラベル。線の上に重ならないよう右上へ。
        if (active) {
            char text[64] = {};
            if (marker.bank) {
                const graph::PathBankPoint* point = nullptr;
                for (const auto& candidate : path.bankPoints) if (candidate.id == marker.id) point = &candidate;
                if (point != nullptr && point->manual) {
                    std::snprintf(text, sizeof(text), "手動 %.1f°", point->angleDegrees);
                } else {
                    std::snprintf(text, sizeof(text), "自動 %.1f°", XMConvertToDegrees(marker.frame.bankRadians));
                }
            } else {
                const graph::PathVerticalPoint* point = nullptr;
                for (const auto& candidate : path.verticalPoints) if (candidate.id == marker.id) point = &candidate;
                if (point != nullptr) {
                    std::snprintf(text, sizeof(text), "VCL %.0f m / %+.1f m", point->vclMeters, point->offsetMeters);
                }
            }
            if (text[0] != '\0') {
                const ImVec2 at(c.x + radius + ui::Scaled(6.0f), c.y - ImGui::GetTextLineHeight() - ui::Scaled(2.0f));
                drawList->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), shadow, text);
                drawList->AddText(at, textColor, text);
            }
        }
    }

    // --- 回転リング（手動のバンクポイント） ---------------------------------------------
    if (bankMode && state.selectedProfile != 0 && !state.profileDragging) {
        const graph::PathBankPoint* point = nullptr;
        for (const auto& candidate : path.bankPoints) if (candidate.id == state.selectedProfile) point = &candidate;
        if (point != nullptr && point->manual) {
            const graph::ProfileFrame frame = graph::EvaluateProfileFrame(path, screen.centerline, point->u);
            const BankRingScreen ring = BuildBankRing(frame, XMConvertToRadians(point->angleDegrees),
                                                     viewProjection, viewportMin, size);
            if (ring.valid) {
                const bool active = state.ringDragging || state.ringHover;
                const ImU32 ringColor = active ? hoverColor : IM_COL32(255, 188, 76, 230);
                const float width = ui::Scaled(active ? 3.0f : 2.0f);
                for (size_t i = 0; i + 1 < ring.ring.size(); ++i) {
                    if (!ring.visible[i] || !ring.visible[i + 1]) continue;
                    drawList->AddLine(ring.ring[i], ring.ring[i + 1], shadow, width + ui::Scaled(2.0f));
                    drawList->AddLine(ring.ring[i], ring.ring[i + 1], ringColor, width);
                }
                if (ring.barVisible) {
                    drawList->AddLine(ring.barLeft, ring.barRight, shadow, ui::Scaled(5.0f));
                    drawList->AddLine(ring.barLeft, ring.barRight, IM_COL32(235, 235, 235, 240), ui::Scaled(3.0f));
                    drawList->AddCircleFilled(ring.barRight, ui::Scaled(4.0f), IM_COL32(235, 235, 235, 255), 12);
                    drawList->AddText(ImVec2(ring.barRight.x + ui::Scaled(6.0f), ring.barRight.y - ui::Scaled(7.0f)),
                                      textColor, "R");
                }
            }
        }
    }
}

graph::Node* Application::CurrentPathNode() {
    graph::Node* node = m_graph.FindMutableNode(m_selectedGraphNode);
    if (node == nullptr || node->kind != graph::NodeKind::Path) {
        return nullptr;
    }
    return node;
}

const graph::RoadGeometry* Application::SurfacePathRoad(const graph::Node& pathNode) const {
    const graph::Node* road = graph::FindSurfaceRoad(m_graph, pathNode);
    if (road == nullptr) {
        m_surfaceBinding.valid = false;
        return nullptr;
    }
    if (m_surfaceBinding.pathNode != pathNode.id || m_surfaceBinding.roadNode != road->id ||
        m_surfaceBinding.revision != m_graph.Revision()) {
        std::string error;
        m_surfaceBinding.valid = graph::EvaluateRoad(m_graph, road->id, m_surfaceBinding.road, error);
        m_surfaceBinding.pathNode = pathNode.id;
        m_surfaceBinding.roadNode = road->id;
        m_surfaceBinding.revision = m_graph.Revision();
    }
    return m_surfaceBinding.valid ? &m_surfaceBinding.road : nullptr;
}

bool Application::PickSurface(const graph::RoadGeometry& road, const ImVec2& mouse, const ImVec2& viewportMin,
                              const ImVec2& viewportMax, float& outLateral, float& outDistance) const {
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) return false;
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    XMVECTOR determinant;
    const XMMATRIX inverse = XMMatrixInverse(&determinant, viewProjection);
    if (XMVectorGetX(determinant) == 0.0f) return false;
    const float ndcX = ((mouse.x - viewportMin.x) / size.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - viewportMin.y) / size.y) * 2.0f;
    const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverse);
    const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverse);
    XMFLOAT3 origin;
    XMFLOAT3 direction;
    XMStoreFloat3(&origin, nearPoint);
    XMStoreFloat3(&direction, XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint)));
    XMFLOAT3 hit;
    if (!graph::RayHitsRoad(road, origin, direction, hit)) return false;
    return graph::RoadSurfaceCoordinates(road, hit, outDistance, outLateral);
}

bool Application::SelectedPathFocusTarget(XMFLOAT3& target) const {
    const auto* node = m_graph.FindNode(m_selectedGraphNode);
    const auto* settings = node ? std::get_if<graph::PathNodeSettings>(&node->settings) : nullptr;
    if (!settings) return false;
    const auto& path = settings->path;
    if (path.surfaceSpace && !SurfacePathRoad(*node)) return false;
    std::vector<graph::PathElementId> ids;
    if (m_pathEdit.nodeId == node->id)
        ids = PathMovablePoints(path, m_pathEdit.selected, m_pathEdit.selectedEdges, m_pathEdit.selectedStrandInterior);
    if (ids.empty())
        for (const auto& point : path.points) ids.push_back(point.id);

    XMFLOAT3 lo{}, hi{};
    bool found = false;
    for (const auto id : ids) {
        const auto* point = path.FindPoint(id);
        if (!point) continue;
        const auto world = PathWorldPosition(point->x, point->z, point->y);
        if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) continue;
        if (!found) { lo = hi = world; found = true; }
        lo.x = std::min(lo.x, world.x); lo.y = std::min(lo.y, world.y); lo.z = std::min(lo.z, world.z);
        hi.x = std::max(hi.x, world.x); hi.y = std::max(hi.y, world.y); hi.z = std::max(hi.z, world.z);
    }
    if (!found) return false;
    target = {std::midpoint(lo.x, hi.x), std::midpoint(lo.y, hi.y), std::midpoint(lo.z, hi.z)};
    return true;
}

XMFLOAT3 Application::PathWorldPosition(float u, float v, float y) const {
    const graph::Node* node = m_graph.FindNode(m_selectedGraphNode);
    const auto* settings = node ? std::get_if<graph::PathNodeSettings>(&node->settings) : nullptr;
    if (settings && settings->path.surfaceSpace) {
        // 面上のパス。u = 横位置、v = 実距離、y = 面からの高さ。
        if (const graph::RoadGeometry* road = SurfacePathRoad(*node)) {
            const graph::RoadSurfacePoint point = graph::RoadSurfacePointAt(*road, v, u);
            return {point.position.x + point.normal.x * y, point.position.y + point.normal.y * y,
                    point.position.z + point.normal.z * y};
        }
    }
    return {u, y, v};
}

// カーソルからレイを飛ばし、パスの作業面（水平面 / 道路面）と交わる所を探す。
bool Application::ViewportRay(const ImVec2& mouse, const ImVec2& viewportMin, const ImVec2& viewportMax,
                              XMFLOAT3& outOrigin, XMFLOAT3& outDirection) const {
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return false;
    }
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    XMVECTOR determinant;
    const XMMATRIX inverse = XMMatrixInverse(&determinant, viewProjection);
    if (XMVectorGetX(determinant) == 0.0f) {
        return false;
    }
    const float ndcX = ((mouse.x - viewportMin.x) / size.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - viewportMin.y) / size.y) * 2.0f;
    const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverse);
    const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverse);
    const XMVECTOR direction = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));
    XMStoreFloat3(&outOrigin, nearPoint);
    XMStoreFloat3(&outDirection, direction);
    return true;
}

bool Application::PickTerrainUv(const ImVec2& mouse, const ImVec2& viewportMin,
                                const ImVec2& viewportMax, float& outU, float& outV) const {
    XMFLOAT3 origin;
    XMFLOAT3 dir;
    if (!ViewportRay(mouse, viewportMin, viewportMax, origin, dir)) {
        return false;
    }

    const graph::Node* node = m_graph.FindNode(m_selectedGraphNode);
    const auto* settings = node ? std::get_if<graph::PathNodeSettings>(&node->settings) : nullptr;
    if (settings && settings->path.surfaceSpace) {
        // 面上のパスは道路面との交点を道路座標にする。面の外は置けない。
        const graph::RoadGeometry* road = SurfacePathRoad(*node);
        return road != nullptr && PickSurface(*road, mouse, viewportMin, viewportMax, outU, outV);
    }
    // メッシュの有無やグリッドの範囲に依存しない作業平面（選択した点の高さ。無ければ Y = 0）。
    float height = 0.0f;
    if (settings && !m_pathEdit.selected.empty()) {
        if (const auto* point = settings->path.FindPoint(m_pathEdit.selected.front())) {
            height = point->y;
        }
    }
    if (std::abs(dir.y) < 1e-6f) return false;
    const float t = (height - origin.y) / dir.y;
    if (!std::isfinite(t) || t < 0.0f) return false;
    outU = origin.x + dir.x * t;
    outV = origin.z + dir.z * t;
    return std::isfinite(outU) && std::isfinite(outV);
}

void Application::HandlePathInput(graph::Node& node, bool itemActive, bool itemHovered,
                                  const ImVec2& viewportMin, const ImVec2& viewportMax) {
    auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return;
    }
    graph::PathSettings& path = settings->path;
    PathEditState& state = m_pathEdit;
    const ImGuiIO& io = ImGui::GetIO();
    (void)itemActive;

    // ノードが変わったら状態を捨てる。消えた点の ID も落とす。
    if (state.nodeId != node.id) {
        state = PathEditState{};
        state.nodeId = node.id;
    }
    std::erase_if(state.selected, [&path](graph::PathElementId id) {
        return path.FindPoint(id) == nullptr;
    });
    std::erase_if(state.selectedEdges, [&path](graph::PathElementId id) {
        return path.FindEdge(id) == nullptr;
    });
    std::erase_if(state.selectedStrandInterior, [&path](graph::PathElementId id) {
        return path.FindPoint(id) == nullptr;
    });
    if (state.dragPoint != 0 && path.FindPoint(state.dragPoint) == nullptr) {
        state.dragPoint = 0;
        state.dragging = false;
    }
    // Surface に道路が繋がった / 外れたときに座標の意味を切り替える。
    // 繋いだときは今の世界座標を面の座標へ写す。外したときは数値をそのまま実寸として扱う。
    {
        // 「面上か」はリンクで決める。道路の評価に一時的に失敗しても座標の意味は変えない
        // （実寸へ戻して次のフレームで再変換すると点が壊れる）。
        const bool bound = graph::FindSurfaceRoad(m_graph, node) != nullptr;
        const graph::RoadGeometry* surfaceRoad = bound ? SurfacePathRoad(node) : nullptr;
        if (surfaceRoad != nullptr && !path.surfaceSpace) {
            for (graph::PathPoint& point : path.points) {
                float distance = 0.0f;
                float lateral = 0.0f;
                if (graph::RoadSurfaceCoordinates(*surfaceRoad, {point.x, point.y, point.z}, distance, lateral)) {
                    // 道路の外にあった点は路面の端へ寄せる。面の外は編集できないため。
                    const float halfWidth = surfaceRoad->settings.widthMeters * 0.5f;
                    point.x = std::clamp(lateral, -halfWidth, halfWidth);
                    point.z = distance;
                    point.y = 0.0f;
                }
            }
            path.surfaceSpace = true;
            state.profileMode = PathEditState::kProfilePoints;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        } else if (!bound && path.surfaceSpace) {
            path.surfaceSpace = false;
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    }
    // 縦断 / バンクの編集モードは別の入力処理。制御点の選択やギズモは出さない。
    if (!path.surfaceSpace && state.profileMode != PathEditState::kProfilePoints) {
        HandlePathProfileInput(node, itemHovered, viewportMin, viewportMax);
        return;
    }
    state.profileDragging = state.ringDragging = state.ringHover = false;
    state.hoverProfile = 0;

    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const auto worldOf = [this](float u, float v, float offset) {
        return PathWorldPosition(u, v, offset);
    };
    const PathScreenCache cache =
        BuildPathScreenCache(path, viewProjection, viewportMin, size, worldOf);

    const ImVec2 mouse = io.MousePos;
    const bool mouseInside = itemHovered;
    float terrainU = 0.0f;
    float terrainV = 0.0f;
    const bool onTerrain =
        mouseInside && PickTerrainUv(mouse, viewportMin, viewportMax, terrainU, terrainV);

    // --- 移動ギズモ -------------------------------------------------------------
    // 選択（点の集合 / 鎖）があれば重心にギズモを出し、掴んで動かす。
    // Ctrl を押している間（伸ばす）は出さない（クリックを横取りしないため）。
    const std::vector<graph::PathElementId> movable = PathMovablePoints(
        path, state.selected, state.selectedEdges, state.selectedStrandInterior);
    // 面上のパスは横位置・実距離の2軸で移動し、面からの高さを維持する。
    const PathGizmoScreen gizmo =
        (!io.KeyCtrl && !state.dragging && !state.boxPending)
            ? BuildPathGizmo(path, movable, viewProjection, viewportMin, size, worldOf, path.surfaceSpace ? SurfacePathRoad(node) : nullptr)
            : PathGizmoScreen{};
    state.gizmoHover = (mouseInside && !state.gizmoDragging && !state.boxPending) ? PathGizmoHit(gizmo, mouse) : -1;

    // --- ホバー ---------------------------------------------------------------
    state.hoverPoint = 0;
    state.hoverEdge = 0;
    if (mouseInside && !state.dragging && !state.gizmoDragging && !state.boxPending && state.gizmoHover < 0) {
        state.hoverPoint = NearestPoint(cache, mouse, ui::Scaled(kPointHitRadius), 0);
        if (state.hoverPoint == 0) {
            state.hoverEdge =
                NearestEdge(cache, mouse, ui::Scaled(kEdgeHitRadius), 0, state.hoverEdgeT);
        }
    }

    bool changed = false;
    // 点の選択とエッジの選択は排他。
    const auto selectOnly = [&](graph::PathElementId id) {
        state.selected.clear();
        state.selectedEdges.clear();
        state.selectedStrandInterior.clear();
        if (id != 0) {
            state.selected.push_back(id);
        }
    };
    // 鎖を丸ごと選ぶ。内側の点も覚えておき、Delete で一緒に消す。
    const auto selectStrand = [&](graph::PathElementId edgeId) {
        state.selected.clear();
        state.selectedEdges.clear();
        state.selectedStrandInterior.clear();
        const graph::PathStrand* strand = graph::FindStrandOfEdge(cache.strands, edgeId);
        if (strand == nullptr) {
            state.selectedEdges.push_back(edgeId);
            return;
        }
        state.selectedEdges = strand->edges;
        state.selectedStrandInterior.clear();
        for (size_t i = 1; i + 1 < strand->points.size(); ++i) {
            state.selectedStrandInterior.push_back(strand->points[i]);
        }
    };
    // 伸ばす起点。選択している点（複数なら先頭）。
    const graph::PathElementId anchor = state.selected.empty() ? 0 : state.selected.front();
    // Ctrl を押している間が「伸ばす」モード。
    const bool extend = io.KeyCtrl;
    // 分離 / 切り離しで点を離す距離を、その点の位置での実寸（m）へ直す。
    const auto detachOffsetUv = [&](graph::PathElementId pointId) {
        const graph::PathPoint* point = path.FindPoint(pointId);
        if (point == nullptr) {
            return 0.01f;
        }
        const ProjectedPoint a = ProjectToViewport(
            viewProjection, worldOf(point->x, point->z, point->y), viewportMin,
            size);
        const ProjectedPoint b = ProjectToViewport(
            viewProjection, worldOf(point->x + 0.01f, point->z, point->y), viewportMin, size);
        if (!a.visible || !b.visible) {
            return 0.01f;
        }
        const float pixelsPerMeter = Distance(a.screen, b.screen) / 0.01f;
        return (pixelsPerMeter > 1e-3f) ? (ui::Scaled(kDetachPixels) / pixelsPerMeter) : 0.01f;
    };

    // --- 押した -----------------------------------------------------------------
    if (mouseInside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && state.gizmoHover >= 0) {
        // ギズモを掴んだ。選択は変えず、各点の今の位置を控える。
        state.gizmoAxis = state.gizmoHover;
        state.gizmoDragging = true;
        state.gizmoPressPos = mouse;
        if (state.gizmoAxis >= 0 && state.gizmoAxis < 3) {
            state.gizmoAxisDirection = gizmo.direction[state.gizmoAxis];
            state.gizmoUnitsPerPixel = gizmo.uvPerPixel[state.gizmoAxis];
        }
        state.gizmoPressU = terrainU;
        state.gizmoPressV = terrainV;
        state.gizmoStart.clear();
        for (const graph::PathElementId id : movable) {
            if (const graph::PathPoint* point = path.FindPoint(id)) {
                state.gizmoStart.push_back({id, point->x, point->z, point->y});
            }
        }
    } else if (mouseInside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.pressPos = mouse;
        state.dragMoved = false;
        state.snapPoint = 0;
        state.snapEdge = 0;
        if (extend) {
            // 伸ばす。起点（選択した点）から、クリックした所へ繋ぐ。
            if (state.hoverPoint != 0) {
                if (anchor != 0 && anchor != state.hoverPoint) {
                    changed |= graph::ConnectPathPoints(path, anchor, state.hoverPoint);
                }
                selectOnly(state.hoverPoint);
            } else if (state.hoverEdge != 0) {
                // エッジの途中に点を挿入して掴む。点を選んでいれば、その点とも繋ぐ。
                const graph::PathElementId inserted =
                    graph::InsertPathPointOnEdge(path, state.hoverEdge, state.hoverEdgeT);
                if (inserted != 0) {
                    if (anchor != 0) {
                        graph::ConnectPathPoints(path, anchor, inserted);
                    }
                    selectOnly(inserted);
                    state.dragPoint = inserted;
                    state.dragging = true;
                    changed = true;
                }
            } else if (onTerrain) {
                // 空の所。選択が無ければ新しい線の始点になる。
                const graph::PathElementId added =
                    graph::AddPathPoint(path, terrainU, terrainV, anchor);
                if (added != 0) {
                    selectOnly(added);
                    state.dragPoint = added;
                    state.dragging = true;
                    changed = true;
                }
            }
        } else if (state.hoverPoint != 0) {
            // 点を選ぶ。そのままドラッグで動かせる。
            selectOnly(state.hoverPoint);
            state.dragPoint = state.hoverPoint;
            state.dragging = true;
        } else if (state.hoverEdge != 0) {
            // エッジを選ぶ = その鎖を選ぶ。挿入は Ctrl + クリック。
            selectStrand(state.hoverEdge);
        } else {
            state.boxPending = true;
            state.boxSelecting = false;
            state.boxAdditive = io.KeyShift;
            state.boxStart = state.boxEnd = mouse;
            state.boxPreviousPoints = state.selected;
            state.boxPreviousEdges = state.selectedEdges;
            state.boxPreviousInterior = state.selectedStrandInterior;

        }
    }

    // 空からのドラッグは画面上の制御点を矩形選択。形状とアンドゥ履歴は変更しない。
    if (state.boxPending) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.selected = state.boxPreviousPoints;
            state.selectedEdges = state.boxPreviousEdges;
            state.selectedStrandInterior = state.boxPreviousInterior;
            state.boxPending = state.boxSelecting = false;
            return;
        }
        state.boxEnd = {std::clamp(mouse.x, viewportMin.x, viewportMax.x),
                        std::clamp(mouse.y, viewportMin.y, viewportMax.y)};
        state.boxSelecting |= Distance(state.boxStart, state.boxEnd) > ui::Scaled(kDragThreshold);
        if (state.boxSelecting) {
            state.selected = state.boxAdditive ? state.boxPreviousPoints : std::vector<graph::PathElementId>{};
            state.selectedEdges.clear();
            state.selectedStrandInterior.clear();
            const ImVec2 lo(std::min(state.boxStart.x, state.boxEnd.x), std::min(state.boxStart.y, state.boxEnd.y));
            const ImVec2 hi(std::max(state.boxStart.x, state.boxEnd.x), std::max(state.boxStart.y, state.boxEnd.y));
            for (const auto& point : cache.points) {
                if (point.visible && point.screen.x >= lo.x && point.screen.x <= hi.x &&
                    point.screen.y >= lo.y && point.screen.y <= hi.y &&
                    std::find(state.selected.begin(), state.selected.end(), point.id) == state.selected.end()) {
                    state.selected.push_back(point.id);
                }
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!state.boxSelecting && !state.boxAdditive) selectOnly(0);
            state.boxPending = state.boxSelecting = false;
        }
    }

    // --- ギズモのドラッグ ---------------------------------------------------------
    bool released = false;
    if (state.gizmoDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float du = 0.0f;
        float dv = 0.0f;
        float dy = 0.0f;
        bool haveDelta = false;
        if (state.gizmoAxis == 3) {
            // 平面。掴んだ所と今のカーソルの作業面上の差。面の外では動かさない。
            if (onTerrain) {
                du = terrainU - state.gizmoPressU;
                dv = terrainV - state.gizmoPressV;
                haveDelta = true;
            }
        } else if (state.gizmoAxis >= 0 && state.gizmoAxis < 3) {
            // 軸。カーソルの動きを軸の向きへ落とし、画面上の距離を実寸へ直す。
            const ImVec2 delta(mouse.x - state.gizmoPressPos.x, mouse.y - state.gizmoPressPos.y);
            const ImVec2 direction = state.gizmoAxisDirection;
            const float along = delta.x * direction.x + delta.y * direction.y;
            const float amount = along * state.gizmoUnitsPerPixel;
            if (state.gizmoAxis == 0) {
                du = amount;
            } else if (state.gizmoAxis == 1) {
                dv = amount;
            } else {
                dy = amount;
            }
            haveDelta = true;
        }
        if (haveDelta && path.surfaceSpace) {
            dy = 0.0f;
            const auto* road = SurfacePathRoad(node);
            if (!road) haveDelta = false;
            else {
                // 全点へ同じ移動量を適用する。端では一群を止め、点間の配置を崩さない。
                const float halfWidth = road->settings.widthMeters * 0.5f;
                float minU = -FLT_MAX, maxU = FLT_MAX, minV = -FLT_MAX, maxV = FLT_MAX;
                for (const auto& start : state.gizmoStart) {
                    minU = std::max(minU, -halfWidth - start.x); maxU = std::min(maxU, halfWidth - start.x);
                    minV = std::max(minV, -start.z); maxV = std::min(maxV, road->rowDistances.back() - start.z);
                }
                du = minU <= maxU ? std::clamp(du, minU, maxU) : 0.0f;
                dv = minV <= maxV ? std::clamp(dv, minV, maxV) : 0.0f;
            }
        }
        if (haveDelta) {
            for (const PathEditState::GizmoStart& start : state.gizmoStart) {
                if (graph::PathPoint* point = path.FindPoint(start.id)) {
                    const float u = start.x + du;
                    const float v = start.z + dv;
                    const float y = start.y + dy;
                    if (u != point->x || v != point->z || y != point->y) {
                        point->x = u;
                        point->z = v;
                        point->y = y;
                        changed = true;
                    }
                }
            }
        }
    } else if (state.gizmoDragging) {
        // 離した。
        state.gizmoDragging = false;
        state.gizmoAxis = -1;
        state.gizmoStart.clear();
        released = true;
    }

    // --- ドラッグ -----------------------------------------------------------------
    if (state.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!state.dragMoved && Distance(mouse, state.pressPos) > ui::Scaled(kDragThreshold)) {
            state.dragMoved = true;
        }
        if (state.dragMoved && onTerrain) {
            if (graph::PathPoint* point = path.FindPoint(state.dragPoint)) {
                point->x = terrainU;
                point->z = terrainV;
                changed = true;
            }
            // 吸着先。Shift を押している間は吸着しない。
            state.snapPoint = 0;
            state.snapEdge = 0;
            if (!io.KeyShift) {
                state.snapPoint =
                    NearestPoint(cache, mouse, ui::Scaled(kSnapRadius), state.dragPoint);
                if (state.snapPoint == 0) {
                    state.snapEdge = NearestEdge(cache, mouse, ui::Scaled(kSnapRadius),
                                                 state.dragPoint, state.snapEdgeT);
                }
            }
        }
    }

    // --- 離した -------------------------------------------------------------------
    if (state.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (state.dragMoved) {
            released = true;
            if (state.snapPoint != 0) {
                // 相手の点へ合体。位置と幅は相手のものが残る。
                if (graph::MergePathPoints(path, state.dragPoint, state.snapPoint)) {
                    selectOnly(state.snapPoint);
                    changed = true;
                }
            } else if (state.snapEdge != 0) {
                // 線の上へ落とした。そこに点を挿入して合体する。
                const graph::PathElementId inserted =
                    graph::InsertPathPointOnEdge(path, state.snapEdge, state.snapEdgeT);
                if (inserted != 0 && graph::MergePathPoints(path, state.dragPoint, inserted)) {
                    selectOnly(inserted);
                    changed = true;
                }
            }
        }
        state.dragging = false;
        state.dragPoint = 0;
        state.snapPoint = 0;
        state.snapEdge = 0;
    }

    // コピー。選択している点の集合、または鎖（両端と内側の点、その間のエッジ）を控える。
    const auto copySelection = [&]() {
        graph::PathClip clip;
        if (graph::ExtractPathClip(path, movable, state.selectedEdges, clip)) {
            m_pathClipboard = std::move(clip);
            TG_LOG_INFO("パスをコピーしました: 点 %zu / エッジ %zu", m_pathClipboard.points.size(),
                        m_pathClipboard.edges.size());
        }
    };
    // 貼り付け。重心がカーソルの作業面上の位置へ来るように置き、貼った点を選択にする
    // （そのままギズモで動かせる）。カーソルが面の外なら、元の位置から少しずらして置く。
    const auto pasteClipboard = [&](bool atCursor, float atU, float atV) {
        if (m_pathClipboard.points.empty()) {
            return;
        }
        float du = 0.02f;
        float dv = 0.02f;
        if (atCursor) {
            float sumU = 0.0f;
            float sumV = 0.0f;
            for (const graph::PathPoint& point : m_pathClipboard.points) {
                sumU += point.x;
                sumV += point.z;
            }
            const float count = static_cast<float>(m_pathClipboard.points.size());
            du = atU - sumU / count;
            dv = atV - sumV / count;
        }
        std::vector<graph::PathElementId> pasted;
        if (graph::PastePathClip(path, m_pathClipboard, du, dv, &pasted, nullptr)) {
            selectOnly(0);
            state.selected = pasted;
            changed = true;
        }
    };

    // --- キー ---------------------------------------------------------------------
    if (mouseInside && !io.WantTextInput && !state.dragging && !state.gizmoDragging && !state.boxPending) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            selectOnly(0);
        }
        // Ctrl+C / Ctrl+V。グラフパネルのノードのコピーと同じキーだが、
        // ビューポートの上にいるときだけパスが受け取る。
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !movable.empty()) {
            copySelection();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            pasteClipboard(onTerrain, terrainU, terrainV);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (!state.selectedEdges.empty()) {
                // 鎖の点（両端と内側）は、エッジが無くなれば一緒に消す。
                // 両端が分岐（別の鎖が付いている）なら、そちらの鎖のために残る。
                std::vector<graph::PathElementId> strandPoints = state.selectedStrandInterior;
                for (const graph::PathElementId id : state.selectedEdges) {
                    if (const graph::PathEdge* edge = path.FindEdge(id)) {
                        strandPoints.push_back(edge->from);
                        strandPoints.push_back(edge->to);
                    }
                }
                for (const graph::PathElementId id : state.selectedEdges) {
                    changed |= graph::DeletePathEdge(path, id);
                }
                for (const graph::PathElementId id : strandPoints) {
                    if (path.FindPoint(id) != nullptr && path.EdgeCount(id) == 0) {
                        changed |= graph::DeletePathPoint(path, id);
                    }
                }
                selectOnly(0);
            } else if (!state.selected.empty()) {
                for (const graph::PathElementId id : state.selected) {
                    changed |= graph::DeletePathPoint(path, id);
                }
                selectOnly(0);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.KeyCtrl) {
            for (const graph::PathElementId id : state.selectedEdges) {
                changed |= graph::ReversePathEdge(path, id);
            }
            for (const graph::PathElementId id : state.selected) {
                changed |= graph::ReversePathEdgesAt(path, id);
            }
        }
    }

    // --- 右クリックのメニュー --------------------------------------------------------
    if (mouseInside && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !state.dragging && !state.boxPending) {
        state.menuPoint = state.hoverPoint;
        state.menuEdge = state.hoverEdge;
        state.menuEdgeT = state.hoverEdgeT;
        state.menuOnTerrain = onTerrain;
        state.menuU = terrainU;
        state.menuV = terrainV;
        if (state.menuPoint != 0) {
            selectOnly(state.menuPoint);
        } else if (state.menuEdge != 0) {
            selectStrand(state.menuEdge);
        }
        ImGui::OpenPopup("##pathContextMenu");
    }
    if (ImGui::BeginPopup("##pathContextMenu")) {
        if (state.menuPoint != 0 && path.FindPoint(state.menuPoint) != nullptr) {
            const graph::PathElementId pointId = state.menuPoint;
            ImGui::TextDisabled("点");
            ImGui::Separator();
            ImGui::BeginDisabled(path.EdgeCount(pointId) < 2);
            if (ImGui::MenuItem("分離")) {
                // エッジの本数ぶんに分ける。離した点をまとめて選択しておく。
                std::vector<graph::PathElementId> created;
                if (graph::SplitPathPoint(path, pointId, detachOffsetUv(pointId), &created)) {
                    state.selected = created;
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(path.EdgeCount(pointId) == 0);
            if (ImGui::MenuItem("向きを反転")) {
                changed |= graph::ReversePathEdgesAt(path, pointId);
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("コピー", "Ctrl+C")) {
                copySelection();
            }
            if (ImGui::MenuItem("削除")) {
                changed |= graph::DeletePathPoint(path, pointId);
                selectOnly(0);
            }
        } else if (state.menuEdge != 0 && path.FindEdge(state.menuEdge) != nullptr) {
            const graph::PathElementId edgeId = state.menuEdge;
            ImGui::TextDisabled("エッジ");
            ImGui::Separator();
            if (ImGui::MenuItem("点を挿入")) {
                const graph::PathElementId inserted =
                    graph::InsertPathPointOnEdge(path, edgeId, state.menuEdgeT);
                if (inserted != 0) {
                    selectOnly(inserted);
                    changed = true;
                }
            }
            if (ImGui::MenuItem("近いほうの点から切り離す")) {
                const graph::PathEdge* edge = path.FindEdge(edgeId);
                if (edge != nullptr) {
                    const graph::PathElementId end =
                        (state.menuEdgeT < 0.5f) ? edge->from : edge->to;
                    const graph::PathElementId created =
                        graph::DetachPathEdgeEnd(path, edgeId, end, detachOffsetUv(end));
                    if (created != 0) {
                        selectOnly(created);
                        changed = true;
                    }
                }
            }
            // 向きは鎖の性質。1 本だけ反転すると鎖の中で食い違うので、鎖ごと反転する。
            if (ImGui::MenuItem("鎖の向きを反転")) {
                for (const graph::PathElementId id : state.selectedEdges) {
                    changed |= graph::ReversePathEdge(path, id);
                }
            }
            if (ImGui::MenuItem("鎖をコピー", "Ctrl+C")) {
                copySelection();
            }
            // 輪にする（Mask Area の面）。末尾から先頭へ繋ぐので、向きは鎖に沿う。
            {
                const graph::PathStrand* strand = graph::FindStrandOfEdge(cache.strands, edgeId);
                const bool canClose = strand != nullptr && !strand->closed &&
                                      strand->points.size() >= 3 &&
                                      strand->points.front() != strand->points.back();
                ImGui::BeginDisabled(!canClose);
                if (ImGui::MenuItem("閉じる（末尾を先頭へ繋ぐ）")) {
                    changed |= graph::ConnectPathPoints(path, strand->points.back(),
                                                        strand->points.front());
                }
                ImGui::EndDisabled();
            }
            if (ImGui::MenuItem("このエッジを消す（ここで切る）")) {
                changed |= graph::DeletePathEdge(path, edgeId);
                selectOnly(0);
            }
        } else {
            ImGui::TextDisabled("パス");
            ImGui::Separator();
            ImGui::BeginDisabled(!state.menuOnTerrain);
            if (ImGui::MenuItem("ここに線を始める")) {
                const graph::PathElementId added =
                    graph::AddPathPoint(path, state.menuU, state.menuV, 0);
                if (added != 0) {
                    selectOnly(added);
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(m_pathClipboard.points.empty());
            if (ImGui::MenuItem("ここに貼り付け", "Ctrl+V")) {
                pasteClipboard(state.menuOnTerrain, state.menuU, state.menuV);
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(state.selected.empty() && state.selectedEdges.empty());
            if (ImGui::MenuItem("選択を外す")) {
                selectOnly(0);
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }

    if (changed) {
        m_graph.MarkDirty();
        MarkDocumentChanged();
    }
    // 離した時点の変更（吸着による合体など）は、ドラッグと同じアンドゥの段に畳む。
    if (released) {
        m_documentJoinsEdit = true;
    }
}

void Application::DrawPathOverlay(const graph::Node& node, const ImVec2& viewportMin,
                                  const ImVec2& viewportMax) {
    const auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return;
    }
    const graph::PathSettings& path = settings->path;
    const PathEditState& state = m_pathEdit;
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const auto worldOf = [this](float u, float v, float offset) {
        return PathWorldPosition(u, v, offset);
    };
    const PathScreenCache cache =
        BuildPathScreenCache(path, viewProjection, viewportMin, size, worldOf);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);
    const bool profileMode = !path.surfaceSpace && state.profileMode != PathEditState::kProfilePoints;

    // 色。ピンの色（水色）と同じ系統で、状態は明るさで分ける。
    const ImU32 lineColor = IM_COL32(120, 200, 240, 220);
    const ImU32 lineShadow = IM_COL32(0, 0, 0, 120);
    const ImU32 hoverColor = IM_COL32(255, 245, 255, 255);
    const ImU32 snapColor = IM_COL32(255, 220, 120, 255);
    const ImU32 pointColor = IM_COL32(205, 235, 250, 240);
    const ImU32 selectedColor = IM_COL32(255, 255, 255, 255);
    const ImU32 tailColor = IM_COL32(255, 220, 120, 255);
    const float lineWidth = ui::Scaled(2.0f);

    // --- エッジ ---------------------------------------------------------------
    for (const PathScreenEdge& edge : cache.edges) {
        const bool hovered = (edge.id == state.hoverEdge && !state.dragging);
        const bool snapping = (edge.id == state.snapEdge);
        const bool selectedEdge =
            std::find(state.selectedEdges.begin(), state.selectedEdges.end(), edge.id) !=
            state.selectedEdges.end();
        // 曲線の鎖のエッジはガイド。薄く細く描き、本線は曲線のほうに任せる。
        const graph::PathStrand* strand = graph::FindStrandOfEdge(cache.strands, edge.id);
        const bool guide =
            strand != nullptr &&
            graph::StrandCurve(path, *strand, nullptr, nullptr) != graph::PathCurve::Line;
        const ImU32 baseColor = guide ? IM_COL32(120, 200, 240, 110) : lineColor;
        const ImU32 color = snapping ? snapColor
                                     : ((hovered || selectedEdge) ? hoverColor : baseColor);
        const float width = selectedEdge ? lineWidth + ui::Scaled(1.5f)
                                         : (guide ? ui::Scaled(1.0f) : lineWidth);
        for (size_t i = 0; i + 1 < edge.polyline.size(); ++i) {
            if (!edge.visible[i] || !edge.visible[i + 1]) {
                continue;
            }
            drawList->AddLine(edge.polyline[i], edge.polyline[i + 1], lineShadow,
                              width + ui::Scaled(2.0f));
            drawList->AddLine(edge.polyline[i], edge.polyline[i + 1], color, width);
        }
        // 向きの矢印。線の真ん中に小さく置く。
        const size_t mid = edge.polyline.size() / 2;
        if (mid > 0 && mid < edge.polyline.size() && edge.visible[mid - 1] && edge.visible[mid]) {
            const ImVec2 a = edge.polyline[mid - 1];
            const ImVec2 b = edge.polyline[mid];
            ImVec2 dir(b.x - a.x, b.y - a.y);
            const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (length > 1.0f) {
                dir.x /= length;
                dir.y /= length;
                const ImVec2 side(-dir.y, dir.x);
                const float head = ui::Scaled(9.0f);
                const float halfWidth = ui::Scaled(4.5f);
                const ImVec2 tip((a.x + b.x) * 0.5f + dir.x * head * 0.5f,
                                 (a.y + b.y) * 0.5f + dir.y * head * 0.5f);
                const ImVec2 base(tip.x - dir.x * head, tip.y - dir.y * head);
                drawList->AddTriangleFilled(
                    tip, ImVec2(base.x + side.x * halfWidth, base.y + side.y * halfWidth),
                    ImVec2(base.x - side.x * halfWidth, base.y - side.y * halfWidth), color);
            }
        }
    }

    // --- 曲線（本線） -----------------------------------------------------------
    for (size_t s = 0; s < cache.curves.size(); ++s) {
        const PathScreenCurve& curve = cache.curves[s];
        const graph::PathStrand& strand = cache.strands[s];
        // 鎖が選ばれていれば本線も明るく太く。
        const bool selectedStrand =
            !strand.edges.empty() &&
            std::find(state.selectedEdges.begin(), state.selectedEdges.end(),
                      strand.edges.front()) != state.selectedEdges.end() &&
            state.selectedEdges.size() == strand.edges.size();
        const ImU32 color = selectedStrand ? hoverColor : lineColor;
        const float width = selectedStrand ? lineWidth + ui::Scaled(1.5f) : lineWidth;
        for (size_t i = 0; i + 1 < curve.polyline.size(); ++i) {
            if (!curve.visible[i] || !curve.visible[i + 1]) {
                continue;
            }
            drawList->AddLine(curve.polyline[i], curve.polyline[i + 1], lineShadow,
                              width + ui::Scaled(2.0f));
            drawList->AddLine(curve.polyline[i], curve.polyline[i + 1], color, width);
        }
    }

    // --- 仮のエッジ（選択した点からカーソルへ） ----------------------------------
    // **Ctrl を押している間だけ出す。** 次のクリックで何が起きるかを先に見せる。
    const ImGuiIO& io = ImGui::GetIO();
    const bool viewportHovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax);
    const graph::PathElementId anchor = state.selected.empty() ? 0 : state.selected.front();
    if (io.KeyCtrl && anchor != 0 && viewportHovered && !state.dragging && !profileMode) {
        const PathScreenPoint* tail = cache.Find(anchor);
        ImVec2 target{};
        bool hasTarget = false;
        if (state.hoverPoint != 0) {
            if (const PathScreenPoint* point = cache.Find(state.hoverPoint)) {
                target = point->screen;
                hasTarget = point->visible;
            }
        } else if (state.hoverEdge != 0) {
            target = ImGui::GetIO().MousePos;
            hasTarget = true;
        } else {
            float u = 0.0f;
            float v = 0.0f;
            if (PickTerrainUv(ImGui::GetIO().MousePos, viewportMin, viewportMax, u, v)) {
                // 面に沿わせて描く（面上のパスで空中を横切る直線だと距離感が狂う）。
                const graph::PathPoint* from = path.FindPoint(anchor);
                if (from != nullptr && tail != nullptr && tail->visible) {
                    constexpr int kSegments = 16;
                    ImVec2 previous = tail->screen;
                    bool previousVisible = true;
                    for (int i = 1; i <= kSegments; ++i) {
                        const float t = static_cast<float>(i) / kSegments;
                        const ProjectedPoint projected = ProjectToViewport(
                            viewProjection,
                            worldOf(from->x + (u - from->x) * t, from->z + (v - from->z) * t,
                                    from->y),
                            viewportMin, size);
                        if (previousVisible && projected.visible) {
                            drawList->AddLine(previous, projected.screen,
                                              IM_COL32(120, 200, 240, 120), lineWidth);
                        }
                        previous = projected.screen;
                        previousVisible = projected.visible;
                    }
                }
            }
        }
        if (hasTarget && tail != nullptr && tail->visible) {
            drawList->AddLine(tail->screen, target, IM_COL32(255, 220, 120, 160), lineWidth);
        }
    }

    // --- 点 ---------------------------------------------------------------------
    for (const PathScreenPoint& point : cache.points) {
        if (!point.visible) {
            continue;
        }
        const bool selected =
            std::find(state.selected.begin(), state.selected.end(), point.id) != state.selected.end();
        const bool hovered = (point.id == state.hoverPoint && !state.dragging);
        const bool snapping = (point.id == state.snapPoint);
        const float radius = ui::Scaled(selected ? 5.5f : 4.5f);
        drawList->AddCircleFilled(point.screen, radius + ui::Scaled(1.5f), lineShadow, 16);
        drawList->AddCircleFilled(point.screen,
                                  radius, snapping ? snapColor : (hovered ? hoverColor : pointColor),
                                  16);
        if (selected) {
            drawList->AddCircle(point.screen, radius + ui::Scaled(3.0f), selectedColor, 20,
                                ui::Scaled(1.5f));
        }
        // Ctrl を押している間は、伸ばす起点をもう 1 つの輪で示す。
        if (io.KeyCtrl && point.id == anchor) {
            drawList->AddCircle(point.screen, radius + ui::Scaled(6.0f), tailColor, 24,
                                ui::Scaled(1.5f));
        }
    }

    // --- 縦断 / バンクのポイントとリング ------------------------------------------
    if (profileMode) {
        DrawPathProfileOverlay(node, drawList, viewportMin, viewportMax);
    }

    // --- 移動ギズモ -------------------------------------------------------------
    // 座標軸ギズモと同じ色（X = 赤、Z = 青）。掴める所は明るくする。
    if (!io.KeyCtrl && !state.dragging && !state.boxPending && !profileMode) {
        const std::vector<graph::PathElementId> movable = PathMovablePoints(
            path, state.selected, state.selectedEdges, state.selectedStrandInterior);
        const PathGizmoScreen gizmo =
            BuildPathGizmo(path, movable, viewProjection, viewportMin, size, worldOf, path.surfaceSpace ? SurfacePathRoad(node) : nullptr);
        if (gizmo.valid) {
            const ImU32 axisColors[3] = {IM_COL32(226, 96, 96, 255), IM_COL32(96, 146, 226, 255), IM_COL32(96, 206, 116, 255)};
            const int active = state.gizmoDragging ? state.gizmoAxis : state.gizmoHover;
            for (int axis = 0; axis < gizmo.axisCount; ++axis) {
                if (!gizmo.axisValid[axis]) continue;
                const ImU32 color = (active == axis) ? hoverColor : axisColors[axis];
                const float width = ui::Scaled((active == axis) ? 3.0f : 2.0f);
                drawList->AddLine(gizmo.center, gizmo.tip[axis], lineShadow, width + ui::Scaled(2.0f));
                drawList->AddLine(gizmo.center, gizmo.tip[axis], color, width);
                if (!path.surfaceSpace) {
                    const char* labels[] = {"X", "Z", "Y"};
                    drawList->AddText(ImVec2(gizmo.tip[axis].x + ui::Scaled(5.0f), gizmo.tip[axis].y), color, labels[axis]);
                }
                // 先端の矢じり。
                const ImVec2 dir = gizmo.direction[axis];
                const ImVec2 side(-dir.y, dir.x);
                const float head = ui::Scaled(10.0f);
                const float halfWidth = ui::Scaled(5.0f);
                const ImVec2 tip = gizmo.tip[axis];
                const ImVec2 base(tip.x - dir.x * head, tip.y - dir.y * head);
                drawList->AddTriangleFilled(
                    tip, ImVec2(base.x + side.x * halfWidth, base.y + side.y * halfWidth),
                    ImVec2(base.x - side.x * halfWidth, base.y - side.y * halfWidth), color);
            }
            // 中央の平面ハンドル。
            const float radius = ui::Scaled(kGizmoCenterRadius);
            const ImU32 centerColor = (active == 3) ? hoverColor : IM_COL32(235, 235, 235, 220);
            drawList->AddRectFilled(ImVec2(gizmo.center.x - radius, gizmo.center.y - radius),
                                    ImVec2(gizmo.center.x + radius, gizmo.center.y + radius),
                                    lineShadow, ui::Scaled(2.0f));
            drawList->AddRect(ImVec2(gizmo.center.x - radius + 1.0f, gizmo.center.y - radius + 1.0f),
                              ImVec2(gizmo.center.x + radius - 1.0f, gizmo.center.y + radius - 1.0f),
                              centerColor, ui::Scaled(2.0f), 0, ui::Scaled(1.5f));
        }
    }

    // --- 操作の案内 ---------------------------------------------------------------
    // ビューポートの右下に、いまの状態でできることを「操作 → 意味」の小さな表で出す。
    // プロパティに書くと視線を外さないと読めないので、見ている場所へ重ねる。
    struct HintRow {
        const char* key;
        const char* action;
    };
    std::vector<HintRow> rows;
    if (profileMode) {
        const bool bankMode = state.profileMode == PathEditState::kProfileBank;
        if (state.ringDragging) {
            rows = {{"離す", "角度を確定"}};
        } else if (state.profileDragging) {
            rows = {{"離す", "位置を確定"}};
        } else if (bankMode) {
            rows = {{"Ctrl + 線をクリック", "バンクポイントを置く"},
                    {"マーカーをドラッグ", "線に沿って動かす"},
                    {"リングをドラッグ", "手動の角度を回す"},
                    {"プロパティ", "設計速度 / 自動・手動 / 角度"},
                    {"Delete / Esc", "消す / 選択を外す"},
                    {"Alt + ドラッグ", "視点"}};
        } else {
            rows = {{"Ctrl + 線をクリック", "縦断ポイントを置く"},
                    {"マーカーをドラッグ", "線に沿って動かす"},
                    {"プロパティ", "縦断曲線長 / オフセット"},
                    {"Delete / Esc", "消す / 選択を外す"},
                    {"Alt + ドラッグ", "視点"}};
        }
    } else if (io.KeyCtrl) {
        if (anchor != 0) {
            rows = {{"クリック", "点を置いて伸ばす"},
                    {"点をクリック", "繋ぐ"},
                    {"線をクリック", "点を挿入して繋ぐ"},
                    {"Ctrl を離す", "戻る"}};
        } else {
            rows = {{"クリック", "線を始める"},
                    {"線をクリック", "点を挿入"},
                    {"Ctrl を離す", "戻る"}};
        }
    } else if (state.dragging) {
        rows = {{"点 / 線に重ねる", "繋ぐ"}, {"Shift", "吸着しない"}};
    } else if (state.gizmoDragging) {
        rows = {{"離す", "確定（経路は作り直す）"}};
    } else if (!state.selectedEdges.empty()) {
        rows = {{"プロパティ", "曲線の種類 / 丸め / 向き"},
                {"ギズモをドラッグ", "鎖を動かす（軸 / 中央で平面）"},
                {"Ctrl + 線をクリック", "点を挿入"},
                {"右クリック", "挿入 / 切り離し / ここで切る"},
                {"Ctrl+C / Ctrl+V", "鎖をコピー / カーソルへ貼る"},
                {"Delete / R", "鎖を消す / 向きを反転"},
                {"Esc", "選択を外す"}};
    } else if (!state.selected.empty()) {
        rows = {{"Ctrl + クリック", "伸ばす（点や線の上で繋ぐ）"},
                {"ドラッグ", "動かす"},
                {"ギズモをドラッグ", "選択をまとめて動かす"},
                {"右クリック", "分離 / 反転 / 削除"},
                {"Ctrl+C / Ctrl+V", "コピー / カーソルへ貼る"},
                {"Delete / R", "消す / 向きを反転"},
                {"Esc", "選択を外す"}};
    } else {
        rows = {{"クリック", "点や線（鎖）を選ぶ"},
                {"空をドラッグ", "ポイントを矩形選択"},
                {"Shift + ドラッグ", "追加選択"},
                {"Ctrl + クリック", "線を始める"},
                {"Ctrl + 線をクリック", "点を挿入"},
                {"Ctrl+V", "コピーしたパスをカーソルへ貼る"},
                {"Alt + ドラッグ", "視点"}};
    }
    {
        float keyWidth = 0.0f;
        float actionWidth = 0.0f;
        for (const HintRow& row : rows) {
            keyWidth = std::max(keyWidth, ImGui::CalcTextSize(row.key).x);
            actionWidth = std::max(actionWidth, ImGui::CalcTextSize(row.action).x);
        }
        const ImVec2 padding(ui::Scaled(10.0f), ui::Scaled(6.0f));
        const float gap = ui::Scaled(14.0f);
        const float margin = ui::Scaled(10.0f);
        const float lineHeight = ImGui::GetTextLineHeight();
        const float spacing = ImGui::GetStyle().ItemSpacing.y * 0.5f;
        const float width = keyWidth + gap + actionWidth + padding.x * 2.0f;
        const float height = lineHeight * static_cast<float>(rows.size()) +
                             spacing * static_cast<float>(rows.size() - 1) + padding.y * 2.0f;
        const ImVec2 boxMax(viewportMax.x - margin, viewportMax.y - margin);
        const ImVec2 boxMin(boxMax.x - width, boxMax.y - height);
        drawList->AddRectFilled(boxMin, boxMax, IM_COL32(8, 10, 12, 190), ui::Scaled(4.0f));
        float y = boxMin.y + padding.y;
        for (const HintRow& row : rows) {
            // 操作は右揃えで落とした色、意味は明るい色。列が揃うので読み飛ばしやすい。
            const float keyX = boxMin.x + padding.x + keyWidth - ImGui::CalcTextSize(row.key).x;
            drawList->AddText(ImVec2(keyX, y), IM_COL32(170, 175, 180, 255), row.key);
            drawList->AddText(ImVec2(boxMin.x + padding.x + keyWidth + gap, y),
                              IM_COL32(235, 235, 235, 255), row.action);
            y += lineHeight + spacing;
        }
    }

    if (state.boxSelecting) {
        const ImVec2 lo(std::min(state.boxStart.x, state.boxEnd.x), std::min(state.boxStart.y, state.boxEnd.y));
        const ImVec2 hi(std::max(state.boxStart.x, state.boxEnd.x), std::max(state.boxStart.y, state.boxEnd.y));
        drawList->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.4f));
        drawList->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_PlotLinesHovered));
    }
    drawList->PopClipRect();
}

bool Application::DrawPathSettings(graph::Node& node) {
    auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return false;
    }
    graph::PathSettings& path = settings->path;
    const graph::PathSettings defaults;
    bool changed = false;

    ui::SectionHeader("パス");
    if (ui::BeginPropertyTable("graphPathRows")) {
        ui::PropertyValue("要素", "点 %zu / エッジ %zu", path.points.size(), path.edges.size());
        ui::PropertyValue("座標", "%s", path.surfaceSpace ? "面上（横位置 / 実距離 m）" : "実寸（m）");
        ui::PropertyLabelEmpty("pathClear");
        ImGui::BeginDisabled(path.points.empty());
        if (ui::Button("全部消す", ui::kWideButtonWidth)) {
            path.points.clear();
            path.edges.clear();
            m_pathEdit = PathEditState{};
            m_pathEdit.nodeId = node.id;
            changed = true;
        }
        ImGui::EndDisabled();
        ui::PropertyEnd();
        ui::EndPropertyTable();
    }

    // --- 道路線形（縦断曲線とバンク角） ------------------------------------------------
    if (!path.surfaceSpace) {
        if (m_pathEdit.nodeId != node.id) {
            m_pathEdit = PathEditState{};
            m_pathEdit.nodeId = node.id;
        }
        const graph::PathSettings profileDefaults;
        ui::SectionHeader("道路線形");
        if (ui::BeginPropertyTable("graphPathProfileRows")) {
            static const char* const kProfileModeLabels[] = {"制御点", "縦断ポイント", "バンクポイント"};
            int mode = m_pathEdit.profileMode;
            if (ui::PropertyCombo("編集", &mode, kProfileModeLabels, IM_ARRAYSIZE(kProfileModeLabels), 0,
                                  "ビューポートで何を編集するか。縦断 / バンクでは線形の上に Ctrl＋クリックでポイントを置く")) {
                m_pathEdit.profileMode = mode;
                m_pathEdit.selectedProfile = 0;
                m_pathEdit.profileDragging = m_pathEdit.ringDragging = false;
            }
            ui::PropertyValue("ポイント", "縦断 %zu / バンク %zu", path.verticalPoints.size(), path.bankPoints.size());
            changed |= ui::PropertyBool("バンクを反映", &path.bankEnabled, profileDefaults.bankEnabled,
                "道路面を接線まわりに傾ける。切ると手動ポイントがあっても水平のまま");
            changed |= ui::PropertyFloat("設計速度", &path.designSpeedKmh, 0.0f, 200.0f, profileDefaults.designSpeedKmh,
                "バンクポイントの無い所の設計速度（km/h）。曲率半径と合わせて自動のバンク角を決める", "%.0f km/h");
            changed |= ui::PropertyFloat("摩擦係数", &path.frictionCoefficient, 0.0f, 1.0f, profileDefaults.frictionCoefficient,
                "横方向の摩擦係数。大きいほど自動のバンク角が小さくなる", "%.2f");
            changed |= ui::PropertyBool("バンクを平滑化", &path.smoothBank, profileDefaults.smoothBank,
                "距離方向にガウス平均して、手動ポイントや曲率変化の急な角度変化をなだらかにする");
            if (path.smoothBank) {
                changed |= ui::PropertyFloat("平滑化距離", &path.bankSmoothMeters, 1.0f, 200.0f, profileDefaults.bankSmoothMeters,
                    "前後にこの距離だけ角度を平均する（m）", "%.0f m");
            }
            ui::EndPropertyTable();
        }
        graph::PathVerticalPoint* vertical = graph::FindVerticalPoint(path, m_pathEdit.selectedProfile);
        graph::PathBankPoint* bank = graph::FindBankPoint(path, m_pathEdit.selectedProfile);
        if (vertical != nullptr) {
            ui::SectionHeader("選択した縦断ポイント");
            if (ui::BeginPropertyTable("graphPathVerticalRows")) {
                changed |= ui::PropertyFloat("位置", &vertical->u, 0.0f, 1.0f, 0.5f,
                    "線形の始点を 0、終点を 1 とした位置", "%.3f");
                changed |= ui::PropertyFloat("縦断曲線長", &vertical->vclMeters, 0.0f, 1000.0f, 50.0f,
                    "この点を中心に前後の勾配をつなぐ放物線の長さ（m）。前後の区間に収まらなければ短くなる", "%.0f m",
                    ImGuiSliderFlags_Logarithmic);
                changed |= ui::PropertyFloat("オフセット", &vertical->offsetMeters, -100.0f, 100.0f, 0.0f,
                    "制御点から決まる高さに足す量（m）", "%.2f m");
                ui::PropertyLabelEmpty("pathVerticalDelete");
                if (ui::Button("削除")) {
                    graph::DeleteProfilePoint(path, m_pathEdit.selectedProfile);
                    m_pathEdit.selectedProfile = 0;
                    changed = true;
                }
                ui::PropertyEnd();
                ui::EndPropertyTable();
            }
        } else if (bank != nullptr) {
            ui::SectionHeader("選択したバンクポイント");
            if (ui::BeginPropertyTable("graphPathBankRows")) {
                changed |= ui::PropertyFloat("位置", &bank->u, 0.0f, 1.0f, 0.5f,
                    "線形の始点を 0、終点を 1 とした位置", "%.3f");
                changed |= ui::PropertyFloat("設計速度", &bank->designSpeedKmh, 0.0f, 200.0f, path.designSpeedKmh,
                    "この位置の設計速度（km/h）。自動のときはここから角度を求め、手動でも速度の補間点になる", "%.0f km/h");
                changed |= ui::PropertyBool("手動", &bank->manual, false,
                    "角度を直接指定する。ビューポートのリングを回しても変えられる");
                if (bank->manual) {
                    changed |= ui::PropertyFloat("角度", &bank->angleDegrees, -90.0f, 90.0f, 0.0f,
                        "バンク角（度）。正で Left 側が上がる", "%.1f°");
                }
                graph::ProfileCurve centerline;
                if (graph::BuildPathCenterline(path, centerline, nullptr)) {
                    const float evaluated = graph::EvaluateBankAngleRadians(path, centerline,
                        std::clamp(bank->u, 0.0f, 1.0f) * centerline.TotalLength());
                    ui::PropertyValue("評価した角度", "%.1f°", DirectX::XMConvertToDegrees(evaluated));
                }
                ui::PropertyLabelEmpty("pathBankDelete");
                if (ui::Button("削除")) {
                    graph::DeleteProfilePoint(path, m_pathEdit.selectedProfile);
                    m_pathEdit.selectedProfile = 0;
                    changed = true;
                }
                ui::PropertyEnd();
                ui::EndPropertyTable();
            }
        }
    }

    // 選択した点。複数選んでいれば全部に同じ値を入れる（表示は先頭の値）。
    std::vector<graph::PathPoint*> selectedPoints;
    if (m_pathEdit.nodeId == node.id) {
        for (const graph::PathElementId id : m_pathEdit.selected) {
            if (graph::PathPoint* point = path.FindPoint(id)) {
                selectedPoints.push_back(point);
            }
        }
    }
    if (!selectedPoints.empty()) {
        ui::SectionHeader(selectedPoints.size() == 1 ? "選択した点" : "選択した点（複数）");
        graph::PathPoint edit = *selectedPoints.front();
        bool pointChanged = false;
        if (ui::BeginPropertyTable("graphPathPointRows")) {
            {
                float xyz[] = {edit.x, edit.y, edit.z};
                constexpr float defaultXyz[] = {0.0f, 0.0f, 0.0f};
                const unsigned axes = ui::PropertyFloat3Input(path.surfaceSpace ? "横位置/高さ/距離 (m)" : "位置 (m)", xyz, defaultXyz,
                    path.surfaceSpace ? "道路面の座標。横位置は正が Left、距離は始点から、高さは面から。変更した軸だけを選択点へ適用する"
                                      : "ワールド座標。変更した軸だけを選択点へ適用する");
                for (auto* point : selectedPoints) {
                    if (axes & 1u) point->x = xyz[0];
                    if (axes & 2u) point->y = xyz[1];
                    if (axes & 4u) point->z = xyz[2];
                }
                changed |= axes != 0;
            }
            if (!path.surfaceSpace) {
                static const char* const kStopLineLabels[] = {"なし", "進行方向", "対向", "両方"};
                int stop = static_cast<int>(edit.stopLine);
                if (ui::PropertyCombo("停止線", &stop, kStopLineLabels, IM_ARRAYSIZE(kStopLineLabels), 0,
                                      "この点の位置に停止線を引く。進行方向の車線か対向車線か。Lane Marking が描く。"
                                      "曲線は点を通らないので、道路上で最も近い位置になる")) {
                    edit.stopLine = static_cast<graph::PathStopLine>(stop);
                    pointChanged = true;
                }
            }
            ui::EndPropertyTable();
        }
        if (pointChanged) {
            for (graph::PathPoint* point : selectedPoints) {
                point->stopLine = edit.stopLine;
            }
            changed = true;
        }
    }

    if (m_pathEdit.nodeId == node.id && !m_pathEdit.selectedEdges.empty()) {
        std::vector<graph::PathEdge*> edges;
        for (const graph::PathElementId id : m_pathEdit.selectedEdges) {
            for (graph::PathEdge& edge : path.edges) {
                if (edge.id == id) {
                    edges.push_back(&edge);
                }
            }
        }
        if (!edges.empty()) {
            ui::SectionHeader("選択した鎖");
            // 曲線の種類と丸め。鎖なら全エッジに同じ値を入れる（表示は先頭。混在なら注記）。
            graph::PathCurve curve = edges.front()->curve;
            float rounding = edges.front()->rounding;
            float clothoidRatio = edges.front()->clothoidRatio;
            bool mixed = false;
            for (const graph::PathEdge* edge : edges) {
                if (edge->curve != curve || std::abs(edge->rounding - rounding) > 1e-4f ||
                    std::abs(edge->clothoidRatio - clothoidRatio) > 1e-4f) {
                    mixed = true;
                }
            }
            if (ui::BeginPropertyTable("graphPathEdgeRows")) {
                ui::PropertyValue("エッジ", "%zu 本（点 %d → 点 %d）", edges.size(),
                                  edges.front()->from, edges.back()->to);
                static const char* const kCurveLabels[] = {"直線", "2 次ベジェ",
                                                           "3 次 B スプライン", "クロソイド"};
                int curveIndex = static_cast<int>(curve);
                bool curveChanged = ui::PropertyCombo(
                    "曲線", &curveIndex, kCurveLabels, IM_ARRAYSIZE(kCurveLabels), 0,
                    "折れ線をガイドにして、その内側に描く曲線。点は通らない。"
                    "2 次は角ごとに丸めて両端だけ通る。3 次はさらに滑らかだが折れ線からより離れる。"
                    "クロソイドは角ごとに緩和曲線 → 円弧 → 緩和曲線で、曲率が 0 から連続的に"
                    "立ち上がる（道路 / 鉄道の線形）");
                curve = static_cast<graph::PathCurve>(curveIndex);
                curveChanged |= ui::PropertyFloat(
                    "丸め", &rounding, 0.0f, 1.0f, 1.0f,
                    "どれだけ角を取るか。0 で折れ線のまま、1 で最大（線分の中点まで）", "%.2f");
                if (curve == graph::PathCurve::Clothoid) {
                    curveChanged |= ui::PropertyFloat(
                        "クロソイド比", &clothoidRatio, 0.0f, 1.0f, 0.5f,
                        "交角のうち緩和曲線 2 本が受け持つ割合。0 で純粋な円弧、"
                        "1 で円弧なし（緩和曲線だけ）",
                        "%.2f");
                }
                if (curveChanged) {
                    for (graph::PathEdge* edge : edges) {
                        edge->curve = curve;
                        edge->rounding = rounding;
                        edge->clothoidRatio = clothoidRatio;
                    }
                    changed = true;
                }
                ui::PropertyLabelEmpty("pathEdgeButtons");
                if (ui::Button("向きを反転")) {
                    for (graph::PathEdge* edge : edges) {
                        changed |= graph::ReversePathEdge(path, edge->id);
                    }
                }
                ImGui::SameLine();
                if (ui::Button("削除")) {
                    for (const graph::PathElementId id : m_pathEdit.selectedEdges) {
                        changed |= graph::DeletePathEdge(path, id);
                    }
                    for (const graph::PathElementId id : m_pathEdit.selectedStrandInterior) {
                        if (path.EdgeCount(id) == 0) {
                            changed |= graph::DeletePathPoint(path, id);
                        }
                    }
                    m_pathEdit.selectedEdges.clear();
                    m_pathEdit.selectedStrandInterior.clear();
                }
                ui::PropertyEnd();
                ui::EndPropertyTable();
            }
            if (mixed) {
                ui::HintText("鎖の中で曲線の設定が混在している。変えると全部に入る");
            }
        }
    }

    ui::HintText(path.surfaceSpace ? "面上のパス。路面の上で Ctrl＋クリックして点を置き、路面に沿ってドラッグする。Decal の Path へ繋ぐ。" :
                 "Ctrl＋クリックで点を追加。グリッド外にも配置できる。高さは選択した点のYで編集する。Surface に Road を繋ぐと面上のパスになる。");
    return changed;
}

}  // namespace tg
