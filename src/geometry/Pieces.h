#pragma once
#include "geometry/Mesh.h"
#include <memory>
#include <string>
#include <stop_token>

namespace rock::geometry {
// 1回の分割の点数と、分割結果の片数の上限。
inline constexpr int MaxScatterPoints = 1024;
// Pieces を再分割したときの子のIDの間隔（子ID = (親ID + 1) × 間隔 + 子の番号）。
// 保存済みの手動選択と個別変換がIDで片を指すので、**この値は変えない。**
// 1片あたりの点数もこの値までに制限する（超えると隣の親のIDと重なる）。
inline constexpr int PieceIdStride = 512;
struct ScatterSettings {
    int count = 24;
    uint32_t seed = 1;
    int version = 1;
    bool planar = false; // ローカルXZ面内に配置。Yは形の中央に固定する。
    // 密度のむら（0～1）。大きなノイズで点を間引き、点の密な所（割れの多い帯・小さな片）と疎な所（割れの少ない芯・
    // 大きな片）を作る。clusterScale は形の最長辺あたりのむらの数（0.5～16）。0 なら従来と同じ配置。
    float clustering = 0;
    float clusterScale = 2;
    // 高さの勾配（-1～1）。正で上（ローカル +Y）ほど点を密に、負で下ほど密にする。地表に近いほど節理が密になる
    // （深い所ほど割れの間隔が広い）ことを表す。受け入れる確率を、高さ t（0 が底、1 が頂）で 1 − g × (1 − t)
    // （負なら 1 − |g| × t）にする。0 なら従来と同じ配置。
    float heightGradient = 0;
    bool operator==(const ScatterSettings &) const = default;
};
struct VoronoiSettings {
    std::array<float, 3> rotation{0, 0, 0}, stretch{1, 1, 1};
    // 節理面への吸着。Points と Planes を両方繋いだとき、最初の系統の構造面で形を板に分け、板の中を点の Voronoi で
    // 割る（板 ∩ Voronoi）。節理面は通り抜ける一枚の面になり、板の中は不規則な多面体（点の Voronoi の不規則さと、
    // 複数の片にまたがる平らな節理面の両立）。点の無い板はできない（隣の板に併せる）。
    bool snap = false;
    // Planes のみ。0 は全系統を貫通。1 以上は第1系統の板をこの枚数ずつ束ね、
    // 第2系統以降の位置と間隔の乱数を束ごとに変える。向きは系統内で共通。
    int jointSpan = 0;
    int version = 1;
    bool operator==(const VoronoiSettings &) const = default;
};
struct PointSet {
    uint64_t source = 0, fingerprint = 0;
    std::vector<Vec3> positions;
    struct Group {
        uint32_t pieceId = 0;
        uint64_t source = 0, fingerprint = 0;
        std::vector<Vec3> positions; // 親ピースのローカル座標。
    };
    bool grouped = false;
    std::vector<Group> groups;
};
struct LayeredBoxesSettings {
    int count = 5;
    std::array<float, 3> size{3, .12f, 2.4f}, rotation{0, 0, 0}, position{0, 0, 0};
    float gap = .005f, thicknessVariation = .3f, sizeVariation = .1f, offset = .12f;
    uint32_t seed = 1;
};
// 面積ベクトルはローカル座標の「単位法線 × 面積」。変換後の面積も求められる。
struct PieceContact {
    uint32_t neighbor = 0;
    std::array<double,3> areaVector{};
};
struct PieceLayerContact {
    uint32_t neighbor = 0;
    double area = 0;
    int side = 0; // 0:下面、1:上面。
};
struct PieceNeighborhood {
    std::vector<PieceContact> contacts;
    std::vector<PieceLayerContact> vertical;
    std::array<double,2> capAreas{};
    bool fixedLayerSupport = false;
    std::array<double,12> supportTransform{};
    std::vector<std::array<double,3>> boundary; // 層付きなら元の側縁だけ（上下面を除く）。
};
struct Piece {
    uint32_t id = 0;
    std::shared_ptr<const PieceNeighborhood> neighborhood;
    std::shared_ptr<const Mesh> mesh;
    // 行優先の3x4アフィン変換。メッシュを複製せず配置を変更する。
    std::array<double, 12> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Vec3 centroid;
    double volume = 0;
    uint32_t outerFaces = 0;
    int layer = -1, parentProducer = 0;
    uint32_t parentId = 0;
    // 元の板のローカル寸法。再分割しても側縁と上下面を区別する。
    std::array<float, 3> layerSize{};
    bool layerRim = false;
    // 0:切断面、1:元の外面。三角形ごとに保持する。
    std::shared_ptr<const std::vector<uint8_t>> faceOrigins;
};
struct PieceCollection {
    int producer = 0;
    uint64_t generation = 0, fingerprint = 0;
    std::vector<Piece> pieces;
    bool adjacencyComplete = false;
};
enum class PieceSelectMode {
    Manual,
    Outer,
    Region,
    Volume,
    Random,
    Rim,
    Peel
};
struct PieceSelectSettings {
    PieceSelectMode mode = PieceSelectMode::Outer;
    uint32_t outerFaces = 63, seed = 1;
    std::array<float, 3> minimum{-1, -1, -1}, maximum{1, 1, 1};
    float minVolume = 0, maxVolume = 1000000, fraction = .5f;
    bool invert = false;
    int layer = -1; // -1は全層。指定時は反転もこの層の中だけで行う。
    int rimLayers = 0, rimSide = 0; // 0層は全層。側: 0両側、1上側、2下側。
    float rimFalloff = 0; // 内側の選択率を下げる強さ。
    float peelNoise = .15f;
    bool protectCore = true;
    // peel 用。大きさの効き（0～1）。小さな片ほど先に欠ける（崩れやすい）。大きな片の芯が残る。
    float peelSize = 0;
    // peel 用。下向きの外面（地面に埋まった側）は露出ではなく支持として数える。地面から生えた岩（岩峰・露頭・崖）が
    // 上と横から欠け、根元が最後まで残る。
    bool grounded = false;
    // peel・接地用。安定（0～1）。下で支える片に載る面積が片の大きさに足りないと転げ落ちる（頭でっかちの片が残らない）。
    float stability = 0;
    // peel 用。側面の後退（m、0 で無効）。元の側面から横へ削れる深さを「後退 × 高さ（0 が底、1 が頂）」までにする。
    // 周りの地面が下がるにつれ上ほど早く地表に出て長く側面から削られたことを表し、上ほど細くなる。芯は削り切らない。
    float peelRetreat = 0;
    // peel 用。底の後退（0～1）。側面の後退の、底での割合。0 で底は削れず（従来）、1 で底も頂と同じ深さまで削れる。
    // 元の岩体の底の外周（箱の角）が裾に残るのを防ぐ。
    float peelRetreatBase = 0;
    // peel 用。稜の効き（0～1）。元の外面が 2 方向以上を向く片（稜・角・張り出した縁）ほど先に欠け、一つの平面しか
    // 向かない片（面の中央）は後回しにする。自然の岩は面の中央から穴が開くのではなく、縁から欠ける。
    float peelEdge = 0;
    // peel・接地用。支えの角度（度、0～90、既定 60）。下の片との接触面の法線が真下からこの角度以内なら「載っている」と
    // 数える。急な節理（76° など）で割った岩では、板の中の片は横に近い面でしか接しないので、広げないと落ちてしまう。
    float supportAngle = 60;
    int producer = 0;
    uint64_t generation = 0;
    std::vector<uint32_t> ids;
    bool operator==(const PieceSelectSettings&) const = default;
};
struct PieceSelection {
    int producer = 0;
    uint64_t generation = 0, input = 0;
    std::vector<uint32_t> ids;
    std::vector<uint32_t> frontier; // Peel削除後の次の候補。表示用。
};
struct PieceFilterSettings {
    bool keep = false;
};
struct PiecePose {
    std::array<float, 3> position{0, 0, 0}, rotation{0, 0, 0}, scale{1, 1, 1};
};
struct PieceOverride {
    uint32_t id = 0;
    PiecePose pose;
};
struct PieceTransformSettings {
    PiecePose pose;
    bool individual = false;
    // 片ごとのばらつき。各片を自分の重心を中心に、乱数で動かして（各軸 ±jitterPosition m）回す（各軸 ±jitterRotation 度）。
    // 節理で割れた岩のブロックのずれ・傾き（クリープ・転倒）、崩れた岩屑。乱数は片の ID と jitterSeed で決まる。
    std::array<float, 3> jitterPosition{0, 0, 0};
    float jitterRotation = 0;
    uint32_t jitterSeed = 1;
    int producer = 0;
    uint64_t generation = 0;
    std::vector<PieceOverride> overrides;
};
uint64_t MeshFingerprint(const Mesh &mesh);
PieceCollection MakeLayeredBoxes(const LayeredBoxesSettings&, int producer, std::string&, std::stop_token = {});
PointSet ScatterPoints(const Mesh &, const ScatterSettings &, std::string &, std::stop_token = {});
PointSet ScatterPiecePoints(const PieceCollection&, const ScatterSettings&, std::string&, std::stop_token = {});
struct StructurePlanes;
// snap に Parallel Planes の系統を渡し settings.snap が真なら、最初の系統の板 ∩ 点の Voronoi で割る（無ければ従来の Voronoi）。
PieceCollection FractureVoronoi(const Mesh &, const PointSet &, const VoronoiSettings &, int producer,
                                std::string &, std::stop_token = {}, const std::vector<StructurePlanes> *snap = nullptr);
// 構造面（Parallel Planes を連結した系統）で凸な Mesh を割る。全系統の板の重なりが 1 ピース（節理で区切られた
// ブロック）。隣接情報は 1 回の分割で揃うので、Piece Select の外周からの侵食（Peel）に使える。
PieceCollection FracturePlanes(const Mesh &, const std::vector<StructurePlanes> &, int producer, std::string &,
                               std::stop_token = {});
PieceCollection FractureJointGroups(const Mesh &, const std::vector<StructurePlanes> &, int jointSpan,
                                   int producer, std::string &, std::stop_token = {});
PieceCollection FracturePieces(const PieceCollection&, const PointSet&, const VoronoiSettings&, int producer,
                              std::string&, std::stop_token = {});
PieceSelection SelectPieces(const PieceCollection &, const PieceSelectSettings &, std::string &, std::stop_token = {});
bool BuildPieceLayerSupport(PieceCollection&, std::string&, std::stop_token = {});
double PieceFaceArea(const Piece&, const std::array<double,3>& areaVector);
PieceSelection PeelPieces(const PieceCollection&, const PieceSelectSettings&, const std::vector<float>& layerWeights,
                          std::string&, std::stop_token = {});
PieceCollection FilterPieces(const PieceCollection &, const PieceSelection &, bool keep, std::string &);
PieceCollection TransformPieces(const PieceCollection &, const PieceSelection *,
                                const PieceTransformSettings &, std::string &);
Mesh PieceMesh(const Piece &);
// 表示用の稜線（変換を適用した位置）。切断面を囲む辺と、元の外面のはっきり折れた辺（20度より大きい）を返す。
// 切断面の三角形分割の対角線と、元の外面のなめらかな部分の辺は含まない。
// faceOrigins が無ければ、すべて切断面として扱う。
std::vector<std::array<Vec3, 2>> PieceEdges(const Piece &);
Mesh PiecesMesh(const PieceCollection &);
Vec3 PieceCenter(const Piece &);
void RefreshPieceFingerprint(PieceCollection &);
} // namespace rock::geometry
