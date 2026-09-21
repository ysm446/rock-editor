#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rock::geometry {
// 右手系 Y-up、メートル。描画の継ぎ目とは独立した共有頂点トポロジー。
struct Vec3 {
    float x = 0, y = 0, z = 0;
    bool operator==(const Vec3&) const = default;
};
struct Mesh {
    struct Uv { float u = 0, v = 0; bool operator==(const Uv&) const = default; };
    std::vector<Vec3> positions;
    std::vector<std::array<uint32_t, 3>> triangles;
    // 面の各コーナーのUV。位置の共有頂点は分割せず、形状トポロジーを保つ。
    std::vector<std::array<Uv, 3>> cornerUvs;
    std::vector<uint32_t> uvCharts;
    uint32_t uvWidth = 0, uvHeight = 0;
};
struct MeshInfo {
    Vec3 minimum, maximum;
    double volume = 0;
    size_t components = 0;
    bool closed = false;
};
// 寸法は 0.001～1000 m。無効値では空のメッシュを返す。
Mesh MakeBox(const std::array<float, 3>& size);
// 不正 index、非有限値、縮退面は false。閉包は共有辺の向きで判定する。
bool InspectMesh(const Mesh& mesh, MeshInfo& info);
Vec3 FaceNormal(const Mesh& mesh, const std::array<uint32_t, 3>& face);
}  // namespace rock::geometry
