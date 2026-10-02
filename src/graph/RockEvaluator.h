#pragma once
#include <atomic>
#include <string>
#include <memory>
#include <map>

#include "geometry/Mesh.h"
#include "graph/NodeGraph.h"
#include "graph/MaterialHeight.h"
namespace rock::graph {
struct GeneratedRock {
    GraphId source = 0;
    geometry::Mesh mesh;
    std::vector<GraphId> meshHistory;
    std::shared_ptr<const std::vector<geometry::OrientedBox>> boxes;
    std::shared_ptr<const geometry::VolumeGrid> volume;
    // Plane Cuts が作った結果だけが持つ。選択時に平面の枠を表示するために使う。
    std::shared_ptr<const geometry::PlaneCutsGuide> planeCuts;
    struct MaterialBinding {
        GraphId surface = 0, mask = 0;
        // 素材のハイトで合成する（Apply Material の設定）。
        bool heightBlend = false;
        float heightBlendRange = .2f;
        float opacity = 1;
        bool operator==(const MaterialBinding&) const = default;
    };
    std::vector<MaterialBinding> materials;
    GraphId materialSource = 0;
    GraphId bakeSource = 0;
    // 形状から作ったマスク（Shape Mask）。マスクのノード → このメッシュのUVに対応する画像。
    std::map<GraphId, std::shared_ptr<const geometry::MaskImage>> maskImages;
    // マスクのノード自身を評価した結果。入力メッシュに、このマスクを白黒のテクスチャとして貼って見せる。
    std::shared_ptr<const geometry::MaskImage> previewMask;
    bool previewMaskInvert = false;
    int pieceId = -1;
    bool pieceSelected = false;
    // Rock Asset が作った LOD（[0] は mesh と同じ LOD0）。Rock Asset 以外は空。
    std::shared_ptr<const std::vector<geometry::Mesh>> lods;
    // 先頭から何段が LOD0 と同じ UV を持つか。残りの段はそれぞれ自分の UV（とアトラス）を持つ。
    int sharedUvLods = 0;
    // Rock Asset が形を減らす前のメッシュ（bakeSource があるときだけ）。LOD は形が違うので、
    // Material Bake の結果がまだ使えるかは、焼いたときのこのメッシュで照らす。
    std::shared_ptr<const geometry::Mesh> bakeMesh;
    // Material Bake の High（ハイポリ）の内容のハッシュ。0 は未接続。変わったら焼き直しが要る。
    uint64_t bakeDetail = 0;
};
// 山グラフの Rock ノードが出す、岩グラフ（焼いた岩アセット）への参照。
struct RockReference {
    GraphId node = 0;
    std::string scene;  // 岩グラフのパス（UTF-8、絶対パス）
    float scale = 1.0f, weight = 1.0f;
    // 焼いた岩アセット（LOD0）の範囲（倍率 1、m）。目録から読む。未焼成なら hasBounds が false。
    bool hasBounds = false;
    geometry::Vec3 minimum{}, maximum{};
};
// 撒いた岩。岩グラフごとにまとめる。描画する側が岩アセットを読み、段を選んで描く。
struct RockInstanceSet {
    GraphId source = 0;  // 撒いたノード（Rock Scatter。Rock を選んで見ているときは Rock）
    std::string scene;
    float scale = 1.0f;  // Rock ノードの倍率
    std::vector<geometry::RockInstance> instances;
    // 撒いたノードの推定の被覆率（足元の面積の合計 ÷ 地形の面積。同じノードの全ての組で同じ値）。
    float coverage = 0.0f;
};
struct RockEvaluation {
    // 構造面の系統（Parallel Planes を連結した順）。
    std::shared_ptr<const std::vector<geometry::StructurePlanes>> planes;
    std::shared_ptr<const geometry::PointSet> points;
    std::shared_ptr<const geometry::PieceCollection> pieces;
    std::shared_ptr<const geometry::PieceSelection> selection;
    bool hasModels = false;
    std::vector<GeneratedRock> rocks;
    // 山グラフ。Rock ノードの参照と、撒いた岩。
    std::vector<RockReference> rockReferences;
    std::vector<RockInstanceSet> rockInstances;
    std::string error;
};
// ボリューム系の枝を設定と接続の内容で再利用する。アプリ単位で保持する。
struct RockEvaluationCache {
    struct Entry { std::string key; RockEvaluation result; };
    std::map<GraphId, Entry> entries;
    // キャッシュ対象ノードの実計算回数。再利用時は増加しない。
    std::map<GraphId, uint64_t> computations;
    std::map<GraphId, Entry> pieceEntries;
    // ピース系ノードの直近の出力（表示用。共有参照なので複製しない）。選択中のノードの稜線を描くのに使う。
    std::map<GraphId, std::shared_ptr<const geometry::PieceCollection>> pieceOutputs;
    struct Surface {
        std::shared_ptr<const geometry::VolumeGrid> volume;
        geometry::Mesh mesh;
        geometry::VolumeMeshingMethod method = geometry::VolumeMeshingMethod::MarchingTetrahedra;
    };
    std::map<GraphId, Surface> surfaces;
    struct UvEntry { geometry::Mesh input, output; geometry::UvUnwrapSettings settings; };
    std::map<GraphId, UvEntry> uvs;
    std::map<GraphId, std::pair<size_t,size_t>> detailCounts;
};
// 別スレッドで走る評価が、いま計算しているノードと段階を UI へ伝える。UI は読むだけ。
struct RockEvaluationProgress {
    std::atomic<GraphId> node{0};
    // ノードの中の段階（UV Unwrap は geometry::UvUnwrapStage + 1）。段階を持たないノードは 0。
    std::atomic<int> stage{0};
    std::atomic<int> percent{-1};  // 0～100。分からないときは -1。
};
RockEvaluation EvaluateRocks(const NodeGraph& graph, GraphId preview = 0,
                            RockEvaluationCache* persistent = nullptr,
                            geometry::VolumeMeshingMethod previewMethod = geometry::VolumeMeshingMethod::MarchingTetrahedra,
                            std::stop_token stop = {}, RockEvaluationProgress* progress = nullptr,
                            const MaterialHeight* heights = nullptr);
}  // namespace rock::graph
