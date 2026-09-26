#pragma once
#include "geometry/Displace.h"
#include "geometry/ShapeMask.h"
#include "geometry/Decimate.h"
#include "geometry/Remesh.h"
#include "geometry/UvUnwrap.h"
#include "geometry/Pieces.h"

#include "compositor/MaterialLayer.h"
#include "geometry/BaseRock.h"
#include "geometry/Volume.h"
#include "renderer/ModelAsset.h"

#include <array>
#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ノードグラフのデータモデル。
//
//   - ノード・ピン・リンクは共通の単一 ID 空間から採番する
//     （imgui-node-editor の NodeId / PinId / LinkId にそのまま流用できる）。
//   - ノードの設定は種類ごとの構造体を std::variant で持つ。
//   - 材質（Surface）は既存の GPU 評価器を使う。グラフは CompileLayersTo() で
//     レイヤー列へ落とし、MaterialStack として評価する。
//
// UI / D3D12 には依存しない（compositor のデータ構造にだけ依存する）。
namespace rock::graph {

using GraphId = int;

enum class PinKind : uint32_t {
    Input,
    Output,
};

// ピンを流れる値の型。同じ型どうしだけ接続できる。
enum class ValueType : uint32_t {
    MeshOrPieces = 15, // Scatter Points / Voronoi Fracture の入力用。
    Planes = 14,
    Mask = 13,
    Points = 10, Pieces = 11, Selection = 12,
    Material = 0,
    Mesh = 3,
    // 置いたモデルの集まり。Model / Transform が受け渡す。
    Model = 5,
    // メッシュとモデルのどちらでも受ける入力（Merge / Mesh Output）。
    // Merge の出力はこの型にはならず、入力から決まる（NodeGraph::EffectiveOutputType）。
    Any = 6,
    Boxes = 7,
    Volume = 8,
    Preview = 9,  // Mesh Output 専用。Mesh / Model / Boxes / Volume を表示する。
};

// 数値は保存名ではなくファイルには書かない（定義テーブルの name を書く）が、
// 撤去した種類の値は再利用しない。
enum class NodeKind : uint32_t {
    LayeredBoxes = 67,
    ParallelPlanes = 66,
    ApplyMaterial = 50, MaterialMask = 51,
    BaseRock = 34,
    // 35～37 は撤去した Crack / Fracture / Joint Set。
    RandomBoxes = 38,
    ToVolume = 39,
    VolumeToMesh = 40,
    VolumeTransform = 41,
    // 2つの Volume の和・交差・差。入力は A（基準の格子）と B。
    VolumeBoolean = 52,
    // 平面の群で Volume を切り落とし、角張った面を作る。
    PlaneCuts = 53,
    // 構造面、または点の群が作る Voronoi 境界に沿って、Volume の表面から割れ目を彫る。
    VolumeCrack = 54,
    // Volume の表面をノイズで削り、サンプル位置をずらして直線的な面を崩す。
    VolumeNoise = 55,
    // 形を保ったまま Mesh の三角形を減らす。UV Unwrap の前に置く。
    Decimate = 56,
    Subdivide = 57, Displace = 58,
    // 形状の遮蔽から作るマスク。UV付きの Mesh を受け、Apply Material の Mask へ渡す。
    ShapeMask = 59,
    // 2つのマスク（Shape Mask / Mask Combine）を画素ごとに合成する。出力は Mask。
    MaskCombine = 60,
    NoiseMask = 68, // UV付きMeshの3D座標からムラのマスクを作る。
    // Volume の表面をなまらせる / 角を立てる。面の向きで量を変えられる。
    VolumeSmooth = 61,
    // Volume にある方向の層状の段（棚）を作る。
    VolumeTerrace = 62,
    // Volume の幅より狭い隙間（割れ目の奥）を埋める。
    VolumeClose = 63,
    // Mesh の三角形を一様な大きさに作り直す（等方リメッシュ）。UV Unwrap の前に置く。
    Remesh = 64,
    // Volume の凸な稜線と角だけを削る（角の摩耗）。
    VolumeEdgeWear = 65,
    UvUnwrap = 42,
    MaterialBake = 43,
    ScatterPoints = 44, VoronoiFracture = 45, PieceSelect = 46,
    PieceFilter = 47, PieceTransform = 48, PiecesToMesh = 49,
    MeshOutput = 25,
    // 複数の Mesh の枝を 1 つにまとめる。同じノード由来のメッシュは 1 回だけ積む。
    Merge = 30,
    // 3D モデル（.rockmodel）を 1 つ置く。出力（Model 型）を Mesh Output か Merge
    // へ繋ぐとビューポートに出る。
    Model = 32,
    // 上流のモデルをまとめて移動・回転・拡大する。Model 型を受けて Model 型を出す。
    Transform = 33,
    // 材質。Material スロットへ渡す。
    Surface = 0,
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

// 母岩の CPU 設定をグラフと保存・Undo で共有する。
using BaseRockNodeSettings = geometry::BaseRockSettings;

// サーフェス。既存のレイヤーそのもの。
struct LayerNodeSettings {
    compositor::MaterialLayer layer;
};
using MaterialMaskSettings = compositor::MaterialMask;
// Apply Material。マスクに加えて、素材のハイトで合成する。
struct ApplyMaterialSettings {
    // 真なら、マスクを基準にこの素材のハイトが下地より高い所を前に出す（道路のレイヤーの「ハイトで競合」と同じ）。
    bool heightBlend = false;
    // 境目のなだらかさ。0.01〜1。小さいほどハイトの差でくっきり分かれる。
    float heightBlendRange = .2f;
    bool operator==(const ApplyMaterialSettings&) const = default;
};
struct MaterialBakeSettings {
    bool geometryAo = false;
    float aoDistance = 0.5f, aoStrength = 1;
    int aoSamples = 32;
    compositor::MaterialLayer bakedLayer;
    std::string fingerprint;
};

// モデル。model は Application のモデル一覧の ID（0 = なし）。position はモデルの底面の中心の位置（m）、
// 倍率はモデルアセットの倍率に掛ける。
// 回転は X・Y・Z 軸まわりの角度（度）で、Z → X → Y の順に回す（DirectX の RollPitchYaw と同じ）。
struct ModelNodeSettings {
    uint64_t model = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotationDegrees[3] = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
    // FBX のノードに足す回転。ノードの名前で指し、0 の回転は持たない。
    std::vector<renderer::ModelNodeRotation> nodeRotations;
};

// Transform。上流のモデルを、倍率 → 回転 → 平行移動の順に動かす（原点まわり）。
struct TransformNodeSettings {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotationDegrees[3] = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
};

// Merge。設定は持たない。メッシュとモデルのどちらも受け、繋いだ枝を順に積む。
// 出力の型は入力から決まる（モデルだけなら Model）。
struct MergeNodeSettings {};

// グラフを評価器の入力へ落とした結果。レイヤー列（Surface 1 枚）。
struct CompiledGraph {
    std::vector<compositor::MaterialLayer> layers;
    // レイヤーごとの元ノードの ID（添字は layers と同じ。既定の下地は 0）。
    // グラフパネルがノードに合成結果のサムネイルを出すのに使う。
    std::vector<GraphId> layerSources;
};

// 設定を持たないノード（Mesh Output）は std::monostate。
using NodeSettings = std::variant<LayerNodeSettings, MergeNodeSettings, ModelNodeSettings,
                                  TransformNodeSettings, BaseRockNodeSettings,
                                  geometry::BoxClusterSettings, geometry::VolumeSettings,
                                  geometry::VolumeTransformSettings, geometry::VolumeToMeshSettings,
                                  geometry::VolumeBooleanSettings, geometry::PlaneCutsSettings,
                                  geometry::LayeredBoxesSettings, geometry::ParallelPlanesSettings, geometry::VolumeCrackSettings, geometry::VolumeNoiseSettings,
                                  geometry::VolumeSmoothSettings, geometry::VolumeTerraceSettings, geometry::VolumeCloseSettings,
                                  geometry::VolumeEdgeWearSettings,
                                  geometry::DecimateSettings, geometry::RemeshSettings,
                                  geometry::SubdivideSettings, geometry::DisplaceSettings, geometry::UvUnwrapSettings, MaterialBakeSettings, MaterialMaskSettings,
                                  geometry::ShapeMaskSettings, geometry::NoiseMaskSettings, geometry::MaskCombineSettings, ApplyMaterialSettings,
                                  geometry::ScatterSettings, geometry::VoronoiSettings,
                                  geometry::PieceSelectSettings, geometry::PieceFilterSettings,
                                  geometry::PieceTransformSettings, std::monostate>;

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
    if (auto* settings = std::get_if<MaterialBakeSettings>(&node.settings)) visit(settings->bakedLayer);
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
    // 出力ピンが実際に運ぶ型。Merge の出力は、繋がった入力がモデルだけなら Model、メッシュが 1 つでもあれば Mesh、
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

    // 既定のレイヤー列。下地 1 枚（MaterialStack::MakeBaseLayer と同じもの）を返す。
    CompiledGraph CompileLayers() const;
    // 指定した Surface のレイヤー 1 枚をレイヤー列にする。
    // Material スロットに繋いだ Surface の評価と、ノードを選んでのプレビューに使う。
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

// ビューポートに出すモデル 1 つぶん。Model ノードと、そこから出力までに通る Transform（近い順）。
// 同じ Model ノードも、Merge の別の入力や別の Transform を通れば別のものとして出る。
struct ModelPlacementPath {
    GraphId model = 0;
    std::vector<GraphId> transforms;
};
// Mesh Output から Merge / Transform を辿ったモデル。previewNodeId がモデルの系統のノードか Merge ならその枝だけ、
// ほかのメッシュのノードなら何も出さない（途中経過を見ているとき）。
std::vector<ModelPlacementPath> CollectOutputModels(const NodeGraph& graph, GraphId previewNodeId = 0);

std::span<const NodeDefinition> NodeDefinitions();
const NodeDefinition* FindNodeDefinition(NodeKind kind);
const NodeDefinition* FindNodeDefinitionByName(std::string_view name);
// レイヤー設定を持つ種類か（Surface）。
bool IsLayerNodeKind(NodeKind kind);
// メッシュの鎖を成す種類か（Base Rock / Volume 系 / Merge など）。Mesh Output は含まない。
// 出力ピンを選ぶと、そのノードまでの鎖がメッシュシーンに出る。
bool IsMeshNodeKind(NodeKind kind);
bool IsPieceNodeKind(NodeKind kind);
// 選ぶとプレビューの対象になる種類か。
bool IsPreviewableNodeKind(NodeKind kind);
// モデルの系統の種類か（Model / Transform）。選ぶとその枝のモデルだけをプレビューに出す。
bool IsModelNodeKind(NodeKind kind);
// 入力数が可変の種類か（Merge）。
bool IsVariableInputNodeKind(NodeKind kind);
// UV空間の画像としてマスクを出す種類か（Shape Mask / Mask Combine）。選ぶと入力メッシュにマスクを貼って見せる。
bool IsImageMaskNodeKind(NodeKind kind);
// 画像マスクのノードの「反転」。使う側（描画・Displace・Subdivide）で 1 - mask にする分。
// Mask Combine は反転を画像に焼き込むので false。
bool ImageMaskInvert(const Node& node);

}  // namespace rock::graph
