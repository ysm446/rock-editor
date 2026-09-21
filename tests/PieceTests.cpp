#include "TestSupport.h"
#include "geometry/Pieces.h"
#include "graph/RockEvaluator.h"
#include "io/PieceSettings.h"
#include "geometry/UvUnwrap.h"
#include <cmath>
void RunPieceTests() {
    using namespace rock::geometry;
    using rock::tests::Check;
    rock::tests::Section("Voronoi pieces");
    for (uint32_t seed = 1; seed <= 16; ++seed) {
        auto mesh = MakeBox({2, 6, 1});
        std::string error;
        auto points = ScatterPoints(mesh, {24, seed}, error);
        Check(error.empty() && points.positions.size() == 24, "deterministic interior scatter");
        Check(points.positions == ScatterPoints(mesh, {24, seed}, error).positions, "repeat scatter");
        VoronoiSettings settings;
        settings.stretch = {1, 4, 1};
        settings.rotation = {13, 27, 9};
        auto pieces = FractureVoronoi(mesh, points, settings, 42, error);
        if (!error.empty())
            std::printf("%s\n", error.c_str());
        Check(error.empty() && pieces.pieces.size() == 24, "anisotropic convex partition");
        double volume = 0;
        for (const auto &p : pieces.pieces) {
            MeshInfo info;
            Check(InspectMesh(*p.mesh, info) && info.closed && info.components == 1 && info.volume > 0,
                  "piece manifold / positive volume");
            volume += info.volume;
            bool contained = true;
            for (auto v : p.mesh->positions)
                contained &= v.x >= -1.00001 && v.x <= 1.00001 && v.y >= -3.00001 && v.y <= 3.00001 &&
                             v.z >= -.50001 && v.z <= .50001;
            Check(contained, "contained in source");
        }
        Check(std::abs(volume - 12) < .00024, "conserved volume");
        PieceSelectSettings select;
        select.mode = PieceSelectMode::Random;
        auto selected = SelectPieces(pieces, select, error);
        auto keep = FilterPieces(pieces, selected, true, error),
             remove = FilterPieces(pieces, selected, false, error);
        Check(keep.pieces.size() + remove.pieces.size() == pieces.pieces.size(),
              "filter complementary partition");
        PieceTransformSettings move;
        move.pose.position = {1, 2, 3};
        auto moved = TransformPieces(pieces, &selected, move, error);
        Check(error.empty() && moved.generation == pieces.generation, "transform preserves generation");
        if (!pieces.pieces.empty())
            Check(moved.pieces[0].mesh == pieces.pieces[0].mesh, "transform shares immutable mesh");
        FilterPieces(moved, selected, true, error);
        Check(!error.empty() || selected.ids.empty(), "selection rejects different pose");
    }
    auto box = MakeBox({2, 2, 2});
    std::string error;
    PointSet points;
    points.source = MeshFingerprint(box);
    points.positions = {{-.5f, 0, 0}, {.5f, 0, 0}};
    auto halves = FractureVoronoi(box, points, {}, 1, error);
    Check(error.empty() && halves.pieces.size() == 2, "plane through triangulation edges");
    if (halves.pieces.size() == 2)
        for (const auto &piece : halves.pieces) {
            double area = 0;
            bool sharedPlane = true;
            for (size_t t = 0; t < piece.mesh->triangles.size(); ++t)
                if ((*piece.faceOrigins)[t] == 0) {
                    auto face = piece.mesh->triangles[t];
                    auto a = piece.mesh->positions[face[0]], b = piece.mesh->positions[face[1]],
                         c = piece.mesh->positions[face[2]];
                    area += std::abs(double(b.y - a.y) * (c.z - a.z) - double(b.z - a.z) * (c.y - a.y)) * .5;
                    auto normal = FaceNormal(*piece.mesh, face);
                    sharedPlane &= std::abs(a.x) < 1e-6 && std::abs(b.x) < 1e-6 && std::abs(c.x) < 1e-6 &&
                                   (piece.id == 0 ? normal.x > .999f : normal.x < -.999f);
                }
            Check(sharedPlane && std::abs(area - 4) < 1e-6, "opposite caps coincide / area and orientation");
        }
    points.positions = {{-.5f, -.5f, 0}, {.5f, -.5f, 0}, {-.5f, .5f, 0}, {.5f, .5f, 0}};
    auto quadrants = FractureVoronoi(box, points, {}, 1, error);
    Check(error.empty() && quadrants.pieces.size() == 4, "bisectors meet at existing edges and vertices");
    auto convexFailure = box;
    convexFailure.positions[6] = {0, 0, 0};
    ScatterPoints(convexFailure, {}, error);
    Check(!error.empty(), "concave input rejected instead of convex hull replacement");
    PieceSelectSettings originalSelection;
    originalSelection.mode = PieceSelectMode::Manual;
    originalSelection.ids = {0};
    originalSelection.producer = halves.producer;
    originalSelection.generation = halves.generation;
    auto pick = SelectPieces(halves, originalSelection, error);
    PieceTransformSettings perPiece;
    perPiece.producer = halves.producer;
    perPiece.generation = halves.generation;
    PieceOverride individual;
    individual.id = 0;
    individual.pose.position = {1, 2, 3};
    individual.pose.rotation = {17, 30, 8};
    individual.pose.scale = {2, 3, 4};
    perPiece.overrides.push_back(individual);
    auto movedHalves = TransformPieces(halves, &pick, perPiece, error);
    if (movedHalves.pieces.size() == 2) {
        MeshInfo info;
        auto movedMesh = PieceMesh(movedHalves.pieces[0]);
        auto center = PieceCenter(movedHalves.pieces[0]);
        Check(InspectMesh(movedMesh, info) && info.closed && std::abs(info.volume - 96) < .001,
              "individual rotation / XYZ scale preserve closed solid");
        Check(std::abs(center.x - .5f) < 1e-5 && std::abs(center.y - 2) < 1e-5 &&
                  std::abs(center.z - 3) < 1e-5,
              "individual pivot is volume centroid");
        Check(movedHalves.pieces[1].transform == halves.pieces[1].transform, "unselected piece untouched");
    } else
        Check(false, "individual transform produces pieces");
    auto otherNamespace = halves;
    otherNamespace.producer = 99;
    RefreshPieceFingerprint(otherNamespace);
    FilterPieces(otherNamespace, pick, true, error);
    Check(!error.empty(), "selection cannot cross fracture namespace");
    points.positions[1] = points.positions[0];
    FractureVoronoi(box, points, {}, 1, error);
    Check(!error.empty(), "duplicate sites rejected");
    auto open = box;
    open.triangles.pop_back();
    ScatterPoints(open, {}, error);
    Check(!error.empty(), "open mesh rejected");

    for (auto size : {std::array<float, 3>{.001f, .001f, .001f}, {2, 6, .01f}, {1000, 1000, 1000}}) {
        auto source = MakeBox(size);
        auto sites = ScatterPoints(source, {128, 5}, error);
        auto result = FractureVoronoi(source, sites, {}, 7, error);
        if (!error.empty())
            std::printf("%s\n", error.c_str());
        Check(error.empty() && result.pieces.size() == 128, "128 cells / thin and extreme boxes");
        // 独立した最近点条件で全頂点のセル所属を確認する。凸セル同士の内部重複も排除する。
        bool owns = true;
        const auto distance = [](Vec3 a, Vec3 b) {
            return double(a.x - b.x) * (a.x - b.x) + double(a.y - b.y) * (a.y - b.y) +
                   double(a.z - b.z) * (a.z - b.z);
        };
        for (const auto &p : result.pieces)
            for (auto v : p.mesh->positions)
                for (auto site : sites.positions)
                    owns &= distance(v, sites.positions[p.id]) <=
                            distance(v, site) + double(size[0]) * size[0] * 2e-6;
        Check(owns, "independent nearest site / no overlapping interiors");
    }
    using namespace rock::graph;
    NodeGraph graph;
    auto base = graph.CreateNode(NodeKind::BaseRock), scatter = graph.CreateNode(NodeKind::ScatterPoints),
         fracture = graph.CreateNode(NodeKind::VoronoiFracture),
         select = graph.CreateNode(NodeKind::PieceSelect), filter = graph.CreateNode(NodeKind::PieceFilter),
         move = graph.CreateNode(NodeKind::PieceTransform),
         convert = graph.CreateNode(NodeKind::PiecesToMesh), uv = graph.CreateNode(NodeKind::UvUnwrap),
         output = graph.CreateNode(NodeKind::MeshOutput);
    const auto link = [&](int from, int to, size_t pin = 0) {
        return graph.CreateLink(graph.FindNode(from)->outputs[0].id, graph.FindNode(to)->inputs[pin].id);
    };
    Check(link(base, scatter) && link(base, fracture) && link(scatter, fracture, 1) &&
              link(fracture, select) && link(fracture, filter) && link(select, filter, 1) &&
              link(filter, move) && link(move, convert) && link(convert, uv) && link(uv, output),
          "typed piece workflow connects");
    Check(!link(fracture, output) && !link(scatter, uv), "no implicit Pieces / Points to Mesh conversion");
    auto &selection = std::get<PieceSelectSettings>(graph.FindMutableNode(select)->settings);
    selection.mode = PieceSelectMode::Random;
    RockEvaluationCache cache;
    auto initial = EvaluateRocks(graph, fracture, &cache);
    Check(initial.error.empty() && initial.pieces && initial.rocks.size() == 24,
          "colored preview keeps individual pieces");
    auto firstMesh = initial.pieces->pieces[0].mesh;
    auto final = EvaluateRocks(graph, 0, &cache);
    Check(final.error.empty() && final.rocks.size() == 1 && HasValidUvs(final.rocks[0].mesh),
          "pieces to UV workflow");
    auto &movement = std::get<PieceTransformSettings>(graph.FindMutableNode(move)->settings);
    movement.pose.position[0] = 1;
    final = EvaluateRocks(graph, 0, &cache);
    initial = EvaluateRocks(graph, fracture, &cache);
    Check(firstMesh == initial.pieces->pieces[0].mesh, "downstream editing reuses fracture geometry");
    selection.mode = PieceSelectMode::Manual;
    selection.ids = {0, 2};
    selection.producer = fracture;
    selection.generation = initial.pieces->generation;
    auto json = rock::io::WritePieceSettings(*graph.FindNode(select));
    Node restored;
    restored.kind = NodeKind::PieceSelect;
    rock::io::ReadPieceSettings(restored, json);
    Check(json == rock::io::WritePieceSettings(restored) && json["generation"].is_string(),
          "selection JSON round trip / exact 64 bit key");
    ++std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).seed;
    auto stale = EvaluateRocks(graph, filter, &cache);
    Check(!stale.error.empty(), "upstream seed invalidates manual selection");
    --std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).seed;
    auto undone = EvaluateRocks(graph, filter, &cache);
    Check(undone.error.empty(), "restored settings recover selection");
    selection.ids.clear();
    std::get<PieceFilterSettings>(graph.FindMutableNode(filter)->settings).keep = true;
    auto empty = EvaluateRocks(graph, filter, &cache);
    Check(empty.error.empty() && empty.pieces && empty.pieces->pieces.empty(),
          "empty Keep is a valid collection");
    std::stop_source cancellation;
    cancellation.request_stop();
    auto canceled = EvaluateRocks(graph, fracture, &cache, VolumeMeshingMethod::MarchingTetrahedra,
                                  cancellation.get_token());
    Check(!canceled.error.empty(), "canceled evaluation cannot publish a result");
}
