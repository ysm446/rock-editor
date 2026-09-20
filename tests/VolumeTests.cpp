#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"
#include "app/UndoHistory.h"
#include <cmath>
#include <limits>

void RunVolumeTests() {
    using namespace rock;
    using tests::Check;
    tests::Section("直方体の塊とボリューム");
    geometry::BoxClusterSettings s;
    std::string error;
    const auto boxes = geometry::MakeBoxCluster(s, error);
    Check(error.empty() && boxes.size() == 8, "指定数の直方体を生成");
    Check(boxes == geometry::MakeBoxCluster(s, error), "Seed で直方体を再現");
    auto other = s;
    ++other.seed;
    Check(boxes != geometry::MakeBoxCluster(other, error), "Seed を変えると形が変わる");
    const std::vector<geometry::OrientedBox> first{boxes.front()};
    for (const auto& box : boxes)
        Check(geometry::BoxUnionField(box.center, first) < 0, "全ての直方体の中心が主塊の内部にある");
    geometry::MeshInfo preview;
    Check(geometry::InspectMesh(geometry::BoxClusterPreview(boxes), preview) && preview.closed &&
              preview.components == 8,
          "変換前は重なった8個の閉じた直方体");
    for (int seed : {1, 42, 101}) {
        s.seed = seed;
        auto sample = geometry::MakeBoxCluster(s, error);
        const auto grid = geometry::BoxesToVolume(sample, {48}, error);
        Check(error.empty() && !grid.values.empty() && grid.spacing > 0, "実体のある3次元スカラーグリッド");
        const auto surface = geometry::VolumeSurface(grid, error);
        Check(error.empty(), error.empty() ? "和集合の表面を抽出" : error.c_str());
        geometry::MeshInfo info;
        Check(geometry::InspectMesh(surface, info) && info.closed && info.components == 1 && info.volume > 0,
              "変換後は閉じた1連結体");
        bool nearSurface = true;
        for (const auto p : surface.positions)
            nearSurface &= std::abs(geometry::BoxUnionField(p, sample)) < grid.spacing * 1.8f;
        Check(nearSurface, "抽出点は和集合の外皮付近（内部の重複面は出さない）");
        bool borderOutside = true;
        for (uint32_t z = 0; z < grid.dimensions[2]; ++z)
            for (uint32_t y = 0; y < grid.dimensions[1]; ++y)
                for (uint32_t x = 0; x < grid.dimensions[0]; ++x)
                    if (x == 0 || y == 0 || z == 0 || x + 1 == grid.dimensions[0] ||
                        y + 1 == grid.dimensions[1] || z + 1 == grid.dimensions[2])
                        borderOutside &= grid.values[grid.Index(x, y, z)] > 0;
        Check(borderOutside, "外周には空の余白があり表面を切り落とさない");
    }
    s = {};
    s.count = 32;
    s.sizeVariation = .8f;
    s.spread = .95f;
    s.rotation = 90;
    const auto maximum = geometry::MakeBoxCluster(s, error);
    auto maxGrid = geometry::BoxesToVolume(maximum, {96}, error);
    auto maxSurface = geometry::VolumeSurface(maxGrid, error);
    Check(error.empty(), error.empty() ? "最大設定の抽出が成功" : error.c_str());
    geometry::MeshInfo maxInfo;
    Check(error.empty() && geometry::InspectMesh(maxSurface, maxInfo) && maxInfo.closed &&
              maxInfo.components == 2 && maxInfo.volume > 0,
          "最大設定では内部空洞の境界も含む閉じた和集合を抽出");
    Check(maxGrid.values.size() <= 102u * 102u * 102u, "格子のメモリ量を上限内に保つ");
    s = {};
    s.count = 1;
    s.rotation = 0;
    s.size = {2, 2, 2};
    const auto cube = geometry::MakeBoxCluster(s, error);
    double coarseError = 0;
    for (int resolution : {16, 48}) {
        const auto grid = geometry::BoxesToVolume(cube, {resolution}, error);
        const auto mesh = geometry::VolumeSurface(grid, error);
        geometry::MeshInfo info;
        Check(error.empty() && geometry::InspectMesh(mesh, info) && info.closed && info.components == 1,
              "格子と一致する単独 Box も閉じる");
        const double volumeError = std::abs(info.volume - 8);
        Check(volumeError < .3, "Box の解析体積に近い");
        if (resolution == 16)
            coarseError = volumeError;
        else
            Check(volumeError <= coarseError, "解像度を上げると解析体積へ近づく");
    }
    auto overlap = cube;
    overlap.push_back(cube[0]);
    overlap[1].center.x = 1;
    auto grid = geometry::BoxesToVolume(overlap, {48}, error);
    auto surface = geometry::VolumeSurface(grid, error);
    geometry::MeshInfo info;
    Check(error.empty() && geometry::InspectMesh(surface, info) && info.closed &&
              std::abs(info.volume - 12) < .4,
          "重なる2個の Box は重複体積を加算せず和集合になる");
    const auto repeated = geometry::BoxesToVolume(overlap, {48}, error);
    Check(grid.values == repeated.values && grid.dimensions == repeated.dimensions, "ボリュームを完全再現");
    s.count = 0;
    Check(geometry::MakeBoxCluster(s, error).empty() && !error.empty(), "不正な個数を診断");
    s = {};
    s.spread = std::numeric_limits<float>::quiet_NaN();
    Check(geometry::MakeBoxCluster(s, error).empty() && !error.empty(), "非有限値を拒否");
    Check(geometry::BoxesToVolume(boxes, {100000}, error).values.empty() && !error.empty(),
          "格子確保前に解像度上限を診断");
    Check(geometry::VolumeSurface({}, error).positions.empty() && !error.empty(), "空のグリッドを拒否");

    tests::Section("Boxes / Volume の型・評価・Undo");
    graph::NodeGraph graph;
    const auto source = graph.CreateNode(graph::NodeKind::RandomBoxes),
               volume = graph.CreateNode(graph::NodeKind::ToVolume),
               output = graph.CreateNode(graph::NodeKind::MeshOutput),
               crack = graph.CreateNode(graph::NodeKind::Crack),
               merge = graph.CreateNode(graph::NodeKind::Merge);
    const auto from = [&](auto id) { return graph.FindNode(id)->outputs[0].id; };
    const auto to = [&](auto id) { return graph.FindNode(id)->inputs[0].id; };
    Check(!graph.CanCreateLink(from(source), to(crack)) && !graph.CanCreateLink(from(volume), to(crack)) &&
              !graph.CanCreateLink(from(volume), to(merge)),
          "Boxes/Volume を Mesh と偽って渡さない");
    Check(graph.CreateLink(from(source), to(volume)) && graph.CreateLink(from(volume), to(output)),
          "Random Boxes → To Volume → Mesh Output を接続");
    std::get<geometry::VolumeSettings>(graph.FindMutableNode(volume)->settings).resolution = 24;
    const auto result = graph::EvaluateRocks(graph);
    Check(
        result.error.empty() && result.rocks.size() == 1 && result.rocks[0].volume && !result.rocks[0].boxes,
        "下流にボリューム本体と表示用外皮を渡す");
    const auto upstream = graph::EvaluateRocks(graph, source);
    Check(upstream.rocks.size() == 1 && upstream.rocks[0].boxes && !upstream.rocks[0].volume,
          "生成元の直方体集合を非破壊で保持");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes();
    before.graphLinks = graph.Links();
    std::get<geometry::BoxClusterSettings>(graph.FindMutableNode(source)->settings).seed = 9;
    std::get<geometry::VolumeSettings>(graph.FindMutableNode(volume)->settings).resolution = 16;
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes();
    after.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    const auto undone = graph::EvaluateRocks(graph);
    Check(undone.error.empty() && undone.rocks.size() == 1 && result.rocks.size() == 1 &&
              undone.rocks[0].volume && result.rocks[0].volume &&
              undone.rocks[0].volume->values == result.rocks[0].volume->values,
          "Undo で Seed・解像度とボリュームを再現");
    auto redone = history.Redo(restored);
    graph.Replace(redone.graphNodes, redone.graphLinks);
    Check(std::get<geometry::BoxClusterSettings>(graph.FindNode(source)->settings).seed == 9 &&
              std::get<geometry::VolumeSettings>(graph.FindNode(volume)->settings).resolution == 16,
          "Redo で編集を復元");

    tests::Section("Volume to Mesh の変換・接続・Undo");
    const auto converter = graph.CreateNode(graph::NodeKind::VolumeToMesh);
    Check(!graph.CanCreateLink(from(source), to(converter)) &&
              !graph.CanCreateLink(from(converter), to(volume)),
          "変換ノードは Volume 入力と Mesh 出力を区別");
    const auto missing = graph::EvaluateRocks(graph, converter);
    Check(missing.rocks.empty() && missing.error.find("Volume to Mesh") != std::string::npos,
          "未接続の入力を対象ノード名付きで診断");
    Check(graph.CanCreateLink(from(converter), to(merge)) &&
              graph.CanCreateLink(from(converter), to(crack)),
          "変換後は通常の Mesh 入力へ接続できる");
    const auto volumeBefore = graph::EvaluateRocks(graph, volume);
    DocumentSnapshot withoutConversion;
    withoutConversion.graphNodes = graph.Nodes();
    withoutConversion.graphLinks = graph.Links();
    Check(graph.CreateLink(from(volume), to(converter)) &&
              graph.CreateLink(from(converter), to(output)),
          "Random Boxes → To Volume → Volume to Mesh → Mesh Output を接続");
    const auto converted = graph::EvaluateRocks(graph);
    geometry::MeshInfo convertedInfo;
    Check(converted.error.empty() && converted.rocks.size() == 1 &&
              converted.rocks[0].source == converter && !converted.rocks[0].volume &&
              !converted.rocks[0].boxes && geometry::InspectMesh(converted.rocks[0].mesh, convertedInfo) &&
              convertedInfo.closed && convertedInfo.volume > 0,
          "独立ノードで閉じた Mesh を生成し Volume データと分離");
    const auto volumeAfter = graph::EvaluateRocks(graph, volume);
    Check(volumeBefore.rocks.size() == 1 && volumeAfter.rocks.size() == 1 &&
              volumeBefore.rocks[0].volume && volumeAfter.rocks[0].volume &&
              volumeBefore.rocks[0].volume->values == volumeAfter.rocks[0].volume->values &&
              converted.rocks.size() == 1 &&
              converted.rocks[0].mesh.positions == volumeBefore.rocks[0].mesh.positions &&
              converted.rocks[0].mesh.triangles == volumeBefore.rocks[0].mesh.triangles,
          "上流グリッドを変えず従来の Volume プレビューと同じ表面を生成");
    DocumentSnapshot withConversion;
    withConversion.graphNodes = graph.Nodes();
    withConversion.graphLinks = graph.Links();
    UndoHistory conversionHistory;
    conversionHistory.Push(withoutConversion, 0);
    const auto disconnected = conversionHistory.Undo(withConversion);
    graph.Replace(disconnected.graphNodes, disconnected.graphLinks);
    Check(graph.FindUpstreamNodeForPin(to(output))->id == volume &&
              !graph::EvaluateRocks(graph, converter).error.empty(),
          "Undo で従来の直接プレビュー接続に戻る");
    const auto reconnected = conversionHistory.Redo(disconnected);
    graph.Replace(reconnected.graphNodes, reconnected.graphLinks);
    const auto regenerated = graph::EvaluateRocks(graph);
    Check(regenerated.error.empty() && regenerated.rocks.size() == 1 &&
              regenerated.rocks[0].source == converter && converted.rocks.size() == 1 &&
              regenerated.rocks[0].mesh.positions == converted.rocks[0].mesh.positions &&
              regenerated.rocks[0].mesh.triangles == converted.rocks[0].mesh.triangles,
          "Redo で変換ノードの接続とメッシュを再現");
    Check(graph.CreateLink(from(converter), to(merge)) && graph.CreateLink(from(merge), to(output)) &&
              graph::EvaluateRocks(graph).error.empty(),
          "変換した Mesh を Merge 経由でも評価できる");

}
