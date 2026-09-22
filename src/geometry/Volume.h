#pragma once
#include "geometry/BoxCluster.h"
#include <stop_token>
#include <string_view>

namespace rock::geometry {
struct VolumeSettings {
    int resolution = 48;  // 母岩の最長辺のセル数。16～128。
};
enum class VolumeMeshingMethod { MarchingTetrahedra, DualContouring };
struct VolumeToMeshSettings {
    VolumeMeshingMethod method = VolumeMeshingMethod::MarchingTetrahedra;
};
struct VolumeGrid {
    Vec3 origin;
    float spacing = 0;
    std::array<uint32_t, 3> dimensions{};  // サンプル点数。外側に余白を持つ。
    std::vector<float> values;             // X が最速。負が内部。
    size_t Index(uint32_t x, uint32_t y, uint32_t z) const {
        return (size_t(z) * dimensions[1] + y) * dimensions[0] + x;
    }
    Vec3 Position(uint32_t x, uint32_t y, uint32_t z) const {
        return {origin.x + x * spacing, origin.y + y * spacing, origin.z + z * spacing};
    }
};
// Volume Transform。倍率 → 回転 → 平行移動の順に、原点まわりで動かす。
// 回転は右手系 Z → X → Y、度（Model と同じ規約）。
struct VolumeTransformSettings {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> rotationDegrees{0, 0, 0};
    float scale = 1;
};
// Volume Boolean。A を基準の格子として、B との和・交差・差を取る。
enum class VolumeBooleanOperation { Union, Intersection, Difference };
const char* VolumeBooleanOperationName(VolumeBooleanOperation operation);
// 不明な名前は Union として読む。
VolumeBooleanOperation ParseVolumeBooleanOperation(std::string_view name);
struct VolumeBooleanSettings {
    VolumeBooleanOperation operation = VolumeBooleanOperation::Union;
    float blend = 0;  // つなぎ目を丸める幅 (m)。0 で角を残す。0～10。
};
// Plane Cuts。平面の群で形を切り落とし、角張った面（ファセット）を作る。
enum class PlaneCutsDistribution { Isotropic, Directional };
// Global は形全体を半空間で切る（枚数が増えるほど凸な形に近づく）。
// Local は表面の凸な点（稜線・角）のまわりだけを切り、凹凸を残したまま欠けを作る。
// 法線は表面の外向きに近いものへ寄せ、浅い欠けにする。球の壁が形に当たる欠けは使わないので、
// 実際の枚数は設定より少なくなることがある。
enum class PlaneCutsScope { Global, Local };
const char* PlaneCutsScopeName(PlaneCutsScope scope);
// 不明な名前は Global として読む。
PlaneCutsScope ParsePlaneCutsScope(std::string_view name);
const char* PlaneCutsDistributionName(PlaneCutsDistribution distribution);
// 不明な名前は Isotropic として読む。
PlaneCutsDistribution ParsePlaneCutsDistribution(std::string_view name);
inline constexpr int MaxPlaneCuts = 256;
inline constexpr float MaxPlaneCutDepth = .45f;
struct PlaneCutsSettings {
    int count = 12;  // 平面の枚数。1～256。
    int seed = 1;
    PlaneCutsScope scope = PlaneCutsScope::Global;
    // 局所（Local）の欠けの半径。形の最長辺に対する比。0.02～1。
    float radius = .2f;
    // 切り込みの深さ。0～0.45。全体では平面の法線方向に測った形の幅に対する比、
    // 局所では欠けの半径に対する比。
    float depthMin = .05f, depthMax = .25f;
    PlaneCutsDistribution distribution = PlaneCutsDistribution::Isotropic;
    // 主方向（Directional）。向きを回した座標系の X / Y / Z 軸を、系統数だけ主方向に使う。
    int systems = 2;  // 1～3。
    std::array<float, 3> rotationDegrees{0, 0, 0};  // 右手系 Z → X → Y、度。
    float spreadDegrees = 12;                       // 主方向からの法線のばらつき。0～90。
    float blend = 0;                                // 稜線を丸める幅 (m)。0 で角を残す。0～10。
};
// 平面 dot(normal, p) = offset。normal は単位長で、切り落とす側（外）を向く。
// radius が正なら局所の欠け。center を中心とする球の中だけを切る。0 なら形全体を切る。
struct CutPlane {
    Vec3 normal;
    float offset = 0;
    Vec3 center;
    float radius = 0;
};
// 表示用の切り口の枠。平面上で、その平面の切り口（結果の表面のうち平面に乗る部分）を囲む矩形。
// 矩形の辺はワールドの上方向（Y）を基準に揃える。plane は MakeCutPlanes の返した平面の番号。
struct CutFaceFrame {
    uint32_t plane = 0;
    std::array<Vec3, 4> corners;  // 周に沿った順。
};
// Plane Cuts の評価で残す、表示用の平面と枠。切り口の残らない平面は枠を持たない。
struct PlaneCutsGuide {
    std::vector<CutPlane> planes;
    std::vector<CutFaceFrame> frames;
    float spacing = 0;  // 切り落とした格子のセル間隔。断面の判定の許容幅に使う。
};
// Volume Crack。点の群が作る Voronoi の境界面に沿って、表面から割れ目を彫る。
inline constexpr int MaxCrackPoints = 512;
struct VolumeCrackSettings {
    // 表面での割れ目の幅と、届く深さ。どちらも形の最長辺に対する比。
    // 断面は V 字で、深さに達すると幅が 0 になる。深さ 1 なら形を貫く。
    float width = .03f;  // 0～0.2。
    float depth = .15f;  // 0.01～1。
    // 割れ目ごとの幅のばらつき。0 で全て同じ幅。大きいほど細い割れ目が増え、一部は閉じる。0～1。
    float variation = .6f;
    // 割れ目に沿った幅のゆらぎ。0～1。noiseScale は最長辺あたりのノイズの山の数。0.5～16。
    float noise = .5f;
    float noiseScale = 3;
    int seed = 1;
};
// Volume Noise。表面をノイズで削り、サンプル位置をずらして直線的な面や割れ目を崩す。
enum class VolumeNoiseType { Smooth, Cellular, Facet };
const char* VolumeNoiseTypeName(VolumeNoiseType type);
// 不明な名前は Smooth として読む。
VolumeNoiseType ParseVolumeNoiseType(std::string_view name);
struct VolumeNoiseSettings {
    VolumeNoiseType type = VolumeNoiseType::Facet;
    // 削る量の最大。形の最長辺に対する比。0～0.2。削る方向にだけ効き、形は広がらない。
    float amount = .02f;
    float scale = 8;  // 最長辺あたりのノイズの山の数。0.5～64。
    int octaves = 2;  // 細かさを倍にしながら重ねる数。1～5。
    // 歪み。サンプル位置をずらす量の最大（最長辺に対する比。0～0.2）と、その細かさ（0.5～16）。
    float warp = 0;
    float warpScale = 2;
    int seed = 1;
};
VolumeGrid BoxesToVolume(const std::vector<OrientedBox>& boxes, const VolumeSettings& settings,
                         std::string& error);
// 閉じた向き付きメッシュを変換。重複成分は和集合、内向きの内殻は空洞として扱う。
VolumeGrid MeshToVolume(const Mesh& mesh, const VolumeSettings& settings, std::string& error,
                        std::stop_token stop = {});
// 格子を作り直して移動・回転・拡大する。セル間隔は倍率に比例させ、解像度を保つ。
VolumeGrid TransformVolume(const VolumeGrid& grid, const VolumeTransformSettings& settings,
                           std::string& error);
// A の格子（間隔と位相）を引き継ぐ。和は B を含む範囲まで広げ、交差は重なる範囲へ狭める。
// 差は A から B を取り除く。結果の距離は内外の符号が正しい近似値になる。
VolumeGrid CombineVolumes(const VolumeGrid& a, const VolumeGrid& b, const VolumeBooleanSettings& settings,
                          std::string& error);
// 入力の形と設定から平面の群を決める。同じ入力と Seed から同じ平面を得る。
// 局所では、使えない欠けを除いた分だけ count より少なくなることがある。
std::vector<CutPlane> MakeCutPlanes(const VolumeGrid& grid, const PlaneCutsSettings& settings,
                                    std::string& error);
// 平面の外側を切り落とす。格子（範囲・セル間隔）は入力のまま。
// usedPlanes を渡すと、切り落としに使った平面を返す。
VolumeGrid CutVolume(const VolumeGrid& grid, const PlaneCutsSettings& settings, std::string& error,
                     std::vector<CutPlane>* usedPlanes = nullptr);
// 切り落とした結果（cut）と使った平面から、表示用の枠を求める。
std::vector<CutFaceFrame> CutFaceFrames(const VolumeGrid& cut, const std::vector<CutPlane>& planes);
// 断面の色分け用。メッシュの面ごとに、その面が乗っている平面の番号（CutPlane の添字）を返す。
// 乗っていない面は -1。枠を持つ平面だけを見る。面の中心が平面から1セル以内で、面の向きが平面の
// 法線とほぼ揃い、局所なら欠けの球の中にあるものを乗っているとみなす。
std::vector<int> CutFaceAssignments(const Mesh& mesh, const PlaneCutsGuide& guide);
// 点は2～512個。形の外にあってもよい。格子（範囲・セル間隔）は入力のまま。
VolumeGrid CrackVolume(const VolumeGrid& grid, const std::vector<Vec3>& points,
                       const VolumeCrackSettings& settings, std::string& error);
// 歪みが 0 なら格子は入力のまま。歪みがあると、表面が動く量だけ外側へ広げる。
// 加工でできた浮いた小片と閉じた空洞は除く。
VolumeGrid NoiseVolume(const VolumeGrid& grid, const VolumeNoiseSettings& settings, std::string& error);
// 表示用の等値面。グリッドを残し、内部に重複面のない外皮を抽出する。
Mesh VolumeSurface(const VolumeGrid& grid, std::string& error,
                   VolumeMeshingMethod method = VolumeMeshingMethod::MarchingTetrahedra);
}  // namespace rock::geometry
