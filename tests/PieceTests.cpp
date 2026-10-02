#include "TestSupport.h"
#include "geometry/Pieces.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"
#include "io/PieceSettings.h"
#include "geometry/UvUnwrap.h"
#include <cmath>
#include <set>
#include <limits>
#include "app/UndoHistory.h"
static void RunLayeredPieceTests();
static void RunPlaneFractureTests();
void RunPieceErosionTests();
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
    RunPlaneFractureTests();
    RunPieceErosionTests();
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
    auto shiftedSettings=rotatedSettings;shiftedSettings.position={1.5f,-2,.25f};
    const auto shiftedLayers=MakeLayeredBoxes(shiftedSettings,10,error);
    bool shifted=error.empty() && shiftedLayers.pieces.size()==rotated.pieces.size() && shiftedLayers.fingerprint!=rotated.fingerprint;
    for (size_t i=0;shifted && i<shiftedLayers.pieces.size();++i)
        for (int row=0;row<3;++row) {
            shifted &= std::abs(shiftedLayers.pieces[i].transform[row*4+3]-rotated.pieces[i].transform[row*4+3]-shiftedSettings.position[row])<1e-5;
            for (int column=0;column<3;++column)
                shifted &= shiftedLayers.pieces[i].transform[row*4+column]==rotated.pieces[i].transform[row*4+column];
        }
    Check(shifted,"位置を変えると向きを保ったまま積み重ね全体を平行移動する");
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
    scatter.planar=true;scatter.count=400;
    ScatterPiecePoints(layers,scatter,error);Check(!error.empty(),"合計1024点を超える設定を拒否する");
    {
        // 1枚あたりは子IDの間隔（512）まで。合計が上限内でも超えれば拒否する。
        auto single=settings;single.count=1;
        const auto plate=MakeLayeredBoxes(single,1,error);
        auto perPiece=scatter;perPiece.count=PieceIdStride+1;
        ScatterPiecePoints(plate,perPiece,error);Check(!error.empty(),"1枚あたり512点を超える設定を拒否する");
        perPiece.count=PieceIdStride;
        const auto most=ScatterPiecePoints(plate,perPiece,error);
        Check(error.empty() && most.positions.size()==size_t(PieceIdStride),"1枚あたり512点までは配置できる");
    }
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

// 構造面（Parallel Planes の系統）でブロックに割る。
static void RunPlaneFractureTests() {
    using namespace rock;
    using namespace rock::geometry;
    using namespace rock::tests;
    Section("構造面で割る（Voronoi Fracture の Planes 入力）");
    std::string error;
    const auto box = MakeBox({2, 2, 2});
    ParallelPlanesSettings horizontal;
    horizontal.spacing = .5f;
    const auto layers = MakeParallelPlanes(horizontal, error);
    auto slabs = FracturePlanes(box, {layers}, 7, error);
    double total = 0;
    bool closed = true, equal = true;
    for (const auto& piece : slabs.pieces) {
        MeshInfo info;
        closed &= InspectMesh(*piece.mesh, info) && info.closed && info.components == 1;
        total += piece.volume;
        equal &= std::abs(piece.volume - 2) < 1e-4;
    }
    // 面は y = -1, -0.5, 0, 0.5, 1。形の上下面と重なる面は厚さ 0 の板になるので捨てる。
    Check(error.empty() && slabs.pieces.size() == 4 && closed && equal && std::abs(total - 8) < 1e-4,
          "水平な面の系統で、厚さ 0.5 m の閉じた板 4 枚に割る");
    {
        // 支えの角度: 45° の板に割った縦長の箱。地面に届かない上の板は 45° の接触面でしか下の板に載らない。
        ParallelPlanesSettings tilted;
        tilted.spacing = .8f;
        tilted.rotationDegrees = {0, 0, 45};
        const auto tall = MakeBox({2, 6, 2});
        const auto tiltedSlabs = FracturePlanes(tall, {MakeParallelPlanes(tilted, error)}, 7, error);
        PieceSelectSettings hold;
        hold.mode = PieceSelectMode::Peel;
        hold.fraction = 0;
        hold.grounded = true;
        hold.supportAngle = 30;
        const auto dropped = SelectPieces(tiltedSlabs, hold, error);
        const bool droppedOk = error.empty() && !dropped.ids.empty();
        hold.supportAngle = 60;
        const auto kept = SelectPieces(tiltedSlabs, hold, error);
        Check(droppedOk && error.empty() && kept.ids.empty() && tiltedSlabs.pieces.size() > 2,
              "支えの角度: 30° では 45° の面で載る板が落ち、60° では残る");
        hold.supportAngle = 91;
        SelectPieces(tiltedSlabs, hold, error);
        Check(!error.empty(), "支えの角度: 範囲外を拒否する");
    }
    ParallelPlanesSettings vertical;
    vertical.spacing = 1;
    vertical.rotationDegrees = {0, 0, 90};
    const auto joints = MakeParallelPlanes(vertical, error);
    const auto blocks = FracturePlanes(box, {layers, joints}, 7, error);
    total = 0;
    for (const auto& piece : blocks.pieces) total += piece.volume;
    Check(error.empty() && blocks.pieces.size() == 8 && std::abs(total - 8) < 1e-4, "2 系統の重なりで 8 個のブロックに割る");
    Check(blocks.adjacencyComplete, "1 回の分割で隣接情報が揃う");
    bool symmetric = true;
    size_t contacts = 0;
    for (const auto& piece : blocks.pieces)
        for (const auto& contact : piece.neighborhood->contacts) {
            ++contacts;
            const auto& other = blocks.pieces[contact.neighbor].neighborhood->contacts;
            symmetric &= std::any_of(other.begin(), other.end(), [&](const auto& back) { return back.neighbor == piece.id; });
        }
    // 4 段 × 2 列: 上下の接触 3 × 2 列 + 左右の接触 4 段 = 10 組（両側で 20）。
    Check(symmetric && contacts == 20, "隣のブロックとの接触面は両側で揃う");
    PieceSelectSettings peel;
    peel.mode = PieceSelectMode::Peel;
    peel.fraction = .5f;
    const auto peeled = SelectPieces(blocks, peel, error);
    Check(error.empty() && !peeled.ids.empty() && peeled.ids.size() < blocks.pieces.size(),
          "外周からの侵食（Peel）でブロックを選べる");
    ParallelPlanesSettings oblique;
    oblique.spacing = .37f;
    oblique.rotationDegrees = {20, 0, 35};
    oblique.variation = .5f;
    const auto tilted = FracturePlanes(box, {MakeParallelPlanes(oblique, error), joints}, 7, error);
    total = 0;
    closed = true;
    for (const auto& piece : tilted.pieces) {
        MeshInfo info;
        closed &= InspectMesh(*piece.mesh, info) && info.closed;
        total += piece.volume;
    }
    Check(error.empty() && tilted.pieces.size() > 8 && closed && std::abs(total - 8) < 1e-3,
          "斜めでばらつきのある系統でも、閉じたブロックに割れて体積が保たれる");
    Check(FracturePlanes(box, {layers, joints}, 7, error).generation == blocks.generation &&
              FracturePlanes(box, {layers}, 7, error).generation != blocks.generation,
          "系統が同じなら同じ分割、変えれば別の分割として扱う");
    FracturePlanes(box, {}, 7, error);
    Check(!error.empty(), "構造面が無ければ診断する");
    ParallelPlanesSettings dense, denseCross;
    dense.spacing = denseCross.spacing = .05f;
    denseCross.rotationDegrees = {0, 0, 90};
    // 40 × 40 = 1600 個で、ピースの上限 1024 を超える。
    FracturePlanes(box, {MakeParallelPlanes(dense, error), MakeParallelPlanes(denseCross, error)}, 7, error);
    Check(!error.empty(), "多すぎるブロックは間隔を広げるよう診断する");

    // 接地: 下向きの外面からは欠かず、支えを失った片は落ちる。4 段 × 2 列のブロックで確かめる。
    {
        PieceSelectSettings grounded = peel;
        grounded.grounded = true;
        grounded.protectCore = false;
        grounded.peelNoise = 0;
        grounded.fraction = .25f;
        const auto picked = SelectPieces(blocks, grounded, error);
        bool bottomKept = true, floating = false;
        std::set<uint32_t> removed(picked.ids.begin(), picked.ids.end());
        for (const auto& piece : blocks.pieces) {
            const bool bottom = piece.centroid.y < -.5f;
            if (bottom) bottomKept &= !removed.contains(piece.id);
        }
        // 残った片は、地面か、残った片の上に載っている（下の段が残っている）。
        for (const auto& piece : blocks.pieces) {
            if (removed.contains(piece.id) || piece.centroid.y < -.5f) continue;
            bool supported = false;
            for (const auto& other : blocks.pieces)
                supported |= !removed.contains(other.id) && std::abs(other.centroid.x - piece.centroid.x) < .1f &&
                             std::abs(other.centroid.y - (piece.centroid.y - .5f)) < .1f;
            floating |= !supported;
        }
        Check(error.empty() && !picked.ids.empty() && bottomKept, "接地: 地面に接した段は欠かない（上と横から欠ける）");
        Check(!floating, "接地: 支えを失った片は落ちて、宙に浮いた片が残らない");
        grounded.fraction = 0;
        Check(SelectPieces(blocks, grounded, error).ids.empty(), "接地: 進行 0 では何も選ばない");
        // 安定: 上の段の片は下の片に 1 m² で載る（体積 2 m³ → 2^(2/3) ≈ 1.59）。安定 1 でも要る面積は 0.79 m² なので載ったまま。
        grounded.fraction = .25f;
        grounded.stability = 1;
        const auto stable = SelectPieces(blocks, grounded, error);
        Check(error.empty() && stable.ids == picked.ids, "安定: 十分な面で載った片は落ちない");
        ParallelPlanesSettings thin;
        thin.spacing = .25f;
        thin.rotationDegrees = {0, 0, 90};
        const auto narrow = FracturePlanes(box, {layers, MakeParallelPlanes(thin, error)}, 7, error);
        PieceSelectSettings topple = grounded;
        topple.fraction = 0;
        topple.stability = 0;
        // 縦の面（90° 回転）の法線の単精度の誤差で、以前は片が閉じずに捨てられ、体積の比較で失敗していた。
        Check(error.empty() && narrow.pieces.size() == 32, "90° 回した細い間隔の系統でも全てのブロックに割れる");
        Check(SelectPieces(narrow, topple, error).ids.empty(), "安定 0 では、載っていれば何も落ちない");
        topple.stability = 1.5f;
        topple.fraction = .25f;
        SelectPieces(narrow, topple, error);
        Check(!error.empty(), "安定: 範囲外を拒否する");
    }

    // Piece Transform のばらつき: 片ごとに自分の重心を中心に乱数で動かして回す。
    {
        PieceTransformSettings shake;
        shake.jitterPosition = {.1f, .05f, .1f};
        shake.jitterRotation = 5;
        const auto moved = TransformPieces(blocks, nullptr, shake, error);
        bool bounded = error.empty() && moved.pieces.size() == blocks.pieces.size(), varied = false;
        for (size_t i = 0; i < moved.pieces.size(); ++i) {
            const auto a = PieceCenter(blocks.pieces[i]), b = PieceCenter(moved.pieces[i]);
            // 重心のまわりに回すので、重心は移動のばらつきの範囲（各軸）しか動かない。
            bounded &= std::abs(b.x - a.x) <= .1f + 1e-4f && std::abs(b.y - a.y) <= .05f + 1e-4f && std::abs(b.z - a.z) <= .1f + 1e-4f;
            varied |= moved.pieces[i].transform != blocks.pieces[i].transform;
        }
        Check(bounded && varied, "ばらつき: 片ごとに重心のまわりで動かして回し、移動は指定の範囲に収まる");
        Check(TransformPieces(blocks, nullptr, shake, error).pieces[3].transform == moved.pieces[3].transform,
              "ばらつき: 同じ Seed なら同じずれ");
        shake.jitterSeed = 2;
        Check(TransformPieces(blocks, nullptr, shake, error).pieces[3].transform != moved.pieces[3].transform,
              "ばらつき: Seed を変えると別のずれ");
        shake.jitterRotation = 200;
        TransformPieces(blocks, nullptr, shake, error);
        Check(!error.empty(), "ばらつき: 範囲外の回転を拒否する");
    }

    // 密度のむらと大きさの効き。
    {
        const auto tall = MakeBox({4, 4, 4});
        ScatterSettings even{200, 3};
        ScatterSettings clustered = even;
        clustered.clustering = 1;
        clustered.clusterScale = 1.5f;
        const auto uniform = ScatterPoints(tall, even, error);
        const auto lumpy = ScatterPoints(tall, clustered, error);
        Check(error.empty() && lumpy.positions.size() == 200 && lumpy.positions != uniform.positions &&
                  lumpy.fingerprint != uniform.fingerprint, "密度のむら: 同じ点数で別の配置になる");
        ScatterSettings none = even;
        none.clusterScale = 5;
        Check(ScatterPoints(tall, none, error).positions == uniform.positions, "密度のむら 0 では従来と同じ配置");
        const auto spread = [&](const PointSet& points) {
            const auto pieces = FractureVoronoi(tall, points, {}, 9, error);
            double small = 1e9, large = 0;
            for (const auto& p : pieces.pieces) { small = std::min(small, p.volume); large = std::max(large, p.volume); }
            return large / small;
        };
        Check(spread(lumpy) > spread(uniform), "密度のむら: 片の大きさの差が広がる");
        ScatterSettings bad = even;
        bad.clustering = 1.5f;
        ScatterPoints(tall, bad, error);
        Check(!error.empty(), "密度のむら: 範囲外を拒否する");
        // 高さの勾配: 正で上半分に点が多く、負で下半分に多い。
        const auto upper = [](const PointSet& points) {
            int n = 0;
            for (auto p : points.positions) n += p.y > 0;
            return n;
        };
        ScatterSettings rising = even;
        rising.heightGradient = 1;
        ScatterSettings sinking = even;
        sinking.heightGradient = -1;
        const auto top = ScatterPoints(tall, rising, error);
        Check(error.empty() && top.positions.size() == 200 && upper(top) > 140 && top.fingerprint != uniform.fingerprint,
              "高さの勾配: 正で上ほど密になる");
        Check(upper(ScatterPoints(tall, sinking, error)) < 60, "高さの勾配: 負で下ほど密になる");
        bad = even;
        bad.heightGradient = -1.5f;
        ScatterPoints(tall, bad, error);
        Check(!error.empty(), "高さの勾配: 範囲外を拒否する");
        // 側面の後退: 横へ削れる深さが高さに比例し、上ほど多く欠けて、芯は残る。
        {
            const auto cells = FractureVoronoi(tall, uniform, {}, 9, error);
            PieceSelectSettings retreat;
            retreat.mode = PieceSelectMode::Peel;
            retreat.fraction = 1;
            retreat.peelNoise = 0;
            retreat.protectCore = false;
            retreat.peelRetreat = 1.2f;
            const auto shaved = SelectPieces(cells, retreat, error);
            int upperRemoved = 0, lowerRemoved = 0;
            for (auto id : shaved.ids)
                for (const auto& p : cells.pieces)
                    if (p.id == id) (p.centroid.y > 0 ? upperRemoved : lowerRemoved) += 1;
            Check(error.empty() && upperRemoved > 2 * lowerRemoved && shaved.ids.size() < cells.pieces.size() / 2,
                  "側面の後退: 上ほど多く欠け、芯は削り切らない");
            // 底の後退: 底でも削れるようになり、下の片の欠ける数が増える。
            retreat.peelRetreatBase = 1;
            const auto evenly = SelectPieces(cells, retreat, error);
            int lowerEven = 0;
            for (auto id : evenly.ids)
                for (const auto& p : cells.pieces)
                    if (p.id == id && p.centroid.y <= 0) ++lowerEven;
            Check(error.empty() && lowerEven > lowerRemoved && evenly.ids.size() < cells.pieces.size(), "底の後退 1 では底の片も欠け、芯は残る");
            retreat.peelRetreatBase = 2;
            SelectPieces(cells, retreat, error);
            Check(!error.empty(), "底の後退: 範囲外を拒否する");
            retreat.peelRetreatBase = 0;
            retreat.peelRetreat = 0;
            Check(SelectPieces(cells, retreat, error).ids.size() == cells.pieces.size(), "側面の後退 0 では上限が無い");
            retreat.peelRetreat = -1;
            SelectPieces(cells, retreat, error);
            Check(!error.empty(), "側面の後退: 範囲外を拒否する");
            // 稜の効き: 元の外面が 2 方向以上を向く片（箱の稜・角）が先に欠け、一つの面しか向かない片（面の中央）は残る。
            retreat.peelRetreat = 0;
            retreat.peelEdge = 1;
            retreat.fraction = .15f;
            const auto edgy = SelectPieces(cells, retreat, error);
            const auto sharpness = [&](const Piece& p) {
                double total = 0; std::array<double, 3> sum{};
                for (const auto& a : p.neighborhood->boundary) {
                    total += std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
                    for (int k = 0; k < 3; ++k) sum[k] += a[k];
                }
                return total > 0 ? 1 - std::sqrt(sum[0]*sum[0]+sum[1]*sum[1]+sum[2]*sum[2]) / total : 0.;
            };
            size_t edged = 0, flatAvailable = 0;
            for (const auto& p : cells.pieces) flatAvailable += sharpness(p) > 1e-6;
            for (auto id : edgy.ids)
                for (const auto& p : cells.pieces)
                    if (p.id == id) edged += sharpness(p) > 1e-6;
            Check(error.empty() && !edgy.ids.empty() && edgy.ids.size() <= flatAvailable && edged == edgy.ids.size(),
                  "稜の効き 1: 欠けるのは稜・角に接する片だけで、面の中央の片は残る");
            retreat.peelEdge = 0;
            const auto plain = SelectPieces(cells, retreat, error);
            size_t plainEdged = 0;
            for (auto id : plain.ids)
                for (const auto& p : cells.pieces)
                    if (p.id == id) plainEdged += sharpness(p) > 1e-6;
            Check(error.empty() && plainEdged < plain.ids.size(), "稜の効き 0: 面の中央の片も欠ける（従来どおり）");
            retreat.peelEdge = 1.5f;
            SelectPieces(cells, retreat, error);
            Check(!error.empty(), "稜の効き: 範囲外を拒否する");
            retreat.peelEdge = 0;
            retreat.fraction = 1;
        }
        const auto pieces = FractureVoronoi(tall, lumpy, {}, 9, error);
        PieceSelectSettings sized;
        sized.mode = PieceSelectMode::Peel;
        sized.fraction = .3f;
        sized.peelNoise = 0;
        sized.protectCore = false;
        const auto mean = [&](const PieceSelection& selection) {
            double total = 0;
            for (auto id : selection.ids) total += pieces.pieces[id].volume;
            return total / std::max<size_t>(1, selection.ids.size());
        };
        const auto plain = SelectPieces(pieces, sized, error);
        sized.peelSize = 1;
        const auto biased = SelectPieces(pieces, sized, error);
        Check(error.empty() && mean(biased) < mean(plain), "大きさの効き: 小さな片から先に欠ける");
    }

    // 束で止まる節理。面積の収支をメッシュから独立に測り、T字で隣接を落とさないことを確かめる。
    for (const bool tiltedCase : {false, true}) for (int span : {1, 2, 3}) {
        std::vector<StructurePlanes> systems{layers, joints};
        if (tiltedCase) {
            ParallelPlanesSettings a, b, c;
            a.rotationDegrees = {11, 17, 76}; a.spacing = .6f; a.variation = .6f;
            b.rotationDegrees = {-15, 23, 0}; b.spacing = .7f; b.variation = .8f;
            c.rotationDegrees = {64, -18, 2}; c.spacing = .8f; c.variation = .5f;
            systems = {MakeParallelPlanes(a,error),MakeParallelPlanes(b,error),MakeParallelPlanes(c,error)};
        }
        const auto grouped = FractureJointGroups(box, systems, span, 7, error);
        Check(error.empty() && grouped.adjacencyComplete && !grouped.pieces.empty(), "節理の束: 分割成功");
        double volumeSum = 0;
        bool groupClosed = true, groupSymmetric = true, areaComplete = true;
        for (const auto& piece : grouped.pieces) {
            MeshInfo info;
            groupClosed &= InspectMesh(*piece.mesh, info) && info.closed && info.components == 1;
            volumeSum += piece.volume;
            double surface = 0, contactArea = 0;
            for (const auto& t : piece.mesh->triangles) {
                const auto a = piece.mesh->positions[t[0]], b = piece.mesh->positions[t[1]], c = piece.mesh->positions[t[2]];
                const double ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
                const double vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
                surface += .5 * std::sqrt(std::pow(uy*vz-uz*vy,2)+std::pow(uz*vx-ux*vz,2)+std::pow(ux*vy-uy*vx,2));
            }
            const auto length = [](const std::array<double,3>& v) { return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); };
            for (const auto& face : piece.neighborhood->boundary) contactArea += length(face);
            for (const auto& contact : piece.neighborhood->contacts) {
                contactArea += length(contact.areaVector);
                const auto& other = grouped.pieces[contact.neighbor];
                const auto back = std::find_if(other.neighborhood->contacts.begin(), other.neighborhood->contacts.end(),
                    [&](const auto& c) { return c.neighbor == piece.id; });
                groupSymmetric &= back != other.neighborhood->contacts.end();
                if (back != other.neighborhood->contacts.end())
                    for (int k=0;k<3;++k) groupSymmetric &= std::abs(back->areaVector[k]+contact.areaVector[k]) < 1e-10;
            }
            areaComplete &= std::abs(surface-contactArea) < surface * 1e-5;
        }
        Check(groupClosed && std::abs(volumeSum-8) < 1e-4, "節理の束: 閉包と体積保存");
        Check(groupSymmetric && areaComplete, "節理の束: T字でも全表面が外面または対称な接触として記録される");
        // 直交する2系統なので、AABB内部の格子点の所属数から空隙と重複を独立に検出できる。
        bool partition = true;
        if (!tiltedCase) for (int x=0;x<13;++x) for (int y=0;y<13;++y) for (int z=0;z<3;++z) {
            const double px = -1 + (x+.371)*2/13, py = -1 + (y+.273)*2/13, pz = -1 + (z+.417)*2/3;
            int owners = 0;
            for (const auto& piece : grouped.pieces) {
                MeshInfo info;
                InspectMesh(*piece.mesh, info);
                owners += px > info.minimum.x && px < info.maximum.x && py > info.minimum.y && py < info.maximum.y &&
                          pz > info.minimum.z && pz < info.maximum.z;
            }
            partition &= owners == 1;
        }
        Check(partition, "節理の束: 内部点に空隙も重複もない");
        Check(FractureJointGroups(box, systems, span, 7, error).fingerprint == grouped.fingerprint,
              "節理の束: 同じ設定で再現");
        PieceSelectSettings groupPeel;
        groupPeel.mode = PieceSelectMode::Peel; groupPeel.fraction = .35f;
        const auto selected = SelectPieces(grouped, groupPeel, error);
        Check(error.empty() && !selected.ids.empty() && selected.ids.size() < grouped.pieces.size(), "節理の束: Peelで欠ける");
    }
    Check(FractureJointGroups(box, {layers,joints}, 0, 7, error).fingerprint == blocks.fingerprint,
          "連続枚数0は従来の構造面分割と同じ");
    FractureJointGroups(box, {layers,joints}, 17, 7, error);
    Check(!error.empty(), "節理の連続枚数の範囲外を診断");
    std::stop_source stopped;
    stopped.request_stop();
    FractureJointGroups(box, {layers,joints}, 1, 7, error, stopped.get_token());
    Check(!error.empty(), "節理の束の取消");

    // グラフ: Parallel Planes を連結して系統を足し、Voronoi Fracture の Planes 入力へ。
    graph::NodeGraph g;
    const auto shape = g.CreateNode(graph::NodeKind::BaseRock), first = g.CreateNode(graph::NodeKind::ParallelPlanes),
               second = g.CreateNode(graph::NodeKind::ParallelPlanes), fracture = g.CreateNode(graph::NodeKind::VoronoiFracture),
               scatter = g.CreateNode(graph::NodeKind::ScatterPoints);
    std::get<ParallelPlanesSettings>(g.FindMutableNode(first)->settings) = horizontal;
    std::get<ParallelPlanesSettings>(g.FindMutableNode(second)->settings) = vertical;
    const auto link = [&](auto a, auto b, int pin) { return g.CreateLink(g.FindNode(a)->outputs[0].id, g.FindNode(b)->inputs[pin].id); };
    Check(link(first, second, 0) && link(shape, fracture, 0) && link(second, fracture, 2), "系統を連結して Planes 入力へつなげる");
    const auto chained = graph::EvaluateRocks(g, second);
    Check(chained.error.empty() && chained.planes && chained.planes->size() == 2, "連結すると上流の系統にこの系統が足される");
    const auto evaluated = graph::EvaluateRocks(g, fracture);
    Check(evaluated.error.empty() && evaluated.pieces && evaluated.pieces->pieces.size() == 8, "グラフで 2 系統のブロックに割る");
    graph::RockEvaluationCache jointCache;
    graph::EvaluateRocks(g, fracture, &jointCache);
    auto& jointSettings = std::get<VoronoiSettings>(g.FindMutableNode(fracture)->settings);
    jointSettings.jointSpan = 2;
    const auto finiteJoints = graph::EvaluateRocks(g, fracture, &jointCache);
    Check(finiteJoints.error.empty() && finiteJoints.pieces && finiteJoints.pieces->generation != evaluated.pieces->generation,
          "節理の連続枚数でキャッシュを更新");
    const auto savedJoints = io::WritePieceSettings(*g.FindNode(fracture));
    jointSettings.jointSpan = 0;
    io::ReadPieceSettings(*g.FindMutableNode(fracture), savedJoints);
    Check(jointSettings.jointSpan == 2, "節理の連続枚数の保存復元");
    jointSettings.jointSpan = 0;
    link(shape, scatter, 0);
    link(scatter, fracture, 1);
    Check(!graph::EvaluateRocks(g, fracture).error.empty(), "Points と Planes の同時接続は、吸着が無効なら診断する");
    std::get<VoronoiSettings>(g.FindMutableNode(fracture)->settings).snap = true;
    const auto snapped = graph::EvaluateRocks(g, fracture);
    Check(snapped.error.empty() && snapped.pieces && !snapped.pieces->pieces.empty(), "吸着が有効なら、Points と Planes の両方で割れる");

    // 節理面への吸着（板 ∩ Voronoi）: 水平な構造面（y = ±0.5, 0）で板に分け、板の中を点の Voronoi で割る。
    {
        const auto points = ScatterPoints(box, {40, 3}, error);
        VoronoiSettings plain;
        const auto loose = FractureVoronoi(box, points, plain, 7, error);
        VoronoiSettings snapSettings;
        snapSettings.snap = true;
        const std::vector<StructurePlanes> sets{layers};
        const auto stuck = FractureVoronoi(box, points, snapSettings, 7, error, {}, &sets);
        const auto onPlanes = [](const PieceCollection& pieces) {
            size_t count = 0, all = 0;
            for (const auto& piece : pieces.pieces)
                for (const auto& p : piece.mesh->positions) {
                    ++all;
                    const double y = piece.transform[4] * p.x + piece.transform[5] * p.y + piece.transform[6] * p.z + piece.transform[7];
                    for (double level : {-.5, 0., .5})
                        if (std::abs(y - level) < 1e-5) { ++count; break; }
                }
            return all > 0 ? double(count) / double(all) : 0.;
        };
        double stuckVolume = 0;
        bool stuckClosed = true, withinSlab = true;
        for (const auto& piece : stuck.pieces) {
            MeshInfo info;
            stuckClosed &= InspectMesh(*piece.mesh, info) && info.closed && info.components == 1;
            stuckVolume += piece.volume;
            // 片は 1 枚の板の中に収まる（上下の面が隣り合う構造面）。
            withinSlab &= info.maximum.y - info.minimum.y <= .5f + 1e-4f;
        }
        Check(error.empty() && stuck.pieces.size() == loose.pieces.size() && stuckClosed, "吸着: 片の数は同じで、どれも閉じている");
        Check(withinSlab, "吸着: 片は 1 枚の板の中に収まる");
        Check(onPlanes(stuck) > onPlanes(loose) + .2 && onPlanes(stuck) > .3, "吸着: 多くの頂点が構造面の上に乗る（複数の片にまたがる一枚の面）");
        Check(std::abs(stuckVolume - 8) < 1e-4, "吸着: 片の体積の合計は元の形と一致する（隙間も重なりも無い）");
        // 板の面を挟んだ隣接: 下の板の片が上の板の片と接触を持ち、面積は両側で対称。
        size_t across = 0;
        bool symmetric = true;
        for (const auto& piece : stuck.pieces)
            for (const auto& contact : piece.neighborhood->contacts) {
                const auto& other = stuck.pieces[contact.neighbor];
                if (std::abs(other.centroid.y - piece.centroid.y) > .25f) {
                    ++across;
                    const auto back = std::find_if(other.neighborhood->contacts.begin(), other.neighborhood->contacts.end(),
                                                   [&](const auto& c) { return c.neighbor == piece.id; });
                    symmetric &= back != other.neighborhood->contacts.end() &&
                                 std::abs(back->areaVector[1] + contact.areaVector[1]) < 1e-9;
                }
            }
        Check(across > 0 && symmetric, "吸着: 板の面を挟んだ片どうしが隣接を持ち、面積は両側で対称");
        PieceSelectSettings peel;
        peel.mode = PieceSelectMode::Peel;
        peel.fraction = .3f;
        const auto peeled = SelectPieces(stuck, peel, error);
        Check(error.empty() && !peeled.ids.empty() && peeled.ids.size() < stuck.pieces.size(), "吸着: Peel で外周から欠ける");
        Check(FractureVoronoi(box, points, snapSettings, 7, error, {}, &sets).generation == stuck.generation &&
                  FractureVoronoi(box, points, plain, 7, error).generation != stuck.generation, "吸着で世代が変わる");
    }

    // Volume Crack: 連結した系統を 1 回で彫る。1 系統だけなら従来の結果と同じ。
    const auto volume = MeshToVolume(box, {48}, error);
    VolumeCrackSettings crack;
    crack.width = .04f;
    crack.depth = .1f;
    const auto single = CrackVolumeWithPlanes(volume, layers, crack, error);
    Check(error.empty() && CrackVolumeWithPlanes(volume, std::vector<StructurePlanes>{layers}, crack, error).values == single.values,
          "1 系統の並びは従来の 1 系統と同じ結果");
    const auto both = CrackVolumeWithPlanes(volume, std::vector<StructurePlanes>{layers, joints}, crack, error);
    bool deeper = error.empty();
    for (size_t i = 0; i < both.values.size(); ++i) deeper &= both.values[i] >= single.values[i];
    Check(deeper && both.values != single.values, "2 系統では両方の系統の割れ目を彫る");
}
