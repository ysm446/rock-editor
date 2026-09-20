#include "graph/Road.h"
#include "graph/RoadProfile.h"
#include "graph/RoadMask.h"

#include <cfloat>
#include <algorithm>
#include <cmath>

#include <string>
#include <unordered_set>
#include <vector>

namespace tg::graph {
namespace {
using namespace DirectX;
XMFLOAT3 Position(const PathCurveSample& p) { return {p.x, p.y, p.z}; }
XMVECTOR Load(const XMFLOAT3& p) { return XMLoadFloat3(&p); }
float Length(XMVECTOR p) { return XMVectorGetX(XMVector3Length(p)); }
void BuildArrowMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                        const RoadLanes& lanes, renderer::MeshData& result);
void BuildDashedStrip(const RoadGeometry& road, const RoadMarkingNodeSettings& settings, float lateral, float line,
                      renderer::MeshData& result);
void BuildStopLines(const RoadGeometry& road, const RoadMarkingNodeSettings& settings, const RoadLanes& lanes,
                    renderer::MeshData& result);
void AddBoundaryPoint(PathSettings& path, const XMFLOAT3& p) {
    const auto previous = path.points.empty() ? 0 : path.points.back().id;
    const auto id = AddPathPoint(path, p.x, p.z, 0);
    path.FindPoint(id)->y = p.y;
    if (previous != 0) ConnectPathPoints(path, previous, id);
}

bool Evaluate(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result,
              std::string& error, std::unordered_set<GraphId>& visiting) {
    const Node* node = graph.FindNode(nodeId);
    const auto* settings = node ? std::get_if<RoadNodeSettings>(&node->settings) : nullptr;
    if (!settings || node->inputs.empty()) { error = "Roadノードが接続されていません"; return false; }
    if (visiting.size() >= 64 || !visiting.insert(nodeId).second) {
        error = "道路の依存が循環しているか、深すぎます"; return false;
    }
    const Pin* source = nullptr;
    for (const auto& link : graph.Links()) {
        if (link.endPin == node->inputs.front().id) source = graph.FindPin(link.startPin);
    }
    const Node* upstream = source ? graph.FindNode(source->nodeId) : nullptr;
    bool success = false;
    if (const auto* path = upstream ? std::get_if<PathNodeSettings>(&upstream->settings) : nullptr) {
        success = BuildRoad(path->path, *settings, result, error);
    } else if (upstream && upstream->kind == NodeKind::Road) {
        RoadGeometry parent;
        if (Evaluate(graph, upstream->id, parent, error, visiting)) {
            success = BuildRoad(source->label == "Left" ? parent.left : parent.right,
                                *settings, result, error);
        }
    } else if (upstream && upstream->kind == NodeKind::Shoulder) {
        // 路肩の Outer（外側の境界）から道路を作る。路肩の格子は left に Outer を持つ。
        RoadGeometry parent;
        if (EvaluateShoulder(graph, upstream->id, parent, error)) {
            success = BuildRoad(parent.left, *settings, result, error);
        }
    } else {
        error = "実寸Pathを接続してください";
    }
    visiting.erase(nodeId);
    return success;
}

// 路肩の Path 入力の上流を、境界を持つ格子（Road / Shoulder）として評価する。
// edgeColumn は境界の列、innerColumn は外向きを決める隣の列。
bool EvaluateBoundarySource(const NodeGraph& graph, const Node& shoulderNode, RoadGeometry& source,
                            uint32_t& edgeColumn, uint32_t& innerColumn, std::string& error, int depth) {
    if (depth >= 64) { error = "路肩の依存が深すぎます"; return false; }
    if (shoulderNode.inputs.empty()) { error = "路肩の Path が無い"; return false; }
    const Pin* pin = nullptr;
    for (const auto& link : graph.Links()) {
        if (link.endPin == shoulderNode.inputs.front().id) pin = graph.FindPin(link.startPin);
    }
    const Node* upstream = pin ? graph.FindNode(pin->nodeId) : nullptr;
    if (!upstream) { error = "Road の Left / Right か Shoulder の Outer を接続してください"; return false; }
    bool built = false;
    if (upstream->kind == NodeKind::Road) {
        built = EvaluateRoad(graph, upstream->id, source, error);
        if (built && pin->label != "Left" && pin->label != "Right") { error = "路肩には Road の Left / Right を繋いでください"; return false; }
        // Road の格子は列 0 が Right、列末尾が Left。
        edgeColumn = (pin->label == "Left") ? source.stride - 1 : 0;
        innerColumn = (pin->label == "Left") ? source.stride - 2 : 1;
    } else if (upstream->kind == NodeKind::Shoulder) {
        RoadGeometry parentSource;
        uint32_t parentEdge = 0, parentInner = 0;
        const auto* parentSettings = std::get_if<ShoulderNodeSettings>(&upstream->settings);
        built = parentSettings &&
                EvaluateBoundarySource(graph, *upstream, parentSource, parentEdge, parentInner, error, depth + 1) &&
                BuildShoulder(parentSource, parentEdge, parentInner, *parentSettings, source, error);
        // 路肩の格子は列末尾が Outer。
        edgeColumn = source.stride - 1;
        innerColumn = source.stride - 2;
    } else {
        error = "路肩には Road の Left / Right か Shoulder の Outer を繋いでください";
    }
    return built;
}
}  // namespace

bool BuildShoulder(const RoadGeometry& source, uint32_t edgeColumn, uint32_t innerColumn,
                   const ShoulderNodeSettings& settings, RoadGeometry& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (!std::isfinite(settings.widthMeters) || settings.widthMeters < 0.1f || settings.widthMeters > 50.0f ||
        !std::isfinite(settings.crossSlopePercent) || std::abs(settings.crossSlopePercent) > 50.0f ||
        !std::isfinite(settings.uvRepeatMeters) || settings.uvRepeatMeters < 0.1f || settings.uvRepeatMeters > 100.0f)
        return fail("幅は0.1〜50 m、横断勾配は±50%、UV反復長は0.1〜100 mにしてください");
    const auto& sv = source.surface.vertices;
    if (source.stride < 2 || sv.size() < source.stride * 2) return fail("境界の元になる面が生成されていません");
    if (edgeColumn >= source.stride || innerColumn >= source.stride || edgeColumn == innerColumn) return fail("境界の列が不正です");
    const size_t rows = sv.size() / source.stride;
    if (source.rowDistances.size() != rows) return fail("境界の実距離が揃っていません");
    if (!std::isfinite(settings.stepHeightMeters) || settings.stepHeightMeters < 0.0f || settings.stepHeightMeters > 0.5f ||
        !std::isfinite(settings.stepWidthMeters) || settings.stepWidthMeters < 0.005f || settings.stepWidthMeters > 1.0f)
        return fail("段差は0〜0.5 m、面取り幅は0.005〜1 mにしてください");
    // 列の横位置。境界 0、（段差があれば）面取り列、以後は約 1 m 刻みで外側まで。
    const uint32_t cells = static_cast<uint32_t>(std::ceil(settings.widthMeters));
    const bool stepped = settings.stepHeightMeters > 0.0f;
    const float stepLateral = std::min(settings.stepWidthMeters, settings.widthMeters * 0.5f);
    std::vector<float> laterals;
    laterals.push_back(0.0f);
    if (stepped) laterals.push_back(stepLateral);
    for (uint32_t cell = 1; cell <= cells; ++cell) {
        const float lateral = settings.widthMeters * static_cast<float>(cell) / static_cast<float>(cells);
        if (lateral > laterals.back() + 1e-4f) laterals.push_back(lateral);
    }
    const uint32_t stride = static_cast<uint32_t>(laterals.size());
    const uint32_t columns = stride - 1;
    if (rows * stride > 65536) return fail("路肩の分割数が多すぎます");
    RoadGeometry built;
    built.stride = stride;
    // 下流（Decal など）が読む設定。幅・反復長・UV の向きは路肩のもの、押し出しは 0。
    built.settings.widthMeters = settings.widthMeters;
    built.settings.uvRepeatMeters = settings.uvRepeatMeters;
    built.settings.uvAlongU = settings.uvAlongU;
    built.settings.displacementMeters = std::max(0.0f, settings.displacementMeters);
    built.settings.layerBlendRange = settings.layerBlendRange;
    for (int slot = 0; slot < kRoadMaterialSlots; ++slot) {
        built.settings.layerWorldUv[slot] = settings.layerWorldUv[slot];
        built.settings.layerUvRepeatMeters[slot] = settings.layerUvRepeatMeters[slot];
        built.settings.layerHeightGate[slot] = settings.layerHeightGate[slot];
        built.settings.layerHeightGateThreshold[slot] = settings.layerHeightGateThreshold[slot];
        built.settings.layerHeightGateSoftness[slot] = settings.layerHeightGateSoftness[slot];
        built.settings.layerBlendMode[slot] = settings.layerBlendMode[slot];
    }
    built.rowDistances = source.rowDistances;
    const float drop = settings.crossSlopePercent * 0.01f;
    for (size_t row = 0; row < rows; ++row) {
        const XMFLOAT3& edge = sv[row * source.stride + edgeColumn].position;
        const XMFLOAT3& inner = sv[row * source.stride + innerColumn].position;
        // 外向きは境界から隣の列を引いた水平成分。境界の頂点はそのまま列 0 に写す（水密）。
        XMVECTOR outward = XMVectorSet(edge.x - inner.x, 0.0f, edge.z - inner.z, 0.0f);
        if (Length(outward) < 1e-5f) return fail("境界の幅が 0 の行があります");
        outward = XMVector3Normalize(outward);
        for (uint32_t column = 0; column <= columns; ++column) {
            const float lateral = laterals[column];
            renderer::MeshVertex vertex{};
            if (column == 0) {
                vertex.position = edge;
            } else {
                XMStoreFloat3(&vertex.position, XMVectorAdd(Load(edge), XMVectorScale(outward, lateral)));
                // 段差は面取り列から先の全列に掛かる（境界の頂点は共有のまま）。
                vertex.position.y -= drop * lateral + (stepped ? settings.stepHeightMeters : 0.0f);
            }
            vertex.uv = {lateral / settings.uvRepeatMeters, source.rowDistances[row] / settings.uvRepeatMeters};
            if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
            vertex.roadUv = vertex.uv;
            built.surface.vertices.push_back(vertex);
            if (column == 0) AddBoundaryPoint(built.right, vertex.position);
            if (column == columns) AddBoundaryPoint(built.left, vertex.position);
        }
        if (row > 0) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t a = static_cast<uint32_t>(row - 1) * stride + column;
                // 列の向きは左右どちらの境界かで変わるので、三角形ごとに法線が上を向くよう並べる。
                const uint32_t tris[2][3] = {{a, a + stride, a + 1}, {a + 1, a + stride, a + stride + 1}};
                for (const auto& tri : tris) {
                    uint32_t x = tri[0], y = tri[1], z = tri[2];
                    const auto& v = built.surface.vertices;
                    const XMVECTOR n = XMVector3Cross(XMVectorSubtract(Load(v[y].position), Load(v[x].position)),
                                                     XMVectorSubtract(Load(v[z].position), Load(v[x].position)));
                    if (XMVectorGetY(n) < 0.0f) std::swap(y, z);
                    built.surface.indices.insert(built.surface.indices.end(), {x, y, z});
                }
            }
        }
    }
    auto& mesh = built.surface;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i + 1]];
        auto& c = mesh.vertices[mesh.indices[i + 2]];
        const XMVECTOR n = XMVector3Cross(XMVectorSubtract(Load(b.position), Load(a.position)),
                                         XMVectorSubtract(Load(c.position), Load(a.position)));
        if (XMVectorGetY(n) <= 1e-7f) return fail("幅に対してカーブが急すぎて路肩が反転します");
        for (auto* vertex : {&a, &b, &c}) XMStoreFloat3(&vertex->normal, XMVectorAdd(Load(vertex->normal), n));
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto& vertex = mesh.vertices[i];
        const XMVECTOR n = XMVector3Normalize(Load(vertex.normal));
        const XMVECTOR across = XMVectorSubtract(Load(mesh.vertices[(i / stride) * stride + columns].position),
                                                 Load(mesh.vertices[(i / stride) * stride].position));
        const XMVECTOR t = XMVector3Normalize(XMVectorSubtract(across, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, across)))));
        XMStoreFloat3(&vertex.normal, n);
        XMStoreFloat4(&vertex.tangent, t);
        vertex.tangent.w = -1.0f;
    }
    result = std::move(built);
    return true;
}

bool EvaluateShoulder(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result, std::string& error) {
    const Node* node = graph.FindNode(nodeId);
    const auto* settings = node ? std::get_if<ShoulderNodeSettings>(&node->settings) : nullptr;
    if (!settings) { error = "Shoulder ノードではありません"; return false; }
    RoadGeometry source;
    uint32_t edgeColumn = 0, innerColumn = 0;
    return EvaluateBoundarySource(graph, *node, source, edgeColumn, innerColumn, error, 0) &&
           BuildShoulder(source, edgeColumn, innerColumn, *settings, result, error);
}

bool BuildRoad(const PathSettings& path, const RoadNodeSettings& settings,
               RoadGeometry& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (!std::isfinite(settings.widthMeters) || settings.widthMeters < 0.1f ||
        settings.widthMeters > 50.0f || !std::isfinite(settings.uvRepeatMeters) ||
        settings.uvRepeatMeters < 0.1f || settings.uvRepeatMeters > 100.0f)
        return fail("幅は0.1〜50 m、UV反復長は0.1〜100 mにしてください");
    if (path.points.size() > 2048 || path.edges.size() > 2048)
        return fail("Pathが大きすぎます（最大2048点）");
    const auto strands = BuildPathStrands(path);
    if (strands.size() != 1 || strands.front().closed || strands.front().points.size() != path.points.size())
        return fail("分岐・閉ループ・孤立点のない1本のPathが必要です");
    const auto samples = SamplePathStrand(path, strands.front(), 24);
    std::vector<XMFLOAT3> centers;
    for (const auto& sample : samples) {
        auto p = Position(sample);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return fail("Pathの座標が不正です");
        if (centers.empty() || Length(XMVectorSubtract(Load(p), Load(centers.back()))) > 1e-5f)
            centers.push_back(p);
    }
    if (centers.size() < 2 || centers.size() > 65536) return fail("道路を生成できる点数ではありません");
    // 曲線の端ではパラメータ刻みが極小区間になる。実距離で刻み直し、
    // 数値誤差や接線の揺れが道路端で増幅されないようにする。直線の角は保持する。
    bool curved = false;
    for (const auto& edge : path.edges) curved |= edge.curve != PathCurve::Line;
    // 縦断曲線とバンクは距離に沿って連続に変わるので、直線の線形でも実距離で刻み直す。
    const bool profiled = !path.verticalPoints.empty() || path.bankEnabled;
    const bool resample = curved || profiled;
    if (resample) {
        std::vector<float> lengths(centers.size(), 0.0f);
        for (size_t i = 1; i < centers.size(); ++i)
            lengths[i] = lengths[i-1] + Length(XMVectorSubtract(Load(centers[i]), Load(centers[i-1])));
        const float total = lengths.back();
        if (!std::isfinite(total) || total > 16000.0f) return fail("道路が長すぎます");
        const size_t steps = std::max(size_t(1), static_cast<size_t>(std::ceil(total / 1.0f)));
        std::vector<XMFLOAT3> uniform;
        size_t segment = 1;
        for (size_t i = 0; i <= steps; ++i) {
            const float distance = total * static_cast<float>(i) / static_cast<float>(steps);
            while (segment + 1 < lengths.size() && lengths[segment] < distance) ++segment;
            const float t = (distance-lengths[segment-1]) / (lengths[segment]-lengths[segment-1]);
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVectorLerp(Load(centers[segment-1]), Load(centers[segment]), t));
            uniform.push_back(p);
        }
        centers = std::move(uniform);
    }
    // 縦断曲線。線形の高さを距離軸の放物線で置き換える。ポイントが無ければそのまま。
    {
        const ProfileCurve base = BuildProfileCurve(centers);
        const std::vector<float> heights = EvaluateVerticalProfile(path, base);
        for (size_t i = 0; i < centers.size(); ++i) centers[i].y = heights[i];
    }
    const ProfileCurve centerline = BuildProfileCurve(centers);
    const uint32_t columns = static_cast<uint32_t>(std::ceil(settings.widthMeters));
    const uint32_t stride = columns + 1;
    std::vector<XMFLOAT3> rights;
    for (size_t i = 1; i < centers.size(); ++i) {
        const float dx = centers[i].x - centers[i-1].x;
        const float dz = centers[i].z - centers[i-1].z;
        const float horizontal = std::hypot(dx, dz);
        if (horizontal < 1e-5f) return fail("垂直な区間には道路面を生成できません");
        rights.push_back({dz / horizontal, 0.0f, -dx / horizontal});
    }
    RoadGeometry built;
    built.stride = stride;
    built.settings = settings;
    float distance = 0.0f;
    for (size_t i = 0; i < centers.size(); ++i) {
        XMVECTOR right = Load(rights[std::min(i, rights.size()-1)]);
        float miter = 1.0f;
        if (i > 0 && i < rights.size()) {
            const XMVECTOR sum = XMVectorAdd(right, Load(rights[i-1]));
            if (Length(sum) < 1e-4f) return fail("折り返しが急すぎます。カーブを緩めてください");
            const XMVECTOR average = XMVector3Normalize(sum);
            const float dot = XMVectorGetX(XMVector3Dot(average, right));
            if (dot < 0.25f) return fail("角が急すぎます。カーブを緩めてください");
            miter = 1.0f / dot;
            right = average;
        }
        if (i > 0) distance += Length(XMVectorSubtract(Load(centers[i]), Load(centers[i-1])));
        if (path.bankEnabled) {
            // バンク。接線まわりに横ベクトルを回す。right は列末尾（Left 側）へ向くベクトルなので、
            // 正のバンク（Left 側が上がる）では up 側へ回す。
            const float bank = EvaluateBankAngleRadians(path, centerline, centerline.arcLengths[i]);
            if (std::abs(bank) > 1e-6f) {
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(
                    Load(centers[std::min(i + 1, centers.size() - 1)]), Load(centers[i == 0 ? 0 : i - 1])));
                const XMVECTOR up = XMVector3Normalize(XMVector3Cross(tangent, right));
                right = XMVectorAdd(XMVectorScale(right, std::cos(bank)), XMVectorScale(up, std::sin(bank)));
            }
        }
        const XMVECTOR offset = XMVectorScale(right, settings.widthMeters * 0.5f * miter);
        for (uint32_t column = 0; column <= columns; ++column) {
            const float across = static_cast<float>(column) / static_cast<float>(columns);
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(Load(centers[i]), XMVectorScale(offset, across * 2.0f - 1.0f)));
            vertex.uv = {across * settings.widthMeters / settings.uvRepeatMeters,
                         distance / settings.uvRepeatMeters};
            if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
            vertex.roadUv = vertex.uv;
            built.surface.vertices.push_back(vertex);
            // 列 0 は進行方向に向かって右（右手系 Y-up で (dz, 0, -dx) は左を向く）。
            if (column == 0) AddBoundaryPoint(built.right, vertex.position);
            if (column == columns) AddBoundaryPoint(built.left, vertex.position);
        }
        if (i > 0) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t a = static_cast<uint32_t>(i-1)*stride + column;
                built.surface.indices.insert(built.surface.indices.end(),
                    {a,a+stride,a+1,a+1,a+stride,a+stride+1});
            }
        }
    }
    auto& mesh = built.surface;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i+1]];
        auto& c = mesh.vertices[mesh.indices[i+2]];
        const XMVECTOR n = XMVector3Cross(XMVectorSubtract(Load(b.position), Load(a.position)),
                                         XMVectorSubtract(Load(c.position), Load(a.position)));
        if (XMVectorGetY(n) <= 1e-7f) return fail("幅に対してカーブが急すぎて道路面が反転します");
        for (auto* vertex : {&a, &b, &c}) XMStoreFloat3(&vertex->normal, XMVectorAdd(Load(vertex->normal), n));
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto& vertex = mesh.vertices[i];
        const XMVECTOR n = XMVector3Normalize(Load(vertex.normal));
        const XMVECTOR across = XMVectorSubtract(Load(mesh.vertices[(i/stride)*stride+columns].position), Load(mesh.vertices[(i/stride)*stride].position));
        const XMVECTOR t = XMVector3Normalize(XMVectorSubtract(across, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, across)))));
        XMStoreFloat3(&vertex.normal, n);
        XMStoreFloat4(&vertex.tangent, t);
        vertex.tangent.w = -1.0f;
    }
    // 直線は先に角のマイターを作ってから行を補間する。先に中心線だけを
    // 分割すると、角の短い区間で幅の内側が反転してしまう。
    if (!resample) {
        renderer::MeshData divided;
        for (size_t row = 0; row < centers.size(); ++row) {
            size_t steps = 1;
            if (row > 0) {
                const float length = Length(XMVectorSubtract(Load(centers[row]), Load(centers[row-1])));
                if (!std::isfinite(length) || length > 16000.0f) return fail("道路が長すぎます");
                steps = std::max(size_t(1), static_cast<size_t>(std::ceil(length)));
            }
            if (divided.vertices.size()/stride + steps > 65536) return fail("道路の分割数が多すぎます");
            for (size_t step = 1; step <= steps; ++step) {
                const float t = static_cast<float>(step)/static_cast<float>(steps);
                for (uint32_t column = 0; column <= columns; ++column) {
                    const auto& b = mesh.vertices[row*stride+column];
                    const auto& a = mesh.vertices[(row == 0 ? 0 : row-1)*stride+column];
                    renderer::MeshVertex v;
                    XMStoreFloat3(&v.position, XMVectorLerp(Load(a.position), Load(b.position), t));
                    const auto n = XMVector3Normalize(XMVectorLerp(Load(a.normal), Load(b.normal), t));
                    const auto tangent = XMVectorLerp(XMLoadFloat4(&a.tangent), XMLoadFloat4(&b.tangent), t);
                    XMStoreFloat3(&v.normal, n);
                    XMStoreFloat4(&v.tangent, XMVector3Normalize(XMVectorSubtract(tangent,
                        XMVectorScale(n,XMVectorGetX(XMVector3Dot(n,tangent))))));
                    v.tangent.w = -1.0f;
                    v.uv = {a.uv.x+(b.uv.x-a.uv.x)*t,a.uv.y+(b.uv.y-a.uv.y)*t};
                    v.roadUv = v.uv;
                    divided.vertices.push_back(v);
                }
                const auto count = static_cast<uint32_t>(divided.vertices.size()/stride);
                if (count > 1) for (uint32_t column = 0; column < columns; ++column) {
                    const auto a = (count-2)*stride+column;
                    divided.indices.insert(divided.indices.end(),{a,a+stride,a+1,a+1,a+stride,a+stride+1});
                }
            }
        }
        mesh = std::move(divided);
    }
    // 行ごとの実距離。UV の向きを入れ替えても部品（白線・デカール）が同じ距離を使えるように持つ。
    {
        const size_t rowCount = mesh.vertices.size() / stride;
        built.rowDistances.assign(rowCount, 0.0f);
        for (size_t row = 1; row < rowCount; ++row) {
            const XMVECTOR a = XMVectorScale(XMVectorAdd(Load(mesh.vertices[(row - 1) * stride].position),
                                                         Load(mesh.vertices[(row - 1) * stride + columns].position)), 0.5f);
            const XMVECTOR b = XMVectorScale(XMVectorAdd(Load(mesh.vertices[row * stride].position),
                                                         Load(mesh.vertices[row * stride + columns].position)), 0.5f);
            built.rowDistances[row] = built.rowDistances[row - 1] + Length(XMVectorSubtract(b, a));
        }
        // 停止線。点に最も近い行の間を線分として射影し、実距離に写す（曲線は点を通らない）。
        for (const PathPoint& point : path.points) {
            if (point.stopLine == PathStopLine::None || rowCount < 2) continue;
            const XMVECTOR p = XMVectorSet(point.x, 0.0f, point.z, 0.0f);
            float bestError = 1e30f;
            float bestDistance = 0.0f;
            for (size_t row = 0; row + 1 < rowCount; ++row) {
                const auto rowCenter = [&](size_t r) {
                    const XMVECTOR c = XMVectorScale(XMVectorAdd(Load(mesh.vertices[r * stride].position),
                                                                 Load(mesh.vertices[r * stride + columns].position)), 0.5f);
                    return XMVectorSetY(c, 0.0f);
                };
                const XMVECTOR a = rowCenter(row);
                const XMVECTOR d = XMVectorSubtract(rowCenter(row + 1), a);
                const float lengthSq = XMVectorGetX(XMVector3LengthSq(d));
                const float t = lengthSq > 1e-8f
                    ? std::clamp(XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, a), d)) / lengthSq, 0.0f, 1.0f) : 0.0f;
                const float err = Length(XMVectorSubtract(p, XMVectorAdd(a, XMVectorScale(d, t))));
                if (err < bestError) {
                    bestError = err;
                    bestDistance = built.rowDistances[row] + t * (built.rowDistances[row + 1] - built.rowDistances[row]);
                }
            }
            built.stopLines.push_back({bestDistance, point.stopLine});
        }
    }
    result = std::move(built);
    return true;
}

bool BuildRoadMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                       bool leftHandTraffic, renderer::MeshData& result, std::string& error, uint32_t typeMask) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto& surface = road.surface;
    if (road.stride < 2 || surface.vertices.size() < road.stride * 2 ||
        surface.vertices.size() % road.stride != 0 || road.rowDistances.size() != surface.vertices.size() / road.stride)
        return fail("道路面が生成されていません");
    const float width = road.settings.widthMeters;
    if (!std::isfinite(settings.edgeInsetMeters) || settings.edgeInsetMeters < 0.0f ||
        !std::isfinite(settings.liftMeters) || settings.liftMeters < 0.0f || settings.liftMeters > 0.1f ||
        !std::isfinite(settings.uvRepeatMeters) || settings.uvRepeatMeters < 0.1f ||
        settings.uvRepeatMeters > 100.0f)
        return fail("線幅は0.05〜1 m、浮かせ量は0〜0.1 m、UV反復長は0.1〜100 mにしてください");
    // 帯の中心の横位置（m）。正が Left（列末尾側）。
    const RoadLanes lanes = ComputeRoadLanes(road.settings, leftHandTraffic);
    struct Strip { float offset; float line; bool dashed; uint32_t type; };
    std::vector<Strip> strips;
    const bool dashedCenter = settings.centerLine && lanes.hasCenter && settings.centerLineDashed;
    if (settings.centerLine && lanes.hasCenter)
        strips.push_back({lanes.centerLateral, settings.centerLineWidthMeters, dashedCenter, 1u});
    if (settings.edgeLines) {
        const float edge = width * 0.5f - settings.edgeInsetMeters;
        if (edge - settings.edgeLineWidthMeters * 0.5f < 0.0f) return fail("外側線が中心を越えています。端からの距離を小さくしてください");
        strips.push_back({-edge, settings.edgeLineWidthMeters, false, 2u});
        strips.push_back({edge, settings.edgeLineWidthMeters, false, 2u});
    }
    if (settings.laneLines)
        for (const float divider : lanes.dividers) strips.push_back({divider, settings.laneLineWidthMeters, true, 4u});
    const bool dashed = dashedCenter || (settings.laneLines && !lanes.dividers.empty());
    if (dashed && (!std::isfinite(settings.dashLengthMeters) || settings.dashLengthMeters < 0.1f ||
                   !std::isfinite(settings.dashGapMeters) || settings.dashGapMeters < 0.0f))
        return fail("破線の長さは0.1 m以上、間隔は0 m以上にしてください");
    const bool stops = settings.stopLines && !road.stopLines.empty();
    if (stops && (!std::isfinite(settings.stopLineWidthMeters) || settings.stopLineWidthMeters < 0.1f ||
                  settings.stopLineWidthMeters > 2.0f))
        return fail("停止線の幅は0.1〜2 mにしてください");
    if (strips.empty() && !stops && !settings.arrows)
        return fail("中央線・外側線・車線境界線・停止線・矢印のどれかを有効にしてください");
    if (settings.arrows && (!std::isfinite(settings.arrowIntervalMeters) || settings.arrowIntervalMeters < 1.0f ||
                            !std::isfinite(settings.arrowLengthMeters) || settings.arrowLengthMeters < 0.5f ||
                            settings.arrowLengthMeters > 20.0f))
        return fail("矢印の間隔は1 m以上、長さは0.5〜20 mにしてください");
    for (size_t i = 0; i < strips.size(); ++i) {
        const auto [offset, line, isDashed, type] = strips[i];
        if (!std::isfinite(line) || line < 0.05f || line > 1.0f)
            return fail("各線の幅は0.05〜1 mにしてください");
        if (lanes.laneWidthMeters < line * 2.0f)
            return fail("車線幅に対して線幅が大きすぎます。車線数か線幅を見直してください");
        if (std::abs(offset) + line * 0.5f > width * 0.5f + 1e-4f)
            return fail("線が道路の外に出ます。線幅か端からの距離を見直してください");
        for (size_t j = 0; j < i; ++j)
            if (std::abs(offset - strips[j].offset) < (line + strips[j].line) * 0.5f)
                return fail("道路幅に対して線が重なります。線幅か端からの距離を見直してください");
    }
    const size_t rows = surface.vertices.size() / road.stride;
    if (rows * strips.size() * 2 > 65536 * 3) return fail("白線の頂点数が多すぎます");
    for (const auto [offset, line, isDashed, type] : strips) {
        if (isDashed || !(typeMask & type)) continue;
        const uint32_t base = static_cast<uint32_t>(result.vertices.size());
        for (size_t row = 0; row < rows; ++row) {
            const auto& left = surface.vertices[row * road.stride];
            const auto& right = surface.vertices[row * road.stride + road.stride - 1];
            // 左右端の差は幅にマイター倍率を掛けた横ベクトル。角でも道路端と平行な帯になる。
            const XMVECTOR across = XMVectorSubtract(Load(right.position), Load(left.position));
            const XMVECTOR center = XMVectorScale(XMVectorAdd(Load(left.position), Load(right.position)), 0.5f);
            const float distance = road.rowDistances[row];
            for (int side = 0; side < 2; ++side) {
                const float lateral = offset + (side == 0 ? -line : line) * 0.5f;
                const float t = lateral / width;
                const XMVECTOR n = XMVector3Normalize(XMVectorLerp(Load(left.normal), Load(right.normal), t + 0.5f));
                renderer::MeshVertex vertex{};
                XMStoreFloat3(&vertex.position, XMVectorAdd(XMVectorAdd(center, XMVectorScale(across, t)),
                                                            XMVectorScale(n, settings.liftMeters)));
                XMStoreFloat3(&vertex.normal, n);
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(across,
                    XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, across)))));
                XMStoreFloat4(&vertex.tangent, tangent);
                vertex.tangent.w = -1.0f;
                vertex.uv = {static_cast<float>(side), distance / settings.uvRepeatMeters};
                if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
                // 路面上の位置。列 0（Right 端）からの横距離と実距離を道路の UV 反復長で割る。
                vertex.roadUv = {(width * 0.5f + lateral) / road.settings.uvRepeatMeters,
                                 distance / road.settings.uvRepeatMeters};
                if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
                result.vertices.push_back(vertex);
            }
            if (row > 0) {
                const uint32_t a = base + static_cast<uint32_t>(row - 1) * 2;
                result.indices.insert(result.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
            }
        }
    }
    for (const auto& strip : strips)
        if (strip.dashed && (typeMask & strip.type)) BuildDashedStrip(road, settings, strip.offset, strip.line, result);
    if (stops && (typeMask & 8u)) BuildStopLines(road, settings, lanes, result);
    if (settings.arrows && (typeMask & 16u)) BuildArrowMarkings(road, settings, lanes, result);
    return true;
}

namespace {
// 道路面上の任意の点（距離と横位置）。行の間は線形補間する。横位置は正が Left（列末尾）側。
struct SurfaceSample {
    XMVECTOR position;
    XMVECTOR normal;
    XMVECTOR across;
};
SurfaceSample SampleRoadSurface(const RoadGeometry& road, float distance, float lateral) {
    const auto& v = road.surface.vertices;
    const size_t rows = v.size() / road.stride;
    const auto rowDistance = [&](size_t row) { return road.rowDistances[row]; };
    size_t upper = 1;
    while (upper + 1 < rows && rowDistance(upper) < distance) ++upper;
    const size_t lower = upper - 1;
    const float span = rowDistance(upper) - rowDistance(lower);
    const float t = span > 1e-6f ? std::clamp((distance - rowDistance(lower)) / span, 0.0f, 1.0f) : 0.0f;
    const auto at = [&](size_t row, uint32_t column) { return &v[row * road.stride + column]; };
    const auto lerp3 = [&](const XMFLOAT3& a, const XMFLOAT3& b) { return XMVectorLerp(Load(a), Load(b), t); };
    const XMVECTOR left = lerp3(at(lower, 0)->position, at(upper, 0)->position);
    const XMVECTOR right = lerp3(at(lower, road.stride - 1)->position, at(upper, road.stride - 1)->position);
    const XMVECTOR leftNormal = lerp3(at(lower, 0)->normal, at(upper, 0)->normal);
    const XMVECTOR rightNormal = lerp3(at(lower, road.stride - 1)->normal, at(upper, road.stride - 1)->normal);
    const float s = lateral / road.settings.widthMeters;
    SurfaceSample sample;
    sample.across = XMVectorSubtract(right, left);
    sample.position = XMVectorAdd(XMVectorScale(XMVectorAdd(left, right), 0.5f), XMVectorScale(sample.across, s));
    sample.normal = XMVector3Normalize(XMVectorLerp(leftNormal, rightNormal, s + 0.5f));
    return sample;
}

// 同方向の車線の間の破線。距離 [start, end) ごとに帯を 1 枚ずつ作り、行をまたぐ区間は行ごとに刻む。
void BuildDashedStrip(const RoadGeometry& road, const RoadMarkingNodeSettings& settings, float lateral, float line,
                      renderer::MeshData& result) {
    const float total = road.rowDistances.empty() ? 0.0f : road.rowDistances.back();
    const float width = road.settings.widthMeters;
    const float period = settings.dashLengthMeters + settings.dashGapMeters;
    const auto addRing = [&](float distance) {
        for (int side = 0; side < 2; ++side) {
            const float at = lateral + (side == 0 ? -line : line) * 0.5f;
            const SurfaceSample sample = SampleRoadSurface(road, distance, at);
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(sample.position, XMVectorScale(sample.normal, settings.liftMeters)));
            XMStoreFloat3(&vertex.normal, sample.normal);
            const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(sample.across,
                XMVectorScale(sample.normal, XMVectorGetX(XMVector3Dot(sample.normal, sample.across)))));
            XMStoreFloat4(&vertex.tangent, tangent);
            vertex.tangent.w = -1.0f;
            vertex.uv = {static_cast<float>(side), distance / settings.uvRepeatMeters};
            if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
            vertex.roadUv = {(width * 0.5f + at) / road.settings.uvRepeatMeters, distance / road.settings.uvRepeatMeters};
            if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
            result.vertices.push_back(vertex);
        }
    };
    for (float start = 0.0f; start < total; start += period) {
        const float end = std::min(start + settings.dashLengthMeters, total);
        if (end - start < 1e-3f) break;
        if (result.vertices.size() + road.rowDistances.size() * 2 > 65536 * 4) break;
        const uint32_t first = static_cast<uint32_t>(result.vertices.size());
        addRing(start);
        for (const float rowDistance : road.rowDistances) {
            if (rowDistance > start + 1e-4f && rowDistance < end - 1e-4f) addRing(rowDistance);
        }
        addRing(end);
        const uint32_t rings = (static_cast<uint32_t>(result.vertices.size()) - first) / 2;
        for (uint32_t ring = 1; ring < rings; ++ring) {
            const uint32_t a = first + (ring - 1) * 2;
            result.indices.insert(result.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
        }
    }
}

// 停止線。その向きの車線の幅いっぱいに、進行方向の手前側へ幅ぶんの帯を置く。
// 進行方向の車線は距離 [d - 幅, d]、対向車線は [d, d + 幅]（対向から見て手前）。
void BuildStopLines(const RoadGeometry& road, const RoadMarkingNodeSettings& settings, const RoadLanes& lanes,
                    renderer::MeshData& result) {
    const float total = road.rowDistances.empty() ? 0.0f : road.rowDistances.back();
    const float width = road.settings.widthMeters;
    const float half = lanes.laneWidthMeters * 0.5f;
    const auto extent = [&](bool forward, float& lo, float& hi) {
        lo = 1e30f; hi = -1e30f;
        for (size_t i = 0; i < lanes.laneCenters.size(); ++i) {
            if (lanes.laneForward[i] != forward) continue;
            lo = std::min(lo, lanes.laneCenters[i] - half);
            hi = std::max(hi, lanes.laneCenters[i] + half);
        }
        return lo < hi;
    };
    const auto addQuad = [&](float d0, float d1, float lat0, float lat1) {
        d0 = std::clamp(d0, 0.0f, total);
        d1 = std::clamp(d1, 0.0f, total);
        if (d1 - d0 < 1e-3f) return;
        const uint32_t first = static_cast<uint32_t>(result.vertices.size());
        const float corners[4][2] = {{d0, lat0}, {d0, lat1}, {d1, lat0}, {d1, lat1}};
        for (const auto& corner : corners) {
            const SurfaceSample sample = SampleRoadSurface(road, corner[0], corner[1]);
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(sample.position, XMVectorScale(sample.normal, settings.liftMeters)));
            XMStoreFloat3(&vertex.normal, sample.normal);
            const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(sample.across,
                XMVectorScale(sample.normal, XMVectorGetX(XMVector3Dot(sample.normal, sample.across)))));
            XMStoreFloat4(&vertex.tangent, tangent);
            vertex.tangent.w = -1.0f;
            // 帯の幅方向（道路の長さ方向）を U、帯の長さ方向（道路の横方向）を V にする。
            vertex.uv = {(corner[0] - d0) / (d1 - d0), (corner[1] - lat0) / (lat1 - lat0)};
            if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
            vertex.roadUv = {(width * 0.5f + corner[1]) / road.settings.uvRepeatMeters, corner[0] / road.settings.uvRepeatMeters};
            if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
            result.vertices.push_back(vertex);
        }
        const uint32_t tris[2][3] = {{first, first + 2, first + 1}, {first + 1, first + 2, first + 3}};
        for (const auto& tri : tris) {
            uint32_t a = tri[0], b = tri[1], c = tri[2];
            const XMVECTOR n = XMVector3Cross(
                XMVectorSubtract(Load(result.vertices[b].position), Load(result.vertices[a].position)),
                XMVectorSubtract(Load(result.vertices[c].position), Load(result.vertices[a].position)));
            if (XMVectorGetX(XMVector3Dot(n, Load(result.vertices[a].normal))) < 0.0f) std::swap(b, c);
            result.indices.insert(result.indices.end(), {a, b, c});
        }
    };
    for (const auto& stop : road.stopLines) {
        float lo = 0.0f, hi = 0.0f;
        const auto kind = static_cast<uint8_t>(stop.kind);
        if ((kind & 1u) && extent(true, lo, hi))
            addQuad(stop.distanceMeters - settings.stopLineWidthMeters, stop.distanceMeters, lo, hi);
        if ((kind & 2u) && extent(false, lo, hi))
            addQuad(stop.distanceMeters, stop.distanceMeters + settings.stopLineWidthMeters, lo, hi);
    }
}

// 進行方向の矢印。各車線の中央に一定間隔で置く。進行方向の車線は線形の向き、対向車線は逆向き。
void BuildArrowMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                        const RoadLanes& lanes, renderer::MeshData& result) {
    const float total = road.rowDistances.empty() ? 0.0f : road.rowDistances.back();
    const float width = road.settings.widthMeters;
    const float length = settings.arrowLengthMeters;
    if (total < length + 1.0f) return;
    const float headHalf = std::clamp(lanes.laneWidthMeters * 0.18f, 0.2f, 0.6f);
    const float shaftHalf = headHalf * 0.4f;
    const float headLength = length * 0.4f;
    const float base = length * 0.5f - headLength;
    struct Local { float s, t, u, w; };
    const Local shape[7] = {
        {-length * 0.5f, -shaftHalf, 0.5f - shaftHalf / (2.0f * headHalf), 0.0f},
        {base, -shaftHalf, 0.5f - shaftHalf / (2.0f * headHalf), (base + length * 0.5f) / length},
        {base, shaftHalf, 0.5f + shaftHalf / (2.0f * headHalf), (base + length * 0.5f) / length},
        {-length * 0.5f, shaftHalf, 0.5f + shaftHalf / (2.0f * headHalf), 0.0f},
        {base, -headHalf, 0.0f, (base + length * 0.5f) / length},
        {base, headHalf, 1.0f, (base + length * 0.5f) / length},
        {length * 0.5f, 0.0f, 0.5f, 1.0f},
    };
    const uint32_t triangles[3][3] = {{0, 1, 3}, {1, 2, 3}, {4, 6, 5}};
    for (float center = settings.arrowIntervalMeters * 0.5f; center + length * 0.5f <= total;
         center += settings.arrowIntervalMeters) {
        if (center - length * 0.5f < 0.0f) continue;
        for (size_t lane = 0; lane < lanes.laneCenters.size(); ++lane) {
            const float direction = lanes.laneForward[lane] ? 1.0f : -1.0f;
            const uint32_t first = static_cast<uint32_t>(result.vertices.size());
            for (const Local& local : shape) {
                const SurfaceSample sample =
                    SampleRoadSurface(road, center + local.s * direction, lanes.laneCenters[lane] + local.t * direction);
                renderer::MeshVertex vertex{};
                XMStoreFloat3(&vertex.position, XMVectorAdd(sample.position, XMVectorScale(sample.normal, settings.liftMeters)));
                XMStoreFloat3(&vertex.normal, sample.normal);
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(sample.across,
                    XMVectorScale(sample.normal, XMVectorGetX(XMVector3Dot(sample.normal, sample.across)))));
                XMStoreFloat4(&vertex.tangent, tangent);
                vertex.tangent.w = -1.0f;
                vertex.uv = {local.u, local.w};
                if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
                vertex.roadUv = {(width * 0.5f + lanes.laneCenters[lane] + local.t * direction) / road.settings.uvRepeatMeters,
                                 (center + local.s * direction) / road.settings.uvRepeatMeters};
                if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
                result.vertices.push_back(vertex);
            }
            for (const auto& triangle : triangles) {
                uint32_t a = first + triangle[0], b = first + triangle[1], c = first + triangle[2];
                // 向きを反転した矢印は巻きも反転するので、法線が路面と同じ側を向くよう並べ直す。
                const XMVECTOR n = XMVector3Cross(
                    XMVectorSubtract(Load(result.vertices[b].position), Load(result.vertices[a].position)),
                    XMVectorSubtract(Load(result.vertices[c].position), Load(result.vertices[a].position)));
                if (XMVectorGetX(XMVector3Dot(n, Load(result.vertices[a].normal))) < 0.0f) std::swap(b, c);
                result.indices.insert(result.indices.end(), {a, b, c});
            }
        }
    }
}
}  // namespace

bool EvaluateRoad(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result, std::string& error) {
    std::unordered_set<GraphId> visiting;
    return Evaluate(graph, nodeId, result, error, visiting);
}

RoadLanes ComputeRoadLanes(const RoadNodeSettings& settings, bool leftHandTraffic) {
    RoadLanes lanes;
    const uint32_t forward = std::max(1u, settings.lanesForward);
    const uint32_t backward = settings.lanesBackward;
    const uint32_t total = forward + backward;
    lanes.laneWidthMeters = settings.widthMeters / static_cast<float>(total);
    // Right 端（横位置 -幅/2）から順に並べる。右側通行なら進行方向の車線が Right 側、左側通行なら対向が Right 側。
    const uint32_t rightSideCount = leftHandTraffic ? backward : forward;
    for (uint32_t i = 0; i < total; ++i) {
        lanes.laneCenters.push_back(-settings.widthMeters * 0.5f + lanes.laneWidthMeters * (static_cast<float>(i) + 0.5f));
        const bool onRightSide = i < rightSideCount;
        lanes.laneForward.push_back(leftHandTraffic ? !onRightSide : onRightSide);
    }
    for (uint32_t i = 1; i < total; ++i) {
        const float boundary = -settings.widthMeters * 0.5f + lanes.laneWidthMeters * static_cast<float>(i);
        if (i == rightSideCount && backward > 0) {
            lanes.hasCenter = true;
            lanes.centerLateral = boundary;
        } else {
            lanes.dividers.push_back(boundary);
        }
    }
    return lanes;
}

namespace {
// Material 入力に繋いだ Result を合成用のスタックにする。
bool BuildStack(const NodeGraph& graph, const Pin& pin, float metersPerUv, compositor::MaterialStack& out,
                compositor::MaterialAssetId* outTopMaterial) {
    const Node* source = graph.FindUpstreamNodeForPin(pin.id);
    if (source == nullptr) return false;
    auto material = graph.CompileLayersTo(source->id);
    // 道路の材質はハイトを材質のハイトマップから読む。Surface ノードの旧地形向け設定
    // （ノイズ / 定数、持ち上げ、起伏の強さ、UV スケール）は使わない。
    for (auto& layer : material.layers) {
        layer.heightSource = compositor::ValueSource::Texture;
        layer.heightBase = 0.5f;
        layer.heightGain = 1.0f;
        layer.uvScale = 1.0f;
    }
    out.Layers() = std::move(material.layers);
    // 1 UVタイルの実寸でハイト由来の法線を評価する。
    out.SetTerrainScale(metersPerUv, 1.0f);
    if (outTopMaterial) {
        for (auto it = out.Layers().rbegin(); it != out.Layers().rend(); ++it) {
            if (it->material != compositor::kNoMaterialAsset) { *outTopMaterial = it->material; break; }
        }
    }
    return true;
}

// プロパティで指定した材質を、従来のSurfaceと同じ評価経路へ渡す。
void AttachMaterial(const std::optional<compositor::MaterialLayer>& binding, renderer::SceneMesh& mesh) {
    if (!binding) return;
    auto layer = binding->enabled ? *binding : compositor::MaterialStack::MakeBaseLayer();
    layer.heightSource = compositor::ValueSource::Texture;
    layer.heightBase = 0.5f;
    layer.heightGain = 1.0f;
    layer.uvScale = 1.0f;
    compositor::MaterialStack stack;
    stack.Layers() = {layer};
    stack.SetTerrainScale(mesh.roadMetersPerUv, 1.0f);
    mesh.blendMaterial = layer.material;
    mesh.materialStack = std::move(stack);
}

// 評価に使う値が同じ線は1メッシュにまとめる（ハイト・UVは上で正規化する）。
bool SameMarkingMaterial(const std::optional<compositor::MaterialLayer>& a,
                         const std::optional<compositor::MaterialLayer>& b) {
    if (!a || !b) return a.has_value() == b.has_value();
    if (a->enabled != b->enabled) return false;
    if (!a->enabled) return true;
    return a->material == b->material && a->channelMask == b->channelMask &&
        a->baseColor.x == b->baseColor.x && a->baseColor.y == b->baseColor.y && a->baseColor.z == b->baseColor.z &&
        a->roughness == b->roughness && a->metallic == b->metallic && a->ambientOcclusion == b->ambientOcclusion;
}

// Road のスロット 1〜4 と道路マスク。スロット 2〜4 は材質とマスクの両方が繋がったときだけ有効。
void AttachRoadLayers(const NodeGraph& graph, const Node& node, const RoadNodeSettings& settings,
                      float lengthMeters, renderer::SceneMesh& mesh, const RoadLanes* lanes,
                      const RoadGeometry* geometry) {
    std::vector<const Pin*> materialPins;
    std::vector<const Pin*> maskPins;
    for (const auto& pin : node.inputs) {
        if (pin.valueType == ValueType::Material) materialPins.push_back(&pin);
        if (pin.valueType == ValueType::RoadMask) maskPins.push_back(&pin);
    }
    mesh.roadWidthMeters = settings.widthMeters;
    mesh.roadLengthMeters = lengthMeters;
    mesh.roadUvAlongU = settings.uvAlongU;
    mesh.layerBlendRange = settings.layerBlendRange;
    for (int slot = 0; slot < kRoadMaterialSlots; ++slot) {
        mesh.layerWorldUv[slot] = settings.layerWorldUv[slot];
        mesh.layerUvRepeat[slot] = slot == 0 ? settings.uvRepeatMeters : std::max(0.01f, settings.layerUvRepeatMeters[slot]);
        mesh.layerHeightGate[slot] = std::min(2u, settings.layerHeightGate[slot]);
        mesh.layerHeightGateThreshold[slot] = std::clamp(settings.layerHeightGateThreshold[slot], 0.0f, 1.0f);
        mesh.layerHeightGateSoftness[slot] = std::clamp(settings.layerHeightGateSoftness[slot], 0.001f, 1.0f);
        mesh.layerBlendMode[slot] = std::min(1u, settings.layerBlendMode[slot]);
    }
    if (!materialPins.empty()) {
        compositor::MaterialStack stack;
        if (BuildStack(graph, *materialPins[0], settings.uvRepeatMeters, stack, &mesh.blendMaterial)) {
            mesh.materialStack = std::move(stack);
        }
    }
    const RoadMaskNodeSettings* channels[3] = {nullptr, nullptr, nullptr};
    bool anyLayer = false;
    for (int layer = 0; layer < 3; ++layer) {
        if (layer + 1 >= static_cast<int>(materialPins.size()) || layer >= static_cast<int>(maskPins.size())) continue;
        const Node* maskNode = graph.FindUpstreamNodeForPin(maskPins[layer]->id);
        const auto* maskSettings = maskNode ? std::get_if<RoadMaskNodeSettings>(&maskNode->settings) : nullptr;
        compositor::MaterialStack stack;
        if (maskSettings == nullptr ||
            !BuildStack(graph, *materialPins[layer + 1], mesh.layerUvRepeat[layer + 1], stack, nullptr))
            continue;
        mesh.layerStacks[layer] = std::move(stack);
        channels[layer] = maskSettings;
        anyLayer = true;
    }
    if (anyLayer) {
        const RoadMaskImage image = BakeRoadMask(channels, settings.widthMeters, lengthMeters, lanes, geometry);
        mesh.roadMask.width = image.width;
        mesh.roadMask.height = image.height;
        mesh.roadMask.rgba = image.rgba;
    }
}

// Mesh Outputから上流へたどり、Roadを起点に白線などの部品を順に積む。
struct MeshChain {
    std::vector<renderer::SceneMesh> meshes;
    // meshes と同じ並びで、そのメッシュを作ったノード。Merge や複数の Mesh Output で
    // 同じノードのメッシュを 2 回積まないための鍵。
    std::vector<GraphId> sources;
    std::vector<uint32_t> sourceParts;
    RoadGeometry road;
    // 道路面が chain.meshes の何番目か。白線の押し出し元にする。
    int roadIndex = -1;
};
int AppendChainMesh(MeshChain& chain, renderer::SceneMesh mesh, GraphId source, uint32_t part = 0) {
    chain.meshes.push_back(std::move(mesh));
    chain.sources.push_back(source);
    chain.sourceParts.push_back(part);
    return static_cast<int>(chain.meshes.size()) - 1;
}
// 複数の理由を「 / 」で繋いで残す。
void AppendError(std::string& errors, const std::string& error) {
    if (error.empty()) return;
    if (!errors.empty()) errors += " / ";
    errors += error;
}
// from の各メッシュを into へ写す。同じノード由来のメッシュは into にあるものを使い、
// displacementSource は写した先の番号へ付け替える。戻り値は from の番号 → into の番号。
std::vector<int> MergeChainMeshes(MeshChain& into, MeshChain& from) {
    std::vector<int> remap(from.meshes.size(), -1);
    for (size_t i = 0; i < from.meshes.size(); ++i) {
        const GraphId source = from.sources[i];
        int existing = -1;
        for (size_t j = 0; j < into.sources.size(); ++j) {
            if (into.sources[j] == source && into.sourceParts[j] == from.sourceParts[i]) { existing = static_cast<int>(j); break; }
        }
        if (existing >= 0) { remap[i] = existing; continue; }
        renderer::SceneMesh mesh = std::move(from.meshes[i]);
        if (mesh.displacementSource >= 0) {
            mesh.displacementSource = remap[static_cast<size_t>(mesh.displacementSource)];
        }
        remap[i] = AppendChainMesh(into, std::move(mesh), source, from.sourceParts[i]);
    }
    return remap;
}
// 戻り値は「道路面（chain.road）が出来たか」。白線・Decal などの部品が失敗しても
// 上流までの部品は残し、理由だけ errors に足す（途中の 1 つの失敗で道路ごと消さない）。
bool EvaluateMeshChain(const NodeGraph& graph, const Node* node, MeshChain& chain,
                       std::string& errors, std::unordered_set<GraphId>& visiting) {
    if (!node) { AppendError(errors, "Mesh OutputにRoadSurfaceを接続してください"); return false; }
    if (visiting.size() >= 64 || !visiting.insert(node->id).second) {
        AppendError(errors, "メッシュの依存が循環しているか、深すぎます"); return false;
    }
    bool success = false;
    std::string error;
    if (node->kind == NodeKind::Road) {
        if (EvaluateRoad(graph, node->id, chain.road, error)) {
            renderer::SceneMesh mesh;
            mesh.geometry = chain.road.surface;
            mesh.material.roughness = 0.85f;
            mesh.roadMetersPerUv = chain.road.settings.uvRepeatMeters;
            mesh.displacementMeters = std::max(0.0f, chain.road.settings.displacementMeters);
            // 轍などのマスクは車線の並びを見る。
            const RoadLanes lanes = ComputeRoadLanes(chain.road.settings, graph.RoadNetwork().leftHandTraffic);
            AttachRoadLayers(graph, *node, chain.road.settings,
                             chain.road.rowDistances.empty() ? 0.0f : chain.road.rowDistances.back(), mesh, &lanes, &chain.road);
            chain.roadIndex = AppendChainMesh(chain, std::move(mesh), node->id);
            success = true;
        }
    } else if (node->kind == NodeKind::Merge) {
        // 繋いだ枝を順に積む。同じノード由来のメッシュは 1 回だけ。下流の部品は最初の道路の枝の面に乗る。
        // モデルの枝（型が Model）は道路のメッシュを持たないので飛ばす（Application が別に描く）。
        bool first = true;
        bool hasModel = false;
        for (const Pin& pin : node->inputs) {
            const Node* upstream = graph.FindUpstreamNodeForPin(pin.id);
            if (!upstream) continue;
            if (graph.EffectiveOutputType(graph.FindUpstreamPin(pin.id)) == ValueType::Model) {
                hasModel = true;
                continue;
            }
            MeshChain branch;
            if (!EvaluateMeshChain(graph, upstream, branch, errors, visiting)) continue;
            const std::vector<int> remap = MergeChainMeshes(chain, branch);
            if (first) {
                chain.road = std::move(branch.road);
                chain.roadIndex = branch.roadIndex >= 0 ? remap[static_cast<size_t>(branch.roadIndex)] : -1;
                first = false;
            }
            success = true;
        }
        if (!success && !hasModel) error = "Input 1 に道路のメッシュかモデルを接続してください";
    } else if (const auto* marking = std::get_if<RoadMarkingNodeSettings>(&node->settings)) {
        const Node* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs.front().id);
        if (!upstream) {
            error = "Lane MarkingにRoadSurfaceを接続してください";
        } else if (EvaluateMeshChain(graph, upstream, chain, errors, visiting)) {
            success = true;  // 道路面はある。白線が失敗しても道路は残す。
            uint32_t remaining = 31u;
            for (size_t slot = 0; slot < marking->materials.size(); ++slot) {
                if (!(remaining & (1u << slot))) continue;
                uint32_t mask = 0;
                for (size_t other = slot; other < marking->materials.size(); ++other)
                    if ((remaining & (1u << other)) && SameMarkingMaterial(marking->materials[slot], marking->materials[other]))
                        mask |= 1u << other;
                remaining &= ~mask;
                renderer::SceneMesh mesh;
                if (!BuildRoadMarkings(chain.road, *marking, graph.RoadNetwork().leftHandTraffic, mesh.geometry, error, mask)) break;
                if (mesh.geometry.vertices.empty()) continue;
                mesh.material.baseColor = {0.85f, 0.85f, 0.82f};
                mesh.material.roughness = 0.6f;
                mesh.roadMetersPerUv = marking->uvRepeatMeters;
                // 路面と同じハイトで押し出し、材質別の各メッシュも路面へ追従させる。
                mesh.displacementMeters = std::max(0.0f, chain.road.settings.displacementMeters);
                mesh.displacementSource = chain.roadIndex;
                mesh.useBlendMode = true;
                AttachMaterial(marking->materials[slot], mesh);
                AppendChainMesh(chain, std::move(mesh), node->id, mask);
            }
        }
    } else if (const auto* decal = std::get_if<DecalNodeSettings>(&node->settings)) {
        const Node* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs.front().id);
        const Node* pathNode = node->inputs.size() > 1 ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr;
        const auto* pathSettings = pathNode ? std::get_if<PathNodeSettings>(&pathNode->settings) : nullptr;
        if (!upstream) {
            error = "DecalにRoadSurfaceを接続してください";
        } else if (EvaluateMeshChain(graph, upstream, chain, errors, visiting)) {
            success = true;  // 道路面はある。Decal が失敗しても道路は残す。
            renderer::SceneMesh mesh;
            if (!pathSettings) {
                error = "DecalのPathに面上のPathを接続してください";
            } else if (BuildDecal(chain.road, pathSettings->path, *decal, mesh.geometry, error)) {
                mesh.material.baseColor = {0.6f, 0.6f, 0.6f};
                mesh.material.roughness = 0.7f;
                mesh.roadMetersPerUv = decal->uvRepeatMeters;
                mesh.additiveHeightMeters = decal->heightMeters;
                mesh.surfaceDepthBiasMeters = std::max(0.0f, chain.road.settings.displacementMeters);
                mesh.showWireframe = decal->showWireframe;
                mesh.displacementMeters = std::max(0.0f, chain.road.settings.displacementMeters);
                mesh.displacementSource = chain.roadIndex;
                mesh.useBlendMode = true;
                AttachMaterial(decal->material, mesh);
                AppendChainMesh(chain, std::move(mesh), node->id);
            }
        }
    } else if (const auto* crack = std::get_if<CrackNodeSettings>(&node->settings)) {
        const Node* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs.front().id);
        if (!upstream) {
            error = "CrackにRoadSurfaceを接続してください";
        } else if (EvaluateMeshChain(graph, upstream, chain, errors, visiting)) {
            success = true;  // 道路面はある。ひび割れが失敗しても道路は残す。
            renderer::SceneMesh mesh;
            const RoadLanes lanes = ComputeRoadLanes(chain.road.settings, graph.RoadNetwork().leftHandTraffic);
            if (BuildCracks(chain.road, lanes, *crack, mesh.geometry, error) && !mesh.geometry.vertices.empty()) {
                mesh.material.baseColor = {0.2f, 0.2f, 0.2f};
                mesh.material.roughness = 0.8f;
                mesh.roadMetersPerUv = crack->uvRepeatMeters;
                mesh.displacementMeters = std::max(0.0f, chain.road.settings.displacementMeters);
                mesh.displacementSource = chain.roadIndex;
                mesh.useBlendMode = true;
                AttachMaterial(crack->material, mesh);
                AppendChainMesh(chain, std::move(mesh), node->id);
            }
        }
    } else if (node->kind == NodeKind::Shoulder) {
        // 路肩は Road とは別の枝。以後の白線・Decal は路肩の面に乗る。
        if (EvaluateShoulder(graph, node->id, chain.road, error)) {
            renderer::SceneMesh mesh;
            mesh.geometry = chain.road.surface;
            mesh.material.baseColor = {0.42f, 0.38f, 0.32f};
            mesh.material.roughness = 0.9f;
            mesh.roadMetersPerUv = chain.road.settings.uvRepeatMeters;
            mesh.displacementMeters = chain.road.settings.displacementMeters;
            // 材質スロットとマスクは Road と同じ。横位置は境界が Right、外側が Left。車線は無い。
            AttachRoadLayers(graph, *node, chain.road.settings,
                             chain.road.rowDistances.empty() ? 0.0f : chain.road.rowDistances.back(), mesh, nullptr, &chain.road);
            chain.roadIndex = AppendChainMesh(chain, std::move(mesh), node->id);
            success = true;
        }
    } else {
        error = "Mesh OutputにはRoad / Lane Marking / Decal / Shoulder / Merge / CrackのRoadSurfaceを接続してください";
    }
    if (!error.empty()) { const NodeDefinition* def = FindNodeDefinition(node->kind); AppendError(errors, std::string(def ? def->title : "?") + ": " + error); }
    visiting.erase(node->id);
    return success;
}
}  // namespace


RoadSurfacePoint RoadSurfacePointAt(const RoadGeometry& road, float distanceMeters, float lateralMeters) {
    RoadSurfacePoint point{};
    if (road.stride < 2 || road.surface.vertices.size() < road.stride * 2) return point;
    const SurfaceSample sample = SampleRoadSurface(road, distanceMeters, lateralMeters);
    XMStoreFloat3(&point.position, sample.position);
    XMStoreFloat3(&point.normal, sample.normal);
    return point;
}

bool RoadSurfaceCoordinates(const RoadGeometry& road, const XMFLOAT3& world, float& outDistanceMeters,
                            float& outLateralMeters) {
    const auto& v = road.surface.vertices;
    if (road.stride < 2 || v.size() < road.stride * 2) return false;
    const size_t rows = v.size() / road.stride;
    const XMVECTOR p = Load(world);
    float bestError = 1e30f;
    for (size_t row = 0; row + 1 < rows; ++row) {
        const auto rowCenter = [&](size_t r) {
            return XMVectorScale(XMVectorAdd(Load(v[r * road.stride].position), Load(v[r * road.stride + road.stride - 1].position)), 0.5f);
        };
        const auto rowAcross = [&](size_t r) {
            return XMVectorSubtract(Load(v[r * road.stride + road.stride - 1].position), Load(v[r * road.stride].position));
        };
        const XMVECTOR c0 = rowCenter(row);
        const XMVECTOR c1 = rowCenter(row + 1);
        const XMVECTOR along = XMVectorSubtract(c1, c0);
        const float alongLength = XMVectorGetX(XMVector3LengthSq(along));
        if (alongLength < 1e-8f) continue;
        const float t = std::clamp(XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, c0), along)) / alongLength, 0.0f, 1.0f);
        const XMVECTOR center = XMVectorLerp(c0, c1, t);
        const XMVECTOR across = XMVectorLerp(rowAcross(row), rowAcross(row + 1), t);
        const float acrossLength = XMVectorGetX(XMVector3LengthSq(across));
        if (acrossLength < 1e-8f) continue;
        const float s = XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, center), across)) / acrossLength;
        const XMVECTOR nearest = XMVectorAdd(center, XMVectorScale(across, s));
        const float error = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(p, nearest)));
        if (error < bestError) {
            bestError = error;
            const float d0 = road.rowDistances[row];
            const float d1 = road.rowDistances[row + 1];
            outDistanceMeters = d0 + (d1 - d0) * t;
            outLateralMeters = s * road.settings.widthMeters;
        }
    }
    return bestError < 1e29f;
}

bool RayHitsRoad(const RoadGeometry& road, const XMFLOAT3& origin, const XMFLOAT3& direction, XMFLOAT3& outHit) {
    const auto& v = road.surface.vertices;
    const auto& indices = road.surface.indices;
    const XMVECTOR o = Load(origin);
    const XMVECTOR d = Load(direction);
    float bestT = 1e30f;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        // Möller–Trumbore。裏面も拾う（面が傾いていても選べるように）。
        const XMVECTOR a = Load(v[indices[i]].position);
        const XMVECTOR e1 = XMVectorSubtract(Load(v[indices[i + 1]].position), a);
        const XMVECTOR e2 = XMVectorSubtract(Load(v[indices[i + 2]].position), a);
        const XMVECTOR pv = XMVector3Cross(d, e2);
        const float det = XMVectorGetX(XMVector3Dot(e1, pv));
        if (std::abs(det) < 1e-9f) continue;
        const float inv = 1.0f / det;
        const XMVECTOR tv = XMVectorSubtract(o, a);
        const float u = XMVectorGetX(XMVector3Dot(tv, pv)) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const XMVECTOR qv = XMVector3Cross(tv, e1);
        const float w = XMVectorGetX(XMVector3Dot(d, qv)) * inv;
        if (w < 0.0f || u + w > 1.0f) continue;
        const float t = XMVectorGetX(XMVector3Dot(e2, qv)) * inv;
        if (t > 1e-4f && t < bestT) bestT = t;
    }
    if (bestT >= 1e29f) return false;
    XMStoreFloat3(&outHit, XMVectorAdd(o, XMVectorScale(d, bestT)));
    return true;
}

const Node* FindSurfaceRoad(const NodeGraph& graph, const Node& pathNode) {
    if (pathNode.inputs.empty() || pathNode.inputs.front().valueType != ValueType::Mesh) return nullptr;
    const Node* current = graph.FindUpstreamNodeForPin(pathNode.inputs.front().id);
    for (int depth = 0; current != nullptr && depth < 64; ++depth) {
        if (current->kind == NodeKind::Road) return current;
        // Lane Marking / Decal は RoadSurface を素通しする。最初の Mesh 入力をたどる（Merge は最初の道路の枝）。
        const Pin* meshInput = nullptr;
        for (const auto& pin : current->inputs) {
            if (pin.valueType != ValueType::Mesh && pin.valueType != ValueType::Any) continue;
            if (pin.valueType == ValueType::Any &&
                graph.EffectiveOutputType(graph.FindUpstreamPin(pin.id)) != ValueType::Mesh) continue;
            meshInput = &pin;
            break;
        }
        current = meshInput ? graph.FindUpstreamNodeForPin(meshInput->id) : nullptr;
    }
    return nullptr;
}

namespace {
// 道路座標の折れ線（横位置・実距離・面からの高さ・帯の幅）。
struct SurfaceStripPoint {
    float lateral;
    float distance;
    float height;
    float width;
};
// 折れ線を約 0.25 m で刻み直し、点ごとの幅を補間して帯にする。幅方向の U は 0〜1、V は折れ線に沿った実距離÷反復長。
// 道路面と同じ位置の道路 UV を持ち、押し出しに追従する。2 点未満なら何もしない。
bool AppendSurfaceStrip(const RoadGeometry& road, const std::vector<SurfaceStripPoint>& input, float lift,
                        float uvRepeat, bool uvAlongU, renderer::MeshData& result, std::string& error, bool preserveCorners = false, float gridSpacing = 0.0f) {
    std::vector<SurfaceStripPoint> points;
    for (const auto& p : input) {
        if (points.empty() || std::hypot(p.lateral - points.back().lateral, p.distance - points.back().distance) > 1e-5f)
            points.push_back(p);
    }
    if (points.size() < 2) return true;
    std::vector<float> lengths(points.size(), 0.0f);
    for (size_t i = 1; i < points.size(); ++i)
        lengths[i] = lengths[i - 1] + std::hypot(points[i].lateral - points[i - 1].lateral, points[i].distance - points[i - 1].distance);
    const float total = lengths.back();
    if (!(total > 1e-4f) || total > 16000.0f) { error = "帯のパスが短すぎるか長すぎます"; return false; }
    // ハイトを使うDecalだけ幅方向も刻む。細い帯は長さ方向も幅に合わせる。
    const float spacing = gridSpacing > 0.0f ? gridSpacing : 0.25f;
    const size_t steps = std::max<size_t>(1, static_cast<size_t>(std::ceil(total / spacing)));
    float maxWidth = 0.0f;
    for (const auto& p : points) maxWidth = std::max(maxWidth, p.width);
    const uint32_t columns = gridSpacing > 0.0f
        ? std::max(1u, static_cast<uint32_t>(std::ceil(maxWidth / spacing))) : 1u;
    const uint32_t stride = columns + 1;
    if (result.vertices.size() + (steps + 1) * stride > 400000) { error = "帯の頂点数が多すぎます"; return false; }
    std::vector<SurfaceStripPoint> uniform;
    std::vector<float> sampleDistances;
    for (size_t i = 0; i <= steps; ++i) sampleDistances.push_back(total * static_cast<float>(i) / static_cast<float>(steps));
    if (preserveCorners) {
        sampleDistances.assign(1, 0.0f);
        for (size_t i = 1; i < lengths.size(); ++i) {
            const int subdivisions = std::max(1, static_cast<int>(std::ceil((lengths[i] - lengths[i - 1]) / 0.25f)));
            for (int j = 1; j <= subdivisions; ++j)
                sampleDistances.push_back(std::lerp(lengths[i - 1], lengths[i], float(j) / float(subdivisions)));
        }
        if (result.vertices.size() + sampleDistances.size() * stride > 400000) {
            error = "帯の頂点数が多すぎます"; return false;
        }
    }
    size_t segment = 1;
    for (const float along : sampleDistances) {
        while (segment + 1 < lengths.size() && lengths[segment] < along) ++segment;
        const float span = lengths[segment] - lengths[segment - 1];
        const float t = span > 1e-6f ? (along - lengths[segment - 1]) / span : 0.0f;
        const auto& a = points[segment - 1];
        const auto& b = points[segment];
        uniform.push_back({a.lateral + (b.lateral - a.lateral) * t, a.distance + (b.distance - a.distance) * t,
                           a.height + (b.height - a.height) * t, std::max(0.002f, a.width + (b.width - a.width) * t)});
    }
    const uint32_t base = static_cast<uint32_t>(result.vertices.size());
    for (size_t i = 0; i < uniform.size(); ++i) {
        const auto& p = uniform[i];
        const auto& prev = uniform[i == 0 ? 0 : i - 1];
        const auto& next = uniform[std::min(i + 1, uniform.size() - 1)];
        // 道路座標の平面での接線と、その法線（帯の横方向）。
        float tx = next.lateral - prev.lateral;
        float tz = next.distance - prev.distance;
        const float tl = std::hypot(tx, tz);
        if (tl < 1e-6f) { tx = 0.0f; tz = 1.0f; } else { tx /= tl; tz /= tl; }
        const float nx = -tz, nz = tx;
        const float along = sampleDistances[i];
        for (uint32_t column = 0; column <= columns; ++column) {
            const float across = static_cast<float>(column) / static_cast<float>(columns);
            const float offset = (across - 0.5f) * p.width;
            const float lateral = p.lateral + nx * offset;
            const float distance = p.distance + nz * offset;
            const SurfaceSample sample = SampleRoadSurface(road, distance, lateral);
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(sample.position, XMVectorScale(sample.normal, lift + p.height)));
            XMStoreFloat3(&vertex.normal, sample.normal);
            const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(sample.across,
                XMVectorScale(sample.normal, XMVectorGetX(XMVector3Dot(sample.normal, sample.across)))));
            XMStoreFloat4(&vertex.tangent, tangent);
            vertex.tangent.w = -1.0f;
            vertex.uv = {across, along / uvRepeat};
            if (uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
            vertex.roadUv = {(road.settings.widthMeters * 0.5f + lateral) / road.settings.uvRepeatMeters,
                             distance / road.settings.uvRepeatMeters};
            if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
            result.vertices.push_back(vertex);
        }
        if (i > 0) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t a = base + static_cast<uint32_t>(i - 1) * stride + column;
                const uint32_t tris[2][3] = {{a, a + stride, a + 1}, {a + 1, a + stride, a + stride + 1}};
                for (const auto& tri : tris) {
                    uint32_t x = tri[0], y = tri[1], z = tri[2];
                    // パスが道路を逆走する区間では巻きが反転するので、法線が路面側を向くよう並べ直す。
                    const XMVECTOR n = XMVector3Cross(
                        XMVectorSubtract(Load(result.vertices[y].position), Load(result.vertices[x].position)),
                        XMVectorSubtract(Load(result.vertices[z].position), Load(result.vertices[x].position)));
                    if (XMVectorGetX(XMVector3Dot(n, Load(result.vertices[x].normal))) < 0.0f) std::swap(y, z);
                    result.indices.insert(result.indices.end(), {x, y, z});
                }
            }
        }
    }
    return true;
}
}  // namespace

bool BuildDecal(const RoadGeometry& road, const PathSettings& surfacePath, const DecalNodeSettings& settings,
                renderer::MeshData& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (road.stride < 2 || road.surface.vertices.size() < road.stride * 2) return fail("道路面が生成されていません");
    if (!surfacePath.surfaceSpace) return fail("Path の Surface に道路の RoadSurface を繋いでください");
    if (!std::isfinite(settings.widthMeters) || settings.widthMeters < 0.05f || settings.widthMeters > 50.0f ||
        !std::isfinite(settings.liftMeters) || settings.liftMeters < 0.0f || settings.liftMeters > 0.1f ||
        !std::isfinite(settings.uvRepeatMeters) || settings.uvRepeatMeters < 0.05f || settings.uvRepeatMeters > 100.0f)
        return fail("幅は0.05〜50 m、浮かせ量は0〜0.1 m、UV反復長は0.05〜100 mにしてください");
    if (!std::isfinite(settings.heightMeters) || settings.heightMeters < 0 || settings.heightMeters > 1 ||
        !std::isfinite(settings.imageWidthScale) || settings.imageWidthScale < 0.01f || settings.imageWidthScale > 100 ||
        !std::isfinite(settings.imageLengthScale) || settings.imageLengthScale < 0.01f || settings.imageLengthScale > 100)
        return fail("凹凸量は0〜1 m、画像倍率は0.01〜100にしてください");
    const auto strands = BuildPathStrands(surfacePath);
    if (strands.empty()) return fail("Path に点を置いてください");
    for (const auto& strand : strands) {
        // 道路座標（x = 横位置, z = 実距離）で曲線を割る。帯の幅は一定。
        std::vector<SurfaceStripPoint> points;
        for (const auto& sample : SamplePathStrand(surfacePath, strand, 24)) {
            if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || !std::isfinite(sample.z)) return fail("Path の座標が不正です");
            points.push_back({sample.x, sample.z, sample.y, settings.widthMeters});
        }
        const float gridSpacing = settings.heightMeters > 0.0f ? std::min(0.25f, settings.widthMeters) : 0.0f;
        if (!AppendSurfaceStrip(road, points, settings.liftMeters, settings.uvRepeatMeters, settings.uvAlongU,
                                result, error, false, gridSpacing))
            return false;
    }
    for (auto& vertex : result.vertices) {
        float& across = settings.uvAlongU ? vertex.uv.y : vertex.uv.x;
        float& along = settings.uvAlongU ? vertex.uv.x : vertex.uv.y;
        across = 0.5f + (across - 0.5f) / settings.imageWidthScale;
        along /= settings.imageLengthScale;
    }
    if (result.vertices.empty()) return fail("Path に 2 点以上の線を置いてください");
    return true;
}

namespace {
// 決定的な乱数（xorshift32）。同じ種なら同じ並び。
struct CrackRandom {
    uint32_t state;
    explicit CrackRandom(uint32_t seed) : state(seed * 2654435761u + 0x9E3779B9u) { if (state == 0) state = 1; }
    uint32_t Next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    float Unit() { return static_cast<float>(Next() & 0xFFFFFFu) / static_cast<float>(0x1000000u); }
    float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }
    int Int(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint32_t>(hi - lo + 1)); }
    bool Chance(float p) { return Unit() < p; }
};
}  // namespace

bool BuildCracks(const RoadGeometry& road, const RoadLanes& lanes, const CrackNodeSettings& settings,
                 renderer::MeshData& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (road.stride < 2 || road.surface.vertices.size() < road.stride * 2 || road.rowDistances.size() < 2)
        return fail("道路面が生成されていません");
    if (!std::isfinite(settings.densityPer100m) || settings.densityPer100m < 0.0f || settings.densityPer100m > 200.0f ||
        !std::isfinite(settings.lengthMinMeters) || !std::isfinite(settings.lengthMaxMeters) ||
        settings.lengthMinMeters < 0.5f || settings.lengthMaxMeters < settings.lengthMinMeters || settings.lengthMaxMeters > 30.0f ||
        !std::isfinite(settings.trunkWidthMeters) || settings.trunkWidthMeters < 0.01f || settings.trunkWidthMeters > 1.0f ||
        settings.branchesMax < settings.branchesMin || settings.branchesMax > 12u ||
        !std::isfinite(settings.liftMeters) || settings.liftMeters < 0.0f || settings.liftMeters > 0.1f ||
        !std::isfinite(settings.uvRepeatMeters) || settings.uvRepeatMeters < 0.05f || settings.uvRepeatMeters > 100.0f)
        return fail("密度は0〜200、長さは0.5〜30 m、幹の幅は0.01〜1 m、枝は最大12本、浮かせ量は0〜0.1 m、UV反復長は0.05〜100 mにしてください");
    const float total = road.rowDistances.back();
    const float width = road.settings.widthMeters;
    const float margin = std::min(0.15f, width * 0.1f);
    const float halfInside = width * 0.5f - margin;
    if (total < 2.0f || halfInside <= 0.0f) return true;  // 短すぎる・狭すぎる面には置かない
    CrackRandom rng(settings.seed);
    const float expected = settings.densityPer100m * total / 100.0f;
    int count = static_cast<int>(std::floor(expected));
    if (rng.Chance(expected - static_cast<float>(count))) ++count;
    count = std::min(count, 2000);
    const float jitter = DirectX::XMConvertToRadians(std::clamp(settings.angleJitterDegrees, 0.0f, 90.0f));
    const float stepMeters = 0.4f;
    // 道路座標の平面で、向き theta（0 が長さ方向、+ が Left 側へ曲がる）のランダムウォーク。
    // 幅は 0〜1 の進み具合で決める。
    const auto walk = [&](float lateral, float distance, float theta, float length,
                          const auto& widthAt) {
        std::vector<SurfaceStripPoint> points;
        float walked = 0.0f;
        points.push_back({lateral, distance, 0.0f, widthAt(0.0f)});
        while (walked < length) {
            const float step = std::min(stepMeters, length - walked);
            theta += rng.Range(-jitter * 0.9f, jitter * 0.9f);
            lateral = std::clamp(lateral + std::sin(theta) * step, -halfInside, halfInside);
            distance = std::clamp(distance + std::cos(theta) * step, 0.05f, total - 0.05f);
            walked += step;
            points.push_back({lateral, distance, 0.0f, widthAt(std::min(1.0f, walked / length))});
            if (points.size() > 256) break;
        }
        return points;
    };
    const auto headingAt = [](const std::vector<SurfaceStripPoint>& points, size_t index) {
        const auto& a = points[index == 0 ? 0 : index - 1];
        const auto& b = points[std::min(index + 1, points.size() - 1)];
        return std::atan2(b.lateral - a.lateral, b.distance - a.distance);
    };
    // 描画される帯の小区間ごとの道路座標の範囲。曲がった道路でもUVから同じ面上で判定する。
    struct Footprint {
        float minX, minZ, maxX, maxZ;
        bool Overlaps(const Footprint& other) const {
            constexpr float Clearance = 0.05f;
            return minX < other.maxX + Clearance && maxX + Clearance > other.minX &&
                   minZ < other.maxZ + Clearance && maxZ + Clearance > other.minZ;
        }
    };
    struct OccupiedCluster {
        Footprint bounds;
        std::vector<Footprint> strips;
    };
    std::vector<OccupiedCluster> occupied;
    std::vector<Footprint> candidate;
    const auto emit = [&](const std::vector<SurfaceStripPoint>& points) {
        const size_t firstIndex = result.indices.size();
        if (!AppendSurfaceStrip(road, points, settings.liftMeters, settings.uvRepeatMeters, settings.uvAlongU, result, error, true)) return false;
        for (size_t i = firstIndex; i + 5 < result.indices.size(); i += 6) {
            Footprint bounds{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (size_t j = 0; j < 6; ++j) {
                auto uv = result.vertices[result.indices[i + j]].roadUv;
                if (road.settings.uvAlongU) std::swap(uv.x, uv.y);
                const float x = uv.x * road.settings.uvRepeatMeters;
                const float z = uv.y * road.settings.uvRepeatMeters;
                bounds.minX = std::min(bounds.minX, x); bounds.maxX = std::max(bounds.maxX, x);
                bounds.minZ = std::min(bounds.minZ, z); bounds.maxZ = std::max(bounds.maxZ, z);
            }
            candidate.push_back(bounds);
        }
        return true;
    };
    // 収まらない密度では配置数を減らす。試行回数を制限し、狭い道路でも処理を終える。
    for (int cluster = 0; cluster < count * 8 && occupied.size() < static_cast<size_t>(count); ++cluster) {
        const size_t firstVertex = result.vertices.size();
        const size_t firstIndex = result.indices.size();
        candidate.clear();
        // 中心。横位置は分布に従う。
        const float center = rng.Range(1.0f, total - 1.0f);
        float lateral = rng.Range(-halfInside, halfInside);
        if (settings.placement == CrackPlacement::WheelTracks && !lanes.laneCenters.empty()) {
            const float laneCenter = lanes.laneCenters[static_cast<size_t>(rng.Int(0, static_cast<int>(lanes.laneCenters.size()) - 1))];
            lateral = std::clamp(laneCenter + (rng.Chance(0.5f) ? 0.75f : -0.75f) + rng.Range(-0.2f, 0.2f), -halfInside, halfInside);
        } else if (settings.placement == CrackPlacement::Edges) {
            lateral = std::clamp((rng.Chance(0.5f) ? 1.0f : -1.0f) * (halfInside - 0.3f) + rng.Range(-0.2f, 0.2f), -halfInside, halfInside);
        }
        const bool transverse = settings.orientation == CrackOrientation::Transverse ||
                                (settings.orientation == CrackOrientation::Mixed && rng.Chance(std::clamp(settings.transverseRatio, 0.0f, 1.0f)));
        float length = rng.Range(settings.lengthMinMeters, settings.lengthMaxMeters);
        if (transverse && lanes.laneWidthMeters > 0.5f) length = std::min(length, lanes.laneWidthMeters);
        float theta = transverse ? DirectX::XM_PIDIV2 : 0.0f;
        if (rng.Chance(0.5f)) theta += DirectX::XM_PI;
        theta += rng.Range(-jitter * 0.15f, jitter * 0.15f);
        // 幹。中心から半分戻った所から歩く。両端を 30% まで細くする。
        const float trunkWidth = settings.trunkWidthMeters;
        const auto trunkWidthAt = [&](float t) {
            const float taper = t < 0.15f ? 0.3f + 0.7f * (t / 0.15f) : (t > 0.85f ? 0.3f + 0.7f * ((1.0f - t) / 0.15f) : 1.0f);
            return trunkWidth * taper;
        };
        const float startLateral = std::clamp(lateral - std::sin(theta) * length * 0.5f, -halfInside, halfInside);
        const float startDistance = std::clamp(center - std::cos(theta) * length * 0.5f, 0.05f, total - 0.05f);
        // 幹は方向を累積して曲げず、基準方向の左右へ交互に折る。
        // 大きな折れと路面への追従用の細分を分離し、細分数で形が変わらないようにする。
        std::vector<SurfaceStripPoint> trunk{{startLateral, startDistance, 0, trunkWidthAt(0)}};
        std::vector<size_t> bends;
        float traveled = 0;
        float turnSide = rng.Chance(0.5f) ? 1.0f : -1.0f;
        CrackRandom shortBendRng(settings.seed ^ (static_cast<uint32_t>(cluster) + 1u) * 0xC2B2AE35u);
        int shortBendsRemaining = 0;
        while (traveled < length) {
            const float heading = theta + turnSide * jitter * 0.8f * rng.Range(0.55f, 1.0f);
            float segmentLength = rng.Range(0.9f, 1.7f);
            // 一部は通常の1/3〜1/2の長さで折る。細かな揺らぎはテクスチャに任せる。
            if (shortBendsRemaining == 0 && shortBendRng.Chance(0.3f))
                shortBendsRemaining = shortBendRng.Int(2, 4);
            if (shortBendsRemaining > 0) {
                segmentLength *= shortBendRng.Range(1.0f / 3.0f, 0.5f);
                --shortBendsRemaining;
            }
            const float segment = std::min(segmentLength, length - traveled);
            const auto origin = trunk.back();
            const float dx = std::sin(heading), dz = std::cos(heading);
            float available = segment;
            if (std::abs(dx) > 1e-6f) available = std::min(available, ((dx > 0 ? halfInside : -halfInside) - origin.lateral) / dx);
            if (std::abs(dz) > 1e-6f) available = std::min(available, ((dz > 0 ? total - 0.05f : 0.05f) - origin.distance) / dz);
            if (available < 1e-4f) break;
            if (trunk.size() > 1) bends.push_back(trunk.size() - 1);
            const int steps = static_cast<int>(std::ceil(available / stepMeters));
            for (int step = 1; step <= steps; ++step) {
                const float at = available * float(step) / float(steps);
                trunk.push_back({std::clamp(origin.lateral + dx * at, -halfInside, halfInside),
                                 std::clamp(origin.distance + dz * at, 0.05f, total - 0.05f), 0,
                                 trunkWidthAt((traveled + at) / length)});
            }
            traveled += available;
            if (available < segment) break; // 道路端に沿って潰れた線を作らない。
            turnSide = -turnSide;
        }
        if (trunk.size() < 3) continue;
        if (!emit(trunk)) return false;
        // 枝。幹の途中から 30〜70° で分かれ、先端で幅 0 へ絞る。半分の確率で 1 段だけ子枝を出す。
        const int branches = rng.Int(static_cast<int>(settings.branchesMin), static_cast<int>(settings.branchesMax));
        for (size_t i = bends.size(); i > 1; --i)
            std::swap(bends[i - 1], bends[static_cast<size_t>(rng.Int(0, static_cast<int>(i) - 1))]);
        for (int b = 0; b < branches; ++b) {
            // 折れ点を優先し、その山側（旋回の外側）へ分岐する。
            const size_t at = bends.empty() ? static_cast<size_t>(rng.Int(1, static_cast<int>(trunk.size()) - 2)) :
                bends[static_cast<size_t>(b) % bends.size()];
            const auto& previous = trunk[at - 1];
            const auto& current = trunk[at];
            const auto& following = trunk[at + 1];
            const float turn = (current.distance - previous.distance) * (following.lateral - current.lateral) -
                               (current.lateral - previous.lateral) * (following.distance - current.distance);
            const float side = std::abs(turn) > 1e-6f ? (turn > 0 ? -1.0f : 1.0f) : (rng.Chance(0.5f) ? 1.0f : -1.0f);
            const float branchTheta = headingAt(trunk, at) + side * DirectX::XMConvertToRadians(rng.Range(30.0f, 70.0f));
            const float branchLength = length * std::clamp(settings.branchLengthRatio, 0.05f, 2.0f) * rng.Range(0.6f, 1.2f);
            const float branchWidth = trunkWidth * std::clamp(settings.branchWidthRatio, 0.05f, 1.0f);
            const auto branchWidthAt = [&](float t) { return branchWidth * (1.0f - t); };
            const std::vector<SurfaceStripPoint> branch = walk(trunk[at].lateral, trunk[at].distance, branchTheta, branchLength, branchWidthAt);
            if (branch.size() < 2) continue;
            if (!emit(branch)) return false;
            if (branch.size() >= 4 && rng.Chance(0.5f)) {
                const size_t mid = branch.size() / 2;
                const float childTheta = headingAt(branch, mid) - side * DirectX::XMConvertToRadians(rng.Range(30.0f, 60.0f));
                const float childWidth = branchWidthAt(static_cast<float>(mid) / static_cast<float>(branch.size() - 1));
                const auto childWidthAt = [&](float t) { return childWidth * (1.0f - t); };
                const std::vector<SurfaceStripPoint> child = walk(branch[mid].lateral, branch[mid].distance, childTheta, branchLength * 0.5f, childWidthAt);
                if (child.size() >= 2 && !emit(child)) return false;
            }
        }
        if (candidate.empty()) continue;
        Footprint bounds = candidate.front();
        for (const auto& strip : candidate) {
            bounds.minX = std::min(bounds.minX, strip.minX); bounds.maxX = std::max(bounds.maxX, strip.maxX);
            bounds.minZ = std::min(bounds.minZ, strip.minZ); bounds.maxZ = std::max(bounds.maxZ, strip.maxZ);
        }
        bool overlaps = false;
        for (const auto& previous : occupied) {
            if (!bounds.Overlaps(previous.bounds)) continue;
            for (const auto& strip : candidate) {
                if (std::any_of(previous.strips.begin(), previous.strips.end(),
                                [&](const auto& other) { return strip.Overlaps(other); })) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) break;
        }
        if (overlaps) {
            result.vertices.resize(firstVertex);
            result.indices.resize(firstIndex);
            continue;
        }
        occupied.push_back({bounds, candidate});
        if (result.vertices.size() > 400000) return fail("ひび割れの頂点数が多すぎます。密度を下げてください");
    }
    return true;
}

CompiledMeshGraph CompileMeshGraph(const NodeGraph& graph, GraphId previewNodeId) {
    CompiledMeshGraph compiled;
    // 複数の Mesh Output が同じノードのメッシュを出しても 1 回だけ積む。
    MeshChain all;
    const auto appendChain = [&](const Node* source) {
        MeshChain chain;
        std::unordered_set<GraphId> visiting;
        std::string errors;
        // 道路面が無ければ何も積まない。部品の失敗は理由だけ残して上流までを積む。
        const bool hasRoad = EvaluateMeshChain(graph, source, chain, errors, visiting);
        AppendError(compiled.error, errors);
        if (!hasRoad) return;
        MergeChainMeshes(all, chain);
    };
    // 途中のノードを見る指定があれば、そのノードまでの鎖だけを出す（Mesh Output は使わない）。
    // モデルの系統のノードを見ているときは道路を出さない（モデルは Application が描く）。
    if (const Node* preview = graph.FindNode(previewNodeId); preview != nullptr && IsModelNodeKind(preview->kind)) {
        compiled.active = true;
    } else if (preview != nullptr && IsMeshNodeKind(preview->kind)) {
        compiled.active = true;
        appendChain(preview);
    } else {
        for (const auto& node : graph.Nodes()) {
            if (node.kind != NodeKind::MeshOutput) continue;
            compiled.active = true;
            // モデル（またはモデルだけをまとめた Merge）を繋いだ Mesh Output は道路を積まない。エラーにもしない。
            const GraphId input = node.inputs.empty() ? 0 : node.inputs.front().id;
            const Node* upstream = input ? graph.FindUpstreamNodeForPin(input) : nullptr;
            if (upstream != nullptr && graph.EffectiveOutputType(graph.FindUpstreamPin(input)) == ValueType::Model) continue;
            appendChain(upstream);
        }
    }
    compiled.scene.meshes = std::move(all.meshes);
    compiled.meshSources = std::move(all.sources);
    return compiled;
}

std::vector<ModelPlacementPath> CollectOutputModels(const NodeGraph& graph, GraphId previewNodeId) {
    std::vector<ModelPlacementPath> result;
    // グラフは DAG なので経路は有限だが、枝分かれの掛け算で増えすぎないよう上限を置く。
    constexpr size_t kMaxModels = 4096;
    std::vector<GraphId> transforms;
    const auto visit = [&](auto&& self, const Node* node, int depth) -> void {
        if (node == nullptr || depth > 64 || result.size() >= kMaxModels) return;
        if (node->kind == NodeKind::Model) {
            ModelPlacementPath path;
            path.model = node->id;
            // transforms は出力側から積んでいるので、モデルに近い順へ並べ替える。
            path.transforms.assign(transforms.rbegin(), transforms.rend());
            result.push_back(std::move(path));
        } else if (node->kind == NodeKind::Transform) {
            transforms.push_back(node->id);
            if (!node->inputs.empty()) self(self, graph.FindUpstreamNodeForPin(node->inputs.front().id), depth + 1);
            transforms.pop_back();
        } else if (node->kind == NodeKind::Merge) {
            // 道路とモデルをまとめた Merge。道路の部品（白線など）の入力は道路面なので、そこから先は見ない。
            for (const Pin& pin : node->inputs) self(self, graph.FindUpstreamNodeForPin(pin.id), depth + 1);
        }
    };
    if (const Node* preview = graph.FindNode(previewNodeId); preview != nullptr) {
        // モデルの系統か Merge ならその枝のモデル、ほかの道路のノードならモデルは出さない。
        if (IsModelNodeKind(preview->kind) || preview->kind == NodeKind::Merge) visit(visit, preview, 0);
        if (IsModelNodeKind(preview->kind) || IsMeshNodeKind(preview->kind)) return result;
    }
    for (const auto& node : graph.Nodes()) {
        if (node.kind == NodeKind::MeshOutput && !node.inputs.empty())
            visit(visit, graph.FindUpstreamNodeForPin(node.inputs.front().id), 0);
    }
    return result;
}
}  // namespace tg::graph
