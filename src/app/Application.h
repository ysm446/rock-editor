#pragma once
#include <future>
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
#include "io/AppSettings.h"
#include "io/RecentFiles.h"
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

// コマンドラインから渡せる起動オプション。
struct StartupOptions {
    // 起動時に読み込む HDRI。空なら手続き的な空を使う。
    std::filesystem::path hdriPath;
    // 起動時にテクスチャライブラリへ読み込む画像。--texture を繰り返し指定できる。
    std::vector<std::filesystem::path> texturePaths;
    // 起動時に開くシーン (.rockscene) または旧プロジェクト (.reproj)。ルートのフォルダや
    // project.reproj を渡すとルートだけを開く。空なら新規シーンで始める。
    std::filesystem::path projectPath;
    // プロジェクトのルートフォルダ（--root）。空なら最近使ったルート、無ければ data/。
    std::filesystem::path projectRoot;
    // 削除確認画面のスクリーンショット検証用。削除そのものは実行しない。
    std::filesystem::path inspectAssetDelete;
    // 指定すると、数フレーム描いてからプロジェクトを保存して終了する。
    // 保存と読み込みを対話なしで確かめるための開発用オプション。
    std::filesystem::path saveProjectPath;
    // 指定すると、数フレーム描いてからビューポートを PNG に書き出して終了する。
    // 画面キャプチャに頼らず描画結果を確認するための開発用オプション。
    std::filesystem::path screenshotPath;
    // 指定すると、ウィンドウ全体（UI 込み）を PNG に書き出して終了する。
    // 画面キャプチャは他ウィンドウを掴むことがあるため、確認にはこちらを使う。
    std::filesystem::path uiScreenshotPath;
    uint32_t screenshotFrame = 8;
    // 開発用。FBX をモデルとして読み込み、FBX のマテリアルからマテリアルを作ってプレビューを開く。
    std::filesystem::path importModel;
    // 開発用。ルート内のアセットをアセットの帯のダブルクリックと同じ経路で開く。
    std::filesystem::path openAsset;
    // 開発用。モデル（.rockmodel / .fbx）を原点へ置く（帯からビューポートへ落としたのと同じ経路）。
    std::filesystem::path placeModel;
    // 開発用。モデルのギズモを回転（E）で始める。
    bool gizmoRotate = false;
    bool gizmoScale = false;
    bool testGpuAo = false;
    int bakeNode = 0; // 開発用。通常のベイク実行と同じ処理を予約する。
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
    // グラフのノード 1 枚。カード・ピン・リンクの当たり判定を描く。
    void DrawGraphNode(const graph::Node& node);
    bool IsGraphPinVisible(const graph::Pin& pin) const;
    void DrawGraphBackground(const ImVec2& min, const ImVec2& max);
    bool DrawLayerSettings(compositor::MaterialLayer& layer);
    // グラフの変更をメッシュシーンへ反映する。フレームの頭（フレームの外）で呼ぶ。
    void SyncMeshGraph();
    void DrawPieceSettings(graph::Node&);
    void CommitPieceViewportSelection();
    bool m_pieceSelectionEditing = false;
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
    // 計算中のノード（無ければ 0）と、「UV Unwrap: 島を配置中 42%・12秒」のような表示文。
    graph::GraphId EvaluatingNode() const;
    std::string EvaluationProgressText() const;
    std::shared_ptr<const geometry::PieceCollection> m_pieceInput, m_piecePreview;
    graph::GraphId m_pieceInputNode = 0;
    int m_pieceGizmoId = -1;
    std::shared_ptr<const geometry::PieceSelection> m_pieceTransformSelection;

    void DrawUvPanel();
    void ApplyRockMaterial(renderer::SceneMesh& mesh, const graph::GeneratedRock& rock, bool useBaked);
    std::string BakeFingerprint(const renderer::SceneMesh& mesh, const geometry::Mesh& input, graph::GraphId bakeNode = 0) const;
    void ProcessPendingBake();
    bool ValidateGpuAo();
    void FinishBake(graph::GraphId id, std::array<LdrImage, 4>& images, const std::string& fingerprint);
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
    // 選択中のノードを控える / 貼り付ける（Ctrl+C / Ctrl+V）。
    void CopySelectedGraphNodes();
    // 控えたノードを貼る。viewCenter は今のキャンバスの中央（キャンバス座標）で、
    // 貼った集合の中心をそこへ置く。相対の配置は保つ。
    void PasteGraphNodes(const ImVec2& viewCenter);
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
    bool DrawMaterialProperties(compositor::MaterialAsset& asset);
    // マテリアルプレビューの窓（回せる球 + プロパティ）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawMaterialSphereWindow();
    // --- モデル（ApplicationModelPanel.cpp） --------------------------------------
    // モデルプレビューの窓（回せるモデル + 寸法・LOD・マテリアルスロット）。
    // アセットの帯でモデル（.rockmodel / .fbx）をダブルクリックするか、ウィンドウメニューから開く。
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
    // --- リンク切れの解消 ---------------------------------------------------
    // ファイルを選ぶダイアログを出し、選ばれたら再リンクを予約する
    // （読み込みは GPU 待機を伴うのでフレームの外で行う）。
    void RequestTextureRelink(compositor::TextureId id);
    // フォルダを選び、そこにあるリンク切れのファイル名をまとめて繋ぎ直す予約をする。
    // 素材のフォルダごと移した（別の PC で開いた）ときの入口。
    void RequestTextureRelinkFolder();
    // 予約した再リンクを処理する。繋ぎ直せたら、参照しているサムネイルと合成を作り直す。
    void ProcessPendingTextureRelinks();
    // マテリアルが参照しているテクスチャのどれかがリンク切れか。一覧の目印に使う。
    bool MaterialHasMissingTexture(const compositor::MaterialAsset& asset) const;
    // テクスチャプレビューの窓（拡大表示 + 詳細）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawTexturePreviewWindow();
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
    // カーソル直下のメッシュを CPU のレイ交差で探し、クリックか矩形で選ぶ。
    void HandleMeshHover(bool itemHovered, const ImVec2& viewportMin, const ImVec2& viewportMax);
    bool HandleLightDrag(renderer::LightSettings& light, LightInteraction& interaction, bool itemActive);
    // ビューポート上の F / A キーで視点をメッシュへ戻す。
    void HandleCameraInput(renderer::PreviewRenderer& preview, bool itemActive, bool itemHovered,
                           bool includeReferenceGrid = false);
    // ライトの向きを示すギズモ。動かしている間と、その直後だけ出す。
    void DrawLightGizmo(const renderer::LightSettings& light, const LightInteraction& interaction,
                        const renderer::Camera& camera, const ImVec2& viewportMin, const ImVec2& viewportMax);

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
    renderer::PreviewRenderer m_layerPreview;
    bool m_layerPreviewInitialized = false;
    bool m_layerPreviewDirty = true;
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
        struct Source {
            int copiedIndex = -1;              // コピーした集合の中の添字
            graph::GraphId externalPin = 0;    // 集合の外なら、その出力ピン
        };
        std::vector<Source> inputs;
    };
    std::vector<GraphClipboardNode> m_graphClipboard;
    // 貼るたびに位置をずらす回数。コピーし直すと 0 に戻す。
    int m_graphPasteCount = 0;
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
    // ORD をまとめて割り当てるときに選ぶテクスチャ（UI の一時状態）。
    compositor::TextureId m_ordTexture = compositor::kNoTexture;
    // ライトの向きを掴んでいる間。ギズモは離してからも少しの間だけ残す。
    LightInteraction m_viewportLightInteraction;
    LightInteraction m_layerLightInteraction;

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
    // フォルダ階層（親 → 子フォルダの一覧）。毎フレーム列挙せず、更新のときに作り直す。
    std::unordered_map<std::wstring, std::vector<std::filesystem::path>> m_assetFolders;
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
    std::filesystem::path m_projectPath;  // 現在のシーン (.rockscene)。未保存なら空
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
    // 削除要求のあったマテリアル。一覧の描画中に消すと、描画側が erase 済みの
    // 要素を読んでしまうため、フレームの外で処理する。
    compositor::MaterialAssetId m_pendingMaterialRemove = compositor::kNoMaterialAsset;
    // 削除要求のあった天球。マテリアルと同じ理由でフレームの外で処理する。
    renderer::SkyAssetId m_pendingSkyRemove = renderer::kNoSkyAsset;
    // 確認待ちのテクスチャ。参照が残っているときだけ入る。
    compositor::TextureId m_textureRemoveCandidate = compositor::kNoTexture;
    std::vector<std::string> m_textureRemoveUsers;
    bool m_pendingProjectNew = false;

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
