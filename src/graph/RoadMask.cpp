#include "graph/RoadMask.h"
#include "graph/Road.h"

#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {

// 整数格子のハッシュ。値ノイズ用。
float HashNoise(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// 2 次元の値ノイズ（0〜1）。
float ValueNoise(float x, float y, uint32_t seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    const float tx = Smooth(x - fx);
    const float ty = Smooth(y - fy);
    const float a = HashNoise(ix, iy, seed);
    const float b = HashNoise(ix + 1, iy, seed);
    const float c = HashNoise(ix, iy + 1, seed);
    const float d = HashNoise(ix + 1, iy + 1, seed);
    return (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (c * (1.0f - tx) + d * tx) * ty;
}

// 3 オクターブの fbm（0〜1 に正規化）。
float Fbm(float x, float y, uint32_t seed) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    for (int octave = 0; octave < 3; ++octave) {
        sum += ValueNoise(x, y, seed + static_cast<uint32_t>(octave) * 101u) * amplitude;
        total += amplitude;
        x *= 2.0f;
        y *= 2.0f;
        amplitude *= 0.5f;
    }
    return sum / total;
}

// 中心 center、幅 width の帯。縁を feather でなだらかにする。
float Band(float x, float center, float width, float feather) {
    const float distance = std::abs(x - center) - width * 0.5f;
    if (feather <= 1e-5f) return distance <= 0.0f ? 1.0f : 0.0f;
    return std::clamp(1.0f - distance / feather, 0.0f, 1.0f);
}

}  // namespace

float EvaluateRoadMask(const RoadMaskNodeSettings& settings, float lateralMeters, float distanceMeters,
                       float halfWidthMeters, float lengthMeters, const RoadLanes* lanes,
                       float worldX, float worldZ, bool hasWorld) {
    (void)lengthMeters;
    float value = 0.0f;
    switch (settings.shape) {
        case RoadMaskShape::WheelTracks: {
            // 車線中央から左右へ trackSpacing / 2 の 2 本。
            // 車線に合わせるときは Road の各車線（対向を含めるかは bothLanes）、手入力なら中心線 ± laneOffset。
            std::vector<float> centers;
            if (settings.tracksFromLanes && lanes != nullptr && !lanes->laneCenters.empty()) {
                for (size_t i = 0; i < lanes->laneCenters.size(); ++i) {
                    if (settings.bothLanes || lanes->laneForward[i]) centers.push_back(lanes->laneCenters[i]);
                }
            } else {
                centers.push_back(settings.laneOffsetMeters);
                if (settings.bothLanes) centers.push_back(-settings.laneOffsetMeters);
            }
            for (const float laneCenter : centers) {
                for (int side = -1; side <= 1; side += 2) {
                    const float center = laneCenter + static_cast<float>(side) * settings.trackSpacingMeters * 0.5f;
                    value = std::max(value, Band(lateralMeters, center, settings.trackWidthMeters, settings.featherMeters));
                }
            }
            break;
        }
        case RoadMaskShape::EdgeFalloff: {
            // 道路端からの距離。端で 1、edgeWidth の内側から feather で 0 へ。
            // 横位置は正が Left（列末尾側）。側を選ぶと反対側の端は遠い扱いになり 0 のまま。
            float fromEdge = halfWidthMeters - std::abs(lateralMeters);
            if (settings.edgeSide == RoadMaskSide::Left) fromEdge = halfWidthMeters - lateralMeters;
            if (settings.edgeSide == RoadMaskSide::Right) fromEdge = halfWidthMeters + lateralMeters;
            const float inner = fromEdge - settings.edgeWidthMeters;
            value = settings.featherMeters <= 1e-5f ? (inner <= 0.0f ? 1.0f : 0.0f)
                                                    : std::clamp(1.0f - inner / settings.featherMeters, 0.0f, 1.0f);
            break;
        }
        case RoadMaskShape::LengthNoise: {
            const float scale = std::max(settings.noiseScaleMeters, 0.05f);
            const float noise = Fbm(distanceMeters / scale, lateralMeters / scale, settings.seed);
            const float softness = std::max(settings.softness, 1e-4f);
            value = std::clamp((noise - settings.threshold) / softness + 0.5f, 0.0f, 1.0f);
            break;
        }
        case RoadMaskShape::WorldNoise: {
            // 方向性の無いノイズ。ワールド座標で評価するので、左右の路肩や隣の道路と模様が連続する。
            const float scale = std::max(settings.noiseScaleMeters, 0.05f);
            const float x = hasWorld ? worldX : lateralMeters;
            const float z = hasWorld ? worldZ : distanceMeters;
            const float noise = Fbm(x / scale, z / scale, settings.seed);
            const float softness = std::max(settings.softness, 1e-4f);
            value = std::clamp((noise - settings.threshold) / softness + 0.5f, 0.0f, 1.0f);
            break;
        }
        case RoadMaskShape::Constant:
        default:
            value = 1.0f;
            break;
    }
    // 長さ方向のムラ。0 で一様、1 でノイズそのまま。
    if (settings.breakupAmount > 1e-4f) {
        const float scale = std::max(settings.breakupScaleMeters, 0.05f);
        const float noise = Fbm(distanceMeters / scale, lateralMeters / (scale * 2.0f), settings.seed + 7919u);
        value *= 1.0f - settings.breakupAmount + settings.breakupAmount * noise;
    }
    value = std::clamp(value * settings.strength, 0.0f, 1.0f);
    return settings.invert ? 1.0f - value : value;
}

RoadMaskImage BakeRoadMask(const RoadMaskNodeSettings* const channels[3], float widthMeters,
                           float lengthMeters, const RoadLanes* lanes, const RoadGeometry* geometry) {
    RoadMaskImage image;
    if (!(widthMeters > 0.0f) || !(lengthMeters > 0.0f) || !std::isfinite(widthMeters) || !std::isfinite(lengthMeters))
        return image;
    image.width = 256;
    image.height = std::clamp(static_cast<uint32_t>(std::ceil(lengthMeters * 16.0f)), 16u, 8192u);
    image.rgba.assign(size_t(image.width) * image.height * 4, 0);
    const float halfWidth = widthMeters * 0.5f;
    // ワールド座標。格子の行の間を距離で補間し、列 0（Right 端）と列末尾（Left 端）の間を横位置で補間する。
    const bool hasWorld = geometry != nullptr && geometry->stride >= 2 &&
                          geometry->surface.vertices.size() >= geometry->stride * 2 &&
                          geometry->rowDistances.size() == geometry->surface.vertices.size() / geometry->stride;
    size_t upperRow = 1;
    for (uint32_t y = 0; y < image.height; ++y) {
        const float distance = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height) * lengthMeters;
        float rightX = 0.0f, rightZ = 0.0f, leftX = 0.0f, leftZ = 0.0f;
        if (hasWorld) {
            const auto& v = geometry->surface.vertices;
            const size_t rows = geometry->rowDistances.size();
            while (upperRow + 1 < rows && geometry->rowDistances[upperRow] < distance) ++upperRow;
            const size_t lowerRow = upperRow - 1;
            const float span = geometry->rowDistances[upperRow] - geometry->rowDistances[lowerRow];
            const float t = span > 1e-6f ? std::clamp((distance - geometry->rowDistances[lowerRow]) / span, 0.0f, 1.0f) : 0.0f;
            const auto& r0 = v[lowerRow * geometry->stride].position;
            const auto& r1 = v[upperRow * geometry->stride].position;
            const auto& l0 = v[lowerRow * geometry->stride + geometry->stride - 1].position;
            const auto& l1 = v[upperRow * geometry->stride + geometry->stride - 1].position;
            rightX = r0.x + (r1.x - r0.x) * t; rightZ = r0.z + (r1.z - r0.z) * t;
            leftX = l0.x + (l1.x - l0.x) * t;  leftZ = l0.z + (l1.z - l0.z) * t;
        }
        for (uint32_t x = 0; x < image.width; ++x) {
            // 列 0 が Right 端（横位置 −幅/2）、列末尾が Left 端。
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width);
            const float lateral = (u - 0.5f) * widthMeters;
            const float worldX = rightX + (leftX - rightX) * u;
            const float worldZ = rightZ + (leftZ - rightZ) * u;
            uint8_t* texel = &image.rgba[(size_t(y) * image.width + x) * 4];
            for (int channel = 0; channel < 3; ++channel) {
                const float value = channels[channel]
                    ? EvaluateRoadMask(*channels[channel], lateral, distance, halfWidth, lengthMeters, lanes,
                                       worldX, worldZ, hasWorld) : 0.0f;
                texel[channel] = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            }
            texel[3] = 255;
        }
    }
    return image;
}

}  // namespace tg::graph
