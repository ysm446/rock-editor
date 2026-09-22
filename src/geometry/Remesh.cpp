#include "geometry/Remesh.h"
#include "geometry/Displace.h"
#include "geometry/UvUnwrap.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_set>
#include <vector>

namespace rock::geometry {
namespace {
using P = std::array<double, 3>;
P Sub(const P& a, const P& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
P Add(const P& a, const P& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
P Scale(const P& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
P Cross(const P& a, const P& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Dot(const P& a, const P& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double Length(const P& a) { return std::sqrt(Dot(a, a)); }
P Unit(const P& a) {
    const double l = Length(a);
    return l > 0 ? Scale(a, 1 / l) : P{0, 0, 0};
}
// 三角形の形の良さ。面積の2倍 ÷ 最長辺の二乗。正三角形で約0.87。
double Quality(const P& a, const P& b, const P& c) {
    const double twiceArea = Length(Cross(Sub(b, a), Sub(c, a)));
    const double longest = std::max({Dot(Sub(b, a), Sub(b, a)), Dot(Sub(c, b), Sub(c, b)), Dot(Sub(a, c), Sub(a, c))});
    return longest > 0 ? twiceArea / longest : 0.0;
}
constexpr double kMinQuality = .05;

// 点から三角形への最近点（Ericson, Real-Time Collision Detection）。
P ClosestPointOnTriangle(const P& p, const P& a, const P& b, const P& c) {
    const P ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
    const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const P bp = Sub(p, b);
    const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return Add(a, Scale(ab, d1 / (d1 - d3)));
    const P cp = Sub(p, c);
    const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return Add(a, Scale(ac, d2 / (d2 - d6)));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return Add(b, Scale(Sub(c, b), w));
    }
    const double denominator = 1 / (va + vb + vc), v = vb * denominator, w = vc * denominator;
    return Add(a, Add(Scale(ab, v), Scale(ac, w)));
}

// 元の表面への投影。三角形を一様格子に登録し、近いセルから順に探す。
struct SurfaceGrid {
    std::vector<P> corners;  // 3つずつ
    P origin{};
    double cell = 1;
    std::array<int, 3> dims{1, 1, 1};
    std::vector<uint32_t> cellStart, cellItems;
    mutable std::vector<uint32_t> stamp;
    mutable uint32_t query = 0;

    void Build(const Mesh& mesh, double cellSize) {
        corners.resize(mesh.triangles.size() * 3);
        P low{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
        P high{-low[0], -low[1], -low[2]};
        for (size_t f = 0; f < mesh.triangles.size(); ++f)
            for (int k = 0; k < 3; ++k) {
                const auto& v = mesh.positions[mesh.triangles[f][k]];
                corners[f * 3 + k] = {v.x, v.y, v.z};
                for (int i = 0; i < 3; ++i) {
                    low[i] = std::min(low[i], corners[f * 3 + k][i]);
                    high[i] = std::max(high[i], corners[f * 3 + k][i]);
                }
            }
        cell = cellSize;
        for (int i = 0; i < 3; ++i) {
            origin[i] = low[i] - cell;
            dims[i] = std::clamp(int(std::ceil((high[i] - low[i]) / cell)) + 3, 1, 256);
        }
        // セル数が上限で切れたら、セルを大きくして全体を覆う。
        for (int i = 0; i < 3; ++i) cell = std::max(cell, (high[i] - low[i] + 2) / dims[i] + 1e-9);
        const size_t cells = size_t(dims[0]) * dims[1] * dims[2];
        std::vector<uint32_t> counts(cells + 1, 0);
        const auto range = [&](size_t f, std::array<int, 3>& lo, std::array<int, 3>& hi) {
            for (int i = 0; i < 3; ++i) {
                double a = corners[f * 3][i], b = a;
                for (int k = 1; k < 3; ++k) { a = std::min(a, corners[f * 3 + k][i]); b = std::max(b, corners[f * 3 + k][i]); }
                lo[i] = std::clamp(int((a - origin[i]) / cell), 0, dims[i] - 1);
                hi[i] = std::clamp(int((b - origin[i]) / cell), 0, dims[i] - 1);
            }
        };
        for (size_t f = 0; f < mesh.triangles.size(); ++f) {
            std::array<int, 3> lo, hi;
            range(f, lo, hi);
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int x = lo[0]; x <= hi[0]; ++x) ++counts[Index(x, y, z) + 1];
        }
        for (size_t i = 1; i <= cells; ++i) counts[i] += counts[i - 1];
        cellStart = counts;
        cellItems.resize(counts[cells]);
        std::vector<uint32_t> fill(counts.begin(), counts.end() - 1);
        for (size_t f = 0; f < mesh.triangles.size(); ++f) {
            std::array<int, 3> lo, hi;
            range(f, lo, hi);
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int x = lo[0]; x <= hi[0]; ++x) cellItems[fill[Index(x, y, z)]++] = uint32_t(f);
        }
        stamp.assign(mesh.triangles.size(), 0);
    }
    size_t Index(int x, int y, int z) const { return (size_t(z) * dims[1] + y) * dims[0] + x; }
    P Closest(const P& p, uint32_t* hit = nullptr) const {
        ++query;
        uint32_t bestFace = 0;
        std::array<int, 3> c;
        for (int i = 0; i < 3; ++i) c[i] = std::clamp(int(std::floor((p[i] - origin[i]) / cell)), 0, dims[i] - 1);
        P best = p;
        double bestDistance = std::numeric_limits<double>::max();
        const int maxRing = std::max({dims[0], dims[1], dims[2]});
        for (int ring = 0; ring <= maxRing; ++ring) {
            // このリングより外の三角形は、少なくとも (ring − 1) セルぶん離れている。
            if (ring > 0 && bestDistance <= double(ring - 1) * cell) break;
            for (int z = c[2] - ring; z <= c[2] + ring; ++z) {
                if (z < 0 || z >= dims[2]) continue;
                for (int y = c[1] - ring; y <= c[1] + ring; ++y) {
                    if (y < 0 || y >= dims[1]) continue;
                    for (int x = c[0] - ring; x <= c[0] + ring; ++x) {
                        if (x < 0 || x >= dims[0]) continue;
                        if (std::max({std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2])}) != ring) continue;
                        const size_t index = Index(x, y, z);
                        for (uint32_t i = cellStart[index]; i < cellStart[index + 1]; ++i) {
                            const uint32_t f = cellItems[i];
                            if (stamp[f] == query) continue;
                            stamp[f] = query;
                            const P q = ClosestPointOnTriangle(p, corners[f * 3], corners[f * 3 + 1], corners[f * 3 + 2]);
                            const double distance = Length(Sub(q, p));
                            if (distance < bestDistance) { bestDistance = distance; best = q; bestFace = f; }
                        }
                    }
                }
            }
        }
        if (hit) *hit = bestFace;
        return best;
    }
};
// 三角形 (a, b, c) の中の点 q の重心座標。
std::array<double, 3> Barycentric(const P& q, const P& a, const P& b, const P& c) {
    const P v0 = Sub(b, a), v1 = Sub(c, a), v2 = Sub(q, a);
    const double d00 = Dot(v0, v0), d01 = Dot(v0, v1), d11 = Dot(v1, v1), d20 = Dot(v2, v0), d21 = Dot(v2, v1);
    const double denominator = d00 * d11 - d01 * d01;
    if (!(std::abs(denominator) > 1e-30)) return {1, 0, 0};
    const double v = (d11 * d20 - d01 * d21) / denominator, w = (d00 * d21 - d01 * d20) / denominator;
    return {std::clamp(1 - v - w, 0.0, 1.0), std::clamp(v, 0.0, 1.0), std::clamp(w, 0.0, 1.0)};
}
double UvArea(const std::array<Mesh::Uv, 3>& uv) {
    return double(uv[1].u - uv[0].u) * (uv[2].v - uv[0].v) - double(uv[1].v - uv[0].v) * (uv[2].u - uv[0].u);
}

struct Remesher {
    struct Vertex {
        P p{};
        std::vector<uint32_t> faces;
        bool alive = true;
        // UV付きのとき。継ぎ目（UVの島の境界）の頂点は動かさず、縮約しない。chart は継ぎ目でない頂点の島。
        bool seam = false;
        uint32_t chart = 0;
    };
    struct Face {
        std::array<uint32_t, 3> v{};
        bool alive = true;
        std::array<Mesh::Uv, 3> uv{};
        uint32_t chart = 0;
    };
    bool hasUvs = false;
    std::vector<Vertex> vertices;
    std::vector<Face> faces;
    size_t aliveFaces = 0;
    double target = 0;
    double cosFeature = -2;  // 特徴なし
    bool useFeatures = false;
    // 特徴辺は最初に決めて、分割・縮約で引き継ぐ。格子から作ったメッシュは面ごとの折れ角がばらつくので、
    // 毎回の折れ角で判定すると稜線でない所まで特徴になり、頂点が動けずに細長い面が残る。
    std::unordered_set<uint64_t> featureEdges;
    std::vector<uint32_t> scratchA, scratchB;
    static uint64_t Key(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b)); }

    P Normal(const Face& f) const {
        return Cross(Sub(vertices[f.v[1]].p, vertices[f.v[0]].p), Sub(vertices[f.v[2]].p, vertices[f.v[0]].p));
    }
    static bool Has(const Face& f, uint32_t v) { return f.v[0] == v || f.v[1] == v || f.v[2] == v; }
    // 辺 (a, b) を共有する2面。閉じた多様体なら必ず2面ある。無ければ false。
    bool EdgeFaces(uint32_t a, uint32_t b, uint32_t& f0, uint32_t& f1) const {
        int found = 0;
        for (const uint32_t f : vertices[a].faces)
            if (Has(faces[f], b)) {
                if (found == 0) f0 = f; else if (found == 1) f1 = f; else return false;
                ++found;
            }
        return found == 2;
    }
    // 折れ角による判定。最初に特徴辺を決めるときだけ使う。
    bool DihedralFeature(uint32_t f0, uint32_t f1) const {
        if (!useFeatures) return false;
        const P m0 = Normal(faces[f0]), m1 = Normal(faces[f1]);
        // 面積のほぼ無い面や細長い面は向きが決まらないので、特徴として扱わない。
        if (!(Length(m0) > 1e-14) || !(Length(m1) > 1e-14)) return false;
        for (const uint32_t f : {f0, f1})
            if (Quality(vertices[faces[f].v[0]].p, vertices[faces[f].v[1]].p, vertices[faces[f].v[2]].p) < .1) return false;
        return Dot(Unit(m0), Unit(m1)) < cosFeature;
    }
    bool EdgeIsFeature(uint32_t a, uint32_t b) const { return featureEdges.contains(Key(a, b)); }
    // 継ぎ目でない頂点のUV（どの面でも同じ）。
    Mesh::Uv UvOf(uint32_t v) const {
        for (const uint32_t f : vertices[v].faces)
            for (int k = 0; k < 3; ++k)
                if (faces[f].v[k] == v) return faces[f].uv[k];
        return {};
    }
    // UVの継ぎ目を特徴辺と同じ扱いにする（反転しない、稜線に沿う）。継ぎ目の頂点は縮約も平滑化もしない。
    void DetectSeams() {
        if (!hasUvs) return;
        for (const auto [a, b] : Edges()) {
            uint32_t f0, f1;
            if (!EdgeFaces(a, b, f0, f1)) continue;
            const auto cornerUv = [&](uint32_t f, uint32_t v) {
                for (int k = 0; k < 3; ++k) if (faces[f].v[k] == v) return faces[f].uv[k];
                return Mesh::Uv{};
            };
            const bool seam = faces[f0].chart != faces[f1].chart || cornerUv(f0, a) != cornerUv(f1, a) || cornerUv(f0, b) != cornerUv(f1, b);
            if (!seam) continue;
            featureEdges.insert(Key(a, b));
            vertices[a].seam = vertices[b].seam = true;
        }
    }
    // 最初の特徴辺を決める。折れ角の大きい辺をつなぎ、合計の長さが短い（目標の辺の長さの 3 倍未満）鎖は
    // 格子の段差とみなして捨てる。残った鎖だけが稜線になる。
    void DetectFeatures() {
        featureEdges.clear();
        if (!useFeatures) return;
        std::vector<std::pair<uint32_t, uint32_t>> candidates;
        for (const auto [a, b] : Edges()) {
            uint32_t f0, f1;
            if (EdgeFaces(a, b, f0, f1) && DihedralFeature(f0, f1)) candidates.emplace_back(a, b);
        }
        // 連結成分ごとの長さ（union-find）。
        std::vector<uint32_t> parent(vertices.size());
        for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
        const auto find = [&](uint32_t v) { while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; } return v; };
        for (const auto [a, b] : candidates) parent[find(a)] = find(b);
        std::vector<double> length(vertices.size(), 0);
        for (const auto [a, b] : candidates) length[find(a)] += EdgeLength(a, b);
        for (const auto [a, b] : candidates)
            if (length[find(a)] >= target * 3) featureEdges.insert(Key(a, b));
    }
    void Neighbours(uint32_t v, std::vector<uint32_t>& out) const {
        out.clear();
        for (const uint32_t f : vertices[v].faces)
            for (const uint32_t other : faces[f].v)
                if (other != v) out.push_back(other);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    // 頂点に集まる特徴辺の数と、その相手。
    int FeatureDegree(uint32_t v, std::array<uint32_t, 2>* along = nullptr) const {
        std::vector<uint32_t> around;
        Neighbours(v, around);
        int count = 0;
        for (const uint32_t n : around)
            if (EdgeIsFeature(v, n)) {
                if (along && count < 2) (*along)[size_t(count)] = n;
                ++count;
            }
        return count;
    }
    // 生きている辺の一覧（a < b）。頂点番号順で、結果を再現できる。
    std::vector<std::pair<uint32_t, uint32_t>> Edges() const {
        std::vector<std::pair<uint32_t, uint32_t>> edges;
        edges.reserve(aliveFaces * 3 / 2 + 1);
        for (const auto& f : faces) {
            if (!f.alive) continue;
            for (int k = 0; k < 3; ++k) {
                uint32_t a = f.v[k], b = f.v[(k + 1) % 3];
                if (a < b) edges.emplace_back(a, b);
            }
        }
        std::sort(edges.begin(), edges.end());
        return edges;
    }
    double EdgeLength(uint32_t a, uint32_t b) const { return Length(Sub(vertices[a].p, vertices[b].p)); }

    // --- 分割 ---
    // 辺 (a, b) を中点で割る。両側の面を2つずつにする。
    uint32_t Split(uint32_t a, uint32_t b, uint32_t f0, uint32_t f1) {
        const uint32_t m = uint32_t(vertices.size());
        vertices.push_back({Scale(Add(vertices[a].p, vertices[b].p), .5)});
        for (const uint32_t f : {f0, f1}) {
            auto& face = faces[f];
            int i = 0;
            while (!((face.v[i] == a && face.v[(i + 1) % 3] == b) || (face.v[i] == b && face.v[(i + 1) % 3] == a))) ++i;
            const uint32_t s = face.v[i], t = face.v[(i + 1) % 3], o = face.v[(i + 2) % 3];
            face.v = {s, m, o};
            // UVは面ごとに補間する（継ぎ目の辺なら両側の島でそれぞれ補間される）。
            const auto uvS = face.uv[size_t(i)], uvT = face.uv[size_t((i + 1) % 3)], uvO = face.uv[size_t((i + 2) % 3)];
            const Mesh::Uv uvM{(uvS.u + uvT.u) * .5f, (uvS.v + uvT.v) * .5f};
            face.uv = {uvS, uvM, uvO};
            const uint32_t chart = face.chart;
            if (f == f0) {
                vertices[m].chart = chart;
                if (featureEdges.erase(Key(a, b))) {
                    featureEdges.insert(Key(a, m));
                    featureEdges.insert(Key(m, b));
                }
                if (hasUvs && vertices[a].seam && vertices[b].seam && IsSeamEdgeFaces(f0, f1, a, b)) vertices[m].seam = true;
            }
            const uint32_t added = uint32_t(faces.size());
            faces.push_back({{m, t, o}, true, {uvM, uvT, uvO}, chart});
            std::erase(vertices[t].faces, f);
            vertices[t].faces.push_back(added);
            vertices[o].faces.push_back(added);
            vertices[m].faces.push_back(f);
            vertices[m].faces.push_back(added);
            ++aliveFaces;
        }
        return m;
    }
    // 辺 (a, b) を挟む2面の間で、UVまたは島が食い違うか（継ぎ目か）。
    bool IsSeamEdgeFaces(uint32_t f0, uint32_t f1, uint32_t a, uint32_t b) const {
        if (!hasUvs) return false;
        const auto cornerUv = [&](uint32_t f, uint32_t v) {
            for (int k = 0; k < 3; ++k) if (faces[f].v[k] == v) return faces[f].uv[k];
            return Mesh::Uv{};
        };
        return faces[f0].chart != faces[f1].chart || cornerUv(f0, a) != cornerUv(f1, a) || cornerUv(f0, b) != cornerUv(f1, b);
    }
    bool SplitLongEdges(std::string& error) {
        const double limit = target * 4 / 3;
        for (const auto [a, b] : Edges()) {
            uint32_t f0, f1;
            if (!EdgeFaces(a, b, f0, f1)) continue;
            if (EdgeLength(a, b) <= limit) continue;
            Split(a, b, f0, f1);
            if (aliveFaces > kMaxRemeshTriangles) {
                error = "出力が300万面を超えます。辺の長さを大きくしてください";
                return false;
            }
        }
        return true;
    }

    // --- 縮約 ---
    // b を a の位置 p へまとめられるか（閉じた多様体のまま、面が裏返らない、辺が長くなりすぎない）。
    bool CanCollapse(uint32_t a, uint32_t b, const P& p, const Mesh::Uv& uvNew) {
        Neighbours(a, scratchA);
        Neighbours(b, scratchB);
        uint32_t common[3];
        size_t commonCount = 0;
        for (size_t i = 0, j = 0; i < scratchA.size() && j < scratchB.size();) {
            if (scratchA[i] < scratchB[j]) ++i;
            else if (scratchA[i] > scratchB[j]) ++j;
            else {
                if (commonCount == 3) return false;
                common[commonCount++] = scratchA[i];
                ++i; ++j;
            }
        }
        if (commonCount != 2) return false;
        for (size_t i = 0; i < 2; ++i)
            if (vertices[common[i]].faces.size() <= 3) return false;
        const double limit = target * 4 / 3;
        for (const auto* ring : {&scratchA, &scratchB})
            for (const uint32_t n : *ring)
                if (n != a && n != b && Length(Sub(vertices[n].p, p)) > limit) return false;
        // 形の悪い三角形を新しく作らない。ただし、もとから形の悪い場所（格子から作ったメッシュの細長い面）では
        // 悪化しない限り許す。そこで断ると何も縮約できず、細長い面が残ってしまう。
        double worstBefore = 1, worstAfter = 1;
        for (const uint32_t end : {a, b})
            for (const uint32_t f : vertices[end].faces) {
                const auto& face = faces[f];
                const P before = Normal(face);
                const double lengthBefore = Length(before);
                worstBefore = std::min(worstBefore, Quality(vertices[face.v[0]].p, vertices[face.v[1]].p, vertices[face.v[2]].p));
                if (Has(face, a) && Has(face, b)) continue;
                P corner[3];
                for (int k = 0; k < 3; ++k) corner[k] = (face.v[k] == a || face.v[k] == b) ? p : vertices[face.v[k]].p;
                const P after = Cross(Sub(corner[1], corner[0]), Sub(corner[2], corner[0]));
                const double lengthAfter = Length(after);
                if (!(lengthAfter > 1e-18)) return false;
                if (hasUvs) {
                    // UV上でも面が裏返らず、潰れないこと。
                    auto afterUv = face.uv;
                    for (int k = 0; k < 3; ++k)
                        if (face.v[k] == a || face.v[k] == b) afterUv[size_t(k)] = uvNew;
                    const double beforeArea = UvArea(face.uv), afterArea = UvArea(afterUv);
                    if (std::abs(afterArea) < 1e-16 || beforeArea * afterArea <= 0 || std::abs(afterArea) < std::abs(beforeArea) * .05) return false;
                }
                // 面積がほぼ無い面は向きが決まらないので、裏返りの検査を通す。
                if (lengthBefore > 1e-14 && Dot(before, after) < .2 * lengthBefore * lengthAfter) return false;
                worstAfter = std::min(worstAfter, Quality(corner[0], corner[1], corner[2]));
            }
        if (worstAfter < kMinQuality && worstAfter < worstBefore) return false;
        return true;
    }
    void Collapse(uint32_t a, uint32_t b, const P& p, const Mesh::Uv& uvNew) {
        auto &keep = vertices[a], &gone = vertices[b];
        for (const uint32_t f : gone.faces) {
            auto& face = faces[f];
            if (Has(face, a)) {
                face.alive = false;
                --aliveFaces;
                for (const uint32_t v : face.v)
                    if (v != b) std::erase(vertices[v].faces, f);
            } else {
                for (uint32_t& v : face.v)
                    if (v == b) v = a;
                keep.faces.push_back(f);
            }
        }
        // 消える頂点の特徴辺は、残す頂点へ付け替える。
        if (!featureEdges.empty()) {
            featureEdges.erase(Key(a, b));
            Neighbours(b, scratchB);
            for (const uint32_t n : scratchB)
                if (n != a && featureEdges.erase(Key(b, n))) featureEdges.insert(Key(a, n));
        }
        if (hasUvs)
            for (const uint32_t f : keep.faces)
                for (int k = 0; k < 3; ++k)
                    if (faces[f].v[k] == a) faces[f].uv[size_t(k)] = uvNew;
        gone.faces.clear();
        gone.alive = false;
        keep.p = p;
    }
    void CollapseShortEdges() {
        const double low = target * 4 / 5;
        for (const auto [a, b] : Edges()) {
            uint32_t f0, f1;
            if (!vertices[a].alive || !vertices[b].alive || !EdgeFaces(a, b, f0, f1)) continue;
            if (EdgeLength(a, b) >= low) continue;
            // 継ぎ目の頂点は縮約しない（Decimate と同じ。島の境界を固定する）。
            if (hasUvs && (vertices[a].seam || vertices[b].seam)) continue;
            // 特徴の扱い。稜線どうしを混ぜず、角は動かさない。
            uint32_t keep = a, drop = b;
            P p = Scale(Add(vertices[a].p, vertices[b].p), .5);
            if (!featureEdges.empty()) {
                const bool feature = EdgeIsFeature(a, b);
                const int degreeA = FeatureDegree(a), degreeB = FeatureDegree(b);
                if (feature) {
                    const bool cornerA = degreeA >= 3, cornerB = degreeB >= 3;
                    if (cornerA && cornerB) continue;
                    if (cornerA) { keep = a; drop = b; p = vertices[a].p; }
                    else if (cornerB) { keep = b; drop = a; p = vertices[b].p; }
                } else {
                    if (degreeA > 0 && degreeB > 0) continue;
                    if (degreeA > 0) { keep = a; drop = b; p = vertices[a].p; }
                    else if (degreeB > 0) { keep = b; drop = a; p = vertices[b].p; }
                }
            }
            Mesh::Uv uvNew{};
            if (hasUvs) {
                // UVは辺上の比率で補間する。
                const P &pk = vertices[keep].p, &pd = vertices[drop].p;
                const P delta = Sub(pd, pk);
                const double t = std::clamp(Dot(Sub(p, pk), delta) / std::max(Dot(delta, delta), 1e-30), 0.0, 1.0);
                const auto uvK = UvOf(keep), uvD = UvOf(drop);
                uvNew = {std::lerp(uvK.u, uvD.u, float(t)), std::lerp(uvK.v, uvD.v, float(t))};
            }
            if (!CanCollapse(keep, drop, p, uvNew)) continue;
            Collapse(keep, drop, p, uvNew);
        }
    }

    // --- 反転 ---
    void EqualizeValences() {
        const auto valence = [&](uint32_t v) { return int(vertices[v].faces.size()); };
        for (const auto [a, b] : Edges()) {
            uint32_t f0, f1;
            if (!EdgeFaces(a, b, f0, f1)) continue;
            if (EdgeIsFeature(a, b)) continue;
            // f0 は a→b、f1 は b→a を持つ向きにそろえる。
            if (!((faces[f0].v[0] == a && faces[f0].v[1] == b) || (faces[f0].v[1] == a && faces[f0].v[2] == b) ||
                  (faces[f0].v[2] == a && faces[f0].v[0] == b)))
                std::swap(f0, f1);
            uint32_t c = 0, d = 0;
            for (const uint32_t v : faces[f0].v) if (v != a && v != b) c = v;
            for (const uint32_t v : faces[f1].v) if (v != a && v != b) d = v;
            // c と d が既につながっていれば、反転で辺が重なる。
            bool adjacent = false;
            for (const uint32_t f : vertices[c].faces) adjacent |= Has(faces[f], d);
            if (adjacent) continue;
            const auto deviation = [](int v) { return std::abs(v - 6); };
            const int before = deviation(valence(a)) + deviation(valence(b)) + deviation(valence(c)) + deviation(valence(d));
            const int after = deviation(valence(a) - 1) + deviation(valence(b) - 1) + deviation(valence(c) + 1) + deviation(valence(d) + 1);
            if (valence(a) <= 3 || valence(b) <= 3) continue;
            // 新しい2面 (c, a, d) と (d, b, c)。裏返らず、形が悪くならないこと。
            const P &pa = vertices[a].p, &pb = vertices[b].p, &pc = vertices[c].p, &pd = vertices[d].p;
            const double qualityBefore = std::min(Quality(pa, pb, pc), Quality(pb, pa, pd));
            const double qualityAfter = std::min(Quality(pc, pa, pd), Quality(pd, pb, pc));
            // 次数が整うか、細長い面（格子から作ったメッシュに多い）がはっきり良くなるときに反転する。
            const bool improvesShape = qualityBefore < .2 && qualityAfter > qualityBefore * 2;
            if (after >= before && !improvesShape) continue;
            const P old = Add(Normal(faces[f0]), Normal(faces[f1]));
            const P n0 = Cross(Sub(pa, pc), Sub(pd, pc)), n1 = Cross(Sub(pb, pd), Sub(pc, pd));
            if (Length(old) > 1e-14 && (Dot(n0, old) <= 0 || Dot(n1, old) <= 0)) continue;
            if (!(Length(n0) > 1e-18) || !(Length(n1) > 1e-18)) continue;
            if (qualityAfter < kMinQuality && qualityAfter < qualityBefore) continue;
            // 継ぎ目でない辺なので両面は同じ島。UVは各面のコーナーから引き継ぐ。
            const auto cornerUv = [&](uint32_t f, uint32_t v) {
                for (int k = 0; k < 3; ++k) if (faces[f].v[k] == v) return faces[f].uv[k];
                return Mesh::Uv{};
            };
            const auto uvA = cornerUv(f0, a), uvB = cornerUv(f0, b), uvC = cornerUv(f0, c), uvD = cornerUv(f1, d);
            if (hasUvs) {
                const double beforeUv = UvArea(faces[f0].uv);
                const double area0 = UvArea({uvC, uvA, uvD}), area1 = UvArea({uvD, uvB, uvC});
                if (std::abs(area0) < 1e-16 || std::abs(area1) < 1e-16 || area0 * beforeUv <= 0 || area1 * beforeUv <= 0) continue;
            }
            faces[f0].v = {c, a, d};
            faces[f1].v = {d, b, c};
            faces[f0].uv = {uvC, uvA, uvD};
            faces[f1].uv = {uvD, uvB, uvC};
            std::erase(vertices[a].faces, f1);
            std::erase(vertices[b].faces, f0);
            vertices[c].faces.push_back(f1);
            vertices[d].faces.push_back(f0);
        }
    }

    // --- 平滑化と投影 ---
    void SmoothAndProject(const SurfaceGrid& surface, const Mesh& input) {
        std::vector<P> normals(vertices.size(), P{0, 0, 0});
        for (const auto& f : faces) {
            if (!f.alive) continue;
            const P n = Normal(f);
            for (const uint32_t v : f.v) normals[v] = Add(normals[v], n);
        }
        std::vector<P> moved(vertices.size());
        std::vector<Mesh::Uv> movedUv(vertices.size());
        std::vector<uint8_t> uvMoved(vertices.size(), 0);
        std::vector<uint32_t> around;
        for (uint32_t v = 0; v < vertices.size(); ++v) {
            const auto& vertex = vertices[v];
            moved[v] = vertex.p;
            if (!vertex.alive || vertex.faces.empty()) continue;
            if (hasUvs && vertex.seam) continue;  // 継ぎ目の頂点は動かさない。
            std::array<uint32_t, 2> along{};
            const int degree = featureEdges.empty() ? 0 : FeatureDegree(v, &along);
            if (degree >= 3 || degree == 1) continue;  // 角と稜線の端は動かさない。
            Neighbours(v, around);
            if (around.empty()) continue;
            P delta;
            if (degree == 2) {
                // 稜線に沿ってだけ動かす。
                const P middle = Scale(Add(vertices[along[0]].p, vertices[along[1]].p), .5);
                const P direction = Unit(Sub(vertices[along[0]].p, vertices[along[1]].p));
                delta = Scale(direction, Dot(Sub(middle, vertex.p), direction));
            } else {
                P centroid{0, 0, 0};
                for (const uint32_t n : around) centroid = Add(centroid, vertices[n].p);
                centroid = Scale(centroid, 1.0 / double(around.size()));
                delta = Sub(centroid, vertex.p);
                const P n = Unit(normals[v]);
                delta = Sub(delta, Scale(n, Dot(delta, n)));
            }
            uint32_t hit = 0;
            const P projected = surface.Closest(Add(vertex.p, delta), &hit);
            if (hasUvs) {
                // 当たった元の三角形が同じ島なら、重心座標でUVを読む。別の島（継ぎ目の向こう）なら動かさない。
                if (input.uvCharts[hit] != vertex.chart) continue;
                const auto& t = input.triangles[hit];
                const P a{input.positions[t[0]].x, input.positions[t[0]].y, input.positions[t[0]].z};
                const P b{input.positions[t[1]].x, input.positions[t[1]].y, input.positions[t[1]].z};
                const P c{input.positions[t[2]].x, input.positions[t[2]].y, input.positions[t[2]].z};
                const auto w = Barycentric(projected, a, b, c);
                const auto& uv = input.cornerUvs[hit];
                movedUv[v] = {float(w[0] * uv[0].u + w[1] * uv[1].u + w[2] * uv[2].u), float(w[0] * uv[0].v + w[1] * uv[1].v + w[2] * uv[2].v)};
                uvMoved[v] = 1;
            }
            moved[v] = projected;
        }
        // 動かしたことで面が裏返る頂点は元に戻す。
        for (uint32_t v = 0; v < vertices.size(); ++v) {
            if (!vertices[v].alive || vertices[v].faces.empty()) continue;
            bool ok = true;
            for (const uint32_t f : vertices[v].faces) {
                const auto& face = faces[f];
                P corner[3];
                for (int k = 0; k < 3; ++k) corner[k] = face.v[k] == v ? moved[v] : vertices[face.v[k]].p;
                const P before = Normal(face), after = Cross(Sub(corner[1], corner[0]), Sub(corner[2], corner[0]));
                if (!(Length(after) > 1e-18) || Dot(before, after) <= 0) { ok = false; break; }
                if (hasUvs && uvMoved[v]) {
                    auto afterUv = face.uv;
                    for (int k = 0; k < 3; ++k) if (face.v[k] == v) afterUv[size_t(k)] = movedUv[v];
                    const double beforeArea = UvArea(face.uv), afterArea = UvArea(afterUv);
                    if (std::abs(afterArea) < 1e-16 || beforeArea * afterArea <= 0) { ok = false; break; }
                }
            }
            if (!ok) continue;
            vertices[v].p = moved[v];
            if (hasUvs && uvMoved[v])
                for (const uint32_t f : vertices[v].faces)
                    for (int k = 0; k < 3; ++k) if (faces[f].v[k] == v) faces[f].uv[size_t(k)] = movedUv[v];
        }
    }
};
}  // namespace

Mesh RemeshMesh(const Mesh& input, const RemeshSettings& s, std::string& error, std::stop_token stop,
                const RemeshProgress& progress) {
    error.clear();
    if (!std::isfinite(s.edgeLength) || s.edgeLength < kMinRemeshEdge || s.edgeLength > kMaxRemeshEdge) {
        error = "辺の長さは 0.002～0.2 にしてください";
        return {};
    }
    if (s.iterations < 1 || s.iterations > kMaxRemeshIterations) {
        error = "繰り返しは 1～20 にしてください";
        return {};
    }
    if (!std::isfinite(s.featureAngle) || s.featureAngle < 0 || s.featureAngle > 180) {
        error = "特徴辺の角度は 0～180 にしてください";
        return {};
    }
    const bool hasUvs = !input.cornerUvs.empty() || !input.uvCharts.empty();
    if (hasUvs && !HasValidUvs(input)) {
        error = "入力のUVが不正です";
        return {};
    }
    MeshInfo info;
    if (!InspectMesh(input, info) || !info.closed || input.triangles.size() < 4) {
        error = "入力は閉じた向き付きのメッシュにしてください";
        return {};
    }
    const double longest = std::max({double(info.maximum.x - info.minimum.x), double(info.maximum.y - info.minimum.y),
                                     double(info.maximum.z - info.minimum.z)});
    if (!(longest > 0)) {
        error = "入力のメッシュに大きさがありません";
        return {};
    }
    const double target = s.edgeLength * longest;
    double area = 0;
    for (const auto& t : input.triangles) {
        const P a{input.positions[t[0]].x, input.positions[t[0]].y, input.positions[t[0]].z};
        const P b{input.positions[t[1]].x, input.positions[t[1]].y, input.positions[t[1]].z};
        const P c{input.positions[t[2]].x, input.positions[t[2]].y, input.positions[t[2]].z};
        area += .5 * Length(Cross(Sub(b, a), Sub(c, a)));
    }
    if (area / (std::sqrt(3.0) / 4 * target * target) > double(kMaxRemeshTriangles)) {
        error = "出力が300万面を超えます。辺の長さを大きくしてください";
        return {};
    }
    // 前処理。目標より大きく長い辺を持つ面を、1→4 分割（隣は共有辺に合わせて 2～4 分割）で先に細かくする。
    // 辺を割るたびに中点から向かいの頂点へ辺を張る増分の分割だけでは、大きな三角形が扇状の細長い面に
    // 割れて面数が爆発する。1→4 分割なら辺が毎回半分になり、形も保たれる。
    Mesh coarse = input;
    for (int round = 0; round < 16; ++round) {
        std::vector<float> longFaces(coarse.triangles.size(), 0.f);
        size_t count = 0;
        for (size_t f = 0; f < coarse.triangles.size(); ++f) {
            const auto& t = coarse.triangles[f];
            for (int k = 0; k < 3; ++k) {
                const auto &p = coarse.positions[t[k]], &q = coarse.positions[t[(k + 1) % 3]];
                const double l = std::sqrt(double(p.x - q.x) * (p.x - q.x) + double(p.y - q.y) * (p.y - q.y) + double(p.z - q.z) * (p.z - q.z));
                if (l > target * 2) { longFaces[f] = 1; ++count; break; }
            }
        }
        if (count == 0) break;
        if (stop.stop_requested()) { error = "Remesh をキャンセルしました"; return {}; }
        SubdivideSettings split;
        split.levels = 1;
        split.threshold = .5f;
        coarse = SubdivideMesh(coarse, split, error, stop, {}, longFaces);
        if (!error.empty()) return {};
        if (coarse.triangles.size() > kMaxRemeshTriangles) {
            error = "出力が300万面を超えます。辺の長さを大きくしてください";
            return {};
        }
    }
    // 辺ごとの隣接面。入力は閉じているので、どの辺も2面を持つ。
    Remesher r;
    r.target = target;
    r.useFeatures = s.featureAngle < 180;
    r.cosFeature = std::cos(std::clamp(double(s.featureAngle), 0.0, 180.0) * std::numbers::pi / 180);
    r.vertices.resize(coarse.positions.size());
    for (size_t i = 0; i < coarse.positions.size(); ++i)
        r.vertices[i].p = {coarse.positions[i].x, coarse.positions[i].y, coarse.positions[i].z};
    r.faces.resize(coarse.triangles.size());
    for (size_t f = 0; f < coarse.triangles.size(); ++f) {
        r.faces[f].v = coarse.triangles[f];
        for (const uint32_t v : coarse.triangles[f]) r.vertices[v].faces.push_back(uint32_t(f));
        if (hasUvs) {
            r.faces[f].uv = coarse.cornerUvs[f];
            r.faces[f].chart = coarse.uvCharts[f];
            for (const uint32_t v : coarse.triangles[f]) r.vertices[v].chart = coarse.uvCharts[f];
        }
    }
    r.aliveFaces = coarse.triangles.size();
    for (const auto [a, b] : r.Edges()) {
        uint32_t f0, f1;
        if (!r.EdgeFaces(a, b, f0, f1)) {
            error = "入力の辺が2面に共有されていません（多様体ではありません）";
            return {};
        }
    }
    r.hasUvs = hasUvs;
    r.DetectFeatures();
    r.DetectSeams();
    SurfaceGrid surface;
    surface.Build(input, std::max(target * 2, longest / 128));
    for (int iteration = 0; iteration < s.iterations; ++iteration) {
        if (stop.stop_requested()) { error = "Remesh をキャンセルしました"; return {}; }
        if (!r.SplitLongEdges(error)) return {};
        r.CollapseShortEdges();
        r.EqualizeValences();
        r.SmoothAndProject(surface, input);
        if (progress) progress(int((iteration + 1) * 100 / s.iterations));
    }
    if (stop.stop_requested()) { error = "Remesh をキャンセルしました"; return {}; }
    // 生きている頂点と面を詰めて出力する。
    Mesh out;
    std::vector<uint32_t> remap(r.vertices.size(), std::numeric_limits<uint32_t>::max());
    for (size_t i = 0; i < r.vertices.size(); ++i) {
        if (!r.vertices[i].alive || r.vertices[i].faces.empty()) continue;
        remap[i] = uint32_t(out.positions.size());
        out.positions.push_back({float(r.vertices[i].p[0]), float(r.vertices[i].p[1]), float(r.vertices[i].p[2])});
    }
    for (const auto& f : r.faces) {
        if (!f.alive) continue;
        out.triangles.push_back({remap[f.v[0]], remap[f.v[1]], remap[f.v[2]]});
        if (hasUvs) {
            out.cornerUvs.push_back(f.uv);
            out.uvCharts.push_back(f.chart);
        }
    }
    if (hasUvs) {
        out.uvWidth = input.uvWidth;
        out.uvHeight = input.uvHeight;
        if (!HasValidUvs(out)) {
            error = "Remesh の結果のUVが不正になりました";
            return {};
        }
    }
    MeshInfo outInfo;
    if (!InspectMesh(out, outInfo) || !outInfo.closed) {
        error = "Remesh の結果が閉じたメッシュになりませんでした";
        return {};
    }
    return out;
}
}  // namespace rock::geometry
