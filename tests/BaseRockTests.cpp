#include <cmath>
#include <limits>
#include <numbers>
#include "TestSupport.h"
#include "app/UndoHistory.h"
#include "geometry/BaseRock.h"
#include "geometry/Pieces.h"
#include "graph/RockEvaluator.h"
#include "renderer/RockMesh.h"

void RunBaseRockTests() {
    using namespace rock;
    using geometry::BaseShape;
    using tests::Check;
    tests::Section("母岩形状 — 閉包・寸法・曲面・決定性");
    geometry::BaseRockSettings settings;
    std::string error;
    auto mesh = geometry::MakeBaseRock(settings, error);
    const auto box = geometry::MakeBox(settings.size);
    Check(error.empty() && mesh.positions == box.positions && mesh.triangles == box.triangles,
          "従来 Box と頂点・面が一致");
    for (auto shape : {BaseShape::RoundedBox, BaseShape::Sphere, BaseShape::Ellipsoid}) {
        settings.shape = shape;
        settings.size =
            shape == BaseShape::Sphere ? std::array<float, 3>{2, 2, 2} : std::array<float, 3>{2, 3, 4};
        for (int resolution : {4, 8, 16, 32}) {
            settings.subdivisions = resolution;
            mesh = geometry::MakeBaseRock(settings, error);
            geometry::MeshInfo info;
            Check(error.empty() && geometry::InspectMesh(mesh, info) && info.closed && info.components == 1 &&
                      info.volume > 0,
                  "曲面が閉じた1連結体");
            Check(mesh.positions.size() == size_t(6 * resolution * resolution + 2) &&
                      mesh.triangles.size() == size_t(12 * resolution * resolution),
                  "面の継ぎ目と角の頂点を共有");
            bool outward = true;
            for (const auto& f : mesh.triangles) {
                const auto n = geometry::FaceNormal(mesh, f), p = mesh.positions[f[0]];
                outward &= double(n.x) * p.x + double(n.y) * p.y + double(n.z) * p.z > 0;
            }
            Check(outward, "全ての面は原点に対して外向き");
            Check(std::abs(info.maximum.x - settings.size[0] * 0.5) < 1e-6 &&
                      std::abs(info.minimum.y + settings.size[1] * 0.5) < 1e-6 &&
                      std::abs(info.maximum.z - settings.size[2] * 0.5) < 1e-6,
                  "外接箱が指定寸法に一致");
            if (shape != BaseShape::RoundedBox) {
                bool surface = true;
                for (auto p : mesh.positions) {
                    const double q = std::pow(p.x / (settings.size[0] * 0.5), 2) +
                                     std::pow(p.y / (settings.size[1] * 0.5), 2) +
                                     std::pow(p.z / (settings.size[2] * 0.5), 2);
                    surface &= std::abs(q - 1) < 1e-6;
                }
                Check(surface, "球・楕円体の頂点が解析曲面上にある");
                const double volume =
                    std::numbers::pi / 6 * settings.size[0] * settings.size[1] * settings.size[2];
                Check(info.volume < volume && info.volume > volume * (resolution == 4 ? 0.9 : 0.97),
                      "体積が解析値の内側へ収束");
            }
            renderer::MeshScene scene;
            renderer::SceneMesh m;
            m.geometry = renderer::MakeRockMeshData(mesh);
            scene.meshes.push_back(m);
            Check(renderer::ValidateMeshScene(scene), "曲面の描画データが有効");
        }
    }
    settings = {};
    settings.shape = BaseShape::RoundedBox;
    settings.roundness = 0;
    Check(geometry::MakeBaseRock(settings, error).positions == box.positions, "丸み0は正確な Box");
    settings.roundness = 1;
    mesh = geometry::MakeBaseRock(settings, error);
    settings.shape = BaseShape::Sphere;
    Check(geometry::MakeBaseRock(settings, error).positions == mesh.positions, "立方体の最大丸みは球");
    settings.size = {2, 5, 7};
    mesh = geometry::MakeBaseRock(settings, error);
    geometry::MeshInfo sphereInfo;
    geometry::InspectMesh(mesh, sphereInfo);
    Check(sphereInfo.maximum.y == 1 && sphereInfo.maximum.z == 1, "Sphere は X の直径を全軸に使う");
    for (float scale : {0.001f, 1000.0f})
        for (auto shape : {BaseShape::RoundedBox, BaseShape::Sphere, BaseShape::Ellipsoid}) {
            settings = {};
            settings.shape = shape;
            settings.size = {scale, scale, scale};
            mesh = geometry::MakeBaseRock(settings, error);
            Check(error.empty() && !mesh.triangles.empty(), "最小/最大寸法の母岩を生成");
        }
    tests::Section("母岩ノイズ — seed・形状品質・無効入力");
    for (auto shape : {BaseShape::Box, BaseShape::RoundedBox, BaseShape::Sphere, BaseShape::Ellipsoid}) {
        settings = {};
        settings.shape = shape;
        settings.size = {2, 3, 4};
        settings.noiseStrength = 0.15f;
        for (int seed : {0, 1, 42, 1000000000}) {
            settings.seed = seed;
            mesh = geometry::MakeBaseRock(settings, error);
            const auto repeated = geometry::MakeBaseRock(settings, error);
            Check(
                error.empty() && mesh.positions == repeated.positions && mesh.triangles == repeated.triangles,
                "同じ seed・設定で完全再現");
            auto cleanSettings = settings;
            cleanSettings.noiseStrength = 0;
            // Box の非ノイズ版は粗い8頂点なので、他の形状で対応頂点の最大変位率を比較する。
            if (shape != BaseShape::Box) {
                const auto clean = geometry::MakeBaseRock(cleanSettings, error);
                bool bounded = true;
                for (size_t i = 0; i < mesh.positions.size(); ++i) {
                    const auto a = mesh.positions[i], b = clean.positions[i];
                    const double length =
                        std::sqrt(double(b.x) * b.x + double(b.y) * b.y + double(b.z) * b.z);
                    const double displacement =
                        std::sqrt(std::pow(double(a.x) - b.x, 2) + std::pow(double(a.y) - b.y, 2) +
                                  std::pow(double(a.z) - b.z, 2));
                    bounded &= displacement <= length * 0.150001;
                }
                Check(bounded, "変位は指定した最大率以内");
            }
            bool outward = true;
            for (const auto& f : mesh.triangles) {
                const auto n = geometry::FaceNormal(mesh, f), p = mesh.positions[f[0]];
                outward &= n.x * p.x + n.y * p.y + n.z * p.z > 0;
            }
            Check(outward, "ノイズ後も原点に対して面が反転しない");
            ++settings.seed;
            Check(geometry::MakeBaseRock(settings, error).positions != mesh.positions,
                  "seed を変えると輪郭が変わる");
        }
    }
    bool boundaryCases = true;
    for (auto shape : {BaseShape::Box, BaseShape::RoundedBox, BaseShape::Sphere, BaseShape::Ellipsoid}) {
        for (int resolution : {4, 16, 32}) {
            settings = {};
            settings.shape = shape;
            settings.subdivisions = resolution;
            settings.noiseStrength = 0.15f;
            settings.noiseScale = 4;
            settings.roundness = 1;
            settings.seed = 12345;
            mesh = geometry::MakeBaseRock(settings, error);
            geometry::MeshInfo info;
            boundaryCases &=
                error.empty() && geometry::InspectMesh(mesh, info) && info.closed && info.components == 1;
            for (const auto& f : mesh.triangles) {
                const auto n = geometry::FaceNormal(mesh, f), p = mesh.positions[f[0]];
                boundaryCases &= double(n.x) * p.x + double(n.y) * p.y + double(n.z) * p.z > 0;
            }
        }
    }
    Check(boundaryCases, "最大の丸み・ノイズ強度・細かさでも閉包と外向きを維持");
    settings = {};
    settings.shape = BaseShape::Invalid;
    Check(geometry::MakeBaseRock(settings, error).positions.empty() && !error.empty(), "不明形状を診断");
    settings = {};
    settings.subdivisions = 1000000;
    Check(geometry::MakeBaseRock(settings, error).positions.empty(), "過剰な分割数を拒否");
    settings = {};
    settings.noiseStrength = std::numeric_limits<float>::quiet_NaN();
    Check(geometry::MakeBaseRock(settings, error).positions.empty(), "非有限ノイズを拒否");
    settings = {};
    settings.roundness = -1;
    Check(geometry::MakeBaseRock(settings, error).positions.empty(), "負の丸みを拒否");
    for (auto shape : {BaseShape::Box, BaseShape::RoundedBox, BaseShape::Sphere, BaseShape::Ellipsoid})
        Check(geometry::ParseBaseShape(geometry::BaseShapeName(shape)) == shape, "形状名の往復");
    Check(geometry::ParseBaseShape("unknown") == BaseShape::Invalid, "不明な保存形状を Box に置換しない");

    tests::Section("母岩形状 — グラフ・Undo");
    graph::NodeGraph graph;
    const auto base = graph.CreateNode(graph::NodeKind::BaseRock);
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    auto& config = std::get<graph::BaseRockNodeSettings>(graph.FindMutableNode(base)->settings);
    config.shape = BaseShape::Ellipsoid;
    config.size = {2, 3, 4};
    config.noiseStrength = .1f;
    config.seed = 17;
    graph.MarkDirty();
    auto evaluated = graph::EvaluateRocks(graph, base);
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1, "グラフから曲面の母岩を生成");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    const auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(graph::EvaluateRocks(graph, base).rocks[0].mesh.positions == box.positions, "Undo で Box に復元");
    const auto redone = history.Redo(restored);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(graph::EvaluateRocks(graph, base).rocks[0].mesh.positions == evaluated.rocks[0].mesh.positions,
          "Redo で形状・ノイズ・seed を再現");

    tests::Section("凸岩峰 — 凸性と分割への接続");
    bool validPeaks = true, convexPeaks = true, fracturePeaks = true, repeatPeaks = true;
    for (int seed=0;seed<8;++seed) for (int sides : {4,7,12}) {
        geometry::BaseRockSettings peak;
        peak.shape = BaseShape::ConvexPeak;
        peak.size = {9,15,7}; peak.seed=seed; peak.peakSides=sides;
        peak.peakTopWidth = seed%2 ? 0 : .6f;
        peak.peakShoulderHeight = seed%2 ? .2f : .85f;
        peak.peakVariation = seed%3 ? .5f : 0;
        peak.peakLeanX = seed%2 ? -.5f : .5f;
        peak.peakLeanZ = seed%3 ? -.5f : .5f;
        const auto hull=geometry::MakeBaseRock(peak,error);
        geometry::MeshInfo info;
        validPeaks &= error.empty() && geometry::InspectMesh(hull,info) && info.closed && info.components==1 && info.volume>0;
        validPeaks &= std::abs(info.maximum.x-info.minimum.x-9)<1e-5 && std::abs(info.maximum.y-info.minimum.y-15)<1e-5 &&
                      std::abs(info.maximum.z-info.minimum.z-7)<1e-5;
        // 全頂点が全外向き面の内側にある。単に閉じているだけでは凹形状を排除できない。
        for (const auto& face : hull.triangles) {
            const auto normal=geometry::FaceNormal(hull,face), a=hull.positions[face[0]];
            for (const auto& p : hull.positions)
                convexPeaks &= double(normal.x)*(p.x-a.x)+double(normal.y)*(p.y-a.y)+double(normal.z)*(p.z-a.z)<3e-6;
        }
        const auto repeated=geometry::MakeBaseRock(peak,error);
        repeatPeaks &= repeated.positions==hull.positions && repeated.triangles==hull.triangles;
        const auto sites=geometry::ScatterPoints(hull,{24,uint32_t(seed+1)},error);
        fracturePeaks &= error.empty();
        const auto pieces=geometry::FractureVoronoi(hull,sites,{},5,error);
        double sum=0;
        for (const auto& p : pieces.pieces) sum+=p.volume;
        fracturePeaks &= error.empty() && pieces.pieces.size()==24 && std::abs(sum-info.volume)<info.volume*2e-5;
    }
    Check(validPeaks, "凸岩峰: 端の設定・複数Seedで閉包・体積・指定寸法を維持");
    Check(convexPeaks, "凸岩峰: 全頂点が全ての面の内側にある");
    Check(fracturePeaks, "凸岩峰: ScatterとVoronoiを直接接続し体積を保って分割できる");
    Check(repeatPeaks, "凸岩峰: Seedと設定で頂点・面を再現");
    Check(geometry::ParseBaseShape("convexPeak")==BaseShape::ConvexPeak, "凸岩峰の保存名");
    auto& peakConfig=std::get<graph::BaseRockNodeSettings>(graph.FindMutableNode(base)->settings);
    peakConfig={}; peakConfig.shape=BaseShape::ConvexPeak;
    const auto volumeNode=graph.CreateNode(graph::NodeKind::ToVolume);
    std::get<geometry::VolumeSettings>(graph.FindMutableNode(volumeNode)->settings).resolution=24;
    graph.CreateLink(graph.FindNode(base)->outputs[0].id,graph.FindNode(volumeNode)->inputs[0].id);
    graph::RockEvaluationCache peakCache;
    const auto oldVolume=graph::EvaluateRocks(graph,volumeNode,&peakCache);
    // CreateNodeで設定への参照が無効になる可能性があるので取り直す。
    std::get<graph::BaseRockNodeSettings>(graph.FindMutableNode(base)->settings).peakLeanX=-.3f;
    const auto newVolume=graph::EvaluateRocks(graph,volumeNode,&peakCache);
    Check(oldVolume.error.empty() && newVolume.error.empty() &&
          oldVolume.rocks[0].volume!=newVolume.rocks[0].volume, "凸岩峰: 輪郭の編集で下流のキャッシュを更新");
}
