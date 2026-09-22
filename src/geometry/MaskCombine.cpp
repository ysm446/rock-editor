#include "geometry/ShapeMask.h"
#include <algorithm>
#include <cmath>

namespace rock::geometry {
MaskImage CombineMasks(const MaskImage& a, bool invertA, const MaskImage& b, bool invertB,
                       const MaskCombineSettings& settings, std::string& error) {
    error.clear();
    const auto valid = [](const MaskImage& m) {
        return m.width > 0 && m.height > 0 && m.pixels.size() == size_t(m.width) * m.height;
    };
    if (!valid(a) || !valid(b)) { error = "合成するマスクの画像がありません"; return {}; }
    if (!std::isfinite(settings.mix) || settings.mix < 0 || settings.mix > 1 ||
        !std::isfinite(settings.low) || !std::isfinite(settings.high) ||
        settings.low < 0 || settings.high > 1 || settings.high - settings.low < .001f ||
        !std::isfinite(settings.gamma) || settings.gamma < .1f || settings.gamma > 10 ||
        static_cast<uint32_t>(settings.operation) > static_cast<uint32_t>(MaskCombineOperation::Mix)) {
        error = "Mask Combineの設定が不正です";
        return {};
    }
    MaskImage out;
    out.width = std::max(a.width, b.width);
    out.height = std::max(a.height, b.height);
    out.pixels.resize(size_t(out.width) * out.height);
    // 同じ解像度なら画素の中心を読むので線形補間は元の値をそのまま返す。
    const auto read = [](const MaskImage& m, bool invert, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        float v;
        if (m.width == w && m.height == h) v = m.pixels[size_t(y) * w + x] / 255.f;
        else v = m.Sample((x + .5f) / w, (y + .5f) / h);
        return invert ? 1 - v : v;
    };
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            const float va = read(a, invertA, x, y, out.width, out.height);
            const float vb = read(b, invertB, x, y, out.width, out.height);
            float v;
            switch (settings.operation) {
            case MaskCombineOperation::Maximum: v = std::max(va, vb); break;
            case MaskCombineOperation::Minimum: v = std::min(va, vb); break;
            case MaskCombineOperation::Subtract: v = std::max(va - vb, 0.f); break;
            case MaskCombineOperation::Mix: v = std::lerp(va, vb, settings.mix); break;
            default: v = va * vb; break;
            }
            double level = std::clamp((v - settings.low) / double(settings.high - settings.low), 0., 1.);
            if (settings.gamma != 1) level = std::pow(level, double(settings.gamma));
            if (settings.invert) level = 1 - level;
            out.pixels[size_t(y) * out.width + x] = uint8_t(std::lround(255 * level));
        }
    return out;
}
}  // namespace rock::geometry
