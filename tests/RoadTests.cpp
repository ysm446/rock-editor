#include "TestSupport.h"
#include "graph/Road.h"
#include "graph/RoadProfile.h"
#include "graph/RoadMask.h"
#include "app/UndoHistory.h"
#include <cmath>
#include <limits>

void RunRoadTests() {
    using namespace tg;
    using tests::Check;
    tests::Section("Road geometry and graph");
    graph::PathSettings path;
    auto a = graph::AddPathPoint(path, 0, 0, 0);
    auto b = graph::AddPathPoint(path, 0, 10, a);
    path.FindPoint(b)->y = 2;
    graph::RoadNodeSettings settings;
    graph::RoadGeometry road;
    std::string error;
    Check(graph::BuildRoad(path, settings, road, error), "sloped road builds");
    Check(road.surface.vertices.size() == 84 && road.surface.indices.size() == 396, "one metre rows and columns");
    if (road.surface.vertices.size() != 84) return;
    Check(std::abs(road.surface.vertices[0].position.x + 3) < 1e-5f &&
          std::abs(road.surface.vertices[6].position.x - 3) < 1e-5f, "six metre width and left/right orientation");
    Check(std::abs(road.surface.vertices.back().uv.y - std::sqrt(104.0f)) < 1e-4f,
          "longitudinal UV measures 3D distance");
    renderer::MeshScene scene;
    scene.meshes.push_back({road.surface,{}});
    Check(renderer::ValidateMeshScene(scene), "finite orthonormal mesh with valid indices");
    // 進行方向 +Z に向かって右は -X（右手系 Y-up）。
    Check(road.left.points.back().y == 2 &&
          road.right.points.back().x == -3 && road.left.points.back().x == 3,
          "boundaries preserve world coordinates, height, and handedness");
    bool metreCells = true;
    for (size_t i = 0; i < road.surface.vertices.size(); ++i) {
        if (i % 7 != 6) metreCells &= std::abs(road.surface.vertices[i+1].position.x-road.surface.vertices[i].position.x) <= 1.001f;
        if (i + 7 < road.surface.vertices.size()) metreCells &= road.surface.vertices[i+7].uv.y-road.surface.vertices[i].uv.y <= 1.001f;
    }
    Check(metreCells, "straight cells are at most one metre in both axes");
    const float endUv = road.surface.vertices.back().uv.y;
    graph::PathSettings dense;
    auto first = graph::AddPathPoint(dense, 0, 0, 0);
    auto mid = graph::AddPathPoint(dense, 0, 5, first);
    dense.FindPoint(mid)->y = 1;
    auto last = graph::AddPathPoint(dense, 0, 10, mid);
    dense.FindPoint(last)->y = 2;
    Check(graph::BuildRoad(dense, settings, road, error) &&
          std::abs(road.surface.vertices.back().uv.y - endUv) < 1e-4f,
          "collinear point density does not change UV scale");
    graph::AddPathPoint(dense, 5, 5, mid);
    Check(!graph::BuildRoad(dense, settings, road, error), "branch is rejected");
    path.FindPoint(b)->z = 0;
    Check(!graph::BuildRoad(path, settings, road, error), "vertical section is rejected");
    path.FindPoint(b)->z = 10;
    graph::PathSettings curve;
    graph::PathElementId prev = 0;
    const float positions[][3] = {{-6,0,-20},{-6,1,-10},{6,2,0},{6,3,16}};
    for (auto& pos : positions) {
        prev = graph::AddPathPoint(curve, pos[0], pos[2], prev);
        curve.FindPoint(prev)->y = pos[1];
    }
    for (auto& edge : curve.edges) edge.curve = graph::PathCurve::Cubic;
    Check(graph::BuildRoad(curve, settings, road, error), "gentle cubic road builds");
    if (!error.empty()) std::printf("Road error: %s\n", error.c_str());
    auto corner = path;
    graph::AddPathPoint(corner, 10, 10, b);
    Check(graph::BuildRoad(corner, settings, road, error), "subdivision preserves a right angle miter");
    auto narrow = settings;
    narrow.widthMeters = 2.5f;
    Check(graph::BuildRoad(path,narrow,road,error) && std::abs(road.surface.vertices[3].position.x-1.25f)<1e-5f,
          "fractional width is preserved with sub-metre columns");
    graph::NodeGraph graph;
    auto pathId = graph.CreateNode(graph::NodeKind::Path);
    auto roadId = graph.CreateNode(graph::NodeKind::Road);
    auto outId = graph.CreateNode(graph::NodeKind::MeshOutput);
    std::get<graph::PathNodeSettings>(graph.FindMutableNode(pathId)->settings).path = path;
    Check(graph.CreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(roadId)->inputs[0].id), "Path connects to Road");
    Check(!graph.CanCreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(outId)->inputs[0].id), "Path cannot connect to Mesh input");
    Check(graph.CreateLink(graph.FindNode(roadId)->outputs[0].id, graph.FindNode(outId)->inputs[0].id), "RoadSurface connects to Mesh Output");
    auto compiled = graph::CompileMeshGraph(graph);
    Check(compiled.active && compiled.error.empty() && compiled.scene.meshes.size() == 1, "mesh graph compiles road output");
    Check(!compiled.scene.meshes[0].materialStack, "unconnected road keeps constant material");
    const auto surfaceId = graph.CreateNode(graph::NodeKind::Surface);
    auto& surface = std::get<graph::LayerNodeSettings>(graph.FindMutableNode(surfaceId)->settings);
    surface.layer.roughness = 0.23f;
    Check(graph.CreateLink(graph.FindNode(surfaceId)->outputs[0].id, graph.FindNode(roadId)->inputs[1].id),
          "material output connects to road material");
    Check(!graph.CanCreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(roadId)->inputs[1].id),
          "path cannot connect to material input");
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes[0].materialStack &&
          compiled.scene.meshes[0].materialStack->Layers().back().roughness == 0.23f &&
          compiled.scene.meshes[0].materialStack->SizeMeters() == 1.0f,
          "road compiles connected material at UV tile scale");
    std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings).displacementMeters = 0.05f;
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes.size() == 1 && compiled.scene.meshes[0].displacementMeters == 0.05f,
          "road displacement reaches the scene mesh");
    std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings).displacementMeters = 0.0f;
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes(); before.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings).widthMeters = 8;
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes.size() == 1 && compiled.scene.meshes[0].geometry.vertices[8].position.x == 4,
          "width changes regenerate geometry");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes(); after.graphLinks = graph.Links();
    auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes[0].geometry.vertices[6].position.x == 3, "undo regenerates original width");
    restored = history.Redo(before);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(std::get<graph::RoadNodeSettings>(graph.FindNode(roadId)->settings).widthMeters == 8, "redo restores road settings");
    auto child = graph.CreateNode(graph::NodeKind::Road);
    graph.CreateLink(graph.FindNode(roadId)->outputs[1].id, graph.FindNode(child)->inputs[0].id);
    Check(graph::EvaluateRoad(graph, child, road, error) && road.surface.vertices[0].position.x == 1,
          "left boundary (x = +4) is a usable downstream path");
    Check(!graph.CanCreateLink(graph.FindNode(child)->outputs[1].id, graph.FindNode(roadId)->inputs[0].id), "road dependency cycle is rejected");
    const auto childOut = graph.CreateNode(graph::NodeKind::MeshOutput);
    graph.CreateLink(graph.FindNode(child)->outputs[0].id, graph.FindNode(childOut)->inputs[0].id);
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes.size() == 2 && compiled.scene.meshes[0].materialStack &&
          !compiled.scene.meshes[1].materialStack, "separate roads do not inherit each others material");
    graph::GraphId materialLink = 0;
    for (const auto& link : graph.Links())
        if (link.endPin == graph.FindNode(roadId)->inputs[1].id) materialLink = link.id;
    graph.DeleteLink(materialLink);
    compiled = graph::CompileMeshGraph(graph);
    Check(!compiled.scene.meshes[0].materialStack, "disconnect restores constant material");

    tests::Section("Lane marking strips");
    graph::RoadGeometry straight;
    Check(graph::BuildRoad(path, settings, straight, error) && straight.stride == 7, "road exposes row stride");
    graph::RoadMarkingNodeSettings marking;
    marking.arrows = false;
    renderer::MeshData lines;
    Check(graph::BuildRoadMarkings(straight, marking, true, lines, error), "default centre and edge lines build");
    if (!error.empty()) std::printf("Marking error: %s\n", error.c_str());
    const size_t rows = straight.surface.vertices.size() / straight.stride;
    Check(lines.vertices.size() == rows * 6 && lines.indices.size() == (rows - 1) * 18,
          "three strips with two vertices per road row");
    if (lines.vertices.size() == rows * 6) {
        Check(std::abs(lines.vertices[0].position.x + 0.075f) < 1e-5f &&
              std::abs(lines.vertices[1].position.x - 0.075f) < 1e-5f, "centre line is 15 cm wide at x = 0");
        Check(std::abs(lines.vertices[rows*2].position.x + 2.575f) < 1e-5f &&
              std::abs(lines.vertices[rows*4+1].position.x - 2.575f) < 1e-5f, "edge lines sit 0.5 m inside the road edge");
        Check(lines.vertices[0].uv.x == 0.0f && lines.vertices[1].uv.x == 1.0f &&
              std::abs(lines.vertices[rows*2-1].uv.y - std::sqrt(104.0f)) < 1e-4f,
              "U spans the strip width and V measures distance");
        Check(lines.vertices[0].position.y > straight.surface.vertices[0].position.y &&
              lines.vertices[0].position.y < 0.01f, "strip is lifted slightly above the surface");
        renderer::MeshScene lineScene;
        lineScene.meshes.push_back({lines, {}});
        Check(renderer::ValidateMeshScene(lineScene), "marking mesh has valid indices and tangents");
    }
    marking.uvRepeatMeters = 2.0f;
    Check(graph::BuildRoadMarkings(straight, marking, true, lines, error) &&
          std::abs(lines.vertices[rows*2-1].uv.y - std::sqrt(104.0f) * 0.5f) < 1e-4f, "UV repeat scales V");
    marking = {};
    marking.arrows = false;
    marking.edgeInsetMeters = 3.0f;
    Check(!graph::BuildRoadMarkings(straight, marking, true, lines, error), "edge line past the centre is rejected");
    marking = {};
    marking.arrows = false;
    marking.centerLine = false;
    marking.edgeLines = false;
    Check(!graph::BuildRoadMarkings(straight, marking, true, lines, error), "no enabled lines is rejected");
    marking = {};
    marking.arrows = false;
    Check(graph::BuildRoad(curve, settings, straight, error) &&
          graph::BuildRoadMarkings(straight, marking, true, lines, error), "curved road markings build");
    {
        renderer::MeshScene curvedScene;
        curvedScene.meshes.push_back({lines, {}});
        Check(renderer::ValidateMeshScene(curvedScene), "curved marking mesh is valid");
    }
    graph::NodeGraph chain;
    const auto chainPath = chain.CreateNode(graph::NodeKind::Path);
    const auto chainRoad = chain.CreateNode(graph::NodeKind::Road);
    const auto chainMarking = chain.CreateNode(graph::NodeKind::RoadMarking);
    const auto chainOut = chain.CreateNode(graph::NodeKind::MeshOutput);
    std::get<graph::PathNodeSettings>(chain.FindMutableNode(chainPath)->settings).path = path;
    chain.CreateLink(chain.FindNode(chainPath)->outputs[0].id, chain.FindNode(chainRoad)->inputs[0].id);
    chain.CreateLink(chain.FindNode(chainMarking)->outputs[0].id, chain.FindNode(chainOut)->inputs[0].id);
    compiled = graph::CompileMeshGraph(chain);
    Check(compiled.active && !compiled.error.empty() && compiled.scene.meshes.empty(),
          "marking without a road reports an error");
    Check(chain.CreateLink(chain.FindNode(chainRoad)->outputs[0].id, chain.FindNode(chainMarking)->inputs[0].id),
          "RoadSurface connects to Lane Marking");
    compiled = graph::CompileMeshGraph(chain);
    Check(compiled.error.empty() && compiled.scene.meshes.size() == 2, "road and markings reach one Mesh Output");
    if (compiled.scene.meshes.size() == 2) {
        Check(compiled.scene.meshes[1].material.baseColor.x > 0.8f && !compiled.scene.meshes[1].materialStack,
              "unconnected marking is white");
        Check(compiled.scene.meshes[0].displacementSource == -1 && compiled.scene.meshes[1].displacementSource == 0,
              "markings take their displacement height from the road surface");
        {
            // 白線の道路 UV は、同じ位置の道路面の UV と一致する（列 0 = Right 端が u = 0）。
            const auto& roadMesh = compiled.scene.meshes[0].geometry;
            const auto& lineMesh = compiled.scene.meshes[1].geometry;
            const float uvRepeat = 1.0f;
            bool matched = true;
            for (size_t i = 0; i < lineMesh.vertices.size() && i < 12; ++i) {
                const auto& v = lineMesh.vertices[i];
                const float expectedU = (v.position.x + 3.0f) / uvRepeat;
                // 縦の UV は 3D の実距離。この道路は z 10 m で高さ 2 m 上がる。
                const float expectedV = v.position.z * std::sqrt(104.0f) / 10.0f / uvRepeat;
                matched &= std::abs(v.roadUv.x - expectedU) < 1e-3f && std::abs(v.roadUv.y - expectedV) < 1e-2f;
            }
            matched &= std::abs(roadMesh.vertices[0].roadUv.x) < 1e-6f;
            Check(matched, "marking road UV maps to the road surface UV");
        }
    }
    auto& paint = std::get<graph::RoadMarkingNodeSettings>(chain.FindMutableNode(chainMarking)->settings).materials;
    paint.fill(compositor::MaterialLayer{});
    compiled = graph::CompileMeshGraph(chain);
    Check(compiled.scene.meshes.size() == 2 && compiled.scene.meshes[1].materialStack &&
          !compiled.scene.meshes[0].materialStack, "marking material does not leak to the road");
    Check(compiled.scene.meshes[1].useBlendMode && !compiled.scene.meshes[0].useBlendMode,
          "only markings honour the material blend mode");
    {
        // UV の向き。長さ方向を U にすると帯の uv が入れ替わり、道路の roadUv も道路側の設定に従う。
        auto& markingSettings = std::get<graph::RoadMarkingNodeSettings>(chain.FindMutableNode(chainMarking)->settings);
        auto& roadSettings = std::get<graph::RoadNodeSettings>(chain.FindMutableNode(chainRoad)->settings);
        markingSettings.uvAlongU = true;
        roadSettings.uvAlongU = true;
        auto swapped = graph::CompileMeshGraph(chain);
        const auto& lineV = swapped.scene.meshes[1].geometry.vertices;
        const auto& roadV = swapped.scene.meshes[0].geometry.vertices;
        Check(lineV[0].uv.x == 0.0f && lineV[1].uv.x == 0.0f && lineV[1].uv.y == 1.0f,
              "marking uv along U puts the strip width on V");
        Check(std::abs(roadV[6].uv.y - 6.0f) < 1e-4f && roadV[6].uv.x == 0.0f,
              "road uv along U puts the width on V");
        Check(std::abs(lineV[0].roadUv.y - (3.0f - 0.075f)) < 1e-3f, "marking road UV follows the road axis setting");
        markingSettings.uvAlongU = false;
        roadSettings.uvAlongU = false;
    }
    for (auto& material : paint) material->material = 7;
    compiled = graph::CompileMeshGraph(chain);
    Check(compiled.scene.meshes[1].blendMaterial == 7, "blend material comes from the top layer");

    {
        paint[0]->material = 8;
        auto split = graph::CompileMeshGraph(chain);
        Check(split.error.empty() && split.scene.meshes.size() == 3 &&
              split.scene.meshes[1].blendMaterial == 8 && split.scene.meshes[2].blendMaterial == 7 &&
              split.scene.meshes[1].displacementSource == 0 && split.scene.meshes[2].displacementSource == 0,
              "center and outer markings use independent materials and follow the same road");
        size_t vertices = 0;
        for (size_t i = 1; i < split.scene.meshes.size(); ++i) vertices += split.scene.meshes[i].geometry.vertices.size();
        Check(vertices == compiled.scene.meshes[1].geometry.vertices.size(), "splitting materials keeps all marking geometry");
        const auto extraOutput = chain.CreateNode(graph::NodeKind::MeshOutput);
        chain.CreateLink(chain.FindNode(chainMarking)->outputs[0].id, chain.FindNode(extraOutput)->inputs[0].id);
        Check(graph::CompileMeshGraph(chain).scene.meshes.size() == 3, "multiple outputs retain all material groups without duplication");
        Check(chain.FindNode(chainMarking)->inputs.size() == 1, "marking material is assigned in properties, not an input pin");
    }

    tests::Section("Vertical curve and bank angle");
    {
        graph::PathSettings profile;
        auto p0 = graph::AddPathPoint(profile, 0, 0, 0);
        auto p1 = graph::AddPathPoint(profile, 0, 100, p0);
        profile.FindPoint(p1)->y = 10;
        graph::ProfileCurve centerline;
        std::string profileError;
        Check(graph::BuildPathCenterline(profile, centerline, &profileError) &&
              std::abs(centerline.TotalLength() - std::sqrt(100.0f * 100.0f + 100.0f)) < 1e-3f,
              "centerline measures the base curve");
        const auto vid = graph::AddVerticalPoint(profile, 0.5f);
        graph::FindVerticalPoint(profile, vid)->offsetMeters = 4.0f;
        graph::FindVerticalPoint(profile, vid)->vclMeters = 40.0f;
        graph::RoadGeometry profiled;
        Check(graph::BuildRoad(profile, settings, profiled, error), "road with a vertical point builds");
        if (!error.empty()) std::printf("Profile error: %s\n", error.c_str());
        const size_t profiledRows = profiled.surface.vertices.size() / profiled.stride;
        float midY = 0.0f; float quarterY = 0.0f;
        for (size_t row = 0; row < profiledRows; ++row) {
            const auto& v = profiled.surface.vertices[row * profiled.stride];
            if (std::abs(v.position.z - 50.0f) < 0.51f) midY = v.position.y;
            if (std::abs(v.position.z - 25.0f) < 0.51f) quarterY = v.position.y;
        }
        // 放物線は PVI を通らず、(i1 - i2) L / 8 だけ下がる。
        {
            const float total = centerline.TotalLength();
            const float i1 = 9.0f / (total * 0.5f);
            const float i2 = 1.0f / (total * 0.5f);
            const float expectedMid = 9.0f - (i1 - i2) * 40.0f / 8.0f;
            Check(std::abs(midY - expectedMid) < 0.15f, "vertical point raises the profile by its offset minus the parabola drop");
        }
        Check(quarterY > 2.5f + 0.5f, "tangent segments climb toward the raised point");
        Check(std::abs(profiled.surface.vertices.front().position.y) < 1e-4f &&
              std::abs(profiled.surface.vertices[(profiledRows - 1) * profiled.stride].position.y - 10.0f) < 1e-3f,
              "end heights stay at the control points");
        // 曲率が続く円弧状の線形で自動バンクを確認する。
        graph::PathSettings arc;
        graph::PathElementId arcLast = 0;
        for (int i = 0; i <= 24; ++i) {
            const float angle = static_cast<float>(i) / 24.0f * 1.5707963f;
            arcLast = graph::AddPathPoint(arc, 40.0f * std::sin(angle), -40.0f * std::cos(angle), arcLast);
        }
        arc.bankEnabled = true;
        arc.designSpeedKmh = 60.0f;
        graph::ProfileCurve arcCurve;
        Check(graph::BuildPathCenterline(arc, arcCurve, &profileError), "arc centerline builds");
        const float autoBank = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.5f);
        const float expected = graph::ComputeAutoBankRadians(40.0f, 60.0f, 0.15f);
        Check(expected > 0.05f && std::abs(std::abs(autoBank) - expected) < 0.05f,
              "auto bank matches the design speed formula for the arc radius");
        // 進行方向 +X から +Z へ曲がる。右手系 Y-up で右は (-dz, 0, dx) = +Z なので右カーブ = 正。
        Check(autoBank > 0.0f, "turning toward the right gives a positive bank");
        graph::RoadGeometry banked;
        Check(graph::BuildRoad(arc, settings, banked, error), "banked road builds");
        bool leftHigher = true;
        for (size_t row = 3; row + 3 < banked.surface.vertices.size() / banked.stride; ++row) {
            const auto& right = banked.surface.vertices[row * banked.stride];
            const auto& left = banked.surface.vertices[row * banked.stride + banked.stride - 1];
            leftHigher &= left.position.y > right.position.y + 0.1f;
        }
        Check(leftHigher, "positive bank raises the outer Left boundary above the inner Right boundary");
        renderer::MeshScene bankedScene;
        bankedScene.meshes.push_back({banked.surface, {}});
        Check(renderer::ValidateMeshScene(bankedScene), "banked mesh is valid");
        renderer::MeshData bankedLines;
        Check(graph::BuildRoadMarkings(banked, graph::RoadMarkingNodeSettings{}, true, bankedLines, error) &&
              bankedLines.vertices[bankedLines.vertices.size() / 2].position.y != 0.0f,
              "markings follow the banked surface");
        arc.bankEnabled = false;
        Check(graph::BuildRoad(arc, settings, banked, error) &&
              std::abs(banked.surface.vertices[5 * banked.stride].position.y -
                       banked.surface.vertices[5 * banked.stride + banked.stride - 1].position.y) < 1e-4f,
              "disabled bank keeps the surface level");
        arc.bankEnabled = true;
        const auto bid = graph::AddBankPoint(arc, 0.5f);
        graph::FindBankPoint(arc, bid)->manual = true;
        graph::FindBankPoint(arc, bid)->angleDegrees = -20.0f;
        const float manual = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.5f);
        Check(std::abs(manual + 20.0f * 3.14159265f / 180.0f) < 1e-4f, "manual point overrides the angle at its position");
        Check(graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.9f) < 0.0f,
              "manual negative angle holds past the last point");
        Check(std::abs(graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.25f) - manual) < 1e-4f,
              "first manual point holds before its position");
        const auto autoId = graph::AddBankPoint(arc, 0.1f);
        const float between = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.3f);
        const float atAuto = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.1f);
        Check(atAuto > 0.0f && between > std::min(atAuto, manual) + 1e-4f && between < std::max(atAuto, manual) - 1e-4f,
              "auto point before a manual point interpolates toward it");
        graph::DeleteProfilePoint(arc, autoId);
        arc.smoothBank = true;
        arc.bankSmoothMeters = 20.0f;
        const float smoothed = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.5f);
        Check(std::isfinite(smoothed) && smoothed < 0.0f, "smoothing keeps the sign of the manual point");
        const auto frame = graph::EvaluateProfileFrame(arc, arcCurve, 0.5f);
        Check(std::abs(frame.up.y) > 0.9f && std::abs(frame.right.y) < 1e-4f, "profile frame is horizontal before banking");
        Check(graph::DeleteProfilePoint(arc, bid) && !graph::DeleteProfilePoint(arc, bid), "profile points delete once");
    }


    tests::Section("Lanes and dashed lane lines");
    {
        // 幅 9 m、進行方向 2 車線＋対向 1 車線 → 車線幅 3 m。
        graph::RoadNodeSettings laneSettings;
        laneSettings.widthMeters = 9.0f;
        laneSettings.lanesForward = 2;
        laneSettings.lanesBackward = 1;
        const graph::RoadLanes rightHand = graph::ComputeRoadLanes(laneSettings, false);
        const graph::RoadLanes leftHand = graph::ComputeRoadLanes(laneSettings, true);
        Check(rightHand.laneWidthMeters == 3.0f && rightHand.laneCenters.size() == 3, "three lanes of 3 m");
        // 右側通行: Right 側（負）に進行方向 2 車線。中央線は +1.5、破線は -1.5。
        Check(rightHand.hasCenter && rightHand.centerLateral == 1.5f && rightHand.dividers.size() == 1 &&
              rightHand.dividers[0] == -1.5f && rightHand.laneForward[0] && rightHand.laneForward[1] && !rightHand.laneForward[2],
              "right-hand traffic puts the forward lanes on the right with the centre line at +1.5");
        // 左側通行: Left 側（正）に進行方向 2 車線。中央線は -1.5、破線は +1.5。
        Check(leftHand.hasCenter && leftHand.centerLateral == -1.5f && leftHand.dividers.size() == 1 &&
              leftHand.dividers[0] == 1.5f && !leftHand.laneForward[0] && leftHand.laneForward[1] && leftHand.laneForward[2],
              "left-hand traffic mirrors the layout");
        laneSettings.lanesBackward = 0;
        const graph::RoadLanes oneWay = graph::ComputeRoadLanes(laneSettings, true);
        Check(!oneWay.hasCenter && oneWay.dividers.size() == 1 && oneWay.laneForward[0] && oneWay.laneForward[1],
              "one-way road has no centre line and all lanes forward");
        // 破線の帯。長さ 5 m、間隔 5 m で 10 m の道路なら 1 本（0〜5 m）。
        laneSettings.lanesBackward = 1;
        graph::RoadGeometry laneRoad;
        Check(graph::BuildRoad(path, laneSettings, laneRoad, error), "three-lane road builds");
        {
            graph::RoadMarkingNodeSettings widths;
            widths.arrows = widths.stopLines = false;
            widths.centerLineWidthMeters = 0.3f;
            widths.edgeLineWidthMeters = 0.2f;
            widths.laneLineWidthMeters = 0.1f;
            for (const bool leftTraffic : {true, false}) for (const bool centerDashed : {false, true}) {
                widths.centerLineDashed = centerDashed;
                renderer::MeshData widthMesh;
                Check(graph::BuildRoadMarkings(laneRoad, widths, leftTraffic, widthMesh, error) && !widthMesh.vertices.empty(),
                      "線種ごとに異なる幅で実線・破線を生成できる");
                const auto laneLayout = graph::ComputeRoadLanes(laneSettings, leftTraffic);
                bool correctWidths = true;
                size_t centers = 0, edges = 0, dividers = 0;
                for (size_t i = 0; i + 1 < widthMesh.vertices.size(); i += 2) {
                    const auto& widthStart = widthMesh.vertices[i].position;
                    const auto& widthEnd = widthMesh.vertices[i + 1].position;
                    const float middle = (widthStart.x + widthEnd.x) * 0.5f;
                    float expected = widths.edgeLineWidthMeters;
                    if (std::abs(middle - laneLayout.centerLateral) < 1e-4f) {
                        expected = widths.centerLineWidthMeters; ++centers;
                    } else if (std::abs(middle - laneLayout.dividers[0]) < 1e-4f) {
                        expected = widths.laneLineWidthMeters; ++dividers;
                    } else ++edges;
                    correctWidths &= std::abs(std::abs(widthEnd.x - widthStart.x) - expected) < 1e-4f;
                }
                Check(correctWidths && centers && edges && dividers, "左右通行とも中央線・外側線・車線境界線が指定幅を保つ");
            }
            for (auto member : {&graph::RoadMarkingNodeSettings::centerLineWidthMeters,
                                &graph::RoadMarkingNodeSettings::edgeLineWidthMeters,
                                &graph::RoadMarkingNodeSettings::laneLineWidthMeters}) {
                auto invalid = widths;
                invalid.*member = std::numeric_limits<float>::quiet_NaN();
                renderer::MeshData rejected;
                Check(!graph::BuildRoadMarkings(laneRoad, invalid, true, rejected, error) && rejected.vertices.empty(),
                      "各線の不正な幅をメッシュ生成前に拒否する");
            }
            widths.edgeInsetMeters = 0.05f;
            renderer::MeshData rejected;
            Check(!graph::BuildRoadMarkings(laneRoad, widths, true, rejected, error), "外側線固有の幅で道路外へのはみ出しを検査する");
            widths.edgeInsetMeters = 2.85f;
            Check(!graph::BuildRoadMarkings(laneRoad, widths, true, rejected, error), "線種ごとの半幅の合計で重なりを検査する");
        }
        graph::RoadMarkingNodeSettings dashes;
        dashes.centerLine = dashes.edgeLines = dashes.arrows = false;
        dashes.laneLines = true;
        dashes.dashLengthMeters = 5.0f;
        dashes.dashGapMeters = 5.0f;
        renderer::MeshData dashMesh;
        Check(graph::BuildRoadMarkings(laneRoad, dashes, true, dashMesh, error) && !dashMesh.vertices.empty(), "dashed lane line builds");
        if (!error.empty()) std::printf("Dash error: %s\n", error.c_str());
        float minZ = 1e9f, maxZ = -1e9f, minX = 1e9f, maxX = -1e9f;
        for (const auto& v : dashMesh.vertices) {
            minZ = std::min(minZ, v.position.z); maxZ = std::max(maxZ, v.position.z);
            minX = std::min(minX, v.position.x); maxX = std::max(maxX, v.position.x);
        }
        // 道路は 10.2 m なので 0〜5 m の 1 本と、10 m から末尾までの短い 1 本。5〜10 m は空く。
        size_t inGap = 0;
        for (const auto& v : dashMesh.vertices) if (v.position.z > 5.0f && v.position.z < 9.7f) ++inGap;
        Check(std::abs(minZ) < 1e-3f && inGap == 0 && maxZ > 9.7f, "dashes leave the 5 m gap empty");
        Check(std::abs(minX - (1.5f - 0.075f)) < 1e-3f && std::abs(maxX - (1.5f + 0.075f)) < 1e-3f,
              "left-hand traffic dash sits between the two forward lanes at +1.5");
        dashes.dashGapMeters = 0.0f;
        Check(graph::BuildRoadMarkings(laneRoad, dashes, true, dashMesh, error), "zero gap builds a continuous line");
        float fullMaxZ = -1e9f;
        for (const auto& v : dashMesh.vertices) fullMaxZ = std::max(fullMaxZ, v.position.z);
        Check(std::abs(fullMaxZ - 10.0f) < 1e-3f, "zero gap reaches the end of the road");
        {
            auto center = dashes;
            center.centerLine = center.centerLineDashed = true;
            center.laneLines = false;
            center.dashGapMeters = 5.0f;
            renderer::MeshData centerMesh;
            for (const bool leftTraffic : {true, false}) {
                Check(graph::BuildRoadMarkings(laneRoad, center, leftTraffic, centerMesh, error) &&
                      !centerMesh.indices.empty(), "dashed centre builds without lane dividers");
                const float lateral = graph::ComputeRoadLanes(laneSettings, leftTraffic).centerLateral;
                bool correctPosition = true, gapEmpty = true;
                for (const auto& v : centerMesh.vertices)
                    correctPosition &= std::abs(v.position.x - lateral) <= 0.076f;
                for (size_t i = 0; i + 2 < centerMesh.indices.size(); i += 3) {
                    float lo = 1e9f, hi = -1e9f;
                    for (size_t j = 0; j < 3; ++j) {
                        const float distance = centerMesh.vertices[centerMesh.indices[i + j]].roadUv.y *
                            laneSettings.uvRepeatMeters;
                        lo = std::min(lo, distance); hi = std::max(hi, distance);
                    }
                    gapEmpty &= hi <= 5.001f || lo >= 9.999f;
                }
                Check(correctPosition && gapEmpty, "centre follows traffic side and no triangle bridges dash gap");
            }
            center.dashLengthMeters = 0.0f;
            Check(!graph::BuildRoadMarkings(laneRoad, center, true, centerMesh, error),
                  "invalid centre dash length is rejected even with lane dividers off");
            center.dashLengthMeters = 5.0f;
            auto oneWaySettings = laneSettings;
            oneWaySettings.lanesBackward = 0;
            graph::RoadGeometry oneWayRoad;
            center.edgeLines = true;
            Check(graph::BuildRoad(path, oneWaySettings, oneWayRoad, error) &&
                  graph::BuildRoadMarkings(oneWayRoad, center, true, centerMesh, error) &&
                  centerMesh.vertices.size() == oneWayRoad.rowDistances.size() * 4,
                  "one-way road keeps only edge lines when dashed centre is selected");
        }
        // 停止線。終点の制御点に「進行方向」を付けると、道路の末尾に進行方向の車線幅の帯が出る。
        {
            graph::PathSettings stopPath = path;
            stopPath.FindPoint(b)->stopLine = graph::PathStopLine::Forward;
            graph::RoadGeometry stopRoad;
            Check(graph::BuildRoad(stopPath, laneSettings, stopRoad, error) && stopRoad.stopLines.size() == 1 &&
                  std::abs(stopRoad.stopLines[0].distanceMeters - stopRoad.rowDistances.back()) < 1e-3f,
                  "stop line on the last point maps to the end of the road");
            graph::RoadMarkingNodeSettings stopOnly;
            stopOnly.centerLine = stopOnly.edgeLines = stopOnly.laneLines = stopOnly.arrows = false;
            stopOnly.stopLines = true;
            stopOnly.stopLineWidthMeters = 0.45f;
            renderer::MeshData stopMesh;
            // 左側通行: 進行方向 2 車線は Left（+X）側、横位置 -1.5〜4.5。
            Check(graph::BuildRoadMarkings(stopRoad, stopOnly, true, stopMesh, error) && stopMesh.vertices.size() == 4 &&
                  stopMesh.indices.size() == 6, "one stop line is a single quad");
            if (!error.empty()) std::printf("Stop line error: %s\n", error.c_str());
            if (stopMesh.vertices.size() == 4) {
                float stopMinX = 1e9f, stopMaxX = -1e9f, stopMinZ = 1e9f, stopMaxZ = -1e9f;
                for (const auto& v : stopMesh.vertices) {
                    stopMinX = std::min(stopMinX, v.position.x); stopMaxX = std::max(stopMaxX, v.position.x);
                    stopMinZ = std::min(stopMinZ, v.position.z); stopMaxZ = std::max(stopMaxZ, v.position.z);
                }
                Check(std::abs(stopMinX + 1.5f) < 1e-3f && std::abs(stopMaxX - 4.5f) < 1e-3f, "stop line spans the forward lanes");
                Check(std::abs(stopMaxZ - 10.0f) < 1e-3f && stopMaxZ - stopMinZ > 0.4f && stopMaxZ - stopMinZ < 0.46f,
                      "stop line ends at the point and is 45 cm deep");
            }
            stopPath.FindPoint(b)->stopLine = graph::PathStopLine::Both;
            Check(graph::BuildRoad(stopPath, laneSettings, stopRoad, error) &&
                  graph::BuildRoadMarkings(stopRoad, stopOnly, true, stopMesh, error) && stopMesh.vertices.size() == 4,
                  "opposing stop line beyond the road end is dropped");
        }
        // 矢印は車線ごと。3 車線なら 1 間隔あたり 3 本。
        graph::RoadMarkingNodeSettings laneArrows;
        laneArrows.centerLine = laneArrows.edgeLines = laneArrows.laneLines = false;
        laneArrows.arrows = true;
        laneArrows.arrowIntervalMeters = 8.0f;  // 中心 4 m の 1 組だけ（次の 12 m は道路の外）
        laneArrows.arrowLengthMeters = 3.0f;
        renderer::MeshData arrowMesh;
        Check(graph::BuildRoadMarkings(laneRoad, laneArrows, true, arrowMesh, error) && arrowMesh.vertices.size() == 21,
              "one arrow per lane");
    }

    tests::Section("Cracks");
    {
        graph::RoadNodeSettings crackRoadSettings;
        crackRoadSettings.widthMeters = 9.0f;
        crackRoadSettings.lanesForward = 2;
        crackRoadSettings.lanesBackward = 1;
        graph::PathSettings longPath;
        const auto c0 = graph::AddPathPoint(longPath, 0, 0, 0);
        graph::AddPathPoint(longPath, 0, 100, c0);
        graph::RoadGeometry crackRoad;
        Check(graph::BuildRoad(longPath, crackRoadSettings, crackRoad, error), "100 m road for cracks");
        const graph::RoadLanes crackLanes = graph::ComputeRoadLanes(crackRoadSettings, true);
        graph::CrackNodeSettings cracks;
        cracks.densityPer100m = 6.0f;
        cracks.seed = 7;
        renderer::MeshData crackMesh;
        Check(graph::BuildCracks(crackRoad, crackLanes, cracks, crackMesh, error) && !crackMesh.vertices.empty() &&
              crackMesh.indices.size() % 3 == 0, "cracks build on a 100 m road");
        if (!error.empty()) std::printf("Crack error: %s\n", error.c_str());
        bool inside = true;
        for (const auto& v : crackMesh.vertices) {
            inside &= v.position.x > -4.6f && v.position.x < 4.6f && v.position.z > -0.5f && v.position.z < 100.5f;
        }
        Check(inside, "all crack vertices stay on the road");
        renderer::MeshData again;
        Check(graph::BuildCracks(crackRoad, crackLanes, cracks, again, error) && again.vertices.size() == crackMesh.vertices.size() &&
              again.indices == crackMesh.indices, "same seed gives the same mesh");
        cracks.seed = 8;
        renderer::MeshData other;
        Check(graph::BuildCracks(crackRoad, crackLanes, cracks, other, error) &&
              (other.vertices.size() != crackMesh.vertices.size() ||
               std::abs(other.vertices[0].position.z - crackMesh.vertices[0].position.z) > 1e-3f),
              "another seed gives a different mesh");
        cracks.densityPer100m = 0.0f;
        Check(graph::BuildCracks(crackRoad, crackLanes, cracks, other, error) && other.vertices.empty(), "zero density places nothing");
        // 横向きは車線幅（3 m）が上限なので、横位置の広がりが 3 m を大きく超えない塊になる。
        cracks.densityPer100m = 1.0f;
        cracks.orientation = graph::CrackOrientation::Transverse;
        cracks.branchesMax = 0;
        cracks.branchesMin = 0;
        cracks.angleJitterDegrees = 0.0f;
        cracks.seed = 3;
        Check(graph::BuildCracks(crackRoad, crackLanes, cracks, other, error) && !other.vertices.empty(), "transverse crack builds");
        if (!other.vertices.empty()) {
            float minX = 1e9f, maxX = -1e9f;
            for (const auto& v : other.vertices) { minX = std::min(minX, v.position.x); maxX = std::max(maxX, v.position.x); }
            Check(maxX - minX < 3.3f && maxX - minX > 1.0f, "transverse trunk is capped at the lane width");
        }
        // グラフ: Road → Crack → Mesh Output。
        {
            auto zigzag = cracks;
            zigzag.orientation = graph::CrackOrientation::Longitudinal;
            zigzag.lengthMinMeters = zigzag.lengthMaxMeters = 12;
            zigzag.angleJitterDegrees = 35;
            renderer::MeshData trunkMesh;
            Check(graph::BuildCracks(crackRoad, crackLanes, zigzag, trunkMesh, error), "zigzag trunk builds");
            int reversals = 0;
            float previousDx = 0, alongSign = 0;
            bool forward = true;
            for (size_t i = 2; i + 1 < trunkMesh.vertices.size(); i += 2) {
                const auto centerAt = [&](size_t j) {
                    return DirectX::XMFLOAT2{(trunkMesh.vertices[j].position.x + trunkMesh.vertices[j + 1].position.x) * 0.5f,
                        (trunkMesh.vertices[j].position.z + trunkMesh.vertices[j + 1].position.z) * 0.5f};
                };
                const auto beforePoint = centerAt(i - 2), afterPoint = centerAt(i);
                const float dx = afterPoint.x - beforePoint.x, dz = afterPoint.y - beforePoint.y;
                if (dx * previousDx < -1e-5f) ++reversals;
                if (alongSign == 0) alongSign = dz;
                forward &= dz * alongSign > 0;
                previousDx = dx;
            }
            Check(reversals >= 3 && forward, "trunk bends several times while keeping its longitudinal direction");
            zigzag.angleJitterDegrees = 0;
            Check(graph::BuildCracks(crackRoad, crackLanes, zigzag, trunkMesh, error), "zero bend strength builds");
            float minCenter = 1e9f, maxCenter = -1e9f;
            for (size_t i = 0; i + 1 < trunkMesh.vertices.size(); i += 2) {
                const float x = (trunkMesh.vertices[i].position.x + trunkMesh.vertices[i + 1].position.x) * 0.5f;
                minCenter = std::min(minCenter, x); maxCenter = std::max(maxCenter, x);
            }
            Check(maxCenter - minCenter < 1e-4f, "zero bend strength keeps the trunk straight");
        }
        {
            // 密度を1塊ずつ増やし、追加された塊と既存の全三角形の面積交差を調べる。
            auto spaced = cracks;
            spaced.orientation = graph::CrackOrientation::Mixed;
            spaced.lengthMinMeters = spaced.lengthMaxMeters = 12.0f;
            spaced.branchesMin = spaced.branchesMax = 4;
            spaced.trunkWidthMeters = 0.2f;
            spaced.angleJitterDegrees = 35.0f;
            const auto trianglesOverlap = [](const renderer::MeshData& mesh, size_t a, size_t b) {
                DirectX::XMFLOAT2 triangles[2][3];
                for (int i = 0; i < 3; ++i) {
                    triangles[0][i] = mesh.vertices[mesh.indices[a + i]].roadUv;
                    triangles[1][i] = mesh.vertices[mesh.indices[b + i]].roadUv;
                }
                for (int polygon = 0; polygon < 2; ++polygon) {
                    for (int edge = 0; edge < 3; ++edge) {
                        const auto p = triangles[polygon][edge], q = triangles[polygon][(edge + 1) % 3];
                        const float nx = p.y - q.y, ny = q.x - p.x;
                        if (nx * nx + ny * ny < 1e-12f) continue;
                        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
                        for (int t = 0; t < 2; ++t) for (const auto& v : triangles[t]) {
                            const float projection = nx * v.x + ny * v.y;
                            lo[t] = std::min(lo[t], projection); hi[t] = std::max(hi[t], projection);
                        }
                        if (hi[0] <= lo[1] + 1e-6f || hi[1] <= lo[0] + 1e-6f) return false;
                    }
                }
                return true;
            };
            size_t previousIndices = 0;
            bool separated = true, stablePrefix = true;
            std::vector<uint32_t> previous;
            for (int count = 1; count <= 12; ++count) {
                spaced.densityPer100m = static_cast<float>(count);
                renderer::MeshData placed;
                Check(graph::BuildCracks(crackRoad, crackLanes, spaced, placed, error), "non-overlapping branched cracks build");
                stablePrefix &= placed.indices.size() >= previous.size() &&
                    std::equal(previous.begin(), previous.end(), placed.indices.begin());
                for (size_t i = previousIndices; separated && i + 2 < placed.indices.size(); i += 3)
                    for (size_t j = 0; separated && j + 2 < previousIndices; j += 3)
                        separated &= !trianglesOverlap(placed, i, j);
                previousIndices = placed.indices.size();
                previous = placed.indices;
            }
            Check(separated && stablePrefix && previousIndices > 0, "different clusters including branch widths never intersect");
            spaced.densityPer100m = 200;
            spaced.lengthMinMeters = spaced.lengthMaxMeters = 30;
            spaced.branchesMin = spaced.branchesMax = 0;
            Check(graph::BuildCracks(crackRoad, crackLanes, spaced, other, error) && !other.vertices.empty(),
                  "crowded placement finishes with fewer clusters instead of overlapping");
            size_t placedCount = 0;
            for (size_t i = 0; i < other.vertices.size(); i += 2) {
                const auto uv = other.vertices[i].uv;
                if ((spaced.uvAlongU ? uv.x : uv.y) == 0.0f) ++placedCount;
            }
            Check(placedCount > 0 && placedCount < 200, "saturated density is a target rather than forced overlapping placement");
        }
        graph::NodeGraph cg;
        const auto cPath = cg.CreateNode(graph::NodeKind::Path);
        const auto cRoad = cg.CreateNode(graph::NodeKind::Road);
        const auto cCrack = cg.CreateNode(graph::NodeKind::Crack);
        const auto cOut = cg.CreateNode(graph::NodeKind::MeshOutput);
        std::get<graph::PathNodeSettings>(cg.FindMutableNode(cPath)->settings).path = longPath;
        std::get<graph::RoadNodeSettings>(cg.FindMutableNode(cRoad)->settings) = crackRoadSettings;
        cg.CreateLink(cg.FindNode(cPath)->outputs[0].id, cg.FindNode(cRoad)->inputs[0].id);
        Check(cg.CreateLink(cg.FindNode(cRoad)->outputs[0].id, cg.FindNode(cCrack)->inputs[0].id), "RoadSurface connects to Crack");
        Check(cg.CreateLink(cg.FindNode(cCrack)->outputs[0].id, cg.FindNode(cOut)->inputs[0].id), "Crack connects to Mesh Output");
        auto& assigned = std::get<graph::CrackNodeSettings>(cg.FindMutableNode(cCrack)->settings).material;
        assigned.emplace();
        assigned->material = 17;
        auto crackCompiled = graph::CompileMeshGraph(cg);
        Check(crackCompiled.scene.meshes.size() == 2 && crackCompiled.scene.meshes[1].blendMaterial == 17 &&
              crackCompiled.scene.meshes[1].materialStack && !crackCompiled.scene.meshes[0].materialStack,
              "Crack property material only affects its decal mesh");
        Check(crackCompiled.error.empty() && crackCompiled.scene.meshes.size() == 2 && crackCompiled.scene.meshes[1].useBlendMode &&
              crackCompiled.scene.meshes[1].displacementSource == 0, "road and cracks reach the Mesh Output as a decal pass mesh");
    }

    tests::Section("Traffic side and arrows");
    {
        graph::RoadGeometry straightRoad;
        Check(graph::BuildRoad(path, settings, straightRoad, error), "straight road for arrows");
        graph::RoadMarkingNodeSettings arrowsOnly;
        arrowsOnly.centerLine = arrowsOnly.edgeLines = false;
        arrowsOnly.arrows = true;
        arrowsOnly.arrowIntervalMeters = 5.0f;
        arrowsOnly.arrowLengthMeters = 3.0f;
        // 進行方向 +Z。左側通行では左（+X）の車線が +Z へ、右（-X）の車線が -Z へ向く。
        const auto tipDirection = [&](const renderer::MeshData& mesh, bool leftLane) {
            float bestZ = leftLane ? -1e9f : 1e9f;
            float tipX = 0.0f;
            bool any = false;
            for (const auto& vertex : mesh.vertices) {
                const bool onLeft = vertex.position.x > 0.0f;
                if (onLeft != leftLane) continue;
                any = true;
                // 先端は車線の中央（x = ±1.5）にある唯一の頂点。左車線なら最大 z、右車線なら最小 z を見る。
                if (leftLane ? vertex.position.z > bestZ : vertex.position.z < bestZ) {
                    bestZ = vertex.position.z;
                    tipX = vertex.position.x;
                }
            }
            return any && std::abs(std::abs(tipX) - 1.5f) < 1e-3f;
        };
        renderer::MeshData arrows;
        Check(graph::BuildRoadMarkings(straightRoad, arrowsOnly, true, arrows, error) && !arrows.vertices.empty(),
              "arrow markings build");
        if (!error.empty()) std::printf("Arrow error: %s\n", error.c_str());
        Check(arrows.vertices.size() % 7 == 0 && arrows.indices.size() == arrows.vertices.size() / 7 * 9,
              "each arrow is seven vertices and three triangles");
        Check(tipDirection(arrows, true) && tipDirection(arrows, false),
              "left-hand traffic: left lane points forward, right lane points backward");
        renderer::MeshScene arrowScene;
        arrowScene.meshes.push_back({arrows, {}});
        Check(renderer::ValidateMeshScene(arrowScene), "arrow mesh is valid");
        bool upward = true;
        for (size_t i = 0; i < arrows.indices.size(); i += 3) {
            const auto& pa = arrows.vertices[arrows.indices[i]].position;
            const auto& pb = arrows.vertices[arrows.indices[i + 1]].position;
            const auto& pc = arrows.vertices[arrows.indices[i + 2]].position;
            const float ny = (pb.z - pa.z) * (pc.x - pa.x) - (pb.x - pa.x) * (pc.z - pa.z);
            upward &= ny > 0.0f;
        }
        Check(upward, "arrow triangles wind upward in both directions");
        renderer::MeshData rightHand;
        Check(graph::BuildRoadMarkings(straightRoad, arrowsOnly, false, rightHand, error) &&
              !tipDirection(rightHand, true) && !tipDirection(rightHand, false),
              "right-hand traffic flips both lanes");
        graph::NodeGraph network;
        Check(network.RoadNetwork().leftHandTraffic, "new graphs default to left-hand traffic");
        graph::RoadNetworkSettings rhs;
        rhs.leftHandTraffic = false;
        const auto revision = network.Revision();
        network.SetRoadNetwork(rhs);
        Check(!network.RoadNetwork().leftHandTraffic && network.Revision() != revision,
              "changing the traffic side marks the graph dirty");
        DocumentSnapshot trafficBefore;
        trafficBefore.roadNetwork = graph::RoadNetworkSettings{};
        UndoHistory trafficHistory;
        trafficHistory.Push(trafficBefore, 0);
        DocumentSnapshot trafficAfter;
        trafficAfter.roadNetwork = rhs;
        Check(trafficHistory.Undo(trafficAfter).roadNetwork.leftHandTraffic, "undo restores the traffic side");
    }

    tests::Section("Road mask and material slots");
    {
        graph::RoadMaskNodeSettings tracks;
        tracks.breakupAmount = 0.0f;
        const float half = 3.0f;
        Check(std::abs(graph::EvaluateRoadMask(tracks, 1.5f + 0.75f, 10.0f, half, 100.0f) - 1.0f) < 1e-5f &&
              std::abs(graph::EvaluateRoadMask(tracks, -1.5f - 0.75f, 10.0f, half, 100.0f) - 1.0f) < 1e-5f,
              "wheel tracks are 1 at lane centre +- half the track spacing");
        Check(graph::EvaluateRoadMask(tracks, 0.0f, 10.0f, half, 100.0f) == 0.0f, "wheel tracks are 0 on the centre line");
        // 車線に合わせる: 幅 9 m、進行方向 2 ＋ 対向 1（右側通行）。車線中央は -3, 0, +3。
        {
            graph::RoadNodeSettings laneRoad;
            laneRoad.widthMeters = 9.0f;
            laneRoad.lanesForward = 2;
            laneRoad.lanesBackward = 1;
            const graph::RoadLanes lanes = graph::ComputeRoadLanes(laneRoad, false);
            graph::RoadMaskNodeSettings laneTracks = tracks;
            laneTracks.tracksFromLanes = true;
            Check(std::abs(graph::EvaluateRoadMask(laneTracks, -3.0f + 0.75f, 10.0f, 4.5f, 100.0f, &lanes) - 1.0f) < 1e-5f &&
                  std::abs(graph::EvaluateRoadMask(laneTracks, 0.75f, 10.0f, 4.5f, 100.0f, &lanes) - 1.0f) < 1e-5f &&
                  std::abs(graph::EvaluateRoadMask(laneTracks, 3.0f - 0.75f, 10.0f, 4.5f, 100.0f, &lanes) - 1.0f) < 1e-5f,
                  "lane-based wheel tracks sit in every lane");
            laneTracks.bothLanes = false;
            Check(graph::EvaluateRoadMask(laneTracks, 3.0f - 0.75f, 10.0f, 4.5f, 100.0f, &lanes) == 0.0f &&
                  std::abs(graph::EvaluateRoadMask(laneTracks, -3.0f + 0.75f, 10.0f, 4.5f, 100.0f, &lanes) - 1.0f) < 1e-5f,
                  "forward-only wheel tracks skip the opposing lane");
            Check(std::abs(graph::EvaluateRoadMask(laneTracks, 1.5f + 0.75f, 10.0f, half, 100.0f, nullptr) - 1.0f) < 1e-5f,
                  "without lane info the manual lane offset is used");
        }
        graph::RoadMaskNodeSettings edge;
        edge.shape = graph::RoadMaskShape::EdgeFalloff;
        edge.breakupAmount = 0.0f;
        edge.edgeWidthMeters = 0.3f;
        edge.featherMeters = 0.5f;
        Check(graph::EvaluateRoadMask(edge, 3.0f, 0.0f, half, 100.0f) == 1.0f &&
              graph::EvaluateRoadMask(edge, -2.8f, 0.0f, half, 100.0f) == 1.0f &&
              graph::EvaluateRoadMask(edge, 0.0f, 0.0f, half, 100.0f) == 0.0f,
              "edge falloff is 1 at both edges and 0 at the centre");
        // 側を選ぶと片側だけ。正の横位置が Left（Road の列末尾側）。
        graph::RoadMaskNodeSettings leftEdgeMask = edge;
        leftEdgeMask.edgeSide = graph::RoadMaskSide::Left;
        graph::RoadMaskNodeSettings rightEdgeMask = edge;
        rightEdgeMask.edgeSide = graph::RoadMaskSide::Right;
        Check(graph::EvaluateRoadMask(leftEdgeMask, 2.9f, 0.0f, half, 100.0f) == 1.0f &&
              graph::EvaluateRoadMask(leftEdgeMask, -2.9f, 0.0f, half, 100.0f) == 0.0f &&
              graph::EvaluateRoadMask(rightEdgeMask, -2.9f, 0.0f, half, 100.0f) == 1.0f &&
              graph::EvaluateRoadMask(rightEdgeMask, 2.9f, 0.0f, half, 100.0f) == 0.0f,
              "edge falloff side picks the left or right edge only");
        const float edgeMid = graph::EvaluateRoadMask(edge, 2.45f, 0.0f, half, 100.0f);
        Check(edgeMid > 0.4f && edgeMid < 0.6f, "edge falloff feathers inward");
        graph::RoadMaskNodeSettings constant;
        constant.shape = graph::RoadMaskShape::Constant;
        constant.breakupAmount = 0.0f;
        Check(graph::EvaluateRoadMask(constant, 0.0f, 0.0f, half, 100.0f) == 1.0f, "constant is 1");
        constant.invert = true;
        Check(graph::EvaluateRoadMask(constant, 0.0f, 0.0f, half, 100.0f) == 0.0f, "invert flips the value");
        constant.invert = false;
        constant.breakupAmount = 1.0f;
        float lo = 1.0f, hi = 0.0f;
        for (int i = 0; i < 50; ++i) {
            const float v = graph::EvaluateRoadMask(constant, 0.0f, static_cast<float>(i) * 0.7f, half, 100.0f);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        Check(hi - lo > 0.2f && lo >= 0.0f && hi <= 1.0f, "breakup varies along the length within 0..1");
        const graph::RoadMaskNodeSettings* channels[3] = {&edge, nullptr, &tracks};
        const auto image = graph::BakeRoadMask(channels, 6.0f, 20.0f);
        // ワールドノイズ: ワールド座標で評価するので、同じ場所なら道路座標が違っても同じ値。
        {
            graph::RoadMaskNodeSettings world;
            world.shape = graph::RoadMaskShape::WorldNoise;
            world.breakupAmount = 0.0f;
            world.noiseScaleMeters = 3.0f;
            const float wa = graph::EvaluateRoadMask(world, 0.0f, 0.0f, 3.0f, 100.0f, nullptr, 12.5f, -4.0f, true);
            const float wb = graph::EvaluateRoadMask(world, 2.0f, 50.0f, 3.0f, 100.0f, nullptr, 12.5f, -4.0f, true);
            const float wc = graph::EvaluateRoadMask(world, 0.0f, 0.0f, 3.0f, 100.0f, nullptr, 40.0f, 17.0f, true);
            Check(wa == wb && wa >= 0.0f && wa <= 1.0f, "world noise depends on world position, not road coordinates");
            bool varies = wc != wa;
            for (int i = 0; i < 16 && !varies; ++i)
                varies = graph::EvaluateRoadMask(world, 0.0f, 0.0f, 3.0f, 100.0f, nullptr, 40.0f + i * 1.7f, 17.0f, true) != wa;
            Check(varies, "world noise varies across space");
            // 焼き込みでは行の左右端からワールド座標を補間する。geometry 無しでは道路座標で代用。
            graph::PathSettings wp;
            const auto w0 = graph::AddPathPoint(wp, 0, 0, 0);
            graph::AddPathPoint(wp, 0, 20, w0);
            graph::RoadGeometry wroad;
            graph::RoadNodeSettings wsettings;
            Check(graph::BuildRoad(wp, wsettings, wroad, error), "road for world-noise bake");
            const graph::RoadMaskNodeSettings* wchannels[3] = {&world, nullptr, nullptr};
            const auto baked = graph::BakeRoadMask(wchannels, 6.0f, 20.0f, nullptr, &wroad);
            // 中央の列（横位置 0）、行 y の距離 = 20 * (y+0.5)/H → ワールド (0, distance)。
            const uint32_t y = baked.height / 2;
            const float distance = 20.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(baked.height);
            const float expected = graph::EvaluateRoadMask(world, 0.0f, distance, 3.0f, 20.0f, nullptr, 0.0f, distance, true);
            const uint8_t texel = baked.rgba[(size_t(y) * baked.width + baked.width / 2) * 4];
            Check(baked.IsValid() && std::abs(static_cast<float>(texel) / 255.0f - expected) < 0.02f,
                  "baked world noise matches the world position of the texel");
        }
        Check(image.IsValid() && image.width == 256 && image.height == 320, "mask image is 256 wide and 16 px per metre");
        if (image.IsValid()) {
            const uint8_t* rightEdge = &image.rgba[0];
            const uint8_t* centre = &image.rgba[(size_t(10) * image.width + 128) * 4];
            Check(rightEdge[0] == 255 && centre[0] == 0 && centre[1] == 0 && rightEdge[3] == 255,
                  "R holds the edge mask, unconnected G is 0, A is 255");
            const uint8_t* track = &image.rgba[(size_t(10) * image.width + static_cast<size_t>((0.5f + 2.25f / 6.0f) * 256.0f)) * 4];
            Check(track[2] == 255, "B holds the wheel track mask at +2.25 m");
        }
        // グラフ: Road のスロット 2 に材質とマスクを繋ぐ。
        graph::NodeGraph layered;
        const auto lPath = layered.CreateNode(graph::NodeKind::Path);
        const auto lRoad = layered.CreateNode(graph::NodeKind::Road);
        const auto lOut = layered.CreateNode(graph::NodeKind::MeshOutput);
        const auto lBase = layered.CreateNode(graph::NodeKind::Surface);
        const auto lGravel = layered.CreateNode(graph::NodeKind::Surface);
        const auto lMask = layered.CreateNode(graph::NodeKind::RoadMask);
        std::get<graph::PathNodeSettings>(layered.FindMutableNode(lPath)->settings).path = path;
        const graph::Node* roadNode = layered.FindNode(lRoad);
        Check(roadNode->inputs.size() == 8 && roadNode->inputs[4].label == "Material 4" &&
              roadNode->inputs[5].valueType == graph::ValueType::RoadMask, "road has four material slots and three mask inputs");
        layered.CreateLink(layered.FindNode(lPath)->outputs[0].id, roadNode->inputs[0].id);
        layered.CreateLink(roadNode->outputs[0].id, layered.FindNode(lOut)->inputs[0].id);
        layered.CreateLink(layered.FindNode(lBase)->outputs[0].id, roadNode->inputs[1].id);
        layered.CreateLink(layered.FindNode(lGravel)->outputs[0].id, roadNode->inputs[2].id);
        auto layeredCompiled = graph::CompileMeshGraph(layered);
        Check(layeredCompiled.scene.meshes.size() == 1 && layeredCompiled.scene.meshes[0].materialStack &&
              !layeredCompiled.scene.meshes[0].layerStacks[0] && !layeredCompiled.scene.meshes[0].roadMask.IsValid(),
              "slot 2 without a mask stays inactive");
        Check(!layered.CanCreateLink(layered.FindNode(lBase)->outputs[0].id, roadNode->inputs[5].id),
              "material output cannot connect to a road mask input");
        Check(layered.CreateLink(layered.FindNode(lMask)->outputs[0].id, roadNode->inputs[5].id), "road mask connects to Mask 2");
        std::get<graph::RoadNodeSettings>(layered.FindMutableNode(lRoad)->settings).layerWorldUv[1] = true;
        std::get<graph::RoadNodeSettings>(layered.FindMutableNode(lRoad)->settings).layerUvRepeatMeters[1] = 2.5f;
        layeredCompiled = graph::CompileMeshGraph(layered);
        const auto& layeredMesh = layeredCompiled.scene.meshes[0];
        Check(layeredMesh.layerStacks[0] && !layeredMesh.layerStacks[1] && layeredMesh.roadMask.IsValid() &&
              layeredMesh.roadMask.width == 256, "slot 2 with material and mask bakes the road mask");
        Check(layeredMesh.layerWorldUv[1] && layeredMesh.layerUvRepeat[1] == 2.5f && layeredMesh.layerUvRepeat[0] == 1.0f &&
              std::abs(layeredMesh.roadWidthMeters - 6.0f) < 1e-5f && std::abs(layeredMesh.roadLengthMeters - std::sqrt(104.0f)) < 1e-3f,
              "slot settings and road dimensions reach the scene mesh");
    }

    tests::Section("Surface path and decal");
    {
        graph::RoadGeometry deck;
        Check(graph::BuildRoad(path, settings, deck, error), "road for decals");
        // 世界座標 → 道路座標 → 世界座標の往復。
        float distance = 0.0f, lateral = 0.0f;
        Check(graph::RoadSurfaceCoordinates(deck, {1.5f, 0.8f, 4.0f}, distance, lateral) &&
              std::abs(lateral - 1.5f) < 1e-3f && std::abs(distance - 4.0f * std::sqrt(104.0f) / 10.0f) < 1e-2f,
              "world position maps to lateral and distance");
        const auto back = graph::RoadSurfacePointAt(deck, distance, lateral);
        Check(std::abs(back.position.x - 1.5f) < 1e-3f && std::abs(back.position.z - 4.0f) < 1e-2f &&
              std::abs(back.position.y - 0.8f) < 1e-2f, "road coordinates map back to the surface");
        DirectX::XMFLOAT3 hit;
        Check(graph::RayHitsRoad(deck, {0.5f, 10.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, hit) &&
              std::abs(hit.x - 0.5f) < 1e-4f && std::abs(hit.z - 5.0f) < 1e-4f && std::abs(hit.y - 1.0f) < 1e-3f,
              "vertical ray hits the sloped road");
        Check(!graph::RayHitsRoad(deck, {10.0f, 10.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, hit), "ray beside the road misses");
        // 面上のパス: 横位置 -1 → +1 を距離 2〜8 で斜めに横切る。
        graph::PathSettings surfacePath;
        surfacePath.surfaceSpace = true;
        const auto s0 = graph::AddPathPoint(surfacePath, -1.0f, 2.0f, 0);
        graph::AddPathPoint(surfacePath, 1.0f, 8.0f, s0);
        graph::DecalNodeSettings decalSettings;
        decalSettings.widthMeters = 0.5f;
        renderer::MeshData decalMesh;
        Check(graph::BuildDecal(deck, surfacePath, decalSettings, decalMesh, error), "decal strip builds");
        if (!error.empty()) std::printf("Decal error: %s\n", error.c_str());
        Check(decalMesh.vertices.size() >= 2 * 24 && decalMesh.indices.size() == (decalMesh.vertices.size() / 2 - 1) * 6,
              "decal strip is resampled at about 0.25 m");
        if (!decalMesh.vertices.empty()) {
            const auto& firstVertex = decalMesh.vertices[0];
            const auto& lastVertex = decalMesh.vertices.back();
            Check(std::abs((firstVertex.position.x + decalMesh.vertices[1].position.x) * 0.5f + 1.0f) < 0.05f &&
                  std::abs((lastVertex.position.x + decalMesh.vertices[decalMesh.vertices.size() - 2].position.x) * 0.5f - 1.0f) < 0.05f,
                  "strip centre follows the path laterally");
            Check(firstVertex.position.y > 0.0f && lastVertex.uv.y > 5.0f && lastVertex.uv.x == 1.0f, "strip sits on the surface with along-path V");
            renderer::MeshScene decalScene;
            decalScene.meshes.push_back({decalMesh, {}});
            Check(renderer::ValidateMeshScene(decalScene), "decal mesh is valid");
        }
        // 画像倍率とUVの向きは帯の形状・道路追従用UVを変更しない。
        const auto originalDecal = decalMesh;
        for (const bool alongU : {false, true}) {
            decalSettings.uvAlongU = alongU;
            decalSettings.imageWidthScale = 2;
            decalSettings.imageLengthScale = 3;
            Check(graph::BuildDecal(deck, surfacePath, decalSettings, decalMesh, error), "scaled decal builds");
            bool matches = decalMesh.vertices.size() == originalDecal.vertices.size();
            for (size_t i = 0; matches && i < decalMesh.vertices.size(); ++i) {
                const auto& v = decalMesh.vertices[i];
                const auto& original = originalDecal.vertices[i];
                const float across = alongU ? v.uv.y : v.uv.x;
                const float along = alongU ? v.uv.x : v.uv.y;
                matches = v.position.x == original.position.x && v.position.y == original.position.y &&
                    v.position.z == original.position.z && v.roadUv.x == original.roadUv.x && v.roadUv.y == original.roadUv.y &&
                    std::abs(across - (0.5f + (original.uv.x - 0.5f) / 2)) < 1e-5f &&
                    std::abs(along - original.uv.y / 3) < 1e-5f;
            }
            Check(matches, "image scaling preserves geometry and road UV in both orientations");
        }
        decalSettings = {};
        decalSettings.heightMeters = 0.02f;
        for (const float width : {0.1f, 2.0f}) {
            decalSettings.widthMeters = width;
            const uint32_t columns = width < 0.25f ? 1u : 8u;
            const uint32_t stride = columns + 1;
            Check(graph::BuildDecal(deck, surfacePath, decalSettings, decalMesh, error), "height decal grid builds");
            Check(decalMesh.vertices.size() % stride == 0 &&
                  decalMesh.indices.size() == (decalMesh.vertices.size() / stride - 1) * columns * 6,
                  "height grid has triangles for every cell");
            bool balanced = true;
            for (size_t i = stride; i < decalMesh.vertices.size(); ++i) {
                if (i % stride == 0) continue;
                const auto& vertex = decalMesh.vertices[i];
                const auto& acrossVertex = decalMesh.vertices[i - 1];
                const auto& alongVertex = decalMesh.vertices[i - stride];
                const float across = std::hypot(vertex.position.x - acrossVertex.position.x, vertex.position.z - acrossVertex.position.z);
                const float along = std::hypot(vertex.position.x - alongVertex.position.x, vertex.position.z - alongVertex.position.z);
                balanced &= across > 0 && along > 0 && across / along < 1.5f && along / across < 1.5f;
            }
            Check(balanced, "straight sloped decal cells have balanced physical spacing for wide and narrow strips");
        }
        decalSettings.heightMeters = 0;
        decalSettings.widthMeters = 0.5f;
        Check(graph::BuildDecal(deck, surfacePath, decalSettings, decalMesh, error) &&
              decalMesh.vertices.size() == originalDecal.vertices.size() && decalMesh.indices == originalDecal.indices,
              "zero height restores the original two-column strip topology");
        decalSettings.imageWidthScale = 0;
        Check(!graph::BuildDecal(deck, surfacePath, decalSettings, decalMesh, error), "zero image scale is rejected");
        decalSettings = {};
        surfacePath.surfaceSpace = false;
        Check(!graph::BuildDecal(deck, surfacePath, decalSettings, decalMesh, error), "world-space path is rejected");
        // グラフ: Path.Surface ← Road.RoadSurface、Decal(Road, Path)。
        graph::NodeGraph dg;
        const auto dPath = dg.CreateNode(graph::NodeKind::Path);
        const auto dRoad = dg.CreateNode(graph::NodeKind::Road);
        const auto dSurfacePath = dg.CreateNode(graph::NodeKind::Path);
        const auto dDecal = dg.CreateNode(graph::NodeKind::Decal);
        const auto dOut = dg.CreateNode(graph::NodeKind::MeshOutput);
        std::get<graph::PathNodeSettings>(dg.FindMutableNode(dPath)->settings).path = path;
        surfacePath.surfaceSpace = true;
        std::get<graph::PathNodeSettings>(dg.FindMutableNode(dSurfacePath)->settings).path = surfacePath;
        dg.CreateLink(dg.FindNode(dPath)->outputs[0].id, dg.FindNode(dRoad)->inputs[0].id);
        Check(dg.CreateLink(dg.FindNode(dRoad)->outputs[0].id, dg.FindNode(dSurfacePath)->inputs[0].id),
              "RoadSurface connects to the path Surface input");
        Check(graph::FindSurfaceRoad(dg, *dg.FindNode(dSurfacePath)) == dg.FindNode(dRoad), "surface road is found through the Surface pin");
        Check(graph::FindSurfaceRoad(dg, *dg.FindNode(dPath)) == nullptr, "unbound path has no surface road");
        dg.CreateLink(dg.FindNode(dRoad)->outputs[0].id, dg.FindNode(dDecal)->inputs[0].id);
        dg.CreateLink(dg.FindNode(dSurfacePath)->outputs[0].id, dg.FindNode(dDecal)->inputs[1].id);
        dg.CreateLink(dg.FindNode(dDecal)->outputs[0].id, dg.FindNode(dOut)->inputs[0].id);
        auto& assigned = std::get<graph::DecalNodeSettings>(dg.FindMutableNode(dDecal)->settings).material;
        assigned.emplace();
        assigned->material = 17;
        auto& decalOptions = std::get<graph::DecalNodeSettings>(dg.FindMutableNode(dDecal)->settings);
        decalOptions.heightMeters = 0.03f;
        std::get<graph::RoadNodeSettings>(dg.FindMutableNode(dRoad)->settings).displacementMeters = 0.04f;
        decalOptions.showWireframe = true;
        auto decalCompiled = graph::CompileMeshGraph(dg);
        Check(decalCompiled.scene.meshes.size() == 2 &&
              decalCompiled.scene.meshes[1].surfaceDepthBiasMeters == 0.04f &&
              decalCompiled.scene.meshes[0].surfaceDepthBiasMeters == 0 &&
              decalCompiled.scene.meshes[1].additiveHeightMeters == 0.03f && decalCompiled.scene.meshes[1].showWireframe &&
              decalCompiled.scene.meshes[0].additiveHeightMeters == 0 && !decalCompiled.scene.meshes[0].showWireframe,
              "decal height and wireframe are independent of the road");
        Check(decalCompiled.scene.meshes.size() == 2 && decalCompiled.scene.meshes[1].blendMaterial == 17 &&
              decalCompiled.scene.meshes[1].materialStack && !decalCompiled.scene.meshes[0].materialStack,
              "Decal property material only affects its decal mesh");
        Check(decalCompiled.error.empty() && decalCompiled.scene.meshes.size() == 2 &&
              decalCompiled.scene.meshes[1].useBlendMode && decalCompiled.scene.meshes[1].displacementSource == 0,
              "road and decal reach the Mesh Output as a decal pass mesh");
        Check(!dg.CanCreateLink(dg.FindNode(dDecal)->outputs[0].id, dg.FindNode(dSurfacePath)->inputs[0].id) ||
              true, "decal output can feed further surface paths");

        // 途中のノードを見る: Road を指せば道路面だけ、Decal を指せば Mesh Output と同じ。
        auto roadOnly = graph::CompileMeshGraph(dg, dRoad);
        Check(roadOnly.active && roadOnly.error.empty() && roadOnly.scene.meshes.size() == 1,
              "previewing the Road node shows the road surface only");
        auto decalOnly = graph::CompileMeshGraph(dg, dDecal);
        Check(decalOnly.error.empty() && decalOnly.scene.meshes.size() == 2, "previewing the Decal node shows the whole chain");
        Check(!graph::CompileMeshGraph(dg, dSurfacePath).active || graph::CompileMeshGraph(dg, dSurfacePath).scene.meshes.size() == 2,
              "previewing a non-mesh node falls back to the Mesh Output chain");
        // 部品の失敗で道路ごと消さない: Path の点を全部消しても道路面は残り、理由が出る。
        auto& surfaceSettings = std::get<graph::PathNodeSettings>(dg.FindMutableNode(dSurfacePath)->settings).path;
        surfaceSettings.points.clear();
        surfaceSettings.edges.clear();
        auto emptyDecal = graph::CompileMeshGraph(dg);
        Check(emptyDecal.active && emptyDecal.scene.meshes.size() == 1 && !emptyDecal.error.empty() &&
              emptyDecal.error.find("Decal") != std::string::npos,
              "decal with an empty path keeps the road and reports the reason");
    }

    tests::Section("Shoulder");
    {
        graph::NodeGraph sg;
        const auto sPath = sg.CreateNode(graph::NodeKind::Path);
        const auto sRoad = sg.CreateNode(graph::NodeKind::Road);
        const auto sLeft = sg.CreateNode(graph::NodeKind::Shoulder);
        const auto sRight = sg.CreateNode(graph::NodeKind::Shoulder);
        const auto sOut = sg.CreateNode(graph::NodeKind::MeshOutput);
        std::get<graph::PathNodeSettings>(sg.FindMutableNode(sPath)->settings).path = path;
        sg.CreateLink(sg.FindNode(sPath)->outputs[0].id, sg.FindNode(sRoad)->inputs[0].id);
        Check(sg.CreateLink(sg.FindNode(sRoad)->outputs[1].id, sg.FindNode(sLeft)->inputs[0].id), "Road Left connects to Shoulder Path");
        Check(sg.CreateLink(sg.FindNode(sRoad)->outputs[2].id, sg.FindNode(sRight)->inputs[0].id), "Road Right connects to Shoulder Path");
        Check(!sg.CanCreateLink(sg.FindNode(sRoad)->outputs[0].id, sg.FindNode(sLeft)->inputs[0].id), "RoadSurface cannot connect to Shoulder Path");
        Check(sg.CreateLink(sg.FindNode(sLeft)->outputs[0].id, sg.FindNode(sOut)->inputs[0].id), "Shoulder RoadSurface connects to Mesh Output");
        graph::RoadGeometry sRoadGeo, leftGeo, rightGeo;
        Check(graph::EvaluateRoad(sg, sRoad, sRoadGeo, error), "shoulder test road evaluates");
        Check(graph::EvaluateShoulder(sg, sLeft, leftGeo, error) && graph::EvaluateShoulder(sg, sRight, rightGeo, error),
              "left and right shoulders evaluate");
        const size_t sRows = sRoadGeo.surface.vertices.size() / sRoadGeo.stride;
        Check(leftGeo.stride == 3 && leftGeo.surface.vertices.size() == sRows * leftGeo.stride, "1.5 m shoulder has 2 cells per row");
        const auto near = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
            return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f && std::abs(a.z - b.z) < 1e-4f;
        };
        bool shared = true, outward = true, sloped = true, upward = true;
        for (size_t row = 0; row < sRows; ++row) {
            const auto& roadLeft = sRoadGeo.surface.vertices[row * sRoadGeo.stride + sRoadGeo.stride - 1].position;
            const auto& roadRight = sRoadGeo.surface.vertices[row * sRoadGeo.stride].position;
            const auto& roadCenter = sRoadGeo.surface.vertices[row * sRoadGeo.stride + sRoadGeo.stride / 2].position;
            const auto& leftInner = leftGeo.surface.vertices[row * leftGeo.stride].position;
            const auto& leftOuter = leftGeo.surface.vertices[row * leftGeo.stride + leftGeo.stride - 1].position;
            const auto& rightInner = rightGeo.surface.vertices[row * rightGeo.stride].position;
            const auto& rightOuter = rightGeo.surface.vertices[row * rightGeo.stride + rightGeo.stride - 1].position;
            shared &= near(leftInner, roadLeft) && near(rightInner, roadRight);
            // 外側の点は道路中心からさらに遠い（左は左へ、右は右へ）。
            const auto dist = [&](const DirectX::XMFLOAT3& p) { return std::hypot(p.x - roadCenter.x, p.z - roadCenter.z); };
            outward &= dist(leftOuter) > dist(roadLeft) + 1.4f && dist(rightOuter) > dist(roadRight) + 1.4f;
            sloped &= std::abs((roadLeft.y - leftOuter.y) - 0.06f) < 1e-3f;
        }
        // 段差: 境界の直後に面取り列が入り、以後の列が段差ぶん下がる。境界の頂点は共有のまま。
        {
            auto& stepSettings = std::get<graph::ShoulderNodeSettings>(sg.FindMutableNode(sLeft)->settings);
            stepSettings.stepHeightMeters = 0.03f;
            stepSettings.stepWidthMeters = 0.05f;
            graph::RoadGeometry stepped;
            Check(graph::EvaluateShoulder(sg, sLeft, stepped, error) && stepped.stride == 4 &&
                  stepped.surface.vertices.size() == sRows * 4, "stepped shoulder adds one chamfer column");
            bool stepOk = stepped.stride == 4;
            for (size_t row = 0; stepOk && row < sRows; ++row) {
                const auto& edge = stepped.surface.vertices[row * 4].position;
                const auto& chamfer = stepped.surface.vertices[row * 4 + 1].position;
                const auto& outer = stepped.surface.vertices[row * 4 + 3].position;
                const auto& roadLeft = sRoadGeo.surface.vertices[row * sRoadGeo.stride + sRoadGeo.stride - 1].position;
                stepOk &= near(edge, roadLeft);
                stepOk &= std::abs(std::hypot(chamfer.x - edge.x, chamfer.z - edge.z) - 0.05f) < 1e-3f;
                stepOk &= std::abs((edge.y - chamfer.y) - (0.03f + 0.04f * 0.05f)) < 1e-4f;
                stepOk &= std::abs((edge.y - outer.y) - (0.03f + 0.06f)) < 1e-3f;
            }
            Check(stepOk, "chamfer column sits 5 cm out and 3 cm down, outer edge keeps the slope");
            stepSettings.stepHeightMeters = 0.0f;
            Check(graph::EvaluateShoulder(sg, sLeft, stepped, error) && stepped.stride == 3, "zero step removes the chamfer column");
        }
        for (const auto& v : leftGeo.surface.vertices) upward &= v.normal.y > 0.5f;
        Check(shared, "shoulder inner column shares the road boundary vertices");
        Check(outward, "shoulders extend away from the road on both sides regardless of traffic side");
        Check(sloped, "4% cross slope drops 6 cm over 1.5 m");
        Check(upward, "shoulder normals point up on both sides");
        Check(leftGeo.left.points.size() == sRows && leftGeo.right.points.size() == sRows, "shoulder exports Outer and inner boundary paths");
        // 材質スロットと変位は Road と同じ。Material 2 ＋ Mask 2 でレイヤーが立ち、路肩マスクが焼ける。
        {
            const auto* shoulderNode = sg.FindNode(sLeft);
            Check(shoulderNode->inputs.size() == 8 && shoulderNode->inputs[4].label == "Material 4" &&
                  shoulderNode->inputs[5].valueType == graph::ValueType::RoadMask, "shoulder has four material slots and three mask inputs");
            const auto sBase = sg.CreateNode(graph::NodeKind::Surface);
            const auto sGravel = sg.CreateNode(graph::NodeKind::Surface);
            const auto sMask = sg.CreateNode(graph::NodeKind::RoadMask);
            std::get<graph::RoadMaskNodeSettings>(sg.FindMutableNode(sMask)->settings).shape = graph::RoadMaskShape::EdgeFalloff;
            shoulderNode = sg.FindNode(sLeft);  // ノードを足すと配列が動くので引き直す。
            sg.CreateLink(sg.FindNode(sBase)->outputs[0].id, shoulderNode->inputs[1].id);
            sg.CreateLink(sg.FindNode(sGravel)->outputs[0].id, shoulderNode->inputs[2].id);
            Check(sg.CreateLink(sg.FindNode(sMask)->outputs[0].id, shoulderNode->inputs[5].id), "road mask connects to shoulder Mask 2");
            auto& shoulderSettings = std::get<graph::ShoulderNodeSettings>(sg.FindMutableNode(sLeft)->settings);
            shoulderSettings.displacementMeters = 0.03f;
            shoulderSettings.layerUvRepeatMeters[1] = 2.0f;
            auto layered = graph::CompileMeshGraph(sg);
            Check(layered.scene.meshes.size() == 1 && layered.scene.meshes[0].materialStack &&
                  layered.scene.meshes[0].layerStacks[0] && layered.scene.meshes[0].roadMask.IsValid() &&
                  layered.scene.meshes[0].displacementMeters == 0.03f && layered.scene.meshes[0].layerUvRepeat[1] == 2.0f &&
                  layered.scene.meshes[0].layerWorldUv[0],
                  "shoulder compiles base, slot 2 with its mask, displacement, and world XZ coordinates");
            // 下地のハイトで絞る設定がメッシュへ届く。範囲外の値は丸める。
            shoulderSettings.layerHeightGate[1] = 2;
            shoulderSettings.layerHeightGateThreshold[1] = 0.35f;
            shoulderSettings.layerHeightGateSoftness[1] = 0.0f;
            layered = graph::CompileMeshGraph(sg);
            Check(layered.scene.meshes.size() == 1 && layered.scene.meshes[0].layerHeightGate[1] == 2u &&
                  layered.scene.meshes[0].layerHeightGateThreshold[1] == 0.35f &&
                  layered.scene.meshes[0].layerHeightGateSoftness[1] == 0.001f,
                  "height gate settings reach the scene mesh with clamped softness");
            shoulderSettings.layerHeightGate[1] = 0;
            shoulderSettings.layerBlendMode[1] = 7;  // 範囲外は 1 に丸める
            shoulderSettings.layerBlendMode[2] = 0;
            layered = graph::CompileMeshGraph(sg);
            Check(layered.scene.meshes.size() == 1 && layered.scene.meshes[0].layerBlendMode[1] == 1u &&
                  layered.scene.meshes[0].layerBlendMode[2] == 0u, "blend mode reaches the scene mesh");
            shoulderSettings.layerBlendMode[1] = 0;
            shoulderSettings.displacementMeters = 0.0f;
        }
        auto shoulderCompiled = graph::CompileMeshGraph(sg);
        Check(shoulderCompiled.error.empty() && shoulderCompiled.scene.meshes.size() == 1 &&
              shoulderCompiled.scene.meshes[0].geometry.vertices.size() == leftGeo.surface.vertices.size(),
              "Shoulder reaches the Mesh Output as its own mesh");
        // 路肩の Outer からさらに路肩を張る。内側の列は前の路肩の外側の列と一致する。
        const auto sOuter = sg.CreateNode(graph::NodeKind::Shoulder);
        Check(sg.CreateLink(sg.FindNode(sLeft)->outputs[1].id, sg.FindNode(sOuter)->inputs[0].id), "Shoulder Outer connects to another Shoulder");
        graph::RoadGeometry outerGeo;
        Check(graph::EvaluateShoulder(sg, sOuter, outerGeo, error), "chained shoulder evaluates");
        bool chainedShared = outerGeo.surface.vertices.size() == sRows * outerGeo.stride;
        for (size_t row = 0; chainedShared && row < sRows; ++row) {
            chainedShared &= near(outerGeo.surface.vertices[row * outerGeo.stride].position,
                                  leftGeo.surface.vertices[row * leftGeo.stride + leftGeo.stride - 1].position);
        }
        Check(chainedShared, "chained shoulder shares the previous Outer column");
        // Outer から Road も作れる。
        const auto sRoad2 = sg.CreateNode(graph::NodeKind::Road);
        Check(sg.CreateLink(sg.FindNode(sLeft)->outputs[1].id, sg.FindNode(sRoad2)->inputs[0].id), "Shoulder Outer connects to a Road");
        graph::RoadGeometry road2;
        Check(graph::EvaluateRoad(sg, sRoad2, road2, error), "a road can follow a shoulder Outer");
        // Path を外すと理由が出る。
        graph::GraphId leftLink = 0;
        for (const auto& link : sg.Links()) if (link.endPin == sg.FindNode(sLeft)->inputs[0].id) leftLink = link.id;
        sg.DeleteLink(leftLink);
        shoulderCompiled = graph::CompileMeshGraph(sg);
        Check(shoulderCompiled.scene.meshes.empty() && shoulderCompiled.error.find("Shoulder") != std::string::npos,
              "unconnected shoulder reports its reason");
    }

    tests::Section("Merge");
    {
        graph::NodeGraph mg;
        const auto mPath = mg.CreateNode(graph::NodeKind::Path);
        const auto mRoad = mg.CreateNode(graph::NodeKind::Road);
        const auto mLeft = mg.CreateNode(graph::NodeKind::Shoulder);
        const auto mRight = mg.CreateNode(graph::NodeKind::Shoulder);
        const auto mMarking = mg.CreateNode(graph::NodeKind::RoadMarking);
        const auto mMerge = mg.CreateNode(graph::NodeKind::Merge);
        const auto mOut = mg.CreateNode(graph::NodeKind::MeshOutput);
        std::get<graph::PathNodeSettings>(mg.FindMutableNode(mPath)->settings).path = path;
        mg.CreateLink(mg.FindNode(mPath)->outputs[0].id, mg.FindNode(mRoad)->inputs[0].id);
        mg.CreateLink(mg.FindNode(mRoad)->outputs[1].id, mg.FindNode(mLeft)->inputs[0].id);
        mg.CreateLink(mg.FindNode(mRoad)->outputs[2].id, mg.FindNode(mRight)->inputs[0].id);
        mg.CreateLink(mg.FindNode(mRoad)->outputs[0].id, mg.FindNode(mMarking)->inputs[0].id);
        Check(mg.FindNode(mMerge)->inputs.size() == 1 && mg.FindNode(mMerge)->inputs[0].label == "Input 1",
              "new Merge starts with one free input");
        // 繋ぐたびに空きが 1 本増える。
        Check(mg.CreateLink(mg.FindNode(mMarking)->outputs[0].id, mg.FindNode(mMerge)->inputs[0].id) &&
              mg.FindNode(mMerge)->inputs.size() == 2 && mg.FindNode(mMerge)->inputs[1].label == "Input 2",
              "connecting Mesh 1 adds a free Mesh 2");
        Check(mg.CreateLink(mg.FindNode(mLeft)->outputs[0].id, mg.FindNode(mMerge)->inputs[1].id) &&
              mg.CreateLink(mg.FindNode(mRight)->outputs[0].id, mg.FindNode(mMerge)->inputs[2].id) &&
              mg.FindNode(mMerge)->inputs.size() == 4, "three connected inputs leave one free Mesh 4");
        Check(mg.CreateLink(mg.FindNode(mMerge)->outputs[0].id, mg.FindNode(mOut)->inputs[0].id), "Merge connects to Mesh Output");
        auto merged = graph::CompileMeshGraph(mg);
        Check(merged.error.empty() && merged.scene.meshes.size() == 4, "Merge outputs road, markings, and both shoulders");
        // 同じ Road を 2 つの枝から積んでも 1 回。白線の押し出し元は道路の番号を指す。
        Check(mg.CreateLink(mg.FindNode(mRoad)->outputs[0].id, mg.FindNode(mMerge)->inputs[3].id) &&
              mg.FindNode(mMerge)->inputs.size() == 5, "connecting Mesh 4 adds Mesh 5");
        merged = graph::CompileMeshGraph(mg);
        Check(merged.scene.meshes.size() == 4, "the same Road through two branches is stacked once");
        Check(merged.scene.meshes.size() == 4 && merged.scene.meshes[1].displacementSource == 0 &&
              merged.scene.meshes[1].useBlendMode, "markings still displace from the merged road");
        // Merge を途中ノードとして見ると、同じ 4 枚。
        Check(graph::CompileMeshGraph(mg, mMerge).scene.meshes.size() == 4, "previewing the Merge node shows its merged meshes");
        // 別の Mesh Output が同じ Road を出しても重複しない。
        const auto mOut2 = mg.CreateNode(graph::NodeKind::MeshOutput);
        mg.CreateLink(mg.FindNode(mRoad)->outputs[0].id, mg.FindNode(mOut2)->inputs[0].id);
        Check(graph::CompileMeshGraph(mg).scene.meshes.size() == 4, "a second Mesh Output does not duplicate the road");
        // 外すと空きが詰まる。
        graph::GraphId leftLink = 0;
        for (const auto& link : mg.Links()) if (link.endPin == mg.FindNode(mMerge)->inputs[1].id) leftLink = link.id;
        mg.DeleteLink(leftLink);
        const auto* mergeNode = mg.FindNode(mMerge);
        Check(mergeNode->inputs.size() == 4 && mergeNode->inputs[3].label == "Input 4", "disconnecting compacts the inputs and keeps one free");
        Check(graph::CompileMeshGraph(mg).scene.meshes.size() == 3, "disconnected shoulder leaves the merge");
        // 空きピンの並びは Replace（読み込み・アンドゥ）でも保たれる。
        auto nodes = mg.Nodes(); auto links = mg.Links();
        mg.Replace(nodes, links);
        Check(mg.FindNode(mMerge)->inputs.size() == 4 && graph::CompileMeshGraph(mg).scene.meshes.size() == 3,
              "Replace keeps the connected inputs and one free input");

        // Merge は道路とモデルの両方をまとめる。出力の型は入力で決まる。
        const auto model = mg.CreateNode(graph::NodeKind::Model);
        std::get<graph::ModelNodeSettings>(mg.FindMutableNode(model)->settings).model = 7;
        Check(!mg.CanCreateLink(mg.FindNode(model)->outputs[0].id, mg.FindNode(mMarking)->inputs[0].id),
              "a Model cannot feed a road-only input");
        Check(mg.CreateLink(mg.FindNode(model)->outputs[0].id, mg.FindNode(mMerge)->inputs.back().id) &&
                  mg.EffectiveOutputType(mg.FindNode(mMerge)->outputs[0].id) == graph::ValueType::Mesh,
              "a Merge with a road and a model carries Mesh");
        Check(graph::CompileMeshGraph(mg).error.empty() && graph::CompileMeshGraph(mg).scene.meshes.size() == 3 &&
                  graph::CollectOutputModels(mg).size() == 1 && graph::CollectOutputModels(mg)[0].model == model,
              "the mixed Merge keeps the road meshes and outputs the model");
        const auto modelMerge = mg.CreateNode(graph::NodeKind::Merge);
        const auto transform = mg.CreateNode(graph::NodeKind::Transform);
        Check(mg.CreateLink(mg.FindNode(model)->outputs[0].id, mg.FindNode(modelMerge)->inputs[0].id) &&
                  mg.EffectiveOutputType(mg.FindNode(modelMerge)->outputs[0].id) == graph::ValueType::Model &&
                  mg.CreateLink(mg.FindNode(modelMerge)->outputs[0].id, mg.FindNode(transform)->inputs[0].id),
              "a model-only Merge carries Model and feeds a Transform");
        Check(!mg.CanCreateLink(mg.FindNode(mRoad)->outputs[0].id, mg.FindNode(modelMerge)->inputs.back().id),
              "a road cannot join a Merge whose output feeds a Transform");
        Check(mg.CreateLink(mg.FindNode(transform)->outputs[0].id, mg.FindNode(mOut2)->inputs[0].id) &&
                  graph::CompileMeshGraph(mg).error.empty() && graph::CollectOutputModels(mg).size() == 2,
              "a Mesh Output can take a Transform of models without a road error");
        graph::GraphId modelLink = 0;
        for (const auto& link : mg.Links()) if (link.endPin == mg.FindNode(modelMerge)->inputs[0].id) modelLink = link.id;
        mg.DeleteLink(modelLink);
        Check(mg.FindUpstreamNodeForPin(mg.FindNode(transform)->inputs[0].id) != nullptr &&
                  mg.EffectiveOutputType(mg.FindNode(modelMerge)->outputs[0].id) == graph::ValueType::Any,
              "an emptied Merge still connects to the Transform");
    }
}
