#include "graph/SurfacePresetGraph.h"
#include "graph/SurfaceBandGeometry.h"
#include "graph/RoadMask.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tg::graph {
namespace {
constexpr float kBandCellMeters = 1.0f;
struct Profile {
    SurfaceId id;
    const SurfacePreset* preset;
    std::vector<float> knots;
};
DirectX::XMFLOAT2 SampleProfile(const Profile& profile, float t) {
    const auto& points = profile.preset->section;
    const auto upper = std::upper_bound(profile.knots.begin(), profile.knots.end(), t);
    const size_t i = std::clamp(size_t(upper - profile.knots.begin()), size_t(1), points.size() - 1);
    const float u = std::clamp((t - profile.knots[i - 1]) / (profile.knots[i] - profile.knots[i - 1]), 0.0f, 1.0f);
    return {std::lerp(points[i - 1].across, points[i].across, u),
            std::lerp(points[i - 1].height, points[i].height, u)};
}
// ワールドノイズは道路中央ではなく、沿道の断面位置で評価する。
void BakeBandWorldNoise(RoadMaskImage& image, const RoadMaskNodeSettings* const masks[3],
                        const RoadGeometry& road, const SurfaceLayoutDocument& document,
                        const SurfaceBand& band, float width) {
    using namespace DirectX;
    if (std::none_of(masks, masks + 3, [](const auto* m) { return m && m->shape == RoadMaskShape::WorldNoise; })) return;
    std::vector<Profile> profiles;
    std::vector<float> arcs;
    for (const auto& preset : document.presets) {
        if (std::none_of(band.spans.begin(), band.spans.end(), [&](const auto& span) { return span.preset == preset.id; })) continue;
        Profile profile{preset.id, &preset, {0}};
        float arc = 0;
        for (size_t i = 1; i < preset.section.size(); ++i) {
            arc += std::hypot(preset.section[i].across - preset.section[i-1].across, preset.section[i].height - preset.section[i-1].height);
            profile.knots.push_back(arc);
        }
        for (auto& knot : profile.knots) knot /= arc;
        profiles.push_back(std::move(profile)); arcs.push_back(arc);
    }
    const float length = road.rowDistances.back();
    for (uint32_t y = 0; y < image.height; ++y) {
        const float distance = (float(y) + 0.5f) / float(image.height) * length;
        const auto upper = std::upper_bound(road.rowDistances.begin(), road.rowDistances.end(), distance);
        const size_t row = std::clamp(size_t(upper - road.rowDistances.begin()), size_t(1), road.rowDistances.size() - 1);
        const float t = (distance - road.rowDistances[row - 1]) / (road.rowDistances[row] - road.rowDistances[row - 1]);
        const auto edge = [&](uint32_t column) {
            return XMVectorLerp(XMLoadFloat3(&road.surface.vertices[(row - 1) * road.stride + column].position),
                                XMLoadFloat3(&road.surface.vertices[row * road.stride + column].position), t);
        };
        const auto right = edge(0), left = edge(road.stride - 1);
        const bool isLeft = band.side == SurfaceSide::Left;
        const auto origin = isLeft ? left : right;
        const auto outward = XMVectorScale(XMVector3Normalize(XMVectorSubtract(left, right)), isLeft ? 1.0f : -1.0f);
        const auto samples = SampleSurfaceBand(document, band, distance);
        float actualArc = 0;
        for (const auto& sample : samples) {
            const auto index = std::find_if(profiles.begin(), profiles.end(), [&](const auto& p) { return p.id == sample.preset; }) - profiles.begin();
            actualArc += arcs[index] * sample.weight;
        }
        for (uint32_t x = 0; x < image.width; ++x) {
            const float meters = (float(x) + 0.5f) / float(image.width) * width;
            float across = 0;
            for (const auto& sample : samples) {
                const auto& profile = *std::find_if(profiles.begin(), profiles.end(), [&](const auto& p) { return p.id == sample.preset; });
                across += SampleProfile(profile, std::clamp(meters / actualArc, 0.0f, 1.0f)).x * sample.weight;
            }
            XMFLOAT3 world; XMStoreFloat3(&world, XMVectorAdd(origin, XMVectorScale(outward, across)));
            auto* pixel = &image.rgba[(size_t(y) * image.width + x) * 4];
            for (size_t c = 0; c < 3; ++c) if (masks[c] && masks[c]->shape == RoadMaskShape::WorldNoise) {
                const float value = EvaluateRoadMask(*masks[c], meters - width * 0.5f, distance, width * 0.5f, length,
                                                    nullptr, world.x, world.z, true);
                pixel[c] = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255));
            }
        }
    }
}
}

bool CreateRoadsideExample(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId,
                          SurfaceSide side, std::string& error) {
    error.clear();
    if (side == SurfaceSide::Road) { error = "左または右の沿道を選んでください"; return false; }
    auto next = document;
    RoadGeometry road;
    if (!EvaluateRoad(graph, roadId, road, error)) return false;
    const float length = road.rowDistances.back();
    if (length < 0.1f || length > 50) { error = "試作は長さ0.1〜50 mに対応します"; return false; }
    auto layout = std::find_if(next.layouts.begin(), next.layouts.end(), [&](const auto& l) { return l.roadNode == roadId; });
    if (layout == next.layouts.end()) {
        RoadLayout created; created.id = next.AllocateId(); created.roadNode = roadId;
        SurfaceBand roadBand; roadBand.id = next.AllocateId();
        created.bands.push_back(roadBand); next.layouts.push_back(created); layout = next.layouts.end() - 1;
    }
    for (const auto& band : layout->bands) if (band.side == side) {
        error = "この側には既に沿道の記述があります"; return false;
    }
    SurfacePreset ground, sidewalk;
    ground.id = next.AllocateId(); ground.name = "仮路肩";
    ground.section = {{next.AllocateId(), 0, 0}, {next.AllocateId(), 2, -0.06f}};
    ground.materials.emplace_back();
    sidewalk.id = next.AllocateId(); sidewalk.name = "仮歩道"; sidewalk.role = SurfaceRole::Sidewalk;
    sidewalk.section = {{next.AllocateId(), 0, 0}, {next.AllocateId(), 0, 0.15f}, {next.AllocateId(), 2, 0.15f}};
    sidewalk.materials.emplace_back(); sidewalk.materials[0].baseColor = {0.5f, 0.5f, 0.5f};
    sidewalk.boundaries[0].mode = BoundaryMode::KeepStep; sidewalk.boundaries[0].preserveOutline = true;
    SurfaceBand band; band.id = next.AllocateId(); band.side = side;
    SurfaceSpan a, b;
    a.id = next.AllocateId(); a.preset = ground.id; a.endMeters = length * 0.5f;
    b.id = next.AllocateId(); b.preset = sidewalk.id; b.startMeters = a.endMeters; b.endMeters = length;
    a.blendOutMeters = b.blendInMeters = std::min(2.0f, length * 0.25f);
    band.spans = {a, b}; layout->bands.push_back(band);
    next.presets.push_back(ground); next.presets.push_back(sidewalk);
    renderer::MeshData mesh;
    if (!BuildSurfaceBandGeometry(road, next, layout->bands.back(), mesh, error)) return false;
    document = std::move(next);
    return true;
}

bool BuildSurfaceBandGeometry(const RoadGeometry& road, const SurfaceLayoutDocument& sourceDocument,
                              const SurfaceBand& band, renderer::MeshData& result, std::string& error) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    using namespace DirectX;
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (!ValidateSurfaceLayouts(document, error)) return false;
    // 検証対象と生成対象の食い違いを防ぐ。文書内の帯を渡す。
    bool belongs = false;
    for (const auto& layout : sourceDocument.layouts) for (const auto& candidate : layout.bands)
        if (&candidate == &band) belongs = true;
    if (!belongs || band.side == SurfaceSide::Road || band.spans.empty())
        return fail("沿道生成には文書内の左または右の帯が必要です");
    if (road.stride < 2 || road.rowDistances.size() < 2 ||
        road.surface.vertices.size() != size_t(road.stride) * road.rowDistances.size())
        return fail("沿道生成には道路格子が必要です");
    const float length = road.rowDistances.back();
    if (!std::isfinite(length) || length < 0.1f || length > 50 || road.rowDistances.front() != 0)
        return fail("沿道生成は長さ0.1〜50 mに対応します");
    for (size_t row = 0; row < road.rowDistances.size(); ++row) {
        if (!std::isfinite(road.rowDistances[row]) || (row && road.rowDistances[row] <= road.rowDistances[row - 1]))
            return fail("道路の実距離が不正です");
    }
    for (const auto& vertex : road.surface.vertices) {
        const auto& p = vertex.position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return fail("道路の座標が不正です");
    }
    float end = 0;
    std::vector<Profile> profiles;
    std::vector<float> knots{0, 1};
    std::vector<float> distances = road.rowDistances;
    for (const auto& span : band.spans) {
        if (span.startMeters != end) return fail("沿道生成には空白のない区間列が必要です");
        end = span.endMeters;
        const float half = (span.endMeters - span.startMeters) * 0.5f;
        distances.insert(distances.end(), {span.startMeters, span.endMeters,
            span.startMeters + std::min(half, span.blendInMeters), span.endMeters - std::min(half, span.blendOutMeters)});
        if (std::any_of(profiles.begin(), profiles.end(), [&](const auto& p) { return p.id == span.preset; })) continue;
        const auto preset = std::find_if(document.presets.begin(), document.presets.end(), [&](const auto& p) { return p.id == span.preset; });
        if (preset->section.front().across != 0 || preset->section.front().height != 0 || preset->section.back().across <= 0)
            return fail("沿道断面は道路端の(0, 0)から始め、外側へ幅を持たせてください");
        Profile profile{span.preset, &*preset, {0}};
        float arc = 0;
        for (size_t i = 1; i < preset->section.size(); ++i) {
            arc += std::hypot(preset->section[i].across - preset->section[i - 1].across,
                              preset->section[i].height - preset->section[i - 1].height);
            profile.knots.push_back(arc);
        }
        for (auto& knot : profile.knots) { knot /= arc; knots.push_back(knot); }
        profiles.push_back(std::move(profile));
    }
    if (std::abs(end - length) > 0.001f) return fail("沿道区間を道路全長に合わせてください");
    // 急な形状変更を勝手に斜面へ置き換えない。段差の端面生成は後続。
    for (size_t i = 1; i < band.spans.size(); ++i) {
        const auto& a = band.spans[i - 1]; const auto& b = band.spans[i];
        if (a.preset != b.preset && a.blendOutMeters + b.blendInMeters <= 0)
            return fail("異なる沿道プリセットの境界には移行距離が必要です");
    }
    const auto sortUnique = [](auto& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    // 道路格子・等間隔点・区間端は同じ距離でも丸め誤差を持つ。
    // 10 μm以内の重複をまとめ、ほぼ幅ゼロの面を作らない。
    for (auto& distance : distances) distance = std::clamp(distance, 0.0f, length);
    std::sort(distances.begin(), distances.end());
    distances.erase(std::unique(distances.begin(), distances.end(), [](float a, float b) {
        return b - a <= 1e-5f;
    }), distances.end());
    distances.back() = length;
    sortUnique(knots);
    // 断面の角を残し、それぞれの区間を実距離1m以下へ分割する。
    // 全プリセットで共通の列を使い、幅・高さが変化しても格子を揃える。
    std::vector<float> dividedKnots{knots.front()};
    for (size_t i = 1; i < knots.size(); ++i) {
        float segmentLength = 0;
        for (const auto& profile : profiles) {
            const auto a = SampleProfile(profile, knots[i - 1]), b = SampleProfile(profile, knots[i]);
            // バンクで横方向と高さ方向が直交しなくても1mを超えない上限。
            segmentLength = std::max(segmentLength, std::abs(b.x - a.x) + std::abs(b.y - a.y));
        }
        const auto steps = std::max(1u, static_cast<uint32_t>(std::ceil(segmentLength / kBandCellMeters)));
        if (dividedKnots.size() + steps > 256) return fail("沿道断面の分割数が多すぎます");
        for (uint32_t step = 1; step <= steps; ++step)
            dividedKnots.push_back(std::lerp(knots[i - 1], knots[i], float(step) / float(steps)));
    }
    knots = std::move(dividedKnots);
    if (knots.size() > 256 || distances.size() > 8192 || knots.size() * distances.size() > 262144)
        return fail("沿道断面または区間の分割数が多すぎます");
    std::vector<XMFLOAT3> positions;
    // 道路の行と区間の端を保持し、曲線外側や断面の変化が1mを超える箇所だけ行を足す。
    for (;;) {
        if (distances.size() > 8192 || knots.size() * distances.size() > 262144)
            return fail("沿道断面または区間の分割数が多すぎます");
        positions.clear();
        positions.reserve(knots.size() * distances.size());
        for (float distance : distances) {
            const auto upper = std::upper_bound(road.rowDistances.begin(), road.rowDistances.end(), distance);
            const size_t row = std::clamp(size_t(upper - road.rowDistances.begin()), size_t(1), road.rowDistances.size() - 1);
            const float t = std::clamp((distance - road.rowDistances[row - 1]) /
                (road.rowDistances[row] - road.rowDistances[row - 1]), 0.0f, 1.0f);
            const auto edge = [&](uint32_t column) {
                return XMVectorLerp(XMLoadFloat3(&road.surface.vertices[(row - 1) * road.stride + column].position),
                                    XMLoadFloat3(&road.surface.vertices[row * road.stride + column].position), t);
            };
            const auto right = edge(0), left = edge(road.stride - 1);
            const bool isLeft = band.side == SurfaceSide::Left;
            const auto delta = XMVectorSubtract(left, right);
            if (XMVectorGetX(XMVector3LengthSq(delta)) < 1e-8f) return fail("道路の幅方向が縮退しています");
            const auto outward = XMVectorScale(XMVector3Normalize(delta), isLeft ? 1.0f : -1.0f);
            const auto origin = isLeft ? left : right;
            const auto samples = SampleSurfaceBand(document, band, distance);
            if (samples.empty()) return fail("沿道区間を道路全長に合わせてください");
            for (float knot : knots) {
                float across = 0, height = 0;
                for (const auto& sample : samples) {
                    const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const auto& p) { return p.id == sample.preset; });
                    const auto p = SampleProfile(*profile, knot);
                    across += p.x * sample.weight; height += p.y * sample.weight;
                }
                XMFLOAT3 p;
                XMStoreFloat3(&p, XMVectorAdd(origin, XMVectorAdd(XMVectorScale(outward, across), XMVectorSet(0, height, 0, 0))));
                positions.push_back(p);
            }
        }
        std::vector<float> refined{distances.front()};
        for (size_t row = 1; row < distances.size(); ++row) {
            float longest = 0;
            for (size_t col = 0; col < knots.size(); ++col) {
                const auto a = XMLoadFloat3(&positions[(row - 1) * knots.size() + col]);
                const auto b = XMLoadFloat3(&positions[row * knots.size() + col]);
                longest = std::max(longest, XMVectorGetX(XMVector3Length(XMVectorSubtract(b, a))));
            }
            if (longest > kBandCellMeters + 1e-5f) {
                const float middle = std::midpoint(distances[row - 1], distances[row]);
                if (middle <= distances[row - 1] || middle >= distances[row]) return fail("沿道の変化が急すぎて分割できません");
                refined.push_back(middle);
            }
            refined.push_back(distances[row]);
        }
        if (refined.size() == distances.size()) break;
        distances = std::move(refined);
    }
    renderer::MeshData mesh;
    // 断面の稜線を保つため面ごとに頂点を持つ。隣接面の位置は共通の標本から取る。
    const size_t columns = knots.size();
    for (size_t row = 1; row < distances.size(); ++row) for (size_t col = 1; col < columns; ++col) {
        const size_t ids[] = {(row - 1) * columns + col - 1, row * columns + col - 1,
                              (row - 1) * columns + col, row * columns + col};
        const auto origin = XMLoadFloat3(&positions[ids[0]]);
        const auto across = XMVectorSubtract(XMLoadFloat3(&positions[ids[2]]), origin);
        const auto along = XMVectorSubtract(XMLoadFloat3(&positions[ids[1]]), origin);
        auto normal = XMVector3Cross(along, across);
        if (band.side == SurfaceSide::Right) normal = XMVectorNegate(normal);
        if (XMVectorGetX(XMVector3LengthSq(normal)) < 1e-16f) return fail("沿道の面が縮退しています");
        normal = XMVector3Normalize(normal);
        const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
        for (size_t i = 0; i < 4; ++i) {
            renderer::MeshVertex vertex{};
            vertex.position = positions[ids[i]];
            XMStoreFloat3(&vertex.normal, normal);
            XMStoreFloat4(&vertex.tangent, XMVector3Normalize(across));
            vertex.tangent.w = band.side == SurfaceSide::Left ? -1.0f : 1.0f;
            vertex.uv = {knots[col - 1 + i / 2], distances[row - 1 + i % 2]};
            vertex.roadUv = vertex.uv;
            mesh.vertices.push_back(vertex);
        }
        if (band.side == SurfaceSide::Left) mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first + 2, first + 1, first + 3});
        else mesh.indices.insert(mesh.indices.end(), {first, first + 2, first + 1, first + 2, first + 3, first + 1});
    }
    result = std::move(mesh);
    return true;
}
CompiledMeshGraph CompileSurfaceBandPreview(const NodeGraph& graph, const SurfaceLayoutDocument& sourceDocument,
                                          GraphId roadId, SurfaceId bandId) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    CompiledMeshGraph result; result.active = true;
    if (!ValidateSurfaceLayouts(document, result.error) || !ValidateSurfaceLayoutRoads(document, graph, result.error)) return result;
    const SurfaceBand* band = nullptr;
    for (const auto& layout : document.layouts) if (layout.roadNode == roadId)
        for (const auto& candidate : layout.bands) if (candidate.id == bandId) band = &candidate;
    if (!band) { result.error = "指定した道路の沿道帯がありません"; return result; }
    RoadGeometry road;
    if (!EvaluateRoad(graph, roadId, road, result.error)) return result;
    renderer::SceneMesh mesh;
    if (!BuildSurfaceBandGeometry(road, document, *band, mesh.geometry, result.error)) return result;
    std::vector<const SurfacePreset*> presets;
    std::vector<float> arcLengths;
    std::vector<renderer::SceneMesh> contexts;
    for (const auto& span : band->spans) {
        if (std::any_of(presets.begin(), presets.end(), [&](const auto* p) { return p->id == span.preset; })) continue;
        const auto found = std::find_if(document.presets.begin(), document.presets.end(), [&](const auto& p) { return p.id == span.preset; });
        if (presets.size() == 3) { result.error = "沿道マテリアルの試作は1帯につき最大3プリセットに対応します"; return result; }
        presets.push_back(&*found);
        float arc = 0;
        for (size_t i = 1; i < found->section.size(); ++i)
            arc += std::hypot(found->section[i].across - found->section[i - 1].across,
                              found->section[i].height - found->section[i - 1].height);
        arcLengths.push_back(arc);
        const auto& preset = *found;
        renderer::SceneMesh context;
        context.materialOnly = true;
        std::vector<PresetMaterial> materials;
        if (!CompilePresetMaterials(preset, materials, result.error)) return result;
        context.roadMetersPerUv = materials.front().uvRepeatMeters;
        context.roadWidthMeters = arc;
        context.roadLengthMeters = road.rowDistances.back();
        context.layerBlendRange = preset.layerBlendRange;
        const RoadMaskNodeSettings* masks[3]{};
        for (size_t slot = 0; slot < materials.size(); ++slot) {
            const auto& source = materials[slot];
            context.layerUvRepeat[slot] = source.uvRepeatMeters;
            context.layerWorldUv[slot] = source.worldUv;
            context.layerHeightGate[slot] = source.heightGate;
            context.layerHeightGateThreshold[slot] = source.heightGateThreshold;
            context.layerHeightGateSoftness[slot] = source.heightGateSoftness;
            context.layerBlendMode[slot] = source.blendMode;
            if (slot && !source.mask) continue;
            compositor::MaterialStack stack;
            auto layer = compositor::MaterialStack::MakeBaseLayer();
            layer.name = preset.name;
            layer.material = source.material;
            layer.baseColor = {source.baseColor[0], source.baseColor[1], source.baseColor[2]};
            layer.roughness = source.roughness;
            layer.metallic = source.metallic;
            layer.ambientOcclusion = source.ambientOcclusion;
            layer.heightSource = compositor::ValueSource::Texture;
            layer.heightBase = 0.5f;
            layer.heightGain = 1;
            stack.Layers() = {layer};
            stack.SetTerrainScale(source.uvRepeatMeters, 1);
            if (slot == 0) context.materialStack = std::move(stack);
            else {
                context.layerStacks[slot - 1] = std::move(stack);
                masks[slot - 1] = &*source.mask;
            }
        }
        if (masks[0] || masks[1] || masks[2]) {
            auto mask = BakeRoadMask(masks, arc, road.rowDistances.back());
            BakeBandWorldNoise(mask, masks, road, document, *band, arc);
            context.roadMask = {mask.width, mask.height, std::move(mask.rgba)};
        }
        contexts.push_back(std::move(context));
    }
    // 形状側の断面比率を実距離に変換する。段差の垂直面にもUV幅がある。
    for (auto& vertex : mesh.geometry.vertices) {
        float arc = 0;
        for (const auto& sample : SampleSurfaceBand(document, *band, vertex.uv.y)) {
            const size_t index = std::find_if(presets.begin(), presets.end(), [&](const auto* p) { return p->id == sample.preset; }) - presets.begin();
            arc += arcLengths[index] * sample.weight;
        }
        vertex.uv.x *= arc;
        vertex.roadUv = vertex.uv;
    }
    mesh.roadMetersPerUv = 1;
    mesh.roadWidthMeters = *std::max_element(arcLengths.begin(), arcLengths.end());
    mesh.roadLengthMeters = road.rowDistances.back();
    // 法線が分かれる縁石の変位を独立に適用すると割れるため、この段階では全コンテキストとも0。
    mesh.displacementMeters = 0;
    for (size_t i = 0; i < 3; ++i) mesh.connectionSources[i] = static_cast<int>(std::min(i, presets.size() - 1) + 1);
    mesh.roadMask.width = 1;
    mesh.roadMask.height = std::max(16u, static_cast<uint32_t>(std::ceil(mesh.roadLengthMeters * 64)));
    mesh.roadMask.rgba.resize(size_t(mesh.roadMask.height) * 4);
    for (uint32_t y = 0; y < mesh.roadMask.height; ++y) {
        const float distance = (float(y) + 0.5f) * mesh.roadLengthMeters / float(mesh.roadMask.height);
        std::array<float, 3> weights{};
        for (const auto& sample : SampleSurfaceBand(document, *band, distance)) {
            const size_t index = std::find_if(presets.begin(), presets.end(), [&](const auto* p) { return p->id == sample.preset; }) - presets.begin();
            weights[index] += sample.weight;
        }
        auto* pixel = &mesh.roadMask.rgba[size_t(y) * 4];
        pixel[0] = static_cast<uint8_t>(std::lround(weights[1] * 255));
        pixel[1] = static_cast<uint8_t>(std::min(255 - int(pixel[0]), static_cast<int>(std::lround(weights[2] * 255))));
        pixel[2] = 0; pixel[3] = 255;
    }
    result.scene.meshes.push_back(std::move(mesh));
    for (auto& context : contexts) result.scene.meshes.push_back(std::move(context));
    return result;
}
bool ConnectSurfaceBandMaterials(CompiledMeshGraph& scene, const NodeGraph& graph,
                                    const SurfaceLayoutDocument& sourceDocument, GraphId roadId,
                                    SurfaceId bandId, std::string& error, bool enableDisplacement) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (!ValidateSurfaceLayouts(document, error)) return false;
    const SurfaceBand* band = nullptr;
    for (const auto& layout : document.layouts) if (layout.roadNode == roadId)
        for (const auto& candidate : layout.bands) if (candidate.id == bandId) band = &candidate;
    if (!band || band->side == SurfaceSide::Road) return fail("横接続には左または右の沿道が必要です");
    const bool isRight = band->side == SurfaceSide::Right;
    const auto roadSource = std::find(scene.meshSources.begin(), scene.meshSources.end(), roadId);
    if (roadSource == scene.meshSources.end()) return fail("接続先の道路が表示されていません");
    const size_t roadIndex = roadSource - scene.meshSources.begin();
    if (roadIndex >= scene.scene.meshes.size()) return fail("道路の描画参照が不正です");
    auto roadside = CompileSurfaceBandPreview(graph, document, roadId, bandId);
    if (!roadside.error.empty()) { error = roadside.error; return false; }
    if (roadside.scene.meshes.size() > 3) return fail("横接続の試作は沿道側の最大2プリセットに対応します");
    const auto& sourceRoad = scene.scene.meshes[roadIndex];
    renderer::SceneMesh roadContext;
    if (sourceRoad.connectionSources[0] >= 0) {
        if (sourceRoad.connectionSources[0] != sourceRoad.connectionSources[1] ||
            sourceRoad.connectionSources[0] != sourceRoad.connectionSources[2])
            return fail("横接続の試作は道路側1種類のプリセットに対応します");
        const size_t index = static_cast<size_t>(sourceRoad.connectionSources[0]);
        if (index >= scene.scene.meshes.size()) return fail("道路マテリアルの参照が不正です");
        roadContext = scene.scene.meshes[index];
    } else roadContext = sourceRoad;
    roadContext.geometry = {}; roadContext.materialOnly = true;
    roadContext.connectionSources = {-1, -1, -1};
    roadContext.connectionSeams.clear();
    if (!enableDisplacement) roadContext.displacementMeters = 0;
    roadContext.layerUvRepeat[0] = roadContext.roadMetersPerUv;
    if (!roadContext.materialStack) {
        compositor::MaterialStack stack;
        auto layer = compositor::MaterialStack::MakeBaseLayer();
        layer.baseColor = sourceRoad.material.baseColor; layer.roughness = sourceRoad.material.roughness;
        layer.metallic = sourceRoad.material.metallic; stack.Layers() = {layer};
        roadContext.materialStack = std::move(stack);
    }
    std::vector<const SurfacePreset*> presets;
    for (const auto& span : band->spans) {
        if (std::any_of(presets.begin(), presets.end(), [&](const auto* p) { return p->id == span.preset; })) continue;
        const auto preset = std::find_if(document.presets.begin(), document.presets.end(), [&](const auto& p) { return p.id == span.preset; });
        presets.push_back(&*preset);
    }
    const float width = sourceRoad.roadWidthMeters;
    const float length = sourceRoad.roadLengthMeters;
    if (width <= 0 || length <= 0) return fail("道路の寸法情報がありません");
    const float totalWidth = width + roadside.scene.meshes[0].roadWidthMeters;
    auto roadMesh = sourceRoad;
    auto sideMesh = std::move(roadside.scene.meshes[0]);
    for (auto& vertex : roadMesh.geometry.vertices) {
        auto meters = vertex.roadUv;
        meters.x *= sourceRoad.roadMetersPerUv; meters.y *= sourceRoad.roadMetersPerUv;
        if (sourceRoad.roadUvAlongU) std::swap(meters.x, meters.y);
        if (isRight) meters.x = width - meters.x;
        vertex.roadUv = meters;
    }
    for (auto& vertex : sideMesh.geometry.vertices) vertex.roadUv.x += width;
    const int contextStart = static_cast<int>(scene.scene.meshes.size());
    for (auto* mesh : {&roadMesh, &sideMesh}) {
        mesh->roadMetersPerUv = 1; mesh->roadUvAlongU = false;
        mesh->roadWidthMeters = totalWidth; mesh->roadLengthMeters = length;
        mesh->displacementMeters = enableDisplacement ? 1.0f : 0.0f;
        mesh->connectionPrototype = enableDisplacement;
        mesh->connectionHeightFade = enableDisplacement
            ? DirectX::XMFLOAT2{width, std::min({0.5f, width * 0.5f, (totalWidth - width) * 0.5f})}
            : DirectX::XMFLOAT2{};
        mesh->connectionSources = {contextStart, contextStart + 1, contextStart + static_cast<int>(presets.size())};
        mesh->connectionAcrossSigns = {isRight ? -1.0f : 1.0f, 1.0f, 1.0f};
        mesh->connectionFrameSign = isRight && mesh == &roadMesh ? -1.0f : 1.0f;
        mesh->connectionOrigins = {DirectX::XMFLOAT2{isRight ? width : 0, 0}, DirectX::XMFLOAT2{width, 0}, DirectX::XMFLOAT2{width, 0}};
        mesh->roadMask.width = 512;
        mesh->roadMask.height = std::max(16u, static_cast<uint32_t>(std::ceil(length * 64)));
        mesh->roadMask.rgba.resize(size_t(mesh->roadMask.width) * mesh->roadMask.height * 4);
        size_t slot = 0;
        for (const auto& span : band->spans) {
            if (!span.boundaryMaterial || std::any_of(mesh->boundaries.begin(), mesh->boundaries.end(),
                [&](const auto& b) { return b.material.id == span.boundaryMaterial; })) continue;
            const auto boundary = std::find_if(document.boundaryMaterials.begin(), document.boundaryMaterials.end(),
                [&](const auto& m) { return m.id == span.boundaryMaterial; });
            if (boundary == document.boundaryMaterials.end() || slot >= 8) continue; // 文書検証で拒否済み。
            mesh->boundaries[slot * 2] = {*boundary, width, 1};
            mesh->boundaries[slot * 2].material.widthMeters = std::min({boundary->widthMeters, width, totalWidth - width});
            ++slot;
        }
        if (slot) {
            mesh->boundaryControl.width = 8; mesh->boundaryControl.height = mesh->roadMask.height;
            mesh->boundaryControl.rgba.assign(size_t(mesh->roadMask.height) * 8 * 4, 0);
        }
    }
    // 同じ物理座標と種から境界を評価する。KeepStep/輪郭保持だけは面の側に応じて分離する。
    for (uint32_t y = 0; y < roadMesh.roadMask.height; ++y) {
        const float distance = (float(y) + 0.5f) * length / float(roadMesh.roadMask.height);
        const auto samples = SampleSurfaceBand(document, *band, distance);
        for (auto* mesh : {&roadMesh, &sideMesh}) if (mesh->boundaryControl.IsValid()) {
            for (size_t slot = 0; slot < 8; ++slot) {
                const auto id = mesh->boundaries[slot * 2].material.id;
                if (!id) continue;
                float enabled = 0, second = 0;
                for (const auto& sample : samples) {
                    const auto span = std::find_if(band->spans.begin(), band->spans.end(), [&](const auto& s) { return s.id == sample.span; });
                    if (span == band->spans.end() || span->boundaryMaterial != id) continue;
                    const auto index = std::find_if(presets.begin(), presets.end(), [&](const auto* p) { return p->id == sample.preset; }) - presets.begin();
                    const auto& contract = presets[index]->boundaries[0];
                    if (contract.mode == BoundaryMode::Blend && !contract.preserveOutline && contract.transitionMeters > 0) {
                        enabled += sample.weight; if (index == 1) second += sample.weight;
                    }
                }
                auto* pixel = &mesh->boundaryControl.rgba[(size_t(y) * 8 + slot) * 4];
                pixel[0] = static_cast<uint8_t>(std::lround(enabled * 255));
                pixel[1] = static_cast<uint8_t>(std::lround(second / std::max(enabled, 1e-6f) * 255));
            }
        }
        for (uint32_t x = 0; x < roadMesh.roadMask.width; ++x) {
            const float offset = (float(x) + 0.5f) * totalWidth / float(roadMesh.roadMask.width) - width;
            for (auto* mesh : {&roadMesh, &sideMesh}) {
                std::array<float, 2> weights{};
                for (const auto& sample : samples) {
                    const size_t index = std::find_if(presets.begin(), presets.end(), [&](const auto* p) { return p->id == sample.preset; }) - presets.begin();
                    const auto& contract = presets[index]->boundaries[0];
                    float coverage = mesh == &sideMesh ? 1.0f : 0.0f;
                    if (contract.mode == BoundaryMode::Blend && !contract.preserveOutline && contract.transitionMeters > 0) {
                        const float transition = std::min({contract.transitionMeters, width * 0.5f, sideMesh.roadWidthMeters - width});
                        const auto span = std::find_if(band->spans.begin(), band->spans.end(), [&](const auto& s) { return s.id == sample.span; });
                        const float wave = transition * 0.2f * std::sin(distance * 2.3f + float(span->seed % 1024));
                        const float t = std::clamp((offset - wave) / (transition * 2) + 0.5f, 0.0f, 1.0f);
                        coverage = t * t * (3 - 2 * t);
                    }
                    weights[index] += coverage * sample.weight;
                }
                auto* pixel = &mesh->roadMask.rgba[(size_t(y) * mesh->roadMask.width + x) * 4];
                pixel[0] = static_cast<uint8_t>(std::lround(weights[0] * 255));
                pixel[1] = static_cast<uint8_t>(std::min(255 - int(pixel[0]), static_cast<int>(std::lround(weights[1] * 255))));
                pixel[2] = 0; pixel[3] = 255;
            }
        }
    }
    auto next = scene;
    // 白線などが持つ旧Road UVも、参照する道路と同じ実距離座標へ変換する。
    for (size_t i = 0; i < next.scene.meshes.size(); ++i) {
        auto& child = next.scene.meshes[i];
        if (i == roadIndex || child.displacementSource != static_cast<int>(roadIndex)) continue;
        for (auto& vertex : child.geometry.vertices) {
            vertex.roadUv.x *= sourceRoad.roadMetersPerUv;
            vertex.roadUv.y *= sourceRoad.roadMetersPerUv;
            if (sourceRoad.roadUvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
            if (isRight) vertex.roadUv.x = width - vertex.roadUv.x;
        }
    }
    next.scene.meshes[roadIndex] = std::move(roadMesh);
    next.scene.meshes.push_back(std::move(roadContext)); next.meshSources.push_back(0);
    for (size_t i = 1; i < roadside.scene.meshes.size(); ++i) {
        roadside.scene.meshes[i].displacementMeters = enableDisplacement ? presets[i - 1]->displacementMeters : 0;
        next.scene.meshes.push_back(std::move(roadside.scene.meshes[i])); next.meshSources.push_back(0);
    }
    next.scene.meshes.push_back(std::move(sideMesh)); next.meshSources.push_back(0);
    if (!renderer::ValidateMeshScene(next.scene)) return fail("横接続の描画データが不正です");
    scene = std::move(next);
    return true;
}
bool ConnectBothSurfaceBands(CompiledMeshGraph& scene, const NodeGraph& graph,
                             const SurfaceLayoutDocument& sourceDocument, GraphId roadId,
                             SurfaceId leftBand, SurfaceId rightBand, std::string& error,
                             bool enableDisplacement) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    auto left = scene, right = scene;
    if (!ConnectSurfaceBandMaterials(left, graph, document, roadId, leftBand, error, enableDisplacement) ||
        !ConnectSurfaceBandMaterials(right, graph, document, roadId, rightBand, error, enableDisplacement)) return false;
    const size_t roadIndex = std::find(scene.meshSources.begin(), scene.meshSources.end(), roadId) - scene.meshSources.begin();
    const auto& leftRoad = left.scene.meshes[roadIndex];
    const auto& rightRoad = right.scene.meshes[roadIndex];
    // 引数の左右取り違えを黙って反転しない。
    if (leftRoad.connectionAcrossSigns[0] != 1 || rightRoad.connectionAcrossSigns[0] != -1) {
        error = "左右の沿道指定が一致しません"; return false;
    }
    const float width = scene.scene.meshes[roadIndex].roadWidthMeters;
    const float rightWidth = rightRoad.roadWidthMeters - width;
    const float totalWidth = leftRoad.roadWidthMeters + rightWidth;
    auto next = left;
    const size_t leftIndex = next.scene.meshes.size() - 1;
    const int rightStart = static_cast<int>(next.scene.meshes.size());
    // 素材は既存の評価・GPU寿命管理へ載せ、左右で道路の下地を共有する。
    for (size_t i = 1; i < 3; ++i) {
        next.scene.meshes.push_back(right.scene.meshes[rightRoad.connectionSources[i]]);
        next.meshSources.push_back(0);
    }
    next.scene.meshes.push_back(right.scene.meshes.back()); next.meshSources.push_back(0);
    // push_back後に参照を取得し直す。元の道路と白線のインデックスは維持する。
    for (size_t i = 0; i < scene.scene.meshes.size(); ++i) {
        if (i != roadIndex && next.scene.meshes[i].displacementSource != static_cast<int>(roadIndex)) continue;
        for (auto& v : next.scene.meshes[i].geometry.vertices) v.roadUv.x += rightWidth;
    }
    for (auto& v : next.scene.meshes[leftIndex].geometry.vertices) v.roadUv.x += rightWidth;
    for (auto& v : next.scene.meshes.back().geometry.vertices) v.roadUv.x = width - v.roadUv.x + rightWidth;
    const auto sample = [](const renderer::SceneMesh& mesh, float across, uint32_t row, size_t channel) {
        const auto& mask = mesh.roadMask;
        const float x = std::clamp(across / mesh.roadWidthMeters * float(mask.width) - 0.5f, 0.0f, float(mask.width - 1));
        const auto lo = static_cast<uint32_t>(x), hi = std::min(lo + 1, mask.width - 1);
        return std::lerp(float(mask.rgba[(size_t(row) * mask.width + lo) * 4 + channel]),
                         float(mask.rgba[(size_t(row) * mask.width + hi) * 4 + channel]), x - float(lo));
    };
    for (const size_t index : {roadIndex, leftIndex, next.scene.meshes.size() - 1}) {
        auto& mesh = next.scene.meshes[index];
        mesh.connectionSources = leftRoad.connectionSources;
        mesh.connectionExtraSources = {rightStart, rightStart + 1};
        mesh.connectionOrigins = {DirectX::XMFLOAT2{rightWidth, 0}, DirectX::XMFLOAT2{rightWidth + width, 0}, DirectX::XMFLOAT2{rightWidth + width, 0}};
        mesh.connectionExtraOrigins = {DirectX::XMFLOAT2{rightWidth, 0}, DirectX::XMFLOAT2{rightWidth, 0}};
        mesh.connectionAcrossSigns = {1, 1, 1}; mesh.connectionExtraSigns = {-1, -1};
        mesh.connectionFrameSign = index == next.scene.meshes.size() - 1 ? -1.0f : 1.0f;
        mesh.connectionHeightFade = leftRoad.connectionHeightFade;
        mesh.connectionHeightFade.x += rightWidth;
        mesh.connectionSecondHeightFade = rightRoad.connectionHeightFade;
        mesh.connectionSecondHeightFade.x = rightWidth;
        for (size_t slot = 0; slot < 8; ++slot) {
            mesh.boundaries[slot * 2] = leftRoad.boundaries[slot * 2];
            mesh.boundaries[slot * 2].center += rightWidth;
            mesh.boundaries[slot * 2 + 1] = rightRoad.boundaries[slot * 2];
            mesh.boundaries[slot * 2 + 1].center = rightWidth;
            mesh.boundaries[slot * 2 + 1].acrossSign = -1;
        }
        if (leftRoad.boundaryControl.IsValid() || rightRoad.boundaryControl.IsValid()) {
            mesh.boundaryControl.width = 8; mesh.boundaryControl.height = mesh.roadMask.height;
            mesh.boundaryControl.rgba.assign(size_t(mesh.roadMask.height) * 8 * 4, 0);
            for (size_t pixel = 0; pixel < size_t(mesh.roadMask.height) * 8; ++pixel) for (size_t c = 0; c < 2; ++c) {
                if (leftRoad.boundaryControl.IsValid()) mesh.boundaryControl.rgba[pixel * 4 + c] = leftRoad.boundaryControl.rgba[pixel * 4 + c];
                if (rightRoad.boundaryControl.IsValid()) mesh.boundaryControl.rgba[pixel * 4 + c + 2] = rightRoad.boundaryControl.rgba[pixel * 4 + c];
            }
        }
        mesh.roadWidthMeters = totalWidth;
        const auto& leftInput = index == leftIndex ? left.scene.meshes.back() : leftRoad;
        const auto& rightInput = index == next.scene.meshes.size() - 1 ? right.scene.meshes.back() : rightRoad;
        for (uint32_t y = 0; y < mesh.roadMask.height; ++y) for (uint32_t x = 0; x < mesh.roadMask.width; ++x) {
            const float across = (float(x) + 0.5f) * totalWidth / float(mesh.roadMask.width) - rightWidth;
            float values[4] = {sample(leftInput, across, y, 0), sample(leftInput, across, y, 1),
                               sample(rightInput, width - across, y, 0), sample(rightInput, width - across, y, 1)};
            const float sum = values[0] + values[1] + values[2] + values[3];
            int remaining = 255;
            for (size_t c = 0; c < 4; ++c) {
                const int value = std::min(remaining, static_cast<int>(std::lround(values[c] * std::min(1.0f, 255.0f / std::max(sum, 1.0f)))));
                mesh.roadMask.rgba[(size_t(y) * mesh.roadMask.width + x) * 4 + c] = static_cast<uint8_t>(value);
                remaining -= value;
            }
        }
    }
    if (!renderer::ValidateMeshScene(next.scene)) { error = "左右接続の描画データが不正です"; return false; }
    scene = std::move(next); error.clear(); return true;
}
bool ConnectSurfaceLayoutBands(CompiledMeshGraph& scene, const NodeGraph& graph,
                               const SurfaceLayoutDocument& sourceDocument, GraphId roadId,
                               SurfaceId leftBand, SurfaceId rightBand, std::string& error,
                               bool enableDisplacement) {
    const auto document = ResolveLayerMaterials(sourceDocument);
    const auto source = std::find(scene.meshSources.begin(), scene.meshSources.end(), roadId);
    if (source == scene.meshSources.end() || !renderer::ValidateMeshScene(scene.scene)) {
        error = "接続先の道路が不正です"; return false;
    }
    const size_t roadIndex = source - scene.meshSources.begin();
    if (roadIndex >= scene.scene.meshes.size()) { error = "道路の描画参照が不正です"; return false; }
    const auto original = scene.scene.meshes[roadIndex];
    const bool multiple = original.connectionSources[0] >= 0 &&
        (original.connectionSources[0] != original.connectionSources[1] || original.connectionSources[0] != original.connectionSources[2]);
    auto next = scene;
    if (multiple) {
        if (original.roadMask.width != 1 || original.connectionExtraSources[0] >= 0 || original.connectionRoadMixSource >= 0) {
            error = "道路の区間混合には未接続の道路プリセットが必要です"; return false;
        }
        next.scene.meshes[roadIndex].connectionSources.fill(original.connectionSources[0]);
    }
    const bool connected = leftBand && rightBand
        ? ConnectBothSurfaceBands(next, graph, document, roadId, leftBand, rightBand, error, enableDisplacement)
        : ConnectSurfaceBandMaterials(next, graph, document, roadId, leftBand ? leftBand : rightBand, error, enableDisplacement);
    if (!connected) return false;
    if (multiple) {
        const int firstContext = next.scene.meshes[roadIndex].connectionSources[0];
        const int extraStart = static_cast<int>(next.scene.meshes.size());
        for (size_t i = 1; i < 3; ++i) {
            auto context = scene.scene.meshes[original.connectionSources[i]];
            if (!enableDisplacement) context.displacementMeters = 0;
            next.scene.meshes.push_back(std::move(context)); next.meshSources.push_back(0);
        }
        renderer::SceneMesh mix;
        mix.materialOnly = true; mix.materialStack.emplace();
        mix.materialStack->Layers() = {compositor::MaterialStack::MakeBaseLayer()};
        mix.roadMask = original.roadMask;
        const int mixSource = static_cast<int>(next.scene.meshes.size());
        next.scene.meshes.push_back(std::move(mix)); next.meshSources.push_back(0);
        for (auto& mesh : next.scene.meshes) {
            if (mesh.materialOnly || mesh.connectionSources[0] != firstContext) continue;
            mesh.connectionRoadSources = {extraStart, extraStart + 1};
            mesh.connectionRoadMixSource = mixSource;
            // 片側接続の旧マスクのA=255は被覆ではない。追加の左右成分は0にする。
            if (mesh.connectionExtraSources[0] < 0)
                for (size_t i = 0; i < mesh.roadMask.rgba.size(); i += 4) mesh.roadMask.rgba[i + 2] = mesh.roadMask.rgba[i + 3] = 0;
        }
    }
    if (!renderer::ValidateMeshScene(next.scene)) { error = "道路区間と沿道の接続データが不正です"; return false; }
    scene = std::move(next); error.clear(); return true;
}
}  // namespace tg::graph
