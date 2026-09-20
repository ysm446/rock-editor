#include "renderer/MeshData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace tg::renderer {

bool ValidateMeshScene(const MeshScene& scene) {
    for (const auto& mesh : scene.meshes) {
        const auto& data = mesh.geometry;
        if (!mesh.boundaryControl.rgba.empty() && (!mesh.boundaryControl.IsValid() || mesh.boundaryControl.width != 8)) return false;
        for (const auto& boundary : mesh.boundaries) {
            const auto& m = boundary.material;
            if (!std::isfinite(boundary.center) || (boundary.acrossSign != 1 && boundary.acrossSign != -1) ||
                !std::isfinite(m.widthMeters) || m.widthMeters <= 0 || !std::isfinite(m.repeatMeters) || m.repeatMeters <= 0 ||
                !std::isfinite(m.depthMeters) || m.depthMeters < 0 || !std::isfinite(m.heightCenter)) return false;
        }
        if (mesh.connectionFrameSign != 1 && mesh.connectionFrameSign != -1) return false;
        for (float sign : mesh.connectionAcrossSigns) if (sign != 1 && sign != -1) return false;
        if (!std::isfinite(mesh.connectionHeightFade.x) || !std::isfinite(mesh.connectionHeightFade.y) ||
            mesh.connectionHeightFade.y < 0) return false;
        if (mesh.materialOnly) {
            if (!data.vertices.empty() || !data.indices.empty() || !mesh.materialStack) return false;
            continue;
        }
        for (int source : mesh.connectionSources) {
            if (source != -1 && (source < 0 || static_cast<size_t>(source) >= scene.meshes.size() ||
                !scene.meshes[static_cast<size_t>(source)].materialOnly)) return false;
        }
        const bool hasContexts = mesh.connectionSources[0] >= 0;
        const bool hasRoadMix = mesh.connectionRoadMixSource >= 0;
        if (mesh.connectionRoadMixSource < -1 || (hasRoadMix && (!hasContexts ||
            static_cast<size_t>(mesh.connectionRoadMixSource) >= scene.meshes.size() ||
            !scene.meshes[mesh.connectionRoadMixSource].materialOnly ||
            !scene.meshes[mesh.connectionRoadMixSource].roadMask.IsValid()))) return false;
        for (int source : mesh.connectionRoadSources)
            if ((source >= 0) != hasRoadMix || source < -1 || (hasRoadMix &&
                (static_cast<size_t>(source) >= scene.meshes.size() || !scene.meshes[source].materialOnly))) return false;
        const bool hasExtra = mesh.connectionExtraSources[0] >= 0;
        if (!std::isfinite(mesh.connectionSecondHeightFade.x) || !std::isfinite(mesh.connectionSecondHeightFade.y) || mesh.connectionSecondHeightFade.y < 0) return false;
        for (size_t i = 0; i < 2; ++i) {
            const int source = mesh.connectionExtraSources[i];
            if ((source >= 0) != hasExtra || source < -1 || (hasExtra && (!hasContexts || static_cast<size_t>(source) >= scene.meshes.size() || !scene.meshes[source].materialOnly)) ||
                !std::isfinite(mesh.connectionExtraOrigins[i].x) || !std::isfinite(mesh.connectionExtraOrigins[i].y) ||
                (mesh.connectionExtraSigns[i] != 1 && mesh.connectionExtraSigns[i] != -1)) return false;
        }
        for (size_t i = 0; i < mesh.connectionSources.size(); ++i) {
            if ((mesh.connectionSources[i] >= 0) != hasContexts ||
                !std::isfinite(mesh.connectionOrigins[i].x) || !std::isfinite(mesh.connectionOrigins[i].y)) return false;
        }
        if (data.vertices.empty() || data.indices.empty() || data.indices.size() % 3 != 0 ||
            data.vertices.size() > std::numeric_limits<uint32_t>::max() / sizeof(MeshVertex) ||
            data.indices.size() > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t)) {
            return false;
        }
        const auto unitRange = [](float value) {
            return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
        };
        const auto& material = mesh.material;
        if (!unitRange(material.baseColor.x) || !unitRange(material.baseColor.y) ||
            !unitRange(material.baseColor.z) || !unitRange(material.roughness) ||
            !unitRange(material.metallic)) {
            return false;
        }
        for (const auto& v : data.vertices) {
            const float values[] = {v.position.x, v.position.y, v.position.z,
                v.normal.x, v.normal.y, v.normal.z, v.tangent.x, v.tangent.y,
                v.tangent.z, v.tangent.w, v.uv.x, v.uv.y, v.roadUv.x, v.roadUv.y};
            for (float value : values) {
                if (!std::isfinite(value)) return false;
            }
            using namespace DirectX;
            const float normalLength = XMVectorGetX(XMVector3Length(XMLoadFloat3(&v.normal)));
            const float tangentLength = XMVectorGetX(XMVector3Length(XMLoadFloat4(&v.tangent)));
            const float dot = XMVectorGetX(XMVector3Dot(XMLoadFloat3(&v.normal), XMLoadFloat4(&v.tangent)));
            if (std::abs(normalLength - 1.0f) > 0.01f ||
                std::abs(tangentLength - 1.0f) > 0.01f || std::abs(dot) > 0.01f ||
                std::abs(std::abs(v.tangent.w) - 1.0f) > 0.001f) return false;
        }
        for (uint32_t index : data.indices) {
            if (index >= data.vertices.size()) return false;
        }
        for (const auto& seam : mesh.connectionSeams)
            for (uint32_t index : seam) if (index >= data.vertices.size()) return false;
    }
    return std::isfinite(MeshSceneRadius(scene));
}

std::vector<uint32_t> MeshOutlineEdges(const MeshData& data) {
    // 位置で頂点をまとめる。継ぎ目で複製された頂点を別の点とみなすと内側に線が残るため。
    const size_t vertexCount = data.vertices.size();
    std::vector<uint32_t> canonical(vertexCount);
    {
        std::vector<uint32_t> order(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i) order[i] = i;
        const auto key = [&](uint32_t i) {
            const auto& p = data.vertices[i].position;
            return std::array<float, 3>{p.x, p.y, p.z};
        };
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
        constexpr float kMergeDistance = 1e-4f;
        uint32_t representative = 0;
        for (size_t n = 0; n < order.size(); ++n) {
            const uint32_t i = order[n];
            const auto& p = data.vertices[i].position;
            const auto& r = data.vertices[representative].position;
            if (n == 0 || std::abs(p.x - r.x) > kMergeDistance || std::abs(p.y - r.y) > kMergeDistance ||
                std::abs(p.z - r.z) > kMergeDistance) {
                representative = i;
            }
            canonical[i] = representative;
        }
    }

    // 辺ごとの出現回数を数える。1 回だけ現れる辺が外周。
    struct Edge { uint32_t lo, hi, first; };
    std::vector<Edge> edges;
    edges.reserve(data.indices.size());
    for (size_t t = 0; t + 2 < data.indices.size(); t += 3) {
        for (size_t k = 0; k < 3; ++k) {
            const uint32_t a = data.indices[t + k];
            const uint32_t b = data.indices[t + (k + 1) % 3];
            if (a >= vertexCount || b >= vertexCount) continue;
            const uint32_t ca = canonical[a];
            const uint32_t cb = canonical[b];
            if (ca == cb) continue;  // 縮退した辺
            edges.push_back({std::min(ca, cb), std::max(ca, cb), static_cast<uint32_t>(t + k)});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
        return x.lo != y.lo ? x.lo < y.lo : x.hi < y.hi;
    });
    std::vector<uint32_t> outline;
    for (size_t n = 0; n < edges.size();) {
        size_t m = n + 1;
        while (m < edges.size() && edges[m].lo == edges[n].lo && edges[m].hi == edges[n].hi) ++m;
        if (m - n == 1) {
            // 描画には元の頂点を使う（位置は同じ）。
            const size_t t = edges[n].first;
            const size_t base = t - (t % 3);
            outline.push_back(data.indices[t]);
            outline.push_back(data.indices[base + (t - base + 1) % 3]);
        }
        n = m;
    }
    return outline;
}

float MeshSceneRadius(const MeshScene& scene) {
    float radius = 0.1f;
    for (const auto& mesh : scene.meshes) {
        for (const auto& vertex : mesh.geometry.vertices) {
            const auto& p = vertex.position;
            radius = std::max(radius, std::hypot(p.x, p.y, p.z));
        }
    }
    return radius;
}

}  // namespace tg::renderer
