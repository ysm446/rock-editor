#pragma once
#include "crack/PartialCut.h"
namespace rock::crack {
// U全幅を覆い、+V側の外部から入る直線状の有限深さ溝。
// 各中間切断は単純断面1本。断面の Bridge 計測はまだ行わない。
PartialCutResult CutMesh(const geometry::Mesh& input, const CrackSettings& settings);
}  // namespace rock::crack
