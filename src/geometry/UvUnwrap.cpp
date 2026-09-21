#include "geometry/UvUnwrap.h"
#include <xatlas.h>
#include <cmath>
#include <memory>
#include <algorithm>
#include <unordered_set>
namespace rock::geometry {
int NormalizeUvResolution(int resolution) {
    int result = 128;
    while (result < resolution && result < 4096) result *= 2;
    return result;
}
namespace {
// 投影が折り重なる細い面を検出する。画素中心だけでなく面積のある交差を調べる。
std::unordered_set<size_t> OverlappingFaces(const Mesh &mesh) {
    constexpr int cells = 64;
    std::array<std::vector<size_t>, cells * cells> bins;
    std::vector<size_t> visited(mesh.triangles.size(), mesh.triangles.size());
    std::unordered_set<size_t> bad;
    const auto overlap = [](const auto &a, const auto &b) {
        for (const auto *triangle : {&a, &b})
            for (int i = 0; i < 3; ++i) {
                const auto p = (*triangle)[i], q = (*triangle)[(i + 1) % 3];
                const double nx = double(q.v) - p.v, ny = double(p.u) - q.u;
                double amin = 1e100, amax = -1e100, bmin = 1e100, bmax = -1e100;
                for (int c = 0; c < 3; ++c) {
                    const double ap = a[c].u * nx + a[c].v * ny, bp = b[c].u * nx + b[c].v * ny;
                    amin = std::min(amin, ap);
                    amax = std::max(amax, ap);
                    bmin = std::min(bmin, bp);
                    bmax = std::max(bmax, bp);
                }
                if (std::min(amax, bmax) - std::max(amin, bmin) <= 1e-9 * std::hypot(nx, ny))
                    return false;
            }
        return true;
    };
    for (size_t f = 0; f < mesh.cornerUvs.size(); ++f) {
        const auto &t = mesh.cornerUvs[f];
        const int x0 = std::clamp(int(std::min({t[0].u, t[1].u, t[2].u}) * cells), 0, cells - 1);
        const int x1 = std::clamp(int(std::max({t[0].u, t[1].u, t[2].u}) * cells), 0, cells - 1);
        const int y0 = std::clamp(int(std::min({t[0].v, t[1].v, t[2].v}) * cells), 0, cells - 1);
        const int y1 = std::clamp(int(std::max({t[0].v, t[1].v, t[2].v}) * cells), 0, cells - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                auto &bin = bins[y * cells + x];
                for (auto other : bin) {
                    if (visited[other] == f)
                        continue;
                    visited[other] = f;
                    if (bad.contains(f) && bad.contains(other))
                        continue;
                    if (overlap(t, mesh.cornerUvs[other])) {
                        bad.insert(f);
                        bad.insert(other);
                    }
                }
                bin.push_back(f);
            }
    }
    return bad;
}
} // namespace
bool HasValidUvs(const Mesh &mesh) {
    if (mesh.cornerUvs.size() != mesh.triangles.size() || mesh.cornerUvs.empty())
        return false;
    for (const auto &uv : mesh.cornerUvs) {
        for (auto p : uv)
            if (!std::isfinite(p.u) || !std::isfinite(p.v) || p.u < 0 || p.u > 1 || p.v < 0 || p.v > 1)
                return false;
        const double a = (double(uv[1].u) - uv[0].u) * (double(uv[2].v) - uv[0].v) -
                         (double(uv[1].v) - uv[0].v) * (double(uv[2].u) - uv[0].u);
        if (std::abs(a) < 1e-16)
            return false;
    }
    return true;
}
Mesh UnwrapMesh(const Mesh &input, const UvUnwrapSettings &s, std::string &error) {
    error.clear();
    MeshInfo info;
    if (!InspectMesh(input, info) || input.triangles.empty()) {
        error = "有効な三角形メッシュを接続してください";
        return {};
    }
    if (input.triangles.size() > 500000) {
        error = "自動展開は50万三角形までです。上流の解像度を下げてください";
        return {};
    }
    if (s.resolution != NormalizeUvResolution(s.resolution)) {
        error = "UV解像度は128〜4096の2のべき乗を指定してください";
        return {};
    }
    if (s.padding < 1 || s.padding > 32 || s.quality < 1 ||
        s.quality > 4) {
        error = "UV展開の解像度・余白・品質が範囲外です";
        return {};
    }
    // xatlasの面積しきい値は絶対値。SDFの細い三角形が無視されないよう、
    // 展開用の写しだけを原点近く・一定の大きさへ揃える。出力形状は元のまま。
    const float extent = std::max(
        {info.maximum.x - info.minimum.x, info.maximum.y - info.minimum.y, info.maximum.z - info.minimum.z});
    std::vector<Vec3> positions;
    positions.reserve(input.positions.size());
    for (auto p : input.positions)
        positions.push_back({(p.x - info.minimum.x) * 1000.0f / extent,
                             (p.y - info.minimum.y) * 1000.0f / extent,
                             (p.z - info.minimum.z) * 1000.0f / extent});
    std::vector<uint32_t> faceMaterials;
    for (int repair = 0; repair < 4; ++repair) {
        std::unique_ptr<xatlas::Atlas, decltype(&xatlas::Destroy)> atlas(xatlas::Create(), xatlas::Destroy);
        xatlas::MeshDecl decl;
        decl.vertexCount = static_cast<uint32_t>(input.positions.size());
        decl.vertexPositionData = positions.data();
        decl.vertexPositionStride = sizeof(Vec3);
        decl.indexCount = static_cast<uint32_t>(input.triangles.size() * 3);
        decl.indexData = input.triangles.data();
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        if (!faceMaterials.empty())
            decl.faceMaterialData = faceMaterials.data();
        auto added = xatlas::AddMesh(atlas.get(), decl);
        if (added != xatlas::AddMeshError::Success) {
            error = std::string("UV展開の入力エラー: ") + xatlas::StringForEnum(added);
            return {};
        }
        xatlas::ChartOptions charts;
        charts.maxIterations = static_cast<uint32_t>(s.quality);
        xatlas::PackOptions pack;
        pack.resolution = static_cast<uint32_t>(s.resolution);
        pack.padding = static_cast<uint32_t>(s.padding);
        pack.bilinear = true;
        xatlas::Generate(atlas.get(), charts, pack);
        // 推定密度で作った配置から、指定サイズの1枚へ収め直す。
        // 余白はピクセル単位を維持し、収まらない場合だけ密度を下げる。
        if (atlas->width && atlas->height) {
            pack.texelsPerUnit = atlas->texelsPerUnit * float(s.resolution) /
                                 float(std::max(atlas->width, atlas->height)) * 0.95f;
            pack.maxChartSize = static_cast<uint32_t>(s.resolution - 2 * s.padding - 4);
            for (int attempt = 0; attempt < 8; ++attempt) {
                xatlas::PackCharts(atlas.get(), pack);
                if (atlas->atlasCount == 1)
                    break;
                pack.texelsPerUnit *= 0.8f;
            }
        }
        if (atlas->meshCount != 1 || atlas->atlasCount != 1 || !atlas->width || !atlas->height) {
            error = "1枚のUVアトラスを生成できませんでした。解像度を上げるか余白を減らしてください";
            return {};
        }
        const auto &out = atlas->meshes[0];
        if (out.indexCount != decl.indexCount) {
            error = "UV展開で面数が一致しません";
            return {};
        }
        Mesh result = input;
        result.cornerUvs.resize(input.triangles.size());
        result.uvCharts.resize(input.triangles.size());
        result.uvWidth = atlas->width;
        result.uvHeight = atlas->height;
        for (size_t f = 0; f < input.triangles.size(); ++f)
            for (size_t c = 0; c < 3; ++c) {
                const auto &v = out.vertexArray[out.indexArray[f * 3 + c]];
                if (v.xref != input.triangles[f][c] || v.atlasIndex != 0 || v.chartIndex < 0) {
                    error = "UVの面対応を確認できませんでした (face=" + std::to_string(f) +
                            ", atlas=" + std::to_string(v.atlasIndex) + ")";
                    return {};
                }
                result.cornerUvs[f][c] = {v.uv[0] / atlas->width, v.uv[1] / atlas->height};
                result.uvCharts[f] = static_cast<uint32_t>(v.chartIndex);
            }
        if (!HasValidUvs(result)) {
            error = "縮退または範囲外のUVが生成されました";
            return {};
        }
        const auto bad = OverlappingFaces(result);
        if (!bad.empty()) {
            // 重なる面だけを別の島へ分離し、他の面の島分けを維持して再配置する。
            // 形状・面番号は変えない。修復しても重なる場合はベイクへ渡さない。
            faceMaterials.resize(input.triangles.size());
            for (size_t f = 0; f < faceMaterials.size(); ++f)
                faceMaterials[f] =
                    bad.contains(f) ? atlas->chartCount + static_cast<uint32_t>(f) : result.uvCharts[f];
            continue;
        }
        return result;
    }
    error = "重なりのないUVを生成できませんでした。上流の形状や解像度を調整してください";
    return {};
}
} // namespace rock::geometry
