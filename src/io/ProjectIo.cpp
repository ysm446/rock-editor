#include "io/LayerMaterialIo.h"
#include "graph/SurfacePresetGraph.h"
#include "io/PieceSettings.h"
#include "io/ProjectIo.h"
#include "io/GraphIo.h"
#include "io/JsonUtil.h"

#include "core/PathUtf8.h"

#include "core/ImageIo.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rock::io {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace detail;

constexpr const char* kProjectFormat = "rock-editor.project";
constexpr const char* kMaterialFormat = "rock-editor.material";
// 形式を変えたら上げる。読み込み側は「これ以下なら読める」として扱う。
//
// 1: rock-editor としての最初の形式。road-editor 時代の terrain-graph.* とは
//    互換を持たない（形式の識別子が違うので、そもそも読み込みで弾かれる）。
constexpr int kProjectFormatVersion = 1;
// マテリアル単体 (.rockmat) の版。
constexpr int kMaterialFormatVersion = 1;


// --- マテリアル -----------------------------------------------------------
//
// プロジェクトへの埋め込みと .rockmat で同じ形を使う。違うのはテクスチャ参照の書き方だけ。

// compositor::MaterialMap の並び。maps のキーと同じ名前。
const char* const kMaterialMapNames[] = {"baseColor", "normal", "roughness", "metallic",
                                         "ambientOcclusion", "height", "opacity"};
static_assert(std::size(kMaterialMapNames) == static_cast<size_t>(compositor::MaterialMap::Count));

void RemapLayerReferences(compositor::MaterialAsset& asset, const std::unordered_map<int, compositor::MaterialAssetId>& ids) {
    if (!asset.layerMaterial) return;
    auto body = WriteLayerMaterial(*asset.layerMaterial);
    MapLayerMaterials(body, [&](const json& value) -> json {
        const auto it = ids.find(value.is_number_integer() ? value.get<int>() : 0);
        return it == ids.end() ? json(0) : json(it->second);
    });
    std::string error;
    ReadLayerMaterial(body, *asset.layerMaterial, error);
}

json WriteMaterialBody(const compositor::MaterialAsset& asset, const TextureWriter& writeTexture) {
    if (asset.layerMaterial) {
        auto node = WriteLayerMaterial(*asset.layerMaterial);
        node["name"] = asset.name;
        return node;
    }
    json node;
    node["name"] = asset.name;
    node["baseColorTint"] = WriteFloat3(asset.baseColorTint);
    node["hueShift"] = asset.hueShiftDegrees;
    node["saturation"] = asset.saturation;
    node["brightness"] = asset.brightness;
    node["roughness"] = asset.roughnessValue;
    node["metallic"] = asset.metallicValue;
    node["ambientOcclusion"] = asset.ambientOcclusionValue;
    // 不透明度と合成モード。材質の属性。
    static const char* const kBlendModeNames[] = {"opaque", "masked", "translucent"};
    node["opacity"] = asset.opacityValue;
    node["blendMode"] = EnumName(kBlendModeNames, static_cast<uint32_t>(asset.blendMode));
    node["maskThreshold"] = asset.maskThreshold;

    json maps;
    maps["baseColor"] = writeTexture(asset.baseColor);
    maps["normal"] = writeTexture(asset.normal);
    // 法線マップの規約（緑の向き）。既定は OpenGL（反転して読む）。
    node["flipNormalGreen"] = asset.flipNormalGreen;
    maps["roughness"] = WriteMapSlot(asset.roughness, writeTexture);
    maps["metallic"] = WriteMapSlot(asset.metallic, writeTexture);
    maps["ambientOcclusion"] = WriteMapSlot(asset.ambientOcclusion, writeTexture);
    maps["height"] = WriteMapSlot(asset.height, writeTexture);
    maps["opacity"] = WriteMapSlot(asset.opacity, writeTexture);
    node["maps"] = std::move(maps);
    // マップごとの UV（1 か 2）。maps の中はテクスチャの参照だけにしておく（ProjectWorkspace が書き換えるため）。
    json uvSets;
    for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::MaterialMap::Count); ++i) {
        uvSets[kMaterialMapNames[i]] = compositor::UsesSecondUv(asset, static_cast<compositor::MaterialMap>(i)) ? 2 : 1;
    }
    node["mapUvSets"] = std::move(uvSets);
    return node;
}

void ReadMaterialBody(const json& node, compositor::MaterialAsset& asset,
                      const TextureReader& readTexture) {
    if (node.contains("materials") || node.contains("materialGraph")) {
        graph::LayerMaterial layer;
        std::string error;
        if (ReadLayerMaterial(node, layer, error)) {
            if (layer.materialGraph) {
                graph::ExtractPresetLayers(layer, layer.materials, error);
                layer.materialGraph.reset();
            }
            asset.layerMaterial = std::move(layer); asset.name = asset.layerMaterial->name;
        }
        else { asset.layerError = error; ROCK_LOG_ERROR("レイヤーマテリアル: %s", error.c_str()); }
        return;
    }
    const compositor::MaterialAsset defaults;
    asset.name = ReadString(node, "name", defaults.name);
    asset.baseColorTint = ReadFloat3(node, "baseColorTint", defaults.baseColorTint);
    asset.hueShiftDegrees = ReadFloat(node, "hueShift", defaults.hueShiftDegrees);
    asset.saturation = ReadFloat(node, "saturation", defaults.saturation);
    asset.brightness = std::clamp(ReadFloat(node, "brightness", defaults.brightness), 0.0f, 8.0f);
    asset.roughnessValue = ReadFloat(node, "roughness", defaults.roughnessValue);
    asset.metallicValue = ReadFloat(node, "metallic", defaults.metallicValue);
    asset.ambientOcclusionValue =
        ReadFloat(node, "ambientOcclusion", defaults.ambientOcclusionValue);
    {
        static const char* const kBlendModeNames[] = {"opaque", "masked", "translucent"};
        asset.opacityValue = std::clamp(ReadFloat(node, "opacity", defaults.opacityValue), 0.0f, 1.0f);
        asset.blendMode = static_cast<compositor::BlendMode>(
            EnumValue(kBlendModeNames, node, "blendMode", static_cast<uint32_t>(defaults.blendMode)));
        asset.maskThreshold = std::clamp(ReadFloat(node, "maskThreshold", defaults.maskThreshold), 0.0f, 1.0f);
    }

    const json* maps = FindMember(node, "maps");
    if (maps == nullptr || !maps->is_object()) {
        return;
    }
    const json* baseColor = FindMember(*maps, "baseColor");
    asset.baseColor = (baseColor != nullptr) ? readTexture(*baseColor) : compositor::kNoTexture;
    asset.flipNormalGreen = ReadBool(node, "flipNormalGreen", defaults.flipNormalGreen);
    const json* normal = FindMember(*maps, "normal");
    asset.normal = (normal != nullptr) ? readTexture(*normal) : compositor::kNoTexture;
    asset.roughness = ReadMapSlot(*maps, "roughness", readTexture);
    asset.metallic = ReadMapSlot(*maps, "metallic", readTexture);
    asset.ambientOcclusion = ReadMapSlot(*maps, "ambientOcclusion", readTexture);
    asset.height = ReadMapSlot(*maps, "height", readTexture);
    asset.opacity = ReadMapSlot(*maps, "opacity", readTexture);
    // 無ければ（版の古いファイル）すべて 1 つ目の UV。
    asset.mapUvSets = defaults.mapUvSets;
    if (const json* uvSets = FindMember(node, "mapUvSets"); uvSets != nullptr && uvSets->is_object()) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::MaterialMap::Count); ++i) {
            if (ReadInt(*uvSets, kMaterialMapNames[i], 1) == 2)
                asset.mapUvSets |= compositor::MaterialMapBit(static_cast<compositor::MaterialMap>(i));
        }
    }
}

// --- プレビューの設定 -----------------------------------------------------

// 天球アセット（M5b-2）で HDRI のパスが preview から抜けたため、パスの解決は不要になった。
json WritePreview(renderer::PreviewRenderer& renderer) {
    json node;
    node["tonemap"] = EnumName(kTonemapNames, static_cast<uint32_t>(renderer.Tonemap()));
    node["tessellation"] = renderer.TessellationEnabled();
    node["tessellationFactor"] = renderer.TessellationFactor();
    node["tessellationTargetPixels"] = renderer.TessellationTargetPixels();
    node["materialResolution"] = renderer.MaterialResolution();
    node["showSkybox"] = renderer.ShowSkybox();
    node["skyboxBlur"] = renderer.SkyboxBlur();
    node["screenSpaceAo"] = {{"enabled", renderer.Ssao().enabled}, {"radius", renderer.Ssao().radius}, {"strength", renderer.Ssao().strength}};
    node["shadow"] = renderer.ShadowEnabled();
    node["shadowResolution"] = renderer.ShadowResolution();
    node["shadowCascadeCount"] = renderer.ShadowCascadeCount();

    // 被写界深度。見え方だけの設定だが、プロジェクトごとに変えるものなので残す。
    const renderer::DofSettings& dof = renderer.Dof();
    json dofNode;
    dofNode["enabled"] = dof.enabled;
    dofNode["focusOnTarget"] = dof.focusOnTarget;
    dofNode["focusDistance"] = dof.focusDistance;
    dofNode["blurScale"] = dof.blurScale;
    dofNode["miniatureScale"] = dof.miniatureScale;
    dofNode["maxBlurPixels"] = dof.maxBlurPixels;
    dofNode["shape"] = EnumName(kApertureShapeNames, static_cast<uint32_t>(dof.shape));
    dofNode["rotationDegrees"] = dof.rotationDegrees;
    node["depthOfField"] = std::move(dofNode);
    // 環境そのもの（HDRI・較正値・空のパラメータ）は天球アセットが持つ。
    // ここには「見え方」だけを書く。

    const renderer::CameraState camera = renderer.GetCamera().State();
    json cameraNode;
    cameraNode["target"] = WriteFloat3(camera.target);
    cameraNode["distance"] = camera.distance;
    cameraNode["yaw"] = camera.yaw;
    cameraNode["pitch"] = camera.pitch;
    cameraNode["fovY"] = camera.fovY;
    node["camera"] = std::move(cameraNode);

    // light は作業用のライト（作業用IBL）。シーンの空の太陽と大気は別に持つ。
    const renderer::LightSettings& light = renderer.WorkLight();
    json lightNode;
    lightNode["azimuth"] = light.azimuth;
    lightNode["elevation"] = light.elevation;
    lightNode["illuminance"] = light.illuminance;
    lightNode["color"] = WriteFloat3(light.color);
    node["light"] = std::move(lightNode);

    // シーンの空（大気散乱）。lightingMode は表示環境（ibl = 作業用IBL、atmospheric = シーンの空）。
    node["lightingMode"] = renderer.AtmosphericMode() ? "atmospheric" : "ibl";
    {
        const renderer::LightSettings& sun = renderer.SceneSunLight();
        const renderer::AtmosphereSettings& sky = renderer.AtmosphericSettings();
        node["atmosphere"] = {{"azimuth", sun.azimuth},
                              {"elevation", sun.elevation},
                              {"illuminance", sun.illuminance},
                              {"density", sky.density},
                              {"mie", sky.mie},
                              {"eccentricity", sky.eccentricity},
                              {"altitude", sky.altitude},
                              {"groundAlbedo", sky.groundAlbedo},
                              {"lowerHemisphere", sky.lowerHemisphere != 0 ? "ground" : "sky"},
                              {"skylightIntensity", renderer.SkylightIntensity()}};
    }

    const renderer::ExposureSettings& exposure = renderer.Exposure();
    json exposureNode;
    exposureNode["automatic"] = exposure.automatic;
    exposureNode["compensation"] = exposure.compensation;
    exposureNode["minEv100"] = exposure.minEv100;
    exposureNode["maxEv100"] = exposure.maxEv100;
    exposureNode["adaptationSpeed"] = exposure.adaptationSpeed;
    exposureNode["useManualEv"] = exposure.useManualEv;
    exposureNode["manualEv100"] = exposure.manualEv100;
    exposureNode["aperture"] = exposure.aperture;
    exposureNode["shutterSpeed"] = exposure.shutterSpeed;
    exposureNode["iso"] = exposure.iso;
    node["exposure"] = std::move(exposureNode);
    return node;
}

void ReadPreview(const json& node, renderer::PreviewRenderer& renderer) {
    // 既定値は renderer::kPreviewDefaults の一択。数値を直接書かない。
    // 名前は各節ローカルの defaults（LightSettings など）と衝突させない。
    const renderer::PreviewDefaults& previewDefaults = renderer::kPreviewDefaults;
    renderer.Tonemap() = static_cast<renderer::TonemapMode>(
        EnumValue(kTonemapNames, node, "tonemap", static_cast<uint32_t>(previewDefaults.tonemap)));
    // 旧ファイルの平面プレビューの設定（useMaterialTextures / displacementScale / planeSize /
    // meshSubdivisions / flatMaterial）は読まない（平面のプレビューは無くなった）。
    renderer.TessellationEnabled() =
        ReadBool(node, "tessellation", previewDefaults.tessellationEnabled);
    renderer.TessellationFactor() =
        std::clamp(ReadFloat(node, "tessellationFactor", previewDefaults.tessellationFactor), 1.0f, 64.0f);
    renderer.TessellationTargetPixels() = std::clamp(
        ReadFloat(node, "tessellationTargetPixels", previewDefaults.tessellationTargetPixels), 2.0f, 64.0f);
    renderer.RequestMaterialResolution(
        ReadUInt(node, "materialResolution", previewDefaults.materialResolution));
    renderer.ShowSkybox() = ReadBool(node, "showSkybox", previewDefaults.showSkybox);
    renderer.SkyboxBlur() = ReadBool(node, "skyboxBlur", previewDefaults.skyboxBlur);
    renderer.ShadowEnabled() = ReadBool(node, "shadow", previewDefaults.shadowEnabled);
    renderer.RequestShadowResolution(ReadUInt(node, "shadowResolution", previewDefaults.shadowResolution));
    renderer.RequestShadowCascadeCount(ReadUInt(node, "shadowCascadeCount", previewDefaults.shadowCascadeCount));

    // 節が丸ごと欠けていても既定値で埋める。file-format.md の「欠けているキーは
    // 既定値で埋める」に合わせる（節ごと飛ばすと前のプロジェクトの値が残る）。
    const json emptySection = json::object();
    const auto section = [&node, &emptySection](const char* key) -> const json& {
        const json* member = FindMember(node, key);
        return (member != nullptr && member->is_object()) ? *member : emptySection;
    };

    {
        const auto& ao = section("screenSpaceAo");
        const renderer::SsaoSettings defaults;
        renderer.Ssao().enabled = ReadBool(ao, "enabled", defaults.enabled);
        renderer.Ssao().radius = std::clamp(ReadFloat(ao, "radius", defaults.radius), 0.001f, 10.0f);
        renderer.Ssao().strength = std::clamp(ReadFloat(ao, "strength", defaults.strength), 0.0f, 3.0f);
    }
    {
        const json& camera = section("camera");
        renderer::CameraState state;
        state.target = ReadFloat3(camera, "target", state.target);
        state.distance = ReadFloat(camera, "distance", state.distance);
        state.yaw = ReadFloat(camera, "yaw", state.yaw);
        state.pitch = ReadFloat(camera, "pitch", state.pitch);
        state.fovY = ReadFloat(camera, "fovY", state.fovY);
        renderer.GetCamera().SetState(state);
    }

    {
        const json& light = section("light");
        renderer::LightSettings& target = renderer.WorkLight();
        const renderer::LightSettings defaults;
        target.azimuth = ReadFloat(light, "azimuth", defaults.azimuth);
        target.elevation = ReadFloat(light, "elevation", defaults.elevation);
        target.illuminance = ReadFloat(light, "illuminance", defaults.illuminance);
        target.color = ReadFloat3(light, "color", defaults.color);
    }

    {
        // シーンの空。無ければ（古いファイル）既定値で、表示環境は作業用IBL。
        renderer.AtmosphericMode() = ReadString(node, "lightingMode", "ibl") == "atmospheric";
        const json& atmosphere = section("atmosphere");
        const renderer::AtmosphereSettings defaults;
        renderer::LightSettings& sun = renderer.SceneSunLight();
        sun.azimuth = ReadFloat(atmosphere, "azimuth", defaults.azimuth);
        sun.elevation = std::clamp(ReadFloat(atmosphere, "elevation", defaults.elevation), -1.5f, 1.5f);
        sun.illuminance = std::max(ReadFloat(atmosphere, "illuminance", defaults.illuminance), 0.0f);
        sun.color = {1.0f, 1.0f, 1.0f};
        renderer::AtmosphereSettings& sky = renderer.AtmosphericSettings();
        sky = defaults;
        sky.density = std::clamp(ReadFloat(atmosphere, "density", defaults.density), 0.1f, 3.0f);
        sky.mie = std::clamp(ReadFloat(atmosphere, "mie", defaults.mie), 0.0f, 2.0f);
        sky.eccentricity = std::clamp(ReadFloat(atmosphere, "eccentricity", defaults.eccentricity), 0.0f, 0.95f);
        sky.altitude = std::clamp(ReadFloat(atmosphere, "altitude", defaults.altitude), 0.0f, 10000.0f);
        sky.groundAlbedo = std::clamp(ReadFloat(atmosphere, "groundAlbedo", defaults.groundAlbedo), 0.0f, 1.0f);
        sky.lowerHemisphere =
            ReadString(atmosphere, "lowerHemisphere", defaults.lowerHemisphere != 0 ? "ground" : "sky") == "sky" ? 0u : 1u;
        renderer.SkylightIntensity() = std::clamp(
            ReadFloat(atmosphere, "skylightIntensity", renderer::PreviewRenderer::kDefaultSkylightIntensity), 0.0f, 8.0f);
    }

    {
        const json& exposure = section("exposure");
        renderer::ExposureSettings& target = renderer.Exposure();
        const renderer::ExposureSettings defaults;
        target.automatic = ReadBool(exposure, "automatic", defaults.automatic);
        target.compensation = std::clamp(ReadFloat(exposure, "compensation", defaults.compensation), -5.0f, 5.0f);
        target.minEv100 = std::clamp(ReadFloat(exposure, "minEv100", defaults.minEv100), -10.0f, 20.0f);
        target.maxEv100 = std::clamp(ReadFloat(exposure, "maxEv100", defaults.maxEv100), -10.0f, 20.0f);
        target.adaptationSpeed = std::clamp(ReadFloat(exposure, "adaptationSpeed", defaults.adaptationSpeed), 0.1f, 20.0f);
        target.useManualEv = ReadBool(exposure, "useManualEv", defaults.useManualEv);
        target.manualEv100 = ReadFloat(exposure, "manualEv100", defaults.manualEv100);
        target.aperture = ReadFloat(exposure, "aperture", defaults.aperture);
        target.shutterSpeed = ReadFloat(exposure, "shutterSpeed", defaults.shutterSpeed);
        target.iso = ReadFloat(exposure, "iso", defaults.iso);
    }

    {
        const json& dofNode = section("depthOfField");
        renderer::DofSettings& target = renderer.Dof();
        const renderer::DofSettings defaults;
        target.enabled = ReadBool(dofNode, "enabled", defaults.enabled);
        target.focusOnTarget = ReadBool(dofNode, "focusOnTarget", defaults.focusOnTarget);
        target.focusDistance = ReadFloat(dofNode, "focusDistance", defaults.focusDistance);
        target.blurScale = ReadFloat(dofNode, "blurScale", defaults.blurScale);
        target.miniatureScale =
            ReadFloat(dofNode, "miniatureScale", defaults.miniatureScale);
        target.maxBlurPixels = ReadFloat(dofNode, "maxBlurPixels", defaults.maxBlurPixels);
        target.shape = static_cast<renderer::ApertureShape>(EnumValue(
            kApertureShapeNames, dofNode, "shape", static_cast<uint32_t>(defaults.shape)));
        target.rotationDegrees =
            ReadFloat(dofNode, "rotationDegrees", defaults.rotationDegrees);
    }
}

// --- 天球 -----------------------------------------------------------------

json WriteSky(const renderer::SkyAsset& asset, const fs::path& baseDir) {
    json node;
    node["name"] = asset.name;
    node["source"] = EnumName(kSkySourceNames, static_cast<uint32_t>(asset.sky.source));
    // 画像はテクスチャと同じく相対パスの参照で持つ。使っていなければ null。
    node["hdri"] = asset.sky.hdriPath.empty()
                       ? json()
                       : json(RelativePathString(asset.sky.hdriPath, baseDir));
    node["skyLuminance"] = asset.sky.skyLuminance;
    node["iblIntensity"] = asset.sky.iblIntensity;

    const renderer::SkySettings& procedural = asset.sky.procedural;
    json proceduralNode;
    proceduralNode["zenithColor"] = WriteFloat3(procedural.zenithColor);
    proceduralNode["horizonColor"] = WriteFloat3(procedural.horizonColor);
    proceduralNode["groundColor"] = WriteFloat3(procedural.groundColor);
    proceduralNode["intensity"] = procedural.intensity;
    node["procedural"] = std::move(proceduralNode);
    return node;
}

// 天球 1 つを読み込んでライブラリへ足す。
renderer::SkyAssetId ReadSky(const json& node, renderer::SkyLibrary& skies,
                             const fs::path& baseDir) {
    const renderer::SkyDefinition defaults;
    std::string name = ReadString(node, "name");
    if (name.empty()) {
        name = "天球";
    }
    const renderer::SkyAssetId id = skies.Add(name);
    renderer::SkyAsset* asset = skies.FindMutable(id);
    if (asset == nullptr) {
        return renderer::kNoSkyAsset;
    }

    // 共有アセットから来たものは置き場所と永続 ID を持つ（埋め込みの旧形式では空）。
    asset->assetPath = FromUtf8(ReadString(node, "_assetPath"));
    asset->assetUid = ReadString(node, "uid");
    asset->sky.source = static_cast<renderer::SkySource>(
        EnumValue(kSkySourceNames, node, "source", static_cast<uint32_t>(defaults.source)));
    if (const std::string hdri = ReadString(node, "hdri"); !hdri.empty()) {
        asset->sky.hdriPath = ResolvePath(hdri, baseDir);
    }
    asset->sky.skyLuminance = ReadFloat(node, "skyLuminance", defaults.skyLuminance);
    asset->sky.iblIntensity = ReadFloat(node, "iblIntensity", defaults.iblIntensity);

    if (const json* procedural = FindMember(node, "procedural");
        procedural != nullptr && procedural->is_object()) {
        renderer::SkySettings& target = asset->sky.procedural;
        const renderer::SkySettings proceduralDefaults;
        target.zenithColor = ReadFloat3(*procedural, "zenithColor", proceduralDefaults.zenithColor);
        target.horizonColor =
            ReadFloat3(*procedural, "horizonColor", proceduralDefaults.horizonColor);
        target.groundColor = ReadFloat3(*procedural, "groundColor", proceduralDefaults.groundColor);
        target.intensity = ReadFloat(*procedural, "intensity", proceduralDefaults.intensity);
    }
    return id;
}

// 天球アセットが無いプロジェクト（天球を入れる前の形式）から 1 つ作る。
// 当時は環境がビューポートに 1 つしか無く、preview 節に直接書かれていた。
void MigrateSkyFromPreview(const json& preview, renderer::SkyLibrary& skies,
                           const fs::path& baseDir) {
    const renderer::SkyDefinition defaults;
    const std::string hdri = ReadString(preview, "hdri");
    const renderer::SkyAssetId id = skies.Add("既定の空");
    renderer::SkyAsset* asset = skies.FindMutable(id);
    if (asset == nullptr) {
        return;
    }
    if (!hdri.empty()) {
        asset->sky.source = renderer::SkySource::Hdri;
        asset->sky.hdriPath = ResolvePath(hdri, baseDir);
        asset->name = ToUtf8Display(asset->sky.hdriPath.stem());
    }
    asset->sky.skyLuminance = ReadFloat(preview, "hdriSkyLuminance", defaults.skyLuminance);
    asset->sky.iblIntensity = ReadFloat(preview, "iblIntensity", defaults.iblIntensity);

    if (const json* sky = FindMember(preview, "sky"); sky != nullptr && sky->is_object()) {
        renderer::SkySettings& target = asset->sky.procedural;
        const renderer::SkySettings proceduralDefaults;
        target.zenithColor = ReadFloat3(*sky, "zenithColor", proceduralDefaults.zenithColor);
        target.horizonColor = ReadFloat3(*sky, "horizonColor", proceduralDefaults.horizonColor);
        target.groundColor = ReadFloat3(*sky, "groundColor", proceduralDefaults.groundColor);
        target.intensity = ReadFloat(*sky, "intensity", proceduralDefaults.intensity);
    }
    skies.SetActive(id);
}

// --- ファイル入出力 -------------------------------------------------------

bool WriteJsonFile(const fs::path& path, const json& document) {
    std::error_code error;
    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, error);
    }

    // いきなり上書きすると、ディスクフルなどで途中失敗したときに元のファイルが
    // 壊れたまま残る。一時ファイルへ書き切ってから rename で差し替える。
    const fs::path tempPath = path.wstring() + L".tmp";
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            ROCK_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
        // 人が読める形で書く。差分も取りやすい。壊れた文字列が混ざっていても
        // 例外を出さない（不正な UTF-8 は置換文字にする）。
        stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        // バッファの最終書き込み・closeの失敗も、元ファイルの差し替え前に検出する。
        stream.close();
        if (!stream.good()) {
            ROCK_LOG_ERROR("ファイルの書き込みに失敗しました: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
    }

    std::error_code renameError;
    fs::rename(tempPath, path, renameError);
    if (renameError) {
        ROCK_LOG_ERROR("ファイルを差し替えられませんでした: %s", ToUtf8Portable(path).c_str());
        std::error_code removeError;
        fs::remove(tempPath, removeError);
        return false;
    }
    return true;
}

bool ReadJsonFile(const fs::path& path, const char* expectedFormat, int maxVersion,
                  json& outDocument) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        ROCK_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // 例外は使わない方針なので、パース失敗は discarded で受ける。
    outDocument = json::parse(stream, nullptr, false);
    if (outDocument.is_discarded() || !outDocument.is_object()) {
        ROCK_LOG_ERROR("JSON として読めませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // material-mixer 時代のファイルは "material-mixer.project" などの形式名を持つ。
    // 中身は同じなので、旧形式名は新形式名へ読み替えて受け付ける（書くのは新形式名のみ）。
    std::string format = ReadString(outDocument, "format");
    if (format.rfind("material-mixer.", 0) == 0) {
        format = "rock-editor." + format.substr(std::string("material-mixer.").size());
    }
    if (format != expectedFormat) {
        ROCK_LOG_ERROR("形式が違います（%s ではなく %s）: %s", expectedFormat, format.c_str(),
                     ToUtf8Portable(path).c_str());
        return false;
    }
    const int version = ReadInt(outDocument, "version", 0);
    if (version > maxVersion) {
        ROCK_LOG_ERROR("このバージョンでは読めません（ファイル %d > 対応 %d）: %s", version,
                     maxVersion, ToUtf8Portable(path).c_str());
        return false;
    }
    return true;
}

}  // namespace

bool SaveProject(const std::filesystem::path& path, const ProjectRefs& refs,
                 ProjectWorkspace* workspace) {
    // 裸のファイル名（親ディレクトリ無し）で保存すると相対パスが作れず、
    // 全参照が絶対パスで書かれてしまう。先に絶対化してから基準を取る。
    std::error_code absoluteError;
    const fs::path absolutePath = fs::absolute(path, absoluteError);
    const fs::path& savePath = absoluteError ? path : absolutePath;
    const fs::path baseDir = savePath.parent_path();

    // シーンとして保存するときは、先に共有アセットを各ファイルへ書く。
    // ここで失敗したら文書には触らない（片方だけ新しい状態を作らない）。
    if (workspace != nullptr) {
        if (!IsSceneFile(savePath) || !workspace->Contains(savePath)) {
            ROCK_LOG_ERROR("シーンはプロジェクトルート内の .rockgraph / .mountaingraph へ保存してください: %s",
                         ToUtf8Display(savePath).c_str());
            return false;
        }
        if (!SaveSharedAssets(*workspace, refs)) {
            ROCK_LOG_ERROR("共有アセットを保存できないため、シーンの保存を中止しました");
            return false;
        }
    }

    json document;
    document["format"] = kProjectFormat;
    document["version"] = kProjectFormatVersion;
    document["app"] = ROCK_APP_VERSION;

    // --- テクスチャ（画像は参照。パスはプロジェクトからの相対） -----------
    // ファイルの中では通し番号で参照する。実行中の ID をそのまま書くと、
    // 削除して番号が飛んだときにファイルが読みにくくなる。
    std::unordered_map<compositor::TextureId, int> textureIndex;
    json textures = json::array();
    for (const compositor::LibraryTexture& entry : refs.textures.Entries()) {
        // 一時的なテクスチャ（Material Bake の結果）はファイルを持たない。参照は「なし」として書かれる。
        if (entry.transient) continue;
        const int index = static_cast<int>(textures.size()) + 1;
        textureIndex[entry.id] = index;

        json node;
        node["id"] = index;
        node["name"] = entry.name;
        node["path"] = RelativePathString(entry.path, baseDir);
        textures.push_back(std::move(node));
    }
    document["textures"] = std::move(textures);

    const TextureWriter writeTexture = [&textureIndex](compositor::TextureId id) {
        const auto it = textureIndex.find(id);
        return (it != textureIndex.end()) ? json(it->second) : json();
    };

    // --- マテリアル（構造ごと埋め込む） -----------------------------------
    std::unordered_map<compositor::MaterialAssetId, int> materialIndex;
    json materials = json::array();
    for (const compositor::MaterialAsset& asset : refs.materials.Entries()) {
        if (asset.transient) continue;
        const int index = static_cast<int>(materials.size()) + 1;
        materialIndex[asset.id] = index;

        json node = WriteMaterialBody(asset, writeTexture);
        node["id"] = index;
        if (workspace != nullptr) {
            node["_assetPath"] = ToUtf8Portable(asset.assetPath);
            node["uid"] = asset.assetUid;
        }
        materials.push_back(std::move(node));
    }
    for (auto& node : materials) MapLayerMaterials(node, [&](const json& value) -> json {
        const auto it = materialIndex.find(value.is_number_integer() ? value.get<uint32_t>() : 0);
        return it == materialIndex.end() ? json(0) : json(it->second);
    });
    document["materials"] = std::move(materials);

    // --- モデル（FBX は参照。スロットのマテリアルは文書内の番号） ----------
    std::unordered_map<uint64_t, int> modelIndex;
    if (refs.models != nullptr) {
        json models = json::array();
        for (const renderer::ModelAsset& asset : *refs.models) {
            modelIndex[asset.id] = static_cast<int>(models.size()) + 1;
            json slots = json::array();
            for (const compositor::MaterialAssetId id : asset.materials) {
                const auto found = materialIndex.find(id);
                slots.push_back(found == materialIndex.end() ? json() : json(found->second));
            }
            json node = {{"id", static_cast<int>(models.size()) + 1},
                         {"name", asset.name},
                         {"path", RelativePathString(asset.path, baseDir)},
                         {"scale", asset.scale},
                         {"materials", std::move(slots)}};
            if (workspace != nullptr) {
                node["_assetPath"] = ToUtf8Portable(asset.assetPath);
                node["uid"] = asset.assetUid;
            }
            models.push_back(std::move(node));
        }
        document["models"] = std::move(models);
    }

    // --- ノードグラフ -----------------------------------------------------
    // 版 4 から layers 節は書かない。合成の構造はグラフだけが持つ。
    const std::function<json(compositor::MaterialAssetId)> writeMaterial =
        [&materialIndex](compositor::MaterialAssetId id) {
            const auto it = materialIndex.find(id);
            return (it != materialIndex.end()) ? json(it->second) : json();
        };
    const std::function<json(uint64_t)> writeModel = [&modelIndex](uint64_t id) {
        const auto found = modelIndex.find(id);
        return found != modelIndex.end() ? json(found->second) : json();
    };
    document["graph"] = WriteGraph(refs.graph, writeMaterial, writeModel, writeTexture, baseDir);

    // 天球はマテリアルと同じく、構造ごと埋め込む（画像だけ相対パスの参照）。
    json skies = json::array();
    int activeSkyIndex = 0;
    for (const renderer::SkyAsset& asset : refs.skies.Entries()) {
        if (asset.id == refs.skies.ActiveId()) {
            activeSkyIndex = static_cast<int>(skies.size());
        }
        skies.push_back(WriteSky(asset, baseDir));
        if (workspace != nullptr) {
            skies.back()["_assetPath"] = ToUtf8Portable(asset.assetPath);
            skies.back()["uid"] = asset.assetUid;
        }
    }
    document["skies"] = std::move(skies);
    document["activeSky"] = activeSkyIndex;

    document["preview"] = WritePreview(refs.renderer);

    if (workspace != nullptr) {
        if (!workspace->SaveScene(savePath, document)) {
            ROCK_LOG_ERROR("シーンを保存できませんでした: %s", ToUtf8Display(savePath).c_str());
            return false;
        }
        ROCK_LOG_INFO("シーンを保存しました: %s", ToUtf8Display(savePath).c_str());
        return true;
    }
    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    ROCK_LOG_INFO("プロジェクトを保存しました: %s", ToUtf8Portable(savePath).c_str());
    return true;
}

bool LoadProject(const std::filesystem::path& path, rhi::Device& device,
                 rhi::PipelineCache& pipelineCache, const ProjectRefs& refs,
                 ProjectWorkspace* workspace, bool outsideRoot) {
    json document;
    if (workspace != nullptr) {
        // シーンは参照する共有アセットを展開してから、従来の読み込み器に渡す。
        // 欠けたアセットがあればここで止まり、現在の文書は保持される。
        if (!workspace->ReadScene(path, document, !outsideRoot)) {
            ROCK_LOG_ERROR("シーンまたは参照アセットを開けません: %s", ToUtf8Display(path).c_str());
            return false;
        }
        if (const int version = ReadInt(document, "version", 0); version > kProjectFormatVersion) {
            ROCK_LOG_ERROR("このバージョンでは読めません（シーン %d > 対応 %d）: %s", version,
                         kProjectFormatVersion, ToUtf8Display(path).c_str());
            return false;
        }
    } else if (!ReadJsonFile(path, kProjectFormat, kProjectFormatVersion, document)) {
        return false;
    }

    const fs::path baseDir = path.parent_path();

    // 旧ファイルの手入力メッシュシーン（scene）は読まない。表示するメッシュは
    // グラフの Mesh Output から生成する。
    if (FindMember(document, "scene") != nullptr) {
        ROCK_LOG_WARN("旧形式の手入力メッシュシーン（scene）は読み飛ばしました");
    }
    refs.renderer.ClearMeshScene(device);

    // ここから先は現在の中身を捨てて入れ替える。読み込みは GPU 待機を伴うため、
    // 呼び出し側がフレームの外で呼んでいること。
    refs.materials.Clear(device);
    refs.skies.Clear(device);
    refs.textures.Clear(device);

    // --- テクスチャ -------------------------------------------------------
    std::unordered_map<int, compositor::TextureId> textureIds;
    if (const json* textures = FindMember(document, "textures");
        textures != nullptr && textures->is_array()) {
        for (const json& node : *textures) {
            if (!node.is_object()) {
                continue;
            }
            const int index = ReadInt(node, "id", 0);
            const fs::path texturePath = ResolvePath(ReadString(node, "path"), baseDir);
            if (index <= 0 || texturePath.empty()) {
                continue;
            }
            const std::string name = ReadString(node, "name");
            compositor::TextureId id = refs.textures.Load(device, pipelineCache, texturePath);
            if (id == compositor::kNoTexture) {
                // 画像が見つからなくても、残りは読み込む。**参照は捨てない。**
                // パスと名前だけの「リンク切れ」として登録し、マテリアルやノードの
                // 割り当てはそこへ繋いでおく。消してしまうと、次に保存した時点で
                // どのファイルを指していたかが失われ、繋ぎ直せなくなる。
                ROCK_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
                            ToUtf8Portable(texturePath).c_str());
                id = refs.textures.AddMissing(texturePath, name);
                if (id == compositor::kNoTexture) {
                    continue;
                }
            }
            textureIds[index] = id;
            if (compositor::LibraryTexture* entry = refs.textures.FindMutable(id);
                entry != nullptr && !name.empty()) {
                entry->name = name;
            }
        }
    }
    const TextureReader readTexture = [&textureIds](const json& node) {
        if (!node.is_number_integer()) {
            return compositor::kNoTexture;
        }
        const auto it = textureIds.find(node.get<int>());
        return (it != textureIds.end()) ? it->second : compositor::kNoTexture;
    };

    // --- マテリアル -------------------------------------------------------
    std::unordered_map<int, compositor::MaterialAssetId> materialIds;
    if (const json* materials = FindMember(document, "materials");
        materials != nullptr && materials->is_array()) {
        for (const json& node : *materials) {
            if (!node.is_object()) {
                continue;
            }
            const int index = ReadInt(node, "id", 0);
            const compositor::MaterialAssetId id = refs.materials.Add("マテリアル");
            if (compositor::MaterialAsset* asset = refs.materials.FindMutable(id);
                asset != nullptr) {
                ReadMaterialBody(node, *asset, readTexture);
                asset->assetPath = FromUtf8(ReadString(node, "_assetPath"));
                asset->assetUid = ReadString(node, "uid");
                asset->thumbnailDirty = true;
            }
            if (index > 0) {
                materialIds[index] = id;
            }
        }
    }

    for (const auto& [index, id] : materialIds) RemapLayerReferences(*refs.materials.FindMutable(id), materialIds);

    // --- モデル -----------------------------------------------------------
    // 欠けた FBX も参照を残す（リンク切れとして表示し、別の場所へ保存し直しても割り当てを失わない）。
    std::unordered_map<int, uint64_t> modelIds;
    if (refs.models != nullptr) {
        refs.models->clear();
        if (const json* models = FindMember(document, "models"); models != nullptr && models->is_array()) {
            for (const json& node : *models) {
                if (!node.is_object()) {
                    continue;
                }
                renderer::ModelAsset asset;
                asset.id = refs.models->size() + 1;
                asset.name = ReadString(node, "name");
                asset.assetPath = FromUtf8(ReadString(node, "_assetPath"));
                asset.assetUid = ReadString(node, "uid");
                asset.path = ResolvePath(ReadString(node, "path"), baseDir);
                asset.scale = ReadModelScale(node);
                if (!renderer::LoadModel(asset.path, asset)) {
                    ROCK_LOG_WARN("モデルを読み込めません（%s）: %s", asset.error.c_str(),
                                ToUtf8Display(asset.path).c_str());
                }
                if (const json* slots = FindMember(node, "materials"); slots != nullptr && slots->is_array()) {
                    asset.materials.resize(std::max(asset.materials.size(), slots->size()),
                                           compositor::kNoMaterialAsset);
                    for (size_t i = 0; i < slots->size(); ++i) {
                        const auto found = (*slots)[i].is_number_integer()
                                               ? materialIds.find((*slots)[i].get<int>())
                                               : materialIds.end();
                        if (found != materialIds.end()) asset.materials[i] = found->second;
                    }
                }
                modelIds[ReadInt(node, "id", 0)] = asset.id;
                refs.models->push_back(std::move(asset));
            }
        }
    }

    // --- ノードグラフ（旧形式は layers[] から移行） -----------------------
    const std::function<compositor::MaterialAssetId(const json&)> readMaterial =
        [&materialIds](const json& value) {
            if (!value.is_number_integer()) {
                return compositor::kNoMaterialAsset;
            }
            const auto it = materialIds.find(value.get<int>());
            return (it != materialIds.end()) ? it->second : compositor::kNoMaterialAsset;
        };
    // graph 節が唯一の合成。無ければ既定（Surface 1 つ）へ戻す。
    const json* graphNode = FindMember(document, "graph");
    bool graphLoaded = false;
    if (graphNode != nullptr && graphNode->is_object()) {
        const std::function<uint64_t(const json&)> readModel = [&modelIds](const json& value) -> uint64_t {
            if (!value.is_number_integer()) return 0;
            const auto found = modelIds.find(value.get<int>());
            return found != modelIds.end() ? found->second : 0;
        };
        graphLoaded = ReadGraph(*graphNode, refs.graph, readMaterial, readModel, readTexture, baseDir);
    }
    if (!graphLoaded) {
        refs.graph = graph::NodeGraph::CreateDefault();
    }

    // preview が無い（または壊れている）プロジェクトでも必ず既定値で埋める。
    // 呼ばないと、前のプロジェクトのカメラ・ライト・露出が残ってしまう。
    const json* preview = FindMember(document, "preview");
    const json emptyPreview = json::object();
    const json& previewNode =
        (preview != nullptr && preview->is_object()) ? *preview : emptyPreview;
    ReadPreview(previewNode, refs.renderer);

    // 天球。無ければ preview 節から 1 つ作る（天球を入れる前のプロジェクト）。
    if (const json* skies = FindMember(document, "skies");
        skies != nullptr && skies->is_array() && !skies->empty()) {
        std::vector<renderer::SkyAssetId> ids;
        for (const json& sky : *skies) {
            if (!sky.is_object()) {
                continue;
            }
            ids.push_back(ReadSky(sky, refs.skies, baseDir));
        }
        const auto activeIndex = static_cast<size_t>(ReadUInt(document, "activeSky", 0));
        if (activeIndex < ids.size()) {
            refs.skies.SetActive(ids[activeIndex]);
        }
    } else {
        MigrateSkyFromPreview(previewNode, refs.skies, baseDir);
    }
    refs.skies.EnsureDefault();

    ROCK_LOG_INFO("プロジェクトを開きました: %s", ToUtf8Portable(path).c_str());
    return true;
}

bool SaveSharedAssets(ProjectWorkspace& workspace, const ProjectRefs& refs) {
    if (!workspace.Scan()) {
        return false;
    }
    bool valid = true;
    // 画像の参照。ルート外の実在ファイルは Imported/ へ取り込む。
    // リンク切れ（ファイルが無い）はパスだけを残し、参照を失わない。
    const auto source = [&](const fs::path& path) -> json {
        if (path.empty()) {
            return nullptr;
        }
        std::error_code error;
        if (!fs::is_regular_file(path, error)) {
            return {{"path", ToUtf8Portable(path.lexically_normal())}};
        }
        const fs::path target = workspace.Import(path, workspace.Root() / L"Imported");
        const json result = target.empty() ? json() : workspace.Reference(target);
        if (result.is_null()) {
            valid = false;
        }
        return result;
    };
    const TextureWriter writeTexture = [&](compositor::TextureId id) -> json {
        const compositor::LibraryTexture* entry = refs.textures.Find(id);
        return (entry != nullptr) ? source(entry->path) : json();
    };
    // この保存で ID を持つアセットが使う ID。ID の無いものを既存のファイルへ寄せるときに奪わない。
    std::unordered_set<std::string> claimedUids;
    for (const compositor::MaterialAsset& entry : refs.materials.Entries()) {
        if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
    }
    for (const renderer::SkyAsset& entry : refs.skies.Entries()) {
        if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
    }
    if (refs.models != nullptr) {
        for (const renderer::ModelAsset& entry : *refs.models) {
            if (!entry.assetUid.empty()) claimedUids.insert(entry.assetUid);
        }
    }
    // 置き場所が未定のものの保存先。ID の無いもの（旧 .reproj・単体 .rockmat から来たもの）は、
    // 同じ中身の既存アセットがあればそれを使う。無ければ名前から連番で作る。
    const auto placement = [&](json& body, const fs::path& current, const char* kind, const wchar_t* folder,
                               const std::string& name, const char* extension) -> fs::path {
        if (!current.empty() && workspace.Contains(current)) {
            return current;
        }
        if (ReadString(body, "uid").empty()) {
            const std::string uid = workspace.FindIdenticalAsset(kind, body, claimedUids);
            if (!uid.empty()) {
                body["uid"] = uid;
                return workspace.Resolve({{"uid", uid}});
            }
        }
        return workspace.UniquePath(workspace.Root() / folder, name, extension);
    };
    for (int pass = 0; pass < 2; ++pass) for (const compositor::MaterialAsset& entry : refs.materials.Entries()) {
        if (entry.transient || bool(entry.layerMaterial) != (pass == 1)) continue;
        compositor::MaterialAsset* asset = refs.materials.FindMutable(entry.id);
        json body = WriteMaterialBody(*asset, writeTexture);
        body["uid"] = asset->assetUid;
        if (asset->layerMaterial) MapLayerMaterials(body, [&](const json& value) -> json {
            const auto* source = refs.materials.Find(value.is_number_integer() ? value.get<uint32_t>() : 0);
            if (!source) return 0;
            if (source->layerMaterial || source->assetPath.empty()) { valid = false; return 0; }
            return workspace.Reference(source->assetPath);
        });
        const char* kind = asset->layerMaterial ? "layer-material-asset" : "material-asset";
        fs::path assetPath = placement(body, asset->assetPath, kind, L"Materials", asset->name, asset->layerMaterial ? ".tglayer" : ".rockmat");
        if (!valid || !workspace.SaveAsset(assetPath, kind, body)) {
            ROCK_LOG_ERROR("マテリアルを保存できません: %s", asset->name.c_str());
            return false;
        }
        asset->assetPath = assetPath;
        asset->assetUid = ReadString(body, "uid");
        claimedUids.insert(asset->assetUid);
    }
    for (const renderer::SkyAsset& entry : refs.skies.Entries()) {
        renderer::SkyAsset* asset = refs.skies.FindMutable(entry.id);
        json body = WriteSky(*asset, workspace.Root());
        body["hdri"] = source(asset->sky.hdriPath);
        body["uid"] = asset->assetUid;
        fs::path assetPath = placement(body, asset->assetPath, "sky-asset", L"Skies", asset->name, ".rocksky");
        if (!valid || !workspace.SaveAsset(assetPath, "sky-asset", body)) {
            ROCK_LOG_ERROR("天球を保存できません: %s", asset->name.c_str());
            return false;
        }
        asset->assetPath = assetPath;
        asset->assetUid = ReadString(body, "uid");
        claimedUids.insert(asset->assetUid);
    }
    // モデル。FBX は元ファイルの固定 ID、スロットは上で保存した .rockmat の固定 ID で参照する
    // （SaveScene が作る本文と一致させ、中身が変わらなければ書き直さない）。
    if (refs.models != nullptr) {
        for (renderer::ModelAsset& model : *refs.models) {
            json slots = json::array();
            for (const compositor::MaterialAssetId id : model.materials) {
                const compositor::MaterialAsset* asset = refs.materials.Find(id);
                json value = (asset != nullptr && !asset->assetPath.empty()) ? workspace.Reference(asset->assetPath) : json();
                if (asset != nullptr && value.is_null()) valid = false;
                slots.push_back(std::move(value));
            }
            json body = {{"name", model.name}, {"source", source(model.path)}, {"scale", model.scale},
                         {"materials", std::move(slots)}};
            body["uid"] = model.assetUid;
            fs::path assetPath = placement(body, model.assetPath, "model-asset", L"Models", model.name, ".model");
            if (!valid || !workspace.SaveAsset(assetPath, "model-asset", body)) {
                ROCK_LOG_ERROR("モデルを保存できません: %s", model.name.c_str());
                return false;
            }
            model.assetPath = assetPath;
            model.assetUid = ReadString(body, "uid");
            claimedUids.insert(model.assetUid);
        }
    }
    return valid;
}

namespace {

// 展開済みの文書（ProjectWorkspace::Expand の結果）の画像とマテリアルをライブラリへ足す。
// 同じ固定 ID のマテリアルが読み込み済みなら足さずにそれを使う。文書内の番号 → ライブラリの ID を返す。
void AddExpandedLibraries(const json& document, rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          compositor::TextureLibrary& textures, compositor::MaterialLibrary& materials,
                          std::unordered_map<int, compositor::TextureId>& textureIds,
                          std::unordered_map<int, compositor::MaterialAssetId>& materialIds) {
    for (const json& node : document.at("textures")) {
        const fs::path texturePath = FromUtf8(ReadString(node, "path"));
        compositor::TextureId id = textures.Load(device, pipelineCache, texturePath);
        if (id == compositor::kNoTexture) {
            ROCK_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s", ToUtf8Display(texturePath).c_str());
            id = textures.AddMissing(texturePath, ReadString(node, "name"));
        }
        textureIds[ReadInt(node, "id", 0)] = id;
    }
    const TextureReader readTexture = [&textureIds](const json& value) {
        const auto it = textureIds.find(value.is_number_integer() ? value.get<int>() : 0);
        return (it != textureIds.end()) ? it->second : compositor::kNoTexture;
    };
    std::vector<compositor::MaterialAssetId> added;
    for (const json& node : document.at("materials")) {
        const std::string uid = ReadString(node, "uid");
        const auto& entries = materials.Entries();
        const auto existing = std::find_if(entries.begin(), entries.end(),
                                           [&uid](const compositor::MaterialAsset& a) { return a.assetUid == uid; });
        if (!uid.empty() && existing != entries.end()) {
            materialIds[ReadInt(node, "id", 0)] = existing->id;
            continue;
        }
        const compositor::MaterialAssetId id = materials.Add(ReadString(node, "name"));
        compositor::MaterialAsset* asset = materials.FindMutable(id);
        ReadMaterialBody(node, *asset, readTexture);
        added.push_back(id);
        asset->assetUid = uid;
        asset->assetPath = FromUtf8(ReadString(node, "_assetPath"));
        asset->thumbnailDirty = true;
        materialIds[ReadInt(node, "id", 0)] = id;
    }
    for (const auto id : added) RemapLayerReferences(*materials.FindMutable(id), materialIds);
}

}  // namespace

bool LoadSharedAsset(ProjectWorkspace& workspace, const std::filesystem::path& path,
                     rhi::Device& device, rhi::PipelineCache& pipelineCache,
                     compositor::TextureLibrary& textures, compositor::MaterialLibrary& materials,
                     renderer::SkyLibrary& skies, bool rescan,
                     std::vector<renderer::ModelAsset>* models) {
    if (rescan && !workspace.Scan()) {
        return false;
    }
    // 読み込みではアセットの原本を書き換えない（Reference は .meta を作ることがある）。
    json header;
    if (!workspace.Contains(path) || !ProjectWorkspace::ReadJson(path, header)) {
        return false;
    }
    const std::string assetUid = ReadString(header, "uid");
    if (assetUid.empty()) {
        return false;
    }
    const json reference = {{"uid", assetUid}, {"path", RelativePathString(path, workspace.Root())}};
    const bool isMaterial = _wcsicmp(path.extension().c_str(), L".rockmat") == 0 || _wcsicmp(path.extension().c_str(), L".tglayer") == 0;
    const bool isModel = _wcsicmp(path.extension().c_str(), L".model") == 0;
    if (isModel && models == nullptr) {
        return false;
    }
    json document;
    document[isMaterial ? "materials" : isModel ? "models" : "skies"] = json::array({{{"id", 1}, {"asset", reference}}});
    if (!workspace.Expand(document)) {
        return false;
    }
    std::unordered_map<int, compositor::TextureId> textureIds;
    std::unordered_map<int, compositor::MaterialAssetId> materialIds;
    AddExpandedLibraries(document, device, pipelineCache, textures, materials, textureIds, materialIds);
    for (const json& node : document["models"]) {
        const std::string uid = ReadString(node, "uid");
        if (std::any_of(models->begin(), models->end(), [&uid](const renderer::ModelAsset& a) { return a.assetUid == uid; })) {
            continue;
        }
        renderer::ModelAsset asset;
        asset.id = 1;
        for (const renderer::ModelAsset& existing : *models) asset.id = std::max(asset.id, existing.id + 1);
        asset.assetUid = uid;
        asset.assetPath = FromUtf8(ReadString(node, "_assetPath"));
        asset.name = ReadString(node, "name");
        asset.path = FromUtf8(ReadString(node, "path"));
        asset.scale = ReadModelScale(node);
        if (!renderer::LoadModel(asset.path, asset)) {
            ROCK_LOG_WARN("モデルを読み込めません（%s）: %s", asset.error.c_str(), ToUtf8Display(asset.path).c_str());
        }
        const json& slots = node.at("materials");
        asset.materials.resize(std::max(asset.materials.size(), slots.size()), compositor::kNoMaterialAsset);
        for (size_t i = 0; i < slots.size(); ++i) {
            const auto found = slots[i].is_number_integer() ? materialIds.find(slots[i].get<int>()) : materialIds.end();
            if (found != materialIds.end()) asset.materials[i] = found->second;
        }
        models->push_back(std::move(asset));
    }
    for (const json& node : document["skies"]) {
        const std::string uid = ReadString(node, "uid");
        const auto& entries = skies.Entries();
        const auto existing = std::find_if(entries.begin(), entries.end(),
                                           [&uid](const renderer::SkyAsset& a) { return a.assetUid == uid; });
        if (existing == entries.end()) {
            skies.SetActive(ReadSky(node, skies, workspace.Root()));
        } else {
            skies.SetActive(existing->id);
        }
    }
    ROCK_LOG_INFO("アセットを読み込みました: %s", ToUtf8Display(path).c_str());
    return true;
}

bool SaveMaterial(const std::filesystem::path& path, const compositor::MaterialAsset& asset,
                  const compositor::TextureLibrary& textures) {
    if (asset.layerMaterial) { ROCK_LOG_ERROR("レイヤーマテリアルは共有アセットとして保存してください"); return false; }
    // SaveProject と同じく、裸のファイル名でも相対パスが作れるよう絶対化する。
    std::error_code absoluteError;
    const fs::path absolutePath = fs::absolute(path, absoluteError);
    const fs::path& savePath = absoluteError ? path : absolutePath;
    const fs::path baseDir = savePath.parent_path();

    // 単体ファイルでは、テクスチャをこのファイルからの相対パスで参照する。
    const TextureWriter writeTexture = [&textures, &baseDir](compositor::TextureId id) {
        const compositor::LibraryTexture* entry = textures.Find(id);
        if (entry == nullptr) {
            return json();
        }
        return json(RelativePathString(entry->path, baseDir));
    };

    json document = WriteMaterialBody(asset, writeTexture);
    document["format"] = kMaterialFormat;
    document["version"] = kMaterialFormatVersion;
    document["app"] = ROCK_APP_VERSION;

    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    ROCK_LOG_INFO("マテリアルを書き出しました: %s", ToUtf8Portable(savePath).c_str());
    return true;
}

compositor::MaterialAssetId LoadMaterial(const std::filesystem::path& path, rhi::Device& device,
                                         rhi::PipelineCache& pipelineCache,
                                         compositor::TextureLibrary& textures,
                                         compositor::MaterialLibrary& materials) {
    json document;
    if (!ReadJsonFile(path, kMaterialFormat, kMaterialFormatVersion, document)) {
        return compositor::kNoMaterialAsset;
    }

    const fs::path baseDir = path.parent_path();
    // 参照している画像はその場で読み込む。すでに同じ画像があれば読み直さない。
    const TextureReader readTexture = [&](const json& node) {
        if (!node.is_string()) {
            return compositor::kNoTexture;
        }
        const fs::path texturePath = ResolvePath(node.get<std::string>(), baseDir);
        if (texturePath.empty()) {
            return compositor::kNoTexture;
        }
        const compositor::TextureId id = textures.Load(device, pipelineCache, texturePath);
        if (id == compositor::kNoTexture) {
            // プロジェクトと同じく、見つからない画像はリンク切れとして残す。
            ROCK_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
                        ToUtf8Portable(texturePath).c_str());
            return textures.AddMissing(texturePath, std::string());
        }
        return id;
    };

    const compositor::MaterialAssetId id = materials.Add("マテリアル");
    compositor::MaterialAsset* asset = materials.FindMutable(id);
    if (asset == nullptr) {
        return compositor::kNoMaterialAsset;
    }
    ReadMaterialBody(document, *asset, readTexture);
    asset->thumbnailDirty = true;

    ROCK_LOG_INFO("マテリアルを読み込みました: %s", ToUtf8Portable(path).c_str());
    return id;
}

}  // namespace rock::io
