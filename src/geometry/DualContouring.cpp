#include "geometry/DualContouring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace rock::geometry {
namespace {
using Point = std::array<double, 3>;
struct Plane { Point point, normal; };
constexpr int kEdges[12][2] = {{0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7}};
int EdgeIndex(int a, int b) {
    if (a > b) std::swap(a,b);
    for (int e = 0; e < 12; ++e) if (kEdges[e][0] == a && kEdges[e][1] == b) return e;
    return -1;
}

// 単位セル内の交点を使い、座標の大きさ・セル幅による条件数の悪化を避ける。
// 平面・稜線で不定な方向は交点の重心へ弱く拘束する。
Point FitVertex(const std::vector<Plane>& planes) {
    Point mass{};
    for (const auto& p : planes)
        for (int a = 0; a < 3; ++a) mass[a] += p.point[a] / planes.size();
    constexpr double regularization = 1e-5;
    double matrix[3][3]{}, rhs[3]{};
    for (int a = 0; a < 3; ++a) {
        matrix[a][a] = regularization;
        rhs[a] = regularization * mass[a];
    }
    for (const auto& p : planes) {
        double distance = 0;
        for (int a = 0; a < 3; ++a) distance += p.normal[a] * p.point[a];
        for (int a = 0; a < 3; ++a) {
            rhs[a] += p.normal[a] * distance;
            for (int b = 0; b < 3; ++b) matrix[a][b] += p.normal[a] * p.normal[b];
        }
    }
    Point best = mass;
    double bestError = std::numeric_limits<double>::infinity();
    // 自由/下限/上限の27通りを解き、単純なクランプによる稜線のずれを避ける。
    // 隣接セルの頂点が完全に重なって縮退しないよう、セル境界から微小量だけ離す。
    constexpr double lower = 1e-4, upper = 1 - lower;
    for (int state = 0; state < 27; ++state) {
        int code = state, freeAxes[3], count = 0;
        bool fixed[3]{};
        Point candidate{};
        for (int a = 0; a < 3; ++a) {
            const int mode = code % 3;
            code /= 3;
            fixed[a] = mode != 0;
            if (fixed[a]) candidate[a] = mode == 1 ? lower : upper;
            else freeAxes[count++] = a;
        }
        double system[3][4]{};
        for (int row = 0; row < count; ++row) {
            const int a = freeAxes[row];
            system[row][count] = rhs[a];
            for (int b = 0; b < 3; ++b)
                if (fixed[b]) system[row][count] -= matrix[a][b] * candidate[b];
            for (int col = 0; col < count; ++col) system[row][col] = matrix[a][freeAxes[col]];
        }
        bool valid = true;
        for (int col = 0; col < count; ++col) {
            int pivot = col;
            for (int row = col + 1; row < count; ++row)
                if (std::abs(system[row][col]) > std::abs(system[pivot][col])) pivot = row;
            if (std::abs(system[pivot][col]) < 1e-12) { valid = false; break; }
            for (int k = col; k <= count; ++k) std::swap(system[col][k], system[pivot][k]);
            const double divisor = system[col][col];
            for (int k = col; k <= count; ++k) system[col][k] /= divisor;
            for (int row = 0; row < count; ++row) {
                if (row == col) continue;
                const double factor = system[row][col];
                for (int k = col; k <= count; ++k) system[row][k] -= factor * system[col][k];
            }
        }
        if (!valid) continue;
        for (int row = 0; row < count; ++row) candidate[freeAxes[row]] = system[row][count];
        double residual = 0;
        for (int a = 0; a < 3; ++a) {
            valid &= candidate[a] >= lower && candidate[a] <= upper && std::isfinite(candidate[a]);
            residual += regularization * (candidate[a] - mass[a]) * (candidate[a] - mass[a]);
        }
        if (!valid) continue;
        for (const auto& p : planes) {
            double distance = 0;
            for (int a = 0; a < 3; ++a) distance += p.normal[a] * (candidate[a] - p.point[a]);
            residual += distance * distance;
        }
        if (residual < bestError) { bestError = residual; best = candidate; }
    }
    return best;
}
}

Mesh ExtractDualContour(const VolumeGrid& g, std::string& error) {
    const auto nx = g.dimensions[0], ny = g.dimensions[1], nz = g.dimensions[2];
    const auto cx = nx - 1, cy = ny - 1, cz = nz - 1;
    constexpr uint32_t absent = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> vertices(size_t(cx) * cy * cz, absent);
    std::vector<std::array<uint32_t, 12>> cellVertices;
    const auto cellIndex = [=](uint32_t x, uint32_t y, uint32_t z) { return (size_t(z) * cy + y) * cx + x; };
    Mesh mesh;
    std::vector<Plane> planes;
    planes.reserve(12);
    for (uint32_t z = 0; z < cz; ++z)
        for (uint32_t y = 0; y < cy; ++y)
            for (uint32_t x = 0; x < cx; ++x) {
                double values[8];
                int inside = 0;
                for (int c = 0; c < 8; ++c) {
                    values[c] = g.values[g.Index(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1))];
                    inside += values[c] < 0;
                }
                if (inside == 0 || inside == 8) continue;
                planes.clear();
                int planeForEdge[12];
                std::fill(std::begin(planeForEdge), std::end(planeForEdge), -1);
                for (int axis = 0; axis < 3; ++axis)
                    for (int c = 0; c < 8; ++c) {
                        if (c & (1 << axis)) continue;
                        const int other = c | (1 << axis);
                        if ((values[c] < 0) == (values[other] < 0)) continue;
                        Plane plane;
                        plane.point = {double(c & 1), double((c >> 1) & 1), double((c >> 2) & 1)};
                        plane.point[axis] = values[c] / (values[c] - values[other]);
                        // セルのトリリニア補間の解析的な勾配。元形状への隠れた依存は持たない。
                        plane.normal = {};
                        for (int a = 0; a < 3; ++a)
                            for (int corner = 0; corner < 8; ++corner) {
                                double weight = corner & (1 << a) ? 1 : -1;
                                for (int b = 0; b < 3; ++b)
                                    if (b != a) weight *= corner & (1 << b) ? plane.point[b] : 1 - plane.point[b];
                                plane.normal[a] += values[corner] * weight;
                            }
                        double length = 0;
                        for (double v : plane.normal) length += v * v;
                        length = std::sqrt(length);
                        if (length <= 0 || !std::isfinite(length)) {
                            error = "Dual Contouring の表面法線を推定できません。解像度か変換方式を変更してください";
                            return {};
                        }
                        for (double& v : plane.normal) v /= length;
                        planeForEdge[EdgeIndex(c, other)] = static_cast<int>(planes.size());
                        planes.push_back(plane);
                    }
                // セル内の別々の表面を分ける。曖昧な面は両セルで同じ双一次補間を使う。
                int parents[12];
                for (int e = 0; e < 12; ++e) parents[e] = e;
                const auto root = [&](int e) { while (parents[e] != e) e = parents[e]; return e; };
                const auto join = [&](int a, int b) { parents[root(a)] = root(b); };
                for (int axis = 0; axis < 3; ++axis) for (int side = 0; side < 2; ++side) {
                    const int u = (axis + 1) % 3, v = (axis + 2) % 3, base = side << axis;
                    const int corners[] = {base, base | (1 << u), base | (1 << u) | (1 << v), base | (1 << v)};
                    int edges[4], crossing[4], count = 0;
                    for (int c = 0; c < 4; ++c) {
                        edges[c] = EdgeIndex(corners[c], corners[(c+1)%4]);
                        if (planeForEdge[edges[c]] >= 0) crossing[count++] = edges[c];
                    }
                    if (count == 2) join(crossing[0], crossing[1]);
                    if (count == 4) {
                        // 鞍点の符号で対角の接続を選択。同値なら面中心で決める。
                        const double a = values[corners[0]], b = values[corners[1]],
                                     c = values[corners[2]], d = values[corners[3]];
                        const double denominator = a - b + c - d;
                        const double center = denominator != 0 ? (a*c-b*d) / denominator : (a+b+c+d)*.25;
                        for (int corner = 0; corner < 4; ++corner)
                            if ((values[corners[corner]] < 0) != (center < 0))
                                join(edges[(corner+3)%4], edges[corner]);
                    }
                }
                std::array<uint32_t, 12> ids;
                ids.fill(absent);
                const auto origin = g.Position(x, y, z);
                for (int e = 0; e < 12; ++e) {
                    if (planeForEdge[e] < 0 || ids[e] != absent) continue;
                    std::vector<Plane> patch;
                    for (int other = 0; other < 12; ++other)
                        if (planeForEdge[other] >= 0 && root(e) == root(other)) patch.push_back(planes[planeForEdge[other]]);
                    const auto fitted = FitVertex(patch);
                    const auto id = static_cast<uint32_t>(mesh.positions.size());
                    for (int other = 0; other < 12; ++other)
                        if (planeForEdge[other] >= 0 && root(e) == root(other)) ids[other] = id;
                    mesh.positions.push_back({float(origin.x + fitted[0] * g.spacing),
                                              float(origin.y + fitted[1] * g.spacing),
                                              float(origin.z + fitted[2] * g.spacing)});
                }
                vertices[cellIndex(x, y, z)] = static_cast<uint32_t>(cellVertices.size());
                cellVertices.push_back(ids);
            }
    // 曖昧な共有面には2本の輪郭がある。同じセル頂点の組につながっても
    // 別の辺として保持するため、輪郭ごとの面上の中間点を共有する。
    std::unordered_map<uint64_t, uint32_t> faceVertices;
    const auto faceVertex = [&](const std::array<uint32_t, 3>& a,
                                const std::array<uint32_t, 3>& b, size_t edgeStart, size_t edgeEnd) {
        int axis = 0;
        while (a[axis] == b[axis]) ++axis;
        auto base = a;
        base[axis] = std::max(a[axis], b[axis]);
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        size_t samples[4];
        Vec3 positions[4];
        double values[4];
        for (int c = 0; c < 4; ++c) {
            auto p = base;
            p[u] += c == 1 || c == 2;
            p[v] += c == 2 || c == 3;
            samples[c] = g.Index(p[0], p[1], p[2]);
            positions[c] = g.Position(p[0], p[1], p[2]);
            values[c] = g.values[samples[c]];
        }
        int current = -1;
        for (int c = 0; c < 4; ++c) {
            const int next = (c + 1) % 4;
            if ((values[c] < 0) == (values[next] < 0)) return absent;
            if (std::min(samples[c], samples[next]) == std::min(edgeStart, edgeEnd) &&
                std::max(samples[c], samples[next]) == std::max(edgeStart, edgeEnd)) current = c;
        }
        if (current < 0) return absent;
        const double denominator = values[0] - values[1] + values[2] - values[3];
        const double center = denominator != 0
            ? (values[0] * values[2] - values[1] * values[3]) / denominator
            : (values[0] + values[1] + values[2] + values[3]) * .25;
        int partner = -1;
        for (int c = 0; c < 4; ++c)
            if ((values[c] < 0) != (center < 0)) {
                const int previous = (c + 3) % 4;
                if (current == c) partner = previous;
                if (current == previous) partner = c;
            }
        const uint64_t key = uint64_t(samples[0]) * 12 + axis * 4 + std::min(current, partner);
        if (const auto found = faceVertices.find(key); found != faceVertices.end()) return found->second;
        Point midpoint{};
        for (int e : {current, partner}) {
            const int next = (e + 1) % 4;
            const double t = values[e] / (values[e] - values[next]);
            const auto p = positions[e], q = positions[next];
            midpoint[0] += (p.x + (double(q.x) - p.x) * t) * .5;
            midpoint[1] += (p.y + (double(q.y) - p.y) * t) * .5;
            midpoint[2] += (p.z + (double(q.z) - p.z) * t) * .5;
        }
        const auto id = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back({float(midpoint[0]), float(midpoint[1]), float(midpoint[2])});
        faceVertices.emplace(key, id);
        return id;
    };
    const auto quad = [&](std::array<uint32_t, 4> ids, std::array<uint32_t, 4> mids, bool forward) {
        if (std::find(ids.begin(), ids.end(), absent) != ids.end()) return false;
        if (!forward) {
            std::swap(ids[1], ids[3]);
            std::reverse(mids.begin(), mids.end());
        }
        if (std::any_of(mids.begin(), mids.end(), [&](auto id) { return id != absent; })) {
            Point center{};
            for (auto id : ids) {
                const auto p = mesh.positions[id];
                center[0] += p.x * .25; center[1] += p.y * .25; center[2] += p.z * .25;
            }
            const auto middle = static_cast<uint32_t>(mesh.positions.size());
            mesh.positions.push_back({float(center[0]), float(center[1]), float(center[2])});
            for (int c = 0; c < 4; ++c) {
                const auto a = ids[c], b = ids[(c + 1) % 4];
                if (mids[c] == absent) mesh.triangles.push_back({a, b, middle});
                else {
                    mesh.triangles.push_back({a, mids[c], middle});
                    mesh.triangles.push_back({mids[c], b, middle});
                }
            }
        } else {
            const auto distance = [&](int a, int b) {
                const auto p = mesh.positions[ids[a]], q = mesh.positions[ids[b]];
                return double(p.x - q.x) * (p.x - q.x) + double(p.y - q.y) * (p.y - q.y) +
                       double(p.z - q.z) * (p.z - q.z);
            };
            if (distance(0, 2) <= distance(1, 3)) {
                mesh.triangles.push_back({ids[0], ids[1], ids[2]});
                mesh.triangles.push_back({ids[0], ids[2], ids[3]});
            } else {
                mesh.triangles.push_back({ids[0], ids[1], ids[3]});
                mesh.triangles.push_back({ids[1], ids[2], ids[3]});
            }
        }
        return true;
    };
    // 符号が変わる格子辺を囲む4セルをつなぐ。各辺を一度だけ処理する。
    for (uint32_t z = 0; z < nz; ++z)
        for (uint32_t y = 0; y < ny; ++y)
            for (uint32_t x = 0; x < nx; ++x)
                for (int axis = 0; axis < 3; ++axis) {
                    const uint32_t p[3] = {x, y, z};
                    if (p[axis] + 1 >= g.dimensions[axis]) continue;
                    const auto value = g.values[g.Index(x, y, z)];
                    const auto next = g.values[g.Index(x + (axis == 0), y + (axis == 1), z + (axis == 2))];
                    if ((value < 0) == (next < 0)) continue;
                    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                    if (p[u] == 0 || p[v] == 0 || p[u] + 1 >= g.dimensions[u] || p[v] + 1 >= g.dimensions[v]) {
                        error = "Dual Contouring の表面が格子の外周に接しています。外側に余白が必要です";
                        return {};
                    }
                    std::array<uint32_t, 4> ids, mids;
                    std::array<std::array<uint32_t, 3>, 4> cells;
                    for (int c = 0; c < 4; ++c) {
                        uint32_t cell[3] = {x, y, z};
                        cell[u] -= c == 0 || c == 3;
                        cell[v] -= c == 0 || c == 1;
                        cells[c] = {cell[0], cell[1], cell[2]};
                        const auto cellId = vertices[cellIndex(cell[0], cell[1], cell[2])];
                        const int corner = int(x - cell[0]) | (int(y - cell[1]) << 1) | (int(z - cell[2]) << 2);
                        const int edge = EdgeIndex(corner, corner | (1 << axis));
                        ids[c] = cellId == absent ? absent : cellVertices[cellId][edge];
                    }
                    for (int c = 0; c < 4; ++c)
                        mids[c] = faceVertex(cells[c], cells[(c + 1) % 4], g.Index(x, y, z),
                                             g.Index(x + (axis == 0), y + (axis == 1), z + (axis == 2)));
                    if (!quad(ids, mids, value < 0)) {
                        error = "Dual Contouring のセル接続を構築できません";
                        return {};
                    }
                }
    return mesh;
}
}
