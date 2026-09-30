#include "geometry/DetailTransfer.h"
#include "geometry/UvUnwrap.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <execution>
#include <limits>
#include <numeric>

namespace rock::geometry {
namespace {
struct V {
    double x = 0, y = 0, z = 0;
    double At(int axis) const { return axis == 0 ? x : axis == 1 ? y : z; }
    V operator+(V b) const { return {x + b.x, y + b.y, z + b.z}; }
    V operator-(V b) const { return {x - b.x, y - b.y, z - b.z}; }
    V operator*(double s) const { return {x * s, y * s, z * s}; }
};
V Convert(Vec3 p) { return {p.x, p.y, p.z}; }
double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V Cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V Normalize(V a, V fallback) {
    const double length = std::sqrt(Dot(a, a));
    return length > 1e-20 ? a * (1 / length) : fallback;
}
V Min(V a, V b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
V Max(V a, V b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

// 最も近い交点を探す BVH。葉は 4 枚以下。走査は明示のスタックで行う（深い再帰を避ける）。
struct ClosestBvh {
    struct Triangle { V a, b, c; };
    struct Node {
        V lo, hi;
        uint32_t begin = 0, end = 0;
        int left = -1, right = -1;
    };
    struct Hit {
        double t = std::numeric_limits<double>::max();
        uint32_t triangle = UINT32_MAX;
        double u = 0, w = 0;  // 重心座標（b と c の重み）
    };
    std::vector<Triangle> triangles;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;

    void Build(const Mesh& mesh) {
        triangles.reserve(mesh.triangles.size());
        std::vector<V> centers;
        centers.reserve(mesh.triangles.size());
        for (const auto& f : mesh.triangles) {
            const V a = Convert(mesh.positions[f[0]]), b = Convert(mesh.positions[f[1]]), c = Convert(mesh.positions[f[2]]);
            triangles.push_back({a, b, c});
            centers.push_back((a + b + c) * (1.0 / 3));
        }
        order.resize(triangles.size());
        std::iota(order.begin(), order.end(), 0u);
        nodes.reserve(triangles.size() / 2 + 1);
        // 再帰の代わりに作業の列で組む。
        struct Task { uint32_t begin, end; int node; };
        std::vector<Task> tasks{{0, uint32_t(triangles.size()), -1}};
        while (!tasks.empty()) {
            const Task task = tasks.back();
            tasks.pop_back();
            Node node;
            node.begin = task.begin;
            node.end = task.end;
            node.lo = node.hi = triangles[order[task.begin]].a;
            for (uint32_t i = task.begin; i < task.end; ++i) {
                const auto& t = triangles[order[i]];
                node.lo = Min(node.lo, Min(t.a, Min(t.b, t.c)));
                node.hi = Max(node.hi, Max(t.a, Max(t.b, t.c)));
            }
            const int id = int(nodes.size());
            nodes.push_back(node);
            if (task.node >= 0) {
                if (nodes[task.node].left < 0) nodes[task.node].left = id;
                else nodes[task.node].right = id;
            }
            if (task.end - task.begin <= 4) continue;
            const V extent = node.hi - node.lo;
            int axis = extent.y > extent.x ? 1 : 0;
            if (extent.z > extent.At(axis)) axis = 2;
            const uint32_t mid = (task.begin + task.end) / 2;
            std::nth_element(order.begin() + task.begin, order.begin() + mid, order.begin() + task.end,
                             [&](uint32_t a, uint32_t b) { return centers[a].At(axis) < centers[b].At(axis); });
            // 右を先に積み、左を先に取り出す（left に先に入るように）。
            tasks.push_back({mid, task.end, id});
            tasks.push_back({task.begin, mid, id});
        }
    }
    // origin から direction へ、distance までで最も近い交点。
    Hit Closest(V origin, V direction, double distance) const {
        Hit best;
        best.t = distance;
        int stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& n = nodes[stack[--top]];
            double tNear = 0, tFar = best.t;
            bool inside = true;
            for (int axis = 0; axis < 3 && inside; ++axis) {
                const double d = direction.At(axis), o = origin.At(axis);
                if (std::abs(d) < 1e-15) {
                    inside = o >= n.lo.At(axis) && o <= n.hi.At(axis);
                } else {
                    double a = (n.lo.At(axis) - o) / d, b = (n.hi.At(axis) - o) / d;
                    if (a > b) std::swap(a, b);
                    tNear = std::max(tNear, a);
                    tFar = std::min(tFar, b);
                    inside = tNear <= tFar;
                }
            }
            if (!inside) continue;
            if (n.left >= 0) {
                if (top + 2 > 64) continue;  // 深すぎる木（あり得ないが、溢れさせない）
                stack[top++] = n.right;
                stack[top++] = n.left;
                continue;
            }
            for (uint32_t i = n.begin; i < n.end; ++i) {
                const auto& t = triangles[order[i]];
                const V e1 = t.b - t.a, e2 = t.c - t.a, p = Cross(direction, e2);
                const double det = Dot(e1, p);
                if (std::abs(det) < 1e-14 * std::sqrt(Dot(e1, e1) * Dot(e2, e2))) continue;
                const V s = origin - t.a;
                const double u = Dot(s, p) / det;
                if (u < 0 || u > 1) continue;
                const V q = Cross(s, e1);
                const double w = Dot(direction, q) / det;
                if (w < 0 || u + w > 1) continue;
                const double ray = Dot(e2, q) / det;
                if (ray >= 0 && ray < best.t) best = {ray, order[i], u, w};
            }
        }
        if (best.triangle == UINT32_MAX) best.t = std::numeric_limits<double>::max();
        return best;
    }
};

// 面積で重み付けした頂点法線。
std::vector<V> VertexNormals(const Mesh& mesh) {
    std::vector<V> normals(mesh.positions.size());
    for (const auto& f : mesh.triangles) {
        const V a = Convert(mesh.positions[f[0]]), b = Convert(mesh.positions[f[1]]), c = Convert(mesh.positions[f[2]]);
        const V n = Cross(b - a, c - a);
        for (const auto index : f) normals[index] = normals[index] + n;
    }
    for (auto& n : normals) n = Normalize(n, {0, 1, 0});
    return normals;
}
}  // namespace

size_t DetailTransferImage::CoveredCount() const { return size_t(std::count(covered.begin(), covered.end(), uint8_t(1))); }
size_t DetailTransferImage::HitCount() const { return size_t(std::count(hit.begin(), hit.end(), uint8_t(1))); }

namespace {
// ローポリの UV の画素の点での向き（接線・従接線・法線）。
struct Frame {
    V t, b, n;
};

// 角の向きを、面の中の点（重心座標の b と c の重み）へ補間する。接線空間は PsBake と同じ取り方。
Frame Interpolate(const std::array<CornerFrame, 3>& frame, double wb, double wc, V faceNormal) {
    const double wa = 1.0 - wb - wc;
    const V n = Normalize(Convert(frame[0].normal) * wa + Convert(frame[1].normal) * wb + Convert(frame[2].normal) * wc, faceNormal);
    const V rawTangent = Convert(frame[0].tangent) * wa + Convert(frame[1].tangent) * wb + Convert(frame[2].tangent) * wc;
    const V t = Normalize(rawTangent - n * Dot(n, rawTangent), Normalize(Cross({0, 1, 0}, n), {1, 0, 0}));
    const double sign = frame[0].sign * wa + frame[1].sign * wb + frame[2].sign * wc < 0 ? -1.0 : 1.0;
    return {t, Cross(n, t) * sign, n};
}

V UnitFaceNormal(const Mesh& mesh, const std::array<uint32_t, 3>& face) {
    return Normalize(Cross(Convert(mesh.positions[face[1]]) - Convert(mesh.positions[face[0]]),
                           Convert(mesh.positions[face[2]]) - Convert(mesh.positions[face[0]])),
                     {0, 1, 0});
}

// 面積で重み付けした頂点法線と、UV から求めた接線。向きを渡されなかったときに使う。
std::vector<std::array<CornerFrame, 3>> FallbackFrames(const Mesh& mesh) {
    const auto normals = VertexNormals(mesh);
    std::vector<std::array<CornerFrame, 3>> frames(mesh.triangles.size());
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        const auto& face = mesh.triangles[f];
        const auto& uv = mesh.cornerUvs[f];
        const V a = Convert(mesh.positions[face[0]]), b = Convert(mesh.positions[face[1]]), c = Convert(mesh.positions[face[2]]);
        const double du1 = uv[1].u - uv[0].u, dv1 = uv[1].v - uv[0].v, du2 = uv[2].u - uv[0].u, dv2 = uv[2].v - uv[0].v;
        const double determinant = du1 * dv2 - du2 * dv1;
        V tangent = Normalize(b - a, {1, 0, 0}), bitangent{};
        if (std::abs(determinant) > 1e-20) {
            tangent = ((b - a) * dv2 - (c - a) * dv1) * (1 / determinant);
            bitangent = ((c - a) * du1 - (b - a) * du2) * (1 / determinant);
        }
        for (int k = 0; k < 3; ++k) {
            const V n = normals[face[k]];
            const V t = Normalize(tangent - n * Dot(n, tangent), Normalize(Cross({0, 1, 0}, n), {1, 0, 0}));
            frames[f][k] = {{float(n.x), float(n.y), float(n.z)}, {float(t.x), float(t.y), float(t.z)},
                            Dot(Cross(n, t), bitangent) < 0 ? -1.0f : 1.0f};
        }
    }
    return frames;
}

// 転写の共通部分。ローポリの UV の画素ごとに、その画素に当たる面の点から、法線の向きにケージ距離だけ外側の点から
// 内側へレイを飛ばし、最初に当たったハイポリの面を探す。画素ごとに visit(index, frame, hit) を呼ぶ
// （hit.triangle が UINT32_MAX なら当たらなかった）。covered には UV の中の画素に 1 を書く。
template <class Visit>
bool Trace(const Mesh& low, const std::vector<std::array<CornerFrame, 3>>& frames, const Mesh& high, float cageDistance,
           uint32_t width, uint32_t height, std::vector<uint8_t>& covered, std::string& error, std::stop_token stop,
           const std::function<void(int)>& progress, Visit&& visit) {
    MeshInfo lowInfo, highInfo;
    if (!HasValidUvs(low) || !InspectMesh(low, lowInfo)) {
        error = "ローポリには UV 付きの Mesh が必要です（UV Unwrap の出力）";
        return false;
    }
    if (high.triangles.empty() || !InspectMesh(high, highInfo)) {
        error = "High に三角形を持つ Mesh を接続してください";
        return false;
    }
    if (!frames.empty() && frames.size() != low.triangles.size()) {
        error = "ローポリの向きの数が三角形の数と合いません";
        return false;
    }
    if (!std::isfinite(cageDistance) || cageDistance < kMinCageDistance || cageDistance > kMaxCageDistance) {
        error = "ケージ距離は 0.0001〜100 m にしてください";
        return false;
    }
    if (width == 0 || height == 0 || width > 8192 || height > 8192) {
        error = "転写の画像サイズが不正です";
        return false;
    }
    if (stop.stop_requested()) {
        error = "転写をキャンセルしました";
        return false;
    }
    ClosestBvh bvh;
    bvh.Build(high);
    std::vector<std::array<CornerFrame, 3>> fallback;
    const auto* cornerFrames = &frames;
    if (frames.empty()) {
        fallback = FallbackFrames(low);
        cornerFrames = &fallback;
    }

    // 画素の中心がどの面のどこに当たるかを先に決める（直列。辺を共有する面が同じ画素を取り合うので、結果を再現させる）。
    struct Texel {
        uint32_t face;
        float b, c;
    };
    constexpr uint32_t kNoFace = UINT32_MAX;
    std::vector<Texel> texels(size_t(width) * height, Texel{kNoFace, 0, 0});
    const auto cross = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
    for (size_t f = 0; f < low.triangles.size(); ++f) {
        const auto& uv = low.cornerUvs[f];
        const double area = cross(uv[1].u - uv[0].u, uv[1].v - uv[0].v, uv[2].u - uv[0].u, uv[2].v - uv[0].v);
        if (std::abs(area) < 1e-16) continue;
        const int x0 = std::max(0, int(std::floor(std::min({uv[0].u, uv[1].u, uv[2].u}) * width))),
                  x1 = std::min(int(width) - 1, int(std::ceil(std::max({uv[0].u, uv[1].u, uv[2].u}) * width)));
        const int y0 = std::max(0, int(std::floor(std::min({uv[0].v, uv[1].v, uv[2].v}) * height))),
                  y1 = std::min(int(height) - 1, int(std::ceil(std::max({uv[0].v, uv[1].v, uv[2].v}) * height)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double u = (x + .5) / width - uv[0].u, v = (y + .5) / height - uv[0].v;
                const double b = cross(u, v, uv[2].u - uv[0].u, uv[2].v - uv[0].v) / area,
                             c = cross(uv[1].u - uv[0].u, uv[1].v - uv[0].v, u, v) / area;
                if (b < 0 || c < 0 || b + c > 1) continue;
                texels[size_t(y) * width + x] = {uint32_t(f), float(b), float(c)};
            }
    }

    covered.assign(texels.size(), 0);
    const double cage = cageDistance;
    std::vector<uint32_t> rows(height);
    std::iota(rows.begin(), rows.end(), 0u);
    std::atomic<uint32_t> done{0};
    std::atomic<bool> cancelled{false};
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](uint32_t y) {
        if (cancelled || stop.stop_requested()) {
            cancelled = true;
            return;
        }
        for (uint32_t x = 0; x < width; ++x) {
            const size_t index = size_t(y) * width + x;
            const Texel texel = texels[index];
            if (texel.face == kNoFace) continue;
            covered[index] = 1;
            const auto& face = low.triangles[texel.face];
            const double wa = 1.0 - texel.b - texel.c;
            const V p = Convert(low.positions[face[0]]) * wa + Convert(low.positions[face[1]]) * texel.b +
                        Convert(low.positions[face[2]]) * texel.c;
            const Frame frame = Interpolate((*cornerFrames)[texel.face], texel.b, texel.c, UnitFaceNormal(low, face));
            // 外側（ケージ）から内側へ。最初に当たったハイポリの面を使う。
            visit(index, frame, bvh.Closest(p + frame.n * cage, frame.n * -1.0, 2 * cage));
        }
        if (progress) progress(int(++done * 100 / height));
    });
    if (cancelled || stop.stop_requested()) {
        error = "転写をキャンセルしました";
        return false;
    }
    return true;
}

// RGBA8 の画像を双線形で読む（行 0 が v = 0。端は繰り返さずに留める）。
std::array<double, 4> Sample(const TextureView& image, double u, double v) {
    const double x = std::clamp(u * image.width - .5, 0.0, double(image.width - 1)),
                 y = std::clamp(v * image.height - .5, 0.0, double(image.height - 1));
    const uint32_t x0 = uint32_t(x), y0 = uint32_t(y);
    const uint32_t x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    const double fx = x - x0, fy = y - y0;
    std::array<double, 4> result{};
    for (int c = 0; c < 4; ++c) {
        const auto at = [&](uint32_t px, uint32_t py) { return double(image.pixels[(size_t(py) * image.width + px) * 4 + c]); };
        result[c] = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
    }
    return result;
}
}  // namespace

bool TransferDetail(const Mesh& low, const std::vector<std::array<CornerFrame, 3>>& frames, const Mesh& high,
                    float cageDistance, uint32_t width, uint32_t height, DetailTransferImage& out, std::string& error,
                    std::stop_token stop, const std::function<void(int)>& progress) {
    error.clear();
    out = {};
    const std::vector<V> highNormals = high.triangles.empty() ? std::vector<V>{} : VertexNormals(high);
    const size_t count = size_t(width) * height;
    out.normals.assign(count, Vec3{0, 0, 1});
    out.heights.assign(count, 0.0f);
    out.hit.assign(count, 0);
    const double cage = cageDistance;
    const bool ok = Trace(low, frames, high, cageDistance, width, height, out.covered, error, stop, progress,
                          [&](size_t index, const Frame& frame, const ClosestBvh::Hit& found) {
        if (found.triangle == UINT32_MAX) return;
        const auto& highFace = high.triangles[found.triangle];
        const V highNormal = Normalize(highNormals[highFace[0]] * (1 - found.u - found.w) + highNormals[highFace[1]] * found.u +
                                           highNormals[highFace[2]] * found.w,
                                       frame.n);
        const V local = Normalize({Dot(highNormal, frame.t), Dot(highNormal, frame.b), Dot(highNormal, frame.n)}, {0, 0, 1});
        out.normals[index] = {float(local.x), float(local.y), float(local.z)};
        out.heights[index] = float(cage - found.t);
        out.hit[index] = 1;
    });
    if (!ok) {
        out = {};
        return false;
    }
    out.width = width;
    out.height = height;
    return true;
}

bool TransferTextures(const Mesh& low, const std::vector<std::array<CornerFrame, 3>>& lowFrames, const Mesh& high,
                      const std::vector<std::array<CornerFrame, 3>>& highFrames, const std::array<TextureView, 4>& highImages,
                      float cageDistance, uint32_t width, uint32_t height, TextureTransferResult& out, std::string& error,
                      std::stop_token stop, const std::function<void(int)>& progress) {
    error.clear();
    out = {};
    if (!HasValidUvs(high)) {
        error = "転写元には UV 付きの Mesh が必要です";
        return false;
    }
    for (const auto& image : highImages)
        if (!image.pixels || image.width == 0 || image.height == 0) {
            error = "転写元のテクスチャがありません";
            return false;
        }
    // 転写元の法線マップを読むための角の向き。渡されなければ頂点法線と UV の接線を使う。
    std::vector<std::array<CornerFrame, 3>> fallback;
    const auto* sourceFrames = &highFrames;
    if (highFrames.empty()) {
        fallback = FallbackFrames(high);
        sourceFrames = &fallback;
    } else if (highFrames.size() != high.triangles.size()) {
        error = "転写元の向きの数が三角形の数と合いません";
        return false;
    }
    const size_t count = size_t(width) * height;
    for (auto& image : out.images) image.assign(count * 4, 0);
    std::vector<uint8_t> hit(count, 0);
    const bool ok = Trace(low, lowFrames, high, cageDistance, width, height, out.covered, error, stop, progress,
                          [&](size_t index, const Frame& frame, const ClosestBvh::Hit& found) {
        if (found.triangle == UINT32_MAX) return;
        const double wb = found.u, wc = found.w, wa = 1 - wb - wc;
        const auto& uv = high.cornerUvs[found.triangle];
        const double u = uv[0].u * wa + uv[1].u * wb + uv[2].u * wc, v = uv[0].v * wa + uv[1].v * wb + uv[2].v * wc;
        const auto store = [&](size_t channel, double r, double g, double b) {
            uint8_t* pixel = &out.images[channel][index * 4];
            pixel[0] = uint8_t(std::lround(std::clamp(r, 0.0, 255.0)));
            pixel[1] = uint8_t(std::lround(std::clamp(g, 0.0, 255.0)));
            pixel[2] = uint8_t(std::lround(std::clamp(b, 0.0, 255.0)));
            pixel[3] = 255;
        };
        for (const size_t channel : {size_t(0), size_t(2), size_t(3)}) {
            const auto value = Sample(highImages[channel], u, v);
            store(channel, value[0], value[1], value[2]);
        }
        // 法線: 転写元の接線空間 → ワールド → ローポリの接線空間。
        const auto encoded = Sample(highImages[1], u, v);
        const V local = Normalize({encoded[0] / 255 * 2 - 1, encoded[1] / 255 * 2 - 1, encoded[2] / 255 * 2 - 1}, {0, 0, 1});
        const Frame source = Interpolate((*sourceFrames)[found.triangle], wb, wc, UnitFaceNormal(high, high.triangles[found.triangle]));
        const V world = Normalize(source.t * local.x + source.b * local.y + source.n * local.z, frame.n);
        const V result = Normalize({Dot(world, frame.t), Dot(world, frame.b), Dot(world, frame.n)}, {0, 0, 1});
        store(1, (result.x * .5 + .5) * 255, (result.y * .5 + .5) * 255, (result.z * .5 + .5) * 255);
        hit[index] = 1;
    });
    if (!ok) {
        out = {};
        return false;
    }
    out.width = width;
    out.height = height;
    out.hits = size_t(std::count(hit.begin(), hit.end(), uint8_t(1)));
    return true;
}
}  // namespace rock::geometry
