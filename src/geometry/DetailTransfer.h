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

// RGBA8 の画像への参照（行 0 が v = 0。1 行は width * 4 バイト）。
struct TextureView {
    uint32_t width = 0, height = 0;
    const uint8_t* pixels = nullptr;
};
// テクスチャの転写結果。images は RGBA8 で、Material Bake と同じ並び（BaseColor / Normal / RoughnessMetallicAO / Height）。
// 転写元に当たった画素だけ不透明（アルファ 255）で、それ以外はアルファ 0（エッジパディングで埋める前提）。
struct TextureTransferResult {
    uint32_t width = 0, height = 0;
    std::array<std::vector<uint8_t>, 4> images;
    std::vector<uint8_t> covered;  // ローポリの UV の中の画素に 1
    size_t hits = 0;               // 転写元に当たった画素の数
};

// UV 付きの転写元（high）のテクスチャ 4 枚を、ローポリ（low）の UV へ転写する（LOD の段ごとのテクスチャ）。
// 探し方は TransferDetail と同じ。当たった点の転写元の UV で画像を双線形に読む。法線は転写元の接線空間
// （highFrames）からワールドへ戻し、ローポリの接線空間（lowFrames）で表し直す。向きが空なら頂点法線と UV の接線を使う。
bool TransferTextures(const Mesh& low, const std::vector<std::array<CornerFrame, 3>>& lowFrames, const Mesh& high,
                      const std::vector<std::array<CornerFrame, 3>>& highFrames, const std::array<TextureView, 4>& highImages,
                      float cageDistance, uint32_t width, uint32_t height, TextureTransferResult& out, std::string& error,
                      std::stop_token stop = {}, const std::function<void(int)>& progress = {});
}  // namespace rock::geometry
