#include "geometry/BaseRock.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <numbers>

namespace rock::geometry {
namespace {
uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
double Sample(int x, int y, int z, int seed) {
    const auto hash = Mix(Mix(uint32_t(x)) ^ Mix(uint32_t(y) + 0x9e3779b9u) ^ Mix(uint32_t(z) + 0x85ebca6bu) ^
                          Mix(uint32_t(seed) + 0xc2b2ae35u));
    return double(hash) / double(UINT32_MAX) * 2 - 1;
}
double Noise(Vec3 direction, float scale, int seed) {
    const double x = double(direction.x) * scale, y = double(direction.y) * scale,
                 z = double(direction.z) * scale;
    const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    const auto smooth = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    const double tx = smooth(x - ix), ty = smooth(y - iy), tz = smooth(z - iz);
    double value = 0;
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
                value += Sample(ix + i, iy + j, iz + k, seed) * (i ? tx : 1 - tx) * (j ? ty : 1 - ty) *
                         (k ? tz : 1 - tz);
    return value;
}
Vec3 Normalize(Vec3 p) {
    const double length = std::sqrt(double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z);
    return {float(p.x / length), float(p.y / length), float(p.z / length)};
}
// 少数の輪郭点から凸包を作る。点を直接結んだ輪郭は凹み得るため、必ず凸包を通す。
// 単位寸法・倍精度で構築し、最後に指定寸法へ写す。Voronoiの母岩にするための形。
Mesh ConvexPeak(const BaseRockSettings& s) {
    struct P {
        double x, y, z;
        P operator+(P b) const { return {x+b.x,y+b.y,z+b.z}; }
        P operator-(P b) const { return {x-b.x,y-b.y,z-b.z}; }
        P operator*(double t) const { return {x*t,y*t,z*t}; }
    };
    const auto dot = [](P a,P b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
    const auto cross = [](P a,P b) { return P{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; };
    uint32_t state = uint32_t(s.seed);
    const auto random = [&] { state = Mix(state+0x9e3779b9u); return double(state)/UINT32_MAX; };
    std::vector<P> points;
    const double phase = random()*2*std::numbers::pi;
    for (int ring=0;ring<3;++ring) {
        const double height = ring==0 ? 0 : ring==1 ? s.peakShoulderHeight : 1;
        const double radius = ring==0 ? .5 : ring==1 ? .36 : .5*s.peakTopWidth;
        for (int i=0;i<s.peakSides;++i) {
            const double angle = phase+2*std::numbers::pi*(i+(random()-.5)*s.peakVariation*.5)/s.peakSides;
            const double r = radius*(1+(random()-.5)*s.peakVariation);
            const double y = height-.5+(ring==1 ? (random()-.5)*s.peakVariation*.2 : 0);
            points.push_back({std::cos(angle)*r+s.peakLeanX*height,y,std::sin(angle)*r+s.peakLeanZ*height});
        }
    }
    // 初期四面体: 最長の点対、直線から最遠の点、その平面から最遠の点。
    uint32_t a=0,b=1,c=0,d=0;
    double best=0;
    for (uint32_t i=0;i<points.size();++i) for (uint32_t j=i+1;j<points.size();++j) {
        const P v=points[j]-points[i]; const double distance=dot(v,v);
        if (distance>best) { best=distance;a=i;b=j; }
    }
    best=0;
    for (uint32_t i=0;i<points.size();++i) {
        const P n=cross(points[b]-points[a],points[i]-points[a]); const double distance=dot(n,n);
        if (distance>best) { best=distance;c=i; }
    }
    const P normal=cross(points[b]-points[a],points[c]-points[a]);
    best=0;
    for (uint32_t i=0;i<points.size();++i) {
        const double distance=std::abs(dot(normal,points[i]-points[a]));
        if (distance>best) { best=distance;d=i; }
    }
    const P interior=(points[a]+points[b]+points[c]+points[d])*.25;
    using Triangle=std::array<uint32_t,3>;
    const auto outward = [&](Triangle f) {
        if (dot(cross(points[f[1]]-points[f[0]],points[f[2]]-points[f[0]]),interior-points[f[0]])>0)
            std::swap(f[1],f[2]);
        return f;
    };
    std::vector<Triangle> faces{outward({a,b,c}),outward({a,d,b}),outward({b,d,c}),outward({c,d,a})};
    for (uint32_t i=0;i<points.size();++i) {
        if (i==a || i==b || i==c || i==d) continue;
        std::vector<Triangle> kept;
        std::set<std::array<uint32_t,2>> horizon;
        for (const auto& f : faces) {
            const P n=cross(points[f[1]]-points[f[0]],points[f[2]]-points[f[0]]);
            if (dot(n,points[i]-points[f[0]])<=1e-11*std::sqrt(dot(n,n))) { kept.push_back(f);continue; }
            for (int e=0;e<3;++e) {
                const uint32_t u=f[e],v=f[(e+1)%3];
                if (!horizon.erase({v,u})) horizon.insert({u,v});
            }
        }
        for (const auto& edge : horizon) kept.push_back(outward({edge[0],edge[1],i}));
        faces=std::move(kept);
    }
    P lo=points[0],hi=lo;
    for (const auto& p : points) {
        lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
        hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
    }
    Mesh result;
    std::map<uint32_t,uint32_t> remap;
    for (auto face : faces) {
        for (auto& id : face) {
            const auto [it,added]=remap.emplace(id,uint32_t(result.positions.size()));
            if (added) {
                const auto p=points[id];
                result.positions.push_back({float(((p.x-lo.x)/(hi.x-lo.x)-.5)*s.size[0]),
                    float(((p.y-lo.y)/(hi.y-lo.y)-.5)*s.size[1]),float(((p.z-lo.z)/(hi.z-lo.z)-.5)*s.size[2])});
            }
            id=it->second;
        }
        result.triangles.push_back(face);
    }
    return result;
}
}  // namespace
const char* BaseShapeName(BaseShape shape) {
    switch (shape) {
        case BaseShape::Box:
            return "box";
        case BaseShape::RoundedBox:
            return "roundedBox";
        case BaseShape::Sphere:
            return "sphere";
        case BaseShape::Ellipsoid:
            return "ellipsoid";
        case BaseShape::ConvexPeak:
            return "convexPeak";
        default:
            return "invalid";
    }
}
BaseShape ParseBaseShape(std::string_view name) {
    for (auto shape : {BaseShape::Box, BaseShape::RoundedBox, BaseShape::Sphere, BaseShape::Ellipsoid, BaseShape::ConvexPeak})
        if (name == BaseShapeName(shape)) return shape;
    return BaseShape::Invalid;
}
Mesh MakeBaseRock(const BaseRockSettings& s, std::string& error) {
    error.clear();
    const auto fail = [&](const char* text) {
        error = text;
        return Mesh{};
    };
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    for (float v : s.size)
        if (!range(v, 0.001f, 1000)) return fail("寸法は有限の 0.001～1000 m にしてください");
    if (s.shape != BaseShape::Box && s.shape != BaseShape::RoundedBox && s.shape != BaseShape::Sphere &&
        s.shape != BaseShape::Ellipsoid && s.shape != BaseShape::ConvexPeak)
        return fail("母岩の形状が不明です");
    if (s.subdivisions != 4 && s.subdivisions != 8 && s.subdivisions != 16 && s.subdivisions != 32)
        return fail("分割数は 4 / 8 / 16 / 32 にしてください");
    if (!range(s.roundness, 0, 1) || !range(s.noiseStrength, 0, 0.15f) || !range(s.noiseScale, 0.5f, 4))
        return fail("丸みは0～1、ノイズ強度は0～0.15、ノイズ細かさは0.5～4にしてください");
    if (s.peakSides<4 || s.peakSides>12 || !range(s.peakTopWidth,0,.6f) || !range(s.peakShoulderHeight,.2f,.85f) ||
        !range(s.peakLeanX,-.5f,.5f) || !range(s.peakLeanZ,-.5f,.5f) || !range(s.peakVariation,0,.5f))
        return fail("凸岩峰の輪郭数・頂の幅・肩の高さ・偏り・ばらつきが範囲外です");
    if (s.shape == BaseShape::ConvexPeak) {
        auto peak=ConvexPeak(s);
        MeshInfo info;
        if (!InspectMesh(peak,info) || !info.closed || info.components!=1 || info.volume<=0)
            return fail("凸岩峰の閉包・体積を確認できません。寸法比やSeedを調整してください");
        return peak;
    }
    if (s.noiseStrength == 0 &&
        (s.shape == BaseShape::Box || (s.shape == BaseShape::RoundedBox && s.roundness == 0)))
        return MakeBox(s.size);
    const int n = s.subdivisions;
    const std::array<float, 3> half{s.size[0] * 0.5f, s.size[1] * 0.5f, s.size[2] * 0.5f};
    const float radius = *std::min_element(half.begin(), half.end()) * s.roundness;
    Mesh mesh;
    std::map<std::array<int, 3>, uint32_t> vertices;
    const auto vertex = [&](const std::array<int, 3>& key) {
        auto [it, inserted] = vertices.emplace(key, static_cast<uint32_t>(mesh.positions.size()));
        if (!inserted) return it->second;
        const std::array<float, 3> cube{2.0f * key[0] / n - 1, 2.0f * key[1] / n - 1, 2.0f * key[2] / n - 1};
        Vec3 p{cube[0] * half[0], cube[1] * half[1], cube[2] * half[2]};
        if (s.shape == BaseShape::Sphere || s.shape == BaseShape::Ellipsoid) {
            const auto unit = Normalize({cube[0], cube[1], cube[2]});
            p = {unit.x * half[0], unit.y * (s.shape == BaseShape::Sphere ? half[0] : half[1]),
                 unit.z * (s.shape == BaseShape::Sphere ? half[0] : half[2])};
        } else if (s.shape == BaseShape::RoundedBox && radius > 0) {
            const Vec3 inner{std::clamp(p.x, -half[0] + radius, half[0] - radius),
                             std::clamp(p.y, -half[1] + radius, half[1] - radius),
                             std::clamp(p.z, -half[2] + radius, half[2] - radius)};
            const auto unit = Normalize({p.x - inner.x, p.y - inner.y, p.z - inner.z});
            p = {inner.x + radius * unit.x, inner.y + radius * unit.y, inner.z + radius * unit.z};
        }
        if (s.noiseStrength > 0) {
            // 正の放射方向変位で、共有頂点と各面の原点に対する向きを保つ。
            const float factor = float(1 + s.noiseStrength * Noise(Normalize(p), s.noiseScale, s.seed));
            p = {p.x * factor, p.y * factor, p.z * factor};
        }
        mesh.positions.push_back(p);
        return it->second;
    };
    for (int axis = 0; axis < 3; ++axis)
        for (int side = 0; side < 2; ++side) {
            // 循環する2軸の外積は +axis。負側だけ面の順を反転する。
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    std::array<uint32_t, 4> ids;
                    for (int k = 0; k < 4; ++k) {
                        std::array<int, 3> key{};
                        key[axis] = side * n;
                        key[u] = i + (k == 1 || k == 2);
                        key[v] = j + (k >= 2);
                        ids[k] = vertex(key);
                    }
                    if (side == 0) std::swap(ids[1], ids[3]);
                    mesh.triangles.push_back({ids[0], ids[1], ids[2]});
                    mesh.triangles.push_back({ids[0], ids[2], ids[3]});
                }
        }
    MeshInfo info;
    if (!InspectMesh(mesh, info) || !info.closed || info.components != 1 || info.volume <= 0)
        return fail("母岩の閉包・体積・面の品質を確認できませんでした。寸法比や設定を見直してください");
    return mesh;
}
}  // namespace rock::geometry
