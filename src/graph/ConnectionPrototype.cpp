#include "graph/ConnectionPrototype.h"

#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {
float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// コンクリート平板の目地。中央を下げ、両縁を斜めに面取りする実寸の断面。
float JointDrop(float distance, const ConnectionPrototypeSettings& s) {
    const float nearest = std::round(distance / s.slabLength) * s.slabLength;
    if (nearest <= 0.0f) return 0.0f;
    const float offset = std::abs(distance - nearest);
    return s.jointDepth * (1.0f - std::clamp((offset - s.jointWidth * 0.25f) /
                                           (s.jointWidth * 0.25f), 0.0f, 1.0f));
}

bool SurfaceStack(const NodeGraph& graph, GraphId id, compositor::MaterialStack& stack) {
    const Node* node = graph.FindNode(id);
    if (!node || node->kind != NodeKind::Surface) return false;
    const auto& source = std::get<LayerNodeSettings>(node->settings).layer;
    if (!source.enabled) return false;
    stack.Layers() = {source};
    auto& layer = stack.Layers().front();
    layer.heightSource = compositor::ValueSource::Texture;
    layer.heightBase = 0.5f;
    layer.heightGain = 1.0f;
    layer.uvScale = 1.0f;
    stack.SetTerrainScale(2.0f, 1.0f);
    return true;
}
}  // namespace

float PrototypeGravelCoverage(float across, float distance, const ConnectionPrototypeSettings& s) {
    // 両側が独立したノイズで食い違わないよう、一つの境界を揺らす。
    const float phase = static_cast<float>(s.seed % 1024) * 0.17f;
    const float wave = 0.19f * std::sin(distance * 1.7f + phase) +
                       0.08f * std::sin(distance * 5.3f - phase);
    const float lateral = 1.0f - Smooth((across - s.shoulderWidth - wave) / s.lateralBlend + 0.5f);
    const float longitudinal = Smooth((distance - s.transitionCenter +
        0.55f * std::sin(across * 3.1f + phase)) / s.transitionLength + 0.5f);
    // 二方向の移行が交わっても一度だけ合成する（被覆率の和は常に 1）。
    return 1.0f - (1.0f - lateral) * (1.0f - longitudinal);
}

bool BuildConnectionPrototype(const RoadGeometry& road, const ConnectionPrototypeSettings& s,
                              renderer::SceneMesh& result, std::string& error) {
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    const float values[] = {s.shoulderWidth, s.sidewalkWidth, s.sidewalkHeight, s.lateralBlend,
        s.transitionCenter, s.transitionLength, s.sampleSpacing, road.settings.widthMeters,
        s.slabLength, s.jointWidth, s.jointDepth};
    for (float value : values) if (!std::isfinite(value)) return fail("試作の寸法が有限値ではありません");
    if (s.shoulderWidth < 0.1f || s.shoulderWidth > 10.0f || s.sidewalkWidth < 0.1f || s.sidewalkWidth > 10.0f ||
        s.sidewalkHeight < 0.01f || s.sidewalkHeight > 0.5f || s.lateralBlend < 0.01f ||
        s.transitionLength < 0.01f || s.sampleSpacing < 0.05f || s.sampleSpacing > 1.0f ||
        road.settings.widthMeters < 0.1f || road.settings.widthMeters > 20.0f)
        return fail("試作の寸法が対応範囲外です");
    if (s.slabLength < 0.5f || s.slabLength > 5.0f || s.jointWidth < 0.004f || s.jointWidth > 0.05f ||
        s.jointDepth < 0.0f || s.jointDepth > s.sidewalkHeight * 0.5f)
        return fail("仮歩道の平板・目地の寸法が対応範囲外です");
    if (road.stride < 2 || road.rowDistances.size() < 2 ||
        road.surface.vertices.size() != road.rowDistances.size() * road.stride)
        return fail("試作には道路の格子が必要です");
    const float length = road.rowDistances.back();
    if (!std::isfinite(length) || length <= 0.0f || length > 50.0f)
        return fail("試作は長さ50 m以内の道路に対応します");
    using namespace DirectX;
    const auto& vertices = road.surface.vertices;
    const XMFLOAT3 origin = vertices.front().position;
    const XMVECTOR right = XMLoadFloat3(&origin);
    const XMVECTOR across = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&vertices[road.stride - 1].position), right));
    const XMVECTOR along = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&vertices[vertices.size() - road.stride].position), right));
    if (!std::isfinite(XMVectorGetX(XMVector3Length(across))) ||
        !std::isfinite(XMVectorGetX(XMVector3Length(along))) ||
        std::abs(XMVectorGetX(XMVector3Dot(across, along))) > 1e-4f ||
        XMVectorGetY(XMVector3Cross(along, across)) < 0.99f)
        return fail("試作の道路座標系が不正です");
    // P0 は平面の直線に限定し、曲線を誤って平らにする結果は返さない。
    for (size_t row = 0; row < road.rowDistances.size(); ++row) {
        if (!std::isfinite(road.rowDistances[row]) || (row && road.rowDistances[row] <= road.rowDistances[row - 1]))
            return fail("道路の実距離が不正です");
        for (uint32_t col = 0; col < road.stride; ++col) {
            const auto& p = vertices[row * road.stride + col].position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return fail("道路の座標が不正です");
            const XMVECTOR expected = XMVectorAdd(right, XMVectorAdd(XMVectorScale(along, road.rowDistances[row]),
                XMVectorScale(across, road.settings.widthMeters * static_cast<float>(col) / static_cast<float>(road.stride - 1))));
            if (std::abs(p.y - origin.y) > 1e-4f ||
                XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&p), expected))) > 1e-4f)
                return fail("P0試作は水平な直線道路のみ対応します");
        }
    }
    renderer::SceneMesh mesh;
    mesh.roadMetersPerUv = 2.0f;
    mesh.roadLengthMeters = length;
    const float groundWidth = s.shoulderWidth + road.settings.widthMeters;
    // u は断面に沿う距離。垂直面にも幅を与えて UV と接線を縮退させない。
    const float totalWidth = groundWidth + s.sidewalkHeight + s.sidewalkWidth;
    mesh.roadWidthMeters = totalWidth;
    mesh.layerUvRepeat = {2.0f, 2.0f, 2.0f, 2.0f};
    mesh.layerBlendRange = 0.0f;
    mesh.connectionPrototype = true;
    mesh.displacementMeters = 1.0f;
    mesh.layerDisplacementMeters = {0.015f, 0.08f, 0.005f, 0.0f};
    const uint32_t regularRows = static_cast<uint32_t>(std::ceil(length / s.sampleSpacing));
    std::vector<float> distances;
    for (uint32_t row = 0; row <= regularRows; ++row)
        distances.push_back(length * static_cast<float>(row) / static_cast<float>(regularRows));
    // 小さな目地を分割密度に依存せず残す。接する面も同じ行で生成する。
    for (float center = s.slabLength; center <= length; center += s.slabLength) {
        for (float factor : {-0.5f, -0.25f, 0.0f, 0.25f, 0.5f}) {
            const float d = center + s.jointWidth * factor;
            if (d > 0.0f && d < length) distances.push_back(d);
        }
    }
    std::sort(distances.begin(), distances.end());
    distances.erase(std::unique(distances.begin(), distances.end(), [](float a, float b) {
        return std::abs(a - b) < 1e-5f;
    }), distances.end());
    const uint32_t rows = static_cast<uint32_t>(distances.size() - 1);
    std::vector<std::array<uint32_t, 2>> previousRightEdges;
    const auto panel = [&](float u0, float u1, float x0, float x1, float y0, float y1) {
        const uint32_t columns = static_cast<uint32_t>(std::ceil((u1 - u0) / s.sampleSpacing));
        const XMVECTOR tangent = XMVector3Normalize(XMVectorAdd(XMVectorScale(across, x1 - x0), XMVectorSet(0, y1 - y0, 0, 0)));
        std::vector<std::array<uint32_t, 2>> rightEdges;
        // 各行間で法線を分け、目地の斜面の法線を平板の中央へ補間しない。
        for (uint32_t segment = 0; segment < rows; ++segment) {
            const uint32_t start = static_cast<uint32_t>(mesh.geometry.vertices.size());
            if (!previousRightEdges.empty()) {
                const auto& edge = previousRightEdges[segment];
                mesh.connectionSeams.push_back({edge[0], edge[1], start, start + columns + 1});
            }
            // 長さ方向の硬い法線境界も独立した両側として検査する。
            if (segment > 0) {
                const uint32_t previousRow = start - columns - 1;
                for (uint32_t col = 0; col < columns; ++col)
                    mesh.connectionSeams.push_back({previousRow + col, previousRow + col + 1, start + col, start + col + 1});
            }
            rightEdges.push_back({start + columns, start + columns * 2 + 1});
            const float derivative = (JointDrop(distances[segment + 1], s) - JointDrop(distances[segment], s)) /
                                     (distances[segment + 1] - distances[segment]);
            for (uint32_t row = 0; row < 2; ++row) {
                const float d = distances[segment + row];
                const float drop = JointDrop(d, s);
                for (uint32_t col = 0; col <= columns; ++col) {
                    const float t = static_cast<float>(col) / static_cast<float>(columns);
                    const float height = y0 + (y1 - y0) * t;
                    const float heightRatio = height / s.sidewalkHeight;
                    renderer::MeshVertex v{};
                    XMStoreFloat3(&v.position, XMVectorAdd(right, XMVectorAdd(XMVectorScale(along, d),
                        XMVectorAdd(XMVectorScale(across, x0 + (x1 - x0) * t - s.shoulderWidth), XMVectorSet(0, height - drop * heightRatio, 0, 0)))));
                    const XMVECTOR surfaceAlong = XMVectorAdd(along, XMVectorSet(0, -derivative * heightRatio, 0, 0));
                    const XMVECTOR normal = XMVector3Normalize(XMVector3Cross(surfaceAlong, tangent));
                    XMStoreFloat3(&v.normal, normal);
                    XMStoreFloat4(&v.tangent, tangent);
                    v.tangent.w = 1.0f;
                    v.uv = {(u0 + (u1 - u0) * t) / 2.0f, d / 2.0f};
                    v.roadUv = v.uv;
                    mesh.geometry.vertices.push_back(v);
                    if (row && col) {
                        const uint32_t a = start + col - 1;
                        mesh.geometry.indices.insert(mesh.geometry.indices.end(), {a, a + columns + 1, a + 1,
                            a + 1, a + columns + 1, a + columns + 2});
                    }
                }
            }
        }
        previousRightEdges = std::move(rightEdges);
    };
    // 地表は一枚。立ち上がりの稜線では法線だけ分け、位置・共通座標を共有する。
    panel(0, groundWidth, 0, groundWidth, 0, 0);
    panel(groundWidth, groundWidth + s.sidewalkHeight, groundWidth, groundWidth, 0, s.sidewalkHeight);
    panel(groundWidth + s.sidewalkHeight, totalWidth, groundWidth, groundWidth + s.sidewalkWidth, s.sidewalkHeight, s.sidewalkHeight);
    mesh.roadMask.width = 512;
    mesh.roadMask.height = std::max(16u, static_cast<uint32_t>(std::ceil(length * 32.0f)));
    mesh.roadMask.rgba.resize(size_t(mesh.roadMask.width) * mesh.roadMask.height * 4);
    for (uint32_t y = 0; y < mesh.roadMask.height; ++y) {
        const float d = (static_cast<float>(y) + 0.5f) / static_cast<float>(mesh.roadMask.height) * length;
        for (uint32_t x = 0; x < mesh.roadMask.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(mesh.roadMask.width) * totalWidth;
            // 縁石の手前で舗装材へ移す。側面と天端の高さサンプルも同じ断面座標で連続。
            const float sidewalk = Smooth((u - groundWidth + 0.12f) / 0.12f);
            const float gravel = PrototypeGravelCoverage(u, d, s) * (1.0f - sidewalk);
            auto* pixel = &mesh.roadMask.rgba[(size_t(y) * mesh.roadMask.width + x) * 4];
            pixel[0] = static_cast<uint8_t>(std::lround(gravel * 255));
            pixel[1] = static_cast<uint8_t>(std::lround(sidewalk * 255));
            pixel[2] = 0;
            pixel[3] = 255;
        }
    }
    result = std::move(mesh);
    return true;
}

CompiledMeshGraph CompileConnectionPrototype(const NodeGraph& graph, GraphId roadId,
                                             GraphId gravelSurfaceId, GraphId sidewalkSurfaceId, bool displacement) {
    CompiledMeshGraph compiled;
    compiled.active = true;
    RoadGeometry road;
    if (!EvaluateRoad(graph, roadId, road, compiled.error)) return compiled;
    std::array<renderer::SceneMesh, 3> contexts;
    const auto readContext = [&](GraphId id, float defaultDisplacement, renderer::SceneMesh& context) {
        const Node* source = graph.FindNode(id);
        if (!source) return false;
        if (source->kind == NodeKind::Surface) {
            compositor::MaterialStack stack;
            if (!SurfaceStack(graph, id, stack)) return false;
            context.materialStack = std::move(stack);
            context.roadMetersPerUv = 2.0f;
            context.layerUvRepeat = {2, 2, 2, 2};
            context.displacementMeters = defaultDisplacement;
        } else if (source->kind == NodeKind::Road || source->kind == NodeKind::Shoulder) {
            auto input = CompileMeshGraph(graph, id);
            if (!input.error.empty() || input.scene.meshes.size() != 1 || !input.scene.meshes[0].materialStack) return false;
            context = std::move(input.scene.meshes[0]);
        } else {
            return false;
        }
        // P0では材質だけを既存評価器へ渡す。接続面の形状は一枚だけ所有する。
        context.geometry = {};
        context.materialOnly = true;
        return true;
    };
    if (!readContext(roadId, 0.015f, contexts[0]) ||
        !readContext(gravelSurfaceId, 0.08f, contexts[1]) ||
        !readContext(sidewalkSurfaceId, 0.005f, contexts[2])) {
        compiled.error = "接続試作には有効なマテリアルを持つRoadと、Surface / Road / Shoulderの接続先が必要です";
        return compiled;
    }
    renderer::SceneMesh mesh;
    ConnectionPrototypeSettings settings;
    settings.transitionCenter = road.rowDistances.back() * 0.55f;
    if (!BuildConnectionPrototype(road, settings, mesh, compiled.error)) return compiled;
    mesh.connectionSources = {1, 2, 3};
    mesh.connectionOrigins = {{{settings.shoulderWidth, 0}, {0, 0},
                              {settings.shoulderWidth + road.settings.widthMeters, 0}}};
    if (!displacement) mesh.displacementMeters = 0.0f;
    compiled.scene.meshes.push_back(std::move(mesh));
    for (auto& context : contexts) compiled.scene.meshes.push_back(std::move(context));
    return compiled;
}

}  // namespace tg::graph
