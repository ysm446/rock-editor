#include "renderer/RockMesh.h"
#include <cmath>
#include <vector>
namespace rock::renderer {
namespace {
// 折れ角のしきい値。これより鋭い辺は法線を分け、岩の割れ面の角を残す。
constexpr float kCreaseDegrees = 40.0f;
}  // namespace
MeshData MakeRockMeshData(const geometry::Mesh& mesh, bool smooth) {
    MeshData result;
    geometry::MeshInfo info;
    if (!geometry::InspectMesh(mesh, info)) return result;
    // 面法線（単位）と、面積の重み（外積の長さ）。重みは平均するときだけ使う。
    std::vector<geometry::Vec3> faceNormals(mesh.triangles.size());
    std::vector<float> faceAreas(mesh.triangles.size(), 0.0f);
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        const auto& face = mesh.triangles[f];
        faceNormals[f] = geometry::FaceNormal(mesh, face);
        const auto a = mesh.positions[face[0]], b = mesh.positions[face[1]], c = mesh.positions[face[2]];
        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        faceAreas[f] = std::sqrt(cx * cx + cy * cy + cz * cz) * 0.5f;
    }
    // 頂点ごとに、その頂点を使う面の一覧（CSR）。滑らかにするときだけ作る。
    std::vector<uint32_t> adjacencyStart, adjacencyFaces;
    if (smooth) {
        adjacencyStart.assign(mesh.positions.size() + 1, 0);
        for (const auto& face : mesh.triangles)
            for (const uint32_t index : face) ++adjacencyStart[index + 1];
        for (size_t i = 1; i < adjacencyStart.size(); ++i) adjacencyStart[i] += adjacencyStart[i - 1];
        std::vector<uint32_t> cursor(adjacencyStart.begin(), adjacencyStart.end() - 1);
        adjacencyFaces.resize(mesh.triangles.size() * 3);
        for (size_t f = 0; f < mesh.triangles.size(); ++f)
            for (const uint32_t index : mesh.triangles[f]) adjacencyFaces[cursor[index]++] = static_cast<uint32_t>(f);
    }
    const float creaseCosine = std::cos(kCreaseDegrees * 3.14159265358979323846f / 180.0f);
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        const auto& face = mesh.triangles[f];
        const auto n = faceNormals[f];
        const auto a = mesh.positions[face[0]], b = mesh.positions[face[1]];
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        for (size_t i = 0; i < 3; ++i) {
            const auto p = mesh.positions[face[i]];
            MeshVertex v{};
            v.position = {p.x, p.y, p.z};
            v.normal = {n.x, n.y, n.z};
            if (smooth) {
                // その頂点を使う面のうち、この面と折れ角以内で向きが揃うものだけを面積で重み付けして平均する。
                float sx = 0, sy = 0, sz = 0;
                const uint32_t index = face[i];
                for (uint32_t k = adjacencyStart[index]; k < adjacencyStart[index + 1]; ++k) {
                    const auto other = faceNormals[adjacencyFaces[k]];
                    if (other.x * n.x + other.y * n.y + other.z * n.z < creaseCosine) continue;
                    const float weight = faceAreas[adjacencyFaces[k]];
                    sx += other.x * weight;
                    sy += other.y * weight;
                    sz += other.z * weight;
                }
                const float sum = std::sqrt(sx * sx + sy * sy + sz * sz);
                if (sum > 0) v.normal = {sx / sum, sy / sum, sz / sum};
            }
            v.tangent = {dx / length, dy / length, dz / length, 1};
            if (smooth) {
                // 平均した法線とは辺の向きが直交しなくなるので、接線を張り直す
                // （描画側は法線と接線の直交を前提にしている）。
                const float d = v.tangent.x * v.normal.x + v.tangent.y * v.normal.y + v.tangent.z * v.normal.z;
                float tx = v.tangent.x - v.normal.x * d, ty = v.tangent.y - v.normal.y * d,
                      tz = v.tangent.z - v.normal.z * d;
                float t = std::sqrt(tx * tx + ty * ty + tz * tz);
                if (t < 1e-6f) {
                    // 辺が法線とほぼ平行。法線に直交する軸を作り直す。
                    const bool useX = std::abs(v.normal.x) < 0.9f;
                    const float ax = useX ? 1.0f : 0.0f, ay = useX ? 0.0f : 1.0f;
                    tx = ay * v.normal.z;
                    ty = -ax * v.normal.z;
                    tz = ax * v.normal.y - ay * v.normal.x;
                    t = std::sqrt(tx * tx + ty * ty + tz * tz);
                }
                if (t > 0) v.tangent = {tx / t, ty / t, tz / t, 1};
            }
            // UV展開前の仮座標。Triplanarはこの値を使わず、描画時に位置から投影する。
            v.uv = {i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
            v.roadUv = v.uv;
            result.indices.push_back(static_cast<uint32_t>(result.vertices.size()));
            result.vertices.push_back(v);
        }
    }
    return result;
}
}  // namespace rock::renderer
