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
// 格子点の値を最も近い添字で読む。
float At(const geometry::VolumeGrid& g, geometry::Vec3 p) {
    const auto cell = [&](float v, float origin, uint32_t count) {
        return uint32_t(std::clamp(std::lround((v - origin) / g.spacing), 0l, long(count) - 1));
    };
    return g.values[g.Index(cell(p.x, g.origin.x, g.dimensions[0]), cell(p.y, g.origin.y, g.dimensions[1]),
                            cell(p.z, g.origin.z, g.dimensions[2]))];
}
}  // namespace

void RunVolumeCrackTests() {
    Section("Volume Crack");
    std::string error;
    // 2 m の立方体。2点の垂直二等分面は x = 0 の平面で、割れ目はこの面に沿う。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    Check(error.empty(), "立方体のボリューム");
    geometry::MeshInfo boxInfo;
    Check(Measure(box, boxInfo), "入力は閉じた表面を持つ");
    const std::vector<geometry::Vec3> pair{{-.5f, 0, 0}, {.5f, 0, 0}};

    // ばらつきとゆらぎを切ると、幅と深さから形が決まる。幅 0.1 × 2 m = 0.2 m、深さ 0.25 × 2 m = 0.5 m。
    geometry::VolumeCrackSettings groove;
    groove.width = .1f;
    groove.depth = .25f;
    groove.variation = 0;
    groove.noise = 0;
    const auto grooved = geometry::CrackVolume(box, pair, groove, error);
    geometry::MeshInfo groovedInfo;
    Check(error.empty() && Measure(grooved, groovedInfo) && groovedInfo.components == 1 &&
              groovedInfo.volume < boxInfo.volume - .05,
          "浅い割れ目は形を分けずに溝を彫る");
    Check(grooved.dimensions == box.dimensions && grooved.spacing == box.spacing && grooved.origin.x == box.origin.x,
          "格子は入力のまま変わらない");
    Check(At(grooved, {0, 0, .95f}) > 0 && At(grooved, {0, .95f, 0}) > 0 && At(grooved, {0, 0, -.95f}) > 0,
          "境界面の上では、どの面からも表面の近くが彫られる");
    Check(At(grooved, {0, 0, 0}) < 0 && At(grooved, {0, 0, .3f}) < 0, "指定の深さより奥は残る");
    // V 字。表面の近く（深さ 0.05 m）では半幅 0.09 m、深さ 0.4 m では半幅 0.02 m。
    Check(At(grooved, {.06f, 0, .95f}) > 0 && At(grooved, {.06f, 0, .6f}) < 0, "割れ目は深くなるほど狭まる");
    bool neverGrows = true, keepsAway = true;
    for (uint32_t z = 0; z < box.dimensions[2]; ++z)
        for (uint32_t y = 0; y < box.dimensions[1]; ++y)
            for (uint32_t x = 0; x < box.dimensions[0]; ++x) {
                const size_t i = box.Index(x, y, z);
                neverGrows &= grooved.values[i] >= box.values[i];
                // 割れ目の壁までの距離が、もとの表面までの距離より遠い点は入力と同じ値のまま。
                // 壁は境界面から最大で半幅（0.1 m）の位置にある。
                const float fromWall = std::abs(box.Position(x, y, z).x) - .1f;
                if (fromWall > std::max(-box.values[i], 0.f) + 1e-4f) keepsAway &= grooved.values[i] == box.values[i];
            }
    Check(neverGrows, "割れ目は形を広げない（どの格子点の距離も減らない）");
    Check(keepsAway, "割れ目から離れた格子点は入力の値と完全に一致する");
    // 左右対称。
    Check(std::abs(groovedInfo.minimum.x + groovedInfo.maximum.x) < box.spacing &&
              At(grooved, {.04f, 0, .95f}) > 0 && At(grooved, {-.04f, 0, .95f}) > 0,
          "境界面の両側を同じだけ彫る");

    // 深さ 1 は形を貫き、2つの塊に分ける。
    auto through = groove;
    through.depth = 1;
    const auto split = geometry::CrackVolume(box, pair, through, error);
    geometry::MeshInfo splitInfo;
    Check(error.empty() && Measure(split, splitInfo) && splitInfo.components == 2, "深さ 1 の割れ目は形を2つに分ける");

    // 幅 0 は何も彫らない。
    auto closed = groove;
    closed.width = 0;
    Check(geometry::CrackVolume(box, pair, closed, error).values == box.values && error.empty(),
          "幅 0 では入力と完全に同じ結果になる");

    // 再現性と Seed。
    const std::vector<geometry::Vec3> many{{-.6f, -.5f, .4f}, {.5f, .6f, -.3f}, {.1f, -.2f, .7f}, {-.3f, .7f, -.6f},
                                           {.7f, -.6f, -.5f}, {-.7f, .1f, -.1f}, {.2f, .3f, .1f},  {.6f, .1f, .6f}};
    geometry::VolumeCrackSettings varied;
    varied.width = .08f;
    const auto first = geometry::CrackVolume(box, many, varied, error);
    Check(error.empty() && geometry::CrackVolume(box, many, varied, error).values == first.values,
          "同じ入力と Seed から同じ結果を得る");
    auto reseeded = varied;
    reseeded.seed = 2;
    Check(geometry::CrackVolume(box, many, reseeded, error).values != first.values, "Seed を変えると幅の散らばり方が変わる");
    geometry::MeshInfo firstInfo, dualInfo;
    Check(Measure(first, firstInfo) && firstInfo.components == 1 && firstInfo.volume < boxInfo.volume,
          "8点の割れ目を彫っても1つの閉じた塊になる");
    Check(Measure(first, dualInfo, geometry::VolumeMeshingMethod::DualContouring), "Dual Contouring でも閉じた表面にできる");

    // ばらつきとゆらぎは彫る量を減らす（一部が細くなり、閉じる）。
    auto uniform = varied;
    uniform.variation = 0;
    uniform.noise = 0;
    auto narrowed = uniform;
    narrowed.variation = 1;
    auto broken = uniform;
    broken.noise = 1;
    geometry::MeshInfo uniformInfo, narrowedInfo, brokenInfo;
    Check(Measure(geometry::CrackVolume(box, many, uniform, error), uniformInfo) &&
              Measure(geometry::CrackVolume(box, many, narrowed, error), narrowedInfo) &&
              Measure(geometry::CrackVolume(box, many, broken, error), brokenInfo),
          "ばらつき・ゆらぎを変えても閉じた表面になる");
    Check(narrowedInfo.volume > uniformInfo.volume + .01 && narrowedInfo.volume < boxInfo.volume,
          "ばらつきを上げると一部の割れ目が細くなり、閉じる");
    Check(brokenInfo.volume > uniformInfo.volume + .01 && brokenInfo.volume < boxInfo.volume,
          "ゆらぎを上げると割れ目が途中で細くなり、途切れる");

    // 形の外の点でもよい。境界面が形を通れば彫られる。
    const std::vector<geometry::Vec3> outside{{-5, 0, 0}, {5, 0, 0}};
    Check(geometry::CrackVolume(box, outside, groove, error).values == grooved.values && error.empty(),
          "点が形の外にあっても、同じ境界面なら同じ割れ目になる");

    // 重なった立体から作ったボリューム。内部に残る面に沿う空洞は作らない。
    auto cross = geometry::MakeBox({3, 1, 1});
    {
        const auto bar = geometry::MakeBox({1, 3, 1});
        const auto offset = static_cast<uint32_t>(cross.positions.size());
        cross.positions.insert(cross.positions.end(), bar.positions.begin(), bar.positions.end());
        for (auto face : bar.triangles) {
            for (auto& index : face) index += offset;
            cross.triangles.push_back(face);
        }
    }
    const auto crossVolume = geometry::MeshToVolume(cross, {64}, error);
    const std::vector<geometry::Vec3> crossPoints{{-1, .1f, .2f}, {1, -.1f, -.2f}, {.1f, 1, .1f}, {-.1f, -1, -.1f},
                                                  {.2f, .1f, .3f}};
    geometry::VolumeCrackSettings crossCrack;
    crossCrack.width = .05f;
    crossCrack.depth = .05f;  // 0.15 m。太さ 1 m の腕を切り離さない浅さ。
    crossCrack.variation = 0;
    crossCrack.noise = 0;
    const auto crackedCross = geometry::CrackVolume(crossVolume, crossPoints, crossCrack, error);
    std::string surfaceError;
    const auto crossMesh = geometry::VolumeSurface(crackedCross, surfaceError);
    geometry::MeshInfo crossInfo;
    Check(error.empty() && surfaceError.empty() && geometry::InspectMesh(crossMesh, crossInfo) && crossInfo.closed &&
              crossInfo.components == 1,
          "内部に残る面のまわりに、外へつながらない空洞を作らない");

    // 不正な設定と入力。
    const auto rejects = [&](const char* name, auto change) {
        geometry::VolumeCrackSettings bad;
        change(bad);
        const auto result = geometry::CrackVolume(box, pair, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejects("負の幅を拒否する", [](auto& s) { s.width = -.1f; });
    rejects("広すぎる幅を拒否する", [](auto& s) { s.width = .5f; });
    rejects("深さ 0 を拒否する", [](auto& s) { s.depth = 0; });
    rejects("範囲外のばらつきを拒否する", [](auto& s) { s.variation = 2; });
    rejects("非有限のゆらぎを拒否する", [](auto& s) { s.noise = std::numeric_limits<float>::quiet_NaN(); });
    rejects("範囲外のゆらぎの細かさを拒否する", [](auto& s) { s.noiseScale = 100; });
    geometry::CrackVolume(box, {{0, 0, 0}}, groove, error);
    Check(!error.empty(), "点が1個なら拒否する");
    geometry::CrackVolume(box, {{0, 0, 0}, {0, 0, 0}}, groove, error);
    Check(!error.empty(), "重複した点を拒否する");
    geometry::CrackVolume(box, {{0, 0, 0}, {std::numeric_limits<float>::infinity(), 0, 0}}, groove, error);
    Check(!error.empty(), "非有限の点を拒否する");
    geometry::CrackVolume(box, std::vector<geometry::Vec3>(size_t(geometry::MaxCrackPoints) + 1), groove, error);
    Check(!error.empty(), "上限を超える点数を拒否する");
    geometry::CrackVolume({}, pair, groove, error);
    Check(!error.empty(), "空のボリュームを拒否する");

    Section("Volume Crack のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               scatter = g.CreateNode(graph::NodeKind::ScatterPoints), crack = g.CreateNode(graph::NodeKind::VolumeCrack),
               surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to, size_t pin) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[pin].id);
    };
    Check(g.FindNode(crack)->inputs.size() == 2 && g.FindNode(crack)->outputs.size() == 1 &&
              std::holds_alternative<geometry::VolumeCrackSettings>(g.FindNode(crack)->settings),
          "ノードは Volume / Points の入力と Volume 出力、既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(crack)->inputs[0].id) &&
              !g.CanCreateLink(g.FindNode(volume)->outputs[0].id, g.FindNode(crack)->inputs[1].id),
          "Mesh は Volume 入力へ、Volume は Points 入力へつなげない");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    std::get<geometry::ScatterSettings>(g.FindMutableNode(scatter)->settings).count = 10;
    Check(link(shape, volume, 0) && link(shape, scatter, 0) && link(volume, crack, 0) && link(crack, surface, 0),
          "Volume 側と下流を接続できる");
    Check(!graph::EvaluateRocks(g, surface).error.empty(), "Points が未接続なら診断する");
    Check(link(scatter, crack, 1), "Scatter Points を Points 入力へ接続できる");
    std::get<geometry::VolumeCrackSettings>(g.FindMutableNode(crack)->settings).width = .08f;
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    const auto plain = graph::EvaluateRocks(g, volume, &cache);
    geometry::MeshInfo graphInfo, plainInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 &&
              geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) && graphInfo.closed &&
              Measure(*plain.rocks[0].volume, plainInfo) && graphInfo.volume < plainInfo.volume - .01,
          "グラフの評価で割れ目の入ったメッシュを得る");
    const auto crackFirst = graph::EvaluateRocks(g, crack, &cache);
    const auto crackAgain = graph::EvaluateRocks(g, crack, &cache);
    Check(crackFirst.error.empty() && crackAgain.rocks[0].volume == crackFirst.rocks[0].volume,
          "変更がなければボリュームを再利用する");
    ++std::get<geometry::ScatterSettings>(g.FindMutableNode(scatter)->settings).seed;
    const auto rescattered = graph::EvaluateRocks(g, crack, &cache);
    const auto volumeKept = graph::EvaluateRocks(g, volume, &cache);
    Check(rescattered.error.empty() && rescattered.rocks[0].volume != crackFirst.rocks[0].volume,
          "点の変更で作り直す");
    Check(volumeKept.rocks[0].volume == plain.rocks[0].volume, "変更していない Volume 側は再利用する");
    std::get<geometry::VolumeCrackSettings>(g.FindMutableNode(crack)->settings).depth = .3f;
    const auto deeper = graph::EvaluateRocks(g, crack, &cache);
    Check(deeper.error.empty() && deeper.rocks[0].volume != rescattered.rocks[0].volume, "設定の変更で作り直す");
}
