#include "renderer/CrackGuide.h"
namespace rock::renderer {
std::vector<OverlayLineSet> MakeCrackGuides(const crack::CrackPatch& patch) {
    std::vector<OverlayLineSet> result;
    const auto rectangle = [&](const auto& corners, DirectX::XMFLOAT4 color, bool fill) {
        OverlayLineSet set;
        set.color = color;
        set.triangles = fill;
        set.depthTest = false;
        const auto add = [&](size_t i) {
            const auto p = corners[i];
            set.points.push_back({p.x, p.y, p.z});
        };
        if (fill)
            for (size_t i : {0, 1, 2, 0, 2, 3}) add(i);
        else
            for (size_t i = 0; i < 4; ++i) {
                add(i);
                add((i + 1) % 4);
            }
        result.push_back(std::move(set));
    };
    rectangle(patch.boundary, {0.2f, 0.65f, 1, 0.12f}, true);
    if (patch.effectiveDepth > 0) rectangle(patch.reached, {1, 0.5f, 0.12f, 0.28f}, true);
    rectangle(patch.boundary, {0.2f, 0.65f, 1, 0.9f}, false);
    if (patch.effectiveDepth > 0) rectangle(patch.reached, {1, 0.65f, 0.2f, 1}, false);
    // 開口は法線方向の幅。実際の切断ではなく到達領域の厚みを線で示す。
    if (patch.aperture > 0 && patch.effectiveDepth > 0) {
        OverlayLineSet box;
        box.color = {1, 0.7f, 0.3f, 0.65f};
        box.depthTest = false;
        const auto point = [&](size_t i, float sign) {
            const auto p = patch.reached[i], n = patch.normal;
            const float d = patch.aperture * 0.5f * sign;
            return DirectX::XMFLOAT3{p.x + n.x * d, p.y + n.y * d, p.z + n.z * d};
        };
        for (size_t i = 0; i < 4; ++i) {
            box.points.push_back(point(i, -1));
            box.points.push_back(point(i, 1));
            for (float sign : {-1.0f, 1.0f}) {
                box.points.push_back(point(i, sign));
                box.points.push_back(point((i + 1) % 4, sign));
            }
        }
        result.push_back(std::move(box));
    }
    return result;
}
}  // namespace rock::renderer
