#include "TestSupport.h"
#include "renderer/AxisProjection.h"
#include "renderer/MeshData.h"
#include "renderer/ModelAsset.h"

#include <algorithm>
#include <cmath>
#include <limits>

void RunMeshSceneTests() {
    using namespace rock;
    tests::Section("Mesh scene");
    renderer::SceneMesh mesh;
    mesh.geometry.vertices = {
        {{0, 0, 0}, {0, 1, 0}, {1, 0, 0, -1}, {0, 0}},
        {{0, 0, 4}, {0, 1, 0}, {1, 0, 0, -1}, {0, 1}},
        {{3, 0, 4}, {0, 1, 0}, {1, 0, 0, -1}, {1, 1}}};
    mesh.geometry.indices = {0, 1, 2};
    renderer::MeshScene scene{{mesh, mesh}};
    scene.meshes[1].material.metallic = 1.0f;
    tests::Check(renderer::ValidateMeshScene(scene), "Multiple meshes are valid");
    tests::Check(std::abs(renderer::MeshSceneRadius(scene) - 5.0f) < 0.0001f,
                 "Radius includes world coordinates");
    scene.meshes[0].geometry.indices[0] = 99;
    tests::Check(!renderer::ValidateMeshScene(scene), "Invalid index rejected");
    scene.meshes[0] = mesh;
    scene.meshes[0].geometry.vertices[0].normal = {0, 0, 0};
    tests::Check(!renderer::ValidateMeshScene(scene), "Zero normal rejected");
    scene.meshes[0] = mesh;
    scene.meshes[0].geometry.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    tests::Check(!renderer::ValidateMeshScene(scene), "Nonfinite position rejected");
    tests::Check(renderer::ValidateMeshScene(renderer::MeshScene{}), "Empty scene is valid");

    tests::Section("Model node hierarchy");
    {
        using namespace DirectX;
        // 戦車の形: 一番上（軸を Z-up から Y-up へ回した親）→ 砲塔（上へ 2 m）→ 砲身（前へ 1 m）。
        // 一番上は FBX を Blender から書き出したときのように X へ −90 度回っていて、子の軸はモデルの軸と違う。
        renderer::ModelGeometry geometry;
        geometry.nodes.resize(3);
        geometry.nodes[0].name = "root";
        XMStoreFloat4x4(&geometry.nodes[0].bindLocal, XMMatrixRotationX(-XM_PIDIV2));
        geometry.nodes[1].name = "turret";
        geometry.nodes[1].parent = 0;
        // Z-up の座標で上へ 2 m（親の −90 度で Y-up の +Y になる）。
        XMStoreFloat4x4(&geometry.nodes[1].bindLocal, XMMatrixTranslation(0.0f, 0.0f, 2.0f));
        geometry.nodes[2].name = "barrel";
        geometry.nodes[2].parent = 1;
        // Z-up の座標で前（−Y）へ 1 m → Y-up の +Z。
        XMStoreFloat4x4(&geometry.nodes[2].bindLocal, XMMatrixTranslation(0.0f, -1.0f, 0.0f));
        const auto point = [](const std::vector<XMFLOAT4X4>& worlds, size_t node, XMFLOAT3 local) {
            XMFLOAT3 out;
            XMStoreFloat3(&out, XMVector3TransformCoord(XMLoadFloat3(&local), XMLoadFloat4x4(&worlds[node])));
            return out;
        };
        const auto near = [](XMFLOAT3 a, XMFLOAT3 b) {
            return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f && std::abs(a.z - b.z) < 1e-4f;
        };
        std::vector<XMFLOAT4X4> worlds;
        renderer::ModelNodeWorlds(geometry, {}, worlds);
        tests::Check(near(point(worlds, 1, {0, 0, 0}), {0, 2, 0}) && near(point(worlds, 2, {0, 0, 0}), {0, 2, 1}),
                     "Bind pose places child nodes through their parents");
        renderer::ModelNodeRotation turret;
        turret.node = "turret";
        turret.rotationDegrees[1] = 90.0f;
        renderer::ModelNodeWorlds(geometry, {turret}, worlds);
        tests::Check(near(point(worlds, 1, {0, 0, 0}), {0, 2, 0}), "Rotated node keeps its origin");
        tests::Check(near(point(worlds, 2, {0, 0, 0}), {1, 2, 0}),
                     "Rotation uses the model Y axis and carries the child (+Z turns to +X)");
        renderer::ModelNodeRotation barrel;
        barrel.node = "barrel";
        barrel.rotationDegrees[0] = -90.0f;
        // 砲身の先（Y-up で砲身の原点から +Z へ 1 m、Z-up のローカルでは −Y へ 1 m）を X へ −90 度 → 上を向く。
        renderer::ModelNodeWorlds(geometry, {barrel}, worlds);
        tests::Check(near(point(worlds, 2, {0, -1, 0}), {0, 3, 1}), "Negative X rotation raises the barrel tip");
    }

    tests::Section("Mesh outline edges");
    {
        // 2 枚の三角形で作る四角形。対角線は共有されるので外周は 4 辺。
        renderer::MeshData quad;
        quad.vertices = {
            {{0, 0, 0}, {0, 1, 0}, {1, 0, 0, -1}, {0, 0}},
            {{1, 0, 0}, {0, 1, 0}, {1, 0, 0, -1}, {1, 0}},
            {{1, 0, 1}, {0, 1, 0}, {1, 0, 0, -1}, {1, 1}},
            {{0, 0, 1}, {0, 1, 0}, {1, 0, 0, -1}, {0, 1}}};
        quad.indices = {0, 1, 2, 0, 2, 3};
        const auto outline = renderer::MeshOutlineEdges(quad);
        tests::Check(outline.size() == 8, "Quad outline has 4 edges");
        bool diagonal = false;
        for (size_t i = 0; i + 1 < outline.size(); i += 2) {
            const auto lo = std::min(outline[i], outline[i + 1]);
            const auto hi = std::max(outline[i], outline[i + 1]);
            if (lo == 0 && hi == 2) diagonal = true;
        }
        tests::Check(!diagonal, "Shared diagonal is not an outline edge");

        // 対角線の頂点を複製した（UV の継ぎ目のような）四角形でも、位置が同じなら外周は 4 辺のまま。
        renderer::MeshData split = quad;
        split.vertices.push_back(quad.vertices[0]);
        split.vertices.push_back(quad.vertices[2]);
        split.vertices[4].uv = {0.5f, 0.5f};
        split.indices = {0, 1, 2, 4, 5, 3};
        tests::Check(renderer::MeshOutlineEdges(split).size() == 8, "Duplicated seam vertices merge by position");

        tests::Check(renderer::MeshOutlineEdges(renderer::MeshData{}).empty(), "Empty mesh has no outline");
    }

    tests::Section("Move axis projection");
    using namespace DirectX;
    const auto projection = XMMatrixPerspectiveFovRH(0.8f, 1.5f, 0.01f, 100.0f);
    const XMFLOAT3 center{0,0,0};
    for (float yaw : {0.001f, 0.5f, 1.57f, 3.14f}) {
        for (float pitch : {0.001f, 0.5f, 1.55f}) {
            const XMVECTOR eye = XMVectorSet(10*std::cos(pitch)*std::sin(yaw),
                10*std::sin(pitch), 10*std::cos(pitch)*std::cos(yaw), 1);
            const auto vp = XMMatrixLookAtRH(eye, XMVectorZero(), XMVectorSet(0,1,0,0))*projection;
            const auto axes = renderer::ProjectMoveAxes(vp, center, 900, 600, 64);
            for (int i = 0; i < 3; ++i) {
                const auto delta = axes.delta[i];
                tests::Check(std::isfinite(delta.x) && std::isfinite(delta.y) &&
                             std::hypot(delta.x,delta.y) <= 64.001f, "axes remain finite and bounded");
                const XMVECTOR step = XMVectorSet(i==0 ? 0.001f:0, i==2 ? 0.001f:0, i==1 ? 0.001f:0, 1);
                const XMVECTOR screen = XMVector3TransformCoord(step,vp);
                const float dx=XMVectorGetX(screen)*450, dy=-XMVectorGetY(screen)*300;
                tests::Check(delta.x*dx+delta.y*dy >= -1e-5f, "positive world axis keeps projected direction");
            }
        }
    }
    const auto view = XMMatrixLookAtRH(XMVectorSet(0,0,0.5f,1), XMVectorZero(), XMVectorSet(0,1,0,0));
    const auto closeAxes = renderer::ProjectMoveAxes(view*projection, {0,0,0},900,600,64);
    tests::Check(std::hypot(closeAxes.delta[1].x,closeAxes.delta[1].y) < 0.001f,
                 "view-aligned axis is not stretched to full length");
    tests::Check(closeAxes.delta[0].x > 63 && closeAxes.delta[2].y < -63,
                 "near camera plane preserves X and Y orientation");
    {
        const auto surfaceView = XMMatrixLookAtRH(XMVectorSet(4,6,8,1), XMVectorZero(), XMVectorSet(0,1,0,0)) * projection;
        const XMFLOAT3 directions[] = {{0,0,2}, {1,0.5f,0}, {0,1,0}};
        const auto local = renderer::ProjectMoveAxes(surfaceView, center, 900,600,64,2,directions);
        const auto world = renderer::ProjectMoveAxes(surfaceView, center, 900,600,64);
        tests::Check(std::abs(local.pixelsPerMeter[0] - world.pixelsPerMeter[1]*2) < 1e-3f &&
                     local.pixelsPerMeter[2] == 0 && local.delta[2].x == 0 && local.delta[2].y == 0,
                     "surface axes preserve coordinate scale and omit height axis");
        const auto origin = XMVector3TransformCoord(XMLoadFloat3(&center), surfaceView);
        const auto step = XMVector3TransformCoord(XMVectorScale(XMLoadFloat3(&directions[1]), 0.001f), surfaceView);
        const float dx = (XMVectorGetX(step)-XMVectorGetX(origin))*450;
        const float dy = -(XMVectorGetY(step)-XMVectorGetY(origin))*300;
        tests::Check(local.delta[1].x*dx + local.delta[1].y*dy > 0,
                     "inclined surface axis follows the road rather than a world axis");
    }
    const auto behind = renderer::ProjectMoveAxes(view*projection, {0,0,1},900,600,64);
    tests::Check(behind.pixelsPerMeter[0] == 0, "axes behind camera are rejected");
}
