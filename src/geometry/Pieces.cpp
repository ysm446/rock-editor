#include "geometry/Pieces.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <execution>
#include <map>
#include <limits>
#include <numeric>
#include <set>

namespace rock::geometry {
namespace {
struct D {
    double x = 0, y = 0, z = 0;
    D operator+(D b) const {
        return {x + b.x, y + b.y, z + b.z};
    }
    D operator-(D b) const {
        return {x - b.x, y - b.y, z - b.z};
    }
    D operator*(double s) const {
        return {x * s, y * s, z * s};
    }
};
D V(Vec3 a) {
    return {a.x, a.y, a.z};
}
Vec3 F(D a) {
    return {float(a.x), float(a.y), float(a.z)};
}
double Dot(D a, D b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
D Cross(D a, D b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Length(D a) {
    return std::sqrt(Dot(a, a));
}
struct Hash {
    uint64_t value = 14695981039346656037ull;
    void Add(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            value ^= (v >> (i * 8)) & 255;
            value *= 1099511628211ull;
        }
    }
    void Float(float v) {
        Add(std::bit_cast<uint32_t>(v));
    }
};
uint64_t Random(uint64_t &state) {
    state += 0x9e3779b97f4a7c15ull;
    auto z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
double Uniform(uint64_t &state) {
    return double(Random(state) >> 11) * 0x1.0p-53;
}
struct Plane {
    D n;
    double d = 0;
};
struct Face {
    std::vector<uint32_t> ids;
    uint32_t outer = 0;
    bool original = true;
};
struct Poly {
    std::vector<D> vertices;
    std::vector<Face> faces;
};
bool Source(const Mesh &mesh, Poly &poly, std::vector<Plane> &planes, D &origin, double &scale,
            std::string &error, std::stop_token stop) {
    MeshInfo info;
    if (mesh.triangles.size() > 20000 || !InspectMesh(mesh, info) || !info.closed || info.components != 1 ||
        info.volume <= 0) {
        error = "入力は閉じた単一の凸メッシュ（2万三角形以下）が必要です";
        return false;
    }
    origin = (V(info.minimum) + V(info.maximum)) * .5;
    auto extent = V(info.maximum) - V(info.minimum);
    scale = std::max({extent.x, extent.y, extent.z});
    if (!(scale > 0)) {
        error = "入力寸法が不正です";
        return false;
    }
    for (auto p : mesh.positions)
        poly.vertices.push_back((V(p) - origin) * (1 / scale));
    for (auto t : mesh.triangles) {
        if (stop.stop_requested()) {
            error = "評価をキャンセルしました";
            return false;
        }
        auto a = poly.vertices[t[0]], n = Cross(poly.vertices[t[1]] - a, poly.vertices[t[2]] - a);
        n = n * (1 / Length(n));
        Plane plane{n, Dot(n, a)};
        for (auto p : poly.vertices)
            if (Dot(n, p) - plane.d > 2e-7) {
                error = "凹形状は未対応です。Boxまたは凸形状を使用してください";
                return false;
            }
        planes.push_back(plane);
        uint32_t mask = 0;
        const double axis[3] = {n.x, n.y, n.z};
        for (int k = 0; k < 3; ++k)
            if (std::abs(axis[k]) > 1 - 1e-7)
                mask |= 1u << (k * 2 + (axis[k] < 0 ? 1 : 0));
        poly.faces.push_back({{t[0], t[1], t[2]}, mask, true});
    }
    return true;
}
// 並列に切り出した片1つ分の結果。
enum PieceError : uint8_t { PieceOk, PieceCancelled, PieceOpenCut, PieceTooSmall };
struct Built {
    Piece piece;
    double volume = 0;
    PieceError error = PieceOk;
};
// 同じ辺の交点を一度だけ生成し、切断面は境界辺の逆向きの閉路から作る。
bool Clip(Poly &poly, Plane plane) {
    // 片ごとに点数−1回呼ばれる。距離の配列は使い回し、呼び出しごとの確保を避ける。
    thread_local std::vector<double> distances;
    distances.clear();
    for (auto p : poly.vertices) {
        auto d = Dot(plane.n, p) - plane.d;
        distances.push_back(std::abs(d) < 1e-12 ? 0 : d);
    }
    bool outside = false, inside = false;
    for (const auto &f : poly.faces)
        for (auto i : f.ids) {
            outside |= distances[i] > 0;
            inside |= distances[i] < 0;
        }
    if (!outside)
        return true;
    if (!inside) {
        poly.faces.clear();
        return true;
    }
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> intersections;
    const auto intersect = [&](uint32_t a, uint32_t b) {
        if (distances[a] == 0)
            return a;
        if (distances[b] == 0)
            return b;
        auto key = std::minmax(a, b);
        auto found = intersections.find(key);
        if (found != intersections.end())
            return found->second;
        auto p = poly.vertices[a] +
                 (poly.vertices[b] - poly.vertices[a]) * (distances[a] / (distances[a] - distances[b]));
        auto id = uint32_t(poly.vertices.size());
        poly.vertices.push_back(p);
        distances.push_back(0);
        intersections[key] = id;
        return id;
    };
    std::vector<Face> faces;
    for (const auto &face : poly.faces) {
        Face out{{}, face.outer, face.original};
        for (size_t i = 0; i < face.ids.size(); ++i) {
            auto a = face.ids[i], b = face.ids[(i + 1) % face.ids.size()];
            if (distances[a] <= 0)
                out.ids.push_back(a);
            if ((distances[a] < 0 && distances[b] > 0) || (distances[a] > 0 && distances[b] < 0))
                out.ids.push_back(intersect(a, b));
        }
        if (out.ids.size() >= 3)
            faces.push_back(std::move(out));
    }
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (const auto &f : faces)
        for (size_t i = 0; i < f.ids.size(); ++i) {
            auto a = f.ids[i], b = f.ids[(i + 1) % f.ids.size()];
            auto opposite = edges.find({b, a});
            if (opposite != edges.end())
                edges.erase(opposite);
            else
                edges[{a, b}] = 1;
        }
    std::map<uint32_t, uint32_t> next;
    for (const auto &[e, count] : edges)
        if (!next.emplace(e.second, e.first).second)
            return false;
    if (next.size() < 3)
        return false;
    Face cap{{}, 0, false};
    auto start = next.begin()->first, current = start;
    do {
        cap.ids.push_back(current);
        auto it = next.find(current);
        if (it == next.end())
            return false;
        current = it->second;
        if (cap.ids.size() > next.size())
            return false;
    } while (current != start);
    if (cap.ids.size() != next.size())
        return false;
    faces.push_back(std::move(cap));
    poly.faces = std::move(faces);
    return true;
}
D Rotate(D p, const std::array<float, 3> &degrees, bool inverse = false) {
    // Z→X→Y。逆変換は逆順・逆角度。
    const int order[3] = {2, 0, 1};
    for (int step = 0; step < 3; ++step) {
        int axis = order[inverse ? 2 - step : step];
        double r = degrees[axis] * 3.141592653589793 / 180 * (inverse ? -1 : 1), c = std::cos(r),
               s = std::sin(r);
        if (axis == 0)
            p = {p.x, c * p.y - s * p.z, s * p.y + c * p.z};
        if (axis == 1)
            p = {c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
        if (axis == 2)
            p = {c * p.x - s * p.y, s * p.x + c * p.y, p.z};
    }
    return p;
}
D Apply(const std::array<double, 12> &m, D p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}
double Determinant(const std::array<double, 12> &m) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
           m[2] * (m[4] * m[9] - m[5] * m[8]);
}
bool Matches(const PieceCollection &c, const PieceSelection &s, std::string &error) {
    if (c.producer == s.producer && c.generation == s.generation && c.fingerprint == s.input)
        return true;
    error = "SelectionとPiecesの入力が一致しません。同じ枝の選択を接続してください";
    return false;
}
bool Contains(const std::vector<uint32_t> &ids, uint32_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
} // namespace
uint64_t MeshFingerprint(const Mesh &m) {
    Hash h;
    h.Add(m.positions.size());
    h.Add(m.triangles.size());
    for (auto p : m.positions) {
        h.Float(p.x);
        h.Float(p.y);
        h.Float(p.z);
    }
    for (auto f : m.triangles)
        for (auto i : f)
            h.Add(i);
    return h.value;
}
PieceCollection MakeLayeredBoxes(const LayeredBoxesSettings& s, int producer, std::string& error, std::stop_token stop) {
    error.clear();
    const auto range=[](float v,float lo,float hi){return std::isfinite(v) && v>=lo && v<=hi;};
    if (s.count<1 || s.count>32 || !range(s.gap,0,10) || !range(s.thicknessVariation,0,.8f) ||
        !range(s.sizeVariation,0,.8f) || !range(s.offset,0,1000) ||
        std::any_of(s.size.begin(),s.size.end(),[&](float v){return !range(v,.001f,1000);}) ||
        std::any_of(s.rotation.begin(),s.rotation.end(),[](float v){return !std::isfinite(v);})) {
        error="板は1〜32枚、寸法0.001〜1000m、隙間0〜10m、ばらつき0〜0.8、ずれ0〜1000mにしてください";
        return {};
    }
    auto rotation=s.rotation;
    for (auto& r:rotation) r=std::fmod(r,360.f);
    const D axes[]={Rotate({1,0,0},rotation),Rotate({0,1,0},rotation),Rotate({0,0,1},rotation)};
    PieceCollection out; out.producer=producer;
    Hash hash; hash.Add(s.count); hash.Add(s.seed);
    for (auto v:s.size) hash.Float(v);
    for (auto v:s.rotation) hash.Float(v);
    hash.Float(s.gap); hash.Float(s.thicknessVariation); hash.Float(s.sizeVariation); hash.Float(s.offset);
    out.generation=hash.value;
    std::vector<D> centers;
    double height=0;
    for (int i=0;i<s.count;++i) {
        if (stop.stop_requested()) {error="評価をキャンセルしました";return {};}
        uint64_t state=uint64_t(s.seed) ^ (uint64_t(i)<<32);
        const auto vary=[&](float v,float amount){return float(v*(1+amount*(2*Uniform(state)-1)));};
        const std::array<float,3> size{vary(s.size[0],s.sizeVariation),vary(s.size[1],s.thicknessVariation),vary(s.size[2],s.sizeVariation)};
        if (*std::min_element(size.begin(),size.end())<.001f || *std::max_element(size.begin(),size.end())>1000) {
            error="ばらつき後の板の寸法が0.001〜1000mを外れます。寸法かばらつきを調整してください";return {};
        }
        Piece p; p.id=uint32_t(i); p.layer=i; p.layerSize=size; p.layerRim=true; p.outerFaces=63;
        p.mesh=std::make_shared<const Mesh>(MakeBox(size));
        p.faceOrigins=std::make_shared<const std::vector<uint8_t>>(p.mesh->triangles.size(),1);
        p.volume=double(size[0])*size[1]*size[2];
        centers.push_back({s.offset*(2*Uniform(state)-1),height+size[1]*.5,s.offset*(2*Uniform(state)-1)});
        height+=size[1]+(i+1<s.count?s.gap:0);
        out.pieces.push_back(std::move(p));
    }
    for (size_t i=0;i<out.pieces.size();++i) {
        centers[i].y-=height*.5;
        const auto position=Rotate(centers[i],rotation);
        out.pieces[i].transform={axes[0].x,axes[1].x,axes[2].x,position.x,
                                 axes[0].y,axes[1].y,axes[2].y,position.y,
                                 axes[0].z,axes[1].z,axes[2].z,position.z};
    }
    RefreshPieceFingerprint(out);
    return out;
}

PointSet ScatterPoints(const Mesh &mesh, const ScatterSettings &s, std::string &error, std::stop_token stop) {
    error.clear();
    PointSet out;
    if (s.version != 1 || s.count < 2 || s.count > MaxScatterPoints) {
        error = "点数は2〜" + std::to_string(MaxScatterPoints) + "、アルゴリズムはversion 1が必要です";
        return {};
    }
    Poly poly;
    std::vector<Plane> planes;
    D origin;
    double scale;
    if (!Source(mesh, poly, planes, origin, scale, error, stop))
        return {};
    D lo = poly.vertices[0], hi = lo;
    for (auto p : poly.vertices) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    uint64_t state = s.seed;
    std::vector<D> accepted;
    for (int attempt = 0; attempt < s.count * 10000 && accepted.size() < size_t(s.count); ++attempt) {
        if (stop.stop_requested()) {
            error = "評価をキャンセルしました";
            return {};
        }
        D p{lo.x + (hi.x - lo.x) * Uniform(state), lo.y + (hi.y - lo.y) * Uniform(state),
            lo.z + (hi.z - lo.z) * Uniform(state)};
        if (s.planar) p.y=(lo.y+hi.y)*.5;
        bool valid = true;
        for (auto plane : planes)
            if (Dot(plane.n, p) > plane.d - 1e-8) {
                valid = false;
                break;
            }
        if (!valid)
            continue;
        // 出力はfloatへ丸める。Voronoi Fractureは丸めた値から局所座標を求め直して内外を調べるので、
        // 原点から遠い入力でも「点が入力の外」とならないよう、同じ値・同じ許容差で確かめておく。
        const Vec3 stored = F(origin + p * scale);
        const D local = (V(stored) - origin) * (1 / scale);
        for (auto plane : planes)
            if (Dot(plane.n, local) > plane.d + 1e-8) {
                valid = false;
                break;
            }
        for (auto other : accepted)
            if (Length(p - other) < 1e-5) {
                valid = false;
                break;
            }
        if (valid) {
            accepted.push_back(p);
            out.positions.push_back(stored);
        }
    }
    if (accepted.size() != size_t(s.count)) {
        error = "点を十分に配置できません。寸法または点数を調整してください";
        return {};
    }
    out.source = MeshFingerprint(mesh);
    Hash h;
    h.Add(out.source);
    h.Add(s.seed);
    h.Add(s.count);
    h.Add(s.version);
    if (s.planar) h.Add(0x504c414e4152ull);
    out.fingerprint = h.value;
    return out;
}
PointSet ScatterPiecePoints(const PieceCollection& pieces, const ScatterSettings& s,
                            std::string& error, std::stop_token stop) {
    error.clear();
    if (s.version!=1 || s.count<2 || s.count>MaxScatterPoints || pieces.pieces.size()>size_t(MaxScatterPoints/s.count)) {
        error="各ピースの点数は2〜512、全体の合計は512点までです"; return {};
    }
    PointSet out; out.grouped=true; out.source=pieces.fingerprint;
    Hash hash; hash.Add(out.source); hash.Add(s.count); hash.Add(s.seed); hash.Add(s.planar);
    std::set<uint32_t> seen;
    for (const auto& p:pieces.pieces) {
        if (stop.stop_requested()) {error="評価をキャンセルしました";return {};}
        if (!p.mesh || !seen.insert(p.id).second) {error="ピースのメッシュまたはIDが不正です";return {};}
        auto local=s;
        // 他の板の追加・削除や配置変更で、この板の点配置を変えない。
        uint64_t state=uint64_t(s.seed) ^ (uint64_t(p.id)<<32);
        local.seed=uint32_t(Random(state));
        auto points=ScatterPoints(*p.mesh,local,error,stop);
        if (!error.empty()) {error="ピース "+std::to_string(p.id)+": "+error;return {};}
        for (auto v:points.positions) {
            const auto world=F(Apply(p.transform,V(v)));
            if (!std::isfinite(world.x)||!std::isfinite(world.y)||!std::isfinite(world.z)) {
                error="ピースの変換が不正です";return {};
            }
            out.positions.push_back(world);
        }
        hash.Add(p.id); hash.Add(points.fingerprint);
        out.groups.push_back({p.id,points.source,points.fingerprint,std::move(points.positions)});
    }
    out.fingerprint=hash.value;
    return out;
}
PieceCollection FractureVoronoi(const Mesh &mesh, const PointSet &points, const VoronoiSettings &s,
                                int producer, std::string &error, std::stop_token stop) {
    error.clear();
    if (points.grouped || points.source != MeshFingerprint(mesh) || points.positions.size() < 2 ||
        points.positions.size() > MaxScatterPoints || s.version != 1) {
        error = "同じMeshから生成した2〜" + std::to_string(MaxScatterPoints) + "点のScatter Pointsが必要です";
        return {};
    }
    for (auto v : s.rotation)
        if (!std::isfinite(v)) {
            error = "回転角度が不正です";
            return {};
        }
    for (auto v : s.stretch)
        if (!std::isfinite(v) || v <= 0) {
            error = "伸長倍率は正の有限値が必要です";
            return {};
        }
    if (*std::max_element(s.stretch.begin(), s.stretch.end()) /
            *std::min_element(s.stretch.begin(), s.stretch.end()) >
        16) {
        error = "伸長倍率の最大/最小比は16以下にしてください";
        return {};
    }
    Poly initial;
    std::vector<Plane> sourcePlanes;
    D origin;
    double scale;
    if (!Source(mesh, initial, sourcePlanes, origin, scale, error, stop))
        return {};
    std::vector<D> sites;
    for (auto p : points.positions) {
        D local = (V(p) - origin) * (1 / scale);
        if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z)) {
            error = "点が不正です";
            return {};
        }
        for (auto plane : sourcePlanes)
            if (Dot(plane.n, local) > plane.d + 1e-8) {
                error = "点が入力の外にあります";
                return {};
            }
        auto q = Rotate(local, s.rotation, true);
        q = {q.x / s.stretch[0], q.y / s.stretch[1], q.z / s.stretch[2]};
        for (auto other : sites)
            if (Length(q - other) < 1e-8) {
                error = "重複または近接しすぎた点があります";
                return {};
            }
        sites.push_back(q);
    }
    std::vector<std::vector<Plane>> cuts(sites.size(), std::vector<Plane>(sites.size()));
    for (size_t i = 0; i < sites.size(); ++i)
        for (size_t j = i + 1; j < sites.size(); ++j) {
            auto n = (sites[j] - sites[i]) * 2;
            double d = Dot(sites[j], sites[j]) - Dot(sites[i], sites[i]);
            n = Rotate({n.x / s.stretch[0], n.y / s.stretch[1], n.z / s.stretch[2]}, s.rotation);
            double len = Length(n);
            n = n * (1 / len);
            d /= len;
            cuts[i][j] = {n, d};
            cuts[j][i] = {n * -1, -d};
        }
    PieceCollection out;
    out.producer = producer;
    Hash hash;
    hash.Add(points.source);
    hash.Add(points.fingerprint);
    hash.Add(s.version);
    for (auto v : s.rotation)
        hash.Float(v);
    for (auto v : s.stretch)
        hash.Float(v);
    for (auto p : points.positions) {
        hash.Float(p.x);
        hash.Float(p.y);
        hash.Float(p.z);
    }
    out.generation = hash.value;
    // 片どうしは独立なので並列に切り出す。診断・三角形数の上限・体積の合計は、
    // 直列処理と同じ結果になるようID順にまとめる。
    std::vector<Built> built(sites.size());
    std::vector<size_t> order(sites.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::atomic<size_t> firstFailure{sites.size()};
    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t i) {
        // 先に失敗した片より後ろは結果を使わない。前の片は診断を揃えるため最後まで処理する。
        if (i > firstFailure.load(std::memory_order_relaxed))
            return;
        const auto fail = [&](PieceError code) {
            built[i].error = code;
            size_t expected = firstFailure.load(std::memory_order_relaxed);
            while (i < expected && !firstFailure.compare_exchange_weak(expected, i)) {
            }
        };
        if (stop.stop_requested())
            return fail(PieceCancelled);
        auto poly = initial;
        for (size_t j = 0; j < sites.size(); ++j)
            if (i != j && !Clip(poly, cuts[i][j]))
                return fail(PieceOpenCut);
        Mesh result;
        std::map<uint32_t, uint32_t> remap;
        std::vector<uint8_t> origins;
        uint32_t outer = 0;
        const auto vertex = [&](uint32_t id) {
            auto [it, added] = remap.emplace(id, uint32_t(result.positions.size()));
            if (added)
                result.positions.push_back(F(origin + poly.vertices[id] * scale));
            return it->second;
        };
        for (const auto &face : poly.faces) {
            D center{};
            for (auto id : face.ids)
                center = center + poly.vertices[id];
            center = center * (1.0 / face.ids.size());
            auto c = uint32_t(result.positions.size());
            result.positions.push_back(F(origin + center * scale));
            for (size_t k = 0; k < face.ids.size(); ++k) {
                auto a = vertex(face.ids[k]), b = vertex(face.ids[(k + 1) % face.ids.size()]);
                result.triangles.push_back({c, a, b});
                origins.push_back(face.original ? 1 : 0);
            }
            outer |= face.outer;
        }
        MeshInfo info;
        if (!InspectMesh(result, info) || !info.closed || info.components != 1 ||
            info.volume <= scale * scale * scale * 1e-14)
            return fail(PieceTooSmall);
        D center{}, base = V(result.positions[0]);
        double volume = 0;
        for (auto t : result.triangles) {
            auto a = V(result.positions[t[0]]) - base, b = V(result.positions[t[1]]) - base,
                 c = V(result.positions[t[2]]) - base;
            double v = Dot(a, Cross(b, c)) / 6;
            volume += v;
            center = center + (a + b + c) * (v / 4);
        }
        auto &piece = built[i].piece;
        piece.id = uint32_t(i);
        piece.centroid = F(base + center * (1 / volume));
        piece.volume = info.volume;
        piece.outerFaces = outer;
        piece.mesh = std::make_shared<const Mesh>(std::move(result));
        piece.faceOrigins = std::make_shared<const std::vector<uint8_t>>(std::move(origins));
        built[i].volume = info.volume;
    });
    double total = 0;
    size_t triangles = 0;
    for (auto &item : built) {
        if (item.error == PieceCancelled || (item.error == PieceOk && !item.piece.mesh)) {
            error = "評価をキャンセルしました";
            return {};
        }
        if (item.error == PieceOpenCut) {
            error = "切断境界を閉じられません。Seedを変更してください";
            return {};
        }
        if (item.error == PieceTooSmall) {
            error = "微小片または精度不足の面を検出しました。Seed・寸法・伸長倍率を調整してください";
            return {};
        }
        triangles += item.piece.mesh->triangles.size();
        if (triangles > 250000) {
            error = "出力が25万三角形を超えました";
            return {};
        }
        total += item.volume;
        out.pieces.push_back(std::move(item.piece));
    }
    MeshInfo source;
    InspectMesh(mesh, source);
    if (std::abs(total - source.volume) > source.volume * 2e-5) {
        error = "分割前後の体積が一致しません";
        return {};
    }
    RefreshPieceFingerprint(out);
    return out;
}
PieceCollection FracturePieces(const PieceCollection& input, const PointSet& points, const VoronoiSettings& s,
                               int producer, std::string& error, std::stop_token stop) {
    error.clear();
    if (!points.grouped || points.source!=input.fingerprint || points.groups.size()!=input.pieces.size() ||
        input.pieces.size()>MaxScatterPoints || points.positions.size()>MaxScatterPoints) {
        error="同じPiecesから生成したScatter Pointsを接続してください（合計512点まで）";return {};
    }
    PieceCollection out; out.producer=producer;
    Hash generation; generation.Add(input.fingerprint); generation.Add(points.fingerprint);
    std::set<uint32_t> ids;
    size_t triangles=0;
    for (size_t i=0;i<input.pieces.size();++i) {
        if (stop.stop_requested()) {error="評価をキャンセルしました";return {};}
        const auto& parent=input.pieces[i]; const auto& group=points.groups[i];
        if (!parent.mesh || group.pieceId!=parent.id || parent.id>uint32_t(std::numeric_limits<int>::max()/MaxScatterPoints-1)) {
            error="親ピースのIDが不正、または再分割のID上限に達しました";return {};
        }
        PointSet local; local.source=group.source; local.fingerprint=group.fingerprint; local.positions=group.positions;
        auto children=FractureVoronoi(*parent.mesh,local,s,producer,error,stop);
        if (!error.empty()) {error="ピース "+std::to_string(parent.id)+": "+error;return {};}
        generation.Add(parent.id); generation.Add(children.generation);
        for (auto& p:children.pieces) {
            p.id=(parent.id+1)*MaxScatterPoints+p.id;
            if (!ids.insert(p.id).second) {error="ピースIDが重複しています";return {};}
            p.parentId=parent.id; p.parentProducer=input.producer; p.layer=parent.layer; p.layerSize=parent.layerSize;
            p.transform=parent.transform;
            if (p.layer>=0) {
                // 元の板の側面に面積を持って接する片だけ。上下面は対象外。
                const float tolerance=std::max(p.layerSize[0],p.layerSize[2])*1e-6f;
                for (const auto& face:p.mesh->triangles)
                    for (int axis:{0,2}) for (int sign:{-1,1}) {
                        bool on=true;
                        for (auto index:face) {
                            const auto& v=p.mesh->positions[index];
                            on &= std::abs((axis==0?v.x:v.z)-sign*p.layerSize[axis]*.5f)<=tolerance;
                        }
                        p.layerRim |= on;
                    }
            }
            triangles+=p.mesh->triangles.size();
            out.pieces.push_back(std::move(p));
            if (out.pieces.size()>MaxScatterPoints || triangles>250000) {
                error="分割結果は合計512ピース・25万三角形までです";return {};
            }
        }
    }
    out.generation=generation.value; RefreshPieceFingerprint(out); return out;
}
void RefreshPieceFingerprint(PieceCollection &c) {
    Hash h;
    h.Add(c.producer);
    h.Add(c.generation);
    h.Add(c.pieces.size());
    for (const auto &p : c.pieces) {
        h.Add(p.id);
        if (p.layer>=0) {
            h.Add(uint64_t(p.layer)); h.Add(p.parentProducer); h.Add(p.parentId); h.Add(p.layerRim);
            for (auto v:p.layerSize) h.Float(v);
        }
        for (auto v : p.transform)
            h.Add(std::bit_cast<uint64_t>(v));
    }
    c.fingerprint = h.value;
}
Vec3 PieceCenter(const Piece &p) {
    return F(Apply(p.transform, V(p.centroid)));
}
PieceSelection SelectPieces(const PieceCollection &c, const PieceSelectSettings &s, std::string &error) {
    error.clear();
    PieceSelection out{c.producer, c.generation, c.fingerprint, {}};
    if (int(s.mode) < 0 || int(s.mode) > 5 || s.layer < -1) {
        error = "選別方法が不正です";
        return {};
    }
    if (s.mode == PieceSelectMode::Region)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(s.minimum[k]) || !std::isfinite(s.maximum[k]) || s.minimum[k] > s.maximum[k]) {
                error = "選択範囲の最小・最大を確認してください";
                return {};
            }
    if (s.mode == PieceSelectMode::Volume && (!std::isfinite(s.minVolume) || !std::isfinite(s.maxVolume) ||
                                              s.minVolume < 0 || s.minVolume > s.maxVolume)) {
        error = "体積範囲が不正です";
        return {};
    }
    if ((s.mode == PieceSelectMode::Random || s.mode == PieceSelectMode::Rim) &&
        (!std::isfinite(s.fraction) || s.fraction < 0 || s.fraction > 1)) {
        error = "選択率は0〜1にしてください";
        return {};
    }
    if (s.mode == PieceSelectMode::Manual && !s.ids.empty() &&
        (s.producer != c.producer || s.generation != c.generation)) {
        error = "上流の分割が変わりました。手動選択をリセットして再選択してください";
        return {};
    }
    std::vector<int> layers;
    if (s.mode == PieceSelectMode::Rim) {
        if (s.rimLayers < 0 || s.rimLayers > 32 || s.rimSide < 0 || s.rimSide > 2 ||
            !std::isfinite(s.rimFalloff) || s.rimFalloff < 0 || s.rimFalloff > 1) {
            error = "外側の層の設定が不正です";
            return {};
        }
        for (const auto& p : c.pieces) if (p.layer >= 0) layers.push_back(p.layer);
        std::sort(layers.begin(), layers.end());
        layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
    }
    for (const auto &p : c.pieces) {
        if (s.layer>=0 && p.layer!=s.layer) continue;
        float rimWeight = 1;
        if (s.mode == PieceSelectMode::Rim && !layers.empty()) {
            if (p.layer < 0) continue;
            const int bottom = int(std::lower_bound(layers.begin(), layers.end(), p.layer)-layers.begin());
            const int top = int(layers.size())-1-bottom;
            const int depth = s.rimSide == 1 ? top : s.rimSide == 2 ? bottom : std::min(top,bottom);
            const int available = s.rimSide ? int(layers.size()) : (int(layers.size())+1)/2;
            const int count = s.rimLayers ? std::min(s.rimLayers,available) : available;
            if (depth >= count) continue; // 反転でも対象外の層は選ばない。
            rimWeight = 1-s.rimFalloff*float(depth)/float(std::max(1,count-1));
        }
        bool selected = false;
        auto center = PieceCenter(p);
        double volume = p.volume * Determinant(p.transform);
        switch (s.mode) {
        case PieceSelectMode::Manual:
            selected = Contains(s.ids, p.id);
            break;
        case PieceSelectMode::Outer:
            selected = (p.outerFaces & s.outerFaces) != 0;
            break;
        case PieceSelectMode::Region:
            selected = center.x >= s.minimum[0] && center.y >= s.minimum[1] && center.z >= s.minimum[2] &&
                       center.x <= s.maximum[0] && center.y <= s.maximum[1] && center.z <= s.maximum[2];
            break;
        case PieceSelectMode::Volume:
            selected = volume >= s.minVolume && volume <= s.maxVolume;
            break;
        case PieceSelectMode::Random: {
            Hash h;
            h.Add(c.producer);
            h.Add(c.generation);
            h.Add(p.id);
            h.Add(s.seed);
            auto state = h.value;
            selected = Uniform(state) < s.fraction;
            break;
        }
        case PieceSelectMode::Rim: {
            // 通常MeshはローカルXZの外周。層情報がある場合は元の板の側面を使う。
            Hash h; h.Add(c.producer); h.Add(p.id); h.Add(s.seed);
            auto state=h.value;
            selected=(p.layer>=0?p.layerRim:(p.outerFaces&51u)!=0) && Uniform(state)<s.fraction*rimWeight;
            break;
        }
        }
        if (selected != s.invert)
            out.ids.push_back(p.id);
    }
    return out;
}
PieceCollection FilterPieces(const PieceCollection &c, const PieceSelection &s, bool keep,
                             std::string &error) {
    error.clear();
    if (!Matches(c, s, error))
        return {};
    auto out = c;
    std::erase_if(out.pieces, [&](const auto &p) { return Contains(s.ids, p.id) != keep; });
    RefreshPieceFingerprint(out);
    return out;
}
PieceCollection TransformPieces(const PieceCollection &c, const PieceSelection *selection,
                                const PieceTransformSettings &s, std::string &error) {
    error.clear();
    if (selection && !Matches(c, *selection, error))
        return {};
    if (!s.overrides.empty() && (s.producer != c.producer || s.generation != c.generation)) {
        error = "上流の分割が変わりました。個別変換をリセットしてください";
        return {};
    }
    auto valid = [](const PiecePose &p) {
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(p.position[k]) || !std::isfinite(p.rotation[k]) ||
                !std::isfinite(p.scale[k]) || p.scale[k] <= 0)
                return false;
        return true;
    };
    if (!valid(s.pose) ||
        std::any_of(s.overrides.begin(), s.overrides.end(), [&](const auto &p) { return !valid(p.pose); })) {
        error = "位置・回転は有限値、倍率は正の有限値が必要です";
        return {};
    }
    D pivot{};
    double weight = 0;
    for (const auto &p : c.pieces)
        if (!selection || Contains(selection->ids, p.id)) {
            double v = p.volume * Determinant(p.transform);
            pivot = pivot + V(PieceCenter(p)) * v;
            weight += v;
        }
    if (weight > 0)
        pivot = pivot * (1 / weight);
    auto out = c;
    const auto apply = [](Piece &p, const PiecePose &pose, D center) {
        const auto operation = [&](D point) {
            auto a = point - center;
            a = {a.x * pose.scale[0], a.y * pose.scale[1], a.z * pose.scale[2]};
            return Rotate(a, pose.rotation) + center +
                   D{pose.position[0], pose.position[1], pose.position[2]};
        };
        auto base = operation(Apply(p.transform, {}));
        D columns[3];
        for (int k = 0; k < 3; ++k) {
            D axis{p.transform[k] * pose.scale[0], p.transform[4 + k] * pose.scale[1],
                   p.transform[8 + k] * pose.scale[2]};
            columns[k] = Rotate(axis, pose.rotation);
        }
        p.transform = {columns[0].x, columns[1].x, columns[2].x, base.x,       columns[0].y, columns[1].y,
                       columns[2].y, base.y,       columns[0].z, columns[1].z, columns[2].z, base.z};
    };
    for (auto &p : out.pieces)
        if (!selection || Contains(selection->ids, p.id)) {
            const auto center = V(PieceCenter(p));
            for (const auto &item : s.overrides)
                if (item.id == p.id)
                    apply(p, item.pose, center);
            apply(p, s.pose, s.individual ? center : pivot);
            if (!std::isfinite(Determinant(p.transform)) || Determinant(p.transform) <= 0) {
                error = "変換の倍率が表現可能な範囲を超えています";
                return {};
            }
        }
    RefreshPieceFingerprint(out);
    return out;
}
Mesh PieceMesh(const Piece &p) {
    auto mesh = *p.mesh;
    for (auto &v : mesh.positions)
        v = F(Apply(p.transform, V(v)));
    return mesh;
}
std::vector<std::array<Vec3, 2>> PieceEdges(const Piece &p) {
    std::vector<std::array<Vec3, 2>> edges;
    if (!p.mesh)
        return edges;
    const Mesh mesh = PieceMesh(p);
    const auto *origins = p.faceOrigins && p.faceOrigins->size() == mesh.triangles.size() ? p.faceOrigins.get() : nullptr;
    // 辺（小さい頂点番号, 大きい頂点番号）ごとに、接する面を集める。
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> faces;
    for (uint32_t f = 0; f < mesh.triangles.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = mesh.triangles[f][k], b = mesh.triangles[f][(k + 1) % 3];
            faces[{std::min(a, b), std::max(a, b)}].push_back(f);
        }
    for (const auto &[edge, adjacent] : faces) {
        bool draw = adjacent.size() != 2;
        if (!draw) {
            const uint32_t f0 = adjacent[0], f1 = adjacent[1];
            const bool cut0 = !origins || (*origins)[f0] == 0, cut1 = !origins || (*origins)[f1] == 0;
            const Vec3 n0 = FaceNormal(mesh, mesh.triangles[f0]), n1 = FaceNormal(mesh, mesh.triangles[f1]);
            const float cosine = n0.x * n1.x + n0.y * n1.y + n0.z * n1.z;
            if (!cut0 && !cut1)
                // 元の外面どうしは、はっきり折れた辺（20度より大きい。箱の角など）だけ出す。
                // 丸い形の細かい三角形の辺は出さない。
                draw = cosine < .94f;
            else
                // 同じ平面の三角形の間（切断面の分割の対角線）は出さない。
                draw = cut0 != cut1 || cosine < .9999f;
        }
        if (draw)
            edges.push_back({mesh.positions[edge.first], mesh.positions[edge.second]});
    }
    return edges;
}
Mesh PiecesMesh(const PieceCollection &c) {
    Mesh out;
    for (const auto &p : c.pieces) {
        auto mesh = PieceMesh(p);
        auto offset = uint32_t(out.positions.size());
        out.positions.insert(out.positions.end(), mesh.positions.begin(), mesh.positions.end());
        for (auto t : mesh.triangles) {
            for (auto &i : t)
                i += offset;
            out.triangles.push_back(t);
        }
    }
    return out;
}
} // namespace rock::geometry
