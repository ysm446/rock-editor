// モデルの系統のノード（Model / Transform。まとめるのは Merge）。ノードの追加と Mesh Output への接続、描画、
// ビューポートでの選択・ギズモ（W 移動 / E 回転 / R 倍率、FBX のノードを回すノード用の輪）・ドラッグ移動、範囲の枠、プロパティ。
// 描画はレンダラの drawSceneExtras から呼ばれ、メッシュと同じシャドウマップ・照明で描く。
// 置き方（位置・回転・倍率）の変更はメッシュに関係しないので、グラフの改版（再生成）を起こさない。
// 仕様は docs/reference/model-assets.md の「モデルの系統のノード」。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/Log.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <DirectXCollision.h>

#include <algorithm>
#include <map>
#include <array>
#include <cfloat>
#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace rock {
namespace fs = std::filesystem;
using namespace DirectX;

namespace {

// ギズモの大きさ（画面上の px、拡大率を掛ける前）と当たり判定の幅。
constexpr float kGizmoLengthPixels = 90.0f;
constexpr float kGizmoPickPixels = 7.0f;
constexpr int kRingSegments = 64;
// 倍率ギズモの中心の四角と、軸の先の四角の半分の大きさ（px、拡大率を掛ける前）。
constexpr float kScaleCenterPixels = 7.0f;
constexpr float kScaleTipPixels = 5.0f;
// ハンドルの番号。Application::ModelInstanceDrag::handle と同じ。
constexpr int kHandleAxis = 0;     // 0〜2: X / Y / Z 軸
constexpr int kHandlePlane = 3;    // 3〜5: 法線が X / Y / Z の平面（YZ / XZ / XY）
constexpr int kHandleRing = 6;     // 6〜8: X / Y / Z 軸まわりの回転
constexpr int kHandleScale = 9;    // 9〜11: X / Y / Z 軸の倍率（倍率は均一なので、どの軸でも全体が変わる）
constexpr int kHandleScaleAll = 12;  // 12: 中心の四角（全体の倍率）
// 座標軸ギズモと同じ色（X = 赤、Y = 緑、Z = 青）。意味を持つ色なのでテーマから引かない。
constexpr ImU32 kAxisColors[3] = {IM_COL32(226, 96, 96, 255), IM_COL32(124, 196, 104, 255), IM_COL32(96, 146, 226, 255)};
const XMFLOAT3 kAxes[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

std::wstring LowerExtension(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const ImVec2 ab(b.x - a.x, b.y - a.y), ap(p.x - a.x, p.y - a.y);
    const float length = ab.x * ab.x + ab.y * ab.y;
    const float t = length > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / length, 0.0f, 1.0f) : 0.0f;
    const float dx = a.x + ab.x * t - p.x, dy = a.y + ab.y * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool PointInQuad(const ImVec2& p, const std::array<ImVec2, 4>& quad) {
    // 凸四角形。辺の外積の符号がすべて揃えば内側。
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const ImVec2& a = quad[i];
        const ImVec2& b = quad[(i + 1) % 4];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        const int s = cross > 0.0f ? 1 : (cross < 0.0f ? -1 : 0);
        if (s == 0) continue;
        if (sign == 0) sign = s;
        else if (s != sign) return false;
    }
    return true;
}

// 下流の Transform の行列から、平行移動と倍率を除いた回転だけを取り出す（倍率は均一）。
XMMATRIX RotationPart(FXMMATRIX matrix) {
    XMMATRIX rotation = matrix;
    rotation.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const float scale = XMVectorGetX(XMVector3Length(rotation.r[0]));
    if (scale > 1e-8f) {
        for (int i = 0; i < 3; ++i) rotation.r[i] = XMVectorScale(rotation.r[i], 1.0f / scale);
    }
    return rotation;
}

// 画面上のギズモの形。ワールドの軸を投影して、画面での大きさを一定にする。
struct GizmoScreen {
    bool valid = false;
    ImVec2 center{};
    ImVec2 tips[3]{};
    bool axisVisible[3]{};
    std::array<ImVec2, 4> planes[3]{};
    std::array<ImVec2, kRingSegments + 1> rings[3]{};
    // 輪の各点がカメラから見て奥側か（奥は薄く描く）。
    std::array<bool, kRingSegments + 1> ringBack[3]{};
};

// axes はギズモの 3 軸（ワールド、単位長）。既定はワールドの X / Y / Z。
GizmoScreen BuildGizmo(const renderer::Camera& camera, const XMFLOAT3& pivot, const ImVec2& viewportMin,
                       const ImVec2& viewportMax, const XMFLOAT3* axes = kAxes) {
    GizmoScreen gizmo;
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) return gizmo;
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const XMFLOAT3 eye = camera.Position();
    const XMVECTOR center = XMLoadFloat3(&pivot);
    const float distance = XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&eye), center)));
    // この距離で画面 1px がワールドの何 m か。これでギズモを画面上の一定の大きさにする。
    const float worldPerPixel = 2.0f * distance * std::tan(camera.FovY() * 0.5f) / size.y;
    const float length = worldPerPixel * ui::Scaled(kGizmoLengthPixels);
    const auto project = [&](FXMVECTOR world) {
        XMFLOAT3 p;
        XMStoreFloat3(&p, world);
        return ProjectToViewport(viewProjection, p, viewportMin, size);
    };
    const ProjectedPoint c = project(center);
    if (!c.visible) return gizmo;
    gizmo.center = c.screen;
    const XMVECTOR toEye = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&eye), center));
    for (int axis = 0; axis < 3; ++axis) {
        const XMVECTOR a = XMLoadFloat3(&axes[axis]);
        const ProjectedPoint tip = project(XMVectorAdd(center, XMVectorScale(a, length)));
        gizmo.axisVisible[axis] = tip.visible;
        gizmo.tips[axis] = tip.screen;
        // 平面ハンドル。残りの 2 軸の 0.2〜0.4 の四角。
        const XMVECTOR u = XMLoadFloat3(&axes[(axis + 1) % 3]);
        const XMVECTOR v = XMLoadFloat3(&axes[(axis + 2) % 3]);
        const float lo = length * 0.2f, hi = length * 0.4f;
        const float corners[4][2] = {{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}};
        for (int i = 0; i < 4; ++i) {
            gizmo.planes[axis][i] = project(XMVectorAdd(center, XMVectorAdd(XMVectorScale(u, corners[i][0]),
                                                                            XMVectorScale(v, corners[i][1])))).screen;
        }
        // 回転の輪。軸に垂直な円。
        for (int i = 0; i <= kRingSegments; ++i) {
            const float angle = XM_2PI * static_cast<float>(i) / kRingSegments;
            const XMVECTOR offset = XMVectorAdd(XMVectorScale(u, std::cos(angle) * length * 0.8f),
                                                XMVectorScale(v, std::sin(angle) * length * 0.8f));
            gizmo.rings[axis][i] = project(XMVectorAdd(center, offset)).screen;
            gizmo.ringBack[axis][i] = XMVectorGetX(XMVector3Dot(offset, toEye)) < 0.0f;
        }
    }
    gizmo.valid = true;
    return gizmo;
}

// ギズモの種類。Application::ModelGizmoMode と同じ並び。
enum class GizmoKind { Translate, Rotate, Scale };

// カーソルが乗っているハンドル。無ければ -1。
int HitGizmo(const GizmoScreen& gizmo, GizmoKind kind, const ImVec2& mouse) {
    if (!gizmo.valid) return -1;
    const float pick = ui::Scaled(kGizmoPickPixels);
    int best = -1;
    float bestDistance = pick;
    if (kind == GizmoKind::Scale) {
        // 中心の四角を軸より優先する（軸の根元と重なるため）。
        const float half = ui::Scaled(kScaleCenterPixels);
        if (std::abs(mouse.x - gizmo.center.x) <= half && std::abs(mouse.y - gizmo.center.y) <= half) return kHandleScaleAll;
        for (int axis = 0; axis < 3; ++axis) {
            if (!gizmo.axisVisible[axis]) continue;
            const float d = DistanceToSegment(mouse, gizmo.center, gizmo.tips[axis]);
            if (d < bestDistance) {
                bestDistance = d;
                best = kHandleScale + axis;
            }
        }
    } else if (kind == GizmoKind::Translate) {
        // 平面ハンドルを軸より優先する（軸の根元と重なるため）。
        for (int axis = 0; axis < 3; ++axis)
            if (PointInQuad(mouse, gizmo.planes[axis])) return kHandlePlane + axis;
        for (int axis = 0; axis < 3; ++axis) {
            if (!gizmo.axisVisible[axis]) continue;
            const float d = DistanceToSegment(mouse, gizmo.center, gizmo.tips[axis]);
            if (d < bestDistance) {
                bestDistance = d;
                best = kHandleAxis + axis;
            }
        }
    } else {
        for (int axis = 0; axis < 3; ++axis) {
            for (int i = 0; i < kRingSegments; ++i) {
                // 奥側の半分は手前のものより掴みにくくする（重なったときに手前を優先）。
                const float d = DistanceToSegment(mouse, gizmo.rings[axis][i], gizmo.rings[axis][i + 1]) +
                                (gizmo.ringBack[axis][i] ? pick * 0.5f : 0.0f);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = kHandleRing + axis;
                }
            }
        }
    }
    return best;
}

// レイと、点 p を通る向き a の直線の最接近点の、直線上の位置（a は単位長）。平行なら偽。
bool ClosestOnLine(const XMFLOAT3& origin, const XMFLOAT3& direction, FXMVECTOR p, FXMVECTOR a, float& t) {
    const XMVECTOR r = XMLoadFloat3(&direction);
    const XMVECTOR w0 = XMVectorSubtract(p, XMLoadFloat3(&origin));
    const float b = XMVectorGetX(XMVector3Dot(a, r));
    const float d = XMVectorGetX(XMVector3Dot(a, w0));
    const float e = XMVectorGetX(XMVector3Dot(r, w0));
    const float denominator = 1.0f - b * b;
    if (std::abs(denominator) < 1e-5f) return false;
    t = (b * e - d) / denominator;
    return true;
}

// レイと、点 p を通る法線 n の平面の交点。当たらなければ偽。
bool IntersectPlane(const XMFLOAT3& origin, const XMFLOAT3& direction, FXMVECTOR p, FXMVECTOR n, XMFLOAT3& hit) {
    const XMVECTOR o = XMLoadFloat3(&origin);
    const XMVECTOR r = XMLoadFloat3(&direction);
    const float denominator = XMVectorGetX(XMVector3Dot(n, r));
    if (std::abs(denominator) < 1e-5f) return false;
    const float t = XMVectorGetX(XMVector3Dot(n, XMVectorSubtract(p, o))) / denominator;
    if (t <= 0.0f || t > 100000.0f) return false;
    XMStoreFloat3(&hit, XMVectorAdd(o, XMVectorScale(r, t)));
    return true;
}

// Model ノードの設定の、FBX のノードの回転を書く。すべて 0 なら項目を消す。変わったら真。
bool SetModelNodeRotation(graph::ModelNodeSettings& settings, const std::string& node, const float degrees[3]) {
    const auto found = std::find_if(settings.nodeRotations.begin(), settings.nodeRotations.end(),
                                    [&](const renderer::ModelNodeRotation& r) { return r.node == node; });
    const bool isZero = degrees[0] == 0.0f && degrees[1] == 0.0f && degrees[2] == 0.0f;
    if (isZero) {
        if (found == settings.nodeRotations.end()) return false;
        settings.nodeRotations.erase(found);
        return true;
    }
    if (found != settings.nodeRotations.end()) {
        if (std::equal(degrees, degrees + 3, found->rotationDegrees)) return false;
        std::copy(degrees, degrees + 3, found->rotationDegrees);
        return true;
    }
    renderer::ModelNodeRotation added;
    added.node = node;
    std::copy(degrees, degrees + 3, added.rotationDegrees);
    settings.nodeRotations.push_back(std::move(added));
    return true;
}

}  // namespace

graph::GraphId Application::PlaceModel(uint64_t modelId, const XMFLOAT3& position) {
    if (FindModel(modelId) == nullptr) return 0;
    const graph::GraphId nodeId = m_graph.CreateNode(graph::NodeKind::Model);
    graph::Node* node = m_graph.FindMutableNode(nodeId);
    if (node == nullptr) return 0;
    auto& settings = std::get<graph::ModelNodeSettings>(node->settings);
    settings.model = modelId;
    settings.position[0] = position.x;
    settings.position[1] = position.y;
    settings.position[2] = position.z;
    m_graphNodesToPlace.push_back(nodeId);

    // Mesh Output へ繋ぐ。無ければ作る。既に別のものが繋がっていれば Merge でまとめる。
    graph::GraphId outputId = 0;
    for (const graph::Node& candidate : m_graph.Nodes())
        if (candidate.kind == graph::NodeKind::MeshOutput) { outputId = candidate.id; break; }
    if (outputId == 0) {
        outputId = m_graph.CreateNode(graph::NodeKind::MeshOutput);
        m_graphNodesToPlace.push_back(outputId);
    }
    // Mesh Output の位置が未定（作った直後）なら、既存のノードの右に置く。重なって見えなくならないように。
    if (graph::Node* created = m_graph.FindMutableNode(outputId); created != nullptr && !created->positionValid) {
        float maxX = 0.0f, minY = 0.0f;
        bool any = false;
        for (const graph::Node& other : m_graph.Nodes()) {
            if (!other.positionValid || other.id == outputId || other.id == nodeId) continue;
            maxX = any ? std::max(maxX, other.posX) : other.posX;
            minY = any ? std::min(minY, other.posY) : other.posY;
            any = true;
        }
        created->posX = maxX + 640.0f;
        created->posY = minY;
        created->positionValid = true;
    }
    const graph::Node* output = m_graph.FindNode(outputId);
    if (output->inputs.empty()) return nodeId;
    const graph::GraphId outputInput = output->inputs.front().id;
    const graph::GraphId modelOutput = m_graph.FindNode(nodeId)->outputs.front().id;
    // エディタ上の置き場所。Mesh Output の左下（Merge を足したらさらに左）へずらす。
    const float baseX = output->posX - 320.0f;
    const float baseY = output->posY + 160.0f;
    const auto placeNear = [&](graph::GraphId id, float dx, float dy) {
        if (graph::Node* n = m_graph.FindMutableNode(id); n != nullptr) {
            n->posX = baseX + dx;
            n->posY = baseY + dy;
            n->positionValid = true;
        }
    };
    const graph::Node* upstream = m_graph.FindUpstreamNodeForPin(outputInput);
    if (upstream == nullptr) {
        m_graph.CreateLink(modelOutput, outputInput);
        placeNear(nodeId, 0.0f, 0.0f);
    } else if (upstream->kind == graph::NodeKind::Merge) {
        const graph::GraphId mergeId = upstream->id;
        m_graph.NormalizeVariablePins();
        for (const graph::Pin& pin : m_graph.FindNode(mergeId)->inputs) {
            if (m_graph.FindUpstreamNodeForPin(pin.id) != nullptr) continue;
            m_graph.CreateLink(modelOutput, pin.id);
            break;
        }
        m_graph.NormalizeVariablePins();
        const graph::Node* merge = m_graph.FindNode(mergeId);
        if (graph::Node* self = m_graph.FindMutableNode(nodeId); self != nullptr && merge->positionValid) {
            self->posX = merge->posX - 320.0f;
            self->posY = merge->posY + 100.0f * static_cast<float>(merge->inputs.size() - 1);
            self->positionValid = true;
        }
    } else {
        // 今繋がっている出力ピンを Merge の Input 1 へ、新しいモデルを Input 2 へ。Merge を Mesh Output へ。
        graph::GraphId previousPin = 0;
        for (const graph::Link& link : m_graph.Links())
            if (link.endPin == outputInput) previousPin = link.startPin;
        const graph::GraphId mergeId = m_graph.CreateNode(graph::NodeKind::Merge);
        m_graphNodesToPlace.push_back(mergeId);
        m_graph.CreateLink(previousPin, m_graph.FindNode(mergeId)->inputs.front().id);
        m_graph.NormalizeVariablePins();
        m_graph.CreateLink(modelOutput, m_graph.FindNode(mergeId)->inputs.back().id);
        m_graph.NormalizeVariablePins();
        m_graph.CreateLink(m_graph.FindNode(mergeId)->outputs.front().id, outputInput);
        placeNear(mergeId, 0.0f, 0.0f);
        placeNear(nodeId, -320.0f, 180.0f);
    }
    m_graph.MarkDirty();
    m_selectedGraphNode = nodeId;
    m_graphSelectionRequest = nodeId;
    m_meshHighlight.selected.clear();
    MarkDocumentChanged();
    ROCK_LOG_INFO("Model ノードを追加しました: %s", FindModel(modelId)->name.c_str());
    return nodeId;
}

std::vector<Application::VisibleModel> Application::CollectVisibleModels() const {
    std::vector<VisibleModel> result;
    const graph::Node* preview = m_graph.FindNode(m_previewGraphNode);
    const auto paths = graph::CollectOutputModels(
        m_graph, preview != nullptr && graph::IsPreviewableNodeKind(preview->kind) ? preview->id : 0);
    const auto transformOf = [&](graph::GraphId id) {
        const graph::Node* node = m_graph.FindNode(id);
        const auto* settings = node ? std::get_if<graph::TransformNodeSettings>(&node->settings) : nullptr;
        return settings ? renderer::NodeTransformMatrix(settings->position, settings->rotationDegrees, settings->scale)
                        : XMMatrixIdentity();
    };
    for (const auto& path : paths) {
        const graph::Node* node = m_graph.FindNode(path.model);
        const auto* settings = node ? std::get_if<graph::ModelNodeSettings>(&node->settings) : nullptr;
        if (settings == nullptr || settings->model == 0) continue;
        const auto model = std::find_if(m_models.begin(), m_models.end(),
                                        [&](const renderer::ModelAsset& m) { return m.id == settings->model; });
        if (model == m_models.end() || !model->geometry) continue;
        XMMATRIX world = renderer::ModelPivotMatrix(*model) *
                         renderer::NodeTransformMatrix(settings->position, settings->rotationDegrees, settings->scale);
        for (const graph::GraphId transform : path.transforms) world = world * transformOf(transform);
        VisibleModel visible;
        visible.node = path.model;
        visible.transforms = path.transforms;
        visible.model = &*model;
        visible.rotations = &settings->nodeRotations;
        XMStoreFloat4x4(&visible.world, world);
        result.push_back(std::move(visible));
    }
    return result;
}

bool Application::NodeTransform(graph::GraphId nodeId, NodeTransformRef& out) {
    graph::Node* node = m_graph.FindMutableNode(nodeId);
    if (node == nullptr) return false;
    if (auto* model = std::get_if<graph::ModelNodeSettings>(&node->settings)) {
        out = {model->position, model->rotationDegrees, &model->scale};
        return true;
    }
    if (auto* transform = std::get_if<graph::TransformNodeSettings>(&node->settings)) {
        out = {transform->position, transform->rotationDegrees, &transform->scale};
        return true;
    }
    if (auto* volume = std::get_if<geometry::VolumeTransformSettings>(&node->settings)) {
        // ボリュームは設定を変えるたびに格子を作り直すので、再評価が要る。
        out = {volume->position.data(), volume->rotationDegrees.data(), &volume->scale, true};
        return true;
    }
    if (auto* pieces = std::get_if<geometry::PieceTransformSettings>(&node->settings)) {
        auto* pose = &pieces->pose;
        if (m_pieceGizmoId >= 0) {
            auto it = std::find_if(pieces->overrides.begin(), pieces->overrides.end(),
                                   [&](const auto& p) { return p.id == uint32_t(m_pieceGizmoId); });
            if (it == pieces->overrides.end()) return false;
            pose = &it->pose;
        }
        out = {pose->position.data(), pose->rotation.data(), pose->scale.data(), true, pose->scale.data()};
        return true;
    }
    return false;
}

bool Application::RockMeshUsesNode(graph::GraphId nodeId) const {
    if (nodeId == 0) return false;
    std::vector<graph::GraphId> pending;
    std::unordered_set<graph::GraphId> seen;
    for (const auto& ref : m_rockMeshReferences) pending.push_back(ref.source);
    while (!pending.empty()) {
        const graph::GraphId id = pending.back();
        pending.pop_back();
        if (id == nodeId) return true;
        if (!seen.insert(id).second) continue;
        const graph::Node* node = m_graph.FindNode(id);
        if (node == nullptr) continue;
        for (const auto& pin : node->inputs)
            if (const graph::Node* upstream = m_graph.FindUpstreamNodeForPin(pin.id))
                pending.push_back(upstream->id);
    }
    return false;
}

bool Application::NodeGizmoFrame(graph::GraphId nodeId, XMFLOAT3& pivot, XMFLOAT4X4& parent) const {
    const graph::Node* node = m_graph.FindNode(nodeId);
    if (node && node->kind == graph::NodeKind::PieceTransform) {
        if (m_modelInstanceDrag.pending && m_modelInstanceDrag.node == nodeId) {
            pivot = m_modelInstanceDrag.pivot; parent = m_modelInstanceDrag.parent; return true;
        }
        if (m_pieceUpdating || !m_pieceInput || m_pieceInputNode != nodeId ||
            !m_meshGraphError.empty() || m_previewGraphNode != nodeId) return false;
        const auto& settings = std::get<geometry::PieceTransformSettings>(node->settings);
        const auto eligible = [&](uint32_t id) { return !m_pieceTransformSelection ||
            std::find(m_pieceTransformSelection->ids.begin(), m_pieceTransformSelection->ids.end(), id) != m_pieceTransformSelection->ids.end(); };
        XMStoreFloat4x4(&parent, XMMatrixIdentity());
        if (m_pieceGizmoId >= 0) {
            if (!eligible(uint32_t(m_pieceGizmoId))) return false;
            std::string error;
            const auto output = geometry::TransformPieces(*m_pieceInput, m_pieceTransformSelection.get(), settings, error);
            if (!error.empty()) return false;
            for (const auto& piece : output.pieces) if (piece.id == uint32_t(m_pieceGizmoId)) {
                const auto center = geometry::PieceCenter(piece); pivot = {center.x, center.y, center.z};
                const auto& p = settings.pose;
                XMStoreFloat4x4(&parent, XMMatrixScaling(p.scale[0], p.scale[1], p.scale[2]) *
                    XMMatrixRotationRollPitchYaw(XMConvertToRadians(p.rotation[0]), XMConvertToRadians(p.rotation[1]), XMConvertToRadians(p.rotation[2])));
                return true;
            }
            return false;
        }
        double x=0,y=0,z=0,total=0;
        for (const auto& piece : m_pieceInput->pieces) if (eligible(piece.id)) {
            const auto center = geometry::PieceCenter(piece);
            const auto& m = piece.transform;
            const auto weight = piece.volume * (m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]));
            x += center.x*weight; y += center.y*weight; z += center.z*weight; total += weight;
        }
        if (total <= 0) return false;
        pivot = {float(x/total)+settings.pose.position[0], float(y/total)+settings.pose.position[1], float(z/total)+settings.pose.position[2]};
        return true;
    }
    if (node && node->kind == graph::NodeKind::VolumeTransform) {
        // ボリュームは倍率 → 回転 → 移動の順に原点まわりで動かすので、移動量がそのまま
        // 回転・倍率の中心になる。表示中の岩がこのノードを通っているときだけ出す。
        const auto* settings = std::get_if<geometry::VolumeTransformSettings>(&node->settings);
        if (settings == nullptr || !m_meshGraphError.empty() || !m_meshGraphActive) return false;
        if (!RockMeshUsesNode(nodeId)) return false;
        pivot = {settings->position[0], settings->position[1], settings->position[2]};
        XMStoreFloat4x4(&parent, XMMatrixIdentity());
        return true;
    }
    if (node == nullptr || (node->kind != graph::NodeKind::Model && node->kind != graph::NodeKind::Transform)) return false;
    const float* position = nullptr;
    if (const auto* model = std::get_if<graph::ModelNodeSettings>(&node->settings)) position = model->position;
    if (const auto* transform = std::get_if<graph::TransformNodeSettings>(&node->settings)) position = transform->position;
    if (position == nullptr) return false;
    // ビューポートに出ている経路のうち最初のもので、そのノードより出力側の Transform をまとめる。
    for (const VisibleModel& visible : CollectVisibleModels()) {
        size_t first = 0;
        if (visible.node == nodeId) {
            first = 0;
        } else {
            const auto found = std::find(visible.transforms.begin(), visible.transforms.end(), nodeId);
            if (found == visible.transforms.end()) continue;
            first = static_cast<size_t>(found - visible.transforms.begin()) + 1;
        }
        XMMATRIX matrix = XMMatrixIdentity();
        for (size_t i = first; i < visible.transforms.size(); ++i) {
            const graph::Node* transform = m_graph.FindNode(visible.transforms[i]);
            const auto* settings = transform ? std::get_if<graph::TransformNodeSettings>(&transform->settings) : nullptr;
            if (settings)
                matrix = matrix * renderer::NodeTransformMatrix(settings->position, settings->rotationDegrees, settings->scale);
        }
        XMStoreFloat4x4(&parent, matrix);
        XMStoreFloat3(&pivot, XMVector3TransformCoord(XMVectorSet(position[0], position[1], position[2], 1.0f), matrix));
        return true;
    }
    return false;
}

bool Application::ModelNodeGizmoFrame(const std::vector<VisibleModel>& visible, ModelNodeGizmo& out) {
    if (!m_modelNodeGizmo) return false;
    graph::Node* node = m_graph.FindMutableNode(m_selectedGraphNode);
    auto* settings = node ? std::get_if<graph::ModelNodeSettings>(&node->settings) : nullptr;
    if (settings == nullptr) return false;
    const auto found = std::find_if(visible.begin(), visible.end(),
                                    [&](const VisibleModel& v) { return v.node == m_selectedGraphNode; });
    if (found == visible.end() || !found->model->geometry) return false;
    const auto& nodes = found->model->geometry->nodes;
    const auto index = std::find_if(nodes.begin(), nodes.end(),
                                    [&](const renderer::ModelNode& n) { return n.name == m_selectedModelNodeName; });
    if (nodes.size() < 2 || index == nodes.end()) return false;
    out.settings = settings;
    out.visible = &*found;
    out.node = static_cast<size_t>(index - nodes.begin());
    // ノード自身の回転を外した姿勢で、原点と回転の軸を求める。軸はモデルの軸を読んだままの姿勢のノードの座標へ
    // 移し（A⁻¹）、今の姿勢（親の回転と置き方）で運んだもの（ModelNodeWorlds の A · R · A⁻¹ と同じ基準）。
    std::vector<renderer::ModelNodeRotation> others;
    for (const auto& rotation : settings->nodeRotations)
        if (rotation.node != m_selectedModelNodeName) others.push_back(rotation);
    std::vector<XMFLOAT4X4> worlds, bindWorlds;
    renderer::ModelNodeWorlds(*found->model->geometry, others, worlds);
    renderer::ModelNodeWorlds(*found->model->geometry, {}, bindWorlds);
    const XMMATRIX current = XMLoadFloat4x4(&worlds[out.node]) * XMLoadFloat4x4(&found->world);
    XMVECTOR scale, orientation, translation;
    if (!XMMatrixDecompose(&scale, &orientation, &translation, XMLoadFloat4x4(&bindWorlds[out.node])))
        orientation = XMQuaternionIdentity();
    const XMMATRIX toNode = XMMatrixTranspose(XMMatrixRotationQuaternion(orientation));
    XMStoreFloat3(&out.origin, XMVector3TransformCoord(XMVectorZero(), current));
    for (int axis = 0; axis < 3; ++axis) {
        const XMVECTOR local = XMVector3TransformNormal(XMLoadFloat3(&kAxes[axis]), toNode);
        XMStoreFloat3(&out.axes[axis], XMVector3Normalize(XMVector3TransformNormal(local, current)));
    }
    return true;
}

float Application::ModelInstancesRadius() const {
    float radius = 0.0f;
    for (const VisibleModel& visible : CollectVisibleModels()) {
        BoundingBox bounds;
        if (!renderer::ModelWorldBounds(*visible.model, XMLoadFloat4x4(&visible.world), bounds)) continue;
        const float extent = XMVectorGetX(XMVector3Length(XMLoadFloat3(&bounds.Extents)));
        radius = std::max(radius, XMVectorGetX(XMVector3Length(XMLoadFloat3(&bounds.Center))) + extent);
    }
    return radius;
}

void Application::DrawSceneModels(ID3D12GraphicsCommandList* commandList, const renderer::SceneDrawContext& context) {
    // 同じモデルはまとめて描く（メッシュとパイプラインの切り替えを減らす）。
    std::unordered_map<uint64_t, std::vector<renderer::ModelInstanceDraw>> worlds;
    for (const VisibleModel& visible : CollectVisibleModels())
        worlds[visible.model->id].push_back({visible.world, visible.rotations});
    for (const auto& [modelId, list] : worlds) {
        const auto preview = m_modelPreviews.find(modelId);
        const renderer::ModelAsset* model = FindModel(modelId);
        if (preview == m_modelPreviews.end() || model == nullptr) continue;
        preview->second->RenderInScene(m_device, m_pipelineCache, commandList, *model, m_materialLibrary,
                                       m_textureLibrary, context, list);
    }
}

graph::GraphId Application::PickModelNode(const XMFLOAT3& origin, const XMFLOAT3& direction, float& distance,
                                         int* modelNode) const {
    graph::GraphId hit = 0;
    if (modelNode != nullptr) *modelNode = -1;
    distance = FLT_MAX;
    const XMVECTOR rayOrigin = XMLoadFloat3(&origin);
    const XMVECTOR rayDirection = XMLoadFloat3(&direction);
    for (const VisibleModel& visible : CollectVisibleModels()) {
        const XMMATRIX world = XMLoadFloat4x4(&visible.world);
        BoundingBox bounds;
        float boxDistance = 0.0f;
        if (!renderer::ModelWorldBounds(*visible.model, world, bounds) ||
            !bounds.Intersects(rayOrigin, rayDirection, boxDistance) || boxDistance >= distance) continue;
        // 三角形は部品（ノード）の座標で調べ、当たった点をワールドへ戻して距離を比べる。
        std::vector<XMFLOAT4X4> nodeWorlds;
        renderer::ModelNodeWorlds(*visible.model->geometry,
                                  visible.rotations ? *visible.rotations : std::vector<renderer::ModelNodeRotation>{},
                                  nodeWorlds);
        for (const auto& part : visible.model->geometry->lods[0].parts) {
            const XMMATRIX partWorld =
                part.node < nodeWorlds.size() ? XMLoadFloat4x4(&nodeWorlds[part.node]) * world : world;
            XMVECTOR determinant;
            const XMMATRIX inverse = XMMatrixInverse(&determinant, partWorld);
            if (XMVectorGetX(determinant) == 0.0f) continue;
            const XMVECTOR localOrigin = XMVector3TransformCoord(rayOrigin, inverse);
            const XMVECTOR localDirection = XMVector3Normalize(XMVector3TransformNormal(rayDirection, inverse));
            const auto& vertices = part.mesh.vertices;
            const auto& indices = part.mesh.indices;
            for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                float localDistance = 0.0f;
                if (!TriangleTests::Intersects(localOrigin, localDirection, XMLoadFloat3(&vertices[indices[t]].position),
                                               XMLoadFloat3(&vertices[indices[t + 1]].position),
                                               XMLoadFloat3(&vertices[indices[t + 2]].position), localDistance)) continue;
                const XMVECTOR point = XMVector3TransformCoord(
                    XMVectorAdd(localOrigin, XMVectorScale(localDirection, localDistance)), partWorld);
                const float worldDistance = XMVectorGetX(XMVector3Length(XMVectorSubtract(point, rayOrigin)));
                if (worldDistance < distance) {
                    distance = worldDistance;
                    hit = visible.node;
                    if (modelNode != nullptr) *modelNode = static_cast<int>(part.node);
                }
            }
        }
    }
    return hit;
}

bool Application::PickGround(const ImVec2& mouse, const ImVec2& viewportMin, const ImVec2& viewportMax, float planeY,
                             bool useMeshes, XMFLOAT3& point) const {
    XMFLOAT3 origin, direction;
    if (!ViewportRay(mouse, viewportMin, viewportMax, origin, direction)) return false;
    const XMVECTOR rayOrigin = XMLoadFloat3(&origin);
    const XMVECTOR rayDirection = XMLoadFloat3(&direction);
    float nearest = FLT_MAX;
    if (useMeshes && m_renderer.HasMeshScene()) {
        // メッシュの面の上へ置く（変位前の形。薄い帯も面として扱う）。
        for (const auto& mesh : m_renderer.Scene().meshes) {
            if (mesh.materialOnly) continue;
            const auto& vertices = mesh.geometry.vertices;
            const auto& indices = mesh.geometry.indices;
            for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                float distance = 0.0f;
                if (TriangleTests::Intersects(rayOrigin, rayDirection, XMLoadFloat3(&vertices[indices[t]].position),
                                              XMLoadFloat3(&vertices[indices[t + 1]].position),
                                              XMLoadFloat3(&vertices[indices[t + 2]].position), distance) &&
                    distance < nearest) nearest = distance;
            }
        }
    }
    if (nearest == FLT_MAX) {
        // 水平面 y = planeY。上から見下ろす向きでなければ当たらない（遠すぎる交点も捨てる）。
        if (std::abs(direction.y) < 1e-5f) return false;
        const float t = (planeY - origin.y) / direction.y;
        if (t <= 0.0f || t > 10000.0f) return false;
        nearest = t;
    }
    XMStoreFloat3(&point, XMVectorAdd(rayOrigin, XMVectorScale(rayDirection, nearest)));
    return true;
}

bool Application::HandleModelInstanceInput(bool itemActive, bool itemHovered, const ImVec2& viewportMin,
                                           const ImVec2& viewportMax) {
    (void)itemActive;
    auto& drag = m_modelInstanceDrag;
    const ImGuiIO& io = ImGui::GetIO();
    m_hoveredModelNode = 0;
    m_modelGizmoHover = -1;
    XMFLOAT3 rayOrigin, rayDirection;
    const bool hasRay = ViewportRay(io.MousePos, viewportMin, viewportMax, rayOrigin, rayDirection);

    // --- 掴んでいる間 -------------------------------------------------------------
    if (drag.pending) {
        NodeTransformRef target;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !NodeTransform(drag.node, target)) {
            drag = {};
            return true;
        }
        const auto documentChanged = [&]() {
            m_documentDirty = true;
            if (target.regenerate) m_graph.MarkDirty();
        };
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            // 掴む前の値へ戻す。
            if (drag.dragging && drag.nodeRotation) {
                graph::Node* node = m_graph.FindMutableNode(drag.node);
                if (auto* settings = node ? std::get_if<graph::ModelNodeSettings>(&node->settings) : nullptr)
                    SetModelNodeRotation(*settings, drag.modelNodeName, drag.startNodeRotation);
                documentChanged();
            } else if (drag.dragging) {
                std::copy(std::begin(drag.startPosition), std::end(drag.startPosition), target.position);
                std::copy(std::begin(drag.startRotation), std::end(drag.startRotation), target.rotation);
                if (target.scale) *target.scale = drag.startScale;
                if (target.scaleXYZ) std::copy(std::begin(drag.startScaleXYZ), std::end(drag.startScaleXYZ), target.scaleXYZ);
                documentChanged();
            }
            drag = {};
            return true;
        }
        drag.dragging |= ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, ui::Scaled(3.0f));
        m_modelGizmoHover = drag.handle;
        m_hoveredModelNode = drag.handle < 0 ? drag.node : 0;
        if (!drag.dragging || !hasRay) return true;
        if (drag.nodeRotation) {
            // ノード用の輪。画面上でノードの原点のまわりに回した角度（モデルの輪と同じ式）を、掴んだときの回転に足す。
            // ノードの回転 S はモデルの軸の基準で掛かるので、軸 k まわりの Ra を後ろから掛ける（S' = S · Ra）。
            graph::Node* node = m_graph.FindMutableNode(drag.node);
            auto* settings = node ? std::get_if<graph::ModelNodeSettings>(&node->settings) : nullptr;
            if (settings == nullptr) return true;
            const auto& camera = m_renderer.GetCamera();
            const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
            const ProjectedPoint center = ProjectToViewport(camera.ViewMatrix() * camera.ProjectionMatrix(), drag.pivot,
                                                            viewportMin, size);
            if (!center.visible) return true;
            const int axisIndex = drag.handle - kHandleRing;
            const float angleNow = std::atan2(io.MousePos.y - center.screen.y, io.MousePos.x - center.screen.x);
            const float anglePress = std::atan2(drag.pressPos.y - center.screen.y, drag.pressPos.x - center.screen.x);
            const XMVECTOR worldAxis = XMLoadFloat3(&drag.nodeAxes[axisIndex]);
            const XMFLOAT3 eye = camera.Position();
            const float facing =
                XMVectorGetX(XMVector3Dot(worldAxis, XMVectorSubtract(XMLoadFloat3(&eye), XMLoadFloat3(&drag.pivot)))) >= 0.0f
                    ? 1.0f : -1.0f;
            float angle = -(angleNow - anglePress) * facing;
            if (io.KeyCtrl) angle = std::round(angle / XMConvertToRadians(15.0f)) * XMConvertToRadians(15.0f);
            const float zero[3] = {0.0f, 0.0f, 0.0f};
            const XMMATRIX start = renderer::NodeTransformMatrix(zero, drag.startNodeRotation, 1.0f);
            const XMMATRIX rotated = start * XMMatrixRotationAxis(XMLoadFloat3(&kAxes[axisIndex]), angle);
            float degrees[3];
            renderer::RotationToDegrees(rotated, degrees);
            // 0 付近は 0 に揃える（すべて 0 なら項目を消す）。
            for (float& value : degrees)
                if (std::abs(value) < 1e-3f) value = 0.0f;
            if (SetModelNodeRotation(*settings, drag.modelNodeName, degrees)) documentChanged();
            return true;
        }
        const XMMATRIX parent = XMLoadFloat4x4(&drag.parent);
        const XMVECTOR pivot = XMLoadFloat3(&drag.pivot);
        // ワールドでの移動量を、ノードの親（下流の Transform）の座標へ戻して足す。
        const auto applyTranslation = [&](FXMVECTOR worldDelta) {
            XMVECTOR determinant;
            const XMMATRIX inverse = XMMatrixInverse(&determinant, parent);
            if (XMVectorGetX(determinant) == 0.0f) return;
            XMFLOAT3 local;
            XMStoreFloat3(&local, XMVector3TransformNormal(worldDelta, inverse));
            const float next[3] = {drag.startPosition[0] + local.x, drag.startPosition[1] + local.y,
                                   drag.startPosition[2] + local.z};
            if (!std::equal(std::begin(next), std::end(next), target.position)) {
                std::copy(std::begin(next), std::end(next), target.position);
                documentChanged();
            }
        };
        if (drag.handle >= kHandleAxis && drag.handle < kHandlePlane) {
            const XMVECTOR axis = XMLoadFloat3(&kAxes[drag.handle - kHandleAxis]);
            float t = 0.0f;
            if (ClosestOnLine(rayOrigin, rayDirection, pivot, axis, t))
                applyTranslation(XMVectorScale(axis, t - drag.pressParameter));
        } else if (drag.handle < 0 || (drag.handle >= kHandlePlane && drag.handle < kHandleRing)) {
            // 本体のドラッグは水平面（法線 Y）のハンドルと同じ。
            const int axisIndex = drag.handle < 0 ? 1 : drag.handle - kHandlePlane;
            XMFLOAT3 hit;
            if (IntersectPlane(rayOrigin, rayDirection, pivot, XMLoadFloat3(&kAxes[axisIndex]), hit))
                applyTranslation(XMVectorSubtract(XMLoadFloat3(&hit), XMLoadFloat3(&drag.pressPoint)));
        } else if (drag.handle >= kHandleScale) {
            if (!target.scale) return true;
            // 軸は、ピボットから掴んだ点までの長さとの比。中心の四角は、右か上へ動かすほど大きくなる。
            float factor = 1.0f;
            if (drag.handle == kHandleScaleAll) {
                const float pixels = (io.MousePos.x - drag.pressPos.x) - (io.MousePos.y - drag.pressPos.y);
                factor = std::exp(pixels / ui::Scaled(kGizmoLengthPixels));
            } else {
                float t = 0.0f;
                if (!ClosestOnLine(rayOrigin, rayDirection, pivot, XMLoadFloat3(&kAxes[drag.handle - kHandleScale]), t) ||
                    std::abs(drag.pressParameter) < 1e-6f) return true;
                factor = t / drag.pressParameter;
            }
            float scale = std::clamp(drag.startScale * factor, 0.01f, 100.0f);
            // Ctrl で 0.1 刻み。
            if (io.KeyCtrl) scale = std::max(0.1f, std::round(scale * 10.0f) / 10.0f);
            if (scale != *target.scale) {
                if (target.scaleXYZ) {
                    const float ratio = scale / drag.startScale;
                    for (int axis=0; axis<3; ++axis) target.scaleXYZ[axis] = drag.startScaleXYZ[axis] * ratio;
                } else *target.scale = scale;
                documentChanged();
            }
        } else if (drag.handle >= kHandleRing) {
            // 画面上でピボットのまわりに回した角度。軸がカメラを向いていれば反時計回りが正（右手系）。
            const auto& camera = m_renderer.GetCamera();
            const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
            const ProjectedPoint center = ProjectToViewport(camera.ViewMatrix() * camera.ProjectionMatrix(), drag.pivot,
                                                            viewportMin, size);
            if (!center.visible) return true;
            const float angleNow = std::atan2(io.MousePos.y - center.screen.y, io.MousePos.x - center.screen.x);
            const float anglePress = std::atan2(drag.pressPos.y - center.screen.y, drag.pressPos.x - center.screen.x);
            const XMVECTOR axis = XMLoadFloat3(&kAxes[drag.handle - kHandleRing]);
            const XMFLOAT3 eye = camera.Position();
            const float facing = XMVectorGetX(XMVector3Dot(axis, XMVectorSubtract(XMLoadFloat3(&eye), pivot))) >= 0.0f ? 1.0f : -1.0f;
            float angle = -(angleNow - anglePress) * facing;
            // Ctrl で 15 度刻み。
            if (io.KeyCtrl) angle = std::round(angle / XMConvertToRadians(15.0f)) * XMConvertToRadians(15.0f);
            // ワールドの軸まわりに回す。ノードの回転 R は親の回転 P の手前に掛かるので、R' = R · P · Ra · P⁻¹。
            const XMMATRIX parentRotation = RotationPart(parent);
            const XMMATRIX start = XMMatrixRotationRollPitchYaw(XMConvertToRadians(drag.startRotation[0]),
                                                                XMConvertToRadians(drag.startRotation[1]),
                                                                XMConvertToRadians(drag.startRotation[2]));
            const XMMATRIX rotated = start * parentRotation * XMMatrixRotationAxis(axis, angle) *
                                     XMMatrixTranspose(parentRotation);
            float degrees[3];
            renderer::RotationToDegrees(rotated, degrees);
            if (!std::equal(std::begin(degrees), std::end(degrees), target.rotation)) {
                std::copy(std::begin(degrees), std::end(degrees), target.rotation);
                documentChanged();
            }
        }
        return true;
    }

    // --- ギズモ -------------------------------------------------------------------
    NodeTransformRef selected;
    XMFLOAT3 pivot{};
    XMFLOAT4X4 parent{};
    const bool hasGizmo = NodeTransform(m_selectedGraphNode, selected) && NodeGizmoFrame(m_selectedGraphNode, pivot, parent);
    if (hasGizmo && !selected.scale && m_modelGizmoMode == ModelGizmoMode::Scale)
        m_modelGizmoMode = ModelGizmoMode::Translate;
    if (itemHovered && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && hasGizmo) {
        // W / E / R はモデルのギズモ。ノード用のギズモから戻る。
        const auto choose = [&](ModelGizmoMode mode) {
            m_modelGizmoMode = mode;
            m_modelNodeGizmo = false;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) choose(ModelGizmoMode::Translate);
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) choose(ModelGizmoMode::Rotate);
        if (selected.scale && ImGui::IsKeyPressed(ImGuiKey_R, false)) choose(ModelGizmoMode::Scale);
    }
    const std::vector<VisibleModel> visibleModels = CollectVisibleModels();
    ModelNodeGizmo nodeGizmo;
    const bool hasNodeGizmo = hasGizmo && ModelNodeGizmoFrame(visibleModels, nodeGizmo);
    if (hasNodeGizmo && itemHovered) {
        const GizmoScreen gizmo = BuildGizmo(m_renderer.GetCamera(), nodeGizmo.origin, viewportMin, viewportMax, nodeGizmo.axes);
        m_modelGizmoHover = HitGizmo(gizmo, GizmoKind::Rotate, io.MousePos);
    } else if (hasGizmo && itemHovered) {
        const GizmoScreen gizmo = BuildGizmo(m_renderer.GetCamera(), pivot, viewportMin, viewportMax);
        m_modelGizmoHover = HitGizmo(gizmo, static_cast<GizmoKind>(m_modelGizmoMode), io.MousePos);
    }
    const auto beginDrag = [&](graph::GraphId nodeId, int handle) {
        NodeTransformRef target;
        XMFLOAT3 framePivot{};
        XMFLOAT4X4 frameParent{};
        if (!NodeTransform(nodeId, target) || !NodeGizmoFrame(nodeId, framePivot, frameParent)) return false;
        drag = {};
        drag.pending = true;
        drag.handle = handle;
        drag.node = nodeId;
        drag.pressPos = io.MousePos;
        drag.pivot = framePivot;
        drag.parent = frameParent;
        std::copy(target.position, target.position + 3, drag.startPosition);
        std::copy(target.rotation, target.rotation + 3, drag.startRotation);
        drag.startScale = target.scale ? *target.scale : 1.0f;
        if (target.scaleXYZ) std::copy(target.scaleXYZ, target.scaleXYZ+3, drag.startScaleXYZ);
        const XMVECTOR p = XMLoadFloat3(&framePivot);
        if (handle >= kHandleAxis && handle < kHandlePlane) {
            ClosestOnLine(rayOrigin, rayDirection, p, XMLoadFloat3(&kAxes[handle - kHandleAxis]), drag.pressParameter);
        } else if (handle >= kHandleScale && handle < kHandleScaleAll) {
            ClosestOnLine(rayOrigin, rayDirection, p, XMLoadFloat3(&kAxes[handle - kHandleScale]), drag.pressParameter);
        } else if (handle < 0 || (handle >= kHandlePlane && handle < kHandleRing)) {
            const int axisIndex = handle < 0 ? 1 : handle - kHandlePlane;
            if (!IntersectPlane(rayOrigin, rayDirection, p, XMLoadFloat3(&kAxes[axisIndex]), drag.pressPoint)) {
                drag = {};
                return false;
            }
        }
        return true;
    };

    m_hoveredModelNodeName.clear();
    if (!itemHovered || !hasRay) return false;
    int hoveredPart = -1;
    if (m_modelGizmoHover < 0) {
        float distance = 0.0f;
        m_hoveredModelNode = PickModelNode(rayOrigin, rayDirection, distance, &hoveredPart);
        // ノード用のギズモでは、選んでいるモデルの部品にカーソルが乗ったらそのノードを示す。
        if (hasNodeGizmo && m_hoveredModelNode == m_selectedGraphNode && hoveredPart >= 0)
            m_hoveredModelNodeName = nodeGizmo.visible->model->geometry->nodes[static_cast<size_t>(hoveredPart)].name;
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (m_modelGizmoHover >= 0 && hasNodeGizmo) {
            // ノードの輪を掴む。
            drag = {};
            drag.pending = true;
            drag.handle = m_modelGizmoHover;
            drag.node = m_selectedGraphNode;
            drag.pressPos = io.MousePos;
            drag.pivot = nodeGizmo.origin;
            drag.nodeRotation = true;
            drag.modelNodeName = m_selectedModelNodeName;
            std::copy(std::begin(nodeGizmo.axes), std::end(nodeGizmo.axes), drag.nodeAxes);
            for (const auto& rotation : nodeGizmo.settings->nodeRotations)
                if (rotation.node == m_selectedModelNodeName)
                    std::copy(std::begin(rotation.rotationDegrees), std::end(rotation.rotationDegrees), drag.startNodeRotation);
            return true;
        }
        if (m_modelGizmoHover >= 0) {
            beginDrag(m_selectedGraphNode, m_modelGizmoHover);
            return true;
        }
        if (!m_hoveredModelNodeName.empty()) {
            // ノード用のギズモでは、部品のクリックはそのノードを選ぶ（モデル全体は動かさない）。
            m_selectedModelNodeName = m_hoveredModelNodeName;
            return true;
        }
        if (m_hoveredModelNode == 0) {
            // モデルを選んでいたら外す（ほかのノードの選択はそのまま）。空やメッシュのクリックはメッシュの選択へ渡す。
            const graph::Node* node = m_graph.FindNode(m_selectedGraphNode);
            if (node != nullptr && graph::IsModelNodeKind(node->kind)) {
                m_selectedGraphNode = 0;
                m_graphSelectionRequest = 0;
            }
            return false;
        }
        m_selectedGraphNode = m_hoveredModelNode;
        m_graphSelectionRequest = m_hoveredModelNode;
        m_meshHighlight.selected.clear();
        // 本体を掴んだら水平に動かす。
        beginDrag(m_hoveredModelNode, -1);
        return true;
    }
    if (hasGizmo && !io.WantTextInput) {
        const graph::Node* selectedNode = m_graph.FindNode(m_selectedGraphNode);
        // Delete はモデルの系統のノードだけ。岩の枝はグラフパネルから消す。
        if (selectedNode && graph::IsModelNodeKind(selectedNode->kind) &&
            ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            // ノードごと消す（グラフのノードの削除と同じ。アンドゥで戻る）。
            m_graph.DeleteNode(m_selectedGraphNode);
            m_graph.NormalizeVariablePins();
            m_graph.MarkDirty();
            m_selectedGraphNode = 0;
            m_graphSelectionRequest = 0;
            MarkDocumentChanged();
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_selectedGraphNode = 0;
            m_graphSelectionRequest = 0;
        }
    }
    return m_hoveredModelNode != 0 || m_modelGizmoHover >= 0;
}

void Application::AppendPlaneCutOverlay(std::vector<renderer::OverlayLineSet>& lines) {
    if (!m_planeCutsShowFrames && !m_planeCutsColorFaces) return;
    const graph::Node* node = m_graph.FindNode(m_selectedGraphNode);
    if (!node || node->kind != graph::NodeKind::PlaneCuts) return;
    // 評価のキャッシュに残る、このノードの結果から読む。評価中は前回の結果を出し続ける。
    const auto found = m_rockEvaluationCache.entries.find(node->id);
    if (found == m_rockEvaluationCache.entries.end() || found->second.result.rocks.empty()) return;
    const auto& guide = found->second.result.rocks[0].planeCuts;
    if (!guide) return;
    // 平面ごとに色相を変える（ピースの色分けと同じ黄金比の刻み）。枠と断面で同じ色にする。
    const auto planeColor = [](uint32_t plane, float alpha) {
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(std::fmod(float(plane) * .618034f, .999f), .65f, 1.f, r, g, b);
        return XMFLOAT4{r, g, b, alpha};
    };
    if (m_planeCutsColorFaces) {
        if (m_cutFaceOverlay.node != node->id || m_cutFaceOverlay.guide != guide ||
            m_cutFaceOverlay.stamp != m_rockPreviewStamp) {
            m_cutFaceOverlay = {node->id, guide, m_rockPreviewStamp, {}};
            // 表示中のメッシュの面を平面ごとに振り分ける。下流で形が変わった面は、どの平面にも乗らない。
            std::map<int, size_t> setOf;
            // 面と同じ位置に描くとちらつくので、平面の外側へ少し浮かせる。
            const float lift = guide->spacing * .2f;
            for (const auto& mesh : m_rockPreviewSurfaces) {
                const auto assigned = geometry::CutFaceAssignments(mesh, *guide);
                for (size_t f = 0; f < assigned.size(); ++f) {
                    if (assigned[f] < 0) continue;
                    auto [entry, added] = setOf.try_emplace(assigned[f], m_cutFaceOverlay.sets.size());
                    if (added) {
                        renderer::OverlayLineSet set{planeColor(uint32_t(assigned[f]), .45f), {}};
                        set.triangles = true;
                        m_cutFaceOverlay.sets.push_back(std::move(set));
                    }
                    const auto n = guide->planes[size_t(assigned[f])].normal;
                    auto& points = m_cutFaceOverlay.sets[entry->second].points;
                    for (uint32_t v : mesh.triangles[f]) {
                        const auto& p = mesh.positions[v];
                        points.push_back({p.x + n.x * lift, p.y + n.y * lift, p.z + n.z * lift});
                    }
                }
            }
        }
        lines.insert(lines.end(), m_cutFaceOverlay.sets.begin(), m_cutFaceOverlay.sets.end());
    }
    if (!m_planeCutsShowFrames) return;
    for (const auto& frame : guide->frames) {
        renderer::OverlayLineSet set{planeColor(frame.plane, 1), {}};
        for (size_t i = 0; i < frame.corners.size(); ++i) {
            const auto& a = frame.corners[i];
            const auto& c = frame.corners[(i + 1) % frame.corners.size()];
            set.points.push_back({a.x, a.y, a.z});
            set.points.push_back({c.x, c.y, c.z});
        }
        lines.push_back(std::move(set));
    }
}

void Application::AppendPieceOverlay(std::vector<renderer::OverlayLineSet>& lines) {
    const graph::Node* node = m_graph.FindNode(m_selectedGraphNode);
    const bool voronoi = node && node->kind == graph::NodeKind::VoronoiFracture;
    const bool filter = node && node->kind == graph::NodeKind::PieceFilter && !node->inputs.empty();
    // Voronoi はオンにしたら岩の面を隠し、ワイヤーフレームと点だけを見せる。Piece Filter は面を残す。
    m_renderer.SetMeshSceneHidden(voronoi && (m_voronoiShowWireframe || m_voronoiShowPoints));
    if (!(voronoi && m_voronoiShowWireframe) && !(filter && m_pieceFilterShowRemoved)) return;
    // 評価のキャッシュに残る、各ノードの直近の出力から読む。評価中は前回の結果を出し続ける。
    const auto output = [&](const graph::Node* source) -> std::shared_ptr<const geometry::PieceCollection> {
        if (!source) return nullptr;
        const auto found = m_rockEvaluationCache.pieceOutputs.find(source->id);
        return found == m_rockEvaluationCache.pieceOutputs.end() ? nullptr : found->second;
    };
    // Voronoi は自身の出力すべて。Piece Filter は入力のうち、出力に残らなかった片。
    std::shared_ptr<const geometry::PieceCollection> pieces, excluded;
    if (voronoi) {
        pieces = output(node);
    } else {
        pieces = output(m_graph.FindUpstreamNodeForPin(node->inputs[0].id));
        excluded = output(node);
        if (!excluded) return;
    }
    if (!pieces) return;
    if (m_pieceWireframe.pieces != pieces || m_pieceWireframe.excluded != excluded) {
        m_pieceWireframe = {pieces, excluded, {}};
        std::vector<uint32_t> kept;
        if (excluded)
            for (const auto& piece : excluded->pieces) kept.push_back(piece.id);
        std::sort(kept.begin(), kept.end());
        // 深度付きで描く。Voronoi は面を隠しているので内部のセルの辺もすべて見え、
        // Piece Filter では残った片の向こう側の辺が隠れる。
        renderer::OverlayLineSet set{XMFLOAT4{.85f, .9f, 1.f, 1.f}, {}};
        for (const auto& piece : pieces->pieces) {
            if (std::binary_search(kept.begin(), kept.end(), piece.id)) continue;
            for (const auto& edge : geometry::PieceEdges(piece))
                for (const auto& p : edge) set.points.push_back({p.x, p.y, p.z});
        }
        m_pieceWireframe.sets.push_back(std::move(set));
    }
    lines.insert(lines.end(), m_pieceWireframe.sets.begin(), m_pieceWireframe.sets.end());
}

std::shared_ptr<const geometry::PointSet> Application::SelectedVoronoiPoints() const {
    if (!m_voronoiShowPoints) return nullptr;
    const graph::Node* node = m_graph.FindNode(m_selectedGraphNode);
    if (!node || node->kind != graph::NodeKind::VoronoiFracture || node->inputs.size() < 2) return nullptr;
    const graph::Node* source = m_graph.FindUpstreamNodeForPin(node->inputs[1].id);
    if (!source) return nullptr;
    const auto found = m_rockEvaluationCache.pieceEntries.find(source->id);
    return found == m_rockEvaluationCache.pieceEntries.end() ? nullptr : found->second.result.points;
}

void Application::DrawModelInstanceOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    // --- 範囲の枠（レンダラが深度付きで描く） ------------------------------------------
    // 選んだ Model ノードはそのモデル、Transform はその枝のモデルすべて。ホバーは薄く。
    std::vector<renderer::OverlayLineSet> lines;
    const auto visible = CollectVisibleModels();
    const auto boxLines = [&](const VisibleModel& model, renderer::OverlayLineSet& set) {
        const XMMATRIX world = XMLoadFloat4x4(&model.world);
        const auto& lo = model.model->geometry->minimum;
        const auto& hi = model.model->geometry->maximum;
        XMFLOAT3 corners[8];
        for (int i = 0; i < 8; ++i) {
            const XMFLOAT3 corner{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
            XMStoreFloat3(&corners[i], XMVector3TransformCoord(XMLoadFloat3(&corner), world));
        }
        static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                              {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : kEdges) {
            set.points.push_back(corners[edge[0]]);
            set.points.push_back(corners[edge[1]]);
        }
    };
    const auto color = [](ImGuiCol index) {
        const ImVec4 value = ImGui::GetStyleColorVec4(index);
        return XMFLOAT4{value.x, value.y, value.z, value.w};
    };
    renderer::OverlayLineSet hovered{color(ImGuiCol_PlotLines), {}};
    renderer::OverlayLineSet selectedSet{color(ImGuiCol_PlotLinesHovered), {}};
    const graph::Node* selectedNode = m_graph.FindNode(m_selectedGraphNode);
    NodeTransformRef activeTransform;
    const bool modelSelected = selectedNode != nullptr && NodeTransform(m_selectedGraphNode, activeTransform);
    for (const VisibleModel& model : visible) {
        const bool inSelection = modelSelected &&
            (model.node == m_selectedGraphNode ||
             std::find(model.transforms.begin(), model.transforms.end(), m_selectedGraphNode) != model.transforms.end());
        if (inSelection) boxLines(model, selectedSet);
        else if (model.node == m_hoveredModelNode) boxLines(model, hovered);
    }
    // ノード用のギズモ: 選んだノードの部品の枠（選択の色）と、カーソルが乗った部品のノードの枠（ホバーの色）。
    ModelNodeGizmo nodeGizmo;
    const bool hasNodeGizmo = modelSelected && ModelNodeGizmoFrame(visible, nodeGizmo);
    if (hasNodeGizmo) {
        selectedSet.points.clear();
        const auto& geometry = *nodeGizmo.visible->model->geometry;
        std::vector<XMFLOAT4X4> nodeWorlds;
        renderer::ModelNodeWorlds(geometry, nodeGizmo.settings->nodeRotations, nodeWorlds);
        const auto nodeBox = [&](const std::string& name, renderer::OverlayLineSet& set) {
            for (const auto& part : geometry.lods[0].parts) {
                if (part.node >= geometry.nodes.size() || geometry.nodes[part.node].name != name) continue;
                const XMMATRIX world = XMLoadFloat4x4(&nodeWorlds[part.node]) * XMLoadFloat4x4(&nodeGizmo.visible->world);
                XMFLOAT3 corners[8];
                for (int i = 0; i < 8; ++i) {
                    const XMFLOAT3 corner{(i & 1) ? part.maximum.x : part.minimum.x, (i & 2) ? part.maximum.y : part.minimum.y,
                                          (i & 4) ? part.maximum.z : part.minimum.z};
                    XMStoreFloat3(&corners[i], XMVector3TransformCoord(XMLoadFloat3(&corner), world));
                }
                static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& edge : kEdges) {
                    set.points.push_back(corners[edge[0]]);
                    set.points.push_back(corners[edge[1]]);
                }
            }
        };
        nodeBox(m_selectedModelNodeName, selectedSet);
        if (!m_hoveredModelNodeName.empty() && m_hoveredModelNodeName != m_selectedModelNodeName)
            nodeBox(m_hoveredModelNodeName, hovered);
    }
    if (!hovered.points.empty()) lines.push_back(std::move(hovered));
    if (!selectedSet.points.empty()) lines.push_back(std::move(selectedSet));
    AppendPlaneCutOverlay(lines);
    AppendPieceOverlay(lines);
    m_renderer.SetOverlayLines(std::move(lines));

    // --- ギズモ（ImGui。深度は見ず常に手前） ---------------------------------------
    XMFLOAT3 pivot{};
    XMFLOAT4X4 parent{};
    if (!modelSelected || !NodeGizmoFrame(m_selectedGraphNode, pivot, parent)) return;
    const GizmoScreen gizmo = hasNodeGizmo
        ? BuildGizmo(m_renderer.GetCamera(), nodeGizmo.origin, viewportMin, viewportMax, nodeGizmo.axes)
        : BuildGizmo(m_renderer.GetCamera(), pivot, viewportMin, viewportMax);
    if (!gizmo.valid) return;
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(viewportMin, viewportMax, true);
    const ImU32 hoverColor = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
    const ImU32 shadow = IM_COL32(0, 0, 0, 140);
    const int active = m_modelGizmoHover;
    const ModelGizmoMode mode = hasNodeGizmo ? ModelGizmoMode::Rotate : m_modelGizmoMode;
    if (mode == ModelGizmoMode::Translate) {
        for (int axis = 0; axis < 3; ++axis) {
            const bool on = active == kHandlePlane + axis;
            const ImU32 base = kAxisColors[axis];
            const ImU32 fill = on ? ((hoverColor & ~IM_COL32_A_MASK) | (160u << IM_COL32_A_SHIFT))
                                  : ((base & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT));
            const auto& quad = gizmo.planes[axis];
            draw->AddQuadFilled(quad[0], quad[1], quad[2], quad[3], fill);
            draw->AddQuad(quad[0], quad[1], quad[2], quad[3], on ? hoverColor : base, ui::Scaled(1.0f));
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (!gizmo.axisVisible[axis]) continue;
            const bool on = active == kHandleAxis + axis;
            const ImU32 lineColor = on ? hoverColor : kAxisColors[axis];
            const float width = ui::Scaled(on ? 3.0f : 2.0f);
            draw->AddLine(gizmo.center, gizmo.tips[axis], shadow, width + ui::Scaled(2.0f));
            draw->AddLine(gizmo.center, gizmo.tips[axis], lineColor, width);
            // 先端の矢じり。
            ImVec2 dir(gizmo.tips[axis].x - gizmo.center.x, gizmo.tips[axis].y - gizmo.center.y);
            const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (length < 1.0f) continue;
            dir = ImVec2(dir.x / length, dir.y / length);
            const ImVec2 side(-dir.y, dir.x);
            const float head = ui::Scaled(10.0f), halfWidth = ui::Scaled(5.0f);
            const ImVec2 tip = gizmo.tips[axis];
            const ImVec2 baseCenter(tip.x - dir.x * head, tip.y - dir.y * head);
            draw->AddTriangleFilled(tip, ImVec2(baseCenter.x + side.x * halfWidth, baseCenter.y + side.y * halfWidth),
                                    ImVec2(baseCenter.x - side.x * halfWidth, baseCenter.y - side.y * halfWidth), lineColor);
            const char* labels[] = {"X", "Y", "Z"};
            draw->AddText(ImVec2(tip.x + ui::Scaled(5.0f), tip.y), lineColor, labels[axis]);
        }
    } else if (mode == ModelGizmoMode::Scale) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!gizmo.axisVisible[axis]) continue;
            const bool on = active == kHandleScale + axis || active == kHandleScaleAll;
            const ImU32 lineColor = on ? hoverColor : kAxisColors[axis];
            const float width = ui::Scaled(on ? 3.0f : 2.0f);
            const ImVec2 tip = gizmo.tips[axis];
            draw->AddLine(gizmo.center, tip, shadow, width + ui::Scaled(2.0f));
            draw->AddLine(gizmo.center, tip, lineColor, width);
            // 先端の四角。
            const float half = ui::Scaled(kScaleTipPixels);
            draw->AddRectFilled(ImVec2(tip.x - half, tip.y - half), ImVec2(tip.x + half, tip.y + half), lineColor);
            const char* labels[] = {"X", "Y", "Z"};
            draw->AddText(ImVec2(tip.x + half + ui::Scaled(3.0f), tip.y), lineColor, labels[axis]);
        }
        // 中心の四角（全体の倍率）。
        const bool on = active == kHandleScaleAll;
        const float half = ui::Scaled(kScaleCenterPixels);
        const ImVec2 lo(gizmo.center.x - half, gizmo.center.y - half), hi(gizmo.center.x + half, gizmo.center.y + half);
        draw->AddRectFilled(lo, hi, on ? ((hoverColor & ~IM_COL32_A_MASK) | (160u << IM_COL32_A_SHIFT)) : IM_COL32(235, 235, 235, 70));
        draw->AddRect(lo, hi, on ? hoverColor : IM_COL32(235, 235, 235, 230), 0.0f, 0, ui::Scaled(1.0f));
    } else {
        for (int axis = 0; axis < 3; ++axis) {
            const bool on = active == kHandleRing + axis;
            const float width = ui::Scaled(on ? 3.0f : 2.0f);
            for (int i = 0; i < kRingSegments; ++i) {
                ImU32 segment = on ? hoverColor : kAxisColors[axis];
                // 奥側の半分は薄く描き、手前と見分ける。
                if (!on && gizmo.ringBack[axis][i]) segment = (segment & ~IM_COL32_A_MASK) | (90u << IM_COL32_A_SHIFT);
                draw->AddLine(gizmo.rings[axis][i], gizmo.rings[axis][i + 1], segment, width);
            }
        }
    }
    draw->AddCircleFilled(gizmo.center, ui::Scaled(3.0f), IM_COL32(235, 235, 235, 230));
    draw->PopClipRect();
}

void Application::ModelDropTarget(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    if (!ImGui::BeginDragDropTarget()) return;
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPathDragDropType);
    if (payload != nullptr && payload->DataSize >= int(sizeof(wchar_t))) {
        const std::wstring text(static_cast<const wchar_t*>(payload->Data));
        XMFLOAT3 point;
        if (PickGround(ImGui::GetIO().MousePos, viewportMin, viewportMax, 0.0f, true, point)) {
            // 複数を落としたら 1 つずつ X へずらして並べる。
            float offset = 0.0f;
            for (size_t begin = 0; begin < text.size();) {
                const size_t end = std::min(text.find(L'\n', begin), text.size());
                const fs::path path = text.substr(begin, end - begin);
                const auto ext = LowerExtension(path);
                if (ext == L".rockmodel" || ext == L".fbx") {
                    m_pendingModelPlacements.push_back({path, {point.x + offset, point.y, point.z}});
                    offset += 2.0f;
                }
                begin = end + 1;
            }
        }
    }
    ImGui::EndDragDropTarget();
}

bool Application::DrawModelNodeSettings(graph::Node& node) {
    NodeTransformRef transform;
    if (!NodeTransform(node.id, transform)) return false;
    auto* settings = std::get_if<graph::ModelNodeSettings>(&node.settings);
    renderer::ModelAsset* model = settings ? FindModel(settings->model) : nullptr;
    bool changed = false;
    ui::SectionHeader(settings ? "モデル" : "Transform");
    if (ui::BeginPropertyTable("modelNode")) {
        if (settings != nullptr) {
            ui::PropertyLabel("モデル", "シーンに読み込んだモデル。アセットの帯で .rockmodel / .fbx をダブルクリックすると候補に加わる");
            ImGui::SetNextItemWidth(std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("##model", model ? model->name.c_str() : "なし")) {
                if (ImGui::Selectable("なし", settings->model == 0)) {
                    settings->model = 0;
                    changed = true;
                }
                for (const renderer::ModelAsset& candidate : m_models) {
                    ImGui::PushID(static_cast<int>(candidate.id));
                    if (ImGui::Selectable(candidate.name.c_str(), settings->model == candidate.id)) {
                        settings->model = candidate.id;
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ui::PropertyEnd();
            model = FindModel(settings->model);
        }
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        if (ui::PropertyFloat3Input("位置 (m)", transform.position, zero,
                                    settings ? "モデルの底面の中心を置く位置。W の移動ギズモか、本体のドラッグ（水平）でも動く"
                                             : "上流のモデルをまとめて動かす量。W の移動ギズモでも動く") != 0) {
            changed = true;
        }
        if (ui::PropertyFloat3Input("回転 (度)", transform.rotation, zero,
                                    "X / Y / Z 軸まわり。Z → X → Y の順に回す。E の回転ギズモでも回る（Ctrl で 15 度刻み）") != 0) {
            changed = true;
        }
        changed |= ui::PropertyFloat("倍率", transform.scale, 0.01f, 100.0f, 1.0f,
                                     settings ? "このノードで掛ける倍率。モデルアセットの倍率に掛かる。R の倍率ギズモでも変わる（Ctrl で 0.1 刻み）"
                                              : "上流のモデルをまとめて拡大・縮小する（原点まわり）。R の倍率ギズモでも変わる（Ctrl で 0.1 刻み）",
                                     "%.3f", ImGuiSliderFlags_Logarithmic);
        *transform.scale = std::clamp(*transform.scale, 0.01f, 100.0f);
        if (model != nullptr && model->geometry) {
            const float scale = model->scale * *transform.scale;
            const auto& g = *model->geometry;
            ui::PropertyValue("寸法 X / Y / Z", "%.2f / %.2f / %.2f m", (g.maximum.x - g.minimum.x) * scale,
                              (g.maximum.y - g.minimum.y) * scale, (g.maximum.z - g.minimum.z) * scale);
        }
        ui::EndPropertyTable();
    }
    // --- FBX のノードの回転（戦車の砲塔の旋回・砲身の俯仰など）---------------------
    if (settings != nullptr && model != nullptr && model->geometry && model->geometry->nodes.size() > 1) {
        const auto& nodes = model->geometry->nodes;
        const auto rotationOf = [&](const std::string& name) {
            return std::find_if(settings->nodeRotations.begin(), settings->nodeRotations.end(),
                                [&](const renderer::ModelNodeRotation& r) { return r.node == name; });
        };
        if (std::none_of(nodes.begin(), nodes.end(),
                         [&](const renderer::ModelNode& n) { return n.name == m_selectedModelNodeName; })) {
            // 最初の子のノード（一番上の空のノードより、砲塔などの動かすものを選びやすく）。
            const auto child = std::find_if(nodes.begin(), nodes.end(), [](const auto& n) { return n.parent >= 0; });
            m_selectedModelNodeName = (child != nodes.end() ? *child : nodes.front()).name;
        }
        ui::SectionHeader("ノード");
        if (ui::BeginPropertyTable("modelNodeRotation")) {
            ui::PropertyLabel("ノード", "FBX のノード（階層は字下げ）。* は回転を足してあるもの");
            ImGui::SetNextItemWidth(std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("##modelNode", m_selectedModelNodeName.c_str())) {
                for (size_t i = 0; i < nodes.size(); ++i) {
                    int depth = 0;
                    for (int p = nodes[i].parent; p >= 0; p = nodes[static_cast<size_t>(p)].parent) ++depth;
                    const std::string label = std::string(static_cast<size_t>(depth) * 2, ' ') + nodes[i].name +
                                              (rotationOf(nodes[i].name) != settings->nodeRotations.end() ? " *" : "");
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(label.c_str(), nodes[i].name == m_selectedModelNodeName))
                        m_selectedModelNodeName = nodes[i].name;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ui::PropertyEnd();
            ui::PropertyBool("ギズモ", &m_modelNodeGizmo, false,
                             "入れると、ビューポートのギズモを選んだノードを回す輪にする。部品のクリックでノードを選ぶ。"
                             "W / E / R でモデルのギズモに戻る（Ctrl で 15 度刻み、Esc で掴む前に戻す）");
            float degrees[3] = {0.0f, 0.0f, 0.0f};
            if (const auto found = rotationOf(m_selectedModelNodeName); found != settings->nodeRotations.end())
                std::copy(std::begin(found->rotationDegrees), std::end(found->rotationDegrees), degrees);
            const float zero[3] = {0.0f, 0.0f, 0.0f};
            if (ui::PropertyFloat3Input("回転 (度)", degrees, zero,
                                        "選んだノードを、その原点を中心にモデルの X / Y / Z 軸まわりに回す"
                                        "（Z → X → Y の順）。子のノードも一緒に回る") != 0) {
                changed |= SetModelNodeRotation(*settings, m_selectedModelNodeName, degrees);
            }
            ui::EndPropertyTable();
        }
        if (!settings->nodeRotations.empty() && ui::Button("すべて戻す", ui::kWideButtonWidth)) {
            settings->nodeRotations.clear();
            changed = true;
        }
    }
    if (model != nullptr && ui::Button("モデルを開く", ui::kWideButtonWidth)) {
        m_selectedModel = model->id;
        m_showModelPreview = true;
    }
    XMFLOAT3 pivot;
    XMFLOAT4X4 parent;
    if (!NodeGizmoFrame(node.id, pivot, parent))
        ui::HintText("Mesh Output へ（Transform / Merge を通して）繋ぐとビューポートに出る");
    ui::HintText("ビューポートで W: 移動ギズモ / E: 回転ギズモ / R: 倍率ギズモ（「ノード」の「ギズモ」でノードを回す輪）。モデルのクリックで選び、本体のドラッグで水平に移動、Delete でノードごと削除");
    return changed;
}

bool Application::SelectedModelInstanceFocusTarget(XMFLOAT3& target) {
    XMFLOAT3 pivot;
    XMFLOAT4X4 parent;
    if (!NodeGizmoFrame(m_selectedGraphNode, pivot, parent)) return false;
    // Model ノードはモデルの範囲の中心、Transform はその原点へ寄る。
    for (const VisibleModel& visible : CollectVisibleModels()) {
        if (visible.node != m_selectedGraphNode) continue;
        BoundingBox bounds;
        if (renderer::ModelWorldBounds(*visible.model, XMLoadFloat4x4(&visible.world), bounds)) {
            target = bounds.Center;
            return true;
        }
    }
    target = pivot;
    return true;
}

// カーソル位置からカメラのレイ（ワールド座標、方向は単位長）。ビューポートが潰れていれば偽。
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

}  // namespace rock
