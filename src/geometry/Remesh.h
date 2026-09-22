#pragma once
#include "geometry/Mesh.h"
#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>

namespace rock::geometry {
// Remesh。三角形を一様な大きさの正三角形に近い形へ作り直す（等方リメッシュ。Botsch & Kobbelt の
// 分割 → 縮約 → 反転 → 接線方向の平滑化を繰り返し、元の表面へ投影する）。
// Volume to Mesh の不揃いな三角形（Marching Tetrahedra の細長い面、Dual Contouring の大きさのばらつき）を
// 揃える。UV付きの入力は、UVの島の境界（継ぎ目）を固定して内部のUVを補間して引き継ぐ（Decimate と同じ方針）。
// UV Unwrap の後に置けるので、展開を重くせずに Displace のための密度を稼げる。
inline constexpr float kMinRemeshEdge = .001f, kMaxRemeshEdge = .2f;
inline constexpr int kMaxRemeshIterations = 20;
inline constexpr size_t kMaxRemeshTriangles = 4000000;
struct RemeshSettings {
    // 目標の辺の長さ。形の最長辺に対する比。0.001～0.2。
    float edgeLength = .02f;
    // 繰り返しの回数。1～20。多いほど揃うが時間がかかる。
    int iterations = 5;
    // 特徴辺の折れ角（度）。これより大きく折れた辺は稜線として保ち、反転せず、頂点は稜線に沿ってしか動かさない。
    // 0～180。180 で特徴なし（角も丸くなる）。
    float featureAngle = 40;
    bool operator==(const RemeshSettings&) const = default;
};
// 0～100 の進み具合。ワーカースレッドから呼ばれる。
using RemeshProgress = std::function<void(int percent)>;
// 入力は閉じた向き付きの多様体メッシュ。出力も同じ性質を保ち、連結成分の数は変わらない。
// UV付きなら、継ぎ目の頂点は動かさず縮約もしない。内部の頂点のUVは元の三角形へ投影して重心座標で読む。
// 出力の三角形数は概ね 表面積 ÷ (√3/4 × 辺の長さ²)。400万面を超える設定は診断する。
Mesh RemeshMesh(const Mesh& input, const RemeshSettings& settings, std::string& error,
                std::stop_token stop = {}, const RemeshProgress& progress = {});
}  // namespace rock::geometry
