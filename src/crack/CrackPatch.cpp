#include "crack/CrackPatch.h"

#include <algorithm>
#include <cmath>
#include <numbers>
namespace rock::crack {
namespace {
geometry::Vec3 Add(geometry::Vec3 a, geometry::Vec3 b, float amount) {
    return {a.x + b.x * amount, a.y + b.y * amount, a.z + b.z * amount};
}
geometry::Vec3 Rotate(geometry::Vec3 p, const std::array<float, 3>& degrees) {
    const float x = degrees[0] * std::numbers::pi_v<float> / 180;
    const float y = degrees[1] * std::numbers::pi_v<float> / 180;
    const float z = degrees[2] * std::numbers::pi_v<float> / 180;
    const geometry::Vec3 rz{std::cos(z) * p.x - std::sin(z) * p.y, std::sin(z) * p.x + std::cos(z) * p.y,
                            p.z};
    const geometry::Vec3 rx{rz.x, std::cos(x) * rz.y - std::sin(x) * rz.z,
                            std::sin(x) * rz.y + std::cos(x) * rz.z};
    return {std::cos(y) * rx.x + std::sin(y) * rx.z, rx.y, -std::sin(y) * rx.x + std::cos(y) * rx.z};
}
}  // namespace
bool BuildCrackPatch(const CrackSettings& s, CrackPatch& patch, std::string& error) {
    patch = {};
    error.clear();
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    for (float v : s.center)
        if (!range(v, -10000, 10000)) {
            error = "中心は有限の -10000～10000 m にしてください";
            return false;
        }
    for (float v : s.rotationDegrees)
        if (!range(v, -360, 360)) {
            error = "回転は有限の -360～360 度にしてください";
            return false;
        }
    if (!range(s.extentU, 0.001f, 1000) || !range(s.extentV, 0.001f, 1000) || !range(s.depth, 0, 2000) ||
        !range(s.persistence, 0, 1) || !range(s.aperture, 0, 100)) {
        error = "半幅は 0.001～1000 m、深さは 0～2000 m、Persistence は 0～1、開口は 0～100 m にしてください";
        return false;
    }
    patch.center = {s.center[0], s.center[1], s.center[2]};
    patch.tangentU = Rotate({1, 0, 0}, s.rotationDegrees);
    patch.tangentV = Rotate({0, 1, 0}, s.rotationDegrees);
    patch.normal = Rotate({0, 0, 1}, s.rotationDegrees);
    patch.effectiveDepth = std::min(s.depth, 2 * s.extentV) * s.persistence;
    patch.aperture = s.aperture;
    const auto top = Add(patch.center, patch.tangentV, s.extentV);
    patch.boundary = {Add(top, patch.tangentU, -s.extentU), Add(top, patch.tangentU, s.extentU),
                      Add(Add(top, patch.tangentV, -2 * s.extentV), patch.tangentU, s.extentU),
                      Add(Add(top, patch.tangentV, -2 * s.extentV), patch.tangentU, -s.extentU)};
    patch.reached = {patch.boundary[0], patch.boundary[1],
                     Add(patch.boundary[1], patch.tangentV, -patch.effectiveDepth),
                     Add(patch.boundary[0], patch.tangentV, -patch.effectiveDepth)};
    return true;
}
}  // namespace rock::crack
