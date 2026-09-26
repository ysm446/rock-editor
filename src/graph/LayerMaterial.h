#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace rock::graph {
// 3/4は移植元の定数/ノイズと一致させる。道路用の形は受け入れない。
enum class LayerMaskShape : uint32_t { Constant = 3, Noise = 4 };
struct LayerMaskSettings {
    LayerMaskShape shape = LayerMaskShape::Noise;
    float noiseScaleMeters = 1, threshold = .5f, softness = .2f;
    uint32_t seed = 1;
    float breakupAmount = 0, breakupScaleMeters = 1, strength = 1;
    bool invert = false;
};

// レイヤーマテリアル内部の PBR 素材と合成設定。
struct PresetMaterial {
    uint32_t material = 0; // プロジェクトに埋め込まれるPBR素材への参照。0は定数材質。
    float uvRepeatMeters = 2;
    bool worldUv = false;
    std::array<float, 3> baseColor{0.42f, 0.4f, 0.36f};
    float roughness = 0.8f;
    float metallic = 0, ambientOcclusion = 1;
    // 下地には不要。上層で未指定なら被覆0（旧版の未結線スロットも同じ）。
    std::optional<LayerMaskSettings> mask;
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
struct LayerMaterial {
    uint32_t id = 0;
    std::string name;
    float displacementMeters = 0;
    float layerBlendRange = 0.2f;
    std::vector<PresetMaterial> materials;
    std::optional<PresetGraph> materialGraph;
    // 共有アセット（.tglayer）のファイルと固定 ID。未保存・複製直後は空。アンドゥの写しにも入る。
    std::filesystem::path assetPath;
    std::string assetUid;
};

}
