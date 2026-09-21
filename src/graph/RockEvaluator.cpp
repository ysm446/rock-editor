#include "graph/RockEvaluator.h"
#include "graph/PieceEvaluator.h"

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
    for (const auto& item : source.rocks)
        if (std::none_of(target.rocks.begin(), target.rocks.end(),
                         [&](const auto& other) { return other.source == item.source && other.materials == item.materials && other.materialSource == item.materialSource && other.bakeSource == item.bakeSource; }))
            target.rocks.push_back(item);
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
    if (const auto* s = std::get_if<geometry::BaseRockSettings>(&node->settings)) {
        add(s->size); add(s->seed); add(s->shape); add(s->subdivisions);
        add(s->roundness); add(s->noiseStrength); add(s->noiseScale);
        return key;
    }
    const auto pose = [&](const geometry::PiecePose& p) { add(p.position); add(p.rotation); add(p.scale); };
    if (node->kind == NodeKind::ToVolume) {
        const auto* s = std::get_if<geometry::VolumeSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->resolution);
    } else if (node->kind == NodeKind::VolumeTransform) {
        const auto* s = std::get_if<geometry::VolumeTransformSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->position); add(s->rotationDegrees); add(s->scale);
    } else if (node->kind == NodeKind::VolumeCrack) {
        const auto* s = std::get_if<geometry::VolumeCrackSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->width); add(s->depth); add(s->variation); add(s->noise); add(s->noiseScale); add(s->seed);
    } else if (node->kind == NodeKind::PlaneCuts) {
        const auto* s = std::get_if<geometry::PlaneCutsSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->count); add(s->seed); add(s->depthMin); add(s->depthMax); add(s->distribution);
        add(s->scope); add(s->radius);
        add(s->systems); add(s->rotationDegrees); add(s->spreadDegrees); add(s->blend);
    } else if (node->kind == NodeKind::VolumeBoolean) {
        const auto* s = std::get_if<geometry::VolumeBooleanSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->operation); add(s->blend);
    } else if (node->kind == NodeKind::VolumeToMesh) {
        const auto* s = std::get_if<geometry::VolumeToMeshSettings>(&node->settings);
        const auto method = s ? s->method : geometry::VolumeMeshingMethod::MarchingTetrahedra;
        add(method);
    } else if (const auto* scatter = std::get_if<geometry::ScatterSettings>(&node->settings)) {
        add(scatter->count); add(scatter->seed); add(scatter->version);
    } else if (const auto* voronoi = std::get_if<geometry::VoronoiSettings>(&node->settings)) {
        add(voronoi->rotation); add(voronoi->stretch); add(voronoi->version);
    } else if (const auto* selection = std::get_if<geometry::PieceSelectSettings>(&node->settings)) {
        add(selection->mode); add(selection->outerFaces); add(selection->seed); add(selection->minimum); add(selection->maximum);
        add(selection->minVolume); add(selection->maxVolume); add(selection->fraction); add(selection->invert); add(selection->producer); add(selection->generation);
        add(selection->ids.size()); for (auto value : selection->ids) add(value);
    } else if (const auto* filter = std::get_if<geometry::PieceFilterSettings>(&node->settings)) {
        add(filter->keep);
    } else if (const auto* transform = std::get_if<geometry::PieceTransformSettings>(&node->settings)) {
        pose(transform->pose); add(transform->individual); add(transform->producer); add(transform->generation); add(transform->overrides.size());
        for (const auto& value : transform->overrides) { add(value.id); pose(value.pose); }
    } else if (const auto* uv = std::get_if<geometry::UvUnwrapSettings>(&node->settings)) {
        add(uv->resolution); add(uv->padding); add(uv->quality);
    } else if (node->kind != NodeKind::PiecesToMesh && node->kind != NodeKind::Merge &&
               node->kind != NodeKind::UvUnwrap && node->kind != NodeKind::MaterialBake &&
               node->kind != NodeKind::ApplyMaterial && node->kind != NodeKind::MeshOutput) return std::nullopt;
    for (const auto& pin : node->inputs) {
        // 材質とマスクは形状を変えない。結果に残るのは接続先のIDだけなので、接続だけをキーへ含める。
        // ここで打ち切らないと、Apply Material より下流のボリュームを毎回作り直すことになる。
        if (pin.valueType == ValueType::Material || pin.valueType == ValueType::Mask) {
            if (node->kind == NodeKind::MaterialBake || node->kind == NodeKind::ApplyMaterial)
                add(graph.FindUpstreamPin(pin.id));
            continue;
        }
        const auto* upstream = graph.FindUpstreamNodeForPin(pin.id);
        add(pin.id);
        const GraphId source = upstream ? upstream->id : 0; add(source);
        if (!upstream) continue;
        const auto parent = VolumeKey(graph, upstream->id, depth + 1);
        if (!parent) return std::nullopt;
        key += *parent;
    }
    return key;
}
}  // namespace
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview, RockEvaluationCache* persistent,
                            geometry::VolumeMeshingMethod previewMethod, std::stop_token stop) {
    if (persistent) {
        std::erase_if(persistent->pieceEntries, [&](const auto& item) { return !graph.FindNode(item.first); });
        std::erase_if(persistent->uvs, [&](const auto& item) { return !graph.FindNode(item.first); });
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
        if (stop.stop_requested()) return Failure(id, "Graph", "評価をキャンセルしました");
        if (const auto found = cache.find(id); found != cache.end()) return found->second;
        if (depth > 256 || active.contains(id))
            return Failure(id, "Graph", "循環または評価深さの上限を検出しました");
        const auto* node = graph.FindNode(id);
        if (!node) return {};
        const bool volumeCache = node->kind == NodeKind::RandomBoxes || node->kind == NodeKind::ToVolume ||
                                 node->kind == NodeKind::VolumeTransform || node->kind == NodeKind::VolumeBoolean ||
                                 node->kind == NodeKind::PlaneCuts || node->kind == NodeKind::VolumeCrack ||
                                 node->kind == NodeKind::VolumeToMesh;
        const auto persistentKey = persistent && volumeCache ? VolumeKey(graph, id) : std::nullopt;
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
        if (IsPieceNodeKind(node->kind)) {
            return finish(EvaluatePieceNode(graph, *node, persistent, [&](GraphId upstream) { return evaluate(upstream, depth+1); }, stop));
        } else if (node->kind == NodeKind::ApplyMaterial) {
            const auto* upstream = graph.FindUpstreamNodeForPin(node->inputs[0].id);
            const auto* material = graph.FindUpstreamNodeForPin(node->inputs[1].id);
            const auto* mask = graph.FindUpstreamNodeForPin(node->inputs[2].id);
            if (!upstream || !material || material->kind != NodeKind::Surface)
                return finish(Failure(id, "Apply Material", "MeshとSurfaceを接続してください"));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty())
                return finish(Failure(id, "Apply Material", "生成メッシュが必要です"));
            if (const auto* layer = std::get_if<LayerNodeSettings>(&material->settings); layer && !layer->layer.enabled)
                return finish(result);
            if (mask) {
                const auto* settings = std::get_if<MaterialMaskSettings>(&mask->settings);
                if (!settings || !std::isfinite(settings->value) || settings->value < 0 || settings->value > 1 ||
                    !std::isfinite(settings->repeatMeters) || settings->repeatMeters < .001f || settings->repeatMeters > 10000)
                    return finish(Failure(id, "Apply Material", "マスクの設定が不正です"));
            }
            for (auto& rock : result.rocks) {
                if (rock.volume) return finish(Failure(id, "Apply Material", "Volume to Meshを通してください"));
                if (!mask) rock.materials.clear();
                else if (rock.materials.empty() && rock.materialSource) rock.materials.push_back({rock.materialSource, 0});
                if (rock.materials.size() >= 8) return finish(Failure(id, "Apply Material", "素材の重ね合わせは8段までです"));
                rock.materials.push_back({material->id, mask ? mask->id : 0});
                rock.materialSource = 0;
                rock.bakeSource = 0;
            }
        } else if (node->kind == NodeKind::UvUnwrap || node->kind == NodeKind::MaterialBake) {
            const auto* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!upstream) return finish(Failure(id, "UV / Bake", "Mesh入力を接続してください"));
            result = evaluate(upstream->id, depth+1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty()) return finish(Failure(id, "UV / Bake", "生成メッシュを接続してください"));
            if (node->kind == NodeKind::UvUnwrap) {
                geometry::Mesh combined;
                for (const auto& rock : result.rocks) {
                    if (rock.volume) return finish(Failure(id, "UV Unwrap", "先にVolume to Meshへ接続してください"));
                    const auto offset = static_cast<uint32_t>(combined.positions.size());
                    combined.positions.insert(combined.positions.end(), rock.mesh.positions.begin(), rock.mesh.positions.end());
                    for (auto face : rock.mesh.triangles) { for (auto& i : face) i += offset; combined.triangles.push_back(face); }
                }
                const auto* settings = std::get_if<geometry::UvUnwrapSettings>(&node->settings);
                if (!settings) return finish(Failure(id, "UV Unwrap", "設定がありません"));
                geometry::Mesh unwrapped;
                bool hit = false;
                if (persistent) if (auto found = persistent->uvs.find(id); found != persistent->uvs.end()) {
                    const auto& entry = found->second;
                    hit = entry.settings == *settings && entry.input.positions == combined.positions && entry.input.triangles == combined.triangles;
                    if (hit) unwrapped = entry.output;
                }
                if (!hit) {
                    std::string error;
                    unwrapped = geometry::UnwrapMesh(combined, *settings, error);
                    if (!error.empty()) return finish(Failure(id, "UV Unwrap", error));
                    if (persistent) persistent->uvs[id] = {std::move(combined), unwrapped, *settings};
                }
                GeneratedRock rock; rock.source = id; rock.mesh = std::move(unwrapped);
                rock.materials = result.rocks[0].materials;
                rock.materialSource = result.rocks[0].materialSource;
                for (const auto& inputRock : result.rocks)
                    if (inputRock.materials != rock.materials || inputRock.materialSource != rock.materialSource)
                        return finish(Failure(id, "UV Unwrap", "異なる素材の枝はUV Unwrap後に適用してください"));
                result = {}; result.rocks.push_back(std::move(rock));
            } else {
                if (result.rocks.size() != 1 || !geometry::HasValidUvs(result.rocks[0].mesh))
                    return finish(Failure(id, "Material Bake", "UV Unwrapの出力を接続してください"));
                const auto* surface = graph.FindUpstreamNodeForPin(node->inputs[1].id);
                if (surface) {
                    result.rocks[0].materials.clear();
                    result.rocks[0].materialSource = surface->id;
                } else if (result.rocks[0].materials.empty() && !result.rocks[0].materialSource)
                    return finish(Failure(id, "Material Bake", "Apply Materialを通すかMaterialにSurfaceを接続してください"));
                result.rocks[0].source = id;
                result.rocks[0].bakeSource = id;
            }
        } else if (node->kind == NodeKind::RandomBoxes) {
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
            if (!settings || !upstream) return finish(Failure(id, "To Volume", "Mesh出力を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            if (input.hasModels || input.rocks.empty())
                return finish(Failure(id, "To Volume", "閉じたMeshが必要です。Modelは直接変換できません"));
            geometry::Mesh combined;
            std::vector<geometry::OrientedBox> boxes;
            bool allBoxes = true;
            for (const auto& rock : input.rocks) {
                if (rock.volume) return finish(Failure(id, "To Volume", "Mesh入力が必要です"));
                allBoxes &= bool(rock.boxes);
                if (rock.boxes) boxes.insert(boxes.end(), rock.boxes->begin(), rock.boxes->end());
                const auto offset = static_cast<uint32_t>(combined.positions.size());
                combined.positions.insert(combined.positions.end(), rock.mesh.positions.begin(), rock.mesh.positions.end());
                for (auto face : rock.mesh.triangles) { for (auto& index : face) index += offset; combined.triangles.push_back(face); }
            }
            std::string error;
            auto volume = allBoxes && boxes.size() <= 32
                ? geometry::BoxesToVolume(boxes, *settings, error)
                : geometry::MeshToVolume(combined, *settings, error, stop);
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
        } else if (node->kind == NodeKind::VolumeCrack) {
            const auto* settings = std::get_if<geometry::VolumeCrackSettings>(&node->settings);
            const bool wired = node->inputs.size() >= 2;
            const auto* upstream = wired ? graph.FindUpstreamNodeForPin(node->inputs[0].id) : nullptr;
            const auto* scatter = wired ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr;
            if (!settings || !upstream || !scatter)
                return finish(Failure(id, "Volume Crack", "Volume と Points の両方を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            const auto scattered = evaluate(scatter->id, depth + 1);
            if (!scattered.error.empty()) return finish(scattered);
            if (input.rocks.size() != 1 || !input.rocks[0].volume)
                return finish(Failure(id, "Volume Crack", "ボリュームが必要です"));
            if (!scattered.points)
                return finish(Failure(id, "Volume Crack", "Scatter Points の出力が必要です"));
            std::string error;
            auto cracked =
                geometry::CrackVolume(*input.rocks[0].volume, scattered.points->positions, *settings, error);
            if (!error.empty()) return finish(Failure(id, "Volume Crack", error));
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(cracked));
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::PlaneCuts) {
            const auto* settings = std::get_if<geometry::PlaneCutsSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream)
                return finish(Failure(id, "Plane Cuts", "Volume 出力を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            if (input.rocks.size() != 1 || !input.rocks[0].volume)
                return finish(Failure(id, "Plane Cuts", "ボリュームが必要です"));
            std::string error;
            auto cut = geometry::CutVolume(*input.rocks[0].volume, *settings, error);
            if (!error.empty()) return finish(Failure(id, "Plane Cuts", error));
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(cut));
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::VolumeBoolean) {
            const auto* settings = std::get_if<geometry::VolumeBooleanSettings>(&node->settings);
            const bool wired = node->inputs.size() >= 2;
            const auto* upstreamA = wired ? graph.FindUpstreamNodeForPin(node->inputs[0].id) : nullptr;
            const auto* upstreamB = wired ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr;
            if (!settings || !upstreamA || !upstreamB)
                return finish(Failure(id, "Volume Boolean", "A と B の両方に Volume 出力を接続してください"));
            const auto inputA = evaluate(upstreamA->id, depth + 1);
            if (!inputA.error.empty()) return finish(inputA);
            const auto inputB = evaluate(upstreamB->id, depth + 1);
            if (!inputB.error.empty()) return finish(inputB);
            if (inputA.rocks.size() != 1 || !inputA.rocks[0].volume || inputB.rocks.size() != 1 ||
                !inputB.rocks[0].volume)
                return finish(Failure(id, "Volume Boolean", "A と B にはボリュームが必要です"));
            std::string error;
            auto combined =
                geometry::CombineVolumes(*inputA.rocks[0].volume, *inputB.rocks[0].volume, *settings, error);
            if (!error.empty()) return finish(Failure(id, "Volume Boolean", error));
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(combined));
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
                return finish(Failure(id, "Base Shape", settings ? error : "設定がありません"));
            GeneratedRock rock;
            rock.source = id;
            rock.mesh = std::move(mesh);
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::Model || node->kind == NodeKind::Transform) {
            result.hasModels = true;
        } else if (node->kind == NodeKind::Merge || node->kind == NodeKind::MeshOutput) {
            for (const auto& pin : node->inputs) {
                if (pin.valueType == ValueType::Material) continue;
                if (const auto* upstream = graph.FindUpstreamNodeForPin(pin.id)) {
                    const auto input = evaluate(upstream->id, depth + 1);
                    if (!input.error.empty()) return finish(input);
                    Append(result, input);
                }
            }
            if (node->kind == NodeKind::MeshOutput && node->inputs.size() > 1) {
                const auto* surface = graph.FindUpstreamNodeForPin(node->inputs[1].id);
                if (surface && surface->kind == NodeKind::Surface)
                    for (auto& rock : result.rocks) { rock.materialSource = surface->id; rock.materials.clear(); rock.bakeSource = 0; }
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
    if (preview != 0) {
        auto result = evaluate(preview, 0);
        PreparePiecePreview(result, preview);
        return preparePreview(std::move(result));
    }
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
