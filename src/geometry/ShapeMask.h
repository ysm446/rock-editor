#pragma once
#include "geometry/Mesh.h"
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
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
    ValleyCurvature = 3, // 凹の平均曲率。平面と凸部は黒。
    RidgeCurvature = 4,  // 凸の平均曲率。平面と凹部は黒。
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
    // 遮蔽では探索距離、曲率では1/mの値を0〜1へ写すスケール。
    float distance = .3f;  // 距離／曲率スケール（m）
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

// Structure Mask。岩の構造から素材の模様を作る（UV は結果の保存先）。
//   縞（Bands）: 構造面（Parallel Planes）の間の層ごとに塗るかを決める。片麻岩の縞・砂岩の色の層。形の割れ目と同じ面に揃う。
//   脈（Veins）: 3D の Voronoi の境界に沿う細い線の網。大理石・石英の脈。
enum class StructureMaskType : uint32_t { Bands = 0, Veins = 1 };
const char* StructureMaskTypeName(StructureMaskType type);
// 知らない名前は Bands。
StructureMaskType ParseStructureMaskType(std::string_view name);
struct StructureMaskSettings {
    StructureMaskType type = StructureMaskType::Bands;
    int resolution = 1024;
    float fill = .4f;       // 縞: 塗る層の割合、脈: 網目のうち残す割合（残りは途切れる）。0～1
    float softness = .15f;  // 縞: 層の境のぼかし（間隔に対する比）、脈: 線の縁のぼかし（線の幅に対する比）。0～1
    float scale = .4f;      // 脈: 網目 1 つの大きさ (m)。0.01～100
    float width = .04f;     // 脈: 線の幅（網目に対する比）。0.005～0.5
    float warp = .3f;       // ゆがみ。縞は間隔、脈は網目に対する比。0～1
    float warpScale = .6f;  // ゆがみのノイズの大きさ (m)。0.01～100
    uint32_t seed = 1;
    bool invert = false;
    bool operator==(const StructureMaskSettings&) const = default;
};
struct StructurePlanes;
// 縞は planes が要る（無ければエラー）。脈は planes を使わない。
MaskImage StructureMask(const Mesh&, const StructureMaskSettings&, const StructurePlanes* planes, std::string&,
                        std::stop_token = {}, const std::function<void(int)>& progress = {});

// メッシュの3D座標から作るムラ。UVは結果の保存先として使う。
struct NoiseMaskSettings {
    float size = .5f, contrast = .35f;
    uint32_t seed = 1;
    float detail = .5f, warp = .2f;
    int resolution = 1024;
    bool invert = false;
    bool operator==(const NoiseMaskSettings&) const = default;
};
MaskImage NoiseMask(const Mesh&, const NoiseMaskSettings&, std::string&,
                    std::stop_token = {}, const std::function<void(int)>& progress = {});

// Volume Diff Mask。後から足した所（または削った所）のマスク。最後のメッシュの表面の各点で、比べる元の
// ボリューム（足す・削る前）の距離を読む。足した所: 元の形の外にある表面（隙間を埋めた土・足した礫）。
// 削った所: 元の形の内側にある表面（割れ目・欠けた跡）。比べる元とメッシュは同じ位置にそろえる
// （Volume Clip の接地・Volume Transform で動かすなら、動かした後から比べる元を取る）。
enum class VolumeDiffMode : uint32_t { Added = 0, Removed = 1 };
const char* VolumeDiffModeName(VolumeDiffMode mode);
// 知らない名前は Added。
VolumeDiffMode ParseVolumeDiffMode(std::string_view name);
struct VolumeDiffMaskSettings {
    VolumeDiffMode mode = VolumeDiffMode::Added;
    float distance = .03f;  // これより離れた所から白くする (m)。元の形の表面の小さな揺れ（ノイズ・摩耗）を除く。0～100
    float softness = .05f;  // 黒から白へ移る幅 (m)。0.001～100
    int resolution = 1024;
    bool invert = false;    // 使う側で 1 - mask にする。画像には掛けない。
    bool operator==(const VolumeDiffMaskSettings&) const = default;
};
// Flow Mask。表面を重力で流れ下る水の筋（流れの量）のマスク。鉄錆・汚れの流れた筋、濡れ跡、流れ下る土。
// メッシュをいったん Volume にし、Volume Erode の流下と同じ筋の追跡で流れの量を積み、最後のメッシュの表面で読む。
struct FlowMaskSettings {
    int resolution = 1024;
    int volumeResolution = 96;  // 筋を追う格子の解像度（最長辺のセル数）。16～128
    float length = .5f;         // 1 つの出発点から筋を追う長さ。最長辺に対する比。0.05～1
    float width = .015f;        // 筋の幅（流れの量をぼかす σ）。最長辺に対する比。0.005～0.1
    float sharpness = 2;        // 最も流れが集まる筋を 1 として流れの量の集中乗。1～8
    bool invert = false;        // 使う側で 1 - mask にする。画像には掛けない。
    bool operator==(const FlowMaskSettings&) const = default;
};
struct VolumeGrid;
// volume を渡せばその格子で筋を追う（Volume to Mesh の前の Volume。UV 展開や Decimate を経たメッシュは格子に変換できないことがある）。
// 無ければメッシュを格子に変換する。
MaskImage FlowMask(const Mesh&, const FlowMaskSettings&, const VolumeGrid* volume, std::string&, std::stop_token = {},
                   const std::function<void(int)>& progress = {});
MaskImage VolumeDiffMask(const Mesh&, const VolumeGrid& before, const VolumeDiffMaskSettings&, std::string&,
                         std::stop_token = {}, const std::function<void(int)>& progress = {});

// 上向きの受け面・近傍の遮蔽・上方の開口から土の堆積候補を作る。
struct DepositionMaskSettings {
    float amount = 1, distance = .3f, maxSlopeDegrees = 60;
    float recessPreference = .8f;
    int resolution = 1024, samples = 32;
    bool invert = false;
    bool operator==(const DepositionMaskSettings&) const = default;
};
MaskImage DepositionMask(const Mesh&, const DepositionMaskSettings&, std::string&,
                         std::stop_token = {}, const std::function<void(int)>& progress = {});

// 2つのマスク画像の合成（Mask Combine）。保存は名前で行う（ProjectIo）。
enum class MaskCombineOperation : uint32_t {
    Multiply = 0,  // A × B。両方が白い所だけ白
    Maximum = 1,   // max(A, B)。どちらかが白ければ白（和）
    Minimum = 2,   // min(A, B)。両方が白い所だけ白（積。乗算より縁が硬い）
    Subtract = 3,  // A − B。A から B を除く（0 で止める）
    Mix = 4,       // A と B を「混合」の割合で補間
};
struct MaskCombineSettings {
    MaskCombineOperation operation = MaskCombineOperation::Multiply;
    float mix = .5f;              // 混合だけが使う。0 で A、1 で B
    float low = 0, high = 1;      // 合成した値の low を 0、high を 1 へ伸ばす
    float gamma = 1;              // 伸ばした値 ^ gamma。0.1〜10
    bool invert = false;          // 合成は安価なので、Shape Mask と違って画像に焼き込む
    bool operator==(const MaskCombineSettings&) const = default;
};
// A と B を画素ごとに合成する。invertA / invertB は入力の Shape Mask の「反転」（画像には掛かっていない）。
// 出力の一辺は大きいほうの入力に合わせ、もう一方は線形補間で読む。失敗では空の画像を返し、error に理由を入れる。
MaskImage CombineMasks(const MaskImage& a, bool invertA, const MaskImage& b, bool invertB,
                       const MaskCombineSettings& settings, std::string& error);

// 1つのマスクの加工（Mask Filter）。保存は名前で行う（ProjectIo）。
enum class MaskFilterType : uint32_t {
    Blur = 0,     // 岩の表面の上でぼかす。UVの継ぎ目をまたいでつながる
    Sharpen = 1,  // アンシャープマスク。元 + 量 × (元 − ぼかし)
    Levels = 2,   // 入力の黒・白、カーブ、出力の黒・白
    Expand = 3,   // 白い所を表面に沿って半径だけ広げる。縁は距離で 1 → 0 になだらかに落ちる（崖錐: 岩の足元の周りに小石）
};
struct MaskFilterSettings {
    MaskFilterType type = MaskFilterType::Blur;
    float radius = .03f;  // ぼかし・シャープの半径（m、表面の3D距離）。ガウスの 2σ。広げるでは広げる距離
    float amount = 1;     // シャープの強さ。0〜4
    // レベル。入力の low を 0、high を 1 へ伸ばし、^gamma を掛け、出力の low〜high へ写す。
    float inputLow = 0, inputHigh = 1, gamma = 1, outputLow = 0, outputHigh = 1;
    bool invert = false;  // Mask Combine と同じく画像に焼き込む
    bool operator==(const MaskFilterSettings&) const = default;
};
// 半径の上限は 100 m（2026-10-03 に 1 m から広げた。地形の被覆マスクを十数 m 広げるため。箱の一辺は半径に比例するので、
// 大きな半径でも計算量は箱の数で決まり、重くならない）。
inline constexpr float kMinMaskFilterRadius = .001f, kMaxMaskFilterRadius = 100.f;
// mesh は input を作ったUV付きのメッシュ。ぼかし・シャープは各画素の表面の3D位置で近い画素を重み付き平均する。
// invertInput は入力のマスクの「反転」（画像には掛かっていない分）。出力は input と同じ解像度で、
// UVの島が無い画素は最も近い島の値で埋める。失敗・取消では空の画像を返し、error に理由を入れる。
MaskImage FilterMask(const Mesh& mesh, const MaskImage& input, bool invertInput, const MaskFilterSettings& settings,
                     std::string& error, std::stop_token stop = {}, const std::function<void(int)>& progress = {});
}  // namespace rock::geometry
