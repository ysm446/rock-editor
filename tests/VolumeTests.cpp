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

    tests::Section("Volume Transform の移動・回転・拡大");
    geometry::BoxClusterSettings single;
    single.count = 1;
    single.rotation = 0;
    single.size = {2, 2, 2};
    const auto unitCube = geometry::MakeBoxCluster(single, error);
    const auto base = geometry::BoxesToVolume(unitCube, {32}, error);
    const auto measure = [](const geometry::VolumeGrid& g, geometry::MeshInfo& info) {
        std::string local;
        const auto mesh = geometry::VolumeSurface(g, local);
        return local.empty() && geometry::InspectMesh(mesh, info) && info.closed && info.components == 1;
    };
    geometry::MeshInfo baseInfo;
    Check(error.empty() && measure(base, baseInfo), "変換元の単独 Box のボリューム");
    const auto identity = geometry::TransformVolume(base, {}, error);
    geometry::MeshInfo identityInfo;
    Check(error.empty() && measure(identity, identityInfo) &&
              std::abs(identityInfo.volume - baseInfo.volume) < baseInfo.volume * .02 &&
              std::abs(identity.spacing - base.spacing) < 1e-6f,
          "既定の設定では形も体積もほぼ変わらない");
    geometry::VolumeTransformSettings move;
    move.position = {3, -1, .5f};
    const auto moved = geometry::TransformVolume(base, move, error);
    geometry::MeshInfo movedInfo;
    Check(error.empty() && measure(moved, movedInfo) &&
              std::abs((movedInfo.minimum.x - baseInfo.minimum.x) - 3) < base.spacing &&
              std::abs((movedInfo.minimum.y - baseInfo.minimum.y) + 1) < base.spacing &&
              std::abs((movedInfo.maximum.z - baseInfo.maximum.z) - .5f) < base.spacing &&
              std::abs(movedInfo.volume - baseInfo.volume) < baseInfo.volume * .02,
          "移動は体積を保ったまま境界だけをずらす");
    Check(moved.values.size() == identity.values.size(), "移動だけでは格子の大きさが変わらない");
    geometry::VolumeTransformSettings spin;
    spin.rotationDegrees = {0, 45, 0};
    const auto spun = geometry::TransformVolume(base, spin, error);
    geometry::MeshInfo spunInfo;
    Check(error.empty() && measure(spun, spunInfo) &&
              std::abs(spunInfo.volume - baseInfo.volume) < baseInfo.volume * .05,
          "45度回転でも閉じた1連結体と体積を保つ");
    Check(spunInfo.maximum.x - spunInfo.minimum.x > (baseInfo.maximum.x - baseInfo.minimum.x) * 1.2f,
          "回転した Box の外接箱が対角方向へ広がる");
    geometry::VolumeTransformSettings grow;
    grow.scale = 2;
    const auto grown = geometry::TransformVolume(base, grow, error);
    geometry::MeshInfo grownInfo;
    Check(error.empty() && measure(grown, grownInfo) &&
              std::abs(grownInfo.volume - baseInfo.volume * 8) < baseInfo.volume * 8 * .03,
          "倍率2で体積が8倍になる");
    Check(grown.values.size() == identity.values.size() &&
              std::abs(grown.spacing - base.spacing * 2) < 1e-6f,
          "倍率を上げてもセル数は変えずセル間隔を比例させる");
    Check(geometry::TransformVolume(base, grow, error).values == grown.values, "変換結果を完全再現");
    geometry::VolumeTransformSettings chain;
    chain.position = {1, 0, 0};
    chain.rotationDegrees = {0, 90, 0};
    chain.scale = .5f;
    const auto chained = geometry::TransformVolume(base, chain, error);
    geometry::MeshInfo chainedInfo;
    Check(error.empty() && measure(chained, chainedInfo) &&
              std::abs(chainedInfo.volume - baseInfo.volume / 8) < baseInfo.volume / 8 * .05,
          "倍率→回転→移動を続けても閉じた形を保つ");
    geometry::VolumeTransformSettings invalid;
    invalid.scale = 0;
    Check(geometry::TransformVolume(base, invalid, error).values.empty() && !error.empty(), "0 倍率を診断");
    invalid = {};
    invalid.position[0] = std::numeric_limits<float>::quiet_NaN();
    Check(geometry::TransformVolume(base, invalid, error).values.empty() && !error.empty(),
          "非有限の移動量を拒否");
    Check(geometry::TransformVolume({}, {}, error).values.empty() && !error.empty(), "空の格子を拒否");
    const auto dense = geometry::BoxesToVolume(unitCube, {96}, error);
    Check(geometry::TransformVolume(dense, spin, error).values.empty() && !error.empty(),
          "回転で格子上限を超える場合は解像度を下げるよう診断");

    tests::Section("Volume Transform の型・評価・Undo");
    graph::NodeGraph moveGraph;
    const auto boxSource = moveGraph.CreateNode(graph::NodeKind::RandomBoxes),
               toVolume = moveGraph.CreateNode(graph::NodeKind::ToVolume),
               transform = moveGraph.CreateNode(graph::NodeKind::VolumeTransform),
               toMesh = moveGraph.CreateNode(graph::NodeKind::VolumeToMesh),
               viewer = moveGraph.CreateNode(graph::NodeKind::MeshOutput);
    const auto out = [&](auto id) { return moveGraph.FindNode(id)->outputs[0].id; };
    const auto in = [&](auto id) { return moveGraph.FindNode(id)->inputs[0].id; };
    Check(!moveGraph.CanCreateLink(out(boxSource), in(transform)) &&
              !moveGraph.CanCreateLink(out(toMesh), in(transform)),
          "Boxes や Mesh を Volume と偽って受けない");
    Check(moveGraph.CanCreateLink(out(transform), in(viewer)) &&
              moveGraph.CanCreateLink(out(transform), in(toMesh)),
          "Volume 出力はプレビューと Volume to Mesh へ繋げる");
    const auto unconnected = graph::EvaluateRocks(moveGraph, transform);
    Check(unconnected.rocks.empty() && unconnected.error.find("Volume Transform") != std::string::npos,
          "未接続の入力を対象ノード名付きで診断");
    Check(moveGraph.CreateLink(out(boxSource), in(toVolume)) &&
              moveGraph.CreateLink(out(toVolume), in(transform)) &&
              moveGraph.CreateLink(out(transform), in(toMesh)) &&
              moveGraph.CreateLink(out(toMesh), in(viewer)),
          "Random Boxes → To Volume → Volume Transform → Volume to Mesh → Mesh Output を接続");
    std::get<geometry::VolumeSettings>(moveGraph.FindMutableNode(toVolume)->settings).resolution = 24;
    const auto untouched = graph::EvaluateRocks(moveGraph);
    geometry::MeshInfo untouchedInfo;
    Check(untouched.error.empty() && untouched.rocks.size() == 1 &&
              geometry::InspectMesh(untouched.rocks[0].mesh, untouchedInfo) && untouchedInfo.closed,
          "既定の設定を通しても閉じたメッシュになる");
    const auto sourceBefore = graph::EvaluateRocks(moveGraph, toVolume);
    DocumentSnapshot beforeMove;
    beforeMove.graphNodes = moveGraph.Nodes();
    beforeMove.graphLinks = moveGraph.Links();
    std::get<geometry::VolumeTransformSettings>(moveGraph.FindMutableNode(transform)->settings).position = {
        5, 0, 0};
    const auto shifted = graph::EvaluateRocks(moveGraph);
    geometry::MeshInfo shiftedInfo;
    Check(shifted.error.empty() && shifted.rocks.size() == 1 &&
              geometry::InspectMesh(shifted.rocks[0].mesh, shiftedInfo) && shiftedInfo.closed &&
              shiftedInfo.minimum.x > untouchedInfo.maximum.x,
          "移動した結果が下流のメッシュへ伝わる");
    const auto sourceAfter = graph::EvaluateRocks(moveGraph, toVolume);
    Check(sourceBefore.rocks.size() == 1 && sourceAfter.rocks.size() == 1 &&
              sourceBefore.rocks[0].volume && sourceAfter.rocks[0].volume &&
              sourceBefore.rocks[0].volume->origin == sourceAfter.rocks[0].volume->origin &&
              sourceBefore.rocks[0].volume->values == sourceAfter.rocks[0].volume->values,
          "上流のボリュームは非破壊で保持");
    DocumentSnapshot afterMove;
    afterMove.graphNodes = moveGraph.Nodes();
    afterMove.graphLinks = moveGraph.Links();
    UndoHistory moveHistory;
    moveHistory.Push(beforeMove, 0);
    const auto back = moveHistory.Undo(afterMove);
    moveGraph.Replace(back.graphNodes, back.graphLinks);
    const auto restoredMesh = graph::EvaluateRocks(moveGraph);
    Check(restoredMesh.error.empty() && restoredMesh.rocks.size() == 1 && untouched.rocks.size() == 1 &&
              restoredMesh.rocks[0].mesh.positions == untouched.rocks[0].mesh.positions,
          "Undo で移動前のメッシュへ戻る");
    const auto forward = moveHistory.Redo(back);
    moveGraph.Replace(forward.graphNodes, forward.graphLinks);
    Check(std::get<geometry::VolumeTransformSettings>(moveGraph.FindNode(transform)->settings).position[0] ==
              5,
          "Redo で移動量を復元");
}
