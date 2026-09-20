#include "TestSupport.h"
#include "graph/ConnectionPrototype.h"

#include <cmath>
#include <limits>

void RunConnectionPrototypeTests() {
    using namespace tg;
    using tests::Check;
    tests::Section("接続試作 — 二方向の被覆と歩道断面");
    graph::PathSettings path;
    const auto a = graph::AddPathPoint(path, 0, 0, 0);
    graph::AddPathPoint(path, 0, 24, a);
    graph::RoadGeometry road;
    std::string error;
    Check(graph::BuildRoad(path, {}, road, error), "試作用の直線道路を生成する");
    graph::ConnectionPrototypeSettings settings;
    renderer::SceneMesh mesh;
    if (!graph::BuildConnectionPrototype(road, settings, mesh, error)) {
        Check(false, "接続試作を生成する");
        return;
    }
    Check(renderer::ValidateMeshScene({{mesh}}), "接続面のインデックス・座標・接空間が有効");
    Check(mesh.roadMask.IsValid(), "共通マスクが有効");
    bool seamPairsValid = !mesh.connectionSeams.empty();
    bool hardNormalSeam = false;
    for (const auto& seam : mesh.connectionSeams) {
        for (size_t endpoint = 0; endpoint < 2; ++endpoint) {
            const auto& left = mesh.geometry.vertices[seam[endpoint]];
            const auto& right = mesh.geometry.vertices[seam[endpoint + 2]];
            seamPairsValid &= std::abs(left.position.x - right.position.x) < 1e-5f &&
                std::abs(left.position.y - right.position.y) < 1e-5f &&
                std::abs(left.position.z - right.position.z) < 1e-5f &&
                std::abs(left.roadUv.x - right.roadUv.x) < 1e-5f && std::abs(left.roadUv.y - right.roadUv.y) < 1e-5f;
            hardNormalSeam |= std::abs(left.normal.y - right.normal.y) > 0.5f;
        }
    }
    Check(seamPairsValid && hardNormalSeam, "GPU検査用の隣接辺は横・長さ方向の両側と硬い法線を保持する");
    auto invalidSeam = mesh;
    invalidSeam.connectionSeams.front()[0] = static_cast<uint32_t>(mesh.geometry.vertices.size());
    Check(!renderer::ValidateMeshScene({{invalidSeam}}), "範囲外の検査頂点をGPUへ渡さない");
    Check(graph::PrototypeGravelCoverage(0, 0, settings) == 1.0f &&
          graph::PrototypeGravelCoverage(5, 0, settings) == 0.0f &&
          graph::PrototypeGravelCoverage(5, 24, settings) == 1.0f,
          "始端は舗装と砂利路肩、終端は道路も砂利へ移る");
    bool validCoverage = true;
    for (float d = 0; d <= 24; d += 0.125f) {
        for (float x = 0; x <= 8; x += 0.125f) {
            const float gravel = graph::PrototypeGravelCoverage(x, d, settings);
            validCoverage &= std::isfinite(gravel) && gravel >= 0 && gravel <= 1;
        }
    }
    Check(validCoverage, "横と進行方向の交点でも被覆率は0〜1");
    bool areaValid = true, hasRiser = false, hasTop = false;
    for (size_t i = 0; i < mesh.geometry.indices.size(); i += 3) {
        using namespace DirectX;
        const auto& v0 = mesh.geometry.vertices[mesh.geometry.indices[i]];
        const auto& v1 = mesh.geometry.vertices[mesh.geometry.indices[i+1]];
        const auto& v2 = mesh.geometry.vertices[mesh.geometry.indices[i+2]];
        const auto cross = XMVector3Cross(XMVectorSubtract(XMLoadFloat3(&v1.position), XMLoadFloat3(&v0.position)),
                                         XMVectorSubtract(XMLoadFloat3(&v2.position), XMLoadFloat3(&v0.position)));
        areaValid &= XMVectorGetX(XMVector3Length(cross)) > 1e-7f &&
                     XMVectorGetX(XMVector3Dot(cross, XMLoadFloat3(&v0.normal))) > 0;
        hasRiser |= std::abs(v0.normal.y) < 0.01f;
        hasTop |= std::abs(v0.position.y - 0.15f) < 1e-6f;
    }
    Check(areaValid && hasRiser && hasTop, "15cmの天端と立ち上がりが縮退・裏返りなしで存在する");
    // 稜線の上下それぞれで、異なる法線の頂点が同じ位置・UVを持つことを確認。
    size_t seams = 0, riserVertices = 0;
    bool hasJoint = false;
    for (const auto& v : mesh.geometry.vertices) {
        hasJoint |= std::abs(v.position.y - (settings.sidewalkHeight - settings.jointDepth)) < 1e-6f;
        if (std::abs(v.normal.y) > 0.01f) continue;
        ++riserVertices;
        bool paired = false;
        for (const auto& other : mesh.geometry.vertices) {
            if (other.normal.y < 0.1f) continue;
            if (std::abs(v.position.x - other.position.x) < 1e-6f &&
                std::abs(v.position.y - other.position.y) < 1e-6f &&
                std::abs(v.position.z - other.position.z) < 1e-6f &&
                std::abs(v.roadUv.x - other.roadUv.x) < 1e-6f &&
                std::abs(v.roadUv.y - other.roadUv.y) < 1e-6f) { paired = true; break; }
        }
        if (paired) ++seams;
    }
    Check(seams == riserVertices && seams > 0, "縁石の上下全行で共通の変位評価座標を保持する");
    Check(hasJoint, "仮コンクリートの目地は4mmの実際のくぼみを持つ");
    renderer::SceneMesh again;
    Check(graph::BuildConnectionPrototype(road, settings, again, error) &&
          again.roadMask.rgba == mesh.roadMask.rgba && again.geometry.indices == mesh.geometry.indices,
          "同じ入力と種で生成が安定する");
    settings.sampleSpacing = 0;
    Check(!graph::BuildConnectionPrototype(road, settings, again, error) && !again.geometry.vertices.empty(),
          "不正な分割間隔は拒否し、前の出力を壊さない");
    settings.sampleSpacing = 0.25f;
    road.surface.vertices[0].position.y = 0.1f;
    Check(!graph::BuildConnectionPrototype(road, settings, again, error), "未対応の非水平な道路を黙って変形しない");
    settings.sidewalkHeight = std::numeric_limits<float>::quiet_NaN();
    Check(!graph::BuildConnectionPrototype(road, settings, again, error), "非有限の寸法を拒否する");
    tests::Section("接続試作 — 4層ずつの材質構成を保持");
    graph::NodeGraph graph;
    const auto pathId = graph.CreateNode(graph::NodeKind::Path);
    std::get<graph::PathNodeSettings>(graph.FindMutableNode(pathId)->settings).path = path;
    std::array<graph::GraphId, 2> roads;
    for (size_t group = 0; group < roads.size(); ++group) {
        const auto roadId = graph.CreateNode(graph::NodeKind::Road);
        roads[group] = roadId;
        graph.CreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(roadId)->inputs[0].id);
        for (size_t slot = 0; slot < 4; ++slot) {
            const auto surface = graph.CreateNode(graph::NodeKind::Surface);
            std::get<graph::LayerNodeSettings>(graph.FindMutableNode(surface)->settings).layer.roughness =
                0.1f * static_cast<float>(1 + group * 4 + slot);
            graph.CreateLink(graph.FindNode(surface)->outputs[0].id, graph.FindNode(roadId)->inputs[slot + 1].id);
            if (slot > 0) {
                const auto mask = graph.CreateNode(graph::NodeKind::RoadMask);
                graph.CreateLink(graph.FindNode(mask)->outputs[0].id, graph.FindNode(roadId)->inputs[slot + 4].id);
            }
        }
        auto& config = std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings);
        config.uvRepeatMeters = 1.3f + static_cast<float>(group);
        for (size_t slot = 0; slot < 4; ++slot) config.layerUvRepeatMeters[slot] = static_cast<float>(slot + 1);
        config.layerWorldUv[2] = true;
        config.uvAlongU = group != 0;
        config.layerHeightGate[1] = 2;
        config.layerHeightGateThreshold[1] = 0.37f;
        config.layerBlendMode[2] = 1;
        config.displacementMeters = 0.01f + 0.03f * static_cast<float>(group);
    }
    const auto sidewalk = graph.CreateNode(graph::NodeKind::Surface);
    const auto compiled = graph::CompileConnectionPrototype(graph, roads[0], roads[1], sidewalk);
    Check(compiled.error.empty() && compiled.scene.meshes.size() == 4 && renderer::ValidateMeshScene(compiled.scene),
          "道路4層・接続先4層・歩道を一つの接続面としてコンパイルする");
    if (compiled.scene.meshes.size() == 4) {
        for (size_t group = 0; group < 2; ++group) {
            const auto original = graph::CompileMeshGraph(graph, roads[group]);
            const auto& context = compiled.scene.meshes[group + 1];
            const auto& expected = original.scene.meshes[0];
            bool sameLayers = context.materialStack.has_value();
            for (size_t slot = 0; slot < 3; ++slot)
                sameLayers &= context.layerStacks[slot] && expected.layerStacks[slot] &&
                    context.layerStacks[slot]->Layers()[0].roughness == expected.layerStacks[slot]->Layers()[0].roughness;
            Check(sameLayers && context.geometry.vertices.empty() && context.materialOnly &&
                  context.roadMask.rgba == expected.roadMask.rgba && context.layerUvRepeat == expected.layerUvRepeat &&
                  context.layerWorldUv == expected.layerWorldUv && context.roadUvAlongU == expected.roadUvAlongU &&
                  context.layerHeightGate == expected.layerHeightGate && context.layerBlendMode == expected.layerBlendMode &&
                  context.layerHeightGateThreshold == expected.layerHeightGateThreshold &&
                  context.displacementMeters == expected.displacementMeters,
                  "接続前のマスク・全レイヤー・UV・高さ条件・変位量を保持する");
        }
        auto invalid = compiled.scene;
        invalid.meshes[0].connectionSources[1] = 100;
        Check(!renderer::ValidateMeshScene(invalid), "範囲外の材質構成参照をGPUへ渡さない");
        invalid = compiled.scene;
        invalid.meshes[0].connectionSources[2] = -1;
        Check(!renderer::ValidateMeshScene(invalid), "欠けた材質構成参照を拒否する");
        const auto flat = graph::CompileConnectionPrototype(graph, roads[0], roads[1], sidewalk, false);
        Check(flat.scene.meshes[0].displacementMeters == 0 && flat.scene.meshes[2].displacementMeters > 0,
              "変位なし表示は元プリセットの変位設定を書き換えない");
    }

}
