#pragma once
#include "geometry/Mesh.h"
#include <functional>
#include <stop_token>
#include <string>
namespace rock::geometry {
struct UvUnwrapSettings {
    int resolution = 1024;
    int padding = 4;
    int quality = 1;
    bool operator==(const UvUnwrapSettings &) const = default;
};
bool HasValidUvs(const Mesh &mesh);
// 旧シーンの任意解像度を、画素数を減らさず128〜4096の2のべき乗へ揃える。
int NormalizeUvResolution(int resolution);
// 展開の段階。重なりの修復で最初からやり直すと、AddMesh へ戻る。
enum class UvUnwrapStage { AddMesh, ComputeCharts, PackCharts, BuildOutput };
// 段階と、その段階の進み具合（0～100）。ワーカースレッドから呼ばれる。
using UvUnwrapProgress = std::function<void(UvUnwrapStage stage, int percent)>;
// stop が要求されると、xatlas の処理を打ち切ってエラーを返す。
Mesh UnwrapMesh(const Mesh &input, const UvUnwrapSettings &settings, std::string &error,
                std::stop_token stop = {}, const UvUnwrapProgress &progress = {});
} // namespace rock::geometry
