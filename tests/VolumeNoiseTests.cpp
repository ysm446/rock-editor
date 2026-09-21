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
}  // namespace

void RunVolumeNoiseTests() {
    Section("Volume Noise");
    std::string error;
    // 2 m の立方体。最長辺が 2 m なので、量 0.05 は最大 0.1 m 削る。
    const auto box = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {64}, error);
    Check(error.empty(), "立方体のボリューム");
    geometry::MeshInfo boxInfo;
    Check(Measure(box, boxInfo), "入力は閉じた表面を持つ");

    geometry::VolumeNoiseSettings off;
    off.amount = 0;
    off.warp = 0;
    const auto same = geometry::NoiseVolume(box, off, error);
    Check(error.empty() && same.values == box.values && same.dimensions == box.dimensions &&
              same.origin.x == box.origin.x,
          "量も歪みも 0 なら入力と完全に同じ結果になる");

    for (const auto type : {geometry::VolumeNoiseType::Smooth, geometry::VolumeNoiseType::Cellular,
                            geometry::VolumeNoiseType::Facet}) {
        geometry::VolumeNoiseSettings s;
        s.type = type;
        s.amount = .05f;
        const auto noisy = geometry::NoiseVolume(box, s, error);
        const std::string name = geometry::VolumeNoiseTypeName(type);
        geometry::MeshInfo info, dual;
        Check(error.empty() && Measure(noisy, info) && info.components == 1, (name + ": 閉じた1つの塊になる").c_str());
        Check(Measure(noisy, dual, geometry::VolumeMeshingMethod::DualContouring),
              (name + ": Dual Contouring でも閉じた表面にできる").c_str());
        Check(noisy.dimensions == box.dimensions && noisy.origin.x == box.origin.x && noisy.spacing == box.spacing,
              (name + ": 歪みが無ければ格子は入力のまま").c_str());
        bool neverGrows = true, bounded = true;
        for (size_t i = 0; i < box.values.size(); ++i) {
            neverGrows &= noisy.values[i] >= box.values[i];
            // 浮いた小片の除去で符号が変わる点を除き、変化は量の最大（0.1 m）まで。
            if (noisy.values[i] < 0 || box.values[i] >= 0) bounded &= noisy.values[i] - box.values[i] <= .1f + 1e-4f;
        }
        Check(neverGrows, (name + ": 削る方向にだけ効き、形を広げない").c_str());
        Check(bounded, (name + ": 削る深さは量の最大を超えない").c_str());
        // 表面積 24 m² を平均 0.05 m ほど削る。全く削らない・削りすぎのどちらでもない。
        Check(info.volume < boxInfo.volume - .2 && info.volume > boxInfo.volume - 24 * .1,
              (name + ": 削る体積は量に見合う").c_str());
        Check(geometry::NoiseVolume(box, s, error).values == noisy.values,
              (name + ": 同じ入力と Seed から同じ結果を得る").c_str());
        auto reseeded = s;
        reseeded.seed = 2;
        Check(geometry::NoiseVolume(box, reseeded, error).values != noisy.values,
              (name + ": Seed を変えると形が変わる").c_str());
    }

    // 重ねる数と細かさ。
    geometry::VolumeNoiseSettings single;
    single.amount = .05f;
    single.octaves = 1;
    auto layered = single;
    layered.octaves = 5;
    Check(geometry::NoiseVolume(box, single, error).values != geometry::NoiseVolume(box, layered, error).values,
          "重ねる数を変えると形が変わる");
    auto fine = single;
    fine.scale = 64;
    fine.amount = .2f;
    geometry::MeshInfo fineInfo;
    const auto fineVolume = geometry::NoiseVolume(box, fine, error);
    Check(error.empty() && Measure(fineVolume, fineInfo) && fineInfo.components == 1,
          "最も細かく最も深いノイズでも、浮いた小片と閉じた空洞を残さない");

    // 歪み。形を削らずにゆらすので、体積はほぼ保たれる。表面が外へも動く分だけ格子が広がる。
    geometry::VolumeNoiseSettings warped;
    warped.amount = 0;
    warped.warp = .05f;
    const auto bent = geometry::NoiseVolume(box, warped, error);
    geometry::MeshInfo bentInfo;
    Check(error.empty() && Measure(bent, bentInfo) && bentInfo.components == 1 &&
              std::abs(bentInfo.volume - boxInfo.volume) < boxInfo.volume * .08,
          "歪みは体積をほぼ保ったまま形をゆらす");
    Check(bent.dimensions[0] > box.dimensions[0] && bent.dimensions[1] > box.dimensions[1] &&
              bent.dimensions[2] > box.dimensions[2] && bent.spacing == box.spacing,
          "歪みがあると、動く量だけ格子を広げる");
    Check(bentInfo.maximum.x > boxInfo.maximum.x + .005f || bentInfo.minimum.x < boxInfo.minimum.x - .005f ||
              bentInfo.maximum.y > boxInfo.maximum.y + .005f || bentInfo.minimum.y < boxInfo.minimum.y - .005f,
          "歪みは表面を外へも動かす");
    Check(bentInfo.maximum.x < boxInfo.maximum.x + .1f * 1.75f + box.spacing &&
              bentInfo.minimum.x > boxInfo.minimum.x - .1f * 1.75f - box.spacing,
          "歪みで動く量は設定の範囲に収まる");
    Check(geometry::NoiseVolume(box, warped, error).values == bent.values, "歪みも同じ入力と Seed から同じ結果を得る");
    // 平面だった面が平面でなくなる。+X の面の近くで、同じ x の格子点の値がばらつく。
    float lowest = std::numeric_limits<float>::max(), highest = std::numeric_limits<float>::lowest();
    const uint32_t column = uint32_t(std::lround((1 - bent.origin.x) / bent.spacing));
    for (uint32_t z = bent.dimensions[2] / 3; z < bent.dimensions[2] * 2 / 3; ++z)
        for (uint32_t y = bent.dimensions[1] / 3; y < bent.dimensions[1] * 2 / 3; ++y) {
            lowest = std::min(lowest, bent.values[bent.Index(column, y, z)]);
            highest = std::max(highest, bent.values[bent.Index(column, y, z)]);
        }
    Check(highest - lowest > .02f, "歪みは平らな面を波打たせる");

    // 上限と不正な設定。
    const auto large = geometry::MeshToVolume(geometry::MakeBox({2, 2, 2}), {96}, error);
    geometry::VolumeTransformSettings turn;
    turn.rotationDegrees = {30, 40, 20};
    const auto turned = geometry::TransformVolume(large, turn, error);
    auto heavy = warped;
    heavy.warp = .2f;
    geometry::NoiseVolume(turned, heavy, error);
    Check(error.find("192") != std::string::npos, "歪みで広げた格子が上限を超える設定を診断する");
    const auto rejects = [&](const char* name, auto change) {
        geometry::VolumeNoiseSettings bad;
        change(bad);
        const auto result = geometry::NoiseVolume(box, bad, error);
        Check(!error.empty() && result.values.empty(), name);
    };
    rejects("負の量を拒否する", [](auto& s) { s.amount = -.1f; });
    rejects("大きすぎる量を拒否する", [](auto& s) { s.amount = .5f; });
    rejects("範囲外の細かさを拒否する", [](auto& s) { s.scale = 0; });
    rejects("重ねる数 0 を拒否する", [](auto& s) { s.octaves = 0; });
    rejects("重ねる数 6 を拒否する", [](auto& s) { s.octaves = 6; });
    rejects("非有限の歪みを拒否する", [](auto& s) { s.warp = std::numeric_limits<float>::quiet_NaN(); });
    rejects("範囲外の歪みの細かさを拒否する", [](auto& s) { s.warpScale = 100; });
    rejects("不明な種類を拒否する", [](auto& s) { s.type = static_cast<geometry::VolumeNoiseType>(9); });
    geometry::NoiseVolume({}, {}, error);
    Check(!error.empty(), "空のボリュームを拒否する");
    Check(geometry::ParseVolumeNoiseType(geometry::VolumeNoiseTypeName(geometry::VolumeNoiseType::Cellular)) ==
                  geometry::VolumeNoiseType::Cellular &&
              geometry::ParseVolumeNoiseType(geometry::VolumeNoiseTypeName(geometry::VolumeNoiseType::Facet)) ==
                  geometry::VolumeNoiseType::Facet &&
              geometry::ParseVolumeNoiseType("?") == geometry::VolumeNoiseType::Smooth,
          "種類の保存名を往復でき、不明な名前はなめらかとして読む");

    Section("Volume Noise のグラフ");
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), volume = g.CreateNode(graph::NodeKind::ToVolume),
               noise = g.CreateNode(graph::NodeKind::VolumeNoise), surface = g.CreateNode(graph::NodeKind::VolumeToMesh);
    const auto link = [&](graph::GraphId from, graph::GraphId to) {
        return g.CreateLink(g.FindNode(from)->outputs[0].id, g.FindNode(to)->inputs[0].id);
    };
    Check(g.FindNode(noise)->inputs.size() == 1 && g.FindNode(noise)->outputs.size() == 1 &&
              std::holds_alternative<geometry::VolumeNoiseSettings>(g.FindNode(noise)->settings),
          "ノードは Volume の入出力と既定の設定を持つ");
    Check(!g.CanCreateLink(g.FindNode(shape)->outputs[0].id, g.FindNode(noise)->inputs[0].id),
          "Mesh 出力は直接つなげない");
    Check(!graph::EvaluateRocks(g, noise).error.empty(), "未接続なら診断する");
    std::get<geometry::VolumeSettings>(g.FindMutableNode(volume)->settings).resolution = 40;
    Check(link(shape, volume) && link(volume, noise) && link(noise, surface),
          "To Volume → Volume Noise → Volume to Mesh を接続できる");
    std::get<geometry::VolumeNoiseSettings>(g.FindMutableNode(noise)->settings).warp = .03f;
    graph::RockEvaluationCache cache;
    const auto evaluated = graph::EvaluateRocks(g, surface, &cache);
    geometry::MeshInfo graphInfo;
    Check(evaluated.error.empty() && evaluated.rocks.size() == 1 &&
              geometry::InspectMesh(evaluated.rocks[0].mesh, graphInfo) && graphInfo.closed && graphInfo.volume > 4,
          "グラフの評価で閉じたメッシュを得る");
    const auto first = graph::EvaluateRocks(g, noise, &cache);
    const auto again = graph::EvaluateRocks(g, noise, &cache);
    Check(first.error.empty() && again.rocks[0].volume == first.rocks[0].volume, "変更がなければボリュームを再利用する");
    const auto upstream = graph::EvaluateRocks(g, volume, &cache);
    std::get<geometry::VolumeNoiseSettings>(g.FindMutableNode(noise)->settings).type = geometry::VolumeNoiseType::Smooth;
    const auto changed = graph::EvaluateRocks(g, noise, &cache);
    const auto upstreamAgain = graph::EvaluateRocks(g, volume, &cache);
    Check(changed.error.empty() && changed.rocks[0].volume != first.rocks[0].volume, "設定の変更で作り直す");
    Check(upstreamAgain.rocks[0].volume == upstream.rocks[0].volume, "変更していない上流は再利用する");
}
