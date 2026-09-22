#pragma once
#include "geometry/Mesh.h"
#include <functional>
#include <stop_token>
#include <string>

namespace rock::geometry {
// Decimate。形をできるだけ保ったまま三角形を減らす（QEM による辺の縮約）。
inline constexpr int MinDecimateTriangles = 64;
inline constexpr int MaxDecimateTriangles = 500000;
// 入力の上限。Subdivide / Remesh の出力上限（400万面）と揃える。
inline constexpr size_t MaxDecimateInputTriangles = 4000000;
struct DecimateSettings {
    // 目標の三角形数。入力がこれ以下なら何もしない。64～500000。
    int targetTriangles = 10000;
    // 許す形のずれの上限。形の最長辺に対する比。0～0.1。0 なら上限なしで目標まで減らす。
    // ずれは、縮約でまとめた面からの距離の二乗を面積で重み付けして平均し、平方根を取った値。
    float maxError = .004f;
    // 稜線の保護。折れ角の大きい辺を動かしにくくする。0～10。0 で保護なし。
    float creaseWeight = 1;
    bool operator==(const DecimateSettings&) const = default;
};
// 0～100 の進み具合。ワーカースレッドから呼ばれる。
using DecimateProgress = std::function<void(int percent)>;
// 入力は閉じた向き付きの多様体メッシュ（全ての辺が2面に共有される）。出力も同じ性質を保つ。
// 連結成分の数は変わらない。UVの境界を保護し、内部を補間して引き継ぐ（展開済みメッシュにも対応）。
// 面の裏返り・非多様体化・成分の消滅を起こす縮約は行わないので、目標に届かないことがある。
Mesh DecimateMesh(const Mesh& input, const DecimateSettings& settings, std::string& error,
                  std::stop_token stop = {}, const DecimateProgress& progress = {});
}  // namespace rock::geometry
