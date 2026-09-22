#include "geometry/Decimate.h"
#include "geometry/UvUnwrap.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <queue>

namespace rock::geometry {
namespace {
using P = std::array<double, 3>;
P Sub(const P& a, const P& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
P Cross(const P& a, const P& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Dot(const P& a, const P& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double Length(const P& a) { return std::sqrt(Dot(a, a)); }

// 平面までの距離の二乗和を表す対称行列（Garland & Heckbert）。weight は足した平面の重みの合計。
struct Quadric {
    double a11 = 0, a12 = 0, a13 = 0, a14 = 0, a22 = 0, a23 = 0, a24 = 0, a33 = 0, a34 = 0, a44 = 0, weight = 0;
    void AddPlane(const P& n, double d, double w) {
        a11 += w * n[0] * n[0];
        a12 += w * n[0] * n[1];
        a13 += w * n[0] * n[2];
        a14 += w * n[0] * d;
        a22 += w * n[1] * n[1];
        a23 += w * n[1] * n[2];
        a24 += w * n[1] * d;
        a33 += w * n[2] * n[2];
        a34 += w * n[2] * d;
        a44 += w * d * d;
        weight += w;
    }
    void Add(const Quadric& q) {
        a11 += q.a11; a12 += q.a12; a13 += q.a13; a14 += q.a14; a22 += q.a22; a23 += q.a23;
        a24 += q.a24; a33 += q.a33; a34 += q.a34; a44 += q.a44; weight += q.weight;
    }
    double Cost(const P& p) const {
        return a11 * p[0] * p[0] + 2 * a12 * p[0] * p[1] + 2 * a13 * p[0] * p[2] + 2 * a14 * p[0] +
               a22 * p[1] * p[1] + 2 * a23 * p[1] * p[2] + 2 * a24 * p[1] + a33 * p[2] * p[2] + 2 * a34 * p[2] + a44;
    }
    // 誤差が最小になる位置。行列が特異に近ければ false。
    bool Optimal(P& out) const {
        const double det = a11 * (a22 * a33 - a23 * a23) - a12 * (a12 * a33 - a23 * a13) + a13 * (a12 * a23 - a22 * a13);
        const double scale = std::max({std::abs(a11), std::abs(a22), std::abs(a33), 1e-300});
        if (!(std::abs(det) > 1e-9 * scale * scale * scale)) return false;
        const double b0 = -a14, b1 = -a24, b2 = -a34;
        out[0] = (b0 * (a22 * a33 - a23 * a23) - a12 * (b1 * a33 - a23 * b2) + a13 * (b1 * a23 - a22 * b2)) / det;
        out[1] = (a11 * (b1 * a33 - a23 * b2) - b0 * (a12 * a33 - a23 * a13) + a13 * (a12 * b2 - b1 * a13)) / det;
        out[2] = (a11 * (a22 * b2 - b1 * a23) - a12 * (a12 * b2 - b1 * a13) + b0 * (a12 * a23 - a22 * a13)) / det;
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
};

// 三角形の形の良さ。面積の2倍 ÷ 最長辺の二乗。正三角形で約0.87、針のような三角形で 0 に近づく。
constexpr double kMinTriangleQuality = .05;
// 平らな場所ではどの縮約も誤差が 0 になり、順序が決まらない。辺の長さの二乗にこの係数を掛けた値を
// 優先順位へ足し、短い辺から縮約して三角形の大きさを揃える。形のずれの判定には含めない。
constexpr double kEdgeLengthPriority = 1e-4;

struct Candidate {
    // priority は取り出す順序、cost は形のずれ（面からの距離の二乗の重み付き和）。
    double priority = 0;
    double cost = 0;
    uint32_t a = 0, b = 0, versionA = 0, versionB = 0;
    P position{};
    Mesh::Uv uv{};
};
// 誤差の小さい縮約から取り出す。同じ誤差は頂点番号で順序を決め、結果を再現できるようにする。
struct Later {
    bool operator()(const Candidate& x, const Candidate& y) const {
        if (x.priority != y.priority) return x.priority > y.priority;
        if (x.a != y.a) return x.a > y.a;
        return x.b > y.b;
    }
};

struct Simplifier {
    struct Vertex {
        P position{};
        Quadric quadric;
        std::vector<uint32_t> faces;
        uint32_t version = 0;
        bool alive = true, moved = false, seam = false;
        Mesh::Uv uv{};
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
    std::priority_queue<Candidate, std::vector<Candidate>, Later> queue;
    std::vector<uint32_t> scratchA, scratchB;

    P Normal(const Face& f) const {
        return Cross(Sub(vertices[f.v[1]].position, vertices[f.v[0]].position),
                     Sub(vertices[f.v[2]].position, vertices[f.v[0]].position));
    }
    void Neighbours(uint32_t v, std::vector<uint32_t>& out) const {
        out.clear();
        for (const uint32_t f : vertices[v].faces)
            for (const uint32_t other : faces[f].v)
                if (other != v) out.push_back(other);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    void Push(uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        if (hasUvs && (vertices[a].seam || vertices[b].seam)) return;
        Quadric q = vertices[a].quadric;
        q.Add(vertices[b].quadric);
        const P &pa = vertices[a].position, &pb = vertices[b].position;
        const P middle{(pa[0] + pb[0]) * .5, (pa[1] + pb[1]) * .5, (pa[2] + pb[2]) * .5};
        Candidate c;
        c.a = a;
        c.b = b;
        c.versionA = vertices[a].version;
        c.versionB = vertices[b].version;
        // 最適な位置が辺から遠く離れるのは、行列の条件が悪いとき。端点と中点から選び直す。
        P best;
        const double edge = Length(Sub(pa, pb));
        if (q.Optimal(best) && Length(Sub(best, middle)) <= 2 * edge) {
            c.position = best;
            c.cost = q.Cost(best);
        } else {
            c.position = pa;
            c.cost = q.Cost(pa);
            for (const P& p : {pb, middle})
                if (const double cost = q.Cost(p); cost < c.cost) {
                    c.cost = cost;
                    c.position = p;
                }
        }
        if (hasUvs) {
            // UVと位置を同じ辺上の比率で補間する。島の境界頂点は動かさない。
            const P delta = Sub(pb, pa);
            const double t = std::clamp(Dot(Sub(c.position, pa), delta) / std::max(Dot(delta, delta), 1e-30), 0.0, 1.0);
            for (int k = 0; k < 3; ++k) c.position[k] = pa[k] + t * delta[k];
            c.uv = {std::lerp(vertices[a].uv.u, vertices[b].uv.u, float(t)),
                    std::lerp(vertices[a].uv.v, vertices[b].uv.v, float(t))};
            c.cost = q.Cost(c.position);
        }
        c.cost = std::max(c.cost, 0.0);
        c.priority = c.cost + kEdgeLengthPriority * edge * edge * q.weight;
        queue.push(c);
    }
    // 縮約しても閉じた多様体のままか、面が裏返らないかを調べる。
    bool CanCollapse(const Candidate& c) {
        Neighbours(c.a, scratchA);
        Neighbours(c.b, scratchB);
        // 辺の両端が共有する隣接頂点は、辺を挟む2面の向かいの頂点だけでなければならない（リンク条件）。
        uint32_t common[3];
        size_t commonCount = 0;
        for (size_t i = 0, j = 0; i < scratchA.size() && j < scratchB.size();) {
            if (scratchA[i] < scratchB[j])
                ++i;
            else if (scratchA[i] > scratchB[j])
                ++j;
            else {
                if (commonCount == 3) return false;
                common[commonCount++] = scratchA[i];
                ++i;
                ++j;
            }
        }
        if (commonCount != 2) return false;
        // 向かいの頂点の面が3枚以下だと、縮約で面2枚だけの頂点（潰れた袋）ができる。四面体もここで守られる。
        for (size_t i = 0; i < 2; ++i)
            if (vertices[common[i]].faces.size() <= 3) return false;
        // 三角形の形の良さ。面積の2倍 ÷ 最長辺の二乗。
        const auto quality = [](const P& a, const P& b, const P& d, double twiceArea) {
            const double longest = std::max({Dot(Sub(b, a), Sub(b, a)), Dot(Sub(d, b), Sub(d, b)), Dot(Sub(a, d), Sub(a, d))});
            return longest > 0 ? twiceArea / longest : 0.0;
        };
        double worstBefore = std::numeric_limits<double>::max(), worstAfter = std::numeric_limits<double>::max();
        for (const uint32_t end : {c.a, c.b})
            for (const uint32_t f : vertices[end].faces) {
                const auto& face = faces[f];
                const bool hasA = face.v[0] == c.a || face.v[1] == c.a || face.v[2] == c.a;
                const bool hasB = face.v[0] == c.b || face.v[1] == c.b || face.v[2] == c.b;
                const P before = Normal(face);
                const double lengthBefore = Length(before);
                worstBefore = std::min(worstBefore, quality(vertices[face.v[0]].position, vertices[face.v[1]].position,
                                                            vertices[face.v[2]].position, lengthBefore));
                if (hasA && hasB) continue;  // この2面は消える。
                if (hasUvs) {
                    auto afterUv = face.uv;
                    for (int k = 0; k < 3; ++k)
                        if (face.v[k] == c.a || face.v[k] == c.b) afterUv[k] = c.uv;
                    const auto area = [](const auto& uv) {
                        return double(uv[1].u-uv[0].u)*(uv[2].v-uv[0].v) - double(uv[1].v-uv[0].v)*(uv[2].u-uv[0].u);
                    };
                    const double beforeArea = area(face.uv), afterArea = area(afterUv);
                    if (std::abs(afterArea) < 1e-16 || beforeArea * afterArea <= 0 || std::abs(afterArea) < std::abs(beforeArea) * .05) return false;
                }
                P corner[3];
                for (int k = 0; k < 3; ++k)
                    corner[k] = (face.v[k] == c.a || face.v[k] == c.b) ? c.position : vertices[face.v[k]].position;
                const P after = Cross(Sub(corner[1], corner[0]), Sub(corner[2], corner[0]));
                const double lengthAfter = Length(after);
                // 裏返る面と、面積のない面を作らない。
                if (!(lengthAfter > 1e-14) || Dot(before, after) < .2 * lengthBefore * lengthAfter) return false;
                worstAfter = std::min(worstAfter, quality(corner[0], corner[1], corner[2], lengthAfter));
            }
        // 細長い三角形は UV 展開で縮退し、陰影も乱れる。形の良かった場所に細長い三角形を作る縮約は断る。
        // Marching Tetrahedra のメッシュのように、もとから細長い面がある場所は自由に動かす。そこで断ると
        // 何も縮約できなくなる。短い辺から縮約するので、細長い面は先に消えていく。
        if (worstAfter < kMinTriangleQuality && worstBefore >= kMinTriangleQuality) return false;
        return true;
    }
    void Collapse(const Candidate& c) {
        auto &keep = vertices[c.a], &gone = vertices[c.b];
        for (const uint32_t f : gone.faces) {
            auto& face = faces[f];
            const bool shared = face.v[0] == c.a || face.v[1] == c.a || face.v[2] == c.a;
            if (shared) {
                face.alive = false;
                for (const uint32_t v : face.v)
                    if (v != c.b) std::erase(vertices[v].faces, f);
            } else {
                for (uint32_t& v : face.v)
                    if (v == c.b) v = c.a;
                keep.faces.push_back(f);
            }
        }
        if (hasUvs) {
            for (const auto f : keep.faces)
                for (int k = 0; k < 3; ++k) if (faces[f].v[k] == c.a) faces[f].uv[k] = c.uv;
            keep.uv = c.uv;
        }
        gone.faces.clear();
        gone.alive = false;
        keep.position = c.position;
        keep.moved = true;
        keep.quadric.Add(gone.quadric);
        ++keep.version;
        ++gone.version;
        Neighbours(c.a, scratchA);
        for (const uint32_t n : scratchA) Push(c.a, n);
    }
};
}  // namespace

Mesh DecimateMesh(const Mesh& input, const DecimateSettings& s, std::string& error, std::stop_token stop,
                  const DecimateProgress& progress) {
    error.clear();
    if (s.targetTriangles < MinDecimateTriangles || s.targetTriangles > MaxDecimateTriangles) {
        error = "目標の三角形数は 64～500000 にしてください";
        return {};
    }
    if (!std::isfinite(s.maxError) || s.maxError < 0 || s.maxError > .1f) {
        error = "形のずれの上限は 0～0.1 にしてください";
        return {};
    }
    if (!std::isfinite(s.creaseWeight) || s.creaseWeight < 0 || s.creaseWeight > 10) {
        error = "稜線の保護は 0～10 にしてください";
        return {};
    }
    if (input.triangles.size() > MaxDecimateInputTriangles) {
        error = "Decimate の入力は300万三角形までです。上流の解像度を下げてください";
        return {};
    }
    MeshInfo info;
    if (!InspectMesh(input, info) || !info.closed || info.volume <= 0) {
        error = "閉じた、外向きの面を持つMeshが必要です";
        return {};
    }
    const bool hasUvs = !input.cornerUvs.empty();
    if ((hasUvs && !HasValidUvs(input)) || (!input.uvCharts.empty() && input.uvCharts.size() != input.triangles.size())) {
        error = "Decimateの入力UVが不正です";
        return {};
    }
    if (input.triangles.size() <= size_t(s.targetTriangles)) return input;
    // 誤差の比較と行列の条件を寸法に依らなくするため、最長辺を 1 にした座標で計算する。
    const double scale =
        std::max({double(info.maximum.x) - info.minimum.x, double(info.maximum.y) - info.minimum.y,
                  double(info.maximum.z) - info.minimum.z});
    if (!(scale > 0)) {
        error = "Meshの範囲が不正です";
        return {};
    }
    Simplifier mesh;
    mesh.hasUvs = hasUvs;
    mesh.vertices.resize(input.positions.size());
    for (size_t i = 0; i < input.positions.size(); ++i) {
        const auto& p = input.positions[i];
        mesh.vertices[i].position = {(double(p.x) - info.minimum.x) / scale, (double(p.y) - info.minimum.y) / scale,
                                     (double(p.z) - info.minimum.z) / scale};
    }
    mesh.faces.resize(input.triangles.size());
    // 辺ごとの隣接面。入力は閉じているので、どの辺も2面を持つ。
    struct EdgeFace {
        uint64_t key;
        uint32_t face;
    };
    std::vector<EdgeFace> edges;
    edges.reserve(input.triangles.size() * 3);
    for (uint32_t f = 0; f < input.triangles.size(); ++f) {
        mesh.faces[f].v = input.triangles[f];
        if (hasUvs) {
            mesh.faces[f].uv = input.cornerUvs[f];
            if (!input.uvCharts.empty()) mesh.faces[f].chart = input.uvCharts[f];
            for (int k = 0; k < 3; ++k) {
                auto& vertex = mesh.vertices[input.triangles[f][k]];
                if (!vertex.faces.empty() && vertex.uv != input.cornerUvs[f][k]) vertex.seam = true;
                vertex.uv = input.cornerUvs[f][k];
            }
        }
        const P normal = mesh.Normal(mesh.faces[f]);
        const double twiceArea = Length(normal);
        const P unit{normal[0] / twiceArea, normal[1] / twiceArea, normal[2] / twiceArea};
        const double d = -Dot(unit, mesh.vertices[input.triangles[f][0]].position);
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = input.triangles[f][k], w = input.triangles[f][(k + 1) % 3];
            mesh.vertices[v].faces.push_back(f);
            // 面積で重み付けする。細かく割れた面が、同じ広さの1枚の面より強く効かないようにする。
            mesh.vertices[v].quadric.AddPlane(unit, d, twiceArea * .5);
            edges.push_back({(uint64_t(std::min(v, w)) << 32) | std::max(v, w), f});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const EdgeFace& a, const EdgeFace& b) {
        return a.key != b.key ? a.key < b.key : a.face < b.face;
    });
    const double creaseCosine = std::cos(40.0 * std::numbers::pi / 180);
    for (size_t i = 0; i + 1 < edges.size(); i += 2) {
        const uint32_t a = uint32_t(edges[i].key >> 32), b = uint32_t(edges[i].key);
        if (hasUvs && mesh.faces[edges[i].face].chart != mesh.faces[edges[i+1].face].chart) {
            mesh.vertices[a].seam = mesh.vertices[b].seam = true;
        }
        if (s.creaseWeight > 0) {
            // 折れ角の大きい辺には、辺を含んで各面に垂直な平面を足す。辺に沿ってしか動けなくなり、稜線が残る。
            const P n0 = mesh.Normal(mesh.faces[edges[i].face]), n1 = mesh.Normal(mesh.faces[edges[i + 1].face]);
            const double l0 = Length(n0), l1 = Length(n1);
            if (Dot(n0, n1) < creaseCosine * l0 * l1) {
                const P along = Sub(mesh.vertices[b].position, mesh.vertices[a].position);
                for (const auto& [normal, length] : {std::pair{n0, l0}, std::pair{n1, l1}}) {
                    P side = Cross(along, normal);
                    const double sideLength = Length(side);
                    if (!(sideLength > 0)) continue;
                    for (double& v : side) v /= sideLength;
                    const double d = -Dot(side, mesh.vertices[a].position), weight = double(s.creaseWeight) * length;
                    mesh.vertices[a].quadric.AddPlane(side, d, weight);
                    mesh.vertices[b].quadric.AddPlane(side, d, weight);
                }
            }
        }
    }
    for (size_t i = 0; i + 1 < edges.size(); i += 2) mesh.Push(uint32_t(edges[i].key >> 32), uint32_t(edges[i].key));

    size_t alive = input.triangles.size();
    const size_t target = size_t(s.targetTriangles), toRemove = alive - target;
    const double maxError = double(s.maxError);
    size_t steps = 0;
    while (alive > target && !mesh.queue.empty()) {
        if ((++steps & 1023) == 0) {
            if (stop.stop_requested()) {
                error = "評価をキャンセルしました";
                return {};
            }
            if (progress) progress(int(std::min<size_t>(100, (input.triangles.size() - alive) * 100 / toRemove)));
        }
        const Candidate c = mesh.queue.top();
        mesh.queue.pop();
        const auto &a = mesh.vertices[c.a], &b = mesh.vertices[c.b];
        if (!a.alive || !b.alive || a.version != c.versionA || b.version != c.versionB) continue;
        if (maxError > 0) {
            // 誤差は増える一方なので、上限を超えた縮約は後でも使えない。
            const double weight = a.quadric.weight + b.quadric.weight;
            if (weight > 0 && std::sqrt(c.cost / weight) > maxError) continue;
        }
        if (!mesh.CanCollapse(c)) continue;
        mesh.Collapse(c);
        alive -= 2;
    }
    if (stop.stop_requested()) {
        error = "評価をキャンセルしました";
        return {};
    }
    Mesh out;
    if (hasUvs) { out.uvWidth = input.uvWidth; out.uvHeight = input.uvHeight; }
    std::vector<uint32_t> remap(mesh.vertices.size(), UINT32_MAX);
    out.triangles.reserve(alive);
    for (const auto& face : mesh.faces) {
        if (!face.alive) continue;
        std::array<uint32_t, 3> triangle{};
        for (int k = 0; k < 3; ++k) {
            uint32_t& index = remap[face.v[k]];
            if (index == UINT32_MAX) {
                index = uint32_t(out.positions.size());
                const auto& vertex = mesh.vertices[face.v[k]];
                // 動かしていない頂点は、入力の座標をそのまま使う。
                out.positions.push_back(vertex.moved ? Vec3{float(vertex.position[0] * scale + info.minimum.x),
                                                            float(vertex.position[1] * scale + info.minimum.y),
                                                            float(vertex.position[2] * scale + info.minimum.z)}
                                                     : input.positions[face.v[k]]);
            }
            triangle[k] = index;
        }
        out.triangles.push_back(triangle);
        if (hasUvs) {
            out.cornerUvs.push_back(face.uv);
            if (!input.uvCharts.empty()) out.uvCharts.push_back(face.chart);
        }
    }
    MeshInfo result;
    if (!InspectMesh(out, result) || !result.closed || result.volume <= 0 || result.components != info.components) {
        error = "三角形を減らした結果が閉じた形になりませんでした。目標を上げるか、形のずれの上限を下げてください";
        return {};
    }
    if (progress) progress(100);
    return out;
}
}  // namespace rock::geometry
