#pragma once
#include "graph/SurfaceLayoutEvaluation.h"

namespace tg::graph {
// 道路最大3構成と左右各2構成を接続。存在しない側はID 0を渡す。
bool ConnectSurfaceLayoutBands(CompiledMeshGraph& scene, const NodeGraph& graph,
                               const SurfaceLayoutDocument& document, GraphId roadId,
                               SurfaceId leftBand, SurfaceId rightBand, std::string& error,
                               bool enableDisplacement = false);
// 左右各1帯を一括接続する。片側でも失敗した場合は元のシーンを保持する。
bool ConnectBothSurfaceBands(CompiledMeshGraph& scene, const NodeGraph& graph,
                             const SurfaceLayoutDocument& document, GraphId roadId,
                             SurfaceId leftBand, SurfaceId rightBand, std::string& error,
                             bool enableDisplacement = false);
bool CreateRoadsideExample(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId,
                          SurfaceSide side, std::string& error);
// 道路に隣接する沿道1帯の形状。左・右、全長を覆う区間列に対応。
// 材質変位・複数帯の積み上げ・境界契約の解決は呼び出し側の後続処理。
// 失敗時は出力を保持する。
bool BuildSurfaceBandGeometry(const RoadGeometry& road, const SurfaceLayoutDocument& document,
                              const SurfaceBand& band, renderer::MeshData& result, std::string& error);
// 最大3プリセットの下地PBR材質を沿道の区間比率で混合する。材質変位は未適用。
CompiledMeshGraph CompileSurfaceBandPreview(const NodeGraph& graph, const SurfaceLayoutDocument& document,
                                          GraphId roadId, SurfaceId bandId);
// 左右いずれか1帯の材質境界試作。道路1構成＋沿道最大2構成。失敗時はシーンを保持する。
// 成功時は道路を置換し沿道を追加する。変位有効時は共通境界で押し出しを抑え、世界Yへ変位する。
bool ConnectSurfaceBandMaterials(CompiledMeshGraph& scene, const NodeGraph& graph,
                                    const SurfaceLayoutDocument& document, GraphId roadId,
                                    SurfaceId bandId, std::string& error, bool enableDisplacement = false);
}  // namespace tg::graph
