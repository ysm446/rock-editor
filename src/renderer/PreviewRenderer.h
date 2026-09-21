#pragma once

#include "renderer/CrackGuide.h"
#include "renderer/ShadowCascades.h"

#include "compositor/MaterialEvaluator.h"
#include "compositor/TextureLibrary.h"
#include "renderer/Atmosphere.h"
#include "renderer/Camera.h"
#include "renderer/Environment.h"
#include "renderer/SkyLibrary.h"
#include "renderer/Mesh.h"
#include "renderer/PreviewDiagnostics.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <DirectXMath.h>

#include <array>
#include <chrono>
#include <functional>

namespace rock::renderer {

// ビューポートに何を出すか。シェーダの ROCK_VIEW_* と一致させること。
//
// チャンネルを覗く表示（ベースカラー〜ハイト）は「中身をそのまま見る」ためのもので、
// 露出もトーンマップも掛けない。**形を見る表示（ワイヤーフレーム / クレイ）は
// これに含まれない**（クレイは陰影を付けるので、シェーディングと同じ扱い）。
enum class DebugView : uint32_t {
    Shaded = 0,
    BaseColor = 1,
    // 陰影に使う向き（法線マップを当てたあと）を、カメラ空間とワールド空間で見る。
    // カメラ空間は「画面に対してどちらを向いているか」が読める（正面が水色）。
    NormalView = 2,
    NormalWorld = 3,
    Roughness = 4,
    Metallic = 5,
    AmbientOcclusion = 6,
    Height = 7,
    // **周りの平均を引いた「その場の起伏」だけ**の表示。
    // 素材のハイトマップをそのまま貼ったように見える。
    HeightLocal = 8,
    // 形だけを見る表示。ラスタライザをワイヤーフレームにする。
    Wireframe = 9,
    // **テクスチャを貼らない単色の陰影**（粘土模型のような見え方）。メッシュの確認用。
    // 形（ディスプレイスメント）はそのままで、色 / 法線 / サーフェスだけを
    // 合成結果から切り離し、単色マテリアルと**面の向き**（画面微分で起こした法線）
    // で陰影を付ける。法線マップに隠れない、実際のポリゴンの形が見える。
    Clay = 10,
};

enum class TonemapMode : uint32_t {
    None = 0,
    Reinhard = 1,
    Aces = 2,
};

// 物理カメラの露出設定。
//   EV100    = log2(N^2 / t) - log2(ISO / 100)
//   exposure = 1 / (1.2 * 2^EV100)
struct ExposureSettings {
    // 自動露出。シーンカラーの輝度ヒストグラムから測った EV100 に補正を足して使う。
    // 自動のときは useManualEv と物理カメラの値を露出には使わない（F 値は被写界深度に残る）。
    bool automatic = false;
    float compensation = 0.0f;           // 露出補正（EV）。
    float minEv100 = -4.0f, maxEv100 = 18.0f;
    float adaptationSpeed = 2.0f;        // 1 秒あたりの追従率。大きいほど速く追従する。
    float autoEv100 = 15.0f;             // 実行時のみ。平滑化した測光値。
    bool autoValid = false;              // 実行時のみ。測光値を一度でも受け取ったか。

    bool useManualEv = false;
    float manualEv100 = 15.0f;
    float aperture = 16.0f;              // N（F 値）
    float shutterSpeed = 1.0f / 250.0f;  // t（秒）
    float iso = 100.0f;

    float Ev100() const;
    float Exposure() const;
};

struct LightSettings {
    float azimuth = 0.9f;               // 方位角（ラジアン）
    float elevation = 0.9f;             // 仰角（ラジアン）
    float illuminance = 100000.0f;      // lux。晴天の直射日光がおよそ 100000
    DirectX::XMFLOAT3 color = {1.0f, 0.98f, 0.95f};

    // サーフェスから光源へ向かう正規化ベクトル。
    DirectX::XMFLOAT3 Direction() const;
};

// 1 フレームぶんの描画の量。ビューポートの右上に出す。
//
// **投入した量（IA が読む量）を数える。** テセレーションを入れると実際に
// 出る三角形はこれより多いが、CPU 側では分からないのでパッチ数と上限を添える。
struct RenderStats {
    uint32_t drawCalls = 0;
    // 投入した頂点とインデックス。インデックス付き描画では
    // 「頂点 = インデックス数」（IA がその回数だけ頂点を読む）。
    uint64_t vertices = 0;
    uint64_t triangles = 0;
    // テセレーションのパッチ数。使っていなければ 0。
    uint64_t patches = 0;
    bool tessellation = false;
    float tessellationFactor = 1.0f;
};

// 絞りの形。ボケの形になる。
enum class ApertureShape : uint32_t {
    Circle = 0,
    Triangle = 1,
    Hexagon = 2,
    Octagon = 3,
};

// 被写界深度。**ビューポートの見え方だけの設定**で、合成結果には一切効かない。
//
// レンズの値は増やさない。焦点距離はカメラの画角から、F 値は露出の絞りから取る。
// 被写界深度のためだけに同じ意味の値をもう一組持つと、どちらが効いているのか
// 分からなくなる（露出とレンズが食い違った絵になる）。
struct DofSettings {
    bool enabled = false;
    // **注視点までの距離をピント面にする。** 軌道カメラなので、見ているものが
    // 常に注視点にある。手で合わせ直す手間をなくすため既定でオン。
    bool focusOnTarget = true;
    // 手動のピント距離（メートル。ワールドの 1 単位を 1m とみなす）。
    float focusDistance = 3.2f;
    // 画面上のボケ半径の上限。現実の式のままだと極端なボケと負荷になるので、
    // 表示のための頭打ちとして持つ。
    float maxBlurPixels = 24.0f;
    // **ミニチュアの縮尺（1 : この値）。** 1 で実物大。
    //
    // 錯乱円は距離に反比例するので、実寸のまま 2km の地形を撮ると
    // ほぼ全部が無限遠の扱いになり、絞りを開けても 1 画素もボケない
    // （既定の 29mm・F16 でピント 2000m のとき、1000m の所で 0.0006 画素）。
    // ここを 1000 にすると「2km の地形を 2m の模型として撮る」計算になり、
    // 素材（2m 角）を撮ったときと同じくらいボケる。
    //
    // **UI の単位は現実のカメラのまま**にしてある（焦点距離 mm / F 値 / m）。
    // 縮めるのはシーンの距離だけで、レンズの側は触らない。
    float miniatureScale = 1.0f;
    // **ボケ量に掛ける誇張の倍率。** 1 で現実どおり。
    //
    // スケールの補正ではない。2m 角の地面を 3.2m から広角で撮れば、
    // **現実のカメラでも全域にピントが合う**（29mm・F1.8 で錯乱円が 1 画素に届かない）。
    // 物理的に正しくても素材の見せ方としては物足りないことがあるので、
    // 物理の関係を保ったまま量だけ持ち上げるための係数として持つ。
    // 倍率を使わずにボケを出したいなら、現実と同じく望遠へ寄せればよい
    // （100mm・F1.8 なら倍率 1 のまま半径 10 画素ほどになる）。
    float blurScale = 1.0f;
    ApertureShape shape = ApertureShape::Circle;
    // 多角形のボケの向き（度）。円のときは効かない。
    float rotationDegrees = 0.0f;
};

// シーンを HDR で描き、露出とトーンマップを通して表示用テクスチャへ書き出す。
// プレビュー設定の既定値。**メンバ初期化子・UI の既定値マーカー・
// プロジェクト読み込みのフォールバックの 3 か所で必ずこれを使う。**
// 数値を直接書くと、片方だけ変えたときに「既定値マーカーが点いたまま」
// 「読み込みで別の値に化ける」という食い違いが起きる。
struct PreviewDefaults {
    TonemapMode tonemap = TonemapMode::Aces;
    bool tessellationEnabled = false;
    float tessellationFactor = 8.0f;
    // 分割で 1 辺を保つ画面上の長さ（px）。小さいほど細かい。
    float tessellationTargetPixels = 10.0f;
    // 道路の材質（スロット 1〜4）を合成する解像度。タイル 1 枚ぶん。
    uint32_t materialResolution = 1024;
    bool showSkybox = true;
    bool skyboxBlur = false;
    bool shadowEnabled = true;
    uint32_t shadowResolution = 2048;
    uint32_t shadowCascadeCount = 4;
};
inline constexpr PreviewDefaults kPreviewDefaults{};

// メッシュシーンとは別に描くもの（配置したモデル）へ渡す、そのパスの描き方。
// 行列は MeshPbr と同じく**転置せずに**入れてある（シェーダは mul(M, v) で読む）。
struct SceneDrawContext {
    // 真ならシャドウカスケードの深度だけのパス（ピクセルシェーダ無し）。
    bool shadowPass = false;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT3 cameraPosition{};
    // 本描画でだけ使う照明と影。
    std::array<DirectX::XMFLOAT4X4, kShadowCascadeCount> lightViewProjections{};
    std::array<uint32_t, kShadowCascadeCount> shadowIndices{};
    std::array<float, kShadowCascadeCount> shadowSplits{};
    std::array<float, kShadowCascadeCount> shadowBiases{};
    float shadowTexelSize = 0.0f;
    float shadowBlend = 0.0f;
    float shadowNear = 0.0f;
    uint32_t shadowCascadeCount = 0;
    const Environment* environment = nullptr;
    float iblIntensity = 1.0f;
    DirectX::XMFLOAT3 lightDirection{};
    float lightIlluminance = 0.0f;
    DirectX::XMFLOAT3 lightColor{};
};

class PreviewRenderer {
public:
    // 作業グリッド（50m 角）を包む球の半径。シーンが無いときのカメラの基準。
    static constexpr float kReferenceGridRadius = 25.0f * 1.41421356f;

    bool Initialize(rhi::Device& device, rhi::PipelineCache& pipelineCache);
    void Shutdown(rhi::Device& device);

    // プロジェクトが持つ設定をすべて既定へ戻す。「新規」で使う。
    //
    // **対象は io::ReadPreview が読む項目と一致させること。**
    // 片方に足し忘れると、「新規にしたのに前のプロジェクトの値が残る」
    // （こちらの漏れ）か「開いても既定に戻らない」（あちらの漏れ）になる。
    // 天球は SkyLibrary が持つので、ここでは触らない。
    void ResetSettings();

    // フレームの外で生成結果（グラフの Mesh Output）を渡す。検証・転送が失敗したら現在のシーンを保つ。
    bool SetGeneratedMeshScene(rhi::Device& device, const MeshScene& scene);
    void ClearMeshScene(rhi::Device& device);
    bool HasMeshScene() const { return m_meshSceneEnabled; }
    const MeshScene& Scene() const { return m_meshScene; }
    // ビューポートで強調するメッシュ（Scene().meshes の添字。hovered は -1 で無し）。
    // ホバー中と選択中の外周を、トーンマップ後に深度を無視して重ねる。**毎フレーム呼んでよい。**
    void SetMeshHighlight(int hovered, const std::vector<int>& selected) {
        m_hoveredMesh = hovered;
        m_selectedMeshes = selected;
    }
    // 材質アセットや画像を編集したときに呼ぶ。シーンの材質を次のフレームで評価し直す
    // （グラフの構造は変わらないので、シーンを作り直す必要はない）。
    void InvalidateSceneMaterials();
    // シーンの材質のどれかが非同期で評価中か（UI の「評価中」表示と、開発用の撮影の待ちに使う）。
    bool IsEvaluating() const;

    // ビューポートに適用する天球を渡す。**毎フレーム呼んでよい。**
    // 前回と中身が違えば、必要な作り直し（環境マップの再生成か、
    // 較正倍率だけの掛け直し）を予約する。実際の生成はフレームの外で行う。
    void SetActiveSky(const SkyDefinition& sky);
    const SkyDefinition& ActiveSky() const { return m_activeSky; }
    // 環境マップやマテリアル解像度の作り直しは GPU 待機を伴うため、
    // フレームの外でまとめて処理する。
    void ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache);

    // 表示先のサイズに合わせてレンダーターゲットを作り直す。
    bool Resize(rhi::Device& device, uint32_t width, uint32_t height);

    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList,
                const compositor::TextureLibrary& textures,
                const compositor::MaterialLibrary& materials);

    Camera& GetCamera() { return m_camera; }
    const Camera& GetCamera() const { return m_camera; }
    ExposureSettings& Exposure() { return m_exposure; }
    // ライティングの方式。偽なら作業用IBL（天球と作業用のライト）、真ならシーンの空（大気散乱と太陽）。
    // どちらの設定も保持し、切り替えても失わない。
    bool& AtmosphericMode() { return m_atmosphericMode; }
    bool AtmosphericMode() const { return m_atmosphericMode; }
    // 今の方式で編集するライト（UI とギズモ）。シーンの空では太陽の方位・仰角・大気圏外照度（色は大気から決まる）。
    LightSettings& Light() { return m_atmosphericMode ? m_atmosphericLight : m_light; }
    LightSettings& WorkLight() { return m_light; }
    LightSettings& SceneSunLight() { return m_atmosphericLight; }
    const LightSettings& SceneSunLight() const { return m_atmosphericLight; }
    AtmosphereSettings& AtmosphericSettings() { return m_atmosphereSettings; }
    const AtmosphereSettings& AtmosphericSettings() const { return m_atmosphereSettings; }
    // 描画に使うライト。シーンの空では、大気を通った太陽の色と照度（地平線より下は 0）。
    LightSettings EffectiveLight() const;
    // シーンの空の環境光（IBL）の倍率。背景の明るさは変えない。
    static constexpr float kDefaultSkylightIntensity = 1.0f;
    float& SkylightIntensity() { return m_skylightIntensity; }
    // 環境光（IBL）の倍率。作業用IBLは天球の値、シーンの空はスカイライトの強さ。
    float EnvironmentIntensity() const { return m_atmosphericMode ? m_skylightIntensity : m_activeSky.iblIntensity; }
    // シーンを包む球の半径（原点中心）。カメラの Frame() が使う。シーンが無ければ作業グリッドの半径。
    // 配置したモデル（SetExtraSceneRadius）も含む。
    float BoundingRadius() const;
    // メッシュシーンの外で描くもの（配置したモデル）。本描画の不透明メッシュの直後と、
    // 各シャドウカスケードで呼ぶ。シェーディング表示（DebugView::Shaded / Clay 以外は呼ばない）でだけ描く。
    std::function<void(ID3D12GraphicsCommandList*, const SceneDrawContext&)> drawSceneExtras;
    // drawSceneExtras が描くものを包む球の半径（原点中心、m）。影の範囲とカメラの距離に使う。0 なら何も無い。
    // **毎フレーム渡してよい。**
    void SetExtraSceneRadius(float radius) { m_extraSceneRadius = radius; }
    // 重ねる線。**毎フレーム渡す**（渡さなければ前のフレームのまま）。
    void SetCrackGuides(std::vector<OverlayLineSet> guides) { m_crackGuides = std::move(guides); }
    void SetOverlayLines(std::vector<OverlayLineSet> lines) { m_overlayLines = std::move(lines); }
    TonemapMode& Tonemap() { return m_tonemap; }
    DebugView& Debug() { return m_debugView; }
    DebugView Debug() const { return m_debugView; }
    // 描画に使う環境。シーンの空で大気の環境ができていればそれ、ほかは作業用IBL。
    const Environment& GetEnvironment() const {
        return m_atmosphericMode && m_atmosphere.IsReady() ? m_atmosphere.GetEnvironment() : m_environment;
    }
    const Environment& WorkEnvironment() const { return m_environment; }
    bool& ShowSkybox() { return m_showSkybox; }
    // 背景だけをぼかす。**IBL の寄与は変えない。**
    // プリフィルタ済みキューブの粗いミップを引くだけなので、追加のパスは要らない。
    bool& SkyboxBlur() { return m_skyboxBlur; }
    // ディレクショナルライトの影を落とすか。落とさないとシャドウパスも走らない。
    bool& ShadowEnabled() { return m_shadowEnabled; }
    uint32_t ShadowCascadeCount() const { return m_requestedShadowCascadeCount; }
    void RequestShadowCascadeCount(uint32_t count) {
        // 1 はシーン全体を覆う 1 枚、2〜4 は視距離で分けるカスケード。0（不正値）は既定、5 以上は 4。
        m_requestedShadowCascadeCount =
            count == 0 ? kPreviewDefaults.shadowCascadeCount : std::min(count, kShadowCascadeCount);
    }
    uint32_t ShadowResolution() const { return m_requestedShadowResolution; }
    void RequestShadowResolution(uint32_t resolution) {
        m_requestedShadowResolution = (resolution == 1024 || resolution == 2048 || resolution == 4096)
            ? resolution : kPreviewDefaults.shadowResolution;
    }
    DofSettings& Dof() { return m_dof; }
    const DofSettings& Dof() const { return m_dof; }
    // 実際にピント面として使う距離。注視点に合わせる設定ならカメラの距離。
    float FocusDistance() const;
    // テセレーション（画面上の辺の長さに応じた分割）を使うか。
    bool& TessellationEnabled() { return m_tessellationEnabled; }
    // 1 辺あたりの分割の上限。
    float& TessellationFactor() { return m_tessellationFactor; }
    float& TessellationTargetPixels() { return m_tessellationTargetPixels; }
    bool& ShowUvChecker() { return m_showUvChecker; }
    bool& ShowReferenceGrid() { return m_showReferenceGrid; }
    // 直前のフレームの描画の量。
    void EnableDiagnostics(bool enabled) { m_diagnostics.SetEnabled(enabled); }
    const RenderStats& Stats() const { return m_stats; }
    // 道路の材質の合成解像度。作り直しは GPU 待機を伴うのでフレームの外で行う（`ProcessPendingWork`）。
    uint32_t MaterialResolution() const { return m_materialResolution; }
    void RequestMaterialResolution(uint32_t resolution) { m_requestedMaterialResolution = resolution; }

    // 表示用テクスチャを PNG に書き出す。フレームの外で呼ぶこと。
    // maxSize を指定すると縦横比を保ってその大きさ以下へ縮小する（シーンのサムネイル用）。
    bool SaveOutputToPng(rhi::Device& device, const std::filesystem::path& path, uint32_t maxSize = 0);

    bool HasOutput() const { return m_output.IsValid(); }
    // 同寸法・同形式のキャッシュへコピー。呼出側はフレーム外で宛先を確保する。
    bool CopyOutputTo(ID3D12GraphicsCommandList* commandList, rhi::GpuTexture& destination);
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    // 適用中の天球から環境マップを作り直す。HDRI の読み込みに失敗したら
    // 手続き的な空へ落とす（アセットの中身は書き換えない）。
    void ApplyActiveSky(rhi::Device& device, rhi::PipelineCache& pipelineCache);

    // ライトから見たビュー×投影。プレビューの被写体を囲む平行投影。
    void ReleaseTargets(rhi::Device& device);
    // 自動露出の測光。シーンカラーのヒストグラムから EV100 を求め、読み戻しバッファへ写す。
    void MeterExposure(rhi::Device& device, rhi::PipelineCache& pipelineCache, ID3D12GraphicsCommandList* commandList);
    // 前回この枠で記録した測光値を読み、順応の速さで平滑化して autoEv100 へ入れる。
    void ReadExposureMeter(rhi::Device& device);
    // 作業グリッドの線。トーンマップ後の表示用テクスチャへ、シーンの深度でテストして描く。
    void DrawGuideOverlay(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          ID3D12GraphicsCommandList* commandList);

    bool UploadMeshScene(rhi::Device& device, const MeshScene& scene);
    MeshScene m_meshScene;
    std::vector<Mesh> m_sceneMeshes;
    struct SceneMaterial {
        compositor::MaterialStack stack;
        std::unique_ptr<compositor::MaterialEvaluator> evaluator;
        // 道路のスロット 2〜4。道路空間マスク（RGBA8）で被覆する。
        std::array<compositor::MaterialStack, 3> layerStacks;
        std::array<std::unique_ptr<compositor::MaterialEvaluator>, 3> layerEvaluators;
        rhi::GpuTexture roadMask;
        rhi::GpuTexture boundaryControl;
    };
    std::vector<SceneMaterial> m_sceneMaterials;
    bool m_meshSceneEnabled = false;
    float m_meshSceneRadius = 0.1f;
    float m_extraSceneRadius = 0.0f;
    std::vector<OverlayLineSet> m_overlayLines;
    std::vector<OverlayLineSet> m_crackGuides;

    rhi::GpuTexture m_sceneColor;  // 線形 HDR
    // 被写界深度を掛けた結果。**トーンマップはこちらを読む。**
    // 元のシーンカラーを潰さないので、掛けるかどうかを毎フレーム選べる。
    rhi::GpuTexture m_sceneColorDof;
    rhi::GpuTexture m_depth;
    rhi::GpuTexture m_output;  // トーンマップ後の表示用
    // ディレクショナルライトから見た深度。ビューポートの大きさとは無関係に固定。
    std::array<rhi::GpuTexture, kShadowCascadeCount> m_shadowMaps;
    bool ResizeShadowMap(rhi::Device& device, uint32_t resolution, uint32_t count);
    uint32_t m_shadowCascadeCount = kPreviewDefaults.shadowCascadeCount;
    uint32_t m_requestedShadowCascadeCount = kPreviewDefaults.shadowCascadeCount;
    uint32_t m_shadowResolution = kPreviewDefaults.shadowResolution;
    uint32_t m_requestedShadowResolution = kPreviewDefaults.shadowResolution;

    Camera m_camera;
    ExposureSettings m_exposure;
    rhi::GpuBuffer m_meterHistogram, m_meterResult, m_meterReadback;
    bool m_meterPending[rhi::kFrameCount] = {};
    std::chrono::steady_clock::time_point m_meterTime{};
    LightSettings m_light;
    Environment m_environment;
    // シーンの空（大気散乱）。作業用IBL（m_environment / m_activeSky / m_light）とは別に持つ。
    bool m_atmosphericMode = false;
    AtmosphereSettings m_atmosphereSettings;
    LightSettings m_atmosphericLight = {AtmosphereSettings{}.azimuth, AtmosphereSettings{}.elevation,
                                        AtmosphereSettings{}.illuminance, {1.0f, 1.0f, 1.0f}};
    float m_skylightIntensity = kDefaultSkylightIntensity;
    Atmosphere m_atmosphere;
    // ビューポートに適用している天球の中身。**Environment の元になっているもの。**
    // 既定値は Environment::Initialize が作る環境と一致させてあるので、
    // 起動直後は作り直しが要らない。
    SkyDefinition m_activeSky;
    TonemapMode m_tonemap = kPreviewDefaults.tonemap;
    DebugView m_debugView = DebugView::Shaded;
    bool m_skyLuminanceRebuildRequested = false;
    uint32_t m_materialResolution = kPreviewDefaults.materialResolution;
    uint32_t m_requestedMaterialResolution = kPreviewDefaults.materialResolution;
    bool m_showSkybox = kPreviewDefaults.showSkybox;
    bool m_skyboxBlur = kPreviewDefaults.skyboxBlur;
    bool m_shadowEnabled = kPreviewDefaults.shadowEnabled;
    DofSettings m_dof;
    RenderStats m_stats;
    PreviewDiagnostics m_diagnostics;
    bool m_tessellationEnabled = kPreviewDefaults.tessellationEnabled;
    float m_tessellationFactor = kPreviewDefaults.tessellationFactor;
    float m_tessellationTargetPixels = kPreviewDefaults.tessellationTargetPixels;
    bool m_showReferenceGrid = true;
    bool m_showUvChecker = false;
    // シーンの材質とは別に保持し、保存データやベイクへ混ぜない。
    compositor::TextureLibrary m_previewTextures;
    compositor::TextureId m_uvCheckerTexture = compositor::kNoTexture;
    int m_hoveredMesh = -1;
    std::vector<int> m_selectedMeshes;
    bool m_skyRebuildRequested = false;
    // Environment がいま持っている HDRI。較正倍率だけを掛け直せるかの判断に使う。
    std::filesystem::path m_loadedHdriPath;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

}  // namespace rock::renderer
