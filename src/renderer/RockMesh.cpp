#include "renderer/RockMesh.h"

#include <cmath>
namespace rock::renderer {
MeshData MakeRockMeshData(const geometry::Mesh& mesh) {
    MeshData result;
    geometry::MeshInfo info;
    if (!geometry::InspectMesh(mesh, info)) return result;
    for (const auto& face : mesh.triangles) {
        const auto n = geometry::FaceNormal(mesh, face);
        const auto a = mesh.positions[face[0]], b = mesh.positions[face[1]];
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        for (size_t i = 0; i < 3; ++i) {
            const auto p = mesh.positions[face[i]];
            MeshVertex v{};
            v.position = {p.x, p.y, p.z};
            v.normal = {n.x, n.y, n.z};
            v.tangent = {dx / length, dy / length, dz / length, 1};
            // P1 の無地表示用。Triplanar は P7 で接続する。
            v.uv = {i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
            v.roadUv = v.uv;
            result.indices.push_back(static_cast<uint32_t>(result.vertices.size()));
            result.vertices.push_back(v);
        }
    }
    return result;
}
}  // namespace rock::renderer
