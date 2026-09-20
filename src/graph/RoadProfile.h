#pragma once

#include "graph/Path.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

// 道路線形の縦断曲線とバンク角。road-editor（D:/GitHub/road-editor）の
// docs/calculation/02_vertical_curve_calculation.md / 03_bank_angle_calculation.md を移植した。
//
// - 縦断: 線形を距離軸へ展開し、始点・縦断ポイント・終点をガイド点にして前後勾配を放物線でつなぐ。
//   ポイントのオフセットは「制御点から決まる高さ」への加算。ポイントが無ければ高さは変えない。
// - バンク: 自動は XZ 平面の曲率半径・設計速度・摩擦係数から求め、曲がる向きの符号を掛ける。
//   手動ポイントがある区間はポイント間で線形補間する。正で Left 側（進行方向に向かって左）が上がる。
//   右カーブでは自動で正になり、内側の Right が下がる。
namespace tg::graph {

// 距離パラメータ付きの折れ線。
struct ProfileCurve {
    std::vector<DirectX::XMFLOAT3> points;
    std::vector<float> arcLengths;  // points と同じ長さ。先頭は 0
    float TotalLength() const { return arcLengths.empty() ? 0.0f : arcLengths.back(); }
    // 距離（両端へクランプ）の位置。
    DirectX::XMFLOAT3 At(float distance) const;
};
ProfileCurve BuildProfileCurve(const std::vector<DirectX::XMFLOAT3>& points);

// 縦断曲線を適用した高さ。base の各点に対応する高さを返す（ポイントが無ければ base の y のまま）。
std::vector<float> EvaluateVerticalProfile(const PathSettings& path, const ProfileCurve& base);

// バンク角（ラジアン）。curve は縦断反映後の中心線。平滑化の設定は path から読む。
float EvaluateBankAngleRadiansRaw(const PathSettings& path, const ProfileCurve& curve, float distance);
float EvaluateBankAngleRadians(const PathSettings& path, const ProfileCurve& curve, float distance);
// 曲率半径と設計速度・摩擦係数から求める自動バンクの大きさ（符号なし）。
float ComputeAutoBankRadians(float radius, float designSpeedKmh, float friction);

// 線形上の位置とフレーム。UI のマーカーと回転ギズモに使う。
struct ProfileFrame {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 tangent{0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 right{1.0f, 0.0f, 0.0f};  // 進行方向に向かって右（水平、バンク前）
    DirectX::XMFLOAT3 up{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    float bankRadians = 0.0f;
};
// Path の唯一の開いた鎖から、縦断反映後の中心線を作る。Road と同じ標本化。
// 失敗（鎖が 1 本でない等）なら偽で、error に理由を入れる。
bool BuildPathCenterline(const PathSettings& path, ProfileCurve& outCenterline, std::string* error);
// 正規化位置 u（0〜1）のフレーム。
ProfileFrame EvaluateProfileFrame(const PathSettings& path, const ProfileCurve& centerline, float u);

// --- ポイントの編集 ---------------------------------------------------------
PathElementId AddVerticalPoint(PathSettings& path, float u);
PathElementId AddBankPoint(PathSettings& path, float u);
bool DeleteProfilePoint(PathSettings& path, PathElementId id);
PathVerticalPoint* FindVerticalPoint(PathSettings& path, PathElementId id);
PathBankPoint* FindBankPoint(PathSettings& path, PathElementId id);

}  // namespace tg::graph
