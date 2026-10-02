#include "geometry/BaseRock.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <limits>
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
// 倍精度で構築し、指定寸法へ写してから、任意で節理面（平行な 2 面）で切る。Voronoiの母岩にするための形。
struct P {
    double x, y, z;
    P operator+(P b) const { return {x+b.x,y+b.y,z+b.z}; }
    P operator-(P b) const { return {x-b.x,y-b.y,z-b.z}; }
    P operator*(double t) const { return {x*t,y*t,z*t}; }
};
double Dot(P a,P b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
P Cross(P a,P b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
using Triangle=std::array<uint32_t,3>;
// 点群の凸包（quickhull の逐次版）。4 点以上で体積のある点群を前提にする。
std::vector<Triangle> Hull(const std::vector<P>& points) {
    // 初期四面体: 最長の点対、直線から最遠の点、その平面から最遠の点。
    uint32_t a=0,b=1,c=0,d=0;
    double best=0;
    for (uint32_t i=0;i<points.size();++i) for (uint32_t j=i+1;j<points.size();++j) {
        const P v=points[j]-points[i]; const double distance=Dot(v,v);
        if (distance>best) { best=distance;a=i;b=j; }
    }
    best=0;
    for (uint32_t i=0;i<points.size();++i) {
        const P n=Cross(points[b]-points[a],points[i]-points[a]); const double distance=Dot(n,n);
        if (distance>best) { best=distance;c=i; }
    }
    const P normal=Cross(points[b]-points[a],points[c]-points[a]);
    best=0;
    for (uint32_t i=0;i<points.size();++i) {
        const double distance=std::abs(Dot(normal,points[i]-points[a]));
        if (distance>best) { best=distance;d=i; }
    }
    const P interior=(points[a]+points[b]+points[c]+points[d])*.25;
    const auto outward = [&](Triangle f) {
        if (Dot(Cross(points[f[1]]-points[f[0]],points[f[2]]-points[f[0]]),interior-points[f[0]])>0)
            std::swap(f[1],f[2]);
        return f;
    };
    std::vector<Triangle> faces{outward({a,b,c}),outward({a,d,b}),outward({b,d,c}),outward({c,d,a})};
    for (uint32_t i=0;i<points.size();++i) {
        if (i==a || i==b || i==c || i==d) continue;
        std::vector<Triangle> kept;
        std::set<std::array<uint32_t,2>> horizon;
        for (const auto& f : faces) {
            const P n=Cross(points[f[1]]-points[f[0]],points[f[2]]-points[f[0]]);
            if (Dot(n,points[i]-points[f[0]])<=1e-11*std::sqrt(Dot(n,n))) { kept.push_back(f);continue; }
            for (int e=0;e<3;++e) {
                const uint32_t u=f[e],v=f[(e+1)%3];
                if (!horizon.erase({v,u})) horizon.insert({u,v});
            }
        }
        for (const auto& edge : horizon) kept.push_back(outward({edge[0],edge[1],i}));
        faces=std::move(kept);
    }
    return faces;
}
// 凸多面体を半空間 n·p <= offset で切る。残る頂点と、辺と面の交点を新しい点群にする（凸包は呼び出し側で取り直す）。
std::vector<P> ClipHull(const std::vector<P>& points, const std::vector<Triangle>& faces, P n, double offset) {
    std::vector<P> result;
    std::vector<char> inside(points.size());
    double span=0;
    for (const auto& p : points) for (const auto& q : points) span=std::max(span,std::abs(Dot(n,p-q)));
    // 面にほぼ載る点は内側として残し、既にある点にごく近い交点は足さない。近い 2 点が並ぶと細長い面ができ、
    // 単精度に落としたときその面の向きが狂う（Voronoi の凸の判定に響く）。
    const double tolerance=span*1e-3;
    for (size_t i=0;i<points.size();++i) inside[i]=Dot(n,points[i])<=offset+tolerance;
    for (size_t i=0;i<points.size();++i) {
        if (!inside[i]) continue;
        // 面のすぐ外（許容差の内）の点は面の上へ投影して残す（はみ出しを残さない）。
        const double d=Dot(n,points[i])-offset;
        result.push_back(d>0 ? points[i]-n*d : points[i]);
    }
    std::set<std::array<uint32_t,2>> edges;
    for (const auto& f : faces) for (int e=0;e<3;++e) {
        const uint32_t u=f[e],v=f[(e+1)%3];
        if (inside[u]==inside[v] || !edges.insert({std::min(u,v),std::max(u,v)}).second) continue;
        const double du=Dot(n,points[u])-offset, dv=Dot(n,points[v])-offset;
        // 内側の端点が面の上に投影された（d > 0）辺は、その投影点が交点の代わり。ここで交点を作ると外挿になる。
        if ((inside[u] ? du : dv)>0) continue;
        const P q=points[u]+(points[v]-points[u])*(du/(du-dv));
        bool duplicate=false;
        for (const auto& r : result) {
            const P d=q-r;
            if (Dot(d,d)<=tolerance*tolerance) { duplicate=true;break; }
        }
        if (!duplicate) result.push_back(q);
    }
    return result;
}
// 同一平面（倍精度で一致する面）の三角形を多角形にまとめ、順に並べ直して扇形に切り直す。わずかに傾いた隣の面はまとめない。切断面の多数の共面点を quickhull が
// 任意の順で三角形化すると細長い面ができ、単精度に落としたとき法線が狂って凸の判定を通らない。
std::vector<Triangle> Retriangulate(const std::vector<P>& points, std::vector<Triangle> faces) {
    struct Group { P normal; double offset; std::vector<uint32_t> ids; };
    std::vector<Group> groups;
    double scale=0;
    for (const auto& p : points) scale=std::max({scale,std::abs(p.x),std::abs(p.y),std::abs(p.z)});
    // 面は大きい順に見る。面の向きは一番大きな三角形から決め、細長い三角形（向きが不正確）は頂点の距離で同じ面に入れる。
    const auto area = [&](const Triangle& f) { const P n=Cross(points[f[1]]-points[f[0]],points[f[2]]-points[f[0]]); return Dot(n,n); };
    std::sort(faces.begin(),faces.end(),[&](const Triangle& a,const Triangle& b) { return area(a)>area(b); });
    for (const auto& f : faces) {
        P n=Cross(points[f[1]]-points[f[0]],points[f[2]]-points[f[0]]);
        const double length=std::sqrt(Dot(n,n));
        if (!(length>0)) continue;
        n=n*(1/length);
        const double offset=Dot(n,points[f[0]]);
        Group* group=nullptr;
        for (auto& g : groups) {
            bool on=true;
            for (auto id : f) on&=std::abs(Dot(g.normal,points[id])-g.offset)<1e-9*scale;
            if (on) { group=&g;break; }
        }
        if (!group) { groups.push_back({n,offset,{}}); group=&groups.back(); }
        for (auto id : f)
            if (std::find(group->ids.begin(),group->ids.end(),id)==group->ids.end()) group->ids.push_back(id);
    }
    std::vector<Triangle> result;
    for (auto& g : groups) {
        P centroid{0,0,0};
        for (auto id : g.ids) centroid=centroid+points[id];
        centroid=centroid*(1.0/g.ids.size());
        const P axis=std::abs(g.normal.x)<.9 ? P{1,0,0} : P{0,1,0};
        P u=Cross(g.normal,axis); u=u*(1/std::sqrt(Dot(u,u)));
        const P v=Cross(g.normal,u);
        // quickhull は共面の点を後から足すとき、先に足した点を面の内側に残す（面の中の頂点）。
        // 面の多角形の 2 次元凸包を取り、内側の点を外す。側面はそれらを参照しない（極点でないため）。
        const auto at = [&](uint32_t id) { const P d=points[id]-centroid; return std::array<double,2>{Dot(d,u),Dot(d,v)}; };
        std::sort(g.ids.begin(),g.ids.end(),[&](uint32_t a,uint32_t b) { return at(a)<at(b); });
        const auto turn = [&](uint32_t o,uint32_t a,uint32_t b) {
            const auto po=at(o),pa=at(a),pb=at(b);
            return (pa[0]-po[0])*(pb[1]-po[1])-(pa[1]-po[1])*(pb[0]-po[0]);
        };
        const double flat=1e-12*scale*scale;
        std::vector<uint32_t> hull;
        for (int pass=0;pass<2;++pass) {
            const size_t start=hull.size();
            for (size_t k=0;k<g.ids.size();++k) {
                const uint32_t id=pass ? g.ids[g.ids.size()-1-k] : g.ids[k];
                while (hull.size()>=start+2 && turn(hull[hull.size()-2],hull.back(),id)<=flat) hull.pop_back();
                hull.push_back(id);
            }
            hull.pop_back();
        }
        for (size_t i=1;i+1<hull.size();++i) {
            Triangle t{hull[0],hull[i],hull[i+1]};
            if (Dot(Cross(points[t[1]]-points[t[0]],points[t[2]]-points[t[0]]),g.normal)<0) std::swap(t[1],t[2]);
            result.push_back(t);
        }
    }
    return result;
}
Mesh ConvexPeak(const BaseRockSettings& s) {
    uint32_t state = uint32_t(s.seed);
    const auto random = [&] { state = Mix(state+0x9e3779b9u); return double(state)/UINT32_MAX; };
    // 走向（Y 軸まわり）。稜線はこの向きに伸び、節理面はこの向きを含む。Parallel Planes の回転 Y と同じ。
    const double strike = s.peakStrike*std::numbers::pi/180, dip = s.peakSlabDip*std::numbers::pi/180;
    const P along{std::sin(strike),0,std::cos(strike)};
    std::vector<P> points;
    const double phase = random()*2*std::numbers::pi;
    for (int ring=0;ring<3;++ring) {
        const double height = ring==0 ? 0 : ring==1 ? s.peakShoulderHeight : 1;
        const double radius = ring==0 ? .5 : ring==1 ? .36 : .5*s.peakTopWidth;
        for (int i=0;i<s.peakSides;++i) {
            const double angle = phase+2*std::numbers::pi*(i+(random()-.5)*s.peakVariation*.5)/s.peakSides;
            const double r = radius*(1+(random()-.5)*s.peakVariation);
            const double y = height-.5+(ring==1 ? (random()-.5)*s.peakVariation*.2 : 0);
            P p{std::cos(angle)*r+s.peakLeanX*height,y,std::sin(angle)*r+s.peakLeanZ*height};
            // 頂の点を走向に沿った線分の上に並べる。長さ 0 なら従来どおり一点のまわり（乱数の順序も変えない）。
            if (ring==2 && s.peakSides>1)
                p=p+along*((double(i)/(s.peakSides-1)-.5)*s.peakRidgeLength);
            points.push_back(p);
        }
    }
    // 外接箱を指定寸法に写してから凸包を取る（アフィン変換で凸包は変わらない）。節理面の傾きは実寸で決める。
    P lo=points[0],hi=lo;
    for (const auto& p : points) {
        lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
        hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
    }
    for (auto& p : points)
        p={((p.x-lo.x)/(hi.x-lo.x)-.5)*s.size[0],((p.y-lo.y)/(hi.y-lo.y)-.5)*s.size[1],((p.z-lo.z)/(hi.z-lo.z)-.5)*s.size[2]};
    auto faces=Hull(points);
    if (s.peakSlabThickness>0) {
        // 節理面の法線 = Ry(走向)·Rz(傾き)·(0,1,0)。Parallel Planes の rotation [0, 走向, 傾き] と一致する。
        const P n0{-std::sin(dip),std::cos(dip),0};
        const P n{std::cos(strike)*n0.x+std::sin(strike)*n0.z,n0.y,-std::sin(strike)*n0.x+std::cos(strike)*n0.z};
        double low=std::numeric_limits<double>::max(),high=-low;
        for (const auto& p : points) { low=std::min(low,Dot(n,p)); high=std::max(high,Dot(n,p)); }
        const double center=(low+high)*.5, half=s.peakSlabThickness*s.size[0]*.5;
        if (center+half<high-1e-9*s.size[0]) {
            points=ClipHull(points,faces,n,center+half);
            faces=Hull(points);
        }
        if (center-half>low+1e-9*s.size[0]) {
            points=ClipHull(points,faces,n*-1.0,-(center-half));
            faces=Hull(points);
        }
    }
    // 近い頂点（外接寸法の 1%）を中点に統合して凸包を取り直す。近い 2 点を結ぶ細長い面は、単精度で向きが狂い
    // Voronoi の凸の判定（1e-5 相対）を通らない。岩の母岩としては 1% の差は見えない。
    const double weld=.01*std::max({s.size[0],s.size[1],s.size[2]});
    for (bool merged=true;merged;) {
        merged=false;
        std::vector<char> used(points.size(),0);
        for (const auto& f : faces) for (auto id : f) used[id]=1;
        for (size_t i=0;i<points.size() && !merged;++i) for (size_t j=i+1;j<points.size();++j) {
            if (!used[i] || !used[j]) continue;
            const P d=points[j]-points[i];
            if (Dot(d,d)>=weld*weld) continue;
            points[i]=(points[i]+points[j])*.5;
            points.erase(points.begin()+j);
            faces=Hull(points);
            merged=true;
            break;
        }
    }
    faces=Retriangulate(points,faces);
    Mesh result;
    std::map<uint32_t,uint32_t> remap;
    for (auto face : faces) {
        for (auto& id : face) {
            const auto [it,added]=remap.emplace(id,uint32_t(result.positions.size()));
            if (added) {
                const auto p=points[id];
                result.positions.push_back({float(p.x),float(p.y),float(p.z)});
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
        !range(s.peakLeanX,-.5f,.5f) || !range(s.peakLeanZ,-.5f,.5f) || !range(s.peakVariation,0,.5f) ||
        !range(s.peakRidgeLength,0,1) || !range(s.peakStrike,-90,90) || !range(s.peakSlabThickness,0,1) ||
        !range(s.peakSlabDip,45,90))
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
