#pragma once

#include "compositor/MaterialLayer.h"
#include "graph/Path.h"
#include "renderer/ModelAsset.h"

#include <array>
#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ノードグラフのデータモデル。terrain-editor から「仕組み」を移植したもの。
//
//   - ノード・ピン・リンクは共通の単一 ID 空間から採番する
//     （imgui-node-editor の NodeId / PinId / LinkId にそのまま流用できる）。
//   - ノードの設定は種類ごとの構造体を std::variant で持つ。
//     terrain-editor の「全種類の設定を 1 構造体に持つファット構造体」はやめた。
//   - 材質（Surface）は既存の GPU 評価器を使う。グラフは CompileLayersTo() で
//     レイヤー列へ落とし、MaterialStack として評価する。
//
// UI / D3D12 には依存しない（compositor のデータ構造にだけ依存する）。
namespace tg::graph {

using GraphId = int;

enum class PinKind : uint32_t {
    Input,
    Output,
};

// ピンを流れる値の型。同じ型どうしだけ接続できる。
enum class ValueType : uint32_t {
    Material = 0,
    // パス（向き付きの線）。Path ノードが出し、Road / Decal / Shoulder が読む。
    Path = 2,
    Mesh = 3,
    // 道路空間マスク（横位置 × 実距離）。Road Mask ノードが出し、Road のマスク入力が読む。
    RoadMask = 4,
    // 置いたモデルの集まり。Model / Transform が受け渡す。道路のメッシュ（Mesh）の入力とは繋がらない。
    Model = 5,
    // 道路のメッシュとモデルのどちらでも受ける入力（Merge / Mesh Output）。
    // Merge の出力はこの型にはならず、入力から決まる（NodeGraph::EffectiveOutputType）。
    Any = 6,
};

// 数値は保存名ではなくファイルには書かない（定義テーブルの name を書く）が、
// 旧地形ノードを撤去した後も残った種類の値は変えない。
enum class NodeKind : uint32_t {
    Road = 24,
    MeshOutput = 25,
    // 道路面の上に白線の帯ポリゴンを生成する。RoadSurfaceを受け、道路と白線をまとめて出す。
    RoadMarking = 26,
    // 道路空間マスク。轍・端の減衰・長さ方向ノイズを Road のスロット 2〜4 の被覆率にする。
    RoadMask = 27,
    // 面上のパスに沿って帯を貼るデカール。ひび・補修跡・汚れなどの模様。
    Decal = 28,
    // 路肩。Road の Left / Right（または別の路肩の Outer）の境界の頂点を起点に、外側へ断面を押し出す。
    Shoulder = 29,
    // 複数の Mesh の枝を 1 つの RoadSurface にまとめる。同じノード由来のメッシュは 1 回だけ積む。
    Merge = 30,
    // ひび割れ。道路面の上に 3〜6 m の枝分かれした割れ目の塊を乱数で配置し、帯メッシュで貼る。
    Crack = 31,
    // 3D モデル（.tgmodel）を 1 つ置く。出力（Model 型）を Mesh Output か Merge へ繋ぐとビューポートに出る。
    // 道路のメッシュとは別系統で、描画は Application が行う（置き方を変えても道路を作り直さない）。
    Model = 32,
    // 上流のモデルをまとめて移動・回転・拡大する。Model 型を受けて Model 型を出す。
    Transform = 33,
    // 材質。Road / Shoulder / Decal などの Material スロットへ渡す。
    Surface = 0,
    // 実寸の 3 次元カーブ。道路の線形や、面上に引いたデカールの経路。ビューポートで編集する。
    Path = 18,
};

struct PinDefinition {
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::Material;
    const char* label = "";
};

// ノードの静的な定義。種類・保存名・表示名・ピン構成をテーブルで持ち、
// CreateNode() がここからピンを生成する。
struct NodeDefinition {
    NodeKind kind = NodeKind::Surface;
    const char* name = "";   // 保存名（ファイルには enum の数値ではなくこれを書く）
    const char* title = "";  // 表示名
    std::span<const PinDefinition> pins;
};

struct Pin {
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::Material;
    std::string label;
};

// --- ノードの設定 ---------------------------------------------------------
//
// ノードを増やすときは、(1) 設定構造体を定義し、(2) NodeSettings へ足し、
// (3) NodeGraph.cpp の定義テーブルへ登録し、(4) 保存とプロパティ UI の
// 対応を足す。それ以外の場所を触る必要がないように保つ。

// サーフェス。既存のレイヤーそのもの。
struct LayerNodeSettings {
    compositor::MaterialLayer layer;
};

// パス（Path ノード）。点と向き付きのエッジ。中身は graph/Path.h。
struct PathNodeSettings {
    PathSettings path;
};

// 道路の材質スロット数。スロット 1 が下地、2〜4 は道路マスクの R / G / B で被覆する。
inline constexpr int kRoadMaterialSlots = 4;

struct RoadNodeSettings {
    float widthMeters = 6.0f;
    float uvRepeatMeters = 1.0f;
    // 車線数。線形の向きへ進む車線（forward）と対向車線（backward）。車線幅は全幅÷合計。
    // どちら側に並ぶかは走行側（RoadNetworkSettings）で決まる。対向 0 で一方通行。
    uint32_t lanesForward = 1;
    uint32_t lanesBackward = 1;
    // Material のハイトで路面を押し出す量（m）。ハイト 0〜1 の全幅がこの高さになる。
    float displacementMeters = 0.0f;
    // 真なら道路の長さ方向を U にする（既定は V）。横長のテクスチャを道路に沿わせるとき。
    bool uvAlongU = false;
    // スロットごとのテクスチャ座標。真ならワールド XZ 平面、偽なら道路 UV。
    bool layerWorldUv[kRoadMaterialSlots] = {false, false, false, false};
    // スロット 2〜4 の UV 反復長（m）。スロット 1 は uvRepeatMeters。
    float layerUvRepeatMeters[kRoadMaterialSlots] = {1.0f, 1.0f, 1.0f, 1.0f};
    // ハイトで競合させるときの境界の柔らかさ（ハイト 0〜1 の単位）。
    float layerBlendRange = 0.2f;
    // 下地のハイトで絞る（スロット 2〜4）。0 = 使わない、1 = 下地の高い所、2 = 下地の低い所。
    // Road Mask が「だいたいこの辺」を決め、下地（スロット 1）の凹凸が「その中のどこ」を決める。
    uint32_t layerHeightGate[kRoadMaterialSlots] = {0, 0, 0, 0};
    float layerHeightGateThreshold[kRoadMaterialSlots] = {0.5f, 0.5f, 0.5f, 0.5f};
    float layerHeightGateSoftness[kRoadMaterialSlots] = {0.2f, 0.2f, 0.2f, 0.2f};
    // 混ぜ方（スロット 2〜4）。0 = マスクどおり（被覆率がそのまま重み。境界だけ下地とのハイト差で崩す）、
    // 1 = ハイトで競合（被覆率をハイトに足して勝った方が出る）。旧ファイルは 1 で読む。
    uint32_t layerBlendMode[kRoadMaterialSlots] = {0, 0, 0, 0};
};

// 道路空間マスクの形。
enum class RoadMaskShape : uint32_t {
    WheelTracks = 0,  // 轍。車線中央 ± タイヤ間隔/2 の帯
    EdgeFalloff = 1,  // 道路端からの距離で減衰
    LengthNoise = 2,  // 長さ方向のノイズをしきい値で切る
    Constant = 3,     // 一様
    WorldNoise = 4,   // ワールド XZ の等方ノイズ（FBM）をしきい値で切る。路肩や地面と地続きの模様
};

// デカール。Path（Surface に道路を繋いだ面上のパス）に沿った幅 widthMeters の帯を、
// 道路面と一体で押し出される帯メッシュとして貼る。材質の不透明度で模様をくり抜く。
struct DecalNodeSettings {
    float heightMeters = 0.0f;
    float imageWidthScale = 1.0f;
    float imageLengthScale = 1.0f;
    bool showWireframe = false;
    std::optional<compositor::MaterialLayer> material;
    float widthMeters = 1.0f;
    float liftMeters = 0.008f;
    float uvRepeatMeters = 1.0f;
    bool uvAlongU = false;
};

// 路肩。境界の点列（Road の Left / Right、路肩の Outer）をそのまま内側の列にして、
// 外側へ widthMeters 押し出した格子。境界の頂点を共有するので道路と水密になる。
// 列 0 が内側（境界）、列末尾が外側（Outer）。進行方向の左右や走行側には依存しない。
struct ShoulderNodeSettings {
    float widthMeters = 1.5f;
    // 横断勾配（%）。正なら外側へ向かって下がる。
    float crossSlopePercent = 4.0f;
    // 舗装端の段差（m）。0 より大きいと、境界の直後に stepWidthMeters の面取り列を 1 つ挟み、そこで段差ぶん下げる。
    float stepHeightMeters = 0.0f;
    float stepWidthMeters = 0.05f;
    float uvRepeatMeters = 1.0f;
    bool uvAlongU = false;
    // 材質のレイヤー構造は Road と同じ（スロット × 4、Mask 2〜4、変位）。
    // 路肩の横位置は境界（列 0）が Right、外側が Left。Road Mask の「側」はその向きで読む。
    float displacementMeters = 0.0f;
    bool layerWorldUv[kRoadMaterialSlots] = {true, true, true, true};
    float layerUvRepeatMeters[kRoadMaterialSlots] = {1.0f, 1.0f, 1.0f, 1.0f};
    float layerBlendRange = 0.2f;
    // 下地のハイトで絞る（スロット 2〜4）。0 = 使わない、1 = 下地の高い所、2 = 下地の低い所。
    // Road Mask が「だいたいこの辺」を決め、下地（スロット 1）の凹凸が「その中のどこ」を決める。
    uint32_t layerHeightGate[kRoadMaterialSlots] = {0, 0, 0, 0};
    float layerHeightGateThreshold[kRoadMaterialSlots] = {0.5f, 0.5f, 0.5f, 0.5f};
    float layerHeightGateSoftness[kRoadMaterialSlots] = {0.2f, 0.2f, 0.2f, 0.2f};
    // 混ぜ方（スロット 2〜4）。0 = マスクどおり（被覆率がそのまま重み。境界だけ下地とのハイト差で崩す）、
    // 1 = ハイトで競合（被覆率をハイトに足して勝った方が出る）。旧ファイルは 1 で読む。
    uint32_t layerBlendMode[kRoadMaterialSlots] = {0, 0, 0, 0};
};

// モデル。model は Application のモデル一覧の ID（0 = なし）。position はモデルの底面の中心の位置（m）、
// 倍率はモデルアセットの倍率に掛ける。道路には依存しない。
// 回転は X・Y・Z 軸まわりの角度（度）で、Z → X → Y の順に回す（DirectX の RollPitchYaw と同じ）。
struct ModelNodeSettings {
    uint64_t model = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotationDegrees[3] = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
    // FBX のノードに足す回転（戦車の砲塔の旋回・砲身の俯仰など）。ノードの名前で指し、0 の回転は持たない。
    std::vector<renderer::ModelNodeRotation> nodeRotations;
};

// Transform。上流のモデルを、倍率 → 回転 → 平行移動の順に動かす（原点まわり）。
struct TransformNodeSettings {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotationDegrees[3] = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
};

// Merge。設定は持たない。道路のメッシュとモデルのどちらも受け、繋いだ枝を順に積む。
// 下流の白線・Decal は最初の道路の枝の面に乗る。出力の型は入力から決まる（モデルだけなら Model）。
struct MergeNodeSettings {};

// ひび割れの向き。縦は道路の長さ方向、横は車線を横切る。
enum class CrackOrientation : uint32_t {
    Longitudinal = 0,
    Transverse = 1,
    Mixed = 2,
};
// ひび割れの横位置の分布。
enum class CrackPlacement : uint32_t {
    Uniform = 0,      // 道路幅に一様
    WheelTracks = 1,  // 各車線の轍の位置
    Edges = 2,        // 道路端の近く
};
// ひび割れ。乱数種と密度から塊を置き、幹（ランダムウォークの折れ線）と枝を Decal と同じ帯メッシュで貼る。
// 生成結果は保存せず、設定から毎回作る。個別に直したいものは面上の Path ＋ Decal で手描きする。
struct CrackNodeSettings {
    std::optional<compositor::MaterialLayer> material;
    uint32_t seed = 1u;
    float densityPer100m = 4.0f;
    float lengthMinMeters = 3.0f;
    float lengthMaxMeters = 6.0f;
    CrackOrientation orientation = CrackOrientation::Mixed;
    float transverseRatio = 0.3f;      // 混合のときの横向きの割合
    float angleJitterDegrees = 20.0f;  // 向きのばらつき
    CrackPlacement placement = CrackPlacement::Uniform;
    float trunkWidthMeters = 0.08f;    // 幹の帯の幅。枝は branchWidthRatio 倍から先端で 0 へ絞る
    uint32_t branchesMin = 2u;
    uint32_t branchesMax = 4u;
    float branchLengthRatio = 0.45f;   // 幹の長さに対する枝の長さ
    float branchWidthRatio = 0.6f;
    float liftMeters = 0.008f;
    float uvRepeatMeters = 1.0f;
    bool uvAlongU = false;
};

// 端の減衰をどちらの端に出すか。左右は Path の進行方向基準（Road の Left / Right と同じ）。走行側には依存しない。
enum class RoadMaskSide : uint32_t {
    Both = 0,
    Left = 1,
    Right = 2,
};

struct RoadMaskNodeSettings {
    RoadMaskShape shape = RoadMaskShape::WheelTracks;
    // 轍。車線中央の中心線からの距離、タイヤ間隔、帯の幅、縁のぼかし。
    float laneOffsetMeters = 1.5f;
    float trackSpacingMeters = 1.5f;
    float trackWidthMeters = 0.35f;
    float featherMeters = 0.25f;
    // 真なら Road の車線数から各車線の中央に置く（laneOffsetMeters は使わない）。
    // 旧ファイルにキーが無ければ偽（手入力のまま）。路肩など車線の無い面では手入力に落ちる。
    bool tracksFromLanes = true;
    // 対向車線にも置くか（車線に合わせるとき）。手入力のときは中心線の左右両方に置くか。
    bool bothLanes = true;
    // 端の減衰。端で 1 になる幅と、その内側のぼかし幅。側を選ぶと片側の端だけになる。
    float edgeWidthMeters = 0.3f;
    RoadMaskSide edgeSide = RoadMaskSide::Both;
    // 長さ方向ノイズ。
    float noiseScaleMeters = 4.0f;
    float threshold = 0.5f;
    float softness = 0.2f;
    uint32_t seed = 1u;
    // 長さ方向のムラ（どの形にも掛かる）。0 で一様。
    float breakupAmount = 0.3f;
    float breakupScaleMeters = 3.0f;
    float strength = 1.0f;
    bool invert = false;
};

// 道路網に共通の設定。走行側は道路ごとではなくプロジェクトで 1 つ。
// 車線の進行方向・矢印・標識の向きの判定に使う。
struct RoadNetworkSettings {
    bool leftHandTraffic = true;
};

// 白線（Lane Marking）。寸法は m。外側線は道路端から中心線側へ edgeInsetMeters の位置に置く。
// 中央線・外側線・車線境界線・停止線・矢印の順。未指定は既定の白。
struct RoadMarkingNodeSettings {
    std::array<std::optional<compositor::MaterialLayer>, 5> materials;
    float centerLineWidthMeters = 0.15f;
    float edgeLineWidthMeters = 0.15f;
    float laneLineWidthMeters = 0.15f;
    // 中央線（進行方向と対向の境）。一方通行なら出ない。
    bool centerLine = true;
    bool centerLineDashed = false;
    bool edgeLines = true;
    float edgeInsetMeters = 0.5f;
    // 車線境界線。同方向の車線の間に破線で引く。
    bool laneLines = true;
    float dashLengthMeters = 5.0f;
    float dashGapMeters = 5.0f;
    // 停止線。Path の点に付けた停止線を、その向きの車線の幅いっぱいに引く。
    bool stopLines = true;
    float stopLineWidthMeters = 0.45f;
    // 進行方向の矢印。左右の車線の中央に一定間隔で置き、走行側に応じて向きを決める。
    bool arrows = true;
    float arrowIntervalMeters = 30.0f;
    float arrowLengthMeters = 5.0f;
    // 路面との重なりによるちらつきを避けるため、法線方向へ浮かせる量。
    float liftMeters = 0.005f;
    // 帯の長さ方向でVが1増える実距離。幅方向のUは帯の左端0〜右端1。
    float uvRepeatMeters = 1.0f;
    // 真なら長さ方向を U、幅方向を V にする（横長の白線テクスチャ向け）。
    bool uvAlongU = false;
};

// グラフを評価器の入力へ落とした結果。レイヤー列（道路の材質では Surface 1 枚）。
struct CompiledGraph {
    std::vector<compositor::MaterialLayer> layers;
    // レイヤーごとの元ノードの ID（添字は layers と同じ。既定の下地は 0）。
    // グラフパネルがノードに合成結果のサムネイルを出すのに使う。
    std::vector<GraphId> layerSources;
};

// 設定を持たないノード（Mesh Output）は std::monostate。
using NodeSettings =
    std::variant<LayerNodeSettings, PathNodeSettings, RoadNodeSettings, RoadMarkingNodeSettings,
                 RoadMaskNodeSettings, DecalNodeSettings, ShoulderNodeSettings, MergeNodeSettings,
                 CrackNodeSettings, ModelNodeSettings, TransformNodeSettings, std::monostate>;

struct Node {
    GraphId id = 0;
    NodeKind kind = NodeKind::Surface;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    NodeSettings settings;
    // エディタ上の位置。UI が読み書きし、保存にも含める。
    // positionValid が偽の間は UI が初期位置を与える。
    float posX = 0.0f;
    float posY = 0.0f;
    bool positionValid = false;
};

// 素材削除・Undo復元で、プロパティ内の参照もSurfaceと同じように扱う。
template<class Visitor>
void VisitNodeMaterialLayers(Node& node, const Visitor& visit) {
    if (auto* settings = std::get_if<LayerNodeSettings>(&node.settings)) visit(settings->layer);
    if (auto* settings = std::get_if<RoadMarkingNodeSettings>(&node.settings))
        for (auto& material : settings->materials) if (material) visit(*material);
    if (auto* settings = std::get_if<DecalNodeSettings>(&node.settings); settings && settings->material) visit(*settings->material);
    if (auto* settings = std::get_if<CrackNodeSettings>(&node.settings); settings && settings->material) visit(*settings->material);
}

struct Link {
    GraphId id = 0;
    GraphId startPin = 0;  // 出力ピン
    GraphId endPin = 0;    // 入力ピン
};

class NodeGraph {
public:
    // サーフェス（ベース）1 つだけの最小構成。
    static NodeGraph CreateDefault();

    const std::vector<Node>& Nodes() const { return m_nodes; }
    std::vector<Node>& MutableNodes() { return m_nodes; }
    const std::vector<Link>& Links() const { return m_links; }

    const Pin* FindPin(GraphId pinId) const;
    const Node* FindNode(GraphId nodeId) const;
    Node* FindMutableNode(GraphId nodeId);
    // 入力ピンに繋がっている上流ノード。無ければ nullptr。
    const Node* FindUpstreamNodeForPin(GraphId inputPinId) const;

    // 接続できるか。別ノード・型の相性・入出力の組み合わせに加えて、
    // **循環ができる接続は弾く**（評価が回らなくなるため）。Merge へ繋いで出力の型が変わるときは、
    // その下流が新しい型を受けられるかも見る。
    bool CanCreateLink(GraphId startPin, GraphId endPin) const;
    // 出力ピンが実際に運ぶ型。Merge の出力は、繋がった入力がモデルだけなら Model、道路が 1 本でもあれば Mesh、
    // 何も無ければ Any（どちらにも繋げる）。それ以外はピンの型そのもの。
    ValueType EffectiveOutputType(GraphId outputPin) const;
    // 入力ピンに繋がっている上流の出力ピン。無ければ 0。
    GraphId FindUpstreamPin(GraphId inputPinId) const;
    // output 型の出力を input 型の入力へ繋げるか。
    static bool TypesCompatible(ValueType output, ValueType input);
    // 接続する。入力ピンに既にある接続は置き換える。
    bool CreateLink(GraphId startPin, GraphId endPin);
    bool DeleteLink(GraphId linkId);
    // 定義テーブルからピンを生成してノードを足す。設定は既定値。
    GraphId CreateNode(NodeKind kind);
    bool DeleteNode(GraphId nodeId);

    // 読み込み用。ID はファイルの値をそのまま使い、次の採番を max+1 に合わせる。
    void Replace(std::vector<Node> nodes, std::vector<Link> links);

    // 道路網の共通設定（走行側）。変えたら再生成の対象なので改版する。
    const RoadNetworkSettings& RoadNetwork() const { return m_roadNetwork; }
    void SetRoadNetwork(const RoadNetworkSettings& settings) { m_roadNetwork = settings; MarkDirty(); }

    // 既定のレイヤー列。下地 1 枚（MaterialStack::MakeBaseLayer と同じもの）を返す。
    CompiledGraph CompileLayers() const;
    // 指定した Surface のレイヤー 1 枚をレイヤー列にする。
    // Road の材質スロットに繋いだ Surface の評価と、ノードを選んでのプレビューに使う。
    // Surface 以外のノードや無効な Surface はレイヤー列を持たないので CompileLayers() と同じ。
    // outputPin は互換のために残してある（Surface の出力は 1 本なので使わない）。
    CompiledGraph CompileLayersTo(GraphId nodeId, GraphId outputPin = 0) const;

    // 変更があったことを記録する。Application はこれを見て再コンパイルする。
    void MarkDirty() { ++m_revision; }
    // 入力数が可変のノード（Merge）の入力を整え、出力の型を入力に合わせ直す。
    // 入力を外して下流と型が合わなくなったリンクはここで外す。繋がった入力を順に残し、末尾に空きを 1 本置く。
    // リンクやノードを変えた後と読み込み後に呼ぶ。
    void NormalizeVariablePins();
    uint64_t Revision() const { return m_revision; }

private:
    GraphId AllocateGraphId() { return m_nextGraphId++; }
    RoadNetworkSettings m_roadNetwork;
    void RebuildNextGraphId();
    // Surface 1 つ（null なら無し）をレイヤー列にする共通部。
    CompiledGraph CompileSurface(const Node* surface) const;
    // producer の出力を辿って target に届くか（循環チェック用）。
    bool ReachesDownstream(GraphId fromNodeId, GraphId targetNodeId) const;
    // Merge の入力 replacedPin に replacement 型を繋いだとしたときの出力の型。
    ValueType MergeTypeWith(const Node& merge, GraphId replacedPin, ValueType replacement, int depth) const;
    // node（Merge）の出力が newType になっても、下流のリンクがすべて受けられるか。
    bool DownstreamAccepts(const Node& node, ValueType newType, int depth) const;

    std::vector<Node> m_nodes;
    std::vector<Link> m_links;
    GraphId m_nextGraphId = 1;
    uint64_t m_revision = 1;
};

std::span<const NodeDefinition> NodeDefinitions();
const NodeDefinition* FindNodeDefinition(NodeKind kind);
const NodeDefinition* FindNodeDefinitionByName(std::string_view name);
// レイヤー設定を持つ種類か（Surface）。
bool IsLayerNodeKind(NodeKind kind);
// 道路メッシュの鎖を成す種類か（Road / Lane Marking / Decal / Shoulder / Merge / Crack）。Mesh Output は含まない。
// 出力ピンを選ぶと、そのノードまでの鎖がメッシュシーンに出る。
bool IsMeshNodeKind(NodeKind kind);
// 選ぶとプレビューの対象になる種類か。Surface と Path、道路メッシュのノード。
// 道路メッシュのノードはそのノードまでの鎖をメッシュシーンに出す。
bool IsPreviewableNodeKind(NodeKind kind);
// モデルの系統の種類か（Model / Transform）。選ぶとその枝のモデルだけをプレビューに出す。
bool IsModelNodeKind(NodeKind kind);
// 入力数が可変の種類か（Merge）。
bool IsVariableInputNodeKind(NodeKind kind);

}  // namespace tg::graph
