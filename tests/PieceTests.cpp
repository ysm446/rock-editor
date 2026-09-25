#include "TestSupport.h"
#include "geometry/Pieces.h"
#include "graph/RockEvaluator.h"
#include "io/PieceSettings.h"
#include "geometry/UvUnwrap.h"
#include <cmath>
#include <set>
#include <limits>
#include "app/UndoHistory.h"
static void RunLayeredPieceTests();
void RunPieceTests() {
    using namespace rock::geometry;
    using rock::tests::Check;
    rock::tests::Section("Voronoi pieces");
    for (uint32_t seed = 1; seed <= 16; ++seed) {
        auto mesh = MakeBox({2, 6, 1});
        std::string error;
        auto points = ScatterPoints(mesh, {24, seed}, error);
        Check(error.empty() && points.positions.size() == 24, "deterministic interior scatter");
        Check(points.positions == ScatterPoints(mesh, {24, seed}, error).positions, "repeat scatter");
        VoronoiSettings settings;
        settings.stretch = {1, 4, 1};
        settings.rotation = {13, 27, 9};
        auto pieces = FractureVoronoi(mesh, points, settings, 42, error);
        if (!error.empty())
            std::printf("%s\n", error.c_str());
        Check(error.empty() && pieces.pieces.size() == 24, "anisotropic convex partition");
        double volume = 0;
        for (const auto &p : pieces.pieces) {
            MeshInfo info;
            Check(InspectMesh(*p.mesh, info) && info.closed && info.components == 1 && info.volume > 0,
                  "piece manifold / positive volume");
            volume += info.volume;
            bool contained = true;
            for (auto v : p.mesh->positions)
                contained &= v.x >= -1.00001 && v.x <= 1.00001 && v.y >= -3.00001 && v.y <= 3.00001 &&
                             v.z >= -.50001 && v.z <= .50001;
            Check(contained, "contained in source");
        }
        Check(std::abs(volume - 12) < .00024, "conserved volume");
        PieceSelectSettings select;
        select.mode = PieceSelectMode::Random;
        auto selected = SelectPieces(pieces, select, error);
        auto keep = FilterPieces(pieces, selected, true, error),
             remove = FilterPieces(pieces, selected, false, error);
        Check(keep.pieces.size() + remove.pieces.size() == pieces.pieces.size(),
              "filter complementary partition");
        PieceTransformSettings move;
        move.pose.position = {1, 2, 3};
        auto moved = TransformPieces(pieces, &selected, move, error);
        Check(error.empty() && moved.generation == pieces.generation, "transform preserves generation");
        if (!pieces.pieces.empty())
            Check(moved.pieces[0].mesh == pieces.pieces[0].mesh, "transform shares immutable mesh");
        FilterPieces(moved, selected, true, error);
        Check(!error.empty() || selected.ids.empty(), "selection rejects different pose");
    }
    auto box = MakeBox({2, 2, 2});
    std::string error;
    auto maximum = ScatterPoints(box, {MaxScatterPoints, 5}, error);
    Check(error.empty() && maximum.positions.size() == MaxScatterPoints, "maximum scatter count");
    auto maximumPieces = FractureVoronoi(box, maximum, {}, 1, error);
    Check(error.empty() && maximumPieces.pieces.size() == MaxScatterPoints, "maximum Voronoi count");
    double maximumVolume = 0;
    for (const auto& piece : maximumPieces.pieces) {
        MeshInfo info;
        Check(InspectMesh(*piece.mesh, info) && info.closed && info.volume > 0,
              "maximum count pieces remain closed");
        maximumVolume += info.volume;
    }
    Check(std::abs(maximumVolume - 8) < .00016, "maximum count conserves volume");
    ScatterPoints(box, {MaxScatterPoints + 1, 5}, error);
    Check(!error.empty(), "scatter rejects excess count");
    maximum.positions.push_back({0, 0, 0});
    FractureVoronoi(box, maximum, {}, 1, error);
    Check(!error.empty(), "Voronoi rejects excess count");
    PointSet points;
    points.source = MeshFingerprint(box);
    points.positions = {{-.5f, 0, 0}, {.5f, 0, 0}};
    auto halves = FractureVoronoi(box, points, {}, 1, error);
    Check(error.empty() && halves.pieces.size() == 2, "plane through triangulation edges");
    if (halves.pieces.size() == 2)
        for (const auto &piece : halves.pieces) {
            double area = 0;
            bool sharedPlane = true;
            for (size_t t = 0; t < piece.mesh->triangles.size(); ++t)
                if ((*piece.faceOrigins)[t] == 0) {
                    auto face = piece.mesh->triangles[t];
                    auto a = piece.mesh->positions[face[0]], b = piece.mesh->positions[face[1]],
                         c = piece.mesh->positions[face[2]];
                    area += std::abs(double(b.y - a.y) * (c.z - a.z) - double(b.z - a.z) * (c.y - a.y)) * .5;
                    auto normal = FaceNormal(*piece.mesh, face);
                    sharedPlane &= std::abs(a.x) < 1e-6 && std::abs(b.x) < 1e-6 && std::abs(c.x) < 1e-6 &&
                                   (piece.id == 0 ? normal.x > .999f : normal.x < -.999f);
                }
            Check(sharedPlane && std::abs(area - 4) < 1e-6, "opposite caps coincide / area and orientation");
            // 半分の箱（1 x 2 x 2）の稜線は12本で長さの合計 4 x (1 + 2 + 2)。切断面の対角線を含まない。
            double length = 0;
            bool axisAligned = true;
            for (const auto &edge : PieceEdges(piece)) {
                const double dx = edge[1].x - edge[0].x, dy = edge[1].y - edge[0].y, dz = edge[1].z - edge[0].z;
                length += std::sqrt(dx * dx + dy * dy + dz * dz);
                axisAligned &= int(std::abs(dx) > 1e-6) + int(std::abs(dy) > 1e-6) + int(std::abs(dz) > 1e-6) == 1;
            }
            Check(axisAligned && std::abs(length - 20) < 1e-5, "wireframe edges outline the half box without diagonals");
        }
    points.positions = {{-.5f, -.5f, 0}, {.5f, -.5f, 0}, {-.5f, .5f, 0}, {.5f, .5f, 0}};
    auto quadrants = FractureVoronoi(box, points, {}, 1, error);
    Check(error.empty() && quadrants.pieces.size() == 4, "bisectors meet at existing edges and vertices");
    auto convexFailure = box;
    convexFailure.positions[6] = {0, 0, 0};
    ScatterPoints(convexFailure, {}, error);
    Check(!error.empty(), "concave input rejected instead of convex hull replacement");
    PieceSelectSettings originalSelection;
    originalSelection.mode = PieceSelectMode::Manual;
    originalSelection.ids = {0};
    originalSelection.producer = halves.producer;
    originalSelection.generation = halves.generation;
    auto pick = SelectPieces(halves, originalSelection, error);
    PieceTransformSettings perPiece;
    perPiece.producer = halves.producer;
    perPiece.generation = halves.generation;
    PieceOverride individual;
    individual.id = 0;
    individual.pose.position = {1, 2, 3};
    individual.pose.rotation = {17, 30, 8};
    individual.pose.scale = {2, 3, 4};
    perPiece.overrides.push_back(individual);
    auto movedHalves = TransformPieces(halves, &pick, perPiece, error);
    if (movedHalves.pieces.size() == 2) {
        MeshInfo info;
        auto movedMesh = PieceMesh(movedHalves.pieces[0]);
        auto center = PieceCenter(movedHalves.pieces[0]);
        Check(InspectMesh(movedMesh, info) && info.closed && std::abs(info.volume - 96) < .001,
              "individual rotation / XYZ scale preserve closed solid");
        Check(std::abs(center.x - .5f) < 1e-5 && std::abs(center.y - 2) < 1e-5 &&
                  std::abs(center.z - 3) < 1e-5,
              "individual pivot is volume centroid");
        Check(movedHalves.pieces[1].transform == halves.pieces[1].transform, "unselected piece untouched");
    } else
        Check(false, "individual transform produces pieces");
    auto otherNamespace = halves;
    otherNamespace.producer = 99;
    RefreshPieceFingerprint(otherNamespace);
    FilterPieces(otherNamespace, pick, true, error);
    Check(!error.empty(), "selection cannot cross fracture namespace");
    points.positions[1] = points.positions[0];
    FractureVoronoi(box, points, {}, 1, error);
    Check(!error.empty(), "duplicate sites rejected");
    auto open = box;
    open.triangles.pop_back();
    ScatterPoints(open, {}, error);
    Check(!error.empty(), "open mesh rejected");

    for (auto size : {std::array<float, 3>{.001f, .001f, .001f}, {2, 6, .01f}, {1000, 1000, 1000}}) {
        auto source = MakeBox(size);
        auto sites = ScatterPoints(source, {128, 5}, error);
        auto result = FractureVoronoi(source, sites, {}, 7, error);
        if (!error.empty())
            std::printf("%s\n", error.c_str());
        Check(error.empty() && result.pieces.size() == 128, "128 cells / thin and extreme boxes");
        // 独立した最近点条件で全頂点のセル所属を確認する。凸セル同士の内部重複も排除する。
        bool owns = true;
        const auto distance = [](Vec3 a, Vec3 b) {
            return double(a.x - b.x) * (a.x - b.x) + double(a.y - b.y) * (a.y - b.y) +
                   double(a.z - b.z) * (a.z - b.z);
        };
        for (const auto &p : result.pieces)
            for (auto v : p.mesh->positions)
                for (auto site : sites.positions)
                    owns &= distance(v, sites.positions[p.id]) <=
                            distance(v, site) + double(size[0]) * size[0] * 2e-6;
        Check(owns, "independent nearest site / no overlapping interiors");
    }
    using namespace rock::graph;
    NodeGraph graph;
    auto base = graph.CreateNode(NodeKind::BaseRock), scatter = graph.CreateNode(NodeKind::ScatterPoints),
         fracture = graph.CreateNode(NodeKind::VoronoiFracture),
         select = graph.CreateNode(NodeKind::PieceSelect), filter = graph.CreateNode(NodeKind::PieceFilter),
         move = graph.CreateNode(NodeKind::PieceTransform),
         convert = graph.CreateNode(NodeKind::PiecesToMesh), uv = graph.CreateNode(NodeKind::UvUnwrap),
         output = graph.CreateNode(NodeKind::MeshOutput);
    const auto link = [&](int from, int to, size_t pin = 0) {
        return graph.CreateLink(graph.FindNode(from)->outputs[0].id, graph.FindNode(to)->inputs[pin].id);
    };
    Check(link(base, scatter) && link(base, fracture) && link(scatter, fracture, 1) &&
              link(fracture, select) && link(fracture, filter) && link(select, filter, 1) &&
              link(filter, move) && link(move, convert) && link(convert, uv) && link(uv, output),
          "typed piece workflow connects");
    Check(!link(fracture, output) && !link(scatter, uv), "no implicit Pieces / Points to Mesh conversion");
    auto &selection = std::get<PieceSelectSettings>(graph.FindMutableNode(select)->settings);
    selection.mode = PieceSelectMode::Random;
    RockEvaluationCache cache;
    auto initial = EvaluateRocks(graph, fracture, &cache);
    Check(initial.error.empty() && initial.pieces && initial.rocks.size() == 24,
          "colored preview keeps individual pieces");
    auto firstMesh = initial.pieces->pieces[0].mesh;
    auto final = EvaluateRocks(graph, 0, &cache);
    Check(final.error.empty() && final.rocks.size() == 1 && HasValidUvs(final.rocks[0].mesh),
          "pieces to UV workflow");
    auto &movement = std::get<PieceTransformSettings>(graph.FindMutableNode(move)->settings);
    movement.pose.position[0] = 1;
    final = EvaluateRocks(graph, 0, &cache);
    initial = EvaluateRocks(graph, fracture, &cache);
    Check(firstMesh == initial.pieces->pieces[0].mesh, "downstream editing reuses fracture geometry");
    selection.mode = PieceSelectMode::Manual;
    selection.ids = {0, 2};
    selection.producer = fracture;
    selection.generation = initial.pieces->generation;
    auto json = rock::io::WritePieceSettings(*graph.FindNode(select));
    Node restored;
    restored.kind = NodeKind::PieceSelect;
    rock::io::ReadPieceSettings(restored, json);
    Check(json == rock::io::WritePieceSettings(restored) && json["generation"].is_string(),
          "selection JSON round trip / exact 64 bit key");
    ++std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).seed;
    auto stale = EvaluateRocks(graph, filter, &cache);
    Check(!stale.error.empty(), "upstream seed invalidates manual selection");
    --std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).seed;
    auto undone = EvaluateRocks(graph, filter, &cache);
    Check(undone.error.empty(), "restored settings recover selection");
    // 表示用に、ピース系ノードの直近の出力が残る（選択中の Piece Filter の稜線を描くのに使う）。
    Check(cache.pieceOutputs.contains(filter) && cache.pieceOutputs.contains(fracture) &&
              cache.pieceOutputs[filter]->pieces.size() == 22 && cache.pieceOutputs[fracture]->pieces.size() == 24,
          "evaluation keeps the latest piece outputs for display");
    ++std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).seed;
    Check(!EvaluateRocks(graph, filter, &cache).error.empty() && !cache.pieceOutputs.contains(filter),
          "a failed piece node drops its displayed output");
    --std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings).seed;
    EvaluateRocks(graph, filter, &cache);
    selection.ids.clear();
    std::get<PieceFilterSettings>(graph.FindMutableNode(filter)->settings).keep = true;
    auto empty = EvaluateRocks(graph, filter, &cache);
    Check(empty.error.empty() && empty.pieces && empty.pieces->pieces.empty(),
          "empty Keep is a valid collection");
    std::stop_source cancellation;
    cancellation.request_stop();
    auto canceled = EvaluateRocks(graph, fracture, &cache, VolumeMeshingMethod::MarchingTetrahedra,
                                  cancellation.get_token());
    Check(!canceled.error.empty(), "canceled evaluation cannot publish a result");
    // 原点から遠い入力では、floatへ丸めた点が面の外へ出ることがある。Scatterが出した点は
    // 必ずVoronoi Fractureの内外判定を通る。
    bool farAccepted = true;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        Mesh farBox = MakeBox({.02f, .03f, .02f});
        for (auto &p : farBox.positions) {
            p.x += 900;
            p.y -= 700;
            p.z += 500;
        }
        ScatterSettings farScatter;
        farScatter.count = MaxScatterPoints;
        farScatter.seed = seed;
        std::string farError;
        const auto farPoints = ScatterPoints(farBox, farScatter, farError);
        if (!farError.empty())
            continue;
        FractureVoronoi(farBox, farPoints, {}, 1, farError);
        farAccepted &= farError.find("外") == std::string::npos;
    }
    Check(farAccepted, "scattered points stay inside for Voronoi far from the origin");
    RunLayeredPieceTests();
}

static void RunLayeredPieceTests() {
    using namespace rock;
    using namespace rock::geometry;
    using namespace rock::graph;
    using namespace rock::tests;
    Section("Layered Boxes / 板ごとの分割と側縁の選別");
    std::string error;
    LayeredBoxesSettings settings; settings.count=3;
    auto layers=MakeLayeredBoxes(settings,10,error);
    Check(error.empty() && layers.pieces.size()==3,"3枚の板をPiecesとして作る");
    if (layers.pieces.size()!=3) return;
    bool closed=true, spaced=true;
    double total=0;
    for (size_t i=0;i<layers.pieces.size();++i) {
        const auto& p=layers.pieces[i]; MeshInfo info;
        closed &= InspectMesh(PieceMesh(p),info) && info.closed && info.components==1 && p.layer==int(i);
        if (i>0) {
            const auto& previous=layers.pieces[i-1];
            const double gap=p.transform[7]-p.layerSize[1]*.5-previous.transform[7]-previous.layerSize[1]*.5;
            spaced &= std::abs(gap-settings.gap)<1e-6;
        }
        total+=p.volume;
    }
    Check(closed && spaced,"板は閉じ、厚さが変わっても指定の隙間で積まれる");
    Check(MakeLayeredBoxes(settings,10,error).fingerprint==layers.fingerprint,"同じ設定は板の配置を再現する");
    auto rotatedSettings=settings;rotatedSettings.rotation={27,13,19};
    const auto rotated=MakeLayeredBoxes(rotatedSettings,10,error);
    Check(error.empty() && rotated.pieces[0].transform!=layers.pieces[0].transform,"向きを変えると平行を保って回転する");
    ScatterSettings scatter; scatter.count=32;scatter.planar=true;
    auto points=ScatterPiecePoints(layers,scatter,error);
    Check(error.empty() && points.grouped && points.groups.size()==3 && points.positions.size()==96,
          "各板に32点ずつ配置し、表示用の全点を返す");
    auto pieces=FracturePieces(layers,points,{},20,error);
    Check(error.empty() && pieces.pieces.size()==96,"板をそれぞれ32個に分割する");
    if (!error.empty() || pieces.pieces.size()!=96) {std::printf("%s\n",error.c_str());return;}
    std::set<uint32_t> ids;
    double sum=0; bool flat=true, provenance=true;
    for (const auto& p:pieces.pieces) {
        MeshInfo info;
        flat &= InspectMesh(*p.mesh,info) && info.closed && info.components==1 &&
                std::abs(info.minimum.y+p.layerSize[1]*.5f)<1e-5 && std::abs(info.maximum.y-p.layerSize[1]*.5f)<1e-5;
        sum+=p.volume;ids.insert(p.id);
        provenance &= p.layer==int(p.parentId) && p.parentProducer==10 && p.transform==layers.pieces[p.parentId].transform;
    }
    Check(flat && provenance && ids.size()==96 && std::abs(sum-total)<total*2e-5,
          "面内分割は厚み・閉包・体積・親と層の情報を保ち、IDが重複しない");
    const auto rotatedPoints=ScatterPiecePoints(rotated,scatter,error);
    Check(error.empty() && rotatedPoints.groups[0].positions==points.groups[0].positions &&
          rotatedPoints.positions!=points.positions,"配置を変えてもローカルの点は不変、表示用の点は移動する");
    auto movedPieces=FracturePieces(rotated,rotatedPoints,{},20,error);
    Check(error.empty() && movedPieces.pieces.size()==96 && movedPieces.pieces[0].transform==rotated.pieces[0].transform,
          "回転した板の分割も元の配置を引き継ぐ");
    PieceSelectSettings rim; rim.mode=PieceSelectMode::Rim;rim.fraction=1;
    const auto edge=SelectPieces(pieces,rim,error);
    auto kept=FilterPieces(pieces,edge,false,error);
    Check(error.empty() && !edge.ids.empty() && !kept.pieces.empty() && kept.pieces.size()<pieces.pieces.size(),
          "側縁だけを除き、上下面に触れる内部の片は残る");
    Check(std::all_of(kept.pieces.begin(),kept.pieces.end(),[](const auto& p){return !p.layerRim;}),
          "残った片は元の板の側縁に触れない");
    rim.layer=1;rim.invert=true;
    const auto middle=SelectPieces(pieces,rim,error);
    bool middleOnly=!middle.ids.empty();
    for (const auto& p:pieces.pieces)
        if (std::find(middle.ids.begin(),middle.ids.end(),p.id)!=middle.ids.end()) middleOnly &= p.layer==1 && !p.layerRim;
    Check(middleOnly,"層指定と反転は指定した層の中だけに効く");
    rim.layer=-1;rim.invert=false;rim.fraction=0;
    Check(SelectPieces(pieces,rim,error).ids.empty(),"側縁の選択率0なら削らない");
    rim.fraction=.5f;
    const auto randomEdges=SelectPieces(pieces,rim,error);
    Check(randomEdges.ids==SelectPieces(pieces,rim,error).ids && randomEdges.ids.size()<edge.ids.size(),
          "側縁の確率選択は再現可能");
    auto outside = rim;
    outside.fraction = 1; outside.rimLayers = 1;
    const auto bothEdges = SelectPieces(pieces,outside,error);
    bool onlyOutside = !bothEdges.ids.empty();
    for (const auto& p : pieces.pieces) {
        const bool selected = std::find(bothEdges.ids.begin(),bothEdges.ids.end(),p.id)!=bothEdges.ids.end();
        onlyOutside &= selected == (p.layerRim && (p.layer==0 || p.layer==2));
    }
    Check(error.empty() && onlyOutside,"両側1層は最上層と最下層の側縁だけを選ぶ");
    outside.rimSide = 1;
    const auto topEdges = SelectPieces(pieces,outside,error);
    bool topOnly = !topEdges.ids.empty();
    for (const auto& p : pieces.pieces)
        topOnly &= (std::find(topEdges.ids.begin(),topEdges.ids.end(),p.id)!=topEdges.ids.end()) == (p.layerRim && p.layer==2);
    Check(topOnly,"上側指定では下側と中央を保護する");
    outside.invert = true;
    const auto topInside = SelectPieces(pieces,outside,error);
    for (const auto& p : pieces.pieces)
        Check((std::find(topInside.ids.begin(),topInside.ids.end(),p.id)!=topInside.ids.end()) == (!p.layerRim && p.layer==2),
              "反転でも外側の対象層を越えない");
    outside.invert=false; outside.layer=1;
    Check(SelectPieces(pieces,outside,error).ids.empty(),"層指定と外側範囲は共通部分を選ぶ");
    outside.layer=-1;outside.rimSide=2;
    const auto bottomEdges=SelectPieces(pieces,outside,error);
    bool bottomOnly=!bottomEdges.ids.empty();
    for (const auto& p:pieces.pieces)
        bottomOnly &= (std::find(bottomEdges.ids.begin(),bottomEdges.ids.end(),p.id)!=bottomEdges.ids.end()) == (p.layerRim && p.layer==0);
    Check(bottomOnly,"下側指定では最下層の側縁を選ぶ");
    outside.rimLayers=0;outside.rimSide=0;outside.rimFalloff=1;
    Check(SelectPieces(pieces,outside,error).ids==bothEdges.ids,"内側への減衰1で中央を残し外側を維持する");
    outside.rimLayers=32;
    Check(SelectPieces(pieces,outside,error).ids==bothEdges.ids,"対象層数が実際より多くても減衰の内端を維持する");
    outside.rimLayers=0;
    outside.rimFalloff=0;outside.fraction=.25f;
    const auto fewer=SelectPieces(pieces,outside,error);
    outside.fraction=.75f;
    const auto more=SelectPieces(pieces,outside,error);
    Check(std::all_of(fewer.ids.begin(),fewer.ids.end(),[&](auto id){return std::find(more.ids.begin(),more.ids.end(),id)!=more.ids.end();}),
          "選択率を増やすと既存の欠けを保って追加する");
    outside.rimLayers=-1;SelectPieces(pieces,outside,error);Check(!error.empty(),"負の対象層数を拒否する");
    outside.rimLayers=1;outside.rimFalloff=std::numeric_limits<float>::quiet_NaN();
    SelectPieces(pieces,outside,error);Check(!error.empty(),"非有限の内側減衰を拒否する");
    Node persisted;persisted.kind=NodeKind::PieceSelect;
    outside.rimFalloff=.65f;outside.rimSide=2;outside.rimLayers=2;persisted.settings=outside;
    Node restoredSelection;restoredSelection.kind=NodeKind::PieceSelect;
    io::ReadPieceSettings(restoredSelection,io::WritePieceSettings(persisted));
    Check(io::WritePieceSettings(restoredSelection)==io::WritePieceSettings(persisted),"外側選択の設定をJSONで保持する");
    io::ReadPieceSettings(restoredSelection,nlohmann::json{{"mode",5}});
    const auto& legacy=std::get<PieceSelectSettings>(restoredSelection.settings);
    Check(legacy.rimLayers==0 && legacy.rimFalloff==0,"旧Rimは全層の選択を維持する");
    // 先頭の板を除いても、残った親のローカル点と子IDは変わらない。
    auto filteredLayers=layers;filteredLayers.pieces.erase(filteredLayers.pieces.begin());RefreshPieceFingerprint(filteredLayers);
    const auto filteredPoints=ScatterPiecePoints(filteredLayers,scatter,error);
    const auto refractured=FracturePieces(filteredLayers,filteredPoints,{},20,error);
    Check(error.empty() && filteredPoints.groups[0].positions==points.groups[1].positions &&
          refractured.pieces[0].id==pieces.pieces[32].id,"他の板を除いても親ごとの乱数とIDを保つ");
    FracturePieces(filteredLayers,points,{},20,error);
    Check(!error.empty(),"違うPiecesの点群を拒否する");
    scatter.planar=false;
    const auto volumetric=FracturePieces(layers,ScatterPiecePoints(layers,scatter,error),{},20,error);
    Check(error.empty() && volumetric.pieces.size()==96,"3Dの点配置でも板ごとに分割できる");
    scatter.planar=true;scatter.count=200;
    ScatterPiecePoints(layers,scatter,error);Check(!error.empty(),"合計512点を超える設定を拒否する");
    auto invalid=settings;invalid.count=0;MakeLayeredBoxes(invalid,1,error);Check(!error.empty(),"板0枚を拒否する");
    invalid=settings;invalid.size[1]=-1;MakeLayeredBoxes(invalid,1,error);Check(!error.empty(),"負の厚さを拒否する");
    invalid=settings;invalid.rotation[0]=std::numeric_limits<float>::infinity();MakeLayeredBoxes(invalid,1,error);
    Check(!error.empty(),"非有限の向きを拒否する");
    std::stop_source cancel;cancel.request_stop();MakeLayeredBoxes(settings,1,error,cancel.get_token());
    Check(!error.empty(),"板の生成をキャンセルできる");
    FracturePieces(layers,points,{},20,error,cancel.get_token());Check(!error.empty(),"複数板の分割をキャンセルできる");

    NodeGraph graph;
    auto source=graph.CreateNode(NodeKind::LayeredBoxes), sites=graph.CreateNode(NodeKind::ScatterPoints),
         fracture=graph.CreateNode(NodeKind::VoronoiFracture), select=graph.CreateNode(NodeKind::PieceSelect),
         filter=graph.CreateNode(NodeKind::PieceFilter), mesh=graph.CreateNode(NodeKind::PiecesToMesh),
         output=graph.CreateNode(NodeKind::MeshOutput), volume=graph.CreateNode(NodeKind::ToVolume);
    std::get<LayeredBoxesSettings>(graph.FindMutableNode(source)->settings)=settings;
    scatter.count=24;std::get<ScatterSettings>(graph.FindMutableNode(sites)->settings)=scatter;
    rim.fraction=1;std::get<PieceSelectSettings>(graph.FindMutableNode(select)->settings)=rim;
    const auto link=[&](int a,int b,size_t pin=0){return graph.CreateLink(graph.FindNode(a)->outputs[0].id,graph.FindNode(b)->inputs[pin].id);};
    Check(link(source,sites) && link(source,fracture) && link(sites,fracture,1) && link(fracture,select) &&
          link(fracture,filter) && link(select,filter,1) && link(filter,mesh) && link(mesh,output) && link(mesh,volume),
          "Piecesの点配置・分割・選別・メッシュ化・任意のボリューム化を接続できる");
    Check(!graph.CanCreateLink(graph.FindNode(volume)->outputs[0].id,graph.FindNode(sites)->inputs[0].id),
          "Geometry入力はVolumeを暗黙変換しない");
    RockEvaluationCache cache;
    const auto result=EvaluateRocks(graph,0,&cache);
    Check(result.error.empty() && result.rocks.size()==1 && !result.rocks[0].mesh.positions.empty(),
          "板の側縁を欠いた形をボリューム化せず表示できる");
    if (!result.error.empty()) {std::printf("%s\n",result.error.c_str());return;}
    const auto first=EvaluateRocks(graph,fracture,&cache).pieces;
    std::get<PieceSelectSettings>(graph.FindMutableNode(select)->settings).fraction=.5f;
    EvaluateRocks(graph,0,&cache);
    Check(EvaluateRocks(graph,fracture,&cache).pieces==first,"選別の変更で重い分割を作り直さない");
    DocumentSnapshot before;before.graphNodes=graph.Nodes();before.graphLinks=graph.Links();
    auto& edit=std::get<LayeredBoxesSettings>(graph.FindMutableNode(source)->settings);edit.offset=.22f;
    const auto moved=EvaluateRocks(graph,fracture,&cache).pieces;
    Check(moved && moved!=first,"板の変更で点配置と分割を更新する");
    DocumentSnapshot after;after.graphNodes=graph.Nodes();after.graphLinks=graph.Links();
    UndoHistory history;history.Push(before,0);const auto undo=history.Undo(after);graph.Replace(undo.graphNodes,undo.graphLinks);
    Check(EvaluateRocks(graph,fracture,&cache).pieces->fingerprint==first->fingerprint,"Undoで板と分割が再現する");
    const auto redo=history.Redo(undo);graph.Replace(redo.graphNodes,redo.graphLinks);
    Check(EvaluateRocks(graph,fracture,&cache).pieces->fingerprint==moved->fingerprint,"Redoで変更後の板と分割が再現する");
    for (auto id:{source,sites,select}) {
        auto original=io::WritePieceSettings(*graph.FindNode(id));Node restored;restored.kind=graph.FindNode(id)->kind;
        io::ReadPieceSettings(restored,original);
        Check(io::WritePieceSettings(restored)==original,"板・面内配置・側縁と層指定を設定JSONで往復する");
    }
    std::get<VolumeSettings>(graph.FindMutableNode(volume)->settings).resolution=32;
    const auto voxel=EvaluateRocks(graph,volume,&cache);
    Check(voxel.error.empty() && voxel.rocks.size()==1 && voxel.rocks[0].volume,"欠けた板の集合を後段でボリューム化できる");
}
