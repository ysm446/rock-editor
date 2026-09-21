#include "geometry/Pieces.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
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
// 同じ辺の交点を一度だけ生成し、切断面は境界辺の逆向きの閉路から作る。
bool Clip(Poly &poly, Plane plane) {
    std::vector<double> distances;
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
PointSet ScatterPoints(const Mesh &mesh, const ScatterSettings &s, std::string &error, std::stop_token stop) {
    error.clear();
    PointSet out;
    if (s.version != 1 || s.count < 2 || s.count > 128) {
        error = "点数は2〜128、アルゴリズムはversion 1が必要です";
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
        bool valid = true;
        for (auto plane : planes)
            if (Dot(plane.n, p) > plane.d - 1e-8) {
                valid = false;
                break;
            }
        for (auto other : accepted)
            if (Length(p - other) < 1e-5)
                valid = false;
        if (valid) {
            accepted.push_back(p);
            out.positions.push_back(F(origin + p * scale));
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
    out.fingerprint = h.value;
    return out;
}
PieceCollection FractureVoronoi(const Mesh &mesh, const PointSet &points, const VoronoiSettings &s,
                                int producer, std::string &error, std::stop_token stop) {
    error.clear();
    if (points.source != MeshFingerprint(mesh) || points.positions.size() < 2 ||
        points.positions.size() > 128 || s.version != 1) {
        error = "同じMeshから生成した2〜128点のScatter Pointsが必要です";
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
    double total = 0;
    size_t triangles = 0;
    for (size_t i = 0; i < sites.size(); ++i) {
        if (stop.stop_requested()) {
            error = "評価をキャンセルしました";
            return {};
        }
        auto poly = initial;
        for (size_t j = 0; j < sites.size(); ++j)
            if (i != j && !Clip(poly, cuts[i][j])) {
                error = "切断境界を閉じられません。Seedを変更してください";
                return {};
            }
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
            info.volume <= scale * scale * scale * 1e-14) {
            error = "微小片または精度不足の面を検出しました。Seed・寸法・伸長倍率を調整してください";
            return {};
        }
        triangles += result.triangles.size();
        if (triangles > 250000) {
            error = "出力が25万三角形を超えました";
            return {};
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
        Piece piece;
        piece.id = uint32_t(i);
        piece.centroid = F(base + center * (1 / volume));
        piece.volume = info.volume;
        piece.outerFaces = outer;
        piece.mesh = std::make_shared<const Mesh>(std::move(result));
        piece.faceOrigins = std::make_shared<const std::vector<uint8_t>>(std::move(origins));
        out.pieces.push_back(std::move(piece));
        total += info.volume;
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
void RefreshPieceFingerprint(PieceCollection &c) {
    Hash h;
    h.Add(c.producer);
    h.Add(c.generation);
    h.Add(c.pieces.size());
    for (const auto &p : c.pieces) {
        h.Add(p.id);
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
    if (int(s.mode) < 0 || int(s.mode) > 4) {
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
    if (s.mode == PieceSelectMode::Random &&
        (!std::isfinite(s.fraction) || s.fraction < 0 || s.fraction > 1)) {
        error = "選択率は0〜1にしてください";
        return {};
    }
    if (s.mode == PieceSelectMode::Manual && !s.ids.empty() &&
        (s.producer != c.producer || s.generation != c.generation)) {
        error = "上流の分割が変わりました。手動選択をリセットして再選択してください";
        return {};
    }
    for (const auto &p : c.pieces) {
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
