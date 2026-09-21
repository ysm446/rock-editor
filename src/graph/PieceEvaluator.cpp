#include "graph/PieceEvaluator.h"
#include <algorithm>
namespace rock::graph {
RockEvaluation EvaluatePieceNode(const NodeGraph &graph, const Node &node, RockEvaluationCache *cache,
                                 const std::function<RockEvaluation(GraphId)> &evaluate,
                                 std::stop_token stop) {
    RockEvaluation out;
    const auto fail = [&](std::string message) {
        RockEvaluation r;
        r.error = std::string(FindNodeDefinition(node.kind)->title) + " #" + std::to_string(node.id) + ": " +
                  message;
        return r;
    };
    const auto input = [&](size_t index) {
        const auto *source =
            index < node.inputs.size() ? graph.FindUpstreamNodeForPin(node.inputs[index].id) : nullptr;
        return source ? evaluate(source->id) : fail("必要な入力を接続してください");
    };
    auto first = input(0);
    if (!first.error.empty())
        return first;
    std::string error;
    if (node.kind == NodeKind::ScatterPoints || node.kind == NodeKind::VoronoiFracture) {
        if (first.hasModels || first.rocks.size() != 1 || first.rocks[0].volume || first.rocks[0].boxes)
            return fail("単一の凸Meshを接続してください");
        const auto &mesh = first.rocks[0].mesh;
        std::string key;
        const auto add = [&](const auto &v) { key.append(reinterpret_cast<const char *>(&v), sizeof(v)); };
        add(node.id);
        add(node.kind);
        add(geometry::MeshFingerprint(mesh));
        RockEvaluation points;
        if (node.kind == NodeKind::ScatterPoints) {
            const auto &s = std::get<geometry::ScatterSettings>(node.settings);
            add(s.count);
            add(s.seed);
            add(s.version);
        } else {
            points = input(1);
            if (!points.error.empty())
                return points;
            if (!points.points)
                return fail("Points入力がありません");
            add(points.points->fingerprint);
            const auto &s = std::get<geometry::VoronoiSettings>(node.settings);
            add(s.rotation);
            add(s.stretch);
            add(s.version);
        }
        if (cache)
            if (auto it = cache->pieceEntries.find(node.id);
                it != cache->pieceEntries.end() && it->second.key == key)
                return it->second.result;
        if (node.kind == NodeKind::ScatterPoints)
            out.points = std::make_shared<const geometry::PointSet>(geometry::ScatterPoints(
                mesh, std::get<geometry::ScatterSettings>(node.settings), error, stop));
        else
            out.pieces = std::make_shared<const geometry::PieceCollection>(geometry::FractureVoronoi(
                mesh, *points.points, std::get<geometry::VoronoiSettings>(node.settings), node.id, error,
                stop));
        if (!error.empty())
            return fail(error);
        if (cache)
            cache->pieceEntries[node.id] = {key, out};
    } else {
        if (!first.pieces)
            return fail("Pieces入力を接続してください");
        if (node.kind == NodeKind::PieceSelect) {
            out.pieces = first.pieces;
            out.selection = std::make_shared<const geometry::PieceSelection>(geometry::SelectPieces(
                *first.pieces, std::get<geometry::PieceSelectSettings>(node.settings), error));
        } else if (node.kind == NodeKind::PiecesToMesh) {
            if (first.pieces->pieces.empty())
                return out;
            GeneratedRock rock;
            rock.source = node.id;
            rock.mesh = geometry::PiecesMesh(*first.pieces);
            geometry::MeshInfo info;
            if (!geometry::InspectMesh(rock.mesh, info) || !info.closed || info.volume <= 0)
                return fail("変換後のメッシュを表現できません。移動量・倍率を小さくしてください");
            out.rocks.push_back(std::move(rock));
        } else {
            RockEvaluation selected;
            if (node.kind == NodeKind::PieceFilter || graph.FindUpstreamNodeForPin(node.inputs[1].id)) {
                selected = input(1);
                if (!selected.error.empty())
                    return selected;
                if (!selected.selection)
                    return fail("Selection入力を接続してください");
            }
            geometry::PieceCollection result;
            if (node.kind == NodeKind::PieceFilter)
                result = geometry::FilterPieces(*first.pieces, *selected.selection,
                                                std::get<geometry::PieceFilterSettings>(node.settings).keep,
                                                error);
            else
                result = geometry::TransformPieces(*first.pieces, selected.selection.get(),
                                                   std::get<geometry::PieceTransformSettings>(node.settings),
                                                   error);
            out.pieces = std::make_shared<const geometry::PieceCollection>(std::move(result));
        }
    }
    return error.empty() ? out : fail(error);
}
void PreparePiecePreview(RockEvaluation &result, GraphId source) {
    if (!result.error.empty())
        return;
    if (result.pieces)
        for (const auto &p : result.pieces->pieces) {
            GeneratedRock rock;
            rock.source = source;
            rock.pieceId = int(p.id);
            rock.mesh = geometry::PieceMesh(p);
            geometry::MeshInfo info;
            if (!geometry::InspectMesh(rock.mesh, info) || !info.closed || info.volume <= 0) {
                result.error =
                    "Piece Preview: 変換後のメッシュを表現できません。移動量・倍率を調整してください";
                result.rocks.clear();
                return;
            }
            if (result.selection)
                rock.pieceSelected = std::find(result.selection->ids.begin(), result.selection->ids.end(),
                                               p.id) != result.selection->ids.end();
            result.rocks.push_back(std::move(rock));
        }
    if (result.points) {
        // 点は小さな八面体で表示し、通常の照明・奥行きを利用する。
        for (auto p : result.points->positions) {
            GeneratedRock rock;
            rock.source = source;
            float r = .025f;
            rock.mesh.positions = {{p.x + r, p.y, p.z}, {p.x - r, p.y, p.z}, {p.x, p.y + r, p.z},
                                   {p.x, p.y - r, p.z}, {p.x, p.y, p.z + r}, {p.x, p.y, p.z - r}};
            rock.mesh.triangles = {{{2, 4, 0}}, {{2, 1, 4}}, {{2, 5, 1}}, {{2, 0, 5}},
                                   {{3, 0, 4}}, {{3, 4, 1}}, {{3, 1, 5}}, {{3, 5, 0}}};
            result.rocks.push_back(std::move(rock));
        }
    }
}
} // namespace rock::graph
