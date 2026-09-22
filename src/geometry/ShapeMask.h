#pragma once
#include "geometry/Mesh.h"
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace rock::geometry {
// 形状から作った材質マスク。入力メッシュのUVに対応する1チャンネルの画像（0〜255）。
struct MaskImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
    // UV（0〜1、端で止める）の位置を線形補間で読む。0〜1。
    float Sample(float u, float v) const;
};
// 何からマスクを作るか。保存は名前で行う（ProjectIo）。
enum class ShapeMaskType : uint32_t {
    Occlusion = 0,  // 遮蔽。白は周りを形に囲まれた所（溝・割れ目・入隅）
    Direction = 1,  // 上向き度。白は上（+Y）を向いた面、黒は下を向いた面
    Height = 2,     // 高さ。白は形の最上部、黒は最下部
};
struct ShapeMaskSettings {
    ShapeMaskType type = ShapeMaskType::Occlusion;
    int resolution = 1024;        // マスク画像の一辺（2のべき乗）
    float low = .2f, high = .8f;  // 元の値（0〜1）の low を 0、high を 1 へ伸ばす
    // 伸ばした後の中間の階調を寄せるカーブ。1 で直線、2 で中間が暗く（白が細く）、0.5 で中間が明るく（白が太く）。
    // 値 = 伸ばした値 ^ gamma。0.1〜10。
    float gamma = 1;
    bool invert = false;          // 使う側（描画・Displace・プレビュー）で 1 - mask にする。画像には掛けない。
    // 遮蔽だけが使う。
    float distance = .3f;  // 遮蔽物を探す距離（m）
    int samples = 32;      // 画素ごとのレイの数
    bool operator==(const ShapeMaskSettings&) const = default;
};
inline constexpr int kMinOcclusionSamples = 8, kMaxOcclusionSamples = 128;
inline constexpr int kMinShapeMaskResolution = 128, kMaxShapeMaskResolution = 4096;
// UV付きのメッシュの各画素について、その表面の点の値を求める。
//   遮蔽    : 面の向きを中心とするコサイン重みの半球へレイを飛ばし、距離以内で形に当たった割合
//   上向き度: (面の法線・+Y + 1) / 2
//   高さ    : 形の外接箱の中での Y の位置
// UVの島が無い画素は、最も近い島の値で全面を埋める（縮小表示や線形補間で縁がにじまない）。
// 失敗・取消では空の画像を返し、error に理由を入れる。
MaskImage ShapeMask(const Mesh& mesh, const ShapeMaskSettings& settings, std::string& error,
                    std::stop_token stop = {}, const std::function<void(int)>& progress = {});
}  // namespace rock::geometry
