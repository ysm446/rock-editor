#pragma once
#include <vector>
#include "fracture/PlaneSplit.h"
namespace rock::fracture {
struct SplitPlane {
    geometry::Vec3 center, normal;
    std::string key;
};
struct SplitPiece {
    geometry::Mesh mesh;
    std::string key;
};
struct SplitConnection {
    size_t negative = 0, positive = 0, plane = 0;
    double area = 0;
};
struct MultiSplitResult {
    std::vector<SplitPlane> planes;
    std::vector<SplitPiece> pieces;
    std::vector<SplitConnection> connections;
    std::string error;
};
// 最大32平面・128片・合計20万三角形。接触/非交差はその片を維持する。
// 各切断は単純断面1本に限定。失敗時は全体を破棄する。
MultiSplitResult SplitByPlanes(const geometry::Mesh& input, const std::vector<SplitPlane>& planes);
}  // namespace rock::fracture
