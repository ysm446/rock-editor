#include "geometry/BakeOcclusion.h"
#include "geometry/ShapeMask.h"
#include "geometry/UvUnwrap.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <execution>
#include <numeric>
#include <numbers>
#include <unordered_map>
namespace rock::geometry {
namespace {
struct V {
    double x, y, z;
    double At(int a) const {
        return a == 0 ? x : a == 1 ? y : z;
    }
    V operator+(V b) const {
        return {x + b.x, y + b.y, z + b.z};
    }
    V operator-(V b) const {
        return {x - b.x, y - b.y, z - b.z};
    }
    V operator*(double s) const {
        return {x * s, y * s, z * s};
    }
};
V Convert(Vec3 p) {
    return {p.x, p.y, p.z};
}
double Dot(V a, V b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
V Cross(V a, V b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V Unit(V a) {
    return a * (1 / std::sqrt(Dot(a, a)));
}
V Min(V a, V b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
V Max(V a, V b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
struct Bvh {
    struct Triangle {
        V a, b, c, lo, hi, center;
    };
    struct Node {
        V lo, hi;
        size_t begin, end;
        int left = -1, right = -1;
    };
    std::vector<Triangle> triangles;
    std::vector<size_t> order;
    std::vector<Node> nodes;
    int Build(size_t begin, size_t end) {
        Node n{triangles[order[begin]].lo, triangles[order[begin]].hi, begin, end};
        for (size_t i = begin + 1; i < end; ++i) {
            n.lo = Min(n.lo, triangles[order[i]].lo);
            n.hi = Max(n.hi, triangles[order[i]].hi);
        }
        int id = int(nodes.size());
        nodes.push_back(n);
        if (end - begin > 8) {
            V extent = n.hi - n.lo;
            int axis = extent.y > extent.x ? 1 : 0;
            if (extent.z > extent.At(axis))
                axis = 2;
            size_t mid = (begin + end) / 2;
            std::nth_element(
                order.begin() + begin, order.begin() + mid, order.begin() + end,
                [&](auto a, auto b) { return triangles[a].center.At(axis) < triangles[b].center.At(axis); });
            int left = Build(begin, mid), right = Build(mid, end);
            nodes[id].left = left;
            nodes[id].right = right;
        }
        return id;
    }
    bool Hit(V origin, V direction, double distance, size_t ignored, int id = 0) const {
        const auto &n = nodes[id];
        double near = 0, far = distance;
        for (int axis = 0; axis < 3; ++axis) {
            double d = direction.At(axis), o = origin.At(axis);
            if (std::abs(d) < 1e-15) {
                if (o < n.lo.At(axis) || o > n.hi.At(axis))
                    return false;
            } else {
                double a = (n.lo.At(axis) - o) / d, b = (n.hi.At(axis) - o) / d;
                if (a > b)
                    std::swap(a, b);
                near = std::max(near, a);
                far = std::min(far, b);
                if (near > far)
                    return false;
            }
        }
        if (n.left >= 0)
            return Hit(origin, direction, distance, ignored, n.left) ||
                   Hit(origin, direction, distance, ignored, n.right);
        for (size_t i = n.begin; i < n.end; ++i) {
            if (order[i] == ignored)
                continue;
            const auto &t = triangles[order[i]];
            V e1 = t.b - t.a, e2 = t.c - t.a, p = Cross(direction, e2);
            double det = Dot(e1, p);
            if (std::abs(det) < 1e-14 * std::sqrt(Dot(e1, e1) * Dot(e2, e2)))
                continue;
            V v = origin - t.a;
            double u = Dot(v, p) / det;
            if (u < 0 || u > 1)
                continue;
            V q = Cross(v, e1);
            double w = Dot(direction, q) / det;
            if (w < 0 || u + w > 1)
                continue;
            double ray = Dot(e2, q) / det;
            if (ray > 0 && ray < distance)
                return true;
        }
        return false;
    }
};
} // namespace
bool BakeOcclusion(const Mesh &mesh, float distance, int samples, float strength,
                   std::vector<uint8_t> &pixels, std::string &error) {
    error.clear();
    pixels.clear();
    MeshInfo info;
    if (!HasValidUvs(mesh) || !InspectMesh(mesh, info) || !std::isfinite(distance) || distance <= 0 ||
        samples < 8 || samples > 128 || !std::isfinite(strength) || strength < 0 || strength > 1) {
        error = "形状AOの入力または設定が不正です";
        return false;
    }
    const auto width = mesh.uvWidth, height = mesh.uvHeight;
    if (!width || !height || width > 4096 || height > 4096) {
        error = "形状AOは4096までのUVアトラスに対応します";
        return false;
    }
    Bvh bvh;
    for (auto f : mesh.triangles) {
        V a = Convert(mesh.positions[f[0]]), b = Convert(mesh.positions[f[1]]),
          c = Convert(mesh.positions[f[2]]);
        bvh.triangles.push_back({a, b, c, Min(a, Min(b, c)), Max(a, Max(b, c)), (a + b + c) * (1. / 3)});
    }
    bvh.order.resize(bvh.triangles.size());
    std::iota(bvh.order.begin(), bvh.order.end(), 0);
    bvh.Build(0, bvh.order.size());
    pixels.assign(size_t(width) * height, 255);
    V extent = Convert(info.maximum) - Convert(info.minimum);
    double bias = std::min(double(distance) * 1e-3, std::max({extent.x, extent.y, extent.z}) * 1e-6);
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        const auto &t = bvh.triangles[f];
        V n = Unit(Cross(t.b - t.a, t.c - t.a));
        V tangent = Unit(Cross(std::abs(n.y) < .9 ? V{0, 1, 0} : V{1, 0, 0}, n)),
          bitangent = Cross(n, tangent);
        const auto uv = mesh.cornerUvs[f];
        auto cross = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
        double area = cross(uv[1].u - uv[0].u, uv[1].v - uv[0].v, uv[2].u - uv[0].u, uv[2].v - uv[0].v);
        if (std::abs(area) < 1e-16)
            continue;
        int x0 = std::max(0, int(std::floor(std::min({uv[0].u, uv[1].u, uv[2].u}) * width))),
            x1 = std::min(int(width) - 1, int(std::ceil(std::max({uv[0].u, uv[1].u, uv[2].u}) * width)));
        int y0 = std::max(0, int(std::floor(std::min({uv[0].v, uv[1].v, uv[2].v}) * height))),
            y1 = std::min(int(height) - 1, int(std::ceil(std::max({uv[0].v, uv[1].v, uv[2].v}) * height)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                double u = (x + .5) / width - uv[0].u, v = (y + .5) / height - uv[0].v;
                double b = cross(u, v, uv[2].u - uv[0].u, uv[2].v - uv[0].v) / area,
                       c = cross(uv[1].u - uv[0].u, uv[1].v - uv[0].v, u, v) / area;
                if (b < 0 || c < 0 || b + c > 1)
                    continue;
                V p = t.a + (t.b - t.a) * b + (t.c - t.a) * c + n * bias;
                int occluded = 0;
                for (int sample = 0; sample < samples; ++sample) {
                    double r = std::sqrt((sample + .5) / samples), phi = sample * 2.399963229728653;
                    V direction = tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) +
                                  n * std::sqrt(1 - r * r);
                    occluded += bvh.Hit(p, direction, distance, f);
                }
                pixels[size_t(y) * width + x] =
                    uint8_t(std::lround(255 * (1 - double(strength) * occluded / samples)));
            }
    }
    return true;
}
float MaskImage::Sample(float u, float v) const {
    if (!width || !height || pixels.size() != size_t(width) * height || !std::isfinite(u) || !std::isfinite(v))
        return 0;
    const float x = std::clamp(u, 0.f, 1.f) * width - .5f, y = std::clamp(v, 0.f, 1.f) * height - .5f;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float tx = x - ix, ty = y - iy;
    const auto at = [&](int a, int b) {
        return pixels[size_t(std::clamp(b, 0, int(height) - 1)) * width + std::clamp(a, 0, int(width) - 1)] / 255.f;
    };
    return std::lerp(std::lerp(at(ix, iy), at(ix + 1, iy), tx), std::lerp(at(ix, iy + 1), at(ix + 1, iy + 1), tx), ty);
}
namespace {
// 格子値を五次曲線で補間する3Dノイズ。負座標にも対応し、大きな座標を整数化する前に周期内へ戻す。
uint32_t NoiseHash(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; return h ^ (h >> 16);
}
double ValueNoise(V p, uint32_t seed) {
    const auto lattice=[](double x) { return uint32_t(int64_t(std::fmod(std::floor(x),1048576.))) & 0xfffffu; };
    const auto fade=[](double x) { x-=std::floor(x); return x*x*x*(x*(x*6-15)+10); };
    const uint32_t x=lattice(p.x), y=lattice(p.y), z=lattice(p.z);
    const auto at=[&](uint32_t a,uint32_t b,uint32_t c) {
        return double(NoiseHash(seed ^ NoiseHash(a&0xfffffu) ^ NoiseHash((b&0xfffffu)+0x9e3779b9u) ^
            NoiseHash((c&0xfffffu)+0x85ebca6bu))) / double(UINT32_MAX);
    };
    const double tx=fade(p.x),ty=fade(p.y),tz=fade(p.z);
    return std::lerp(std::lerp(std::lerp(at(x,y,z),at(x+1,y,z),tx),
                              std::lerp(at(x,y+1,z),at(x+1,y+1,z),tx),ty),
                     std::lerp(std::lerp(at(x,y,z+1),at(x+1,y,z+1),tx),
                              std::lerp(at(x,y+1,z+1),at(x+1,y+1,z+1),tx),ty),tz);
}
double CloudNoise(V surface,const NoiseMaskSettings& settings) {
    V p=surface*(1./settings.size);
    if (settings.warp>0) {
        const V q=p*.5;
        p=p+V{ValueNoise(q,settings.seed+101)*2-1,ValueNoise(q,settings.seed+211)*2-1,
              ValueNoise(q,settings.seed+307)*2-1}*double(settings.warp);
    }
    double value=0,weight=1,total=0;
    for (uint32_t octave=0;octave<4;++octave) {
        value+=weight*ValueNoise(p,settings.seed+octave*0x9e3779b9u);
        total+=weight;weight*=settings.detail*.7;p=p*2;
    }
    return std::clamp((value/total-.5)*(1+15*settings.contrast*settings.contrast)+.5,0.,1.);
}
}
static MaskImage SurfaceMask(const Mesh &mesh, const ShapeMaskSettings &settings, std::string &error,
                    std::stop_token stop, const std::function<void(int)> &progress, const NoiseMaskSettings* noise, const DepositionMaskSettings* deposition = nullptr) {

    error.clear();
    const int resolution = settings.resolution;
    if (stop.stop_requested()) { error="マスク生成をキャンセルしました";return {}; }
    const bool occlusion = !noise && settings.type == ShapeMaskType::Occlusion;
    const bool curvature = !noise && !deposition && (settings.type == ShapeMaskType::ValleyCurvature || settings.type == ShapeMaskType::RidgeCurvature);
    if ((settings.type != ShapeMaskType::Occlusion && settings.type != ShapeMaskType::Direction &&
         settings.type != ShapeMaskType::Height && !curvature) ||
        !std::isfinite(settings.distance) || settings.distance < .001f || settings.distance > 1000 ||
        settings.samples < kMinOcclusionSamples || settings.samples > kMaxOcclusionSamples ||
        resolution < kMinShapeMaskResolution || resolution > kMaxShapeMaskResolution ||
        (resolution & (resolution - 1)) != 0 || !std::isfinite(settings.low) || !std::isfinite(settings.high) ||
        settings.low < 0 || settings.high > 1 || settings.high - settings.low < .001f ||
        !std::isfinite(settings.gamma) || settings.gamma < .1f || settings.gamma > 10) {
        error = "Shape Maskの設定が不正です";
        return {};
    }
    MeshInfo info;
    if (!HasValidUvs(mesh) || !InspectMesh(mesh, info)) {
        error = "UV付きのMeshが必要です。先にUV Unwrapを通してください";
        return {};
    }
    const size_t width = size_t(resolution), height = size_t(resolution);
    Bvh bvh;
    bvh.triangles.reserve(mesh.triangles.size());
    for (auto f : mesh.triangles) {
        V a = Convert(mesh.positions[f[0]]), b = Convert(mesh.positions[f[1]]), c = Convert(mesh.positions[f[2]]);
        bvh.triangles.push_back({a, b, c, Min(a, Min(b, c)), Max(a, Max(b, c)), (a + b + c) * (1. / 3)});
    }
    // 向きは頂点法線を補間して使う。面の法線のままだと、隣の面と向きが違う辺で値が段になる
    // （上向き度は法線そのものなので、三角形の形がマスクに出る）。共有頂点で面積重み付き平均する。
    std::vector<V> vertexNormals(mesh.positions.size(), V{0, 0, 0});
    for (auto f : mesh.triangles) {
        const V a = Convert(mesh.positions[f[0]]), b = Convert(mesh.positions[f[1]]), c = Convert(mesh.positions[f[2]]);
        const V n = Cross(b - a, c - a);
        for (auto i : f) vertexNormals[i] = vertexNormals[i] + n;
    }
    for (auto &n : vertexNormals) {
        const double length = std::sqrt(Dot(n, n));
        if (length > 0) n = n * (1 / length);
    }
    // 面積で正規化したcotangent Laplacianを法線へ射影する。
    // 符号は凹が正、凸が負。UVの島ではなくメッシュの共有頂点を使う。
    std::vector<double> signedCurvature;
    if (curvature) {
        std::vector<V> laplacian(mesh.positions.size(), V{0,0,0});
        std::vector<double> areas(mesh.positions.size(), 0);
        std::unordered_map<uint64_t, uint32_t> edgeCounts;
        for (const auto& face : mesh.triangles) {
            if (stop.stop_requested()) { error="曲率マスクをキャンセルしました"; return {}; }
            const V a=Convert(mesh.positions[face[0]]), b=Convert(mesh.positions[face[1]]), c=Convert(mesh.positions[face[2]]);
            const V cross=Cross(b-a,c-a);
            const double twiceArea=std::sqrt(Dot(cross,cross));
            for (auto index : face) areas[index] += twiceArea / 6.;
            for (int corner=0; corner<3; ++corner) {
                const auto i=face[(corner+1)%3], j=face[(corner+2)%3], k=face[corner];
                const V pi=Convert(mesh.positions[i]), pj=Convert(mesh.positions[j]), pk=Convert(mesh.positions[k]);
                const double weight=twiceArea>1e-20 ? Dot(pi-pk,pj-pk)/twiceArea : 0;
                laplacian[i]=laplacian[i]+(pj-pi)*weight;
                laplacian[j]=laplacian[j]+(pi-pj)*weight;
                const auto lo=std::min(i,j), hi=std::max(i,j);
                ++edgeCounts[(uint64_t(lo)<<32)|hi];
            }
        }
        // 開いた端や非多様体の端を、曲がりとして誤認しない。
        std::vector<bool> boundary(mesh.positions.size(), false);
        for (const auto& [edge,count] : edgeCounts) if (count!=2) {
            boundary[edge>>32]=true; boundary[uint32_t(edge)]=true;
        }
        signedCurvature.resize(mesh.positions.size(), 0);
        for (size_t i=0; i<signedCurvature.size(); ++i)
            if (!boundary[i] && areas[i]>1e-20)
                signedCurvature[i]=Dot(laplacian[i],vertexNormals[i])/(4*areas[i]);
    }
    // レイを飛ばすのは遮蔽だけ。
    if (occlusion) {
        bvh.order.resize(bvh.triangles.size());
        std::iota(bvh.order.begin(), bvh.order.end(), 0);
        bvh.Build(0, bvh.order.size());
    }
    // 画素の中心がどの面のどこに当たるかを先に決める。辺を共有する面が同じ画素へ書くので、ここは直列にして
    // 結果を再現させる。重いレイの計算だけを後で並列にする。
    struct Texel { uint32_t face; float b, c; };
    constexpr uint32_t kNoFace = UINT32_MAX;
    std::vector<Texel> texels(width * height, Texel{kNoFace, 0, 0});
    const auto cross = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        if (stop.stop_requested()) {error="マスク生成をキャンセルしました";return {};}
        const auto uv = mesh.cornerUvs[f];
        const double area = cross(uv[1].u - uv[0].u, uv[1].v - uv[0].v, uv[2].u - uv[0].u, uv[2].v - uv[0].v);
        if (std::abs(area) < 1e-16)
            continue;
        const int x0 = std::max(0, int(std::floor(std::min({uv[0].u, uv[1].u, uv[2].u}) * width))),
                  x1 = std::min(int(width) - 1, int(std::ceil(std::max({uv[0].u, uv[1].u, uv[2].u}) * width)));
        const int y0 = std::max(0, int(std::floor(std::min({uv[0].v, uv[1].v, uv[2].v}) * height))),
                  y1 = std::min(int(height) - 1, int(std::ceil(std::max({uv[0].v, uv[1].v, uv[2].v}) * height)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double u = (x + .5) / width - uv[0].u, v = (y + .5) / height - uv[0].v;
                const double b = cross(u, v, uv[2].u - uv[0].u, uv[2].v - uv[0].v) / area,
                             c = cross(uv[1].u - uv[0].u, uv[1].v - uv[0].v, u, v) / area;
                if (b < 0 || c < 0 || b + c > 1)
                    continue;
                texels[size_t(y) * width + x] = {uint32_t(f), float(b), float(c)};
            }
    }
    const V extent = Convert(info.maximum) - Convert(info.minimum);
    const double distance = settings.distance;
    const double bias = std::min(distance * 1e-3, std::max({extent.x, extent.y, extent.z}) * 1e-6);
    MaskImage image;
    image.width = uint32_t(width);
    image.height = uint32_t(height);
    image.pixels.assign(width * height, 0);
    std::vector<size_t> rows(height);
    std::iota(rows.begin(), rows.end(), size_t(0));
    std::atomic<size_t> done{0};
    std::atomic<bool> cancelled{false};
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](size_t y) {
        if (stop.stop_requested()) {
            cancelled = true;
            return;
        }
        for (size_t x = 0; x < width; ++x) {
            const auto texel = texels[y * width + x];
            if (texel.face == kNoFace)
                continue;
            const auto &t = bvh.triangles[texel.face];
            const auto f = mesh.triangles[texel.face];
            const double wa = 1 - texel.b - texel.c;
            V n = vertexNormals[f[0]] * wa + vertexNormals[f[1]] * double(texel.b) + vertexNormals[f[2]] * double(texel.c);
            const double nl = std::sqrt(Dot(n, n));
            n = nl > 1e-12 ? n * (1 / nl) : Unit(Cross(t.b - t.a, t.c - t.a));
            const V surface = t.a + (t.b - t.a) * double(texel.b) + (t.c - t.a) * double(texel.c);
            double ratio = 0;
            if (noise) {
                ratio=CloudNoise(surface,*noise);
            } else if (curvature) {
                const double value = signedCurvature[f[0]] * wa + signedCurvature[f[1]] * double(texel.b) + signedCurvature[f[2]] * double(texel.c);
                const double sign = settings.type == ShapeMaskType::ValleyCurvature ? 1 : -1;
                ratio = std::clamp(sign * value * settings.distance, 0., 1.);
            } else if (settings.type == ShapeMaskType::Direction) {
                ratio = (n.y + 1) * .5;
            } else if (settings.type == ShapeMaskType::Height) {
                ratio = extent.y > 0 ? (surface.y - double(info.minimum.y)) / extent.y : 0;
            } else {
                // 半球の軸は補間した法線。面の平面より下へ向くレイは隣の面に当たるだけなので数えない。
                const V faceNormal = Unit(Cross(t.b - t.a, t.c - t.a));
                const V tangent = Unit(Cross(std::abs(n.y) < .9 ? V{0, 1, 0} : V{1, 0, 0}, n)), bitangent = Cross(n, tangent);
                const V p = surface + faceNormal * bias;
                int occluded = 0, counted = 0;
                for (int sample = 0; sample < settings.samples; ++sample) {
                    const double r = std::sqrt((sample + .5) / settings.samples), phi = sample * 2.399963229728653;
                    const V direction =
                        tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) + n * std::sqrt(1 - r * r);
                    if (Dot(direction, faceNormal) <= 1e-3) continue;
                    ++counted;
                    occluded += bvh.Hit(p, direction, distance, texel.face);
                }
                ratio = counted > 0 ? double(occluded) / counted : 0;
                if (deposition) {
                    // 土を受ける面だけを対象とする。壁面の平滑法線による回り込みも除く。
                    const double slopeLimit = std::cos(deposition->maxSlopeDegrees * std::numbers::pi / 180.);
                    double support = std::clamp((n.y - slopeLimit) / (1 - slopeLimit), 0., 1.);
                    support = support * support * (3 - 2 * support);
                    if (faceNormal.y <= 0) support = 0;
                    // 近傍半球の遮蔽は隙間の優先度。上方の開口は入力全体の外まで調べる。
                    // 真上と15度の円錐を固定サンプリングし、天井の下に土を塗らない。
                    int open = 0;
                    if (support > 0 && deposition->amount > 0) {
                        const double skyDistance = std::sqrt(Dot(extent, extent)) + distance;
                        for (int ray = 0; ray < 9; ++ray) {
                            const double radius = ray == 0 ? 0 : std::sin(15. * std::numbers::pi / 180.);
                            const double angle = ray * 2. * std::numbers::pi / 8.;
                            const V direction{radius * std::cos(angle), std::sqrt(1 - radius * radius), radius * std::sin(angle)};
                            open += !bvh.Hit(p, direction, skyDistance, texel.face);
                        }
                    }
                    const double recess = std::clamp(ratio * 3., 0., 1.);
                    ratio = deposition->amount * support * (open / 9.) *
                        (1 - deposition->recessPreference + deposition->recessPreference * recess);
                }
            }
            double level = std::clamp((ratio - settings.low) / double(settings.high - settings.low), 0., 1.);
            if (settings.gamma != 1) level = std::pow(level, double(settings.gamma));
            image.pixels[y * width + x] = uint8_t(std::lround(255 * level));
        }
        if (progress)
            progress(int(++done * 95 / height));
    });
    if (cancelled || stop.stop_requested()) {
        error = "Shape Maskをキャンセルしました";
        return {};
    }
    // 島の無い画素を、縦横の歩数で最も近い島の値で埋める（幅優先）。
    std::vector<size_t> frontier, next;
    std::vector<uint8_t> filled(width * height, 0);
    for (size_t i = 0; i < texels.size(); ++i)
        if (texels[i].face != kNoFace) {
            filled[i] = 1;
            frontier.push_back(i);
        }
    while (!frontier.empty()) {
        if (stop.stop_requested()) {error="マスク生成をキャンセルしました";return {};}
        next.clear();
        for (const size_t i : frontier) {
            const size_t x = i % width, y = i / width;
            const auto visit = [&](bool valid, size_t j) {
                if (!valid || filled[j])
                    return;
                filled[j] = 1;
                image.pixels[j] = image.pixels[i];
                next.push_back(j);
            };
            visit(x > 0, i - 1);
            visit(x + 1 < width, i + 1);
            visit(y > 0, i - width);
            visit(y + 1 < height, i + width);
        }
        frontier.swap(next);
    }
    if (progress)
        progress(100);
    return image;
}
MaskImage ShapeMask(const Mesh& mesh,const ShapeMaskSettings& settings,std::string& error,
                    std::stop_token stop,const std::function<void(int)>& progress) {
    return SurfaceMask(mesh,settings,error,stop,progress,nullptr);
}
MaskImage DepositionMask(const Mesh& mesh, const DepositionMaskSettings& settings, std::string& error,
                         std::stop_token stop, const std::function<void(int)>& progress) {
    error.clear();
    const auto unit = [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; };
    if (!unit(settings.amount) || !unit(settings.recessPreference) || !std::isfinite(settings.maxSlopeDegrees) ||
        settings.maxSlopeDegrees < 1 || settings.maxSlopeDegrees > 89 ||
        !std::isfinite(settings.distance) || settings.distance < .001f || settings.distance > 1000 ||
        settings.samples < kMinOcclusionSamples || settings.samples > kMaxOcclusionSamples ||
        settings.resolution < kMinShapeMaskResolution || settings.resolution > kMaxShapeMaskResolution ||
        (settings.resolution & (settings.resolution - 1))) {
        error = "Deposition Maskの設定が不正です"; return {};
    }
    ShapeMaskSettings raster;
    raster.resolution = settings.resolution; raster.distance = settings.distance; raster.samples = settings.samples;
    raster.low = 0; raster.high = 1;
    return SurfaceMask(mesh, raster, error, stop, progress, nullptr, &settings);
}
MaskImage NoiseMask(const Mesh& mesh,const NoiseMaskSettings& settings,std::string& error,
                    std::stop_token stop,const std::function<void(int)>& progress) {
    error.clear();
    const auto unit=[](float v){return std::isfinite(v) && v>=0 && v<=1;};
    if (!std::isfinite(settings.size) || settings.size<.001f || settings.size>1000 ||
        !unit(settings.contrast) || !unit(settings.detail) || !unit(settings.warp) ||
        settings.resolution<kMinShapeMaskResolution || settings.resolution>kMaxShapeMaskResolution ||
        (settings.resolution & (settings.resolution-1))) {
        error="Noise Maskの設定が不正です";return {};
    }
    ShapeMaskSettings raster;raster.type=ShapeMaskType::Height;raster.resolution=settings.resolution;
    raster.low=0;raster.high=1;
    return SurfaceMask(mesh,raster,error,stop,progress,&settings);
}
} // namespace rock::geometry
