#include "geometry/BakeOcclusion.h"
#include "geometry/UvUnwrap.h"
#include <algorithm>
#include <cmath>
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
} // namespace rock::geometry
