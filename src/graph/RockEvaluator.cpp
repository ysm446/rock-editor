#include "graph/RockEvaluator.h"
#include "fracture/MultiSplit.h"
#include "crack/MeshCut.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_set>
namespace rock::graph {
namespace {
RockEvaluation Failure(GraphId id, const char* kind, const std::string& message) {
    RockEvaluation result;
    result.error = std::string(kind) + " #" + std::to_string(id) + ": " + message;
    return result;
}
// 同じ生成結果へ合流したときだけ重複を除く。元の Box と加工した枝は別の source を持つ。
void Append(RockEvaluation& target, const RockEvaluation& source) {
    const auto append = [](auto& dst, const auto& src) {
        for (const auto& item : src)
            if (std::none_of(dst.begin(), dst.end(),
                             [&](const auto& other) { return other.source == item.source; }))
                dst.push_back(item);
    };
    for (const auto& item : source.rocks)
        if (std::none_of(target.rocks.begin(), target.rocks.end(), [&](const auto& other) {
                return other.source == item.source && other.chunk == item.chunk;
            }))
            target.rocks.push_back(item);
    for (const auto& item : source.fractures)
        if (std::none_of(target.fractures.begin(), target.fractures.end(), [&](const auto& other) {
                return other.source == item.source && other.negative == item.negative &&
                       other.positive == item.positive;
            }))
            target.fractures.push_back(item);
    for (const auto& item : source.jointPlanes)
        if (std::none_of(target.jointPlanes.begin(), target.jointPlanes.end(), [&](const auto& other) {
                return other.source == item.source && other.index == item.index;
            }))
            target.jointPlanes.push_back(item);
    for (const auto& item : source.cracks)
        if (std::none_of(target.cracks.begin(), target.cracks.end(), [&](const auto& other) {
                return other.source == item.source && other.index == item.index;
            }))
            target.cracks.push_back(item);
    append(target.cuts, source.cuts);
    target.hasModels |= source.hasModels;
}
// 対応する枝を完全な値で比較し、改版番号の巻き戻りにも対応する。
std::optional<std::string> VolumeKey(const NodeGraph& graph, GraphId id, size_t depth = 0) {
    const auto* node = graph.FindNode(id);
    if (!node || depth > 256) return std::nullopt;
    std::string key;
    const auto add = [&](const auto& value) {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    add(id);
    add(node->kind);
    if (node->kind == NodeKind::RandomBoxes) {
        const auto* s = std::get_if<geometry::BoxClusterSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->count); add(s->size); add(s->sizeVariation); add(s->spread); add(s->rotation); add(s->seed);
        return key;
    }
    if (node->kind == NodeKind::ToVolume) {
        const auto* s = std::get_if<geometry::VolumeSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->resolution);
    } else if (node->kind == NodeKind::VolumeTransform) {
        const auto* s = std::get_if<geometry::VolumeTransformSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->position); add(s->rotationDegrees); add(s->scale);
    } else if (node->kind == NodeKind::VolumeToMesh) {
        const auto* s = std::get_if<geometry::VolumeToMeshSettings>(&node->settings);
        const auto method = s ? s->method : geometry::VolumeMeshingMethod::MarchingTetrahedra;
        add(method);
    } else return std::nullopt;
    const auto* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
    if (!upstream) return std::nullopt;
    const auto parent = VolumeKey(graph, upstream->id, depth + 1);
    if (!parent) return std::nullopt;
    key += *parent;
    return key;
}
}  // namespace
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview, RockEvaluationCache* persistent,
                            geometry::VolumeMeshingMethod previewMethod) {
    if (persistent) {
        std::erase_if(persistent->entries, [&](const auto& item) {
            const auto key = VolumeKey(graph, item.first);
            return !key || *key != item.second.key;
        });
        std::erase_if(persistent->surfaces, [&](const auto& item) {
            return !persistent->entries.contains(item.first);
        });
    }
    // 評価一回の中では、他の種類の共有上流も再利用する。
    std::map<GraphId, RockEvaluation> cache;
    std::unordered_set<GraphId> active;
    std::function<RockEvaluation(GraphId, size_t)> evaluate;
    evaluate = [&](GraphId id, size_t depth) -> RockEvaluation {
        if (const auto found = cache.find(id); found != cache.end()) return found->second;
        if (depth > 256 || active.contains(id))
            return Failure(id, "Graph", "循環または評価深さの上限を検出しました");
        const auto* node = graph.FindNode(id);
        if (!node) return {};
        const auto persistentKey = persistent ? VolumeKey(graph, id) : std::nullopt;
        if (persistentKey) {
            const auto found = persistent->entries.find(id);
            if (found != persistent->entries.end()) return found->second.result;
        }
        active.insert(id);
        RockEvaluation result;
        const auto finish = [&](RockEvaluation value) {
            active.erase(id);
            cache[id] = value;
            if (persistentKey && value.error.empty())
                persistent->entries[id] = {*persistentKey, value};
            return value;
        };
        if (node->kind == NodeKind::RandomBoxes) {
            const auto* settings = std::get_if<geometry::BoxClusterSettings>(&node->settings);
            if (!settings) return finish(Failure(id, "Random Boxes", "設定がありません"));
            std::string error;
            auto boxes = geometry::MakeBoxCluster(*settings, error);
            if (!error.empty()) return finish(Failure(id, "Random Boxes", error));
            GeneratedRock rock;
            rock.source = id;
            rock.mesh = geometry::BoxClusterPreview(boxes);
            rock.boxes = std::make_shared<const std::vector<geometry::OrientedBox>>(std::move(boxes));
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::ToVolume) {
            const auto* settings = std::get_if<geometry::VolumeSettings>(&node->settings);
            const auto* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream) return finish(Failure(id, "To Volume", "Random Boxes の Boxes 出力を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            if (input.rocks.size() != 1 || !input.rocks[0].boxes)
                return finish(Failure(id, "To Volume", "直方体の集合が必要です"));
            std::string error;
            auto volume = geometry::BoxesToVolume(*input.rocks[0].boxes, *settings, error);
            if (!error.empty()) return finish(Failure(id, "To Volume", error));
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(volume));
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::VolumeTransform) {
            const auto* settings = std::get_if<geometry::VolumeTransformSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream)
                return finish(Failure(id, "Volume Transform", "Volume 出力を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            if (input.rocks.size() != 1 || !input.rocks[0].volume)
                return finish(Failure(id, "Volume Transform", "ボリュームが必要です"));
            std::string error;
            auto moved = geometry::TransformVolume(*input.rocks[0].volume, *settings, error);
            if (!error.empty()) return finish(Failure(id, "Volume Transform", error));
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(moved));
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::VolumeToMesh) {
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!upstream)
                return finish(Failure(id, "Volume to Mesh", "To Volume の Volume 出力を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            if (input.rocks.size() != 1 || !input.rocks[0].volume)
                return finish(Failure(id, "Volume to Mesh", "ボリュームが必要です"));
            std::string error;
            const auto* settings = std::get_if<geometry::VolumeToMeshSettings>(&node->settings);
            auto mesh = geometry::VolumeSurface(*input.rocks[0].volume, error,
                settings ? settings->method : geometry::VolumeMeshingMethod::MarchingTetrahedra);
            if (!error.empty()) return finish(Failure(id, "Volume to Mesh", error));
            GeneratedRock rock;
            rock.source = id;
            rock.mesh = std::move(mesh);
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::BaseRock) {
            const auto* settings = std::get_if<BaseRockNodeSettings>(&node->settings);
            std::string error;
            auto mesh = settings ? geometry::MakeBaseRock(*settings, error) : geometry::Mesh{};
            if (!settings || !error.empty())
                return finish(Failure(id, "Base Rock", settings ? error : "設定がありません"));
            const auto uncutBox = settings->shape == geometry::BaseShape::Box && settings->noiseStrength == 0
                                      ? std::optional{settings->size}
                                      : std::nullopt;
            result.rocks.push_back({id, std::move(mesh), uncutBox});
        } else if (node->kind == NodeKind::JointSet) {
            const auto* settings = std::get_if<crack::JointSetSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream)
                return finish(Failure(id, "Joint Set", "Mesh 入力を接続してください"));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty())
                return finish(Failure(id, "Joint Set", "母岩の Mesh を接続してください"));
            std::vector<crack::CrackPatch> patches;
            std::string error;
            if (!crack::BuildJointSet(*settings, patches, error))
                return finish(Failure(id, "Joint Set", error));
            for (size_t i = 0; i < patches.size(); ++i) {
                const GeneratedCrack generated{id, patches[i], static_cast<int>(i)};
                result.jointPlanes.push_back(generated);
                if (settings->showGuide) result.cracks.push_back(generated);
            }
        } else if (node->kind == NodeKind::Crack) {
            const auto* settings = std::get_if<crack::CrackSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            crack::CrackPatch patch;
            std::string error;
            if (!settings || !upstream || !crack::BuildCrackPatch(*settings, patch, error))
                return finish(Failure(id, "Crack", !upstream ? "Mesh 入力を接続してください" : error));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (settings->applyCut) {
                if (result.hasModels || result.rocks.size() != 1 || result.rocks.front().chunk != 0)
                    return finish(Failure(id, "Crack", "部分切断は未分割の岩1個を接続してください"));
                if (settings->meshCut && std::any_of(result.cuts.begin(), result.cuts.end(),
                                                     [](const auto& c) { return c.removedVolume > 0; }))
                    return finish(Failure(
                        id, "Crack",
                        "Mesh 部分切断は未加工の母岩への1回だけ対応します。交差する複数亀裂は未対応です"));
                if (!settings->meshCut && !result.rocks.front().uncutBox)
                    return finish(Failure(
                        id, "Crack",
                        "Box 部分切断は未加工の Box 1個のみ対応します。曲面は Mesh 有限溝を選んでください"));
                auto cut = settings->meshCut ? crack::CutMesh(result.rocks.front().mesh, *settings)
                                             : crack::CutBox(*result.rocks.front().uncutBox, *settings);
                if (!cut.error.empty()) return finish(Failure(id, "Crack", cut.error));
                if (cut.removedVolume > 0) {
                    result.rocks.front() = {id, std::move(cut.mesh), std::nullopt};
                }
                result.cuts.push_back(
                    {id, cut.bridge, cut.penetration, cut.removedVolume, settings->showBridge, cut.status});
            }
            if (settings->showGuide) result.cracks.push_back({id, patch});
        } else if (node->kind == NodeKind::Fracture) {
            const auto* settings = std::get_if<fracture::FractureSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream) return finish(Failure(id, "Fracture", "Mesh 入力を接続してください"));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.size() != 1 || result.rocks.front().chunk != 0)
                return finish(
                    Failure(id, "Fracture", "未分割の岩1個を接続してください。再帰分割は未対応です"));
            const auto parent = result.rocks.front().source;
            std::string error;
            std::vector<fracture::SplitPiece> pieces;
            if (settings->useJointSets) {
                std::vector<fracture::SplitPlane> planes;
                for (const auto& joint : result.jointPlanes)
                    planes.push_back({joint.patch.center, joint.patch.normal,
                                      std::to_string(joint.source) + ":" + std::to_string(joint.index)});
                auto split = fracture::SplitByPlanes(result.rocks.front().mesh, planes);
                if (!split.error.empty()) return finish(Failure(id, "Fracture", split.error));
                for (const auto& connection : split.connections) {
                    const auto& plane = split.planes[connection.plane];
                    const auto joint = std::find_if(
                        result.jointPlanes.begin(), result.jointPlanes.end(), [&](const auto& p) {
                            return std::to_string(p.source) + ":" + std::to_string(p.index) == plane.key;
                        });
                    result.fractures.push_back({id, parent, plane.center, plane.normal, connection.area,
                                                static_cast<int>(connection.negative + 1),
                                                static_cast<int>(connection.positive + 1), joint->source,
                                                joint->index});
                }
                pieces = std::move(split.pieces);
            } else {
                crack::CrackSettings plane;
                plane.center = settings->center;
                plane.rotationDegrees = settings->rotationDegrees;
                crack::CrackPatch frame;
                if (!crack::BuildCrackPatch(plane, frame, error))
                    return finish(Failure(id, "Fracture", error));
                auto split = fracture::SplitByPlane(result.rocks.front().mesh, frame.center, frame.normal);
                if (!split.error.empty()) return finish(Failure(id, "Fracture", split.error));
                result.fractures.push_back({id, parent, frame.center, frame.normal, split.sectionArea});
                for (auto& mesh : split.meshes) pieces.push_back({std::move(mesh), {}});
            }
            result.rocks.clear();
            result.cracks.clear();
            result.jointPlanes.clear();
            result.cuts.clear();
            for (size_t side = 0; side < pieces.size(); ++side) {
                auto& piece = pieces[side];
                fracture::ChunkSettings transform;
                if (!settings->useJointSets)
                    transform = settings->chunks[side];
                else if (const auto found = settings->jointChunks.find(piece.key);
                         found != settings->jointChunks.end())
                    transform = found->second;
                crack::CrackSettings rotation;
                rotation.center = transform.position;
                rotation.rotationDegrees = transform.rotationDegrees;
                crack::CrackPatch basis;
                if (!crack::BuildCrackPatch(rotation, basis, error))
                    return finish(Failure(id, "Fracture", error));
                geometry::MeshInfo info;
                geometry::InspectMesh(piece.mesh, info);
                const geometry::Vec3 pivot{(info.minimum.x + info.maximum.x) * 0.5f,
                                           (info.minimum.y + info.maximum.y) * 0.5f,
                                           (info.minimum.z + info.maximum.z) * 0.5f};
                for (auto& p : piece.mesh.positions) {
                    const float x = p.x - pivot.x, y = p.y - pivot.y, z = p.z - pivot.z;
                    p = {pivot.x + basis.tangentU.x * x + basis.tangentV.x * y + basis.normal.x * z +
                             transform.position[0],
                         pivot.y + basis.tangentU.y * x + basis.tangentV.y * y + basis.normal.y * z +
                             transform.position[1],
                         pivot.z + basis.tangentU.z * x + basis.tangentV.z * y + basis.normal.z * z +
                             transform.position[2]};
                }
                geometry::MeshInfo transformed;
                if (!geometry::InspectMesh(piece.mesh, transformed) || !transformed.closed ||
                    transformed.components != 1 ||
                    std::abs(transformed.volume - info.volume) > info.volume * 1e-4)
                    return finish(Failure(id, "Fracture",
                                          "移動・回転後の精度を保てません。移動量を小さくしてください"));
                result.rocks.push_back({id, std::move(piece.mesh), std::nullopt, static_cast<int>(side + 1),
                                        parent, transform.locked, pivot, piece.key});
            }
        } else if (node->kind == NodeKind::Model || node->kind == NodeKind::Transform) {
            result.hasModels = true;
        } else if (node->kind == NodeKind::Merge || node->kind == NodeKind::MeshOutput) {
            for (const auto& pin : node->inputs)
                if (const auto* upstream = graph.FindUpstreamNodeForPin(pin.id)) {
                    const auto input = evaluate(upstream->id, depth + 1);
                    if (!input.error.empty()) return finish(input);
                    Append(result, input);
                }
        }
        return finish(result);
    };
    // Volume の外皮は描画へ渡す最後にだけ抽出する。下流の評価にはグリッドを渡す。
    const auto preparePreview = [persistent, previewMethod](RockEvaluation result) {
        if (!result.error.empty()) return result;
        for (auto& rock : result.rocks) {
            if (!rock.volume) continue;
            if (persistent) {
                const auto found = persistent->surfaces.find(rock.source);
                if (found != persistent->surfaces.end() && found->second.volume == rock.volume &&
                    found->second.method == previewMethod) {
                    rock.mesh = found->second.mesh;
                    continue;
                }
            }
            std::string error;
            rock.mesh = geometry::VolumeSurface(*rock.volume, error, previewMethod);
            if (!error.empty()) return Failure(rock.source, "Volume Preview", error);
            if (persistent) persistent->surfaces[rock.source] = {rock.volume, rock.mesh, previewMethod};
        }
        return result;
    };
    if (preview != 0) return preparePreview(evaluate(preview, 0));
    RockEvaluation result;
    for (const auto& node : graph.Nodes())
        if (node.kind == NodeKind::MeshOutput) {
            const auto input = evaluate(node.id, 0);
            if (!input.error.empty()) return input;
            Append(result, input);
        }
    return preparePreview(std::move(result));
}
}  // namespace rock::graph
