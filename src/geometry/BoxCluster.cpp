#include "geometry/BoxCluster.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rock::geometry {
namespace {
Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Rotate(Vec3 p, float x, float y, float z) {
    const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z),
                sz = std::sin(z);
    p = {cz * p.x - sz * p.y, sz * p.x + cz * p.y, p.z};
    p = {p.x, cx * p.y - sx * p.z, sx * p.y + cx * p.z};
    return {cy * p.x + sy * p.z, p.y, -sy * p.x + cy * p.z};
}
}  // namespace
std::vector<OrientedBox> MakeBoxCluster(const BoxClusterSettings& s, std::string& error) {
    error.clear();
    if (s.count < 1 || s.count > 32 || !std::isfinite(s.sizeVariation) || s.sizeVariation < 0 ||
        s.sizeVariation > .8f || !std::isfinite(s.spread) || s.spread < 0 || s.spread > .95f ||
        !std::isfinite(s.rotation) || s.rotation < 0 || s.rotation > 90 ||
        std::any_of(s.size.begin(), s.size.end(),
                    [](float v) { return !std::isfinite(v) || v < .1f || v > 100; })) {
        error = "個数1～32、寸法0.1～100m、サイズばらつき0～0.8、広がり0～0.95、回転0～90度にしてください";
        return {};
    }
    uint32_t state = static_cast<uint32_t>(s.seed);
    const auto random = [&]() {
        state += 0x9e3779b9u;
        uint32_t v = state;
        v = (v ^ (v >> 16)) * 0x21f0aaadu;
        v = (v ^ (v >> 15)) * 0x735a2d97u;
        return float((v ^ (v >> 15)) >> 8) / 16777216.f;
    };
    std::vector<OrientedBox> boxes;
    for (int i = 0; i < s.count; ++i) {
        OrientedBox box{};
        const float x = (random() * 2 - 1) * s.rotation * .01745329252f;
        const float y = (random() * 2 - 1) * s.rotation * .01745329252f;
        const float z = (random() * 2 - 1) * s.rotation * .01745329252f;
        box.axes = {Rotate({1, 0, 0}, x, y, z), Rotate({0, 1, 0}, x, y, z), Rotate({0, 0, 1}, x, y, z)};
        for (int axis = 0; axis < 3; ++axis) {
            box.halfSize[axis] = s.size[axis] * .5f * (i == 0 ? 1 : 1 - s.sizeVariation * random());
            if (i > 0)
                box.center = Add(box.center, Scale(boxes[0].axes[axis],
                                                   (random() * 2 - 1) * boxes[0].halfSize[axis] * s.spread));
        }
        boxes.push_back(box);
    }
    return boxes;
}
Mesh BoxClusterPreview(const std::vector<OrientedBox>& boxes) {
    Mesh result;
    for (const auto& box : boxes) {
        auto mesh = MakeBox({box.halfSize[0] * 2, box.halfSize[1] * 2, box.halfSize[2] * 2});
        const auto offset = static_cast<uint32_t>(result.positions.size());
        for (auto p : mesh.positions)
            result.positions.push_back(
                Add(box.center,
                    Add(Scale(box.axes[0], p.x), Add(Scale(box.axes[1], p.y), Scale(box.axes[2], p.z)))));
        for (const auto& t : mesh.triangles)
            result.triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
    }
    return result;
}
float BoxUnionField(Vec3 p, const std::vector<OrientedBox>& boxes) {
    float field = std::numeric_limits<float>::infinity();
    for (const auto& box : boxes) {
        const Vec3 d{p.x - box.center.x, p.y - box.center.y, p.z - box.center.z};
        const float x = std::abs(Dot(d, box.axes[0])) - box.halfSize[0],
                    y = std::abs(Dot(d, box.axes[1])) - box.halfSize[1],
                    z = std::abs(Dot(d, box.axes[2])) - box.halfSize[2];
        const float ox = std::max(0.f, x), oy = std::max(0.f, y), oz = std::max(0.f, z);
        field = std::min(field, std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(0.f, std::max({x, y, z})));
    }
    return field;
}
}  // namespace rock::geometry
