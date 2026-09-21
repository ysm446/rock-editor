#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <numbers>
namespace {
using namespace rock::geometry;
void Append(Mesh &out, const Mesh &input, Vec3 offset = {}, bool reverse = false) {
    auto start = uint32_t(out.positions.size());
    for (auto p : input.positions)
        out.positions.push_back({p.x + offset.x, p.y + offset.y, p.z + offset.z});
    for (auto face : input.triangles) {
        for (auto &i : face)
            i += start;
        if (reverse)
            std::swap(face[1], face[2]);
        out.triangles.push_back(face);
    }
}
Mesh Concave() {
    Mesh out;
    std::map<std::array<int, 3>, uint32_t> vertices;
    const std::set<std::array<int, 3>> cells = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    auto box = MakeBox({1, 1, 1});
    for (auto cell : cells)
        for (auto t : box.triangles) {
            auto normal = FaceNormal(box, t);
            auto neighbor = cell;
            neighbor[0] += int(normal.x);
            neighbor[1] += int(normal.y);
            neighbor[2] += int(normal.z);
            if (cells.contains(neighbor))
                continue;
            for (auto &i : t) {
                auto p = box.positions[i];
                std::array<int, 3> q = {cell[0] + int(p.x + .5f), cell[1] + int(p.y + .5f),
                                        cell[2] + int(p.z + .5f)};
                auto [found, added] = vertices.emplace(q, uint32_t(out.positions.size()));
                if (added)
                    out.positions.push_back({float(q[0]) - 1, float(q[1]) - 1, float(q[2]) - .5f});
                i = found->second;
            }
            out.triangles.push_back(t);
        }
    return out;
}
float At(const VolumeGrid &grid, Vec3 p) {
    const auto index = [&](float value, float origin, uint32_t count) {
        return uint32_t(std::clamp(int(std::lround((value - origin) / grid.spacing)), 0, int(count) - 1));
    };
    return grid.values[grid.Index(index(p.x, grid.origin.x, grid.dimensions[0]),
                                  index(p.y, grid.origin.y, grid.dimensions[1]),
                                  index(p.z, grid.origin.z, grid.dimensions[2]))];
}
} // namespace
void RunMeshVolumeTests() {
    using namespace rock;
    using tests::Check;
    tests::Section("Mesh to Volume");
    std::string error;
    auto box = MakeBox({2, 2, 2});
    auto volume = MeshToVolume(box, {24}, error);
    Check(error.empty() && !volume.values.empty(), "closed mesh converts to volume");
    if (volume.values.empty())
        return;
    double maxError = 0;
    for (uint32_t z = 0; z < volume.dimensions[2]; ++z)
        for (uint32_t y = 0; y < volume.dimensions[1]; ++y)
            for (uint32_t x = 0; x < volume.dimensions[0]; ++x) {
                auto p = volume.Position(x, y, z);
                double a = std::abs(p.x) - 1, b = std::abs(p.y) - 1, c = std::abs(p.z) - 1;
                double exact = std::sqrt(std::pow(std::max(a, 0.), 2) + std::pow(std::max(b, 0.), 2) +
                                         std::pow(std::max(c, 0.), 2)) +
                               std::min(std::max({a, b, c}), 0.);
                maxError = std::max(maxError, std::abs(exact - volume.values[volume.Index(x, y, z)]));
            }
    Check(maxError < volume.spacing * .001, "all box samples agree with independent analytic distance");
    auto surface = VolumeSurface(volume, error);
    MeshInfo info;
    Check(error.empty() && InspectMesh(surface, info) && info.closed && info.volume > 7.5 &&
              info.volume < 8.5,
          "converted mesh has closed surface and expected volume");
    auto concave = Concave();
    Check(InspectMesh(concave, info) && info.closed, "concave fixture is a single closed mesh");
    auto l = MeshToVolume(concave, {32}, error);
    Check(error.empty() && !l.values.empty(), "concave mesh supported");
    if (!l.values.empty())
        Check(At(l, {-.5f, .5f, 0}) < 0 && At(l, {.5f, -.5f, 0}) < 0 && At(l, {.5f, .5f, 0}) > 0,
              "concave notch stays outside");
    Mesh overlap;
    Append(overlap, box, {-.4f, 0, 0});
    Append(overlap, box, {.4f, 0, 0});
    auto unionGrid = MeshToVolume(overlap, {28}, error);
    Check(error.empty() && !unionGrid.values.empty(), "overlapping closed components convert");
    if (!unionGrid.values.empty())
        Check(At(unionGrid, {0, 0, 0}) < 0 && At(unionGrid, {.6f, 0, 0}) < 0,
              "overlap uses union instead of parity XOR / internal sheets");
    Mesh touching;
    Append(touching, box, {-1, 0, 0});
    Append(touching, box, {1, 0, 0});
    auto contact = MeshToVolume(touching, {32}, error);
    Check(error.empty() && !contact.values.empty(), "coincident opposite faces cancel");
    if (!contact.values.empty())
        Check(At(contact, {0, 0, 0}) < 0, "contact interface does not create a zero sheet");
    Mesh cavity;
    Append(cavity, MakeBox({4, 4, 4}));
    Append(cavity, box, {}, true);
    auto hollow = MeshToVolume(cavity, {32}, error);
    Check(error.empty() && !hollow.values.empty(), "inward inner shell represents cavity");
    if (!hollow.values.empty())
        Check(At(hollow, {0, 0, 0}) > 0 && At(hollow, {1.5f, 0, 0}) < 0, "cavity sign is preserved");
    Mesh torus;
    constexpr uint32_t around = 32, section = 16;
    for (uint32_t u = 0; u < around; ++u)
        for (uint32_t v = 0; v < section; ++v) {
            double a = 2 * std::numbers::pi * u / around, b = 2 * std::numbers::pi * v / section;
            torus.positions.push_back({float((2 + .7 * std::cos(b)) * std::cos(a)), float(.7 * std::sin(b)),
                                       float((2 + .7 * std::cos(b)) * std::sin(a))});
            auto i = u * section + v, j = ((u + 1) % around) * section + v,
                 k = ((u + 1) % around) * section + (v + 1) % section, l = u * section + (v + 1) % section;
            torus.triangles.push_back({i, k, j});
            torus.triangles.push_back({i, l, k});
        }
    auto ring = MeshToVolume(torus, {32}, error);
    Check(error.empty() && !ring.values.empty(), "closed mesh with a through hole converts");
    if (!ring.values.empty())
        Check(At(ring, {0, 0, 0}) > 0 && At(ring, {2, 0, 0}) < 0, "torus hole remains outside");
    for (const auto size : {.001f, 1000.f}) {
        auto extreme = MeshToVolume(MakeBox({size, size, size}), {16}, error);
        Check(error.empty() && !extreme.values.empty(), "minimum and maximum Base Shape dimensions");
    }
    Mesh separated;
    Append(separated, box, {-2, 0, 0});
    Append(separated, box, {2, 0, 0});
    auto islands = MeshToVolume(separated, {32}, error);
    for (auto method : {VolumeMeshingMethod::MarchingTetrahedra, VolumeMeshingMethod::DualContouring}) {
        auto surface = VolumeSurface(islands, error, method);
        MeshInfo info;
        Check(error.empty() && InspectMesh(surface, info) && info.closed && info.components == 2,
              "both surface methods preserve separated mesh components");
    }
    auto open = box;
    open.triangles.pop_back();
    MeshToVolume(open, {24}, error);
    Check(!error.empty(), "open mesh rejected");
    Mesh reversed;
    Append(reversed, box, {}, true);
    MeshToVolume(reversed, {24}, error);
    Check(!error.empty(), "inverted exterior rejected");
    MeshToVolume(box, {1000}, error);
    Check(!error.empty(), "invalid resolution rejected");
    std::stop_source stop;
    stop.request_stop();
    MeshToVolume(box, {24}, error, stop.get_token());
    Check(!error.empty(), "cancellation returns no stale grid");
    graph::NodeGraph graph;
    auto base = graph.CreateNode(graph::NodeKind::BaseRock), to = graph.CreateNode(graph::NodeKind::ToVolume),
         mesh = graph.CreateNode(graph::NodeKind::VolumeToMesh);
    auto connect = [&](int a, int b) {
        return graph.CreateLink(graph.FindNode(a)->outputs[0].id, graph.FindNode(b)->inputs[0].id);
    };
    Check(connect(base, to) && connect(to, mesh), "Base Shape Mesh -> To Volume -> Volume to Mesh");
    std::get<VolumeSettings>(graph.FindMutableNode(to)->settings).resolution = 24;
    graph::RockEvaluationCache cache;
    auto first = graph::EvaluateRocks(graph, to, &cache);
    auto again = graph::EvaluateRocks(graph, to, &cache);
    Check(first.error.empty() && first.rocks.size() == 1 && again.rocks[0].volume == first.rocks[0].volume,
          "generic mesh volume cache reuses grid");
    std::get<BaseRockSettings>(graph.FindMutableNode(base)->settings).size[0] = 3;
    auto changed = graph::EvaluateRocks(graph, to, &cache);
    Check(changed.error.empty() && changed.rocks[0].volume != first.rocks[0].volume,
          "upstream geometry invalidates generic volume cache");
    auto random = graph.CreateNode(graph::NodeKind::RandomBoxes);
    connect(random, to);
    auto fast = graph::EvaluateRocks(graph, to, &cache);
    auto raw = graph::EvaluateRocks(graph, random);
    auto expected = BoxesToVolume(*raw.rocks[0].boxes, {24}, error);
    Check(fast.error.empty() && fast.rocks[0].volume->values == expected.values,
          "Random Boxes retains analytic fast path exactly");
    auto uv = graph.CreateNode(graph::NodeKind::UvUnwrap);
    std::get<BoxClusterSettings>(graph.FindMutableNode(random)->settings).count = 1;
    std::get<UvUnwrapSettings>(graph.FindMutableNode(uv)->settings).resolution = 128;
    Check(connect(random, uv), "Random Boxes Mesh connects directly to UV Unwrap");
    auto unwrapped = graph::EvaluateRocks(graph, uv, &cache);
    Check(unwrapped.error.empty() && !unwrapped.rocks.empty() && HasValidUvs(unwrapped.rocks[0].mesh),
          "mesh consumers accept Box provenance metadata");
    auto scatter = graph.CreateNode(graph::NodeKind::ScatterPoints),
         fracture = graph.CreateNode(graph::NodeKind::VoronoiFracture),
         piecesMesh = graph.CreateNode(graph::NodeKind::PiecesToMesh);
    connect(base, scatter);
    connect(base, fracture);
    graph.CreateLink(graph.FindNode(scatter)->outputs[0].id, graph.FindNode(fracture)->inputs[1].id);
    connect(fracture, piecesMesh);
    connect(piecesMesh, to);
    std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).count = 5;
    auto piecesVolume = graph::EvaluateRocks(graph, to, &cache);
    Check(piecesVolume.error.empty() && !piecesVolume.rocks.empty(),
          "Voronoi -> Pieces to Mesh -> To Volume supports touching cells");
    if (!piecesVolume.error.empty() || piecesVolume.rocks.empty()) return;
    if (piecesVolume.error.empty())
        Check(At(*piecesVolume.rocks[0].volume, {0, 0, 0}) < 0,
              "Voronoi cell interfaces do not create cavities");
    auto remembered = piecesVolume.rocks[0].volume;
    auto unchanged = graph::EvaluateRocks(graph, to, &cache);
    Check(unchanged.rocks[0].volume == remembered, "piece branch volume cache reused");
    ++std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).seed;
    auto revised = graph::EvaluateRocks(graph, to, &cache);
    Check(revised.error.empty() && revised.rocks[0].volume != remembered,
          "piece upstream edits invalidate volume cache");
}
