#pragma once
#include "geometry/Mesh.h"
#include <array>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace rock::geometry {
// ハイポリからローポリへの転写（High → Low のベイク）。
//
// ローポリの UV の画素ごとに、その画素に当たるローポリの表面の点から、外側（法線の向きにケージ距離だけ
// 離れた所）から内側へレイを飛ばし、最初に当たったハイポリの面を探す。当たった点のハイポリの法線を
// ローポリの接線空間で表したものと、ローポリの面からの距離（外向きが正）を画素に書く。
//
// ローポリの角の向き（frames）は描画と同じもの（renderer::MakeRockMeshData の法線・接線）を渡す。
// 接線空間は MeshPbr.hlsl の PsBake と同じ取り方（接線を法線に直交させ、従接線 = 法線 × 接線 × 符号）。
struct CornerFrame {
    Vec3 normal{};
    Vec3 tangent{};
    float sign = 1.0f;
};
inline constexpr float kMinCageDistance = 0.0001f;
inline constexpr float kMaxCageDistance = 100.0f;

struct DetailTransferImage {
    uint32_t width = 0, height = 0;
    // 画素ごと（行 0 が v = 0）。covered が 0 の画素はローポリの UV の外。hit が 0 の画素はハイポリに当たらなかった。
    std::vector<Vec3> normals;   // ローポリの接線空間の単位ベクトル（当たらなければ (0, 0, 1)）
    std::vector<float> heights;  // ローポリの面からハイポリまでの距離（m）。外向きが正
    std::vector<uint8_t> covered, hit;
    size_t CoveredCount() const;
    size_t HitCount() const;
};

// frames は面ごとの 3 つの角（low.triangles と同じ並び）。空なら面積で重み付けした頂点法線と、UV から求めた接線を使う。
// low は UV 付きの閉じたメッシュ、high は三角形を 1 枚以上持つメッシュ（UV は不要）。
bool TransferDetail(const Mesh& low, const std::vector<std::array<CornerFrame, 3>>& frames, const Mesh& high,
                    float cageDistance, uint32_t width, uint32_t height, DetailTransferImage& out, std::string& error,
                    std::stop_token stop = {}, const std::function<void(int)>& progress = {});
}  // namespace rock::geometry
