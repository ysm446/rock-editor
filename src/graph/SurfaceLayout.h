#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include "graph/NodeGraph.h"
#include "compositor/BoundaryMaterial.h"

namespace tg::graph {

// 文書内の安定ID。グラフのノードID・マテリアルIDとは別の名前空間。
using SurfaceId = uint32_t;
enum class SurfaceRole : uint32_t { Road, Ground, Sidewalk };
enum class BoundaryMode : uint32_t { Blend, KeepStep, Fixed };
enum class SurfaceSide : uint32_t { Road, Left, Right };

struct BoundaryContract {
    BoundaryMode mode = BoundaryMode::Blend;
    float transitionMeters = 0.5f;
    float maxHeightAdjustment = 0.15f;
    bool preserveOutline = false;
};
struct CrossSectionPoint {
    SurfaceId id = 0;
    float across = 0, height = 0;
};
struct PresetParameter {
    SurfaceId id = 0;
    std::string name;
    float minimum = 0, maximum = 1, defaultValue = 0;
};
struct PresetMaterial {
    uint32_t material = 0; // プロジェクトに埋め込まれるPBR素材への参照。0は定数材質。
    float uvRepeatMeters = 2;
    bool worldUv = false;
    std::array<float, 3> baseColor{0.42f, 0.4f, 0.36f};
    float roughness = 0.8f;
    float metallic = 0, ambientOcclusion = 1;
    // 下地には不要。上層で未指定なら被覆0（旧版の未結線スロットも同じ）。
    std::optional<RoadMaskNodeSettings> mask;
    uint32_t blendMode = 0, heightGate = 0;
    float heightGateThreshold = 0.5f, heightGateSoftness = 0.2f;
    bool enabled = true;
};
enum class PresetNodeKind : uint32_t { Material, Mask, Blend, Output };
struct PresetNode {
    uint32_t id = 0;
    PresetNodeKind kind = PresetNodeKind::Material;
    // 合成: 下地・上層素材・マスク。出力: 入力0。0は未接続。
    std::array<uint32_t, 3> inputs{};
    std::array<float, 2> position{};
    PresetMaterial settings;
};
struct PresetGraph {
    uint32_t nextId = 1;
    std::vector<PresetNode> nodes;
};
struct SurfacePreset {
    SurfaceId id = 0;
    uint32_t version = 1;
    std::string name;
    SurfaceRole role = SurfaceRole::Ground;
    float displacementMeters = 0;
    std::vector<CrossSectionPoint> section;
    // 内端・外端・始端・終端。接続相手別の定義は持たない。
    std::array<BoundaryContract, 4> boundaries;
    std::vector<PresetMaterial> materials;
    std::vector<PresetParameter> parameters;
    float layerBlendRange = 0.2f;
    std::optional<PresetGraph> materialGraph;
    // 0は旧形式の埋込材質。保存・アプリ編集時に独立アセットへ移行する。
    SurfaceId layerMaterial = 0;
};
struct LayerMaterial {
    SurfaceId id = 0;
    std::string name;
    float displacementMeters = 0;
    float layerBlendRange = 0.2f;
    std::vector<PresetMaterial> materials;
    std::optional<PresetGraph> materialGraph;
    // 共有アセット（.tglayer）のファイルと固定 ID。未保存・複製直後は空。アンドゥの写しにも入る。
    std::filesystem::path assetPath;
    std::string assetUid;
};
struct SpanParameter {
    SurfaceId parameter = 0;
    float startValue = 0, endValue = 0;
};
struct SurfaceSpan {
    SurfaceId id = 0, preset = 0;
    float startMeters = 0, endMeters = 1;
    float blendInMeters = 0, blendOutMeters = 0;
    uint32_t seed = 1;
    std::vector<SpanParameter> parameters;
    SurfaceId boundaryMaterial = 0;
};
struct SurfaceBand {
    SurfaceId id = 0;
    SurfaceSide side = SurfaceSide::Road;
    // 同じ側はRoadに近い帯から順に格納。区間の切れ目は帯ごとに独立。
    std::vector<SurfaceSpan> spans;
};
struct RoadLayout {
    SurfaceId id = 0;
    int32_t roadNode = 0;
    std::vector<SurfaceBand> bands;
};
struct SurfaceLayoutDocument {
    SurfaceId nextId = 1;
    std::vector<SurfacePreset> presets;
    std::vector<RoadLayout> layouts;
    std::vector<LayerMaterial> layerMaterials;
    std::vector<compositor::BoundaryMaterial> boundaryMaterials;
    SurfaceId AllocateId();
};
// 旧埋込材質は移行時に空にする。評価用の写しだけに参照先の材質を展開する。
bool ExtractLayerMaterials(SurfaceLayoutDocument& document, std::string& error);
SurfaceLayoutDocument ResolveLayerMaterials(const SurfaceLayoutDocument& document);
SurfacePreset MaterialPreviewPreset(const LayerMaterial& material);
SurfaceId PresetLayerMaterial(const SurfaceLayoutDocument& document, SurfaceId preset);
// 断面・公開値のIDと区間の参照を一緒に複製する。ID不足では変更しない。
bool CloneSurfacePresetForSpan(SurfaceLayoutDocument& document, SurfaceSpan& span);
bool AssignLayerMaterial(SurfaceLayoutDocument& document, SurfaceSpan& span, SurfaceId material);

// 参照・区間・寸法を検査する。シーングラフやGPUには依存しない。
bool ValidateSurfaceLayouts(const SurfaceLayoutDocument& document, std::string& error);
class NodeGraph;
bool ValidateSurfaceLayoutRoads(const SurfaceLayoutDocument& document, const NodeGraph& graph, std::string& error);

}  // namespace tg::graph
