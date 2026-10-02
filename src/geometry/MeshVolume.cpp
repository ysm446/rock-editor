#include "geometry/Volume.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <execution>
#include <limits>
#include <numeric>

namespace rock::geometry {
namespace {
struct D {
    double x = 0, y = 0, z = 0;
    double Axis(int a) const {
        return a == 0 ? x : (a == 1 ? y : z);
    }
    D operator+(D p) const {
        return {x + p.x, y + p.y, z + p.z};
    }
    D operator-(D p) const {
        return {x - p.x, y - p.y, z - p.z};
    }
    D operator*(double s) const {
        return {x * s, y * s, z * s};
    }
};
D V(Vec3 p) {
    return {p.x, p.y, p.z};
}
double Dot(D a, D b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
D Cross(D a, D b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
D Min(D a, D b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
D Max(D a, D b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
struct Triangle {
    D a, b, c, normal, lo, hi, center;
};
double SegmentDistance(D p, D a, D b) {
    auto e = b - a;
    auto q = p - (a + e * std::clamp(Dot(p - a, e) / Dot(e, e), 0.0, 1.0));
    return Dot(q, q);
}
double TriangleDistance(D p, const Triangle &t) {
    auto a = t.a - p, b = t.b - p, c = t.c - p;
    if (Dot(Cross(b - a, a * -1), t.normal) >= 0 && Dot(Cross(c - b, b * -1), t.normal) >= 0 &&
        Dot(Cross(a - c, c * -1), t.normal) >= 0) {
        double d = Dot(a, t.normal);
        return d * d / Dot(t.normal, t.normal);
    }
    return std::min(
        {SegmentDistance(p, t.a, t.b), SegmentDistance(p, t.b, t.c), SegmentDistance(p, t.c, t.a)});
}
struct Crossing {
    double x;
    int direction;
};
struct Bvh {
    struct Node {
        D lo, hi;
        size_t begin = 0, end = 0;
        int left = -1, right = -1;
    };
    std::vector<Triangle> triangles;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    int Build(size_t begin, size_t end) {
        Node node;
        node.begin = begin;
        node.end = end;
        node.lo = triangles[order[begin]].lo;
        node.hi = triangles[order[begin]].hi;
        D lo = triangles[order[begin]].center, hi = lo;
        for (size_t i = begin + 1; i < end; ++i) {
            const auto &t = triangles[order[i]];
            node.lo = Min(node.lo, t.lo);
            node.hi = Max(node.hi, t.hi);
            lo = Min(lo, t.center);
            hi = Max(hi, t.center);
        }
        int id = int(nodes.size());
        nodes.push_back(node);
        if (end - begin > 8) {
            D extent = hi - lo;
            int axis = extent.y > extent.x ? 1 : 0;
            if (extent.z > extent.Axis(axis))
                axis = 2;
            size_t mid = (begin + end) / 2;
            std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                             [&](uint32_t a, uint32_t b) {
                                 return triangles[a].center.Axis(axis) < triangles[b].center.Axis(axis);
                             });
            int left = Build(begin, mid), right = Build(mid, end);
            nodes[id].left = left;
            nodes[id].right = right;
        }
        return id;
    }
    double BoundsDistance(D p, int id) const {
        const auto &n = nodes[id];
        double total = 0;
        for (int a = 0; a < 3; ++a) {
            auto d = std::max({n.lo.Axis(a) - p.Axis(a), 0.0, p.Axis(a) - n.hi.Axis(a)});
            total += d * d;
        }
        return total;
    }
    // boundsは呼び出し側が求めたこのノードの箱までの距離。子の順序付けに使った値を再利用する。
    void Nearest(D p, int id, double bounds, double &best) const {
        const auto &n = nodes[id];
        if (bounds >= best)
            return;
        if (n.left < 0) {
            for (size_t i = n.begin; i < n.end; ++i)
                best = std::min(best, TriangleDistance(p, triangles[order[i]]));
            return;
        }
        int first = n.left, second = n.right;
        double firstBounds = BoundsDistance(p, first), secondBounds = BoundsDistance(p, second);
        if (firstBounds > secondBounds) {
            std::swap(first, second);
            std::swap(firstBounds, secondBounds);
        }
        Nearest(p, first, firstBounds, best);
        Nearest(p, second, secondBounds, best);
    }
    void Crossings(double y, double z, int id, std::vector<Crossing> &out) const {
        const auto &n = nodes[id];
        if (y < n.lo.y || y > n.hi.y || z < n.lo.z || z > n.hi.z)
            return;
        if (n.left >= 0) {
            Crossings(y, z, n.left, out);
            Crossings(y, z, n.right, out);
            return;
        }
        for (size_t i = n.begin; i < n.end; ++i) {
            const auto &t = triangles[order[i]];
            if (t.normal.x == 0)
                continue;
            D a = t.a, b = t.b, c = t.c;
            if (t.normal.x < 0)
                std::swap(b, c);
            const auto edge = [&](D u, D v) { return (u.y - y) * (v.z - z) - (u.z - z) * (v.y - y); };
            // YZ投影を半開三角形にして、共有辺や頂点の交差を二重に数えない。
            const auto inside = [&](D u, D v) {
                double e = edge(u, v);
                return e > 0 || (e == 0 && ((v.z - u.z) > 0 || ((v.z - u.z) == 0 && (v.y - u.y) < 0)));
            };
            if (!inside(a, b) || !inside(b, c) || !inside(c, a))
                continue;
            double area = std::abs(t.normal.x);
            double x = (edge(b, c) * a.x + edge(c, a) * b.x + edge(a, b) * c.x) / area;
            out.push_back({x, t.normal.x > 0 ? -1 : 1});
        }
    }
};
} // namespace
VolumeGrid MeshToVolume(const Mesh &mesh, const VolumeSettings &settings, std::string &error,
                        std::stop_token stop) {
    error.clear();
    if (settings.resolution < 16 || settings.resolution > 128) {
        error = "ボリュームの解像度は16〜128にしてください";
        return {};
    }
    if (mesh.triangles.size() > 250000 || mesh.positions.size() > 1000000) {
        error = "Mesh入力は25万三角形・100万頂点以下にしてください";
        return {};
    }
    if (stop.stop_requested()) {
        error = "評価をキャンセルしました";
        return {};
    }
    MeshInfo info;
    if (!InspectMesh(mesh, info) || !info.closed || info.volume <= 0) {
        error = "閉じた、外向きの面を持つMeshが必要です。穴・縮退面・面の向きを確認してください";
        return {};
    }
    const auto origin = V(info.minimum), extent = V(info.maximum) - origin;
    const double scale = std::max({extent.x, extent.y, extent.z});
    if (!std::isfinite(scale) || scale <= 0) {
        error = "Meshの範囲が不正です";
        return {};
    }
    VolumeGrid grid;
    grid.spacing = float(scale / settings.resolution);
    if (!std::isfinite(grid.spacing) || grid.spacing <= 0) {
        error = "セル間隔を表現できません";
        return {};
    }
    grid.origin = {info.minimum.x - grid.spacing * 2, info.minimum.y - grid.spacing * 2,
                   info.minimum.z - grid.spacing * 2};
    for (int k = 0; k < 3; ++k)
        grid.dimensions[k] = uint32_t(std::ceil(extent.Axis(k) / grid.spacing)) + 5;
    auto next = grid.Position(1, 1, 1);
    auto end = grid.Position(grid.dimensions[0] - 1, grid.dimensions[1] - 1, grid.dimensions[2] - 1);
    if (!std::isfinite(end.x) || !std::isfinite(end.y) || !std::isfinite(end.z)) {
        error = "Meshの座標が表現可能な範囲を超えています";
        return {};
    }
    if (!std::isfinite(grid.spacing) || grid.spacing <= 0 || !std::isfinite(next.x) ||
        !std::isfinite(next.y) || !std::isfinite(next.z) || next.x == grid.origin.x ||
        next.y == grid.origin.y || next.z == grid.origin.z) {
        error = "座標に対してセルが小さすぎます。原点に近づけるか寸法を調整してください";
        return {};
    }
    Bvh bvh;
    bvh.triangles.reserve(mesh.triangles.size());
    for (auto f : mesh.triangles) {
        auto a = (V(mesh.positions[f[0]]) - origin) * (1 / scale),
             b = (V(mesh.positions[f[1]]) - origin) * (1 / scale),
             c = (V(mesh.positions[f[2]]) - origin) * (1 / scale);
        bvh.triangles.push_back(
            {a, b, c, Cross(b - a, c - a), Min(a, Min(b, c)), Max(a, Max(b, c)), (a + b + c) * (1. / 3)});
    }
    bvh.order.resize(bvh.triangles.size());
    std::iota(bvh.order.begin(), bvh.order.end(), 0);
    bvh.Build(0, bvh.order.size());
    // 隠れた面: 隙間なく接触する片の共有面（両側とも内部）。巻き数の相殺で内外には効かないが、最近距離には
    // 効いてしまい、内側の値が浅くなって表面の位置が外へずれる（接触面に沿った筋）。面の表裏のすぐ近くの点が
    // どちらも内部なら隠れた面として、距離の計算から外す。一部だけ隠れた面（隣が欠けて半分露出）は残す。
    const auto merged = [](std::vector<Crossing> &crossings) {
        std::sort(crossings.begin(), crossings.end(), [](auto a, auto b) { return a.x < b.x; });
        size_t count = 0;
        for (size_t i = 0; i < crossings.size();) {
            auto value = crossings[i++];
            while (i < crossings.size() && crossings[i].x - value.x < 1e-10)
                value.direction += crossings[i++].direction;
            crossings[count++] = value;
        }
        crossings.resize(count);
    };
    std::vector<uint8_t> hidden(bvh.triangles.size(), 0);
    {
        std::vector<size_t> ids(bvh.triangles.size());
        std::iota(ids.begin(), ids.end(), size_t(0));
        std::for_each(std::execution::par, ids.begin(), ids.end(), [&](size_t i) {
            if (stop.stop_requested()) return;
            const auto &t = bvh.triangles[i];
            const double length = std::sqrt(Dot(t.normal, t.normal));
            if (!(length > 0)) return;
            const D n = t.normal * (1 / length);
            const double eps = 1e-6;
            thread_local std::vector<Crossing> crossings;
            const auto inside = [&](D p) {
                crossings.clear();
                bvh.Crossings(p.y, p.z, 0, crossings);
                merged(crossings);
                int winding = 0;
                for (const auto &c : crossings)
                    if (c.x < p.x - 1e-10) winding += c.direction;
                return winding > 0;
            };
            // 重心と、重心から各頂点へ 2/3 寄った 3 点。全部の表裏が内部のときだけ隠れた面にする。
            const D samples[4] = {t.center, t.center + (t.a - t.center) * (2. / 3), t.center + (t.b - t.center) * (2. / 3),
                                  t.center + (t.c - t.center) * (2. / 3)};
            for (const auto &q : samples)
                if (!inside(q + n * eps) || !inside(q - n * eps)) return;
            hidden[i] = 1;
        });
    }
    if (stop.stop_requested()) {
        error = "評価をキャンセルしました";
        return {};
    }
    // 最近距離は露出した面だけで測る。内外（Crossings）は全部の面で判定する。
    Bvh surface;
    for (size_t i = 0; i < bvh.triangles.size(); ++i)
        if (!hidden[i]) surface.triangles.push_back(bvh.triangles[i]);
    const bool anyHidden = surface.triangles.size() != bvh.triangles.size();
    if (anyHidden) {
        surface.order.resize(surface.triangles.size());
        std::iota(surface.order.begin(), surface.order.end(), 0);
        surface.Build(0, surface.order.size());
    }
    const Bvh &nearestBvh = anyHidden ? surface : bvh;
    grid.values.resize(size_t(grid.dimensions[0]) * grid.dimensions[1] * grid.dimensions[2]);
    // 行（Y,Z）どうしは独立なので並列に処理する。各格子点の値は行の順序に依存しない。
    // 診断は直列処理と同じく最初の行のものを返すため、失敗した行より前の行は最後まで調べる。
    enum RowError : uint8_t { RowOk, RowOrientation, RowUndetermined };
    const size_t rowCount = size_t(grid.dimensions[1]) * grid.dimensions[2];
    std::vector<uint8_t> rowErrors(rowCount, RowOk), rowInside(rowCount, 0);
    std::vector<size_t> rows(rowCount);
    std::iota(rows.begin(), rows.end(), size_t(0));
    std::atomic<size_t> firstFailure{rowCount};
    std::atomic<bool> cancelled{false};
    // 巻き数が 2 以上の所がある = 重なった立体（Random Boxes など）。内部に隠れた面が残る。
    std::atomic<bool> overlapping{false};
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](size_t rowIndex) {
        if (cancelled.load(std::memory_order_relaxed) || rowIndex > firstFailure.load(std::memory_order_relaxed))
            return;
        if (stop.stop_requested()) {
            cancelled.store(true, std::memory_order_relaxed);
            return;
        }
        const uint32_t y = uint32_t(rowIndex % grid.dimensions[1]), z = uint32_t(rowIndex / grid.dimensions[1]);
        const auto fail = [&](RowError code) {
            rowErrors[rowIndex] = code;
            size_t expected = firstFailure.load(std::memory_order_relaxed);
            while (rowIndex < expected && !firstFailure.compare_exchange_weak(expected, rowIndex)) {
            }
        };
        D row = (V(grid.Position(0, y, z)) - origin) * (1 / scale);
        thread_local std::vector<Crossing> crossings;
        crossings.clear();
        bvh.Crossings(row.y, row.z, 0, crossings);
        // 接触する片の向きが逆の断面は、数値誤差以内の交差をまとめて相殺する。
        merged(crossings);
        int total = 0;
        for (auto crossing : crossings) {
            total += crossing.direction;
            if (total < 0)
                return fail(RowOrientation);
            if (total > 1)
                overlapping.store(true, std::memory_order_relaxed);
        }
        if (total != 0)
            return fail(RowUndetermined);
        size_t event = 0;
        int winding = 0;
        bool inside = false;
        double previous = std::numeric_limits<double>::max(), previousX = 0;
        for (uint32_t x = 0; x < grid.dimensions[0]; ++x) {
            auto point = (V(grid.Position(x, y, z)) - origin) * (1 / scale);
            while (event < crossings.size() && crossings[event].x <= point.x + 1e-10)
                winding += crossings[event++].direction;
            // 最近距離は隣の点から、点どうしの距離までしか増えない。余裕を持たせた上限から始めても、
            // 真の最近三角形は必ず調べるので結果は上限なしの探索と同じになる。
            double nearest = std::numeric_limits<double>::max();
            if (x > 0) {
                const double bound = std::sqrt(previous) + (point.x - previousX);
                nearest = bound * bound * (1 + 1e-6) + 1e-300;
            }
            nearestBvh.Nearest(point, 0, nearestBvh.BoundsDistance(point, 0), nearest);
            previous = nearest;
            previousX = point.x;
            float distance = std::max(float(std::sqrt(nearest) * scale), grid.spacing * 1e-4f);
            grid.values[grid.Index(x, y, z)] = winding > 0 ? -distance : distance;
            inside |= winding > 0;
        }
        rowInside[rowIndex] = inside ? 1 : 0;
    });
    if (cancelled.load() || stop.stop_requested()) {
        error = "評価をキャンセルしました";
        return {};
    }
    if (const size_t failed = firstFailure.load(); failed < rowCount) {
        error = rowErrors[failed] == RowOrientation
                    ? "Meshの面の向きが不正です。外面を外向きに揃えてください"
                    : "Meshの内外を判定できません。面の接続と向きを確認してください";
        return {};
    }
    const bool hasInside = std::find(rowInside.begin(), rowInside.end(), uint8_t(1)) != rowInside.end();
    if (!hasInside) {
        error = "形がセルより薄いため内部を捉えられません。解像度を上げるか寸法を調整してください";
        return {};
    }
    // セルより薄い閉じた隙間を埋める。ずらした片の集合（Piece Transform の 0.01 m のずれなど）は、片の間に
    // セルより細い隙間ができ、格子ではその中の数点だけが外部になって、閉じた微小な空洞（泡）として残る。
    // 外周から届かない外部のうち、「芯」（6 近傍が全部外部の点）を持たない塊は厚みが 2 セル未満の隙間で、
    // 解像度で表せないので内部に戻す。芯を持つ大きな閉じた空洞（内向きの殻で作った洞）はそのまま残す。
    {
        const size_t nx = grid.dimensions[0], ny = grid.dimensions[1], nz = grid.dimensions[2];
        std::vector<uint8_t> reached(grid.values.size(), 0);
        std::vector<size_t> stack;
        const auto visit = [&](size_t next) {
            if (grid.values[next] < 0 || reached[next]) return;
            reached[next] = 1;
            stack.push_back(next);
        };
        const auto neighbors = [&](size_t index, auto&& fn) {
            const size_t x = index % nx, y = (index / nx) % ny, z = index / (nx * ny);
            if (x > 0) fn(index - 1);
            if (x + 1 < nx) fn(index + 1);
            if (y > 0) fn(index - nx);
            if (y + 1 < ny) fn(index + nx);
            if (z > 0) fn(index - nx * ny);
            if (z + 1 < nz) fn(index + nx * ny);
        };
        for (size_t z = 0; z < nz; ++z)
            for (size_t y = 0; y < ny; ++y)
                for (size_t x = 0; x < nx; ++x)
                    if (x == 0 || y == 0 || z == 0 || x + 1 == nx || y + 1 == ny || z + 1 == nz)
                        visit((z * ny + y) * nx + x);
        while (!stack.empty()) {
            const size_t index = stack.back();
            stack.pop_back();
            neighbors(index, visit);
        }
        std::vector<size_t> component;
        for (size_t start = 0; start < grid.values.size(); ++start) {
            if (grid.values[start] < 0 || reached[start]) continue;
            component.clear();
            component.push_back(start);
            reached[start] = 1;
            bool core = false;
            for (size_t k = 0; k < component.size(); ++k) {
                int outsideNeighbors = 0;
                neighbors(component[k], [&](size_t next) {
                    if (grid.values[next] >= 0) ++outsideNeighbors;
                    if (grid.values[next] < 0 || reached[next]) return;
                    reached[next] = 1;
                    component.push_back(next);
                });
                core |= outsideNeighbors == 6;
            }
            if (core) continue;
            for (size_t index : component) grid.values[index] = -grid.spacing * .5f;
        }
    }
    // 重なった立体は、内部の距離が隠れた面までになって浅く歪むので、表面から測り直す。
    if (overlapping.load())
        RedistanceInterior(grid, stop);
    if (stop.stop_requested()) {
        error = "評価をキャンセルしました";
        return {};
    }
    return grid;
}
} // namespace rock::geometry
