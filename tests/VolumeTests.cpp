#include "TestSupport.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"
#include "app/UndoHistory.h"
#include <cmath>
#include <limits>
#include <chrono>

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
        const auto dual = geometry::VolumeSurface(grid, error, geometry::VolumeMeshingMethod::DualContouring);
        geometry::MeshInfo dualInfo;
        Check(error.empty(), error.empty() ? "Dual Contouring で和集合を抽出" : error.c_str());
        Check(geometry::InspectMesh(dual, dualInfo) && dualInfo.closed && dualInfo.volume > 0 &&
                  std::abs(dualInfo.volume - info.volume) < info.volume * .05,
              "Dual Contouring は閉包と和集合の体積を保つ");
        Check(dual.triangles.size() < surface.triangles.size(), "Dual Contouring は少ない三角形で表面を表現");
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
    const auto maxDual = geometry::VolumeSurface(maxGrid, error, geometry::VolumeMeshingMethod::DualContouring);
    geometry::MeshInfo maxDualInfo;
    Check(error.empty(), error.empty() ? "Dual Contouring の最大設定" : error.c_str());
    Check(geometry::InspectMesh(maxDual, maxDualInfo) && maxDualInfo.closed && maxDualInfo.components > 1 &&
              std::abs(maxDualInfo.volume - maxInfo.volume) < maxInfo.volume * .02,
          "Dual Contouring は内部空洞を埋めず体積を保つ");
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
    Check(geometry::VolumeSurface({}, error, geometry::VolumeMeshingMethod::DualContouring).positions.empty() &&
              !error.empty(), "Dual Contouring でも空のグリッドを拒否");
    Check(geometry::VolumeSurface(grid, error, static_cast<geometry::VolumeMeshingMethod>(99)).positions.empty() &&
              !error.empty(), "不明な変換方式を拒否");

    tests::Section("Dual Contouring の解析形状");
    for (float angle : {0.f, 15.f, 37.f}) {
        auto angled = cube;
        const float radians = angle * 3.14159265358979323846f / 180;
        angled[0].axes = {geometry::Vec3{std::cos(radians), 0, -std::sin(radians)},
                         geometry::Vec3{0, 1, 0}, geometry::Vec3{std::sin(radians), 0, std::cos(radians)}};
        const auto samples = geometry::BoxesToVolume(angled, {24}, error);
        const auto dual = geometry::VolumeSurface(samples, error, geometry::VolumeMeshingMethod::DualContouring);
        geometry::MeshInfo dualInfo;
        Check(error.empty() && geometry::InspectMesh(dual, dualInfo) && dualInfo.closed &&
                  std::abs(dualInfo.volume - 8) < .4, "斜めのBoxも閉包と解析体積を保つ");
        const auto same = geometry::VolumeSurface(samples, error, geometry::VolumeMeshingMethod::DualContouring);
        Check(dual.positions == same.positions && dual.triangles == same.triangles, "Dual Contouring の決定性");
        bool nearSurface = !dual.positions.empty();
        for (const auto& p : dual.positions)
            nearSurface &= std::abs(geometry::BoxUnionField(p, angled)) < samples.spacing;
        Check(nearSurface, "角の頂点が元のBox表面から1セル以上飛び出さない");
    }
    geometry::VolumeGrid sphere;
    sphere.dimensions = {25, 25, 25};
    sphere.spacing = .1f;
    sphere.origin = {-1.2f, -1.2f, -1.2f};
    sphere.values.resize(25 * 25 * 25);
    for (uint32_t z = 0; z < 25; ++z) for (uint32_t y = 0; y < 25; ++y) for (uint32_t x = 0; x < 25; ++x) {
        const auto p = sphere.Position(x, y, z);
        sphere.values[sphere.Index(x,y,z)] = std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z) - .93f;
    }
    const auto dualSphere = geometry::VolumeSurface(sphere, error, geometry::VolumeMeshingMethod::DualContouring);
    geometry::MeshInfo sphereInfo;
    Check(error.empty() && geometry::InspectMesh(dualSphere, sphereInfo) && sphereInfo.closed &&
              std::abs(sphereInfo.volume - 4.0/3 * 3.141592653589793 * .93*.93*.93) < .08,
          "球のSDFから曲面と解析体積を再現");
    auto shell = sphere;
    for (auto& value : shell.values) {
        const float radius = value + .93f;
        value = std::max(radius - .93f, .45f - radius);
    }
    const auto dualShell = geometry::VolumeSurface(shell, error, geometry::VolumeMeshingMethod::DualContouring);
    geometry::MeshInfo shellInfo;
    Check(error.empty() && geometry::InspectMesh(dualShell, shellInfo) && shellInfo.closed && shellInfo.components == 2 &&
              std::abs(shellInfo.volume - 4.0/3 * 3.141592653589793 * (.93*.93*.93 - .45*.45*.45)) < .1,
          "球殻は内壁を外向きに反転し空洞の解析体積を保つ");

    tests::Section("Boxes / Volume の型・評価・Undo");
    graph::NodeGraph graph;
    const auto source = graph.CreateNode(graph::NodeKind::RandomBoxes),
               volume = graph.CreateNode(graph::NodeKind::ToVolume),
               output = graph.CreateNode(graph::NodeKind::MeshOutput),
               meshInput = graph.CreateNode(graph::NodeKind::UvUnwrap),
               merge = graph.CreateNode(graph::NodeKind::Merge);
    const auto from = [&](auto id) { return graph.FindNode(id)->outputs[0].id; };
    const auto to = [&](auto id) { return graph.FindNode(id)->inputs[0].id; };
    Check(!graph.CanCreateLink(from(source), to(meshInput)) && !graph.CanCreateLink(from(volume), to(meshInput)) &&
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
              graph.CanCreateLink(from(converter), to(meshInput)),
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
    DocumentSnapshot tetraSnapshot;
    tetraSnapshot.graphNodes = graph.Nodes();
    tetraSnapshot.graphLinks = graph.Links();
    graph::RockEvaluationCache methodCache;
    graph::EvaluateRocks(graph, 0, &methodCache);
    const auto sharedVolume = methodCache.entries.at(volume).result.rocks[0].volume;
    std::get<geometry::VolumeToMeshSettings>(graph.FindMutableNode(converter)->settings).method =
        geometry::VolumeMeshingMethod::DualContouring;
    const auto dualConverted = graph::EvaluateRocks(graph, 0, &methodCache);
    const auto dualFresh = graph::EvaluateRocks(graph);
    Check(dualConverted.error.empty() && dualConverted.rocks.size() == 1 && dualFresh.rocks.size() == 1 &&
              dualConverted.rocks[0].mesh.positions == dualFresh.rocks[0].mesh.positions &&
              dualConverted.rocks[0].mesh.triangles != converted.rocks[0].mesh.triangles &&
              methodCache.entries.at(volume).result.rocks[0].volume == sharedVolume,
          "方式変更はメッシュだけ再生成し上流SDFを再利用");
    DocumentSnapshot dualSnapshot;
    dualSnapshot.graphNodes = graph.Nodes();
    dualSnapshot.graphLinks = graph.Links();
    UndoHistory methodHistory;
    methodHistory.Push(tetraSnapshot, 0);
    const auto tetraRestored = methodHistory.Undo(dualSnapshot);
    graph.Replace(tetraRestored.graphNodes, tetraRestored.graphLinks);
    Check(graph::EvaluateRocks(graph, 0, &methodCache).rocks[0].mesh.positions == converted.rocks[0].mesh.positions,
          "Undoで変換方式と従来メッシュを復元");
    const auto dualRestored = methodHistory.Redo(tetraRestored);
    graph.Replace(dualRestored.graphNodes, dualRestored.graphLinks);
    Check(graph::EvaluateRocks(graph, 0, &methodCache).rocks[0].mesh.positions == dualConverted.rocks[0].mesh.positions,
          "RedoでDual Contouringのメッシュを復元");
    graph.Replace(tetraSnapshot.graphNodes, tetraSnapshot.graphLinks);
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
    for (const std::array<float, 3> rotation : {
             std::array<float, 3>{0, 0, 0}, {0, 1, 0}, {0, 15, 0}, {0, 45, 0}, {0, 90, 0},
             {0, -45, 0}, {0, 180, 0}, {0, 360, 0}, {35, 45, 30}}) {
        auto denseSettings = spin;
        denseSettings.rotationDegrees = rotation;
        const auto rotated = geometry::TransformVolume(dense, denseSettings, error);
        geometry::MeshInfo denseInfo;
        Check(error.empty() && measure(rotated, denseInfo) &&
                  std::abs(denseInfo.volume - 8) < .4 && rotated.spacing == dense.spacing,
              "最大解像度の回転でも精度と閉包・体積を保って表面を抽出");
    }
    auto excessive = dense;
    excessive.dimensions[0] = 193;
    Check(geometry::TransformVolume(excessive, spin, error).values.empty() && !error.empty(),
          "上限を超える入力グリッドは確保前に拒否");
    geometry::VolumeGrid wide;
    wide.dimensions = {192, 2, 192};
    wide.spacing = 1;
    wide.values.resize(size_t(192) * 2 * 192, 1);
    Check(geometry::TransformVolume(wide, spin, error).values.empty() && !error.empty(),
          "回転後の格子が新しい上限を超える場合も確保前に拒否");

    geometry::BoxClusterSettings regressionBoxes;
    regressionBoxes.count = 11;
    regressionBoxes.size = {1, 2.4000000953674316f, 1.7999999523162842f};
    regressionBoxes.rotation = 73.60299682617188f;
    regressionBoxes.seed = 285425088;
    const auto regressionGrid = geometry::BoxesToVolume(
        geometry::MakeBoxCluster(regressionBoxes, error), {64}, error);
    for (const std::array<float, 3> rotation : {
             std::array<float, 3>{0, 15, 0}, {29.097206f, -166.721252f, -114.63147f},
             {29.097206f, -151.721252f, -114.63147f}}) {
        geometry::VolumeTransformSettings settings;
        settings.position[0] = .2700706124f;
        settings.rotationDegrees = rotation;
        const auto rotated = geometry::TransformVolume(regressionGrid, settings, error);
        geometry::MeshInfo regressionInfo;
        const auto regressionMesh = error.empty() ? geometry::VolumeSurface(rotated, error) : geometry::Mesh{};
        Check(error.empty() && geometry::InspectMesh(regressionMesh, regressionInfo) && regressionInfo.closed,
              "保存済みシーンの直方体設定でY15度と複合回転を表示可能");
        std::printf("  Rotation regression dimensions: %u x %u x %u, error: %s\n",
                    rotated.dimensions[0], rotated.dimensions[1], rotated.dimensions[2], error.c_str());
    }

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

    tests::Section("ボリュームの永続キャッシュ");
    graph::RockEvaluationCache cache;
    const auto compare = [&]() {
        const auto cached = graph::EvaluateRocks(moveGraph, 0, &cache);
        const auto fresh = graph::EvaluateRocks(moveGraph);
        return cached.error == fresh.error && cached.rocks.size() == fresh.rocks.size() &&
               (cached.rocks.empty() ||
                (cached.rocks[0].mesh.positions == fresh.rocks[0].mesh.positions &&
                 cached.rocks[0].mesh.triangles == fresh.rocks[0].mesh.triangles));
    };
    Check(compare(), "初回のキャッシュ評価と通常評価が一致");
    const auto originalVolume = cache.entries.at(toVolume).result.rocks[0].volume;
    auto& moving = std::get<geometry::VolumeTransformSettings>(moveGraph.FindMutableNode(transform)->settings);
    moving.rotationDegrees[1] = 13;
    Check(compare() && cache.entries.at(toVolume).result.rocks[0].volume == originalVolume,
          "回転時に上流SDFを再利用し、出力は通常評価と一致");
    moving.position[0] = 2;
    Check(compare(), "移動後のキャッシュ出力が通常評価と一致");
    std::get<geometry::VolumeSettings>(moveGraph.FindMutableNode(toVolume)->settings).resolution = 28;
    Check(compare() && cache.entries.at(toVolume).result.rocks[0].volume != originalVolume,
          "解像度変更で下流を含めてキャッシュを更新");
    ++std::get<geometry::BoxClusterSettings>(moveGraph.FindMutableNode(boxSource)->settings).seed;
    Check(compare(), "Seed変更を下流に反映");
    moveGraph.Replace(beforeMove.graphNodes, beforeMove.graphLinks);
    Check(compare(), "Undo相当のグラフ復元で古いキャッシュを使用しない");
    Check(moveGraph.CreateLink(out(toVolume), in(toMesh)) && compare(),
          "接続変更で変換を迂回した出力も一致");
    Check(moveGraph.CreateLink(out(transform), in(toMesh)), "変換の接続を復元");
    const auto direct = graph::EvaluateRocks(moveGraph, transform, &cache);
    const auto directAgain = graph::EvaluateRocks(moveGraph, transform, &cache);
    Check(direct.error.empty() && directAgain.error.empty() &&
              direct.rocks[0].volume == directAgain.rocks[0].volume &&
              direct.rocks[0].mesh.positions == directAgain.rocks[0].mesh.positions,
          "Volume直接プレビューもSDFと外皮を再利用");
    tests::Section("SDFプレビューの共通変換方式");
    const auto sameMesh = [](const geometry::Mesh& a, const geometry::Mesh& b) {
        return a.positions == b.positions && a.triangles == b.triangles;
    };
    const auto dualMethod = geometry::VolumeMeshingMethod::DualContouring;
    for (const auto node : {toVolume, transform}) {
        const auto tetraPreview = graph::EvaluateRocks(moveGraph, node, &cache);
        const auto dualPreview = graph::EvaluateRocks(moveGraph, node, &cache, dualMethod);
        Check(tetraPreview.error.empty() && dualPreview.error.empty() && !dualPreview.rocks.empty(),
              "To VolumeとVolume Transformの共通プレビュー方式を切り替え");
        if (!tetraPreview.rocks.empty() && !dualPreview.rocks.empty()) {
            const auto& volumeData = dualPreview.rocks[0].volume;
            const auto expected = geometry::VolumeSurface(*volumeData, error, dualMethod);
            Check(volumeData == tetraPreview.rocks[0].volume &&
                      sameMesh(expected, dualPreview.rocks[0].mesh) &&
                      !sameMesh(tetraPreview.rocks[0].mesh, dualPreview.rocks[0].mesh),
                  "SDFを再利用し、共通設定どおりの外皮だけを再生成");
            const auto reverted = graph::EvaluateRocks(moveGraph, node, &cache);
            Check(!reverted.rocks.empty() && sameMesh(reverted.rocks[0].mesh, tetraPreview.rocks[0].mesh),
                  "方式を戻しても別方式の外皮キャッシュを使わない");
        }
    }
    const auto explicitTetra = graph::EvaluateRocks(moveGraph, 0, &cache);
    const auto explicitWithDualPreview = graph::EvaluateRocks(moveGraph, 0, &cache, dualMethod);
    Check(!explicitTetra.rocks.empty() && !explicitWithDualPreview.rocks.empty() &&
              sameMesh(explicitTetra.rocks[0].mesh, explicitWithDualPreview.rocks[0].mesh),
          "共通プレビューを変更してもVolume to Meshの出力は変わらない");
    Check(moveGraph.CreateLink(out(transform), in(viewer)), "VolumeをMesh Outputへ直接接続");
    const auto outputPreview = graph::EvaluateRocks(moveGraph, 0, &cache, dualMethod);
    const auto transformPreview = graph::EvaluateRocks(moveGraph, transform, &cache, dualMethod);
    Check(!outputPreview.rocks.empty() && !transformPreview.rocks.empty() &&
              sameMesh(outputPreview.rocks[0].mesh, transformPreview.rocks[0].mesh),
          "Mesh OutputへのVolume直接接続にも共通設定を適用");
    Check(moveGraph.CreateLink(out(toMesh), in(viewer)), "Mesh Outputの明示的な変換を復元");
    tests::Section("Mesh Outputの材質接続");
    const auto surfaceNode = moveGraph.CreateNode(graph::NodeKind::Surface);
    const auto materialPin = moveGraph.FindNode(viewer)->inputs[1].id;
    Check(moveGraph.CreateLink(out(surfaceNode), materialPin), "SurfaceをMaterial入力へ接続");
    Check(!moveGraph.CanCreateLink(out(surfaceNode), in(viewer)) &&
              !moveGraph.CanCreateLink(out(toMesh), materialPin), "材質と形状の型を分離");
    const auto materialResult = graph::EvaluateRocks(moveGraph, 0, &cache);
    Check(materialResult.error.empty() && materialResult.rocks.size() == 1 &&
              materialResult.rocks[0].materialSource == surfaceNode &&
              sameMesh(materialResult.rocks[0].mesh, explicitTetra.rocks[0].mesh), "材質を付けても形状を維持");
    const auto beforeMaterialEdit = graph::EvaluateRocks(moveGraph, transform, &cache);
    std::get<graph::LayerNodeSettings>(moveGraph.FindMutableNode(surfaceNode)->settings).layer.mapping.method =
        compositor::MappingMethod::Triplanar;
    moveGraph.MarkDirty();
    const auto afterMaterialEdit = graph::EvaluateRocks(moveGraph, transform, &cache);
    Check(beforeMaterialEdit.rocks[0].volume == afterMaterialEdit.rocks[0].volume &&
              afterMaterialEdit.rocks[0].materialSource == 0, "材質変更時にSDFを再利用し上流プレビューへ漏らさない");
    moveGraph.DeleteNode(surfaceNode);
    Check(graph::EvaluateRocks(moveGraph, 0, &cache).rocks[0].materialSource == 0, "材質削除後は未割当へ戻る");
    moveGraph.DeleteNode(boxSource);
    Check(compare() && !cache.entries.contains(boxSource) && !cache.entries.contains(toVolume),
          "上流削除時はキャッシュを破棄して入力エラーを返す");

    // UI/GPU転送を含まない、ドラッグに相当する連続更新の比較。時間は合否条件にしない。
    moveGraph.Replace(beforeMove.graphNodes, beforeMove.graphLinks);
    std::get<geometry::VolumeSettings>(moveGraph.FindMutableNode(toVolume)->settings).resolution = 48;
    const auto measureDrag = [&](graph::RockEvaluationCache* retained) {
        graph::EvaluateRocks(moveGraph, 0, retained);
        const auto start = std::chrono::steady_clock::now();
        for (int step = 0; step < 6; ++step) {
            auto& settings = std::get<geometry::VolumeTransformSettings>(moveGraph.FindMutableNode(transform)->settings);
            settings.position[0] = step * .1f;
            settings.rotationDegrees[1] = step * 2.f;
            const auto result = graph::EvaluateRocks(moveGraph, 0, retained);
            Check(result.error.empty(), "連続操作の評価成功");
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 6;
    };
    const auto freshMs = measureDrag(nullptr), cachedMs = measureDrag(&cache);
    std::printf("  Volume drag CPU (48 cells): fresh %.2f ms, cached %.2f ms per update\n", freshMs, cachedMs);
}
