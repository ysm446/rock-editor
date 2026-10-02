#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <functional>
#include <numbers>
#include <string>

using namespace rock::tests;
using namespace rock;

namespace {
bool Measure(const geometry::VolumeGrid& grid, geometry::MeshInfo& info,
             geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra) {
    std::string error;
    const geometry::Mesh mesh = geometry::VolumeSurface(grid, error, method);
    return error.empty() && geometry::InspectMesh(mesh, info) && info.closed;
}
// 面の中央の近く（その面の法線方向に表面から 1 セル内側）の格子点で、入力からどれだけ削れたかの最大。
float CarvedAtFace(const geometry::VolumeGrid& before, const geometry::VolumeGrid& after, int axis, int sign) {
    float carved = 0;
    for (uint32_t z = 0; z < before.dimensions[2]; ++z)
        for (uint32_t y = 0; y < before.dimensions[1]; ++y)
            for (uint32_t x = 0; x < before.dimensions[0]; ++x) {
                const auto p = before.Position(x, y, z);
                const float c[3] = {p.x, p.y, p.z};
                // その面: 軸の座標が ±1 の近く、他の 2 軸は中央の ±0.3 m。
                bool central = true;
                for (int i = 0; i < 3; ++i)
                    if (i != axis) central &= std::abs(c[i]) < .3f;
                if (!central || std::abs(c[axis] - float(sign) * .95f) > .06f) continue;
                carved = std::max(carved, after.values[before.Index(x, y, z)] - before.values[before.Index(x, y, z)]);
            }
    return carved;
}
}  // namespace

void RunVolumeErodeTests() {
    Section("Volume Erode");
    std::string error;
    // 2 m の立方体（各軸 -1〜1）。最長辺 2 m。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    geometry::MeshInfo boxInfo;
    Check(error.empty() && Measure(box, boxInfo), "立方体のボリューム");

    // さらされた面: -X から来る風。-X の面が削れ、+X の面（風下）と上面は変わらない。
    geometry::VolumeErodeSettings wind;
    wind.type = geometry::VolumeErodeType::Exposure;
    wind.amount = .05f;
    wind.direction = {-1, 0, 0};
    wind.noise = 0;
    wind.iterations = 2;
    const auto blown = geometry::ErodeVolume(box, wind, error);
    geometry::MeshInfo blownInfo, dual;
    Check(error.empty() && Measure(blown, blownInfo) && blownInfo.components == 1 && blownInfo.volume < boxInfo.volume,
          "さらされた面: 削って体積が減り、1 つの塊のまま");
    Check(Measure(blown, dual, geometry::VolumeMeshingMethod::DualContouring), "さらされた面: Dual Contouring でも閉じた表面にできる");
    Check(blown.dimensions == box.dimensions && blown.spacing == box.spacing, "格子は入力のまま");
    const float windward = CarvedAtFace(box, blown, 0, -1), leeward = CarvedAtFace(box, blown, 0, 1), top = CarvedAtFace(box, blown, 1, 1);
    Check(windward > .06f && leeward < 1e-4f && top < windward * .5f && top > 0,
          "さらされた面: 風上の面が最も削れ、横向きの上面は弱く削れ、風下（陰）は変わらない");
    {
        geometry::VolumeErodeSettings narrow = wind;
        narrow.sharpness = 8;
        const auto focused = geometry::ErodeVolume(box, narrow, error);
        Check(error.empty() && CarvedAtFace(box, focused, 1, 1) < .002f && CarvedAtFace(box, focused, 0, -1) > .06f,
              "集中 8 では横向きの面はほとんど削れない");
    }
    Check(windward <= .05f * 2 + 1e-3f, "さらされた面: 削る深さは量（0.05 × 2 m）を超えない");
    // 形を広げない: 入力で外だった格子点は外のまま（回の間の再距離化で内部の値は深くなることがある）。
    bool neverGrows = true;
    for (size_t i = 0; i < box.values.size(); ++i) neverGrows &= box.values[i] < 0 || blown.values[i] >= 0;
    Check(neverGrows, "さらされた面: 形を広げない");

    // 陰: 風上に別の塊があると、その風下の面は削れない。2 m の立方体の -X 側に板を置く。
    {
        auto shielded = box;
        // 板: x が -1.6〜-1.3、y・z は立方体と同じ範囲（格子の中に収まるよう、格子は立方体のものを広げて使う）。
        geometry::VolumeGrid wide = geometry::MeshToVolume(geometry::MakeBox({4, 2, 2}), {64}, error);
        for (uint32_t z = 0; z < wide.dimensions[2]; ++z)
            for (uint32_t y = 0; y < wide.dimensions[1]; ++y)
                for (uint32_t x = 0; x < wide.dimensions[0]; ++x) {
                    const auto p = wide.Position(x, y, z);
                    const float cube = std::max({std::abs(p.x) - 1, std::abs(p.y) - 1, std::abs(p.z) - 1});
                    const float plate = std::max({std::abs(p.x + 1.45f) - .15f, std::abs(p.y) - 1, std::abs(p.z) - 1});
                    wide.values[wide.Index(x, y, z)] = std::min(cube, plate);
                }
        geometry::VolumeErodeSettings shade = wind;
        shade.shadow = .5f;  // 1 m。板まで 0.3 m
        shade.iterations = 1;
        const auto shadowed = geometry::ErodeVolume(wide, shade, error);
        float cubeFace = 0, plateFace = 0;
        for (uint32_t z = 0; z < wide.dimensions[2]; ++z)
            for (uint32_t y = 0; y < wide.dimensions[1]; ++y)
                for (uint32_t x = 0; x < wide.dimensions[0]; ++x) {
                    const auto p = wide.Position(x, y, z);
                    if (std::abs(p.y) > .3f || std::abs(p.z) > .3f) continue;
                    const float d = shadowed.values[wide.Index(x, y, z)] - wide.values[wide.Index(x, y, z)];
                    if (std::abs(p.x + .95f) < .06f) cubeFace = std::max(cubeFace, d);
                    if (std::abs(p.x + 1.55f) < .06f) plateFace = std::max(plateFace, d);
                }
        Check(error.empty() && plateFace > .03f && cubeFace < 1e-4f, "陰: 風上の板が削れ、その風下にある立方体の面は削れない");
        shade.shadow = 0;
        const auto unshadowed = geometry::ErodeVolume(wide, shade, error);
        float exposedFace = 0;
        for (uint32_t z = 0; z < wide.dimensions[2]; ++z)
            for (uint32_t y = 0; y < wide.dimensions[1]; ++y)
                for (uint32_t x = 0; x < wide.dimensions[0]; ++x) {
                    const auto p = wide.Position(x, y, z);
                    if (std::abs(p.y) > .3f || std::abs(p.z) > .3f || std::abs(p.x + .95f) > .06f) continue;
                    exposedFace = std::max(exposedFace, unshadowed.values[wide.Index(x, y, z)] - wide.values[wide.Index(x, y, z)]);
                }
        Check(error.empty() && exposedFace > .03f, "陰の距離 0 では陰を見ず、板の風下の面も削れる");
    }

    // 流下: 傾いた板。上の面を水が流れ下り、流れが集まる下の縁の近くほど削れる。上面の上の方（出発点）は削れない。
    {
        // 幅 2 m・厚さ 0.4 m・奥行き 2 m の板を X 軸まわりに 35° 傾ける（Z 方向に傾斜）。格子は 3 m の箱で作る。
        geometry::VolumeGrid slab = geometry::MeshToVolume(geometry::MakeBox({3, 3, 3}), {64}, error);
        const float c = std::cos(35 * std::numbers::pi_v<float> / 180), s = std::sin(35 * std::numbers::pi_v<float> / 180);
        for (uint32_t z = 0; z < slab.dimensions[2]; ++z)
            for (uint32_t y = 0; y < slab.dimensions[1]; ++y)
                for (uint32_t x = 0; x < slab.dimensions[0]; ++x) {
                    const auto p = slab.Position(x, y, z);
                    const float ry = p.y * c - p.z * s, rz = p.y * s + p.z * c;
                    slab.values[slab.Index(x, y, z)] = std::max({std::abs(p.x) - 1, std::abs(ry) - .2f, std::abs(rz) - 1});
                }
        geometry::VolumeErodeSettings rain;
        rain.type = geometry::VolumeErodeType::Flow;
        rain.amount = .04f;
        rain.length = .5f;
        rain.width = .02f;
        rain.noise = 0;
        rain.iterations = 2;
        geometry::MeshInfo slabInfo;
        Check(Measure(slab, slabInfo), "傾いた板のボリューム");
        const auto rilled = geometry::ErodeVolume(slab, rain, error);
        geometry::MeshInfo rilledInfo;
        Check(error.empty() && Measure(rilled, rilledInfo) && rilledInfo.components == 1 && rilledInfo.volume < slabInfo.volume,
              "流下: 削って体積が減り、1 つの塊のまま");
        // 上面（回転後の +Y 側の面）の、傾斜の上端の近くと下端の近くでの削れ。
        float upper = 0, lower = 0;
        for (uint32_t z = 0; z < slab.dimensions[2]; ++z)
            for (uint32_t y = 0; y < slab.dimensions[1]; ++y)
                for (uint32_t x = 0; x < slab.dimensions[0]; ++x) {
                    const auto p = slab.Position(x, y, z);
                    const float ry = p.y * c - p.z * s, rz = p.y * s + p.z * c;
                    if (std::abs(p.x) > .6f || std::abs(ry - .15f) > .06f) continue;
                    const float d = rilled.values[slab.Index(x, y, z)] - slab.values[slab.Index(x, y, z)];
                    // この回転では上面の法線は -Z 側へ傾き、水は -Z（rz が負の側）へ流れ下る。上端は rz > 0.75、下の方は rz が -0.85〜-0.3。
                    if (rz > .75f) upper = std::max(upper, d);
                    if (rz > -.85f && rz < -.3f) lower = std::max(lower, d);
                }
        Check(lower > upper + 1e-3f && lower > .01f, "流下: 流れが集まる斜面の下の方ほど削れ、上端はほとんど削れない");
        bool slabNeverGrows = true;
        for (size_t i = 0; i < slab.values.size(); ++i) slabNeverGrows &= slab.values[i] < 0 || rilled.values[i] >= 0;
        Check(slabNeverGrows, "流下: 形を広げない");
    }

    const auto again = geometry::ErodeVolume(box, wind, error);
    Check(error.empty() && again.values == blown.values, "同じ入力と設定から同じ結果を得る");
    geometry::VolumeErodeSettings zero = wind;
    zero.amount = 0;
    Check(geometry::ErodeVolume(box, zero, error).values == box.values && error.empty(), "量 0 では入力のまま");

    const auto rejects = [&](const char* name, const std::function<void(geometry::VolumeErodeSettings&)>& change) {
        geometry::VolumeErodeSettings bad;
        change(bad);
        geometry::ErodeVolume(box, bad, error);
        Check(!error.empty(), name);
    };
    rejects("量が範囲外なら診断する", [](auto& s) { s.amount = .5f; });
    rejects("集中が範囲外なら診断する", [](auto& s) { s.sharpness = 0; });
    rejects("陰の距離が範囲外なら診断する", [](auto& s) { s.shadow = 2; });
    rejects("筋の長さが範囲外なら診断する", [](auto& s) { s.length = 0; });
    rejects("溝の幅が範囲外なら診断する", [](auto& s) { s.width = .5f; });
    rejects("回数が範囲外なら診断する", [](auto& s) { s.iterations = 0; });
    rejects("ばらつきが範囲外なら診断する", [](auto& s) { s.noise = 2; });
    rejects("向きが非有限なら診断する", [](auto& s) { s.direction = {NAN, 0, 0}; });
    Check(geometry::ParseVolumeErodeType(geometry::VolumeErodeTypeName(geometry::VolumeErodeType::Flow)) == geometry::VolumeErodeType::Flow &&
              geometry::ParseVolumeErodeType("?") == geometry::VolumeErodeType::Exposure,
          "種類の保存名を往復でき、不明な名前はさらされた面として読む");

    // グラフ: Base Shape → To Volume → Volume Erode。
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeErode);
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    g.CreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(volume)->inputs[0].id);
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    g.CreateLink(g.FindNode(volume)->outputs[0].id, g.FindNode(node)->inputs[0].id);
    graph::RockEvaluationCache cache;
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && first.rocks.size() == 1 && first.rocks[0].volume, "グラフで評価できる");
    std::get<geometry::VolumeErodeSettings>(g.FindMutableNode(node)->settings).type = geometry::VolumeErodeType::Flow;
    Check(graph::EvaluateRocks(g, node, &cache).rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
    std::get<geometry::VolumeErodeSettings>(g.FindMutableNode(node)->settings).iterations = 99;
    Check(!graph::EvaluateRocks(g, node, &cache).error.empty(), "不正な設定はグラフの診断になる");
}
