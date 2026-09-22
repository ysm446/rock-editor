#include "geometry/BakeOcclusion.h"
#include "geometry/ShapeMask.h"
#include "geometry/UvUnwrap.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <execution>
#include <numeric>
#include <numbers>
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
MaskImage ShapeMask(const Mesh &mesh, const ShapeMaskSettings &settings, std::string &error,
                    std::stop_token stop, const std::function<void(int)> &progress) {
    error.clear();
    const int resolution = settings.resolution;
    const bool occlusion = settings.type == ShapeMaskType::Occlusion;
    if ((settings.type != ShapeMaskType::Occlusion && settings.type != ShapeMaskType::Direction &&
         settings.type != ShapeMaskType::Height) ||
        !std::isfinite(settings.distance) || settings.distance < .001f || settings.distance > 1000 ||
        settings.samples < kMinOcclusionSamples || settings.samples > kMaxOcclusionSamples ||
        resolution < kMinShapeMaskResolution || resolution > kMaxShapeMaskResolution ||
        (resolution & (resolution - 1)) != 0 || !std::isfinite(settings.low) || !std::isfinite(settings.high) ||
        settings.low < 0 || settings.high > 1 || settings.high - settings.low < .001f) {
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
            if (settings.type == ShapeMaskType::Direction) {
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
            }
            const double level = std::clamp((ratio - settings.low) / double(settings.high - settings.low), 0., 1.);
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
} // namespace rock::geometry
