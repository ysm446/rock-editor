#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <limits>

using namespace rock::tests;
using namespace rock;

namespace {
bool Measure(const geometry::VolumeGrid& grid, geometry::MeshInfo& info,
             geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra) {
    std::string error;
    const auto mesh = geometry::VolumeSurface(grid, error, method);
    return error.empty() && geometry::InspectMesh(mesh, info) && info.closed;
}
// 位置 (x, y, z) に最も近い格子点の値。
float At(const geometry::VolumeGrid& g, float x, float y, float z) {
    const uint32_t ix = uint32_t(std::lround((x - g.origin.x) / g.spacing));
    const uint32_t iy = uint32_t(std::lround((y - g.origin.y) / g.spacing));
    const uint32_t iz = uint32_t(std::lround((z - g.origin.z) / g.spacing));
    return g.values[g.Index(ix, iy, iz)];
}
// 2 m の立方体の上面（y = 1）に、z 方向へ走る V 字の溝を彫る。表面での幅 0.3 m、深さ 0.8 m（底は y = 0.2）。
// 幅は深さに比例して狭まる。y での半幅 = 0.15 × (y − 0.2) / 0.8。
geometry::VolumeGrid GroovedBox(std::string& error) {
    auto grid = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    for (uint32_t z = 0; z < grid.dimensions[2]; ++z)
        for (uint32_t y = 0; y < grid.dimensions[1]; ++y)
            for (uint32_t x = 0; x < grid.dimensions[0]; ++x) {
                const auto p = grid.Position(x, y, z);
                // 箱の上（y > 1）まで彫ると、外側の距離が実際より小さくなり、外挿の前提が崩れる。
                if (p.y < .2f || p.y > 1.f) continue;
                const float halfWidth = .15f * (p.y - .2f) / .8f;
                auto& value = grid.values[grid.Index(x, y, z)];
                value = std::max(value, halfWidth - std::abs(p.x));
            }
    return grid;
}
}  // namespace

void RunVolumeCloseTests() {
    Section("Volume Close（幅）");
    std::string error;
    const auto grooved = GroovedBox(error);
    Check(error.empty(), "溝のある立方体のボリューム");
    geometry::MeshInfo groovedInfo;
    Check(Measure(grooved, groovedInfo) && groovedInfo.components == 1 && At(grooved, 0, .3f, 0) > 0 && At(grooved, 0, .9f, 0) > 0 &&
              At(grooved, .5f, .5f, 0) < 0,
          "入力は溝の中が外部で、溝の脇が内部の閉じた形");

    // 幅 0.05 × 2 m = 0.1 m。溝の幅が 0.1 m を下回るのは y < 0.2 + 0.8 × (0.1 / 0.3) ≈ 0.47 より下。
    geometry::VolumeCloseSettings settings;
    settings.mode = geometry::VolumeCloseMode::Width;
    settings.width = .05f;
    const auto closed = geometry::CloseVolume(grooved, settings, error);
    geometry::MeshInfo info, dual;
    Check(error.empty() && Measure(closed, info) && info.components == 1, "閉じた1つの塊になる");
    Check(Measure(closed, dual, geometry::VolumeMeshingMethod::DualContouring), "Dual Contouring でも閉じた表面にできる");
    Check(closed.dimensions == grooved.dimensions && closed.origin.x == grooved.origin.x && closed.spacing == grooved.spacing, "格子は入力のまま");
    Check(At(closed, 0, .3f, 0) < 0 && At(closed, 0, .25f, 0) < 0, "幅より狭い溝の奥は埋まる");
    Check(At(closed, 0, .9f, 0) > 0 && At(closed, 0, .7f, 0) > 0, "幅より広い溝の入口は残る");
    // 埋まる境は幅 0.1 m の高さ（y ≈ 0.47）の付近。格子の精度（1セル 0.031 m）で数セルの幅を許す。
    bool boundaryNear = true;
    for (float y = .2f; y < 1; y += grooved.spacing) {
        const bool filled = At(closed, 0, y, 0) < 0;
        if (y < .47f - 3 * grooved.spacing) boundaryNear &= filled;
        if (y > .47f + 3 * grooved.spacing) boundaryNear &= !filled;
    }
    Check(boundaryNear, "埋まる境は、溝の幅が設定の幅になる深さにある");
    bool neverRemoves = true, sameInside = true, faceKept = true;
    for (size_t i = 0; i < grooved.values.size(); ++i) {
        if (grooved.values[i] < 0) {
            neverRemoves &= closed.values[i] < 0;
            sameInside &= closed.values[i] == grooved.values[i];
        }
    }
    // 溝から離れた面（-X の面）の近くの値は変わらない。
    for (float y = -.9f; y < .9f && faceKept; y += .1f)
        for (float z = -.9f; z < .9f; z += .1f)
            faceKept &= std::abs(At(closed, -1.02f, y, z) - At(grooved, -1.02f, y, z)) < 1e-5f && std::abs(At(closed, -.98f, y, z) - At(grooved, -.98f, y, z)) < 1e-5f;
    Check(neverRemoves, "内部を減らさない");
    Check(sameInside, "元から内部だった点の値は入力のまま");
    Check(faceKept, "隙間から離れた表面の値は変わらない");
    Check(info.volume > groovedInfo.volume + .01 && info.volume < groovedInfo.volume + .3 &&
              std::abs(info.maximum.x - groovedInfo.maximum.x) < 1e-4f && std::abs(info.maximum.y - groovedInfo.maximum.y) < 1e-4f,
          "埋めたぶんだけ体積が増え、外接箱は変わらない");
    Check(geometry::CloseVolume(grooved, settings, error).values == closed.values, "同じ入力から同じ結果を得る");

    // 幅を溝の入口より広くすると溝は全て埋まり、立方体に戻る（表面の半セルぶんだけは埋まらない）。
    geometry::VolumeCloseSettings wide;
    wide.mode = geometry::VolumeCloseMode::Width;
    wide.width = .2f;
    const auto restored = geometry::CloseVolume(grooved, wide, error);
    geometry::MeshInfo restoredInfo;
    Check(error.empty() && Measure(restored, restoredInfo) && At(restored, 0, .9f, 0) < 0 && At(restored, 0, .95f, 0) < 0 &&
              std::abs(restoredInfo.volume - 8) < .15,
          "溝より広い幅では溝が全て埋まり、元の立方体に戻る");
    // 溝の無い立方体は変わらない（凸な形は閉じても同じ）。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    const auto sameBox = geometry::CloseVolume(box, wide, error);
    bool unchanged = error.empty();
    for (size_t i = 0; i < box.values.size() && unchanged; ++i) unchanged &= (sameBox.values[i] < 0) == (box.values[i] < 0);
    Check(unchanged, "凸な形は内外が変わらない");
    // 1セル未満の半径では何も起きない。
    geometry::VolumeCloseSettings tiny;
    tiny.mode = geometry::VolumeCloseMode::Width;
    tiny.width = .005f;
    Check(geometry::CloseVolume(grooved, tiny, error).values == grooved.values, "半径が1セル未満なら入力のまま");

    // 埋めた結果として閉じ込められた空洞（入口が狭く奥が広い穴）も埋まる。
    auto pocket = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    for (uint32_t z = 0; z < pocket.dimensions[2]; ++z)
        for (uint32_t y = 0; y < pocket.dimensions[1]; ++y)
            for (uint32_t x = 0; x < pocket.dimensions[0]; ++x) {
                const auto p = pocket.Position(x, y, z);
                auto& value = pocket.values[pocket.Index(x, y, z)];
                // 入口：半径 0.04 の縦穴（y > 0.4）。奥：半径 0.3 の球（中心 y = 0）。
                const float shaft = (p.y > .4f && p.y <= 1.f) ? .04f - std::sqrt(p.x * p.x + p.z * p.z) : -1.f;
                const float chamber = .3f - std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
                value = std::max({value, shaft, chamber});
            }
    const auto sealed = geometry::CloseVolume(pocket, settings, error);
    geometry::MeshInfo sealedInfo;
    Check(error.empty() && At(pocket, 0, 0, 0) > 0 && At(sealed, 0, 0, 0) < 0 && At(sealed, 0, .7f, 0) < 0 && Measure(sealed, sealedInfo) &&
              sealedInfo.components == 1 && std::abs(sealedInfo.volume - 8) < .15,
          "狭い入口を埋めて閉じ込められた空洞も埋まる");

    const auto rejects = [&](const char* name, auto change) {
        geometry::VolumeCloseSettings bad;
        bad.mode = geometry::VolumeCloseMode::Width;
        change(bad);
        const auto result = geometry::CloseVolume(grooved, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejects("小さすぎる幅を拒否する", [](auto& s) { s.width = 0; });
    rejects("大きすぎる幅を拒否する", [](auto& s) { s.width = 1; });
    rejects("非有限の幅を拒否する", [](auto& s) { s.width = std::numeric_limits<float>::quiet_NaN(); });
    geometry::CloseVolume({}, {}, error);
    Check(!error.empty(), "空のボリュームを拒否する");

    Section("Volume Close（遮蔽）");
    // 既定は遮蔽モード。距離 0.3 × 2 m = 0.6 m、しきい値 0.75。
    geometry::VolumeCloseSettings occlusion;
    occlusion.distance = .3f;
    Check(geometry::VolumeCloseSettings{}.mode == geometry::VolumeCloseMode::Occlusion, "既定は遮蔽モード");
    const auto shaded = geometry::CloseVolume(grooved, occlusion, error);
    geometry::MeshInfo shadedInfo, shadedDual;
    Check(error.empty() && Measure(shaded, shadedInfo) && shadedInfo.components == 1, "遮蔽：閉じた1つの塊になる");
    Check(Measure(shaded, shadedDual, geometry::VolumeMeshingMethod::DualContouring), "遮蔽：Dual Contouring でも閉じた表面にできる");
    Check(shaded.dimensions == grooved.dimensions && shaded.spacing == grooved.spacing, "遮蔽：格子は入力のまま");
    Check(At(shaded, 0, .25f, 0) < 0 && At(shaded, 0, .4f, 0) < 0, "遮蔽：溝の奥は埋まる");
    Check(At(shaded, 0, .95f, 0) > 0 && At(shaded, 0, .85f, 0) > 0, "遮蔽：溝の入口は残る");
    bool occlusionKeepsInside = true, occlusionKeepsFace = true;
    for (size_t i = 0; i < grooved.values.size(); ++i)
        if (grooved.values[i] < 0) occlusionKeepsInside &= shaded.values[i] == grooved.values[i];
    for (float y = -.9f; y < .9f && occlusionKeepsFace; y += .1f)
        for (float z = -.9f; z < .9f; z += .1f)
            occlusionKeepsFace &= std::abs(At(shaded, -1.02f, y, z) - At(grooved, -1.02f, y, z)) < 1e-5f &&
                                  std::abs(At(shaded, -.98f, y, z) - At(grooved, -.98f, y, z)) < 1e-5f &&
                                  std::abs(At(shaded, -1.05f, y, z) - At(grooved, -1.05f, y, z)) < 1e-5f;
    Check(occlusionKeepsInside, "遮蔽：元から内部だった点の値は入力のまま");
    Check(occlusionKeepsFace, "遮蔽：平らな面の近くの値は変わらない（遮蔽率 0.5 は埋めない）");
    Check(shadedInfo.volume > groovedInfo.volume + .01 && shadedInfo.volume < groovedInfo.volume + .3 &&
              std::abs(shadedInfo.maximum.x - groovedInfo.maximum.x) < 1e-4f && std::abs(shadedInfo.maximum.y - groovedInfo.maximum.y) < 1e-4f,
          "遮蔽：埋めたぶんだけ体積が増え、外接箱は変わらない");
    Check(geometry::CloseVolume(grooved, occlusion, error).values == shaded.values, "遮蔽：同じ入力から同じ結果を得る");
    auto strict = occlusion;
    strict.threshold = .95f;
    const auto strictGrid = geometry::CloseVolume(grooved, strict, error);
    auto loose = occlusion;
    loose.threshold = .55f;
    const auto looseGrid = geometry::CloseVolume(grooved, loose, error);
    const auto filledHeight = [&](const geometry::VolumeGrid& g) {
        float top = .2f;
        for (float y = .2f; y < 1; y += g.spacing)
            if (At(g, 0, y, 0) < 0) top = y;
        return top;
    };
    Check(error.empty() && filledHeight(strictGrid) < filledHeight(shaded) && filledHeight(shaded) < filledHeight(looseGrid),
          "遮蔽：しきい値が低いほど溝の浅い所まで埋まる");
    auto few = occlusion;
    few.samples = 8;
    Check(geometry::CloseVolume(grooved, few, error).values != shaded.values && error.empty(), "遮蔽：サンプル数で結果が変わる");
    // 浅く広いくぼみ（幅 0.16 m、深さ 0.05 m）は、幅モード（幅 0.2 m）では埋まり、遮蔽モードでは残る（底でも 3 割は空に開いている）。
    auto pit = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    for (uint32_t z = 0; z < pit.dimensions[2]; ++z)
        for (uint32_t y = 0; y < pit.dimensions[1]; ++y)
            for (uint32_t x = 0; x < pit.dimensions[0]; ++x) {
                const auto p = pit.Position(x, y, z);
                if (p.y < .95f || p.y > 1.f) continue;
                auto& value = pit.values[pit.Index(x, y, z)];
                value = std::max(value, .08f - std::abs(p.x));
            }
    Check(At(pit, 0, .96f, 0) > 0, "浅いくぼみの中は外部");
    geometry::VolumeCloseSettings broad;
    broad.mode = geometry::VolumeCloseMode::Width;
    broad.width = .1f;
    const auto pitByWidth = geometry::CloseVolume(pit, broad, error);
    const auto pitByOcclusion = geometry::CloseVolume(pit, occlusion, error);
    Check(error.empty() && At(pitByWidth, 0, .96f, 0) < 0, "幅モードは幅より狭ければ浅いくぼみも埋める");
    Check(At(pitByOcclusion, 0, .96f, 0) > 0, "遮蔽モードは浅いくぼみを残す");
    // 狭い入口の奥の空洞は、遮蔽でも埋まる（レイが全て当たる）。
    const auto sealedByOcclusion = geometry::CloseVolume(pocket, occlusion, error);
    geometry::MeshInfo sealedOcclusionInfo;
    Check(error.empty() && At(sealedByOcclusion, 0, 0, 0) < 0 && At(sealedByOcclusion, 0, .7f, 0) < 0 && Measure(sealedByOcclusion, sealedOcclusionInfo) &&
              sealedOcclusionInfo.components == 1 && std::abs(sealedOcclusionInfo.volume - 8) < .15,
          "遮蔽：狭い入口とその奥の空洞も埋まる");
    const auto convex = geometry::CloseVolume(box, occlusion, error);
    bool convexUnchanged = error.empty();
    for (size_t i = 0; i < box.values.size() && convexUnchanged; ++i) convexUnchanged &= (convex.values[i] < 0) == (box.values[i] < 0);
    Check(convexUnchanged, "遮蔽：凸な形は内外が変わらない");
    const auto rejectsOcclusion = [&](const char* name, auto change) {
        geometry::VolumeCloseSettings bad;
        change(bad);
        const auto result = geometry::CloseVolume(grooved, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejectsOcclusion("遮蔽：範囲外の距離を拒否する", [](auto& s) { s.distance = 2; });
    rejectsOcclusion("遮蔽：範囲外のしきい値を拒否する", [](auto& s) { s.threshold = .2f; });
    rejectsOcclusion("遮蔽：範囲外のサンプル数を拒否する", [](auto& s) { s.samples = 4; });
    rejectsOcclusion("遮蔽：範囲外のなだらかさを拒否する", [](auto& s) { s.softness = 1; });
    rejectsOcclusion("不明なモードを拒否する", [](auto& s) { s.mode = static_cast<geometry::VolumeCloseMode>(9); });
    Check(geometry::ParseVolumeCloseMode(geometry::VolumeCloseModeName(geometry::VolumeCloseMode::Width)) == geometry::VolumeCloseMode::Width &&
              geometry::ParseVolumeCloseMode("?") == geometry::VolumeCloseMode::Occlusion,
          "モードの保存名を往復でき、不明な名前は遮蔽として読む");

    Section("Volume Close のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               node = g.CreateNode(graph::NodeKind::VolumeClose), surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(graph::FindNodeDefinitionByName("volumeClose") && g.FindNode(node)->inputs.size() == 1 &&
              g.FindNode(node)->outputs.size() == 1 && std::holds_alternative<geometry::VolumeCloseSettings>(g.FindNode(node)->settings),
          "ノードは Volume の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(node)->inputs[0].id), "Mesh 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    Check(link(shape, volume) && link(volume, node) && link(node, surface), "To Volume → Volume Close → Volume to Mesh を接続できる");
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 && geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) &&
              graphInfo.closed && graphInfo.volume > 4,
          "グラフの評価で閉じたメッシュを得る");
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && graph::EvaluateRocks(g, node, &cache).rocks[0].volume == first.rocks[0].volume, "変更がなければボリュームを再利用する");
    std::get<geometry::VolumeCloseSettings>(g.FindMutableNode(node)->settings).threshold = .6f;
    const auto changed = graph::EvaluateRocks(g, node, &cache);
    Check(changed.error.empty() && changed.rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
    std::get<geometry::VolumeCloseSettings>(g.FindMutableNode(node)->settings).mode = geometry::VolumeCloseMode::Width;
    std::get<geometry::VolumeCloseSettings>(g.FindMutableNode(node)->settings).width = 5;
    Check(!graph::EvaluateRocks(g, node, &cache).error.empty(), "不正な設定はノードで診断する");
}
