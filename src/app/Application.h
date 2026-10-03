#pragma once
#include <future>
#include <limits>
#include <stop_token>

#include "compositor/MaterialLibrary.h"
#include "compositor/MaterialStack.h"
#include "compositor/TextureLibrary.h"
#include "core/Log.h"
#include "core/FrameLimiter.h"
#include "core/Window.h"
#include "graph/NodeGraph.h"
#include "graph/RockEvaluator.h"
#include "app/AssetThumbnailCache.h"
#include "app/UndoHistory.h"
#include "io/AssetRelations.h"
#include "io/ProjectWorkspace.h"
#include "io/RockAssetIo.h"
#include "io/AppSettings.h"
#include "io/RecentFiles.h"
#include "io/RockTemplates.h"
#include "renderer/MaterialSphere.h"
#include "renderer/OcclusionBake.h"
#include "renderer/ModelAsset.h"
#include "renderer/ModelPreview.h"
#include "renderer/PreviewRenderer.h"
#include "renderer/SkyLibrary.h"
#include "renderer/SkySphere.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"
#include "rhi/ShaderCompiler.h"
#include "ui/ImGuiLayer.h"
#include "ui/Toast.h"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// imgui-node-editor のコンテキスト。ヘッダを丸ごと引き込まないための前方宣言。
namespace ax::NodeEditor {
struct EditorContext;
}

namespace rock {

struct TextureChoices;  // ApplicationUiHelpers.h
struct MaterialFileChoices;  // ApplicationUiHelpers.h

// コマンドラインから渡せる起動オプション。
struct StartupOptions {
    // 起動時に読み込む HDRI。空なら手続き的な空を使う。
    std::filesystem::path hdriPath;
    // 起動時にテクスチャライブラリへ読み込む画像。--texture を繰り返し指定できる。
    std::vector<std::filesystem::path> texturePaths;
    // 起動時に開くシーン (.rockgraph) または旧プロジェクト (.reproj)。ルートのフォルダや
    // project.reproj を渡すとルートだけを開く。空なら新規シーンで始める。
    std::filesystem::path projectPath;
    // プロジェクトのルートフォルダ（--root）。空なら最近使ったルート、無ければ data/。
    std::filesystem::path projectRoot;
    // 削除確認画面のスクリーンショット検証用。削除そのものは実行しない。
    std::filesystem::path inspectAssetDelete;
    // 指定すると、数フレーム描いてからプロジェクトを保存して終了する。
    // 保存と読み込みを対話なしで確かめるための開発用オプション。
    std::filesystem::path saveProjectPath;
    // 開発用: 読み込んだシーンの先頭以外の全ノードをコピーし、このシーンを開いて貼る。
    // 別のファイルへの貼り付けを対話なしで確かめる（--save-project と組み合わせる）。
    std::filesystem::path testCopyTo;
    // 指定すると、数フレーム描いてからビューポートを PNG に書き出して終了する。
    // 画面キャプチャに頼らず描画結果を確認するための開発用オプション。
    std::filesystem::path screenshotPath;
    // 指定すると、ウィンドウ全体（UI 込み）を PNG に書き出して終了する。
    // 画面キャプチャは他ウィンドウを掴むことがあるため、確認にはこちらを使う。
    std::filesystem::path uiScreenshotPath;
    uint32_t screenshotFrame = 8;
    // 開発用。グラフの評価が終わるたびに、カメラを形全体が入るように引く（キーの A と同じ。グリッドは含めない）。
    // 撮影で被写体が画面からはみ出さないようにする（LLM が見た目を確かめるときなど）。
    bool frameAll = false;
    // 開発用。起動時に「テンプレートから作成」を開く / テンプレート（id）を未保存の文書として開く。
    bool openTemplates = false;
    std::string templateId;
    // 開発用。カメラの向き（度）。NaN なら変えない。撮影で正面・側面などを撮り分けるのに使う。
    // yaw は注視点のまわりの水平の角度、pitch は見下ろす角度（正で上から）。
    float cameraYawDegrees = std::numeric_limits<float>::quiet_NaN();
    float cameraPitchDegrees = std::numeric_limits<float>::quiet_NaN();
    // 開発用。注視点からの距離（m）と注視点（m）。NaN なら変えない。岩の内側にカメラを置いて内部の面を確かめるのに使う。
    float cameraDistance = std::numeric_limits<float>::quiet_NaN();
    float cameraTarget[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::quiet_NaN()};
    // 開発用。作業用ライトの方位（度）。NaN なら変えない。視点を回して撮るときに光も回し、裏側が影で潰れないようにする。
    float lightAzimuthDegrees = std::numeric_limits<float>::quiet_NaN();
    // 撮影用: 光の仰角（度）・照度（lux）・露出補正（EV。正で暗く、負で明るく）。NaN なら変えない。
    float lightElevationDegrees = std::numeric_limits<float>::quiet_NaN();
    float lightIlluminance = std::numeric_limits<float>::quiet_NaN();
    float exposureCompensation = std::numeric_limits<float>::quiet_NaN();
    float skylightIntensity = std::numeric_limits<float>::quiet_NaN();  // シーンの空の環境光（IBL）の倍率
    int lightingMode = -1;  // -1 変えない、0 作業用 IBL、1 シーンの空（大気）
    bool exposureApplied = false;  // 露出補正は一度だけ適用する（手動 EV に足すため）
    // 開発用。FBX をモデルとして読み込み、FBX のマテリアルからマテリアルを作ってプレビューを開く。
    std::filesystem::path importModel;
    // 開発用。ルート内のアセットをアセットの帯のダブルクリックと同じ経路で開く。
    std::filesystem::path openAsset;
    // 開発用。モデル（.model / .fbx）を原点へ置く（帯からビューポートへ落としたのと同じ経路）。
    std::filesystem::path placeModel;
    // 開発用。モデルのギズモを回転（E）で始める。
    bool gizmoRotate = false;
    bool gizmoScale = false;
    bool testGpuAo = false;
    int bakeNode = 0; // 開発用。通常のベイク実行と同じ処理を予約する。
    // 開発用。bakeNode のベイクが終わったら、結果をこのフォルダへ出力する（出力ボタンと同じ処理。ダイアログは出さない）。
    std::filesystem::path exportBakeDirectory;
    int exportBakeNode = 0;
    // --place-model で置いたモデルの FBX のノードに足す回転（--model-node-rotation <node> <x> <y> <z>）。
    std::vector<renderer::ModelNodeRotation> modelNodeRotations;
    // ノード用のギズモをオンにして、そのノードを選ぶ（--model-node-gizmo <node>）。
    std::string modelNodeGizmo;
    // 起動直後に前へ出すパネル（ドックのタブ）の名前（--focus-panel <name>）。撮影用。
    std::string focusPanel;
    bool measurePreview = false;
    // プロジェクト読込後に選択するノード。スクリーンショット検証用。
    graph::GraphId selectNode = 0;
    graph::GraphId previewNode = 0;
    // Rock Asset の LOD の出し方（--rock-asset-lod <n>。-1 は自動）と表示モード（--view <番号>）。撮影用。
    int rockAssetLod = -2;
    int debugView = -1;
    // 読み込み後にこの Rock Asset を焼く（--bake-asset <node>）。検証用。
    graph::GraphId bakeAssetNode = 0;
    // Rock Asset の段をフレームごとに切り替え、SyncMeshGraph にかかった時間をログへ出す（--test-lod-switch）。検証用。
    bool testLodSwitch = false;
    bool testDrag = false;
    bool testLayerThumbnailCache = false;
    bool testDragShift = false;
    int testViewportGesture = 0;
    bool testDragCancel = false;
    bool testDoubleClick = false;
    bool testDelete = false;
    ImVec2 testDragStart{};
    ImVec2 testDragEnd{};
    bool testClickAfterDrag = false;
    ImVec2 testClickPosition{};
};

// アプリ本体。ウィンドウ、デバイス、UI の生存期間とフレームループを持つ。
class Application {
public:
    bool Initialize(const StartupOptions& options);
    void Shutdown();
    int Run();

private:
    void PollShaderHotReload();
    // F12 で撮ったスクリーンショットの書き出しを要求する。撮れたら通知を出す。
    void RequestScreenshot();
    void DrawUi();
    // 既定のドックレイアウトを組む。ini に配置が無いときと、明示的な要求で呼ぶ。
    void BuildDefaultLayout(ImGuiID dockspaceId);
    void DrawViewportPanel();

    void DrawMaterialPanel();
    void DrawLightingPanel();
    // 実行状況の情報ウィンドウ（ウィンドウ > 情報）。常設ドックには置かない。
    void DrawInfoWindow();
    // ノードグラフパネル。Surface / Model などを繋ぎ、
    // Mesh Output へ届いた鎖をメッシュにしてビューポートに出す。
    void DrawGraphPanel();
    // エディタのコンテキストを破棄する。Shutdown から呼ぶ。
    void DestroyGraphEditor();
    // グラフのノード位置をエディタへ流し込み直す（読み込み・リセット・アンドゥの後）。
    // navigate が真なら、流し込み後に全体を画面へ収める。
    // アンドゥでは偽にする（戻すたびに視点が飛ぶと編集にならない）。
    void RequestGraphNodePlacement(bool navigate = true);
    // グラフのエディタ部（imgui-node-editor）。パネルの中で呼ぶ。
    void DrawGraphEditor();
    // ノードのメモの先頭を、ノードの上に吹き出しで描く。ed::Begin と ed::End の間で呼ぶ。
    void DrawGraphNodeNotes();
    // グラフのノード 1 枚。カード・ピン・リンクの当たり判定を描く。
    void DrawGraphNode(const graph::Node& node);
    // nodeId は Surface ノード（未読み込みのマテリアルを選んだとき、読み込み後に割り当てる先）。
    bool DrawLayerSettings(compositor::MaterialLayer& layer, graph::GraphId nodeId);
    // マテリアルのコンボに出す未読み込みのマテリアル。選ぶと読み込みを予約し、読めたら assign で割り当てる。
    MaterialFileChoices MaterialFilesForUi(std::function<void(compositor::MaterialAssetId)> assign);
    void ProcessPendingMaterialLoads();
    // グラフの変更をメッシュシーンへ反映する。フレームの頭（フレームの外）で呼ぶ。
    void SyncMeshGraph();
    void DrawPieceSettings(graph::Node&);
    void CommitPieceViewportSelection();
    bool m_pieceSelectionEditing = false;
    int m_pieceSelectView = 0; // 0選択、1削除後。表示専用。
    float m_pieceSelectSpread = 0;
    renderer::OverlayLineSet m_pieceSelectEdges, m_pieceSelectRemovedEdges;
    bool m_pieceUpdating = false;
    uint64_t m_pieceEpoch = 1;
    std::string m_pieceTaskKey, m_pieceTaskGeometryKey, m_pieceCompletedKey;
    struct PieceTaskResult {
        graph::RockEvaluation output, input, selection;
        graph::RockEvaluationCache cache;
    };
    std::future<PieceTaskResult> m_pieceTask;
    std::stop_source m_pieceStop;
    // 評価スレッドが書き、UI が読む。いま計算しているノードと段階。
    std::shared_ptr<graph::RockEvaluationProgress> m_pieceProgress;
    std::chrono::steady_clock::time_point m_pieceTaskStart{};
    // 岩グラフの評価の開始時刻と、直近の評価にかかった時間（評価の開始から結果をビューポートへ渡し終えるまで、ms）。
    // ステータスバーの右端に出す。まだ評価していなければ負。
    std::chrono::steady_clock::time_point m_evaluationStart{};
    double m_lastEvaluationMs = -1;
    // 計算中のノード（無ければ 0）と、「UV Unwrap: 島を配置中 42%・12秒」のような表示文。
    graph::GraphId EvaluatingNode() const;
    std::string EvaluationProgressText() const;
    std::shared_ptr<const geometry::PieceCollection> m_pieceInput, m_piecePreview;
    // Piece Select の設定欄の集計。選別は重いので、入力か設定が変わったときだけ計算し直す。
    struct PieceSelectSummary {
        std::shared_ptr<const geometry::PieceCollection> input;
        geometry::PieceSelectSettings settings;
        geometry::PieceSelection evaluated;
        std::vector<uint32_t> chosenIds, candidateIds;  // 並べ替え済み
        std::string error;
    };
    std::optional<PieceSelectSummary> m_pieceSelectSummary;
    // Scatter Points をプレビューしているときの点。ビューポートに 2D の点で重ねる。
    std::shared_ptr<const geometry::PointSet> m_pointPreview;
    graph::GraphId m_pieceInputNode = 0;
    int m_pieceGizmoId = -1;
    std::shared_ptr<const geometry::PieceSelection> m_pieceTransformSelection;

    void DrawUvPanel();
    // bakeGeometry を渡すと、Material Bake の結果がまだ使えるかをそのメッシュで照らす（Rock Asset の LOD は形が違うため）。
    // 戻り値は、Material Bake の結果（まだ使えるもの）を貼ったか。inputFingerprint を渡すと、
    // いまの入力でベイクしたときの指紋（ベイクしたかどうかに依らない）を入れる。Material Bake が無ければ空。
    bool ApplyRockMaterial(renderer::SceneMesh& mesh, const graph::GeneratedRock& rock, bool useBaked,
                           const renderer::MeshData* bakeGeometry = nullptr, std::string* inputFingerprint = nullptr);
    // detail は Material Bake の High（ハイポリ）の内容のハッシュ（GeneratedRock::bakeDetail）。0 は未接続。
    std::string BakeFingerprint(const renderer::SceneMesh& mesh, const geometry::Mesh& input, graph::GraphId bakeNode = 0,
                                uint64_t detail = 0) const;
    void ProcessPendingBake();
    void PrepareMaterialHeights();
    std::shared_ptr<const graph::MaterialHeight> m_materialHeights;
    std::string m_materialHeightKey;
    bool ValidateGpuAo();
    void FinishBake(graph::GraphId id, std::array<LdrImage, 4>& images, const std::string& fingerprint);
    // Material Bake の High（ハイポリ）から法線とハイトを転写し、素材を焼いた画像に重ねる。mesh はローポリの描画用データ。
    bool TransferHighDetail(const graph::Node& node, const graph::GeneratedRock& rock, const renderer::SceneMesh& mesh,
                            const graph::MaterialBakeSettings& settings, std::array<LdrImage, 4>& images, std::string& error);
    struct BakeJob {
        graph::GraphId id = 0;
        uint64_t epoch = 0, revision = 0;
        std::filesystem::path root;
        std::string fingerprint;
        std::array<LdrImage, 4> images;
        renderer::OcclusionBake ao;
        bool cancel = false;
    };
    std::optional<BakeJob> m_bakeJob;
    graph::GraphId m_pendingBake = 0;
    std::unordered_map<graph::GraphId,std::string> m_bakeStatus;
    // ベイク結果の画像（BaseColor / Normal / RoughnessMetallicAO / Height）。「テクスチャを出力…」で書き出す元。
    // ファイルへは自動で保存しない。プロジェクトを開き直すと消える。
    std::unordered_map<graph::GraphId, std::array<LdrImage, 4>> m_bakeImages;
    // Shape Mask の画像を載せた一時テクスチャ。評価結果の画像（共有）ごとに1枚。使われなくなったら SyncMeshGraph が捨てる。
    struct ShapeMaskTexture {
        std::shared_ptr<const geometry::MaskImage> image;
        compositor::TextureId texture = compositor::kNoTexture;
        bool used = false;
    };
    std::vector<ShapeMaskTexture> m_shapeMaskTextures;
    compositor::TextureId ShapeMaskTextureFor(const std::shared_ptr<const geometry::MaskImage>& image);
    // Rock Asset の自分の UV を持つ段のテクスチャ。LOD0 の Material Bake の結果から転写する。
    // 一時テクスチャと材質を持ち、使われなくなったら SyncMeshGraph が捨てる。
    struct LodTextureSet {
        std::string key;  // 転写元（ベイクの指紋と LOD0）とこの段のメッシュのハッシュ
        std::array<LdrImage, 4> images;
        std::array<compositor::TextureId, 4> textures{};
        compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
        bool used = false;
    };
    std::vector<LodTextureSet> m_lodTextures;
    // rock の level 段（自分の UV を持つ段）のテクスチャ。Material Bake の結果がまだ使えるときだけ作る。
    // 作れなければ nullptr（error に理由。ベイクが無いだけなら空）。
    const LodTextureSet* RockLodTextures(const graph::GeneratedRock& rock, size_t level, std::string& error);
    // 焼いた画像 4 枚の背景を埋め、一時テクスチャと材質を作る（Material Bake と同じ作り方）。失敗したら kNoMaterialAsset。
    // name はテクスチャの名前の頭、materialName は材質の名前（どちらも末尾に「（一時）」を付ける）。
    compositor::MaterialAssetId MakeBakedMaterial(const std::string& name, const std::string& materialName,
                                                  std::array<LdrImage, 4>& images,
                                                  std::array<compositor::TextureId, 4>& textures);
    // フォルダを選んで、ベイク結果の PNG を書き出す。
    // directory が空ならフォルダを選ぶダイアログを出す。
    void ExportBakedTextures(graph::GraphId id, std::filesystem::path directory = {});
    geometry::Mesh m_uvPreviewMesh;
    float m_uvZoom = 1.0f;
    ImVec2 m_uvPan{};
    bool m_uvCheckerPreview = false;
    graph::GraphId m_uvLastSelectedNode = 0;
    graph::GraphId m_uvPreviousPreviewNode = 0, m_uvPreviousPreviewPin = 0;
    graph::RockEvaluationCache m_rockEvaluationCache;
    uint64_t m_meshGraphRevision = 0;
    // 法線を頂点に焼くので、表示設定の切り替えでもメッシュを作り直す。
    bool m_meshGraphSmoothShading = false;
    float m_meshGraphSmoothShadingAngle = 60.0f;
    geometry::VolumeMeshingMethod m_meshGraphSdfPreviewMethod = geometry::VolumeMeshingMethod::MarchingTetrahedra;
    // 直近にメッシュシーンへ出した「途中のメッシュノード」。0 なら Mesh Output の鎖。
    graph::GraphId m_meshGraphPreviewNode = 0;
    bool m_meshGraphActive = false;
    std::string m_meshGraphError;
    struct RockMeshReference {
        graph::GraphId source = 0;
        int pieceId = -1;
    };
    std::vector<RockMeshReference> m_rockMeshReferences;
    // m_rockMeshReferences と同じ並びの三角形数。ノードの設定欄に出力の規模を出す。
    std::vector<size_t> m_rockTriangleCounts;
    // Rock Asset のプレビュー。選んでいる間、ビューポートで LOD を切り替えて見る。
    struct RockAssetView {
        graph::GraphId node = 0;             // いま LOD を出している Rock Asset（無ければ 0）
        std::vector<size_t> triangles;       // 段ごとの三角形数（全メッシュの合計）
        DirectX::XMFLOAT3 center{};          // LOD0 を包む球
        float radius = 0.0f;
        int shown = 0;                       // いま出している段
        // いまの結果を焼いたときのハッシュ（io::RockAssetHash）。焼いた岩アセットの目録と比べて古さを判定する。
        std::string hash;
    } m_rockAssetView;
    // Rock Asset の段をメッシュごとに 1 つへまとめる（切り替えの大きさは設定から）。
    static std::vector<io::RockAssetLod> MergeRockAssetLods(const graph::RockEvaluation& evaluated,
                                                            const graph::RockAssetSettings& settings);
    // 「岩アセットを焼く」を押した Rock Asset。上流の Material Bake が古ければ先にベイクし、終わってから焼く。
    graph::GraphId m_pendingAssetBake = 0;
    bool m_assetBakeRequestedMaterial = false;
    // Material Bake を先に焼く前に見ていたプレビュー（焼き終えたら戻す）。
    graph::GraphId m_assetBakePreviewNode = 0, m_assetBakePreviewPin = 0;
    std::string m_assetBakeStatus;
    void ProcessPendingAssetBake();
    // 焼いた岩アセットの目録のハッシュ（シーンのパスと目録の更新時刻で読み直す）。
    struct BakedAssetCache {
        std::filesystem::path scene;
        std::filesystem::file_time_type time{};
        bool exists = false;
        std::string hash;
    } m_bakedAssetCache;
    const BakedAssetCache& BakedAsset();
    // -1 は自動（画面上の大きさで選ぶ）、0 以上はその段に固定。
    int m_rockAssetLodMode = -1;
    // LOD だけを切り替えるときに評価し直さないよう、Rock Asset を出している間だけ直近の評価結果を持つ。
    std::optional<graph::RockEvaluation> m_rockAssetEvaluation;
    // 岩を包む球の直径が画面の高さに占める割合（UE の Screen Size）。
    float RockAssetScreenSize() const;
    // いま出すべき段。previewNode が LOD を出している Rock Asset でなければ 0。
    int RockAssetWantedLod(graph::GraphId previewNode) const;
    // ビューポート左上の LOD の切り替え（Rock Asset を出しているときだけ）。
    void DrawRockAssetLodControls();
    // Rock Asset の出している段だけを変える（シーンは作り直さない）。統計と当たり判定の形も揃える。
    void ShowRockAssetLod(int lod);
    // 選択中のノードを控える / 貼り付ける（Ctrl+C / Ctrl+V）。
    void CopySelectedGraphNodes();
    // 控えたノードを貼る。viewCenter は今のキャンバスの中央（キャンバス座標）で、
    // 貼った集合の中心をそこへ置く。相対の配置は保つ。
    // 別の文書でコピーしたものは、参照するアセットを読み込む必要があるのでフレームの外へ回す
    // （m_pendingGraphPaste → ProcessPendingFileWork）。
    void PasteGraphNodes(const ImVec2& viewCenter);
    // 控えたノードを今の文書向けに直す。集合の外へのつながりを外し、マテリアル・テクスチャ・
    // モデルの番号を控えたファイルから引き直す（無ければ読み込み、見つからなければ「なし」）。
    // 読み込みを伴うのでフレームの外で呼ぶ。
    void AdoptGraphClipboard();
    // ビューポートに出すノードを決める。メッシュノード以外や無効な ID は
    // 「Mesh Output の鎖」（0）に落とす。
    // outputPin は**どの出力を見るか**。0 なら最初の出力。
    void SetPreviewGraphNode(graph::GraphId nodeId, graph::GraphId outputPin = 0);
    void DrawMaterialLibraryPanel();
    // 一覧の右クリックメニュー（追加 / 複製 / 削除 / 読み込み / 書き出し）。
    // target が kNoMaterialAsset なら、対象の要る項目は出さない。
    void DrawMaterialContextMenu(compositor::MaterialAssetId target);
    // マテリアル 1 つのプロパティ（基本 + マップ）。変更があれば真を返す。
    // **置き場所はプレビューの窓だけ**（一覧はサムネイルだけを出す）。
    bool DrawLayerMaterialProperties(compositor::MaterialAsset& asset);
    bool DrawMaterialProperties(compositor::MaterialAsset& asset);
    // マテリアルプレビューの窓（回せる球 + プロパティ）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawMaterialSphereWindow();
    // --- モデル（ApplicationModelPanel.cpp） --------------------------------------
    // モデルプレビューの窓（回せるモデル + 寸法・LOD・マテリアルスロット）。
    // アセットの帯でモデル（.model / .fbx）をダブルクリックするか、ウィンドウメニューから開く。
    void DrawModelPreviewWindow();
    // FBX の取り込み、スロットのマテリアル作成、シーンから外す、GPU メッシュの用意。フレームの外で呼ぶ。
    void ProcessModelWork();
    // 窓に出しているモデルと、まだ描いていないサムネイルを描く。フレームの中で呼ぶ。
    void RenderModelPreviews(ID3D12GraphicsCommandList* commandList);
    // 未割り当てのスロットに、FBX のマテリアル（拡散色のテクスチャと色）からマテリアルを作って割り当てる。
    void CreateModelMaterials(uint64_t modelId);
    // FBX をモデルとしてシーンへ足す（ルート外なら表示中のフォルダへ取り込む）。読み込み済みならそれを返す。
    // 失敗したら 0。フレームの外で呼ぶ。
    uint64_t ImportModelFile(const std::filesystem::path& inputPath);
    renderer::ModelAsset* FindModel(uint64_t id);

    // --- モデルの系統のノード（ApplicationModelPlacement.cpp） --------------------------
    // Model ノードを作ってモデルを position（底面の中心）へ置き、Mesh Output へ繋いで選ぶ
    // （既に別のものが繋がっていれば Merge でまとめる）。作ったノードの ID を返す（失敗は 0）。
    graph::GraphId PlaceModel(uint64_t modelId, const DirectX::XMFLOAT3& position);
    // ビューポートに出すモデル 1 つぶん（Model ノード、通る Transform、ワールド行列）。
    struct VisibleModel {
        graph::GraphId node = 0;
        std::vector<graph::GraphId> transforms;
        const renderer::ModelAsset* model = nullptr;
        DirectX::XMFLOAT4X4 world{};
        // Model ノードの設定のノードの回転（無ければ読んだままの姿勢）。
        const std::vector<renderer::ModelNodeRotation>* rotations = nullptr;
    };
    std::vector<VisibleModel> CollectVisibleModels() const;
    // Model / Transform ノードの位置・回転・倍率。どちらでもなければ偽。
    struct NodeTransformRef {
        float* position = nullptr;
        float* rotation = nullptr;
        float* scale = nullptr;
        // 形そのものを作り直す設定か（Volume Transform）。真ならグラフを改版して再評価する。
        bool regenerate = false;
        float* scaleXYZ = nullptr;
    };
    bool NodeTransform(graph::GraphId nodeId, NodeTransformRef& out);
    // 選んでいる Model / Transform / Volume Transform ノードのギズモの基準。
    // pivot はそのノードの原点のワールド位置、parent は下流の Transform をまとめた行列。
    // ビューポートに出ていなければ偽。
    bool NodeGizmoFrame(graph::GraphId nodeId, DirectX::XMFLOAT3& pivot, DirectX::XMFLOAT4X4& parent) const;
    // 表示中の岩メッシュのどれかが、このノードを上流に持つか（Volume Transform のギズモの表示条件）。
    bool RockMeshUsesNode(graph::GraphId nodeId) const;
    // レンダラの drawSceneExtras から呼ぶ。ビューポートに出すモデルを本描画・シャドウパスへ描く。
    void DrawSceneModels(ID3D12GraphicsCommandList* commandList, const renderer::SceneDrawContext& context);

    // --- 山グラフの岩（ApplicationRocks.cpp） -------------------------------------
    // 読んだ岩アセット（岩グラフの付属フォルダ）。岩グラフのパスごと。目録が更新されたら読み直す。
    struct LoadedRockAsset {
        std::filesystem::file_time_type time{};
        bool exists = false, loaded = false, textured = false;
        renderer::ModelAsset model;                   // 形（全段）と材質（一時）
        std::unique_ptr<renderer::ModelPreview> gpu;  // 全段の GPU メッシュとインスタンス描画
        std::vector<float> screenSizes;               // 段ごとの切り替えの大きさ
        std::vector<size_t> triangles;                // 段ごとの三角形数
        std::vector<compositor::TextureId> textures;  // 一時のテクスチャ（読み直すときに捨てる）
        float radius = 0.0f, height = 0.0f;           // LOD0 を包む球の半径と高さ（倍率 1）
        std::string error;
    };
    std::map<std::string, LoadedRockAsset> m_rockAssets;
    // 評価で得た、撒いた岩（岩グラフごと）。
    std::vector<graph::RockInstanceSet> m_rockInstanceSets;
    // このフレームの描画。岩グラフごとに、段ごとのまとまり（行列はバッファの中）。
    struct RockDraw {
        std::string scene;
        std::vector<renderer::ModelInstanceBatch> batches;
    };
    std::vector<RockDraw> m_rockDraws;
    std::array<rhi::GpuBuffer, rhi::kFrameCount> m_rockInstanceBuffers;
    std::array<size_t, rhi::kFrameCount> m_rockInstanceCapacity{};
    uint32_t m_rockInstanceBufferSrv = 0;
    uint64_t m_rockInstanceFrame = UINT64_MAX;
    // 撒いた岩を包む球の半径（原点から）。レンダラの「追加で描くもの」の範囲に使う。
    float m_rockInstancesRadius = 0.0f;
    struct RockInstanceStats {
        size_t drawn = 0, culled = 0;
        std::vector<size_t> perLod;
    } m_rockInstanceStats;
    void ReleaseRockAssets();
    LoadedRockAsset* RockAssetFor(const std::string& scene);
    LoadedRockAsset* LoadPlantAsset(const std::string& scene, const std::filesystem::path& path, LoadedRockAsset& asset);
    void SyncRockInstances(const std::vector<graph::RockInstanceSet>& sets);
    void BuildRockInstanceBatches();
    void DrawRockInstances(ID3D12GraphicsCommandList* commandList, const renderer::SceneDrawContext& context);
    // ビューポートに出すモデルを包む球の半径（原点中心）。無ければ 0。
    float ModelInstancesRadius() const;
    // カーソル直下のモデルの Model ノード（と距離）。無ければ 0。modelNode を渡すと、当たった部品の FBX のノードの番号も返す。
    graph::GraphId PickModelNode(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction, float& distance,
                                 int* modelNode = nullptr) const;
    // ノード用のギズモ（選んでいる Model ノードの、m_selectedModelNodeName の FBX のノードを回す）の基準。
    // origin はノードの原点のワールド位置、axes は回転の軸（モデルの軸を親の回転と置き方に合わせたもの。ワールド）。
    // ノード用のギズモを出さない状態なら偽。
    struct ModelNodeGizmo {
        graph::ModelNodeSettings* settings = nullptr;
        const VisibleModel* visible = nullptr;
        size_t node = 0;
        DirectX::XMFLOAT3 origin{};
        DirectX::XMFLOAT3 axes[3]{};
    };
    bool ModelNodeGizmoFrame(const std::vector<VisibleModel>& visible, ModelNodeGizmo& out);
    // カーソル位置の地面（メッシュ、無ければ高さ planeY の水平面）。当たらなければ偽。
    bool PickGround(const ImVec2& mouse, const ImVec2& viewportMin, const ImVec2& viewportMax, float planeY,
                    bool useMeshes, DirectX::XMFLOAT3& point) const;
    // モデルのホバー・クリック選択（Model ノードを選ぶ）・ギズモ（W 移動 / E 回転）・
    // 本体のドラッグ（水平移動）・Delete。この入力を使ったら真（メッシュの選択へ渡さない）。
    bool HandleModelInstanceInput(bool itemActive, bool itemHovered, const ImVec2& viewportMin, const ImVec2& viewportMax);
    // 選んでいる Model / Transform ノードのギズモを ImGui で重ね、範囲の枠をレンダラの深度付きの線で出す。
    void DrawModelInstanceOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // プレビュー中の点（m_pointPreview）を画面上の丸で重ねる。奥行きでは隠さない。
    void DrawPointPreview(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // アセットの帯のモデルをビューポートへ落としたときの受け口。
    void ModelDropTarget(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // Model / Transform ノードの設定（グラフパネルのプロパティ欄）。変えたら真。
    bool DrawModelNodeSettings(graph::Node& node);
    bool SelectedModelInstanceFocusTarget(DirectX::XMFLOAT3& target);
    // 天球パネル。一覧で選んだものがそのままビューポートの環境になる。
    void DrawSkyLibraryPanel();
    // 天球一覧の右クリックメニュー（追加 / 複製 / 削除）。
    // target が kNoSkyAsset なら、対象の要る項目は出さない。
    void DrawSkyContextMenu(renderer::SkyAssetId target);
    // 天球プレビューの窓（大きい絵 + 設定）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawSkyPreviewWindow();
    void DrawTextureLibraryPanel();
    // アセットの帯（ルートのフォルダ階層とフォルダの中身）。ApplicationAssetBrowser.cpp。
    void DrawAssetBrowser();
    void RefreshAssetBrowser();
    void ProcessAssetWork();
    void DrawSceneSwitchDialog();
    void DrawAssetDeleteDialog();
    // 「テンプレートから作成」。岩のテンプレートを選び、未保存の文書として開く（ファイルは作らない）。
    // 保存すると保存先を聞き、directory（空ならルート）とテンプレート名を初期値にする。ApplicationTemplates.cpp。
    void OpenTemplateWindow(const std::filesystem::path& directory);
    void DrawTemplateWindow();
    void ProcessTemplateWork();
    void DestroyTemplateThumbnails();
    void OpenTemplate(const io::RockTemplate& source);
    // 現在のシーンがそのファイルを使っているか（削除の可否）。
    bool IsAssetLoaded(const std::filesystem::path& path) const;
    void ResumeSceneSwitch();
    // 保存したシーンのプレビュー画像（ビューポートの縮小）を .rock-editor/scene-thumbnails へ残す。
    void SaveSceneThumbnail(const std::filesystem::path& path);
    // テクスチャ一覧の右クリックメニュー（読み込む / 削除）。
    // target が kNoTexture なら、対象の要る項目は出さない。
    void DrawTextureContextMenu(compositor::TextureId target);
    // 削除の確認モーダルを開く。参照が無くても必ず通す。
    void RequestTextureRemove(compositor::TextureId id);
    // 参照を外してからライブラリから消す（フレームの外で呼ぶ）。
    void RemoveTextureNow(compositor::TextureId id);
    // ファイルの無いテクスチャを消す。referenced が偽なら、どこからも使われていないものだけ。消した数を返す。
    size_t PruneMissingTextures(bool referenced);
    bool m_pendingMissingTexturePrune = false;
    // --- リンク切れの解消 ---------------------------------------------------
    // ファイルを選ぶダイアログを出し、選ばれたら再リンクを予約する
    // （読み込みは GPU 待機を伴うのでフレームの外で行う）。
    void RequestTextureRelink(compositor::TextureId id);
    // フォルダを選び、そこにあるリンク切れのファイル名をまとめて繋ぎ直す予約をする。
    // 素材のフォルダごと移した（別の PC で開いた）ときの入口。
    void RequestTextureRelinkFolder();
    // 予約した再リンクを処理する。繋ぎ直せたら、参照しているサムネイルと合成を作り直す。
    void ProcessPendingTextureRelinks();
    // テクスチャのコンボに渡す候補（読み込み済み + ルート内の未読み込みの画像）。
    TextureChoices TextureChoicesForUi();
    // 未読み込みの画像をその場で割り当てられるよう、リンク切れとして登録して読み込みを予約する。
    compositor::TextureId RequestTextureLoad(const std::filesystem::path& path);
    // マテリアルが参照しているテクスチャのどれかがリンク切れか。一覧の目印に使う。
    bool MaterialHasMissingTexture(const compositor::MaterialAsset& asset) const;
    // テクスチャプレビューの窓（拡大表示 + 詳細）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawTexturePreviewWindow();
    // Material Bake の結果を、チャンネルごとのタイルで並べる。ベイク済みで、指紋が現在の入力と合うときだけ「最新」。
    void DrawBakedTextureTiles(const graph::MaterialBakeSettings& bake);
    // アプリの設定ウィンドウ（ウィンドウ > 設定）。プロジェクトに保存しない設定を置く。
    void DrawSettingsWindow();
    // 開発用オプション（スクリーンショット / 保存）で動いているか。
    // 真のときはフレームレートを落とさない。
    bool Headless() const;
    // 設定から決まる UI の拡大率。追従なら Windows の表示スケール。
    // 起動（Initialize）からの経過時間。起動の速さをログへ出すため。
    float ElapsedSinceStartMs() const;
    float DesiredUiScale() const;
    // 拡大率を掛けた既定のクライアント領域。1920x1080 を拡大率倍したもの。
    // 追従を入れたときに作業面積（論理サイズ）が変わらないようにするため。
    uint32_t DefaultClientWidth() const;
    uint32_t DefaultClientHeight() const;
    // 設定に合わせて拡大率とウィンドウの大きさを反映する。フレームの外で呼ぶこと。
    void ApplyUiScale();
    // ファイルメニュー。要求を積むだけで、読み書きはフレームの外で行う。
    void DrawFileMenu();
    // キーボードショートカット（Ctrl+N / O / S / Shift+S）。メニューと同じ入口を通す。
    void HandleShortcuts();
    void RequestOpenProject();
    // 「最近使ったプロジェクト」。開く要求を積むだけ。
    void DrawRecentMenu();
    // saveAs が偽でも、まだ保存先が決まっていなければダイアログを出す。
    void RequestSaveProject(bool saveAs);
    // 画面下端のステータスバー。直近の通知と、いま何を持っているかを出す。
    // ドックスペースより前に呼ぶこと（作業領域をバーのぶん狭める）。
    void DrawStatusBar();
    // ログをステータスバーへ流す。Initialize で SetLogSink に登録する。
    void PushStatus(LogLevel level, const char* text);
    // エクスプローラから落とされたファイルを、拡張子で行き先へ振り分ける。
    void HandleDroppedFiles(const std::vector<std::filesystem::path>& paths);
    // プロジェクトとマテリアルの読み書き、テクスチャの追加と削除。
    // どれも GPU 待機を伴うため、フレームの外（Run のフレーム前）で呼ぶ。
    void ProcessPendingFileWork();
    // 中身を空にして作り直す。プロジェクトを開く前と「新規」で使う。
    void ResetProject();
    // ウィンドウタイトルを「プロジェクト名 - Rock Editor」に揃える。
    void UpdateWindowTitle();
    // このテクスチャを使っている場所の一覧（削除の確認に出す）。
    std::vector<std::string> CollectTextureUsers(compositor::TextureId id) const;
    // 参照している箇所の数だけを数える。毎フレーム呼ぶので文字列は作らない。
    size_t CountTextureUsers(compositor::TextureId id) const;
    // 参照が残っているテクスチャを消そうとしたときの確認。
    void DrawTextureRemoveModal();
    // --- アンドゥ -----------------------------------------------------------
    // 対象はグラフ（ノード / リンク / 設定 / 位置）とマテリアル。
    // テクスチャの読み込みと削除、プレビュー設定は含めない
    // （前者 2 つは GPU リソースそのもの）。
    // ノードの移動だけでは段を積まない（位置は他の変更の段に相乗りする）。
    //
    // いまの文書を写し取る。
    DocumentSnapshot CaptureDocument() const;
    // 写し取った文書を書き戻す。**マテリアルの破棄を伴うのでフレームの外で呼ぶ。**
    void ApplyDocument(const DocumentSnapshot& snapshot);
    // レイヤーかマテリアルを変えたときに呼ぶ。フレームの終わりに 1 段積まれる。
    void MarkDocumentChanged();
    // 存在しないテクスチャ ID を「なし」に落とす。
    // テクスチャは履歴の外で消えるため、書き戻した参照が宙に浮くことがある。
    compositor::TextureId ValidTexture(compositor::TextureId id) const;
    // ビューポートに重ねる操作（表示モードと、重ねる情報の切り替え）。
    // 画像の描画より後に呼ぶ。右上には FPS を出すので、右端の座標も渡す。
    void DrawViewportOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // ビューポート上の L + 左ドラッグでライトの向きを変える。
    // 掴んでいる間は true を返す（軌道やパス編集へ渡さない）。
    struct LightInteraction {
        bool dragging = false;
        double gizmoUntil = 0.0;
    };
    // メッシュのホバーと選択（Scene().meshes の添字。hovered は -1 で無し）。
    // レンダラが外周を重ね描きし、F キーのフォーカス先になる。シーンを作り直したら選択は消える。
    // 空からのドラッグは画面上の矩形で複数選択。Shift で追加、Esc で開始前へ戻す。
    struct MeshHighlightState {
        int hovered = -1;
        std::vector<int> selected;
        bool boxPending = false;
        bool boxSelecting = false;
        bool boxAdditive = false;
        ImVec2 boxStart{};
        ImVec2 boxEnd{};
        std::vector<int> boxPrevious;
    };
    MeshHighlightState m_meshHighlight;
    // Plane Cuts を選んでいる間の表示。ノードの設定ではない（評価・Undo・保存に関わらない）。
    bool m_planeCutsShowFrames = true;
    bool m_parallelPlanesShowFrames = true;
    bool m_planeCutsColorFaces = false;
    // 表示中の岩メッシュ（位置と面だけ）。断面の色分けで、どの面がどの平面に乗るかを調べる。
    std::vector<geometry::Mesh> m_rockPreviewSurfaces;
    uint64_t m_rockPreviewStamp = 0;  // m_rockPreviewSurfaces を作り直すたびに増やす。
    // 断面の色分けの三角形。ノード・平面の群・表示中のメッシュが変わったときだけ作り直す。
    struct CutFaceOverlay {
        graph::GraphId node = 0;
        std::shared_ptr<const geometry::PlaneCutsGuide> guide;
        uint64_t stamp = 0;
        std::vector<renderer::OverlayLineSet> sets;
    };
    CutFaceOverlay m_cutFaceOverlay;
    // 選んだ Plane Cuts の平面の枠（深度付きの線）と断面の色分け（半透明の面）を lines へ加える。
    void AppendPlaneCutOverlay(std::vector<renderer::OverlayLineSet>& lines);
    // Voronoi Fracture を選んでいる間の表示。どちらかがオンなら岩の面を隠す。
    // ノードの設定ではない（評価・Undo・保存に関わらない）。
    bool m_voronoiShowWireframe = false;
    bool m_voronoiShowPoints = false;
    // Piece Filter を選んでいる間の表示。オンなら、除外した分割片を稜線で見せる（残った片は面のまま）。
    bool m_pieceFilterShowRemoved = false;
    // 分割片の稜線。分割の結果が変わったときだけ作り直す。
    // 分割片の稜線。描く片の集合（pieces）と除く片の集合（excluded）が変わったときだけ作り直す。
    struct PieceWireframe {
        std::shared_ptr<const geometry::PieceCollection> pieces, excluded;
        std::vector<renderer::OverlayLineSet> sets;
    };
    PieceWireframe m_pieceWireframe;
    // 選んだ Voronoi Fracture の分割片、または Piece Filter で除外した片の稜線を lines へ加え、
    // 岩の面を隠すかどうかを描画器へ伝える。
    void AppendPieceOverlay(std::vector<renderer::OverlayLineSet>& lines);
    // 選んだ Voronoi Fracture の Points 入力の点。表示しないときや未評価なら空。
    std::shared_ptr<const geometry::PointSet> SelectedVoronoiPoints() const;
    // カーソル直下のメッシュを CPU のレイ交差で探し、クリックか矩形で選ぶ。
    void HandleMeshHover(bool itemHovered, const ImVec2& viewportMin, const ImVec2& viewportMax);
    bool HandleLightDrag(renderer::LightSettings& light, LightInteraction& interaction, bool itemActive);
    // ビューポート上の F / A キーで視点をメッシュへ戻す。
    // flying はフライ中（右ボタンを押している間）。ホイールのズームと F / A を止める。
    void HandleCameraInput(renderer::PreviewRenderer& preview, bool itemActive, bool itemHovered,
                           bool includeReferenceGrid = false, bool flying = false);
    // フライの入力。ビューポートの不可視ボタンの直後に呼ぶ。右ボタンを押している間は true。
    bool HandleFlyCamera(bool itemActive, bool enabled);
    // フライの速さ（m/s、Shift を含まない）。被写体の大きさから決める基準 × 倍率。
    float FlySpeed() const;
    // ホイールで速さを変えた直後だけ、ビューポートの下の中央に速さを出す。
    void DrawFlySpeed(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // ライトの向きを示すギズモ。動かしている間と、その直後だけ出す。
    void DrawLightGizmo(const renderer::LightSettings& light, const LightInteraction& interaction,
                        const renderer::Camera& camera, const ImVec2& viewportMin, const ImVec2& viewportMax);
    // 上の本体。origin を中心に半径 gizmoRadius で描く。マテリアルプレビューのように
    // renderer::Camera を持たない画面からも同じギズモを出すために分けてある。
    void DrawLightGizmoAt(const renderer::LightSettings& light, const LightInteraction& interaction,
                          const DirectX::XMMATRIX& viewProjection, const DirectX::XMFLOAT3& origin,
                          float gizmoRadius, const ImVec2& viewportMin, const ImVec2& viewportMax);
    // マテリアルプレビューの光源。強さと色はシーンに合わせ、向きだけプレビュー専用にできる。
    renderer::LightSettings MaterialPreviewLight() const;

    // カーソル位置からカメラのレイ（ワールド座標、方向は単位長）。ビューポートが潰れていれば偽。
    bool ViewportRay(const ImVec2& mouse, const ImVec2& viewportMin, const ImVec2& viewportMax,
                     DirectX::XMFLOAT3& outOrigin, DirectX::XMFLOAT3& outDirection) const;
    // 選択メッシュの境界ボックスの中心。選択が無ければ偽。
    bool SelectedMeshFocusTarget(DirectX::XMFLOAT3& target) const;
    Window m_window;
    rhi::Device m_device;
    rhi::ShaderCompiler m_shaderCompiler;
    rhi::PipelineCache m_pipelineCache;
    renderer::PreviewRenderer m_renderer;
    // マテリアルプレビューの球。窓を開いている間だけ描く。
    renderer::MaterialSphere m_materialSphere;
    // 天球プレビューの球。同じく窓を開いている間だけ描く。
    renderer::SkySphere m_skySphere;
    // --- ノードグラフ -------------------------------------------------------
    // グラフが唯一の入口。Mesh Output へ届いた鎖をメッシュシーンにして
    // レンダラへ渡す（SyncMeshGraph）。材質の合成はメッシュごとにレンダラ側で評価する。
    graph::NodeGraph m_graph = graph::NodeGraph::CreateDefault();
    graph::GraphId m_selectedGraphNode = 0;
    // エディタで選ばれているノード全部。コピーはこれを見る
    // （プロパティに出すのは先頭の 1 つ = m_selectedGraphNode）。
    std::vector<graph::GraphId> m_selectedGraphNodes;

    // ノードのコピー元。**OS のクリップボードは使わない**（アプリ内だけ）。
    // 位置と設定に加えて、**入力ピンごとの接続元**を覚える。
    // コピーした集合の中を指していれば貼った側どうしで繋ぎ直し、
    // 外を指していれば**元の親へ繋いだまま**にする。
    struct GraphClipboardNode {
        graph::GraphId originalId = 0;
        graph::NodeKind kind = graph::NodeKind::Surface;
        graph::NodeSettings settings;
        float posX = 0.0f;
        float posY = 0.0f;
        // コピーした時点のノードの大きさ。貼るときに集合の中心を出すのに使う
        // （位置だけだと左上しか分からず、画面中央に寄せると右下へずれる）。
        float sizeX = 0.0f;
        float sizeY = 0.0f;
        std::string note;
        // 参照しているアセットのファイル。別の文書へ貼るとき、番号をこれで引き直す。
        // 空なら持ち越せない（未保存・一時的なもの）。
        std::vector<std::filesystem::path> materialPaths;  // VisitNodeMaterialLayers の順
        std::filesystem::path texturePath;                  // Material Mask の画像
        std::filesystem::path modelPath;                    // Model（.model か取り込み元）
        struct Source {
            int copiedIndex = -1;              // コピーした集合の中の添字
            graph::GraphId externalPin = 0;    // 集合の外なら、その出力ピン
        };
        std::vector<Source> inputs;
    };
    std::vector<GraphClipboardNode> m_graphClipboard;
    // 貼るたびに位置をずらす回数。コピーし直すと 0 に戻す。
    int m_graphPasteCount = 0;
    // 開いている文書の通し番号。新規・読み込みで進める。控えたノードがどの文書のものかの判定に使う。
    uint64_t m_documentGeneration = 1;
    // 控えたノードの番号（ピン・アセット）が有効な文書。
    uint64_t m_graphClipboardDocument = 0;
    // フレームの外で貼る予約（別の文書でコピーしたとき）。値は貼る先のキャンバスの中央。
    std::optional<ImVec2> m_pendingGraphPaste;
    // メモの印（か省略したメモ）にカーソルが載っているノード。ed::End の後でツールチップを出す。
    graph::GraphId m_graphNoteHover = 0;
    // ビューポートに出しているメッシュノード。**選択とは別に持つ。**
    // 結果を見ながら別のノードのプロパティをいじれるようにするため。0 は Mesh Output の鎖。
    graph::GraphId m_previewGraphNode = 0;
    // プレビューしている出力ピン。0 なら最初の出力。
    graph::GraphId m_previewGraphPin = 0;
    // 出力ピンの「クリック」を拾うための押した位置。ドラッグ（リンク作成）と
    // 区別するために、押した / 離したが同じピンで、ほとんど動いていないときだけ
    // クリックとみなす。
    graph::GraphId m_graphPressedPin = 0;
    ImVec2 m_graphPressedPinPos{};
    ax::NodeEditor::EditorContext* m_nodeEditor = nullptr;
    // グラフパネル内の「エディタ / プロパティ」境界の高さ（96 DPI 基準）。
    float m_graphEditorHeight = 380.0f;
    // 位置をエディタへ流し込むべきノード。作成・読み込みのときに積む。
    std::vector<graph::GraphId> m_graphNodesToPlace;
    // 位置を流し込んだ後に全体を画面へ収めるまでの残りフレーム数。
    // **キャンバスの大きさが安定しているフレームだけ数える。** エディタは
    // サイズ変化のたびに前の表示領域を復元するので、ドックの確定前に寄せると
    // 上書きされて効かない。
    int m_graphNavigateCountdown = 0;
    ImVec2 m_graphCanvasSize = ImVec2(0.0f, 0.0f);
    compositor::TextureLibrary m_textureLibrary;
    compositor::MaterialLibrary m_materialLibrary;
    // モデル。マテリアルと同じく文書の一部で、スロットの割り当てはアンドゥの対象。
    // CPU の形状は共有ポインタなので、スナップショットへ複製しても頂点は複製されない。
    std::vector<renderer::ModelAsset> m_models;
    uint64_t m_nextModelId = 1;
    // 一覧（アセットの帯）で選んでいるモデル。窓はこれを映す。
    uint64_t m_selectedModel = 0;
    int m_modelLod = 0;
    // Model ノードのプロパティで回転を編集している FBX のノード（名前）。
    std::string m_selectedModelNodeName;
    // モデルごとの GPU メッシュと出力。選んでいるものは窓が開いている間毎フレーム描き、
    // それ以外はサムネイルとして 1 度だけ描く（m_renderedModelThumbnails）。
    std::unordered_map<uint64_t, std::unique_ptr<renderer::ModelPreview>> m_modelPreviews;
    std::unordered_set<uint64_t> m_renderedModelThumbnails;
    // フレームの外で処理するモデルの作業。
    std::vector<std::filesystem::path> m_pendingModelImports;
    uint64_t m_pendingModelMaterials = 0;
    uint64_t m_pendingModelRemove = 0;
    // カーソル直下の Model ノード（ビューポートの枠の表示用）。
    graph::GraphId m_hoveredModelNode = 0;
    // ビューポートからグラフエディタの選択を変える要求（0 は選択を外す）。エディタの描画の中で反映する。
    std::optional<graph::GraphId> m_graphSelectionRequest;
    // モデルのドラッグ（本体の水平移動とギズモ）。掴んだときの値から置き直す（誤差を溜めない）。
    // handle: -1 = 本体、0〜2 = 軸（X / Y / Z）、3〜5 = 平面（YZ / XZ / XY。法線の軸の番号 + 3）、6〜8 = 回転の輪（X / Y / Z）。
    struct ModelInstanceDrag {
        bool pending = false;
        bool dragging = false;
        int handle = -1;
        graph::GraphId node = 0;
        ImVec2 pressPos{};
        DirectX::XMFLOAT3 pivot{};
        DirectX::XMFLOAT4X4 parent{};
        DirectX::XMFLOAT3 pressPoint{};
        float pressParameter = 0.0f;
        float startPosition[3] = {};
        float startRotation[3] = {};
        float startScale = 1.0f;
        float startScaleXYZ[3] = {1,1,1};
        float planeY = 0.0f;
        // ノード用のギズモで FBX のノードを回しているとき。軸はワールド、角度は掴んだときのノードの回転（度）。
        bool nodeRotation = false;
        std::string modelNodeName;
        DirectX::XMFLOAT3 nodeAxes[3]{};
        float startNodeRotation[3] = {};
    } m_modelInstanceDrag;
    // ギズモの種類（W で移動、E で回転、R で倍率）と、カーソルが乗っているハンドル（-1 = 無し）。
    enum class ModelGizmoMode { Translate, Rotate, Scale };
    ModelGizmoMode m_modelGizmoMode = ModelGizmoMode::Translate;
    int m_modelGizmoHover = -1;
    // ノード用のギズモ（プロパティの「ノード」の「ギズモ」）。オンの間は、選んでいる Model ノードの
    // FBX のノード（m_selectedModelNodeName）を回す輪を出し、部品のクリックでノードを選ぶ。W / E / R で外れる。
    bool m_modelNodeGizmo = false;
    // ノード用のギズモでカーソルが乗っている部品のノード（枠の表示用。無ければ空）。
    std::string m_hoveredModelNodeName;
    // 帯から落としたモデルの配置。ファイルの読み込みを伴うのでフレームの外で行う。
    struct PendingModelPlacement {
        std::filesystem::path path;
        DirectX::XMFLOAT3 position{};
    };
    std::vector<PendingModelPlacement> m_pendingModelPlacements;
    // 取り込んだ FBX のスロットへ、続けてマテリアルを作るか（--import-model）。
    bool m_createImportedModelMaterials = false;
    // 天球アセット。マテリアルと並ぶアセットだが、**アンドゥの対象には入れない。**
    // 環境は作っているマテリアルそのものではなく、見え方の設定に近い
    // （プレビュー設定を履歴に載せないのと同じ理由）。
    renderer::SkyLibrary m_skyLibrary;
    int m_selectedMaterial = 0;
    compositor::MaterialAssetId m_layerEditorAsset = 0;
    int m_layerEditorSelection = 0;
    // ORD をまとめて割り当てるときに選ぶテクスチャ（UI の一時状態）。
    compositor::TextureId m_ordTexture = compositor::kNoTexture;
    // ライトの向きを掴んでいる間。ギズモは離してからも少しの間だけ残す。
    LightInteraction m_viewportLightInteraction;
    // フライ（UE5 / terrain-graph と同じ）: ビューポートで右ボタンを押している間、
    // マウスで見回し WASD / QE で動く。
    struct FlyCamera {
        bool held = false;     // ビューポートで右ボタンを押している
        bool active = false;   // この押下でフライになった（見回したか、キーで動いた）
        float dragPixels = 0;  // 押してから動いた量（フライになるまで）
        int anchorX = 0, anchorY = 0;  // 押した位置（画面座標）。見回す間はここへカーソルを戻す
        float speedScale = 1;          // ホイールで変える速さの倍率
        double speedShownUntil = 0;    // 速さを表示する時刻（ImGui::GetTime）
    } m_fly;
    // マテリアルプレビューの L + ドラッグ。**シーンの太陽とは別に持つ。**
    // 一度も動かしていなければシーンの太陽の向きに合わせ、「光源を戻す」でそこへ戻る。
    LightInteraction m_materialPreviewLightInteraction;
    // レイヤーマテリアルを出しているときの、プレビュー窓の左（プレビュー）の幅。96 DPI 基準。
    float m_layerPreviewPaneWidth = 320.0f;
    bool m_materialPreviewLightCustom = false;
    float m_materialPreviewLightAzimuth = 0.0f;
    float m_materialPreviewLightElevation = 0.0f;

    int m_selectedTexture = 0;
    // 拡大プレビューで出すチャンネル。0 = RGB、1..4 = R / G / B / A。
    // ORD のように 1 枚へ複数のマップを詰めたテクスチャの中身を確かめるためのもの。
    int m_previewChannel = 0;
    // 読み込んだ直後のテクスチャを一覧に見せるための要求。
    // 一覧はスクロールするので、追加しただけでは枠外に入って気づけない。
    bool m_scrollToSelectedTexture = false;
    // 追加・複製した直後のマテリアルを一覧の枠内へ送る要求。上と同じ理由。
    bool m_scrollToSelectedMaterial = false;
    // 追加・複製した直後の天球を一覧の枠内へ送る要求。上と同じ理由。
    bool m_scrollToSelectedSky = false;

    // ステータスバーに出す直近の通知。ログから受け取る。
    // 時刻は ImGui に依存させない（ログはコンテキストが無い時期にも来る）。
    struct StatusMessage {
        std::string text;
        LogLevel level = LogLevel::Info;
        std::chrono::steady_clock::time_point time{};
        bool valid = false;
    };
    StatusMessage m_status;
    // 読み込みは GPU 待機を伴うため、フレームの外で処理する。
    std::vector<std::filesystem::path> m_pendingTexturePaths;

    // --- ファイル操作の保留 -------------------------------------------------
    // ダイアログはフレームの中で出すが、読み書きは GPU 待機を伴うので、
    // 選ばれたパスをここへ積んでおき、次のフレームの頭で処理する。
    // プロジェクトのルートフォルダと共有アセット。常に 1 つ開いている。
    io::ProjectWorkspace m_workspace;
    // アセットの帯の状態。表示中のフォルダとその中身、選択、未読み込みのサムネイル。
    AssetThumbnailCache m_assetThumbnails;
    std::filesystem::path m_assetDirectory;
    std::vector<std::filesystem::directory_entry> m_assetEntries;
    // 「テンプレートから作成」の状態。一覧とサムネイルは初めて開いたときに読む。
    bool m_templateWindow = false, m_templatesLoaded = false;
    std::filesystem::path m_templateDirectory;
    // テンプレートを未保存の文書として開く要求（m_pendingProjectOpen がテンプレートのファイル）。
    bool m_pendingTemplateOpen = false;
    // 未保存の文書の名前と、初めて保存するときの保存先の初期値（テンプレートから開いたとき）。
    std::string m_untitledName;
    std::filesystem::path m_untitledDirectory;
    std::vector<io::RockTemplate> m_rockTemplates;
    std::vector<rhi::GpuTexture> m_templateThumbnails;
    // フォルダ階層（親 → 子フォルダの一覧）。毎フレーム列挙せず、更新のときに作り直す。
    std::unordered_map<std::wstring, std::vector<std::filesystem::path>> m_assetFolders;
    // ルート内の画像ファイル（全フォルダ）。テクスチャのコンボに未読み込みの候補として出す。フォルダ階層と同時に作り直す。
    std::vector<std::filesystem::path> m_workspaceImages;
    // ルート内のマテリアル（.rockmat / .tglayer）。マテリアルのコンボに未読み込みの候補として出す。
    std::vector<std::filesystem::path> m_workspaceMaterials;
    // 一覧で選んでいるファイル・フォルダ。クリックで単独、Ctrl+クリックで追加 / 除外、Shift+クリックで起点からの範囲。
    std::vector<std::filesystem::path> m_selectedAssets;
    std::filesystem::path m_assetSelectionAnchor;
    bool IsAssetSelected(const std::filesystem::path& path) const;
    void SelectAsset(const std::filesystem::path& path, bool toggle, bool range);
    bool m_assetRefresh = true;
    // 名前の変更。一覧のサムネイルの下（またはフォルダ階層の行）でその場で入力し、確定分をフレームの外で処理する。
    // m_assetRenameTarget が空でなければ編集中。m_assetRenameFocus は入力欄が掴むまで立てておく。
    void OpenAssetRename(const std::filesystem::path& path);
    // その場の入力を終える。commit なら入力した名前で改名を予約する（拡張子は元のまま）。
    void FinishAssetRename(bool commit);
    // 改名したファイル・フォルダを指す、読み込み済みのアセットの絶対パスを付け替える（フォルダなら配下も）。
    void RelinkAssetPaths(const std::filesystem::path& from, const std::filesystem::path& to);
    std::filesystem::path m_assetRenameTarget;
    bool m_assetRenameFocus = false;
    // 左のフォルダ階層の行で編集しているとき true（一覧の同じフォルダには欄を出さない）。
    bool m_assetRenameInTree = false;
    char m_assetRenameBuffer[256] = {};
    std::filesystem::path m_pendingAssetRename;
    std::string m_pendingAssetRenameName;
    // **ファイルを持つアセットの名前はファイル名（拡張子なし）を正とする。**
    // 読み込み・改名・移動のあとに、名前をファイル名へ揃える（毎フレームの保留処理の最後で呼ぶ）。
    void SyncAssetNamesToFiles();
    // パネルの「名前」欄からの変更。ファイルがあればその改名を予約し（名前は改名後に追従する）、
    // 未保存なら名前だけを変える。変えたときは true。
    bool RequestAssetNameChange(const std::filesystem::path& assetPath, std::string& name, const char* newName);
    // 一覧のフォルダ・左のフォルダ階層に置くドロップ先。サムネイルが落とされたらそのフォルダへの移動を予約する。
    void AssetFolderDropTarget(const std::filesystem::path& directory);
    std::vector<std::filesystem::path> m_pendingAssetMoves;
    std::filesystem::path m_pendingAssetMoveTarget;
    // ファイルの削除（退避）。検査 → 確認 → 実行の順で、実行はフレームの外。
    std::filesystem::path m_pendingAssetDeleteInspect;
    io::AssetRelations m_assetDeleteRelations;
    bool m_assetDeleteDialog = false;
    bool m_pendingAssetDelete = false;
    // Del キーで複数を選んで消すときの残り。確認は 1 件ずつ出し、終わる（か取り消す）たびに次を検査する。
    std::vector<std::filesystem::path> m_assetDeleteQueue;
    // 表示中のフォルダの更新時刻。外から消された・足されたファイルを、次の確認で一覧へ反映する。
    std::filesystem::file_time_type m_assetDirectoryStamp{};
    double m_assetDirectoryChecked = 0;
    // ルートの切り替え、共有アセットの保存、ファイルを開く要求。フレームの外で処理する。
    std::filesystem::path m_pendingRoot;
    std::filesystem::path m_pendingAssetOpen;
    bool m_pendingAssetsSave = false;
    // シーン / ルートの切り替え前の確認（保存して切り替え / 保存せず / キャンセル）。
    std::filesystem::path m_deferredRoot;
    std::filesystem::path m_deferredScene;
    bool m_deferredNew = false;
    bool m_sceneSwitchDialog = false;
    bool m_allowSceneSwitch = false;
    bool m_saveThenSwitch = false;
    // 旧「テクスチャ / マテリアル / 天球」の一覧。ウィンドウメニューから出す補助ウィンドウ。
    bool m_showTextureList = false;
    bool m_showMaterialList = false;
    bool m_showSkyList = false;
    std::filesystem::path m_projectPath;  // 現在のシーン (.rockgraph)。未保存なら空
    io::RecentFiles m_recentProjects;
    io::AppSettings m_settings;
    // 設定ウィンドウを出しているか。ドックへは収めない補助ウィンドウ。
    bool m_showSettings = false;
    // 情報ウィンドウ。必要なときだけウィンドウメニューから開く。
    bool m_showInfo = false;
    // マテリアルプレビューの窓。ドックへは収めない補助ウィンドウ。
    bool m_showMaterialSphere = false;
    // テクスチャプレビューの窓。同じくドックへは収めない。
    bool m_showTexturePreview = false;
    // 天球プレビューの窓。同じくドックへは収めない。
    bool m_showSkyPreview = false;
    // モデルプレビューの窓と、その中身をこのフレームに描いたか。
    bool m_showModelPreview = false;
    bool m_modelPreviewVisible = false;
    // その窓の中身をこのフレームに描いたか（折りたたまれていれば球も描かない）。
    bool m_skyPreviewVisible = false;
    // その窓の中身をこのフレームに描いたか。**折りたたまれていれば球も描かない。**
    // UI（DrawUi）はフレームの記録より前に走るので、その結果をここへ残して使う。
    bool m_materialSphereVisible = false;
    std::filesystem::path m_pendingProjectSave;
    std::filesystem::path m_pendingProjectOpen;
    std::filesystem::path m_pendingMaterialExport;
    std::filesystem::path m_pendingMaterialImport;
    compositor::MaterialAssetId m_pendingExportMaterial = compositor::kNoMaterialAsset;
    compositor::TextureId m_pendingTextureRemove = compositor::kNoTexture;
    // 繋ぎ直しの予約（対象のテクスチャと新しいパス）。ダイアログで選んだものと、
    // フォルダ指定で見つけたものの両方がここへ積まれる。
    struct TextureRelink {
        compositor::TextureId id = compositor::kNoTexture;
        std::filesystem::path path;
    };
    std::vector<TextureRelink> m_pendingTextureRelinks;
    // コンボで選んだ未読み込みのマテリアル。フレームの外で読み込み、assign で割り当て先へ入れる。
    struct MaterialLoadRequest {
        std::filesystem::path path;
        std::function<void(compositor::MaterialAssetId)> assign;
    };
    std::vector<MaterialLoadRequest> m_pendingMaterialLoads;
    // 削除要求のあったマテリアル。一覧の描画中に消すと、描画側が erase 済みの
    // 要素を読んでしまうため、フレームの外で処理する。
    compositor::MaterialAssetId m_pendingMaterialRemove = compositor::kNoMaterialAsset;
    // 削除要求のあった天球。マテリアルと同じ理由でフレームの外で処理する。
    renderer::SkyAssetId m_pendingSkyRemove = renderer::kNoSkyAsset;
    // 確認待ちのテクスチャ。参照が残っているときだけ入る。
    compositor::TextureId m_textureRemoveCandidate = compositor::kNoTexture;
    std::vector<std::string> m_textureRemoveUsers;
    bool m_pendingProjectNew = false;
    // 開いている文書の種類。拡張子（.rockgraph / .mountaingraph）で決まり、右クリックメニューと保存先に効く。
    enum class DocumentKind { Rock, Mountain };
    DocumentKind m_documentKind = DocumentKind::Rock;
    // 新規作成で作る文書の種類（m_pendingProjectNew と一緒に使う）。
    DocumentKind m_pendingNewKind = DocumentKind::Rock;
    // 新規作成した文書をすぐ保存する先（アセット欄の右クリックから作ったとき）。空なら保存しない。
    std::filesystem::path m_pendingNewPath;
    // 作った文書を保存する先と、保存してよいフレーム。新しい文書を描いてから保存する
    // （すぐ保存すると、サムネイルに前の文書の画面が写る）。
    std::filesystem::path m_newDocumentSavePath;
    uint64_t m_newDocumentSaveFrame = 0;

    // --- アンドゥの状態 -----------------------------------------------------
    UndoHistory m_undoHistory;
    // 直近に確定した文書。変更を見つけたとき、これを「変更前」として積む。
    DocumentSnapshot m_committed;
    // このフレームでレイヤーかマテリアルが変わったか。フレームの終わりに畳む。
    bool m_documentDirty = false;
    // このフレームの変更が「直前の編集の続き」であることの印。アンドゥの段を直前の
    // 段に畳む（ドラッグを離した時点の合体などが、別の段にならないように）。
    bool m_documentJoinsEdit = false;
    // -1 でアンドゥ、+1 でリドゥ。マテリアルの破棄を伴うのでフレームの外で処理する。
    int m_pendingHistoryStep = 0;

    ImGuiLayer m_imgui;
    // 右下に出す通知。保存の完了などを知らせる。
    ui::ToastQueue m_toasts;
    // F12 が押されたフレームに立つ。EndFrame で撮ってから下ろす。
    bool m_screenshotPending = false;

    // ビューポートの表示サイズ。UI 側で決まり、次のフレーム頭で反映する。
    uint32_t m_requestedViewportWidth = 512;
    uint32_t m_requestedViewportHeight = 512;

    StartupOptions m_options;

    // CoInitializeEx が成功したときだけ CoUninitialize する。
    bool m_comInitialized = false;
    // ドックレイアウトの初期化。ini に配置が無ければ既定レイアウトを組む。
    bool m_layoutChecked = false;
    bool m_rebuildLayout = false;
    // 前面へ出したいタブ（右カラムは「グラフ」）を押さえるための残りフレーム数。
    //
    // **起動のたびに効かせる。** ini には前回選んでいたタブが残っているので、
    // それに任せると「前回ライティングを見ていた」だけで次の起動もそこから始まる。
    // 作業の起点はグラフなので、起動時は必ずグラフを前面にする。
    int m_focusDefaultTabs = 3;
    // **表示設定（垂直同期・FPS 上限・ホットリロード・背景色・オーバーレイ）は
    // ここに写しを持たない。** `m_settings.Display()` を直接読み書きする。
    // 写しを持つと「UI では変わったのに設定へ書き戻し忘れて次回起動で戻る」
    // という壊れ方をする（実際に FPS 上限でそうなった）。
    FrameLimiter m_frameLimiter;
    // 前フレームで前面だったか。切り替わった時点で締め切りを捨てる。
    bool m_wasForeground = true;
    uint32_t m_frameCounter = 0;
    std::chrono::steady_clock::time_point m_startTime;
};

}  // namespace rock
