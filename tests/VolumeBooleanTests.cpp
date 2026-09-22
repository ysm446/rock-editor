#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <numbers>

using namespace rock::tests;
using namespace rock;

namespace {
geometry::VolumeGrid BoxVolume(const std::array<float, 3>& size, int resolution, std::string& error) {
    return geometry::MeshToVolume(geometry::MakeBox(size), {resolution}, error);
}
bool Measure(const geometry::VolumeGrid& grid, geometry::MeshInfo& info,
             geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra) {
    std::string error;
    const auto mesh = geometry::VolumeSurface(grid, error, method);
    return error.empty() && geometry::InspectMesh(mesh, info) && info.closed;
}
}  // namespace

void RunVolumeBooleanTests() {
    Section("Volume Boolean");
    std::string error;
    // A: 2 m の立方体。B: 1 m の立方体を +X へ 1 m ずらし、A の面から半分だけ出す。
    const auto a = BoxVolume({2, 2, 2}, 40, error);
    Check(error.empty(), "A のボリューム");
    const auto small = BoxVolume({1, 1, 1}, 40, error);
    geometry::VolumeTransformSettings shift;
    shift.position = {1, 0, 0};
    const auto b = geometry::TransformVolume(small, shift, error);
    Check(error.empty(), "B のボリューム");
    geometry::MeshInfo aInfo, bInfo;
    Check(Measure(a, aInfo) && Measure(b, bInfo), "入力は閉じた表面を持つ");
    // 重なりは 0.5 × 1 × 1。格子の近似なので、体積は表面積 × セル間隔の程度まで許す。
    const double tolerance = 24 * a.spacing;

    geometry::VolumeBooleanSettings settings;
    const auto united = geometry::CombineVolumes(a, b, settings, error);
    geometry::MeshInfo unitedInfo;
    Check(error.empty() && Measure(united, unitedInfo) && unitedInfo.components == 1 &&
              std::abs(unitedInfo.volume - 8.5) < tolerance,
          "和は重なりを二重に数えない（8 + 1 − 0.5）");
    Check(std::abs(unitedInfo.maximum.x - 1.5f) < a.spacing && std::abs(unitedInfo.minimum.x + 1) < a.spacing,
          "和は B を含む範囲まで広がる");
    Check(united.spacing == a.spacing, "結果は A のセル間隔を引き継ぐ");
    Check(united.dimensions[0] > a.dimensions[0] && united.dimensions[1] == a.dimensions[1],
          "和の格子は必要な軸だけ広がる");

    settings.operation = geometry::VolumeBooleanOperation::Intersection;
    const auto common = geometry::CombineVolumes(a, b, settings, error);
    geometry::MeshInfo commonInfo;
    Check(error.empty() && Measure(common, commonInfo) && commonInfo.components == 1 &&
              std::abs(commonInfo.volume - .5) < tolerance * .25,
          "交差は重なりだけを残す");
    Check(common.values.size() < a.values.size(), "交差の格子は重なる範囲へ狭まる");

    settings.operation = geometry::VolumeBooleanOperation::Difference;
    const auto carved = geometry::CombineVolumes(a, b, settings, error);
    geometry::MeshInfo carvedInfo;
    Check(error.empty() && Measure(carved, carvedInfo) && carvedInfo.components == 1 &&
              std::abs(carvedInfo.volume - 7.5) < tolerance,
          "差は A から B を取り除く");
    Check(carved.dimensions == a.dimensions && carved.origin.x == a.origin.x && carved.origin.y == a.origin.y &&
              carved.origin.z == a.origin.z,
          "差は A の格子をそのまま使う");
    geometry::MeshInfo carvedDual;
    Check(Measure(carved, carvedDual, geometry::VolumeMeshingMethod::DualContouring) &&
              std::abs(carvedDual.volume - 7.5) < tolerance,
          "差の結果を Dual Contouring でも閉じた表面にできる");

    // A の格子点は補間せずに引き継ぐ。B から離れた点は A と同じ値になる。
    bool exact = true;
    for (uint32_t z = 0; z < a.dimensions[2]; ++z)
        for (uint32_t y = 0; y < a.dimensions[1]; ++y) exact &= carved.values[carved.Index(2, y, z)] == a.values[a.Index(2, y, z)];
    Check(exact, "B の影響がない格子点は A の値と完全に一致する");

    const auto again = geometry::CombineVolumes(a, b, settings, error);
    Check(again.values == carved.values && again.dimensions == carved.dimensions, "同じ入力から同じ結果を得る");

    // なめらかさ。和はつなぎ目が盛り上がり、差はえぐれた縁が埋まる。どちらも体積が増える。
    settings.operation = geometry::VolumeBooleanOperation::Union;
    settings.blend = .4f;
    const auto blended = geometry::CombineVolumes(a, b, settings, error);
    geometry::MeshInfo blendedInfo;
    Check(error.empty() && Measure(blended, blendedInfo) && blendedInfo.volume > unitedInfo.volume + .01 &&
              blendedInfo.volume < unitedInfo.volume + 1,
          "なめらかな和はつなぎ目を埋める");
    settings.operation = geometry::VolumeBooleanOperation::Difference;
    const auto softCarved = geometry::CombineVolumes(a, b, settings, error);
    geometry::MeshInfo softInfo;
    Check(error.empty() && Measure(softCarved, softInfo) && softInfo.volume < carvedInfo.volume - .01,
          "なめらかな差は縁を余分に削る");

    // 大きな幅でも外周は空のまま残り、表面が閉じる。
    settings.operation = geometry::VolumeBooleanOperation::Union;
    settings.blend = 2;
    const auto wide = geometry::CombineVolumes(a, b, settings, error);
    geometry::MeshInfo wideInfo;
    Check(error.empty() && Measure(wide, wideInfo), "幅の広いなめらかな和も閉じた表面になる");

    // 離れた B。和は2つの成分、交差と差は診断または A のまま。
    geometry::VolumeTransformSettings farShift;
    farShift.position = {2.2f, 0, 0};
    const auto apart = geometry::TransformVolume(small, farShift, error);
    settings = {};
    const auto two = geometry::CombineVolumes(a, apart, settings, error);
    geometry::MeshInfo twoInfo;
    Check(error.empty() && Measure(two, twoInfo) && twoInfo.components == 2 &&
              std::abs(twoInfo.volume - 9) < tolerance,
          "離れた B との和は2つの塊になる");
    settings.operation = geometry::VolumeBooleanOperation::Intersection;
    geometry::CombineVolumes(a, apart, settings, error);
    Check(!error.empty(), "重ならない交差は診断する");
    settings.operation = geometry::VolumeBooleanOperation::Difference;
    const auto untouched = geometry::CombineVolumes(a, apart, settings, error);
    Check(error.empty() && untouched.values == a.values, "重ならない差は A をそのまま返す");
    const auto gone = geometry::CombineVolumes(small, a, settings, error);
    Check(!error.empty() && gone.values.empty(), "B が A を覆う差は内部が残らず診断する");

    // 上限と不正な設定。
    geometry::VolumeTransformSettings veryFar;
    veryFar.position = {40, 0, 0};
    const auto distant = geometry::TransformVolume(small, veryFar, error);
    settings = {};
    geometry::CombineVolumes(a, distant, settings, error);
    Check(error.find("256") != std::string::npos, "和の格子が上限を超える配置を診断する");
    settings.blend = -1;
    geometry::CombineVolumes(a, b, settings, error);
    Check(!error.empty(), "負のなめらかさを拒否する");
    settings.blend = std::numeric_limits<float>::quiet_NaN();
    geometry::CombineVolumes(a, b, settings, error);
    Check(!error.empty(), "非有限のなめらかさを拒否する");
    settings = {};
    geometry::CombineVolumes(a, {}, settings, error);
    Check(!error.empty(), "空の B を拒否する");
    Check(geometry::ParseVolumeBooleanOperation(geometry::VolumeBooleanOperationName(
              geometry::VolumeBooleanOperation::Difference)) == geometry::VolumeBooleanOperation::Difference &&
              geometry::ParseVolumeBooleanOperation("?") == geometry::VolumeBooleanOperation::Union,
          "演算の保存名を往復でき、不明な名前は和として読む");

    // グラフ。Base Shape ×2 → To Volume ×2 →（B は Volume Transform）→ Volume Boolean → Volume to Mesh。
    Section("Volume Boolean のグラフ");
    graph::NodeGraph g;
    const auto shapeA = g.CreateNode(graph::NodeKind::BaseRock), shapeB = g.CreateNode(graph::NodeKind::BaseRock),
               volumeA = g.CreateNode(graph::NodeKind::ToVolume), volumeB = g.CreateNode(graph::NodeKind::ToVolume),
               moveB = g.CreateNode(graph::NodeKind::VolumeTransform),
               boolean = g.CreateNode(graph::NodeKind::VolumeBoolean),
               surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to, size_t pin) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[pin].id);
    };
    Check(g.FindNode(boolean)->inputs.size() == 2 && g.FindNode(boolean)->outputs.size() == 1 &&
              std::holds_alternative<geometry::VolumeBooleanSettings>(g.FindNode(boolean)->settings),
          "ノードは A / B の入力と Volume 出力、既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shapeA)->outputs[0].id, g.FindNode(boolean)->inputs[0].id),
          "Mesh 出力は直接つなげない");
    std::get<graph::BaseRockNodeSettings>(g.FindMutableNode(shapeB)->settings).size = {1, 1, 1};
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volumeA)->settings).resolution = 32;
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volumeB)->settings).resolution = 32;
    std::get<geometry::VolumeTransformSettings>(g.FindMutableNode(moveB)->settings).position = {1, 0, 0};
    Check(link(shapeA, volumeA, 0) && link(shapeB, volumeB, 0) && link(volumeB, moveB, 0) &&
              link(volumeA, boolean, 0) && link(boolean, surface, 0),
          "A 側と下流を接続できる");
    const auto missing = graph::EvaluateRocks(g, surface);
    Check(!missing.error.empty(), "B が未接続なら診断する");
    Check(link(moveB, boolean, 1), "B 側を接続できる");
    std::get<geometry::VolumeBooleanSettings>(g.FindMutableNode(boolean)->settings).operation =
        geometry::VolumeBooleanOperation::Difference;
    graph::RockEvaluationCache cache;
    const auto first = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(first.error.empty() && first.rocks.size() == 1 && geometry::InspectMesh(first.rocks[0].mesh, graphInfo) &&
              graphInfo.closed && std::abs(graphInfo.volume - 7.5) < .6,
          "グラフの評価で差の形になる");
    const auto direct = graph::EvaluateRocks(g, boolean, &cache);
    const auto repeated = graph::EvaluateRocks(g, boolean, &cache);
    Check(direct.error.empty() && direct.rocks.size() == 1 && direct.rocks[0].volume &&
              repeated.rocks[0].volume == direct.rocks[0].volume,
          "変更がなければボリュームを再利用する");
    std::get<geometry::VolumeBooleanSettings>(g.FindMutableNode(boolean)->settings).blend = .3f;
    const auto softened = graph::EvaluateRocks(g, boolean, &cache);
    Check(softened.error.empty() && softened.rocks[0].volume != direct.rocks[0].volume, "設定の変更で作り直す");
    const auto keptB = graph::EvaluateRocks(g, moveB, &cache);
    std::get<geometry::VolumeTransformSettings>(g.FindMutableNode(moveB)->settings).position = {1, .5f, 0};
    const auto movedB = graph::EvaluateRocks(g, boolean, &cache);
    const auto keptA = graph::EvaluateRocks(g, volumeA, &cache);
    Check(movedB.error.empty() && movedB.rocks[0].volume != softened.rocks[0].volume, "B 側の上流の変更で作り直す");
    const auto keptAAgain = graph::EvaluateRocks(g, volumeA, &cache);
    Check(keptA.rocks[0].volume == keptAAgain.rocks[0].volume && keptB.error.empty(),
          "変更していない A 側のボリュームは再利用する");
}
