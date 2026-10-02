#include "geometry/Pieces.h"
#include "geometry/Volume.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <execution>
#include <map>
#include <numbers>
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
// 0～1 のなめらかな値ノイズ（整数格子のハッシュを三線形にならす）。点の密度のむらに使う。
double SmoothNoise(D p, uint64_t seed) {
    const auto hash = [&](int64_t x, int64_t y, int64_t z) {
        uint64_t h = seed ^ (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(y) * 0xC2B2AE3D27D4EB4Full) ^
                     (uint64_t(z) * 0x165667B19E3779F9ull);
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
        return double((h ^ (h >> 31)) >> 11) * 0x1.0p-53;
    };
    const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const auto smooth = [](double t) { return t * t * (3 - 2 * t); };
    const double tx = smooth(p.x - fx), ty = smooth(p.y - fy), tz = smooth(p.z - fz);
    const int64_t x = int64_t(fx), y = int64_t(fy), z = int64_t(fz);
    const auto mix = [](double a, double b, double t) { return a + (b - a) * t; };
    return mix(mix(mix(hash(x, y, z), hash(x + 1, y, z), tx), mix(hash(x, y + 1, z), hash(x + 1, y + 1, z), tx), ty),
               mix(mix(hash(x, y, z + 1), hash(x + 1, y, z + 1), tx), mix(hash(x, y + 1, z + 1), hash(x + 1, y + 1, z + 1), tx), ty), tz);
}
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
    int neighbor = -1;
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
// 吸着（板 ∩ Voronoi）の板の面。隣は複数の片なので、面の多角形を取っておいて後で交差面積から隣接を作る。
constexpr int kSlabLower = -2, kSlabUpper = -3;
struct SlabFace {
    bool upper = false;
    std::vector<D> vertices;  // 局所の正規化座標
    int plane = -1; // 束で止まる節理用。-4 以下の neighbor に面番号と表裏を符号化する。
};
struct Built {
    Piece piece;
    double volume = 0;
    PieceError error = PieceOk;
    std::vector<SlabFace> slabFaces;
};
struct Flat {
    size_t site;
    std::vector<std::array<double, 2>> points;
    std::array<double, 2> low{}, high{};
};
Flat FlattenFace(size_t site, const SlabFace &face, D normal) {
    D u = Cross(std::abs(normal.y) < .9 ? D{0, 1, 0} : D{1, 0, 0}, normal);
    u = u * (1 / Length(u));
    const D v = Cross(normal, u);
    Flat flat{site, {}, {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()},
              {-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()}};
    for (const auto &p : face.vertices) {
        const std::array<double, 2> q{Dot(p, u), Dot(p, v)};
        flat.points.push_back(q);
        for (int k = 0; k < 2; ++k) {
            flat.low[k] = std::min(flat.low[k], q[k]);
            flat.high[k] = std::max(flat.high[k], q[k]);
        }
    }
    double signedArea = 0;
    for (size_t k = 0; k < flat.points.size(); ++k) {
        const auto &a = flat.points[k], &b = flat.points[(k + 1) % flat.points.size()];
        signedArea += a[0] * b[1] - a[1] * b[0];
    }
    if (signedArea < 0) std::reverse(flat.points.begin(), flat.points.end());
    return flat;
}
double FaceOverlap(const Flat &a, const Flat &b) {
    for (int k = 0; k < 2; ++k)
        if (a.high[k] <= b.low[k] || b.high[k] <= a.low[k]) return 0.0;
    std::vector<std::array<double, 2>> poly = a.points, next;
    for (size_t e = 0; e < b.points.size() && !poly.empty(); ++e) {
        const auto &p = b.points[e], &q = b.points[(e + 1) % b.points.size()];
        const auto side = [&](const std::array<double, 2> &x) { return (q[0] - p[0]) * (x[1] - p[1]) - (q[1] - p[1]) * (x[0] - p[0]); };
        next.clear();
        for (size_t k = 0; k < poly.size(); ++k) {
            const auto &cur = poly[k], &prev = poly[(k + poly.size() - 1) % poly.size()];
            const double sc = side(cur), sp = side(prev);
            if (sc >= 0) {
                if (sp < 0) {
                    const double t = sp / (sp - sc);
                    next.push_back({prev[0] + (cur[0] - prev[0]) * t, prev[1] + (cur[1] - prev[1]) * t});
                }
                next.push_back(cur);
            } else if (sp >= 0) {
                const double t = sp / (sp - sc);
                next.push_back({prev[0] + (cur[0] - prev[0]) * t, prev[1] + (cur[1] - prev[1]) * t});
            }
        }
        poly.swap(next);
    }
    double area = 0;
    for (size_t k = 0; k < poly.size(); ++k) {
        const auto &x = poly[k], &y = poly[(k + 1) % poly.size()];
        area += x[0] * y[1] - x[1] * y[0];
    }
    return std::abs(area) * .5;
}

// 切り出した凸多面体 1 つからピースを作る（メッシュ・隣接面の面積・体積・重心）。
// 面の neighbor は隣のピースの番号（Voronoi では点の番号、構造面では仮のセル番号）。
void BuildPiece(const Poly &poly, D origin, double scale, uint32_t pieceId, Built &out) {
    Mesh result;
    std::map<uint32_t, uint32_t> remap;
    std::vector<uint8_t> origins;
    uint32_t outer = 0;
    auto neighborhood = std::make_shared<PieceNeighborhood>();
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
        D area{};
        for (size_t k=0;k<face.ids.size();++k)
            area = area + Cross(poly.vertices[face.ids[k]]-center,
                                poly.vertices[face.ids[(k+1)%face.ids.size()]]-center)*(.5*scale*scale);
        if (Length(area) > scale*scale*1e-12) {
            const std::array<double,3> vector{area.x,area.y,area.z};
            if (face.neighbor >= 0) neighborhood->contacts.push_back({uint32_t(face.neighbor),vector});
            else if (face.neighbor <= kSlabLower) {
                SlabFace slab;
                slab.upper = face.neighbor == kSlabUpper;
                if (face.neighbor <= -4) {
                    slab.plane = (-4 - face.neighbor) / 2;
                    slab.upper = (-4 - face.neighbor) % 2 == 0;
                }
                for (auto id : face.ids) slab.vertices.push_back(poly.vertices[id]);
                out.slabFaces.push_back(std::move(slab));
            } else neighborhood->boundary.push_back(vector);
        }
        outer |= face.outer;
    }
    MeshInfo info;
    if (!InspectMesh(result, info) || !info.closed || info.components != 1 ||
        info.volume <= scale * scale * scale * 1e-14) {
        out.error = PieceTooSmall;
        return;
    }
    D center{}, base = V(result.positions[0]);
    double volume = 0;
    for (auto t : result.triangles) {
        auto a = V(result.positions[t[0]]) - base, b = V(result.positions[t[1]]) - base,
             c = V(result.positions[t[2]]) - base;
        double v = Dot(a, Cross(b, c)) / 6;
        volume += v;
        center = center + (a + b + c) * (v / 4);
    }
    auto &piece = out.piece;
    piece.id = pieceId;
    piece.neighborhood = std::move(neighborhood);
    piece.centroid = F(base + center * (1 / volume));
    piece.volume = info.volume;
    piece.outerFaces = outer;
    piece.mesh = std::make_shared<const Mesh>(std::move(result));
    piece.faceOrigins = std::make_shared<const std::vector<uint8_t>>(std::move(origins));
    out.volume = info.volume;
}
// 並列に切り出した片をID順にまとめる。診断・三角形数の上限・分割前後の体積の一致を確かめ、
// 両側の切断計算に由来する隣接面の面積の微小な差を揃える。
bool CollectPieces(std::vector<Built> &built, const Mesh &mesh, PieceCollection &out, std::string &error) {
    double total = 0;
    size_t triangles = 0;
    for (auto &item : built) {
        if (item.error == PieceCancelled || (item.error == PieceOk && !item.piece.mesh)) {
            error = "評価をキャンセルしました";
            return false;
        }
        if (item.error == PieceOpenCut) {
            error = "切断境界を閉じられません。Seedを変更してください";
            return false;
        }
        if (item.error == PieceTooSmall) {
            error = "微小片または精度不足の面を検出しました。Seed・寸法・伸長倍率を調整してください";
            return false;
        }
        triangles += item.piece.mesh->triangles.size();
        if (triangles > 250000) {
            error = "出力が25万三角形を超えました";
            return false;
        }
        total += item.volume;
        out.pieces.push_back(std::move(item.piece));
    }
    MeshInfo source;
    InspectMesh(mesh, source);
    if (std::abs(total - source.volume) > source.volume * 2e-5) {
        error = "分割前後の体積が一致しません";
        return false;
    }
    // 両側の切断計算に由来する微小な差を揃え、面積ゼロの接触を除く。
    std::vector<std::shared_ptr<PieceNeighborhood>> neighborhoods;
    for (const auto& p : out.pieces) neighborhoods.push_back(std::make_shared<PieceNeighborhood>(*p.neighborhood));
    for (size_t i=0;i<neighborhoods.size();++i) {
        auto& contacts=neighborhoods[i]->contacts;
        std::erase_if(contacts,[&](const auto& contact) {
            const auto& other=neighborhoods[contact.neighbor]->contacts;
            return std::none_of(other.begin(),other.end(),[&](const auto& back){return back.neighbor==i;});
        });
        for (auto& contact : contacts) if (contact.neighbor>i) {
            auto& other=neighborhoods[contact.neighbor]->contacts;
            auto back=std::find_if(other.begin(),other.end(),[&](const auto& v){return v.neighbor==i;});
            for (int k=0;k<3;++k) {
                const double area=(contact.areaVector[k]-back->areaVector[k])*.5;
                contact.areaVector[k]=area; back->areaVector[k]=-area;
            }
        }
    }
    for (size_t i=0;i<out.pieces.size();++i) out.pieces[i].neighborhood=neighborhoods[i];
    return true;
}
// 同じ辺の交点を一度だけ生成し、切断面は境界辺の逆向きの閉路から作る。
bool Clip(Poly &poly, Plane plane, int neighbor) {
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
    // 距離と同じく、辺の表も呼び出しごとに確保し直さない。
    thread_local std::map<std::pair<uint32_t, uint32_t>, uint32_t> intersections;
    intersections.clear();
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
        Face out{{}, face.outer, face.original, face.neighbor};
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
    thread_local std::map<std::pair<uint32_t, uint32_t>, int> edges;
    edges.clear();
    for (const auto &f : faces)
        for (size_t i = 0; i < f.ids.size(); ++i) {
            auto a = f.ids[i], b = f.ids[(i + 1) % f.ids.size()];
            auto opposite = edges.find({b, a});
            if (opposite != edges.end())
                edges.erase(opposite);
            else
                edges[{a, b}] = 1;
        }
    thread_local std::map<uint32_t, uint32_t> next;
    next.clear();
    for (const auto &[e, count] : edges)
        if (!next.emplace(e.second, e.first).second)
            return false;
    if (next.size() < 3)
        return false;
    Face cap{{}, 0, false, neighbor};
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
// 面から参照されなくなった頂点を詰める。Clip は毎回全頂点を調べるので、
// 切り落とした側の頂点（入力の形の頂点を含む）を持ち越さない。
void Compact(Poly &poly) {
    std::vector<uint32_t> remap(poly.vertices.size(), std::numeric_limits<uint32_t>::max());
    std::vector<D> kept;
    for (auto &face : poly.faces)
        for (auto &id : face.ids) {
            if (remap[id] == std::numeric_limits<uint32_t>::max()) {
                remap[id] = uint32_t(kept.size());
                kept.push_back(poly.vertices[id]);
            }
            id = remap[id];
        }
    poly.vertices = std::move(kept);
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
        std::any_of(s.rotation.begin(),s.rotation.end(),[](float v){return !std::isfinite(v);}) ||
        std::any_of(s.position.begin(),s.position.end(),[](float v){return !std::isfinite(v) || std::abs(v)>100000;})) {
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
    for (auto v:s.position) hash.Float(v);
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
        auto position=Rotate(centers[i],rotation);
        position.x+=s.position[0]; position.y+=s.position[1]; position.z+=s.position[2];
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
    if (!std::isfinite(s.clustering) || s.clustering < 0 || s.clustering > 1 || !std::isfinite(s.clusterScale) ||
        s.clusterScale < .5f || s.clusterScale > 16) {
        error = "密度のむらは 0～1、むらの細かさは 0.5～16 にしてください";
        return {};
    }
    if (!std::isfinite(s.heightGradient) || s.heightGradient < -1 || s.heightGradient > 1) {
        error = "高さの勾配は -1～1 にしてください";
        return {};
    }
    // むらの間引きは別の乱数で行い、むら 0 では従来と同じ点の並びにする。
    uint64_t thinning = uint64_t(s.seed) ^ 0x6C75737465720000ull;
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
        if (s.clustering > 0 || s.heightGradient != 0) {
            // 受け入れる確率: むらの低い所ほど下げる（ノイズの 2 乗で、疎な所をはっきり疎にする）。
            double keep = 1;
            if (s.clustering > 0) {
                const double n = SmoothNoise(p * double(s.clusterScale), uint64_t(s.seed) * 0x2545F4914F6CDD1Dull);
                keep = (1 - s.clustering) + s.clustering * n * n;
            }
            // 高さの勾配: 密にしない側ほど受け入れる確率を下げる。
            if (s.heightGradient != 0 && hi.y > lo.y) {
                const double t = std::clamp((p.y - lo.y) / (hi.y - lo.y), 0.0, 1.0);
                keep *= s.heightGradient > 0 ? 1 - s.heightGradient * (1 - t) : 1 + s.heightGradient * t;
            }
            if (Uniform(thinning) > keep)
                continue;
        }
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
    if (s.clustering > 0) {
        h.Float(s.clustering);
        h.Float(s.clusterScale);
    }
    if (s.heightGradient != 0)
        h.Float(s.heightGradient);
    out.fingerprint = h.value;
    return out;
}
PointSet ScatterPiecePoints(const PieceCollection& pieces, const ScatterSettings& s,
                            std::string& error, std::stop_token stop) {
    error.clear();
    if (s.version!=1 || s.count<2 || s.count>PieceIdStride || pieces.pieces.size()>size_t(MaxScatterPoints/s.count)) {
        error="各ピースの点数は2〜"+std::to_string(PieceIdStride)+"、全体の合計は"+std::to_string(MaxScatterPoints)+"点までです";
        return {};
    }
    PointSet out; out.grouped=true; out.source=pieces.fingerprint;
    Hash hash; hash.Add(out.source); hash.Add(s.count); hash.Add(s.seed); hash.Add(s.planar);
    if (s.clustering > 0) { hash.Float(s.clustering); hash.Float(s.clusterScale); }
    if (s.heightGradient != 0) hash.Float(s.heightGradient);
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
                                int producer, std::string &error, std::stop_token stop, const std::vector<StructurePlanes> *snap) {
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
    std::vector<D> sites, locals;
    for (auto p : points.positions) {
        D local = (V(p) - origin) * (1 / scale);
        locals.push_back(local);
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
    // 吸着（板 ∩ Voronoi）: 最初の系統の構造面（局所の正規化座標）で形を板に分け、各点を板に振り分ける。
    // ワールドの面 n·x = offset は、x = origin + scale·q で n·q = (offset − n·origin) / scale。
    // 点の無い板ができないように、面は「その面より下（直前に残した面より上）に点がある」ものだけ残す。
    // こうすると板ごとに点の Voronoi が板全体を覆い、隙間も重なりもできない（片の集まりは元の形の分割）。
    const bool snapping = snap && !snap->empty() && s.snap;
    D slabNormal{0, 1, 0};
    std::vector<double> slabPlanes;        // 残した面の位置（昇順）
    std::vector<int> slabOf(sites.size(), 0);
    if (snapping) {
        MeshInfo info;
        InspectMesh(mesh, info);
        const auto &set = snap->front();
        std::string expandError;
        const auto expanded = ExpandParallelPlanes(set, info.minimum, info.maximum, expandError);
        if (!expandError.empty()) {
            error = expandError;
            return {};
        }
        D normal = V(set.normal);
        const auto clean = [](double v) { return std::abs(v) < 1e-6 ? 0.0 : v; };
        normal = {clean(normal.x), clean(normal.y), clean(normal.z)};
        const double length = Length(normal);
        if (!(length > 0)) {
            error = "吸着する構造面の向きが不正です";
            return {};
        }
        slabNormal = normal * (1 / length);
        std::vector<double> candidates;
        for (const auto &plane : expanded) candidates.push_back((double(plane.offset) - Dot(slabNormal, origin)) / scale);
        std::sort(candidates.begin(), candidates.end());
        std::vector<double> offsets;
        for (const auto &q : locals) offsets.push_back(Dot(slabNormal, q));
        std::sort(offsets.begin(), offsets.end());
        double last = -std::numeric_limits<double>::infinity();
        for (double d : candidates) {
            // 直前に残した面以上、この面より下に点があれば残す（点が面の上にあれば上の板に属する）。
            const auto lower = std::lower_bound(offsets.begin(), offsets.end(), last);
            if (lower != offsets.end() && *lower < d) {
                slabPlanes.push_back(d);
                last = d;
            }
        }
        while (!slabPlanes.empty() && offsets.back() < slabPlanes.back()) slabPlanes.pop_back();
        for (size_t i = 0; i < sites.size(); ++i)
            slabOf[i] = int(std::upper_bound(slabPlanes.begin(), slabPlanes.end(), Dot(slabNormal, locals[i])) - slabPlanes.begin());
    }
    // i の片を j 側から切る二等分面。点数の2乗の表は持たず、使う面だけその場で作る。
    // 差と符号の反転は浮動小数でも厳密なので、cut(j, i) は cut(i, j) の裏返しと一致する。
    const auto cut = [&](size_t i, size_t j) {
        auto n = (sites[j] - sites[i]) * 2;
        double d = Dot(sites[j], sites[j]) - Dot(sites[i], sites[i]);
        n = Rotate({n.x / s.stretch[0], n.y / s.stretch[1], n.z / s.stretch[2]}, s.rotation);
        double len = Length(n);
        return Plane{n * (1 / len), d / len};
    };
    // 片の頂点を、点を置いた空間（回転を戻して伸長で割った空間）へ写す。
    const auto toSite = [&](D p) {
        auto q = Rotate(p, s.rotation, true);
        return D{q.x / s.stretch[0], q.y / s.stretch[1], q.z / s.stretch[2]};
    };
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
    if (snapping) {
        hash.Add(1);
        hash.Float(float(slabNormal.x)); hash.Float(float(slabNormal.y)); hash.Float(float(slabNormal.z));
        for (double d : slabPlanes) hash.Float(float(d));
    }
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
        // 吸着: 先に自分の板の上下の面で切る（隣は複数の片なので面の多角形だけ取っておく）。
        if (snapping) {
            const int slab = slabOf[i];
            if (slab > 0 && !Clip(poly, Plane{{-slabNormal.x, -slabNormal.y, -slabNormal.z}, -slabPlanes[size_t(slab) - 1]}, kSlabLower))
                return fail(PieceOpenCut);
            if (size_t(slab) < slabPlanes.size() && !Clip(poly, Plane{slabNormal, slabPlanes[size_t(slab)]}, kSlabUpper))
                return fail(PieceOpenCut);
            Compact(poly);
        }
        // 近い点から順に切り、片の半径 R の2倍より遠い点で打ち切る。
        // 距離 L > 2R の点との二等分面は、片のどの頂点からも i のほうが近いので切らない。
        // これで片ごとの切断は全点ではなく近傍の点の数で済む。
        thread_local std::vector<std::pair<double, uint32_t>> nearby;
        nearby.clear();
        for (size_t j = 0; j < sites.size(); ++j)
            if (j != i) {
                const D delta = sites[j] - sites[i];
                nearby.push_back({Dot(delta, delta), uint32_t(j)});
            }
        std::sort(nearby.begin(), nearby.end());
        const auto radius = [&] {
            double r = 0;
            for (auto p : poly.vertices) {
                const D delta = toSite(p) - sites[i];
                r = std::max(r, Dot(delta, delta));
            }
            return std::sqrt(r);
        };
        double reach = 2 * radius();
        for (auto [distance, j] : nearby) {
            if (std::sqrt(distance) > reach * (1 + 1e-6) + 1e-9)
                break;
            // 吸着: 別の板の点とは板の面で既に分かれている。
            if (snapping && slabOf[j] != slabOf[i])
                continue;
            const auto vertices = poly.vertices.size();
            const auto faces = poly.faces.size();
            if (!Clip(poly, cut(i, j), int(j)))
                return fail(PieceOpenCut);
            // 切れたときだけ半径を測り直す。測り直さなくても半径は大きめに残るだけで結果は変わらない。
            if (poly.vertices.size() != vertices || poly.faces.size() != faces) {
                Compact(poly);
                reach = 2 * radius();
            }
        }
        BuildPiece(poly, origin, scale, uint32_t(i), built[i]);
    });
    // 吸着: 板の面を挟んで隣り合う片の隣接を、面の多角形の交差面積から作る（面は複数の片と接する）。
    if (snapping) {
        // 面ごとに、下の板の上面と上の板の下面を集める。
        std::vector<std::vector<Flat>> lowerSide(slabPlanes.size() + 1), upperSide(slabPlanes.size() + 1);
        for (size_t i = 0; i < built.size(); ++i) {
            if (built[i].error != PieceOk || !built[i].piece.mesh) continue;
            for (const auto &face : built[i].slabFaces) {
                const int slab = slabOf[i];
                if (face.upper && size_t(slab) < slabPlanes.size()) lowerSide[size_t(slab)].push_back(FlattenFace(i, face, slabNormal));
                if (!face.upper && slab > 0) upperSide[size_t(slab) - 1].push_back(FlattenFace(i, face, slabNormal));
            }
        }
        std::vector<std::vector<PieceContact>> extra(built.size());
        for (size_t k = 0; k < slabPlanes.size(); ++k)
            for (const auto &below : lowerSide[k])
                for (const auto &above : upperSide[k]) {
                    const double area = FaceOverlap(below, above) * scale * scale;
                    if (area <= scale * scale * 1e-12) continue;
                    extra[below.site].push_back({uint32_t(above.site), {slabNormal.x * area, slabNormal.y * area, slabNormal.z * area}});
                    extra[above.site].push_back({uint32_t(below.site), {-slabNormal.x * area, -slabNormal.y * area, -slabNormal.z * area}});
                }
        for (size_t i = 0; i < built.size(); ++i) {
            if (extra[i].empty() || !built[i].piece.neighborhood) continue;
            auto neighborhood = std::make_shared<PieceNeighborhood>(*built[i].piece.neighborhood);
            neighborhood->contacts.insert(neighborhood->contacts.end(), extra[i].begin(), extra[i].end());
            built[i].piece.neighborhood = std::move(neighborhood);
        }
    }
    if (!CollectPieces(built, mesh, out, error))
        return {};
    out.adjacencyComplete=true;
    RefreshPieceFingerprint(out);
    return out;
}
// 構造面（節理・層理）の系統で割る。系統ごとに隣り合う面のあいだの板（スラブ）を作り、全系統の板の重なりを
// 1 つのピースにする（セル）。隣のセルは系統の板の番号が 1 つだけ違うもの。ピースは凸のまま。
PieceCollection FracturePlanes(const Mesh &mesh, const std::vector<StructurePlanes> &sets, int producer,
                               std::string &error, std::stop_token stop) {
    error.clear();
    if (sets.empty()) {
        error = "構造面を接続してください";
        return {};
    }
    Poly initial;
    std::vector<Plane> sourcePlanes;
    D origin;
    double scale;
    if (!Source(mesh, initial, sourcePlanes, origin, scale, error, stop))
        return {};
    MeshInfo info;
    InspectMesh(mesh, info);
    // 系統ごとに、形の範囲を通る面の位置（法線方向）。
    struct Slabs {
        D normal;
        std::vector<double> offsets;
    };
    std::vector<Slabs> slabs;
    size_t cells = 1;
    for (const auto &set : sets) {
        std::string expandError;
        const auto planes = ExpandParallelPlanes(set, info.minimum, info.maximum, expandError);
        if (!expandError.empty()) {
            error = expandError;
            return {};
        }
        // 回転の単精度の誤差（cos 90° ≈ −4e−8）を法線から除く。誤差が残ると、別の系統の面との交点がわずかにずれて
        // 長さ 1e−9 ほどの辺ができ、閉じた形として扱えない片が出る（間隔 0.25 m の縦の面で起きた）。
        D normal = V(set.normal);
        const auto clean = [](double v) { return std::abs(v) < 1e-6 ? 0.0 : v; };
        normal = {clean(normal.x), clean(normal.y), clean(normal.z)};
        normal = normal * (1 / Length(normal));
        Slabs item{normal, {}};
        // 形の端に（ほぼ）重なる面は割らない。単精度の向きでは形の面とわずかにずれ、紙のように薄い切れ端ができる。
        double low = std::numeric_limits<double>::max(), high = -low;
        for (const auto &p : mesh.positions) {
            low = std::min(low, Dot(item.normal, V(p)));
            high = std::max(high, Dot(item.normal, V(p)));
        }
        const double tolerance = scale * 1e-5;
        for (const auto &plane : planes)
            if (plane.offset > low + tolerance && plane.offset < high - tolerance)
                item.offsets.push_back(plane.offset);
        std::sort(item.offsets.begin(), item.offsets.end());
        cells *= item.offsets.size() + 1;
        if (cells > 65536) {
            error = "構造面で割るセルが多すぎます。平行面の間隔を広げてください";
            return {};
        }
        slabs.push_back(std::move(item));
    }
    std::vector<size_t> stride(slabs.size(), 1);
    for (size_t k = 1; k < slabs.size(); ++k)
        stride[k] = stride[k - 1] * (slabs[k - 1].offsets.size() + 1);
    // 面 n·p = o を正規化した座標（Source と同じ q = (p − origin) / scale）へ写す。
    const auto local = [&](D n, double offset) { return (offset - Dot(n, origin)) / scale; };
    std::vector<Built> built(cells);
    std::vector<uint8_t> present(cells, 0);
    std::vector<size_t> order(cells);
    std::iota(order.begin(), order.end(), size_t(0));
    std::atomic<bool> cancelled{false};
    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t cell) {
        if (cancelled.load(std::memory_order_relaxed) || stop.stop_requested()) {
            cancelled.store(true, std::memory_order_relaxed);
            return;
        }
        auto poly = initial;
        for (size_t k = 0; k < slabs.size(); ++k) {
            const size_t index = (cell / stride[k]) % (slabs[k].offsets.size() + 1);
            const D n = slabs[k].normal;
            // 上の面: n·p ≤ offsets[index]。下の面: n·p ≥ offsets[index − 1]。切り口の隣は板の番号 ±1 のセル。
            if (index < slabs[k].offsets.size() &&
                !Clip(poly, Plane{n, local(n, slabs[k].offsets[index])}, int(cell + stride[k]))) {
                built[cell].error = PieceOpenCut;
                return;
            }
            if (index > 0 && !Clip(poly, Plane{n * -1, -local(n, slabs[k].offsets[index - 1])}, int(cell - stride[k]))) {
                built[cell].error = PieceOpenCut;
                return;
            }
            if (poly.faces.empty())
                return;
            Compact(poly);
        }
        if (poly.faces.empty())
            return;
        BuildPiece(poly, origin, scale, uint32_t(cell), built[cell]);
        // 面や頂点にかすっただけの薄すぎるセルは捨てる（体積が残れば、分割前後の体積の比較で診断される）。
        if (built[cell].error == PieceTooSmall)
            built[cell] = {};
        else
            present[cell] = 1;
    });
    if (cancelled.load()) {
        error = "評価をキャンセルしました";
        return {};
    }
    // 空のセルを詰め、ピースの番号を 0 から振り直す。隣接面の相手も新しい番号へ写す。
    std::vector<int64_t> remap(cells, -1);
    std::vector<Built> kept;
    for (size_t cell = 0; cell < cells; ++cell)
        if (present[cell] || built[cell].error != PieceOk) {
            remap[cell] = int64_t(kept.size());
            kept.push_back(std::move(built[cell]));
        }
    if (kept.size() > size_t(MaxScatterPoints)) {
        error = "構造面で割ったピースは" + std::to_string(MaxScatterPoints) + "個までです。平行面の間隔を広げてください";
        return {};
    }
    for (size_t i = 0; i < kept.size(); ++i) {
        auto &item = kept[i];
        if (!item.piece.mesh)
            continue;
        item.piece.id = uint32_t(i);
        auto neighborhood = std::make_shared<PieceNeighborhood>(*item.piece.neighborhood);
        std::erase_if(neighborhood->contacts, [&](const auto &contact) {
            return contact.neighbor >= cells || remap[contact.neighbor] < 0;
        });
        for (auto &contact : neighborhood->contacts)
            contact.neighbor = uint32_t(remap[contact.neighbor]);
        item.piece.neighborhood = std::move(neighborhood);
    }
    PieceCollection out;
    out.producer = producer;
    Hash hash;
    hash.Add(MeshFingerprint(mesh));
    for (const auto &set : sets) {
        hash.Float(set.normal.x);
        hash.Float(set.normal.y);
        hash.Float(set.normal.z);
        hash.Float(set.spacing);
        hash.Float(set.offset);
        hash.Float(set.variation);
        hash.Add(uint64_t(uint32_t(set.seed)));
    }
    out.generation = hash.value;
    if (!CollectPieces(kept, mesh, out, error))
        return {};
    out.adjacencyComplete = true;
    RefreshPieceFingerprint(out);
    return out;
}
// 第1系統の板を束ね、その束の中だけで他の系統を切る。各切断の両側を同じ面から作るので、
// T字の止まりでも空隙・重複を作らない。DFN の亀裂成長や未破断部のモデルではない。
PieceCollection FractureJointGroups(const Mesh& mesh, const std::vector<StructurePlanes>& sets, int span,
                                    int producer, std::string& error, std::stop_token stop) {
    error.clear();
    if (span < 0 || span > 16) {
        error = "節理の連続枚数は 0〜16 にしてください";
        return {};
    }
    if (span == 0 || sets.size() < 2)
        return FracturePlanes(mesh, sets, producer, error, stop);
    Poly initial;
    std::vector<Plane> sourcePlanes;
    D origin;
    double scale;
    if (!Source(mesh, initial, sourcePlanes, origin, scale, error, stop)) return {};
    MeshInfo info;
    InspectMesh(mesh, info);
    struct Cell { Poly poly; size_t slab = 0; };
    std::vector<Cell> cells{{std::move(initial), 0}};
    std::vector<D> normals;
    // 元形状を覆う切断面を展開する。束ごとの違いは位置と間隔だけで、面の向きは変えない。
    const auto cuts = [&](StructurePlanes set, size_t system, size_t group) {
        if (system > 0) {
            uint64_t random = uint64_t(uint32_t(set.seed)) ^ (uint64_t(group + 1) * 0x9e3779b97f4a7c15ull)
                              ^ (uint64_t(system) * 0xc2b2ae3d27d4eb4full);
            set.offset += float((Uniform(random) - .5) * set.spacing);
            set.seed = int(Random(random) & 0x7fffffff);
        }
        const auto expanded = ExpandParallelPlanes(set, info.minimum, info.maximum, error);
        std::vector<Plane> result;
        if (!error.empty()) return result;
        D n = V(set.normal);
        const auto clean = [](double x) { return std::abs(x) < 1e-6 ? 0. : x; };
        n = {clean(n.x), clean(n.y), clean(n.z)};
        n = n * (1 / Length(n));
        for (const auto& plane : expanded)
            result.push_back({n, (plane.offset - Dot(n, origin)) / scale});
        return result;
    };
    const auto split = [&](Plane plane, size_t group, bool primary) {
        const int planeId = int(normals.size());
        normals.push_back(plane.n);
        const size_t count = cells.size();
        for (size_t i = 0; i < count; ++i) {
            if (stop.stop_requested()) { error = "評価をキャンセルしました"; return false; }
            if (!primary && cells[i].slab / size_t(span) != group) continue;
            double low = std::numeric_limits<double>::max(), high = -low;
            for (const auto& p : cells[i].poly.vertices) {
                const double d = Dot(plane.n, p) - plane.d;
                low = std::min(low, d); high = std::max(high, d);
            }
            // 接するだけの面では割らない。片を捨てず元の領域を残す。
            if (low >= -1e-7 || high <= 1e-7) continue;
            if (cells.size() >= size_t(MaxScatterPoints)) {
                error = "節理で割ったピースは1024個までです。平行面の間隔を広げてください";
                return false;
            }
            Cell above = cells[i];
            if (!Clip(cells[i].poly, plane, -4 - 2 * planeId) ||
                !Clip(above.poly, {plane.n * -1, -plane.d}, -5 - 2 * planeId)) {
                error = "節理の切断境界を閉じられません";
                return false;
            }
            Compact(cells[i].poly); Compact(above.poly);
            cells.push_back(std::move(above));
        }
        return true;
    };
    const auto primary = cuts(sets.front(), 0, 0);
    if (!error.empty()) return {};
    for (const auto& plane : primary) if (!split(plane, 0, true)) return {};
    const D axis = V(sets.front().normal);
    const auto center = [&](const Cell& cell) {
        double sum = 0;
        for (const auto& p : cell.poly.vertices) sum += Dot(axis, p);
        return sum / cell.poly.vertices.size();
    };
    std::sort(cells.begin(), cells.end(), [&](const Cell& a, const Cell& b) { return center(a) < center(b); });
    const size_t groups = (cells.size() + size_t(span) - 1) / size_t(span);
    for (size_t i = 0; i < cells.size(); ++i) cells[i].slab = i;
    for (size_t system = 1; system < sets.size(); ++system)
        for (size_t group = 0; group < groups; ++group) {
            const auto planes = cuts(sets[system], system, group);
            if (!error.empty()) return {};
            for (const auto& plane : planes) if (!split(plane, group, false)) return {};
        }
    std::vector<Built> built(cells.size());
    std::vector<std::vector<Flat>> below(normals.size()), above(normals.size());
    for (size_t i = 0; i < cells.size(); ++i) {
        if (stop.stop_requested()) { error = "評価をキャンセルしました"; return {}; }
        BuildPiece(cells[i].poly, origin, scale, uint32_t(i), built[i]);
        for (const auto& face : built[i].slabFaces) {
            const size_t plane = size_t(face.plane);
            (face.upper ? below[plane] : above[plane]).push_back(FlattenFace(i, face, normals[plane]));
        }
    }
    std::vector<std::vector<PieceContact>> contacts(built.size());
    for (size_t plane = 0; plane < normals.size(); ++plane) {
        if (stop.stop_requested()) { error = "評価をキャンセルしました"; return {}; }
        const D n = normals[plane];
        for (const auto& a : below[plane])
            for (const auto& b : above[plane]) {
                const double area = FaceOverlap(a, b) * scale * scale;
                if (area <= scale * scale * 1e-12) continue;
                contacts[a.site].push_back({uint32_t(b.site), {n.x * area, n.y * area, n.z * area}});
                contacts[b.site].push_back({uint32_t(a.site), {-n.x * area, -n.y * area, -n.z * area}});
            }
    }
    for (size_t i = 0; i < built.size(); ++i) {
        if (!built[i].piece.neighborhood) continue;
        auto neighborhood = std::make_shared<PieceNeighborhood>(*built[i].piece.neighborhood);
        neighborhood->contacts = std::move(contacts[i]);
        built[i].piece.neighborhood = std::move(neighborhood);
    }
    PieceCollection out;
    out.producer = producer;
    Hash hash;
    hash.Add(MeshFingerprint(mesh)); hash.Add(uint64_t(span));
    for (const auto& set : sets) {
        hash.Float(set.normal.x); hash.Float(set.normal.y); hash.Float(set.normal.z);
        hash.Float(set.spacing); hash.Float(set.offset); hash.Float(set.variation); hash.Add(uint32_t(set.seed));
    }
    out.generation = hash.value;
    if (!CollectPieces(built, mesh, out, error)) return {};
    out.adjacencyComplete = true;
    RefreshPieceFingerprint(out);
    return out;
}
PieceCollection FracturePieces(const PieceCollection& input, const PointSet& points, const VoronoiSettings& s,
                               int producer, std::string& error, std::stop_token stop) {
    error.clear();
    if (!points.grouped || points.source!=input.fingerprint || points.groups.size()!=input.pieces.size() ||
        input.pieces.size()>MaxScatterPoints || points.positions.size()>MaxScatterPoints ||
        std::any_of(points.groups.begin(),points.groups.end(),[](const auto& g){return g.positions.size()>size_t(PieceIdStride);})) {
        error="同じPiecesから生成したScatter Pointsを接続してください（1片"+std::to_string(PieceIdStride)+"点、合計"+
              std::to_string(MaxScatterPoints)+"点まで）";return {};
    }
    PieceCollection out; out.producer=producer;
    // 再分割では親をまたぐ面の重なりをまだ再構築しない。誤った接続で侵食しない。
    out.adjacencyComplete=std::none_of(input.pieces.begin(),input.pieces.end(),
                                      [](const auto& p){return bool(p.neighborhood);});
    Hash generation; generation.Add(input.fingerprint); generation.Add(points.fingerprint);
    std::set<uint32_t> ids;
    size_t triangles=0;
    for (size_t i=0;i<input.pieces.size();++i) {
        if (stop.stop_requested()) {error="評価をキャンセルしました";return {};}
        const auto& parent=input.pieces[i]; const auto& group=points.groups[i];
        if (!parent.mesh || group.pieceId!=parent.id || parent.id>uint32_t(std::numeric_limits<int>::max()/PieceIdStride-1)) {
            error="親ピースのIDが不正、または再分割のID上限に達しました";return {};
        }
        PointSet local; local.source=group.source; local.fingerprint=group.fingerprint; local.positions=group.positions;
        auto children=FractureVoronoi(*parent.mesh,local,s,producer,error,stop);
        if (!error.empty()) {error="ピース "+std::to_string(parent.id)+": "+error;return {};}
        generation.Add(parent.id); generation.Add(children.generation);
        for (auto& p:children.pieces) {
            p.id=(parent.id+1)*PieceIdStride+p.id;
            if (!ids.insert(p.id).second) {error="ピースIDが重複しています";return {};}
            p.parentId=parent.id; p.parentProducer=input.producer; p.layer=parent.layer; p.layerSize=parent.layerSize;
            p.transform=parent.transform;
            auto neighborhood=std::make_shared<PieceNeighborhood>(*p.neighborhood);
            for (auto& contact:neighborhood->contacts) contact.neighbor+=(parent.id+1)*PieceIdStride;
            if (p.layer>=0) std::erase_if(neighborhood->boundary,[](const auto& area) {
                const double length=std::sqrt(area[0]*area[0]+area[1]*area[1]+area[2]*area[2]);
                return std::abs(area[1])>length*(1-1e-7); // 板の上下面から侵食を開始しない。
            });
            p.neighborhood=std::move(neighborhood);
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
                error="分割結果は合計"+std::to_string(MaxScatterPoints)+"ピース・25万三角形までです";return {};
            }
        }
    }
    if (out.adjacencyComplete && !BuildPieceLayerSupport(out,error,stop)) return {};
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
    if (c.adjacencyComplete) {
        h.Add(0x5045454c); // メッシュ生成の世代は変更せず、隣接データをキャッシュキーへ含める。
        for (const auto& p:c.pieces) if (p.neighborhood) {
            h.Add(p.neighborhood->fixedLayerSupport);
            for (auto area:p.neighborhood->capAreas) h.Add(std::bit_cast<uint64_t>(area));
            for (auto value:p.neighborhood->supportTransform) h.Add(std::bit_cast<uint64_t>(value));
            h.Add(p.neighborhood->vertical.size());
            for (const auto& contact:p.neighborhood->vertical) {
                h.Add(contact.neighbor);h.Add(contact.side);h.Add(std::bit_cast<uint64_t>(contact.area));
            }
            h.Add(p.neighborhood->contacts.size());
            for (const auto& contact:p.neighborhood->contacts) {
                h.Add(contact.neighbor);
                for (auto v:contact.areaVector) h.Add(std::bit_cast<uint64_t>(v));
            }
            h.Add(p.neighborhood->boundary.size());
            for (const auto& area:p.neighborhood->boundary)
                for (auto v:area) h.Add(std::bit_cast<uint64_t>(v));
        }
    }
    c.fingerprint = h.value;
}
Vec3 PieceCenter(const Piece &p) {
    return F(Apply(p.transform, V(p.centroid)));
}
PieceSelection SelectPieces(const PieceCollection &c, const PieceSelectSettings &s, std::string &error, std::stop_token stop) {
    error.clear();
    PieceSelection out{c.producer, c.generation, c.fingerprint, {}};
    if (int(s.mode) < 0 || int(s.mode) > int(PieceSelectMode::Peel) || s.layer < -1) {
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
    if ((s.mode == PieceSelectMode::Random || s.mode == PieceSelectMode::Rim || s.mode == PieceSelectMode::Peel) &&
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
    if (s.mode == PieceSelectMode::Rim || s.mode == PieceSelectMode::Peel) {
        if (s.rimLayers < 0 || s.rimLayers > 32 || s.rimSide < 0 || s.rimSide > 2 ||
            !std::isfinite(s.rimFalloff) || s.rimFalloff < 0 || s.rimFalloff > 1) {
            error = "外側の層の設定が不正です";
            return {};
        }
        for (const auto& p : c.pieces) if (p.layer >= 0) layers.push_back(p.layer);
        std::sort(layers.begin(), layers.end());
        layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
    }
    const auto layerWeight = [&](const Piece& p) -> float {
        if (s.layer>=0 && p.layer!=s.layer) return -1;
        if ((s.mode == PieceSelectMode::Rim || s.mode == PieceSelectMode::Peel) && !layers.empty()) {
            if (p.layer < 0) return -1;
            const int bottom = int(std::lower_bound(layers.begin(), layers.end(), p.layer)-layers.begin());
            const int top = int(layers.size())-1-bottom;
            const int depth = s.rimSide == 1 ? top : s.rimSide == 2 ? bottom : std::min(top,bottom);
            const int available = s.rimSide ? int(layers.size()) : (int(layers.size())+1)/2;
            const int count = s.rimLayers ? std::min(s.rimLayers,available) : available;
            if (depth >= count) return -1;
            return s.mode==PieceSelectMode::Peel ? 1.f : 1-s.rimFalloff*float(depth)/float(std::max(1,count-1));
        }
        return 1;
    };
    if (s.mode == PieceSelectMode::Peel) {
        std::vector<float> weights;
        for (const auto& p:c.pieces) weights.push_back(layerWeight(p));
        return PeelPieces(c,s,weights,error,stop);
    }
    // 画面が毎フレーム評価するので、手動IDは並べて二分探索する（片数の2乗にしない）。
    std::vector<uint32_t> manual;
    if (s.mode == PieceSelectMode::Manual) {
        manual = s.ids;
        std::sort(manual.begin(), manual.end());
    }
    for (const auto &p : c.pieces) {
        const float rimWeight=layerWeight(p);
        if (rimWeight<0) continue;
        bool selected = false;
        auto center = PieceCenter(p);
        double volume = p.volume * Determinant(p.transform);
        switch (s.mode) {
        case PieceSelectMode::Manual:
            selected = std::binary_search(manual.begin(), manual.end(), p.id);
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
        case PieceSelectMode::Peel: break; // 上で隣接グラフを評価済み。
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
    if (std::any_of(s.jitterPosition.begin(), s.jitterPosition.end(), [](float v) { return !std::isfinite(v) || v < 0 || v > 100; }) ||
        !std::isfinite(s.jitterRotation) || s.jitterRotation < 0 || s.jitterRotation > 180) {
        error = "ばらつきは移動 0～100 m、回転 0～180 度にしてください";
        return {};
    }
    const bool jitter = s.jitterRotation > 0 || s.jitterPosition[0] > 0 || s.jitterPosition[1] > 0 || s.jitterPosition[2] > 0;
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
            if (jitter) {
                // 片の ID と Seed だけで決まる乱数。片を増減しても、ほかの片のずれは変わらない。
                uint64_t state = (uint64_t(p.id) << 32) ^ (uint64_t(s.jitterSeed) * 0x9E3779B97F4A7C15ull);
                const auto signedUnit = [&] { return Uniform(state) * 2 - 1; };
                PiecePose shake;
                for (int k = 0; k < 3; ++k) shake.position[k] = float(signedUnit() * s.jitterPosition[k]);
                for (int k = 0; k < 3; ++k) shake.rotation[k] = float(signedUnit() * s.jitterRotation);
                apply(p, shake, center);
            }
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
