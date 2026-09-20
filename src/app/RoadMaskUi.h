#pragma once

#include "graph/NodeGraph.h"
#include "ui/UiStyle.h"
#include <algorithm>

namespace tg {
// 呼び出し側のプロパティ表へ描く。Road Maskノードとプリセットで編集項目を共有する。
inline bool DrawRoadMaskPropertyRows(graph::RoadMaskNodeSettings& roadMask) {
    bool changed = false;
    const graph::RoadMaskNodeSettings defaults;
    static const char* const kShapeLabels[] = {"轍", "端の減衰", "長さ方向ノイズ", "一様", "ワールドノイズ"};
    int shape = static_cast<int>(roadMask.shape);
    if (ui::PropertyCombo("形", &shape, kShapeLabels, IM_ARRAYSIZE(kShapeLabels), static_cast<int>(defaults.shape),
                          "轍: 車線中央 ± タイヤ間隔/2 の帯。端の減衰: 道路端で 1。長さ方向ノイズ: しきい値で切る")) {
        roadMask.shape = static_cast<graph::RoadMaskShape>(shape);
        changed = true;
    }
    switch (roadMask.shape) {
        case graph::RoadMaskShape::WheelTracks:
            changed |= ui::PropertyBool("車線に合わせる", &roadMask.tracksFromLanes, defaults.tracksFromLanes,
                                        "Road の車線数から各車線の中央に置く。路肩など車線の無い面では手入力の値を使う");
            if (!roadMask.tracksFromLanes) {
                changed |= ui::PropertyFloat("車線中央", &roadMask.laneOffsetMeters, 0.0f, 10.0f, defaults.laneOffsetMeters,
                                             "中心線から車線中央までの距離", "%.2f m");
            }
            changed |= ui::PropertyFloat("タイヤ間隔", &roadMask.trackSpacingMeters, 0.5f, 3.0f, defaults.trackSpacingMeters,
                                         "左右のタイヤの間隔", "%.2f m");
            changed |= ui::PropertyFloat("帯の幅", &roadMask.trackWidthMeters, 0.05f, 2.0f, defaults.trackWidthMeters,
                                         "轍 1 本の幅", "%.2f m");
            changed |= ui::PropertyFloat("ぼかし", &roadMask.featherMeters, 0.0f, 2.0f, defaults.featherMeters,
                                         "帯の縁を 0 へ落とす幅", "%.2f m");
            changed |= ui::PropertyBool("両車線", &roadMask.bothLanes, defaults.bothLanes,
                                        "対向車線にも置く。手入力のときは中心線の左右両方に置く");
            break;
        case graph::RoadMaskShape::EdgeFalloff: {
            static const char* const kSideLabels[] = {"両側", "左", "右"};
            int side = static_cast<int>(roadMask.edgeSide);
            if (ui::PropertyCombo("側", &side, kSideLabels, IM_ARRAYSIZE(kSideLabels), static_cast<int>(defaults.edgeSide),
                                  "どちらの端に出すか。左右は Path の進行方向基準（Road の Left / Right と同じ）。走行側には依存しない")) {
                roadMask.edgeSide = static_cast<graph::RoadMaskSide>(side);
                changed = true;
            }
            changed |= ui::PropertyFloat("端の幅", &roadMask.edgeWidthMeters, 0.0f, 5.0f, defaults.edgeWidthMeters,
                                         "道路端から内側へ 1 のまま続く幅", "%.2f m");
            changed |= ui::PropertyFloat("ぼかし", &roadMask.featherMeters, 0.0f, 5.0f, defaults.featherMeters,
                                         "その内側で 0 へ落とす幅", "%.2f m");
            break;
        }
        case graph::RoadMaskShape::LengthNoise:
        case graph::RoadMaskShape::WorldNoise:
            changed |= ui::PropertyFloat("ノイズの大きさ", &roadMask.noiseScaleMeters, 0.1f, 50.0f, defaults.noiseScaleMeters,
                                         roadMask.shape == graph::RoadMaskShape::WorldNoise
                                             ? "ノイズ 1 周期の実距離。ワールド XZ で評価するので方向性が無く、路肩や隣の道路と模様が続く"
                                             : "ノイズ 1 周期の実距離", "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("しきい値", &roadMask.threshold, 0.0f, 1.0f, defaults.threshold,
                                         "これより大きい所が 1", "%.2f");
            changed |= ui::PropertyFloat("柔らかさ", &roadMask.softness, 0.01f, 1.0f, defaults.softness,
                                         "しきい値まわりの遷移幅", "%.2f");
            break;
        default:
            break;
    }
    changed |= ui::PropertyFloat("ムラ", &roadMask.breakupAmount, 0.0f, 1.0f, defaults.breakupAmount,
                                 "長さ方向のノイズを掛ける量。0 で一様", "%.2f");
    if (roadMask.breakupAmount > 0.0f) {
        changed |= ui::PropertyFloat("ムラの大きさ", &roadMask.breakupScaleMeters, 0.1f, 50.0f, defaults.breakupScaleMeters,
                                     "ムラ 1 周期の実距離", "%.1f m", ImGuiSliderFlags_Logarithmic);
    }
    int seed = static_cast<int>(roadMask.seed);
    if (ui::PropertyInt("シード", &seed, 0, 9999, static_cast<int>(defaults.seed), "ノイズの並びを変える")) {
        roadMask.seed = static_cast<uint32_t>(std::max(0, seed));
        changed = true;
    }
    changed |= ui::PropertyFloat("強さ", &roadMask.strength, 0.0f, 1.0f, defaults.strength, "全体に掛ける倍率", "%.2f");
    changed |= ui::PropertyBool("反転", &roadMask.invert, defaults.invert, "1 − 値にする");
    return changed;
}
}  // namespace tg
