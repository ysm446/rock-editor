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
    bool operator==(const ScatterSettings &) const = default;
};
struct VoronoiSettings {
    std::array<float, 3> rotation{0, 0, 0}, stretch{1, 1, 1};
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
    int producer = 0;
    uint64_t generation = 0;
    std::vector<uint32_t> ids;
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
    int producer = 0;
    uint64_t generation = 0;
    std::vector<PieceOverride> overrides;
};
uint64_t MeshFingerprint(const Mesh &mesh);
PieceCollection MakeLayeredBoxes(const LayeredBoxesSettings&, int producer, std::string&, std::stop_token = {});
PointSet ScatterPoints(const Mesh &, const ScatterSettings &, std::string &, std::stop_token = {});
PointSet ScatterPiecePoints(const PieceCollection&, const ScatterSettings&, std::string&, std::stop_token = {});
PieceCollection FractureVoronoi(const Mesh &, const PointSet &, const VoronoiSettings &, int producer,
                                std::string &, std::stop_token = {});
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
