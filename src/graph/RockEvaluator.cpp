#include "graph/RockEvaluator.h"
#include "graph/PieceEvaluator.h"
#include "core/ImageIo.h"
#include "core/PathUtf8.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
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
                         [&](const auto& other) { return other.source == item.source && other.meshHistory == item.meshHistory && other.materials == item.materials && other.materialSource == item.materialSource && other.bakeSource == item.bakeSource; }))
            target.rocks.push_back(item);
    target.hasModels |= source.hasModels;
    // 撒いた岩は、同じノードの同じ岩グラフが 2 度届いても 1 回だけ描く。
    for (const auto& set : source.rockInstances)
        if (std::none_of(target.rockInstances.begin(), target.rockInstances.end(),
                         [&](const auto& other) { return other.source == set.source && other.scene == set.scene; }))
            target.rockInstances.push_back(set);
}
// 対応する枝を完全な値で比較し、改版番号の巻き戻りにも対応する。
std::optional<std::string> VolumeKey(const NodeGraph& graph, GraphId id, const std::map<GraphId,std::string>& heightKeys, size_t depth = 0, bool usesHeight = false) {
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
    } else if (node->kind == NodeKind::VolumeNoise) {
        const auto* s = std::get_if<geometry::VolumeNoiseSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->type); add(s->amount); add(s->scale); add(s->octaves); add(s->warp); add(s->warpScale);
        add(s->seed);
    } else if (node->kind == NodeKind::VolumeSmooth) {
        const auto* s = std::get_if<geometry::VolumeSmoothSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->mode); add(s->radius); add(s->amount); add(s->upwardFocus);
    } else if (node->kind == NodeKind::VolumeEdgeWear) {
        const auto* s = std::get_if<geometry::VolumeEdgeWearSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->radius); add(s->amount); add(s->noise); add(s->noiseScale); add(s->upwardFocus); add(s->seed);
    } else if (node->kind == NodeKind::VolumeClip) {
        const auto* s = std::get_if<geometry::VolumeClipSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->height); add(s->invert); add(int(s->mode)); add(s->embed);
    } else if (node->kind == NodeKind::VolumeClose) {
        const auto* s = std::get_if<geometry::VolumeCloseSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->mode); add(s->width); add(s->distance); add(s->threshold); add(s->samples); add(s->softness);
    } else if (node->kind == NodeKind::VolumeTerrace) {
        const auto* s = std::get_if<geometry::VolumeTerraceSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->step); add(s->depth); add(s->ratio); add(s->softness); add(s->variation); add(s->noise); add(s->noiseScale);
        add(s->rotationDegrees); add(s->seed);
    } else if (node->kind == NodeKind::ParallelPlanes) {
        const auto* s = std::get_if<geometry::ParallelPlanesSettings>(&node->settings);
        if (!s) return std::nullopt;
        add(s->rotationDegrees); add(s->spacing); add(s->offset); add(s->variation); add(s->seed);
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
    } else if (const auto* layers = std::get_if<geometry::LayeredBoxesSettings>(&node->settings)) {
        add(layers->count); add(layers->size); add(layers->rotation); add(layers->position); add(layers->gap);
        add(layers->thicknessVariation); add(layers->sizeVariation); add(layers->offset); add(layers->seed);
    } else if (const auto* scatter = std::get_if<geometry::ScatterSettings>(&node->settings)) {
        add(scatter->count); add(scatter->seed); add(scatter->version); add(scatter->planar);
    } else if (const auto* voronoi = std::get_if<geometry::VoronoiSettings>(&node->settings)) {
        add(voronoi->rotation); add(voronoi->stretch); add(voronoi->version);
    } else if (const auto* selection = std::get_if<geometry::PieceSelectSettings>(&node->settings)) {
        add(selection->mode); add(selection->outerFaces); add(selection->seed); add(selection->minimum); add(selection->maximum);
        add(selection->minVolume); add(selection->maxVolume); add(selection->fraction); add(selection->invert); add(selection->producer); add(selection->generation); add(selection->layer);
        add(selection->rimLayers); add(selection->rimSide); add(selection->rimFalloff);
        add(selection->peelNoise); add(selection->protectCore);
        add(selection->ids.size()); for (auto value : selection->ids) add(value);
    } else if (const auto* filter = std::get_if<geometry::PieceFilterSettings>(&node->settings)) {
        add(filter->keep);
    } else if (const auto* transform = std::get_if<geometry::PieceTransformSettings>(&node->settings)) {
        pose(transform->pose); add(transform->individual); add(transform->producer); add(transform->generation); add(transform->overrides.size());
        for (const auto& value : transform->overrides) { add(value.id); pose(value.pose); }
    } else if (const auto* displace = std::get_if<geometry::DisplaceSettings>(&node->settings)) {
        add(displace->amount); add(displace->midpoint); usesHeight = true;
    } else if (const auto* subdivide = std::get_if<geometry::SubdivideSettings>(&node->settings)) {
        add(subdivide->levels); add(subdivide->threshold);
    } else if (const auto* decimate = std::get_if<geometry::DecimateSettings>(&node->settings)) {
        add(decimate->targetTriangles); add(decimate->maxError); add(decimate->creaseWeight);
    } else if (const auto* terrain = std::get_if<geometry::HeightmapSettings>(&node->settings)) {
        add(terrain->source); add(terrain->width); add(terrain->depth); add(terrain->minHeight); add(terrain->maxHeight);
        add(terrain->resolution); add(terrain->textureResolution);
        if (terrain->source == geometry::HeightmapSource::Image) {
            // 画像はパスと、ファイルの大きさ・更新時刻で見分ける（描き直した画像を読み直すため）。
            key += terrain->image;
            std::error_code error;
            const auto path = FromUtf8(terrain->image);
            add(uint64_t(std::filesystem::file_size(path, error)));
            add(std::filesystem::last_write_time(path, error).time_since_epoch().count());
        } else {
            add(terrain->seed); add(terrain->featureSize); add(terrain->roughness); add(terrain->peak);
        }
    } else if (const auto* asset = std::get_if<RockAssetSettings>(&node->settings)) {
        // 切り替えの大きさ（screenSize）は表示だけに使うので含めない。
        add(asset->lodCount); add(asset->maxTriangles);
        for (const float percent : asset->trianglePercent) add(percent);
        add(RockAssetSharedUvLods(*asset));
    } else if (const auto* remesh = std::get_if<geometry::RemeshSettings>(&node->settings)) {
        add(remesh->edgeLength); add(remesh->iterations); add(remesh->featureAngle);
    } else if (const auto* uv = std::get_if<geometry::UvUnwrapSettings>(&node->settings)) {
        add(uv->resolution); add(uv->padding); add(uv->quality);
    } else if (const auto* deposition = std::get_if<geometry::DepositionMaskSettings>(&node->settings)) {
        add(deposition->amount); add(deposition->distance); add(deposition->maxSlopeDegrees);
        add(deposition->recessPreference); add(deposition->resolution); add(deposition->samples);
    } else if (const auto* noise = std::get_if<geometry::NoiseMaskSettings>(&node->settings)) {
        add(noise->size); add(noise->contrast); add(noise->seed); add(noise->detail); add(noise->warp); add(noise->resolution);
    } else if (const auto* occlusion = std::get_if<geometry::ShapeMaskSettings>(&node->settings)) {
        // 反転は画像を変えない（使う側で掛ける）ので含めない。
        add(occlusion->type); add(occlusion->distance); add(occlusion->samples); add(occlusion->resolution); add(occlusion->low); add(occlusion->high); add(occlusion->gamma);
    } else if (const auto* combine = std::get_if<geometry::MaskCombineSettings>(&node->settings)) {
        // 反転も画像に焼き込むので含める。入力（A / B）は下の Mask のピンの処理で辿る。
        add(combine->operation); add(combine->mix); add(combine->low); add(combine->high); add(combine->gamma); add(combine->invert);
    } else if (const auto* maskFilter = std::get_if<geometry::MaskFilterSettings>(&node->settings)) {
        // 反転も画像に焼き込むので含める。入力は下の Mask のピンの処理で辿る。
        add(maskFilter->type); add(maskFilter->radius); add(maskFilter->amount); add(maskFilter->inputLow);
        add(maskFilter->inputHigh); add(maskFilter->gamma); add(maskFilter->outputLow); add(maskFilter->outputHigh);
        add(maskFilter->invert);
    } else if (const auto* apply = std::get_if<ApplyMaterialSettings>(&node->settings)) {
        add(apply->heightBlend); add(apply->heightBlendRange); add(apply->opacity);
    } else if (node->kind != NodeKind::PiecesToMesh && node->kind != NodeKind::Merge &&
               node->kind != NodeKind::UvUnwrap && node->kind != NodeKind::MaterialBake &&
               node->kind != NodeKind::ApplyMaterial && node->kind != NodeKind::MeshOutput) return std::nullopt;
    for (const auto& pin : node->inputs) {
        // 材質とマスクは形状を変えない。結果に残るのは接続先のIDだけなので、接続だけをキーへ含める。
        // ここで打ち切らないと、Apply Material より下流のボリュームを毎回作り直すことになる。
        if (pin.valueType == ValueType::Material || pin.valueType == ValueType::Mask) {
            if (node->kind == NodeKind::MaterialBake || node->kind == NodeKind::ApplyMaterial)
                add(graph.FindUpstreamPin(pin.id));
            // 無効なApply Materialは入力の素材束をそのまま通す。細分化のキャッシュにも反映する。
            if (node->kind == NodeKind::ApplyMaterial && pin.valueType == ValueType::Material)
                if (const auto* surface = graph.FindUpstreamNodeForPin(pin.id))
                    if (const auto* layer = std::get_if<LayerNodeSettings>(&surface->settings)) add(layer->layer.enabled);
            // 形状マスクの画像は下流の結果（rock.maskImages）に残る。マスクのノードの設定と、その入力メッシュで決まる。
            // Mask Combine の入力（A / B）もここを通り、入力のマスクのキーをつなぐ。
            if (const auto* shape = graph.FindUpstreamNodeForPin(pin.id); shape && IsImageMaskNodeKind(shape->kind)) {
                // Mask Combine / Mask Filter は入力の反転を画像に焼き込むので、常に含める。
                if (usesHeight || node->kind == NodeKind::Subdivide || node->kind == NodeKind::MaskCombine ||
                    node->kind == NodeKind::MaskFilter)
                    add(ImageMaskInvert(*shape));
                const auto maskKey = VolumeKey(graph, shape->id, heightKeys, depth + 1, false);
                if (!maskKey) return std::nullopt;
                add(maskKey->size()); key += *maskKey;
                continue;
            }
            if (usesHeight || node->kind == NodeKind::Subdivide) {
                const auto* source = graph.FindUpstreamNodeForPin(pin.id);
                if (source) {
                    if (const auto* mask = std::get_if<MaterialMaskSettings>(&source->settings)) {
                        add(mask->texture); add(mask->value); add(mask->repeatMeters); add(mask->invert); add(mask->triplanar);
                        if (!mask->texture) continue;
                    }
                    const auto found = heightKeys.find(source->id);
                    if (found == heightKeys.end()) return std::nullopt;
                    add(found->second.size()); key += found->second;
                }
            }
            continue;
        }
        const auto* upstream = graph.FindUpstreamNodeForPin(pin.id);
        add(pin.id);
        const GraphId source = upstream ? upstream->id : 0; add(source);
        if (!upstream) continue;
        const auto parent = VolumeKey(graph, upstream->id, heightKeys, depth + 1, usesHeight);
        if (!parent) return std::nullopt;
        key += *parent;
    }
    return key;
}
}  // namespace
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview, RockEvaluationCache* persistent,
                            geometry::VolumeMeshingMethod previewMethod, std::stop_token stop,
                            RockEvaluationProgress* progress, const MaterialHeight* heights) {
    const auto heightKeys = heights ? heights->CacheKeys() : std::map<GraphId,std::string>{};
    if (persistent) {
        std::erase_if(persistent->computations, [&](const auto& item) { return !graph.FindNode(item.first); });
        std::erase_if(persistent->pieceEntries, [&](const auto& item) { return !graph.FindNode(item.first); });
        std::erase_if(persistent->pieceOutputs, [&](const auto& item) { return !graph.FindNode(item.first); });
        std::erase_if(persistent->detailCounts, [&](const auto& item) { return !graph.FindNode(item.first); });
        std::erase_if(persistent->uvs, [&](const auto& item) { return !graph.FindNode(item.first); });
        std::erase_if(persistent->entries, [&](const auto& item) {
            const auto key = VolumeKey(graph, item.first, heightKeys);
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
        const bool volumeCache = node->kind == NodeKind::ParallelPlanes || node->kind == NodeKind::RandomBoxes || node->kind == NodeKind::ToVolume ||
                                 node->kind == NodeKind::VolumeTransform || node->kind == NodeKind::VolumeBoolean ||
                                 node->kind == NodeKind::PlaneCuts || node->kind == NodeKind::VolumeCrack ||
                                 node->kind == NodeKind::VolumeNoise || node->kind == NodeKind::Decimate || node->kind == NodeKind::Remesh ||
                                 node->kind == NodeKind::RockAsset || node->kind == NodeKind::Heightmap ||
                                 node->kind == NodeKind::VolumeSmooth || node->kind == NodeKind::VolumeTerrace ||
                                 node->kind == NodeKind::VolumeClose || node->kind == NodeKind::VolumeEdgeWear ||
                                 node->kind == NodeKind::VolumeClip ||
                                 node->kind == NodeKind::VolumeToMesh || node->kind == NodeKind::Subdivide || node->kind == NodeKind::Displace ||
                                 IsImageMaskNodeKind(node->kind);
        const auto persistentKey = persistent && volumeCache ? VolumeKey(graph, id, heightKeys) : std::nullopt;
        if (persistentKey) {
            const auto found = persistent->entries.find(id);
            if (found != persistent->entries.end()) return found->second.result;
        }
        if (persistent && volumeCache) ++persistent->computations[id];
        active.insert(id);
        // 計算中のノードを UI へ伝える。上流の評価から戻ったら、このノードへ戻す。
        const GraphId outer = progress ? progress->node.load(std::memory_order_relaxed) : 0;
        const auto report = [&](GraphId node, int stage, int percent) {
            if (!progress) return;
            progress->stage.store(stage, std::memory_order_relaxed);
            progress->percent.store(percent, std::memory_order_relaxed);
            progress->node.store(node, std::memory_order_relaxed);
        };
        report(id, 0, -1);
        RockEvaluation result;
        const auto finish = [&](RockEvaluation value) {
            active.erase(id);
            report(outer, 0, -1);
            cache[id] = value;
            if (persistentKey && value.error.empty())
                persistent->entries[id] = {*persistentKey, value};
            return value;
        };
        if (node->kind == NodeKind::ParallelPlanes) {
            const auto* settings = std::get_if<geometry::ParallelPlanesSettings>(&node->settings);
            if (!settings) return finish(Failure(id, "Parallel Planes", "設定がありません"));
            std::string error;
            auto planes = geometry::MakeParallelPlanes(*settings, error);
            if (!error.empty()) return finish(Failure(id, "Parallel Planes", error));
            result.planes = std::make_shared<const geometry::StructurePlanes>(planes);
            return finish(std::move(result));
        } else if (IsPieceNodeKind(node->kind)) {
            auto pieces = EvaluatePieceNode(graph, *node, persistent, [&](GraphId upstream) { return evaluate(upstream, depth+1); }, stop);
            if (persistent) {
                if (pieces.error.empty() && pieces.pieces) persistent->pieceOutputs[id] = pieces.pieces;
                else persistent->pieceOutputs.erase(id);
            }
            return finish(std::move(pieces));
        } else if (node->kind == NodeKind::ShapeMask || node->kind == NodeKind::NoiseMask || node->kind == NodeKind::DepositionMask) {
            const char* title = node->kind == NodeKind::DepositionMask ? "Deposition Mask" : node->kind == NodeKind::NoiseMask ? "Noise Mask" : "Shape Mask";
            const auto* deposition = std::get_if<geometry::DepositionMaskSettings>(&node->settings);
            const auto* noise = std::get_if<geometry::NoiseMaskSettings>(&node->settings);
            const auto* settings = std::get_if<geometry::ShapeMaskSettings>(&node->settings);
            const auto* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if ((!settings && !noise && !deposition) || !upstream) return finish(Failure(id, title, "UV付きのMeshを接続してください"));
            result = evaluate(upstream->id, depth + 1);
            report(id, 0, 0);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.size() != 1 || result.rocks[0].volume)
                return finish(Failure(id, title, "UV付きの生成メッシュを1つ接続してください（UV Unwrapの出力）"));
            std::string error;
            auto image = deposition ? geometry::DepositionMask(result.rocks[0].mesh, *deposition, error, stop, [&](int p) { report(id, 0, p); })
                               : noise ? geometry::NoiseMask(result.rocks[0].mesh, *noise, error, stop, [&](int p) { report(id, 0, p); })
                               : geometry::ShapeMask(result.rocks[0].mesh, *settings, error, stop, [&](int p) { report(id, 0, p); });
            if (!error.empty()) return finish(Failure(id, title, error));
            result.rocks[0].previewMask = std::make_shared<const geometry::MaskImage>(std::move(image));
            result.rocks[0].previewMaskInvert = ImageMaskInvert(*node);
        } else if (node->kind == NodeKind::MaskCombine) {
            const auto* settings = std::get_if<geometry::MaskCombineSettings>(&node->settings);
            const auto* a = node->inputs.size() > 1 ? graph.FindUpstreamNodeForPin(node->inputs[0].id) : nullptr;
            const auto* b = node->inputs.size() > 1 ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr;
            if (!settings || !a || !b) return finish(Failure(id, "Mask Combine", "AとBにShape Mask、Noise Mask、Deposition MaskまたはMask Combineを接続してください"));
            // Material Mask は面の中心で読む定数か画像で、UV空間の画像を持たない。
            if (!IsImageMaskNodeKind(a->kind) || !IsImageMaskNodeKind(b->kind))
                return finish(Failure(id, "Mask Combine", "Material Maskは合成できません。Shape Mask、Noise Mask、Deposition MaskまたはMask Combineを接続してください"));
            result = evaluate(a->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            const auto second = evaluate(b->id, depth + 1);
            report(id, 0, 0);
            if (!second.error.empty()) return finish(second);
            if (result.rocks.size() != 1 || !result.rocks[0].previewMask || second.rocks.size() != 1 || !second.rocks[0].previewMask)
                return finish(Failure(id, "Mask Combine", "AとBのマスクがありません"));
            // 合成した画像は A のメッシュのUVで貼る。B は同じアトラスのUVを持つ必要がある。
            if (result.rocks[0].mesh.uvWidth != second.rocks[0].mesh.uvWidth || result.rocks[0].mesh.uvHeight != second.rocks[0].mesh.uvHeight ||
                result.rocks[0].mesh.triangles.size() != second.rocks[0].mesh.triangles.size())
                return finish(Failure(id, "Mask Combine", "AとBには同じUVのMeshから作ったマスクを接続してください（同じUV Unwrapの出力から分ける）"));
            // 入力の反転は、キャッシュに残った評価結果ではなく、いまのノードの設定から読む（Shape Mask のキーに反転は入っていない）。
            std::string error;
            auto image = geometry::CombineMasks(*result.rocks[0].previewMask, ImageMaskInvert(*a),
                                                *second.rocks[0].previewMask, ImageMaskInvert(*b), *settings, error);
            if (!error.empty()) return finish(Failure(id, "Mask Combine", error));
            result.rocks[0].previewMask = std::make_shared<const geometry::MaskImage>(std::move(image));
            result.rocks[0].previewMaskInvert = false;
        } else if (node->kind == NodeKind::MaskFilter) {
            const auto* settings = std::get_if<geometry::MaskFilterSettings>(&node->settings);
            const auto* source = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !source)
                return finish(Failure(id, "Mask Filter", "MaskにShape Mask、Noise Mask、Deposition Mask、Mask CombineまたはMask Filterを接続してください"));
            // Material Mask は面の中心で読む定数か画像で、UV空間の画像を持たない。
            if (!IsImageMaskNodeKind(source->kind))
                return finish(Failure(id, "Mask Filter", "Material Maskは加工できません。Shape Maskなど、UVの画像を作るマスクを接続してください"));
            result = evaluate(source->id, depth + 1);
            report(id, 0, 0);
            if (!result.error.empty()) return finish(result);
            if (result.rocks.size() != 1 || !result.rocks[0].previewMask)
                return finish(Failure(id, "Mask Filter", "入力のマスクがありません"));
            // 入力の反転は、キャッシュに残った評価結果ではなく、いまのノードの設定から読む（Shape Mask のキーに反転は入っていない）。
            std::string error;
            auto image = geometry::FilterMask(result.rocks[0].mesh, *result.rocks[0].previewMask, ImageMaskInvert(*source),
                                              *settings, error, stop, [&](int p) { report(id, 0, p); });
            if (!error.empty()) return finish(Failure(id, "Mask Filter", error));
            result.rocks[0].previewMask = std::make_shared<const geometry::MaskImage>(std::move(image));
            result.rocks[0].previewMaskInvert = false;
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
            std::shared_ptr<const geometry::MaskImage> shapeMask;
            if (mask && IsImageMaskNodeKind(mask->kind)) {
                const auto masked = evaluate(mask->id, depth + 1);
                if (!masked.error.empty()) return finish(masked);
                shapeMask = masked.rocks[0].previewMask;
                // マスクはUVで貼る。マスクを作ったメッシュと同じアトラスのUVを持つメッシュにだけ使える。
                for (const auto& rock : result.rocks)
                    if (!geometry::HasValidUvs(rock.mesh) || rock.mesh.uvWidth != masked.rocks[0].mesh.uvWidth ||
                        rock.mesh.uvHeight != masked.rocks[0].mesh.uvHeight)
                        return finish(Failure(id, "Apply Material", "Shape MaskのMeshと同じUVのMeshを接続してください（同じUV Unwrapの出力から分ける）"));
            } else if (mask) {
                const auto* settings = std::get_if<MaterialMaskSettings>(&mask->settings);
                if (!settings || !std::isfinite(settings->value) || settings->value < 0 || settings->value > 1 ||
                    !std::isfinite(settings->repeatMeters) || settings->repeatMeters < .001f || settings->repeatMeters > 10000)
                    return finish(Failure(id, "Apply Material", "マスクの設定が不正です"));
            }
            const auto* apply = std::get_if<ApplyMaterialSettings>(&node->settings);
            const float opacity = apply ? std::clamp(apply->opacity, 0.f, 1.f) : 1.f;
            for (auto& rock : result.rocks) {
                if (rock.volume) return finish(Failure(id, "Apply Material", "Volume to Meshを通してください"));
                // 全面置換は不透明なときだけ。半透明なら Mask 未接続でも上流へ重ねる。
                if (!mask && opacity >= 1) { rock.materials.clear(); rock.maskImages.clear(); }
                else if (rock.materials.empty() && rock.materialSource) rock.materials.push_back({rock.materialSource, 0});
                if (rock.materials.size() >= 8) return finish(Failure(id, "Apply Material", "素材の重ね合わせは8段までです"));
                GeneratedRock::MaterialBinding binding{material->id, mask ? mask->id : 0};
                if (apply) {
                    binding.heightBlend = apply->heightBlend;
                    binding.heightBlendRange = std::clamp(apply->heightBlendRange, .01f, 1.f);
                }
                binding.opacity = opacity;
                rock.materials.push_back(binding);
                if (shapeMask) rock.maskImages[mask->id] = shapeMask;
                rock.previewMask.reset();
                rock.materialSource = 0;
                rock.bakeSource = 0;
            }
        } else if (node->kind == NodeKind::Subdivide || node->kind == NodeKind::Displace) {
            const char* name = node->kind == NodeKind::Subdivide ? "Subdivide" : "Displace";
            const auto* upstream = graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!upstream) return finish(Failure(id, name, "Mesh入力を接続してください"));
            result = evaluate(upstream->id, depth+1);
            report(id, 0, 0);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty()) return finish(Failure(id, name, "生成メッシュを接続してください"));
            size_t inputCount = 0;
            for (const auto& rock : result.rocks) inputCount += rock.mesh.triangles.size();
            size_t predicted = inputCount;
            if (const auto* settings = std::get_if<geometry::SubdivideSettings>(&node->settings)) {
                if (settings->levels < 0 || settings->levels > 6) return finish(Failure(id, name, "細分化の段階数は0〜6です"));
                for (int level=0;level<settings->levels;++level) {
                    if (predicted > geometry::kMaxDetailTriangles/4) return finish(Failure(id, name, "出力合計が400万面を超えます"));
                    predicted *= 4;
                }
            }
            if (predicted > geometry::kMaxDetailTriangles) return finish(Failure(id, name, "出力合計が400万面を超えます"));
            if (persistent) persistent->detailCounts[id] = {inputCount,predicted};
            size_t total = 0;
            for (auto& rock : result.rocks) {
                if (rock.volume) return finish(Failure(id, name, "Volume to Meshを通してください"));
                std::string error;
                if (node->kind == NodeKind::Subdivide) {
                    // マスクで割る面を選ぶ。面の中心で値を読む。Shape Mask は UV の画像、Material Mask は定数か画像（UV / Triplanar）。
                    std::vector<float> faceMask;
                    if (const auto* maskNode = node->inputs.size() > 1 ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr) {
                        faceMask.assign(rock.mesh.triangles.size(), 1.f);
                        const auto centroidUv = [&](size_t f) {
                            const auto& u = rock.mesh.cornerUvs[f];
                            return geometry::Mesh::Uv{(u[0].u + u[1].u + u[2].u) / 3, (u[0].v + u[1].v + u[2].v) / 3};
                        };
                        if (IsImageMaskNodeKind(maskNode->kind)) {
                            const auto masked = evaluate(maskNode->id, depth + 1);
                            report(id, 0, 0);
                            if (!masked.error.empty()) return finish(masked);
                            const auto& image = masked.rocks[0].previewMask;
                            if (!geometry::HasValidUvs(rock.mesh) || rock.mesh.uvWidth != masked.rocks[0].mesh.uvWidth ||
                                rock.mesh.uvHeight != masked.rocks[0].mesh.uvHeight)
                                return finish(Failure(id, name, "Shape MaskのMeshと同じUVのMeshを接続してください"));
                            const bool invert = ImageMaskInvert(*maskNode);
                            for (size_t f = 0; f < faceMask.size(); ++f) {
                                const auto uv = centroidUv(f);
                                const float v = image->Sample(uv.u, uv.v);
                                faceMask[f] = invert ? 1 - v : v;
                            }
                        } else if (const auto* mask = std::get_if<MaterialMaskSettings>(&maskNode->settings)) {
                            const graph::ScalarField* field = nullptr;
                            if (mask->texture) {
                                if (!heights || !heights->masks.contains(maskNode->id)) return finish(Failure(id, name, "マスク画像を読み込めません"));
                                field = &heights->masks.at(maskNode->id);
                                if (!mask->triplanar && !geometry::HasValidUvs(rock.mesh))
                                    return finish(Failure(id, name, "UVのマスク画像には先にUV Unwrapが必要です"));
                            }
                            for (size_t f = 0; f < faceMask.size(); ++f) {
                                float v = mask->value;
                                if (field) {
                                    const float inv = 1 / mask->repeatMeters;
                                    if (mask->triplanar) {
                                        const auto t = rock.mesh.triangles[f];
                                        const auto a = rock.mesh.positions[t[0]], b = rock.mesh.positions[t[1]], c = rock.mesh.positions[t[2]];
                                        const geometry::Vec3 p{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
                                        const auto n = geometry::FaceNormal(rock.mesh, t);
                                        geometry::Vec3 w{std::pow(std::abs(n.x), 4.f), std::pow(std::abs(n.y), 4.f), std::pow(std::abs(n.z), 4.f)};
                                        const float sum = std::max(w.x + w.y + w.z, 1e-8f);
                                        v *= (field->Sample(p.z * inv, p.y * inv, true) * w.x + field->Sample(p.x * inv, p.z * inv, true) * w.y +
                                              field->Sample(p.x * inv, p.y * inv, true) * w.z) / sum;
                                    } else {
                                        const auto uv = centroidUv(f);
                                        v *= field->Sample(uv.u * inv, uv.v * inv, true);
                                    }
                                }
                                if (mask->invert) v = 1 - v;
                                faceMask[f] = std::clamp(v, 0.f, 1.f);
                            }
                        }
                    }
                    rock.mesh = geometry::SubdivideMesh(rock.mesh, std::get<geometry::SubdivideSettings>(node->settings), error, stop,
                                                       [&](int p) { report(id, 0, p); }, faceMask);
                } else {
                    if (!heights) return finish(Failure(id, name, "素材ハイトが準備されていません"));
                    auto bindings = rock.materials;
                    if (bindings.empty() && rock.materialSource) bindings.push_back({rock.materialSource, 0});
                    if (bindings.empty()) return finish(Failure(id, name, "先にApply Materialで素材を適用してください"));
                    for (const auto& binding : bindings) {
                        const auto found = heights->surfaces.find(binding.surface);
                        if (found == heights->surfaces.end()) return finish(Failure(id, name, "Surfaceのハイトがありません"));
                        if (!found->second.error.empty()) return finish(Failure(id, name, found->second.error));
                        const auto* maskNode = graph.FindNode(binding.mask);
                        const auto* mask = maskNode ? std::get_if<MaterialMaskSettings>(&maskNode->settings) : nullptr;
                        if (mask && mask->texture && !heights->masks.contains(binding.mask))
                            return finish(Failure(id, name, "マスク画像を読み込めません"));
                        if (!geometry::HasValidUvs(rock.mesh) && (found->second.mapping.method == compositor::MappingMethod::UV ||
                            (mask && mask->texture && !mask->triplanar) || rock.maskImages.contains(binding.mask)))
                            return finish(Failure(id, name, "UV投影には先にUV Unwrapが必要です。UVなしではTriplanarを使用してください"));
                    }
                    const bool wrap = heights->surfaces.at(bindings.front().surface).mapping.method == compositor::MappingMethod::Triplanar;
                    const auto sample = [&](geometry::Vec3 p, geometry::Vec3 n, geometry::Mesh::Uv uv) {
                        float h = .5f;
                        for (const auto& binding : bindings) {
                            const auto* maskNode = graph.FindNode(binding.mask);
                            const auto* mask = maskNode ? std::get_if<MaterialMaskSettings>(&maskNode->settings) : nullptr;
                            // 形状マスクは画像をUVで読み、定数のマスクとして渡す。
                            compositor::MaterialMask shape;
                            if (const auto image = rock.maskImages.find(binding.mask); image != rock.maskImages.end()) {
                                shape.value = image->second->Sample(uv.u, uv.v);
                                shape.invert = ImageMaskInvert(*maskNode);
                                mask = &shape;
                            }
                            h = heights->Sample(binding.surface, mask, binding.mask, p, n, uv, h, wrap,
                                                binding.heightBlend, binding.heightBlendRange, binding.opacity);
                        }
                        return h;
                    };
                    rock.mesh = geometry::DisplaceMesh(rock.mesh, std::get<geometry::DisplaceSettings>(node->settings), sample, error, stop,
                                                      [&](int p) { report(id, 0, p); });
                }
                if (!error.empty()) return finish(Failure(id, name, error));
                total += rock.mesh.triangles.size();
                if (total > geometry::kMaxDetailTriangles) return finish(Failure(id, name, "出力合計が400万面を超えます"));
                rock.meshHistory.push_back(rock.source);
                rock.source = id; rock.boxes.reset(); rock.bakeSource = 0;
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
                    // 展開し直すとUVが変わり、形状マスクの画像と合わなくなる。
                    if (!rock.maskImages.empty()) return finish(Failure(id, "UV Unwrap", "Shape Maskを使う素材は、UV Unwrapの後で適用してください"));
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
                    report(id, 1, 0);
                    unwrapped = geometry::UnwrapMesh(combined, *settings, error, stop,
                                                     [&](geometry::UvUnwrapStage stage, int percent) {
                                                         // 配置は収まるまで何度も詰め直し、重なりの修復では最初から
                                                         // やり直す。百分率が何度も 0 へ戻るので、UI へは島への分割の
                                                         // 進み具合だけを渡す。
                                                         const bool meaningful = stage == geometry::UvUnwrapStage::ComputeCharts;
                                                         report(id, int(stage) + 1, meaningful ? percent : -1);
                                                     });
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
                    result.rocks[0].maskImages.clear();
                    result.rocks[0].materialSource = surface->id;
                } else if (result.rocks[0].materials.empty() && !result.rocks[0].materialSource)
                    return finish(Failure(id, "Material Bake", "Apply Materialを通すかMaterialにSurfaceを接続してください"));
                result.rocks[0].source = id;
                result.rocks[0].bakeSource = id;
                // 焼くのはこのメッシュそのもの。上流の Rock Asset が残した照合用のメッシュは使わない。
                result.rocks[0].bakeMesh.reset();
                // High（ハイポリ）。法線とハイトの転写に使う。内容が変わったら焼き直しが要るので、ハッシュを持つ。
                result.rocks[0].bakeDetail = 0;
                if (const auto* high = node->inputs.size() > 2 ? graph.FindUpstreamNodeForPin(node->inputs[2].id) : nullptr) {
                    const auto detail = evaluate(high->id, depth + 1);
                    if (!detail.error.empty()) return finish(detail);
                    if (detail.hasModels || detail.rocks.empty())
                        return finish(Failure(id, "Material Bake", "Highには生成メッシュを接続してください"));
                    uint64_t hash = 14695981039346656037ull;
                    const auto bytes = [&](const void* data, size_t size) {
                        const auto* p = static_cast<const unsigned char*>(data);
                        for (size_t i = 0; i < size; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
                    };
                    for (const auto& rock : detail.rocks)
                        if (rock.volume) return finish(Failure(id, "Material Bake", "HighにはVolume to Meshの後のMeshを接続してください"));
                    // 上流の設定と接続から決まるキーがあれば、それで足りる（メッシュを丸ごと読むより軽い）。
                    if (const auto key = VolumeKey(graph, high->id, heightKeys)) {
                        bytes(key->data(), key->size());
                    } else {
                        for (const auto& rock : detail.rocks) {
                            bytes(rock.mesh.positions.data(), rock.mesh.positions.size() * sizeof(geometry::Vec3));
                            bytes(rock.mesh.triangles.data(), rock.mesh.triangles.size() * sizeof(rock.mesh.triangles[0]));
                        }
                    }
                    result.rocks[0].bakeDetail = hash | 1;
                }
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
        } else if (node->kind == NodeKind::Remesh) {
            const auto* settings = std::get_if<geometry::RemeshSettings>(&node->settings);
            const auto* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream) return finish(Failure(id, "Remesh", "Mesh出力を接続してください"));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty())
                return finish(Failure(id, "Remesh", "生成メッシュが必要です。Modelは直接変換できません"));
            size_t total = 0;
            for (const auto& rock : result.rocks) {
                if (rock.volume) return finish(Failure(id, "Remesh", "Mesh入力が必要です"));
                total += rock.mesh.triangles.size();
            }
            size_t done = 0;
            for (auto& rock : result.rocks) {
                std::string error;
                const size_t before = rock.mesh.triangles.size();
                auto remeshed = geometry::RemeshMesh(rock.mesh, *settings, error, stop, [&](int percent) {
                    report(id, 0, int((double(done) + double(before) * percent / 100.0) * 100.0 / double(std::max<size_t>(total, 1))));
                });
                if (!error.empty()) return finish(Failure(id, "Remesh", error));
                rock.mesh = std::move(remeshed);
                rock.boxes.reset();
                rock.meshHistory.push_back(rock.source);
                rock.source = id;
                rock.bakeSource = 0;
                done += before;
            }
        } else if (node->kind == NodeKind::Decimate) {
            const auto* settings = std::get_if<geometry::DecimateSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream) return finish(Failure(id, "Decimate", "Mesh出力を接続してください"));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty())
                return finish(Failure(id, "Decimate", "生成メッシュが必要です。Modelは直接変換できません"));
            size_t total = 0;
            for (const auto& rock : result.rocks) {
                if (rock.volume) return finish(Failure(id, "Decimate", "Mesh入力が必要です"));
                total += rock.mesh.triangles.size();
            }
            // 目標は入力全体に対する数。複数のメッシュには、もとの三角形数に応じて割り振る。
            size_t done = 0;
            for (auto& rock : result.rocks) {
                auto share = *settings;
                share.targetTriangles = std::clamp(
                    int(double(settings->targetTriangles) * double(rock.mesh.triangles.size()) / double(std::max<size_t>(total, 1))),
                    geometry::MinDecimateTriangles, geometry::MaxDecimateTriangles);
                std::string error;
                const size_t before = rock.mesh.triangles.size();
                auto reduced = geometry::DecimateMesh(rock.mesh, share, error, stop, [&](int percent) {
                    report(id, 0, int((double(done) + double(before) * percent / 100.0) * 100.0 / double(std::max<size_t>(total, 1))));
                });
                if (!error.empty()) return finish(Failure(id, "Decimate", error));
                rock.mesh = std::move(reduced);
                // 直方体の集まりという由来は、形を変えた時点で失われる（To Volume の解析的な高速経路に渡さない）。
                rock.boxes.reset();
                rock.meshHistory.push_back(rock.source);
                rock.source = id;
                rock.bakeSource = 0;
                done += before;
            }
        } else if (node->kind == NodeKind::Heightmap) {
            const auto* settings = std::get_if<geometry::HeightmapSettings>(&node->settings);
            std::string error;
            if (!settings) return finish(Failure(id, "Heightmap", "設定がありません"));
            if (!geometry::ValidateHeightmapSettings(*settings, error)) return finish(Failure(id, "Heightmap", error));
            geometry::HeightGrid grid;
            if (settings->source == geometry::HeightmapSource::Image) {
                if (settings->image.empty()) return finish(Failure(id, "Heightmap", "ハイトマップの画像を選んでください"));
                if (!LoadHeightImage(FromUtf8(settings->image), grid.width, grid.height, grid.values))
                    return finish(Failure(id, "Heightmap", "ハイトマップの画像を読み込めません: " + settings->image));
            } else {
                grid = geometry::MakeNoiseHeights(*settings, uint32_t(settings->resolution) + 1);
            }
            report(id, 0, 50);
            auto mesh = geometry::MakeTerrainMesh(grid, *settings, error);
            if (!error.empty()) return finish(Failure(id, "Heightmap", error));
            GeneratedRock rock;
            rock.source = id;
            rock.mesh = std::move(mesh);
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::Rock) {
            const auto* settings = std::get_if<RockNodeSettings>(&node->settings);
            if (!settings) return finish(Failure(id, "Rock", "設定がありません"));
            if (settings->scene.empty()) return finish(Failure(id, "Rock", "岩グラフ（.rockgraph）を選んでください"));
            result.rockReferences.push_back({id, settings->scene, settings->scale, settings->weight});
            // 選んで見るときは、原点に 1 つ置く（Rock Scatter はこの配置は使わず、参照だけを読む）。
            RockInstanceSet single;
            single.source = id;
            single.scene = settings->scene;
            single.scale = settings->scale;
            single.instances.push_back({});
            result.rockInstances.push_back(std::move(single));
        } else if (node->kind == NodeKind::RockScatter) {
            const auto* settings = std::get_if<geometry::RockScatterSettings>(&node->settings);
            if (!settings) return finish(Failure(id, "Rock Scatter", "設定がありません"));
            const auto* terrain = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!terrain) return finish(Failure(id, "Rock Scatter", "Terrainに地形（Heightmap など）を接続してください"));
            result = evaluate(terrain->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty())
                return finish(Failure(id, "Rock Scatter", "Terrainには生成メッシュ（地形）を接続してください"));
            // 地形。複数のメッシュは 1 つにまとめて撒く。
            geometry::Mesh surface;
            for (const auto& rock : result.rocks) {
                if (rock.volume) return finish(Failure(id, "Rock Scatter", "TerrainにはMeshを接続してください（Volume to Meshを挟む）"));
                const auto offset = static_cast<uint32_t>(surface.positions.size());
                surface.positions.insert(surface.positions.end(), rock.mesh.positions.begin(), rock.mesh.positions.end());
                for (auto face : rock.mesh.triangles) {
                    for (auto& index : face) index += offset;
                    surface.triangles.push_back(face);
                }
                surface.cornerUvs.insert(surface.cornerUvs.end(), rock.mesh.cornerUvs.begin(), rock.mesh.cornerUvs.end());
                surface.uvWidth = rock.mesh.uvWidth;
                surface.uvHeight = rock.mesh.uvHeight;
            }
            if (surface.cornerUvs.size() != surface.triangles.size()) surface.cornerUvs.clear();
            // マスク（地形の UV の画像）。Shape Mask などの画像のマスクだけを受ける。
            std::shared_ptr<const geometry::MaskImage> mask;
            bool invert = false;
            if (const auto* maskNode = node->inputs.size() > 1 ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr) {
                if (!IsImageMaskNodeKind(maskNode->kind))
                    return finish(Failure(id, "Rock Scatter", "MaskにはShape Maskなど、地形のUVの画像を作るマスクを接続してください"));
                const auto masked = evaluate(maskNode->id, depth + 1);
                if (!masked.error.empty()) return finish(masked);
                if (masked.rocks.size() != 1 || !masked.rocks[0].previewMask ||
                    masked.rocks[0].mesh.triangles.size() != surface.triangles.size())
                    return finish(Failure(id, "Rock Scatter", "Maskは、Terrainと同じ地形から作ったマスクを接続してください"));
                mask = masked.rocks[0].previewMask;
                invert = ImageMaskInvert(*maskNode);
            }
            // つないだ Rock。
            std::vector<RockReference> references;
            for (size_t pin = FixedInputCount(node->kind); pin < node->inputs.size(); ++pin) {
                const auto* upstream = graph.FindUpstreamNodeForPin(node->inputs[pin].id);
                if (!upstream) continue;
                const auto rocks = evaluate(upstream->id, depth + 1);
                if (!rocks.error.empty()) return finish(rocks);
                references.insert(references.end(), rocks.rockReferences.begin(), rocks.rockReferences.end());
            }
            if (references.empty()) return finish(Failure(id, "Rock Scatter", "Rockに岩（Rockノード）を接続してください"));
            std::vector<float> weights;
            for (const auto& reference : references) weights.push_back(reference.weight);
            report(id, 0, -1);
            std::string error;
            const auto instances = geometry::ScatterRocks(surface, mask.get(), invert, weights, *settings, error, stop);
            if (!error.empty()) return finish(Failure(id, "Rock Scatter", error));
            // 地形は、この Rock Scatter を選んで見ているときだけ通す（Mesh Output で地形と一緒に出すと二重になるため）。
            if (id != preview) result.rocks.clear();
            // 岩グラフごとにまとめる。
            for (size_t i = 0; i < references.size(); ++i) {
                RockInstanceSet set;
                set.source = id;
                set.scene = references[i].scene;
                set.scale = references[i].scale;
                for (const auto& instance : instances)
                    if (instance.rock == i) set.instances.push_back(instance);
                if (!set.instances.empty()) result.rockInstances.push_back(std::move(set));
            }
        } else if (node->kind == NodeKind::RockAsset) {
            const auto* settings = std::get_if<RockAssetSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream) return finish(Failure(id, "Rock Asset", "Mesh出力を接続してください"));
            result = evaluate(upstream->id, depth + 1);
            if (!result.error.empty()) return finish(result);
            if (result.hasModels || result.rocks.empty())
                return finish(Failure(id, "Rock Asset", "生成メッシュが必要です。Modelは岩アセットにできません"));
            size_t total = 0;
            for (const auto& rock : result.rocks) {
                if (rock.volume) return finish(Failure(id, "Rock Asset", "Mesh入力が必要です（Volume to Meshを挟んでください）"));
                total += rock.mesh.triangles.size();
            }
            const int lodCount = std::clamp(settings->lodCount, 1, kMaxRockAssetLods);
            const int maxTriangles = std::clamp(settings->maxTriangles, kMinRockAssetTriangles, kMaxRockAssetTriangles);
            // 形のずれの上限は設けず、目標の数まで減らす（段の三角形数を揃えるため）。UV があれば島の境界は Decimate が固定する。
            geometry::DecimateSettings reduce;
            reduce.maxError = 0;
            // 進み具合は「メッシュ × 段」の数で割る。
            const double steps = double(result.rocks.size()) * lodCount;
            double step = 0;
            const auto decimate = [&](const geometry::Mesh& input, int target, std::string& error) {
                reduce.targetTriangles = std::clamp(target, geometry::MinDecimateTriangles, geometry::MaxDecimateTriangles);
                return geometry::DecimateMesh(input, reduce, error, stop, [&](int percent) {
                    report(id, 0, int((step + percent / 100.0) * 100.0 / steps));
                });
            };
            for (auto& rock : result.rocks) {
                std::vector<geometry::Mesh> lods;
                lods.reserve(lodCount);
                // LOD0。上限は入力全体に対する数なので、複数のメッシュには三角形数に応じて割り振る（Decimate と同じ）。
                const int lod0Target = std::max(kMinRockAssetTriangles,
                    int(double(maxTriangles) * double(rock.mesh.triangles.size()) / double(std::max<size_t>(total, 1))));
                std::string error;
                if (rock.mesh.triangles.size() > size_t(lod0Target)) lods.push_back(decimate(rock.mesh, lod0Target, error));
                else lods.push_back(rock.mesh);
                if (!error.empty()) return finish(Failure(id, "Rock Asset", "LOD0: " + error));
                ++step;
                // LOD1 以降は 1 つ前の段から減らす（速く、段どうしの形も揃う）。
                // LOD0 が UV を持たなければ、どの段も UV を持たないので全段を「共有」として扱う。
                const bool hasUvs = !lods[0].cornerUvs.empty();
                const int shared = hasUvs ? std::min(RockAssetSharedUvLods(*settings), lodCount) : lodCount;
                for (int level = 1; level < lodCount; ++level, ++step) {
                    const float percent = std::clamp(settings->trianglePercent[level], 0.1f, 100.0f);
                    const int target = std::max(kMinRockAssetTriangles, int(double(lods[0].triangles.size()) * percent / 100.0));
                    const std::string label = "LOD" + std::to_string(level) + ": ";
                    if (level < shared) {
                        // LOD0 の UV を共有する段。UV の島の境界を Decimate が固定する。
                        const geometry::Mesh& previous = lods.back();
                        if (previous.triangles.size() <= size_t(target)) {
                            lods.push_back(previous);
                            continue;
                        }
                        auto reduced = decimate(previous, target, error);
                        if (!error.empty()) return finish(Failure(id, "Rock Asset", label + error));
                        lods.push_back(std::move(reduced));
                        continue;
                    }
                    // 自分の UV を持つ段。UV を捨てて減らし（継ぎ目に縛られない）、この段だけで展開し直す。
                    // アトラスは LOD0 を段ごとに半分にした大きさ（最小 128）。割れの多い岩は数百の島になり、
                    // 小さいアトラスでは島ごとの余白で埋まる。収まらなければ余白を 4→2→1 px と詰め、
                    // それでも収まらなければ解像度を倍にする（LOD0 のアトラスまで）。
                    geometry::Mesh plain = lods.back();
                    plain.cornerUvs.clear();
                    plain.uvCharts.clear();
                    plain.uvWidth = plain.uvHeight = 0;
                    if (plain.triangles.size() > size_t(target)) {
                        plain = decimate(plain, target, error);
                        if (!error.empty()) return finish(Failure(id, "Rock Asset", label + error));
                    }
                    geometry::UvUnwrapSettings unwrap;
                    const int atlas = int(std::max(lods[0].uvWidth, lods[0].uvHeight));
                    const int largest = geometry::NormalizeUvResolution(std::max(kMinRockAssetUvResolution, atlas));
                    geometry::Mesh unwrapped;
                    for (unwrap.resolution = geometry::NormalizeUvResolution(std::max(kMinRockAssetUvResolution, atlas >> level));;
                         unwrap.resolution *= 2) {
                        for (const int padding : {4, 2, 1}) {
                            unwrap.padding = padding;
                            unwrapped = geometry::UnwrapMesh(plain, unwrap, error, stop);
                            if (error.empty() || stop.stop_requested()) break;
                        }
                        if (error.empty() || stop.stop_requested() || unwrap.resolution >= largest) break;
                    }
                    if (!error.empty()) return finish(Failure(id, "Rock Asset", label + error));
                    lods.push_back(std::move(unwrapped));
                }
                rock.sharedUvLods = shared;
                // LOD0 の UV を共有する段は、上流の Material Bake の結果をそのまま使える。
                // 形が変わるので、焼いた結果がまだ使えるかは減らす前のメッシュで照らす。
                if (rock.bakeSource && !rock.bakeMesh) rock.bakeMesh = std::make_shared<const geometry::Mesh>(rock.mesh);
                rock.mesh = lods[0];
                rock.lods = std::make_shared<const std::vector<geometry::Mesh>>(std::move(lods));
                rock.boxes.reset();
                rock.meshHistory.push_back(rock.source);
                rock.source = id;
            }
        } else if (node->kind == NodeKind::VolumeSmooth || node->kind == NodeKind::VolumeTerrace || node->kind == NodeKind::VolumeClose ||
                   node->kind == NodeKind::VolumeEdgeWear || node->kind == NodeKind::VolumeClip) {
            const char* name = node->kind == NodeKind::VolumeSmooth ? "Volume Smooth"
                             : node->kind == NodeKind::VolumeTerrace ? "Volume Terrace"
                             : node->kind == NodeKind::VolumeEdgeWear ? "Volume Edge Wear"
                             : node->kind == NodeKind::VolumeClip ? "Volume Clip" : "Volume Close";
            const auto* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!upstream) return finish(Failure(id, name, "Volume 出力を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            if (input.rocks.size() != 1 || !input.rocks[0].volume) return finish(Failure(id, name, "ボリュームが必要です"));
            std::string error;
            geometry::VolumeGrid processed;
            if (const auto* settings = std::get_if<geometry::VolumeSmoothSettings>(&node->settings))
                processed = geometry::SmoothVolume(*input.rocks[0].volume, *settings, error);
            else if (const auto* terrace = std::get_if<geometry::VolumeTerraceSettings>(&node->settings))
                processed = geometry::TerraceVolume(*input.rocks[0].volume, *terrace, error);
            else if (const auto* close = std::get_if<geometry::VolumeCloseSettings>(&node->settings))
                processed = geometry::CloseVolume(*input.rocks[0].volume, *close, error);
            else if (const auto* wear = std::get_if<geometry::VolumeEdgeWearSettings>(&node->settings))
                processed = geometry::EdgeWearVolume(*input.rocks[0].volume, *wear, error);
            else if (const auto* clip = std::get_if<geometry::VolumeClipSettings>(&node->settings))
                processed = geometry::ClipVolume(*input.rocks[0].volume, *clip, error);
            else error = "設定がありません";
            if (!error.empty()) return finish(Failure(id, name, error));
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(processed));
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::VolumeNoise) {
            const auto* settings = std::get_if<geometry::VolumeNoiseSettings>(&node->settings);
            const auto* upstream =
                node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs[0].id);
            if (!settings || !upstream)
                return finish(Failure(id, "Volume Noise", "Volume 出力を接続してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            if (input.rocks.size() != 1 || !input.rocks[0].volume)
                return finish(Failure(id, "Volume Noise", "ボリュームが必要です"));
            std::string error;
            auto noisy = geometry::NoiseVolume(*input.rocks[0].volume, *settings, error);
            if (!error.empty()) return finish(Failure(id, "Volume Noise", error));
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(noisy));
            result.rocks.push_back(std::move(rock));
        } else if (node->kind == NodeKind::VolumeCrack) {
            const auto* settings = std::get_if<geometry::VolumeCrackSettings>(&node->settings);
            const bool wired = node->inputs.size() >= 2;
            const auto* upstream = wired ? graph.FindUpstreamNodeForPin(node->inputs[0].id) : nullptr;
            const auto* scatter = wired ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr;
            const auto* planes = node->inputs.size() > 2 ? graph.FindUpstreamNodeForPin(node->inputs[2].id) : nullptr;
            if (!settings || !upstream || (!scatter && !planes))
                return finish(Failure(id, "Volume Crack", "Volume と、Points または Planes の一方を接続してください"));
            if (scatter && planes)
                return finish(Failure(id, "Volume Crack", "Points と Planes は同時に接続できません。一方を外してください"));
            const auto input = evaluate(upstream->id, depth + 1);
            if (!input.error.empty()) return finish(input);
            const auto scattered = evaluate(planes ? planes->id : scatter->id, depth + 1);
            if (!scattered.error.empty()) return finish(scattered);
            if (input.rocks.size() != 1 || !input.rocks[0].volume)
                return finish(Failure(id, "Volume Crack", "ボリュームが必要です"));
            if (planes ? !scattered.planes : !scattered.points)
                return finish(Failure(id, "Volume Crack", "割れ目の源の出力が不正です"));
            std::string error;
            auto cracked = planes
                ? geometry::CrackVolumeWithPlanes(*input.rocks[0].volume, *scattered.planes, *settings, error)
                : geometry::CrackVolume(*input.rocks[0].volume, scattered.points->positions, *settings, error);
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
            geometry::PlaneCutsGuide guide;
            auto cut = geometry::CutVolume(*input.rocks[0].volume, *settings, error, &guide.planes);
            if (!error.empty()) return finish(Failure(id, "Plane Cuts", error));
            guide.frames = geometry::CutFaceFrames(cut, guide.planes);
            guide.spacing = cut.spacing;
            GeneratedRock rock;
            rock.source = id;
            rock.volume = std::make_shared<const geometry::VolumeGrid>(std::move(cut));
            rock.planeCuts = std::make_shared<const geometry::PlaneCutsGuide>(std::move(guide));
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
                    for (auto& rock : result.rocks) { rock.materialSource = surface->id; rock.materials.clear(); rock.maskImages.clear(); rock.bakeSource = 0; }
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
