#include "crack/JointSet.h"
#include <cmath>
#include <cstdint>
namespace rock::crack {
namespace {
float Random(int seed, int index, uint32_t channel) {
    uint32_t x = static_cast<uint32_t>(seed) ^ (static_cast<uint32_t>(index) * 0x9e3779b9u) ^ channel;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<float>(x >> 8) / 16777215.0f * 2 - 1;
}
geometry::Vec3 Rotate(geometry::Vec3 p, const CrackPatch& b) {
    return {p.x * b.tangentU.x + p.y * b.tangentV.x + p.z * b.normal.x,
            p.x * b.tangentU.y + p.y * b.tangentV.y + p.z * b.normal.y,
            p.x * b.tangentU.z + p.y * b.tangentV.z + p.z * b.normal.z};
}
}  // namespace
bool BuildJointSet(const JointSetSettings& s, std::vector<CrackPatch>& patches, std::string& error) {
    patches.clear();
    error.clear();
    const auto range = [](float x, float lo, float hi) { return std::isfinite(x) && x >= lo && x <= hi; };
    if (s.count < 1 || s.count > 64 || !range(s.spacing, 0.001f, 1000) ||
        !range(s.spacingVariance, 0, 0.49f) || !range(s.angleVariance, 0, 30) ||
        !range(s.offset, -10000, 10000)) {
        error =
            "本数は1～64、間隔は0.001～1000 "
            "m、位置ばらつきは0～0.49、角度ばらつきは0～30度、オフセットは±10000 mにしてください";
        return false;
    }
    CrackSettings frameSettings;
    frameSettings.center = s.center;
    frameSettings.rotationDegrees = s.rotationDegrees;
    CrackPatch frame;
    if (!BuildCrackPatch(frameSettings, frame, error)) return false;
    std::vector<CrackPatch> generated;
    for (int i = 0; i < s.count; ++i) {
        CrackSettings local;
        local.extentU = s.extentU;
        local.extentV = s.extentV;
        local.depth = s.depth;
        local.persistence = s.persistence;
        local.aperture = s.aperture;
        local.rotationDegrees = {Random(s.seed, i, 0x12345678u) * s.angleVariance,
                                 Random(s.seed, i, 0x87654321u) * s.angleVariance, 0};
        CrackPatch patch;
        if (!BuildCrackPatch(local, patch, error)) return false;
        const float distance = s.offset + (i - (s.count - 1) * 0.5f) * s.spacing +
                               Random(s.seed, i, 0xabcd1234u) * s.spacing * s.spacingVariance;
        const geometry::Vec3 center{frame.center.x + distance * frame.normal.x,
                                    frame.center.y + distance * frame.normal.y,
                                    frame.center.z + distance * frame.normal.z};
        if (!range(center.x, -10000, 10000) || !range(center.y, -10000, 10000) ||
            !range(center.z, -10000, 10000)) {
            error = "生成される節理中心が±10000 mを超えます。間隔・本数・中心を小さくしてください";
            return false;
        }
        const auto point = [&](geometry::Vec3 p) {
            p = Rotate(p, frame);
            return geometry::Vec3{p.x + center.x, p.y + center.y, p.z + center.z};
        };
        for (auto& p : patch.boundary) p = point(p);
        for (auto& p : patch.reached) p = point(p);
        patch.center = center;
        patch.normal = Rotate(patch.normal, frame);
        patch.tangentU = Rotate(patch.tangentU, frame);
        patch.tangentV = Rotate(patch.tangentV, frame);
        generated.push_back(patch);
    }
    patches = std::move(generated);
    return true;
}
}  // namespace rock::crack
