#include "TestSupport.h"
#include "geometry/Pieces.h"
#include "graph/RockEvaluator.h"
#include "io/PieceSettings.h"
#include "app/UndoHistory.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

void RunPieceErosionTests() {
    using namespace rock;
    using namespace geometry;
    using namespace graph;
    using tests::Check;
    tests::Section("Piece adjacency and progressive erosion");
    std::string error;
    LayeredBoxesSettings settings;settings.count=1;settings.size={5,.2f,5};
    settings.offset=settings.sizeVariation=settings.thicknessVariation=0;
    auto layers=MakeLayeredBoxes(settings,1,error);
    auto points=ScatterPiecePoints(layers,{25,4,1,true},error);
    if (!error.empty() || points.groups.empty()) {Check(false,"grid source");return;}
    points.positions.clear();points.groups[0].positions.clear();
    for (int z=-2;z<=2;++z) for (int x=-2;x<=2;++x) points.groups[0].positions.push_back({float(x),0,float(z)});
    points.positions=points.groups[0].positions;
    auto grid=FracturePieces(layers,points,{},20,error);
    Check(error.empty() && grid.pieces.size()==25 && grid.adjacencyComplete,"5x5の薄板分割が完全な隣接グラフを持つ");
    if (!error.empty() || grid.pieces.size()!=25) {std::printf("%s\n",error.c_str());return;}
    size_t edges=0,exterior=0;bool reciprocal=true,areaCorrect=true;
    for (const auto& p:grid.pieces) {
        if (!p.neighborhood->boundary.empty()) ++exterior;
        for (const auto& edge:p.neighborhood->contacts) {
            ++edges;
            const auto other=std::find_if(grid.pieces.begin(),grid.pieces.end(),[&](const auto& q){return q.id==edge.neighbor;});
            if (other==grid.pieces.end()) {reciprocal=false;continue;}
            const auto back=std::find_if(other->neighborhood->contacts.begin(),other->neighborhood->contacts.end(),[&](const auto& e){return e.neighbor==p.id;});
            reciprocal &= back!=other->neighborhood->contacts.end();
            if (back!=other->neighborhood->contacts.end())
                for (int k=0;k<3;++k) reciprocal &= edge.areaVector[k]==-back->areaVector[k];
            areaCorrect &= std::abs(PieceFaceArea(p,edge.areaVector)-.2)<1e-6;
        }
    }
    Check(edges==80 && reciprocal,"共有面は40組で対称、点・辺だけの接触は含まない");
    Check(areaCorrect,"共有面積は各0.2平方メートル");
    Check(exterior==16 && grid.pieces[12].neighborhood->boundary.empty(),"上下面を除き周囲16片だけが初期の露出面を持つ");
    auto transformed=grid.pieces[0];transformed.transform={2,0,0,0,0,3,0,0,0,0,4,0};
    Check(std::abs(PieceFaceArea(transformed,{1,0,0})-12)<1e-9 &&
          std::abs(PieceFaceArea(transformed,{0,1,0})-8)<1e-9,"非均等倍率で共有面積を正しく変換する");

    PieceSelectSettings peel;peel.mode=PieceSelectMode::Peel;peel.peelNoise=0;peel.fraction=0;
    auto selected=SelectPieces(grid,peel,error);
    Check(error.empty() && selected.ids.empty() && selected.frontier.size()==16,"削除量0では最初の外周のみが候補");
    peel.fraction=.4f;auto first=SelectPieces(grid,peel,error);
    peel.fraction=.8f;auto more=SelectPieces(grid,peel,error);
    Check(first.ids.size()==10 && more.ids.size()==20 && std::equal(first.ids.begin(),first.ids.end(),more.ids.begin()),
          "削除量を増やすと同じ順序を延長する");
    std::set<uint32_t> removed;bool exposedOrder=true,reachedInside=false;
    for (auto id:more.ids) {
        const auto it=std::find_if(grid.pieces.begin(),grid.pieces.end(),[&](const auto& p){return p.id==id;});
        const bool original=!it->neighborhood->boundary.empty();
        exposedOrder &= original || std::any_of(it->neighborhood->contacts.begin(),it->neighborhood->contacts.end(),
                                                [&](const auto& edge){return removed.contains(edge.neighbor);});
        reachedInside |= !original;removed.insert(id);
    }
    Check(exposedOrder && reachedInside,"毎回露出した片だけを選び、元の外周より内側へ進む");
    peel.fraction=1;auto all=SelectPieces(grid,peel,error);
    Check(all.ids.size()==24 && std::find(all.ids.begin(),all.ids.end(),grid.pieces[12].id)==all.ids.end(),
          "削除量100%でも最も奥の中心片を保護する");
    auto reverse=grid;std::reverse(reverse.pieces.begin(),reverse.pieces.end());RefreshPieceFingerprint(reverse);
    Check(SelectPieces(reverse,peel,error).ids==all.ids,"入力配列の順序に依存しない侵食");
    peel.protectCore=false;peel.fraction=.2f;
    auto part=SelectPieces(grid,peel,error);auto filtered=FilterPieces(grid,part,false,error);
    peel.fraction=.25f;auto continued=SelectPieces(filtered,peel,error);
    peel.fraction=.4f;auto combined=SelectPieces(grid,peel,error);
    Check(continued.ids.size()==5 && combined.ids.size()==10 &&
          std::equal(continued.ids.begin(),continued.ids.end(),combined.ids.begin()+5),
          "Filterの後も共有面積を保持し、除去した隣から侵食を継続する");
    peel.fraction=1;Check(SelectPieces(grid,peel,error).ids.size()==25,"中心保護をオフにすれば全片を選べる");
    auto moved=grid;moved.pieces[0].transform[3]+=.1;RefreshPieceFingerprint(moved);
    SelectPieces(moved,peel,error);Check(!error.empty(),"個別移動で接触が変わったグラフは診断する");
    auto missing=grid;missing.adjacencyComplete=false;
    SelectPieces(missing,peel,error);Check(!error.empty(),"不完全な隣接グラフを黙って使わない");
    std::stop_source cancel;cancel.request_stop();SelectPieces(grid,peel,error,cancel.get_token());
    Check(!error.empty(),"侵食処理をキャンセルできる");
    peel.peelNoise=std::numeric_limits<float>::quiet_NaN();SelectPieces(grid,peel,error);
    Check(!error.empty(),"非有限のばらつきを拒否する");peel.peelNoise=.15f;

    // 同じ次数の2片でも、支持の面積比が小さい片を先に選ぶ。
    PieceCollection weighted;weighted.producer=9;weighted.adjacencyComplete=true;
    for (uint32_t i=0;i<3;++i) {
        Piece p;p.id=i;p.layer=0;p.volume=1;
        auto neighbors=std::make_shared<PieceNeighborhood>();
        if (i>0) neighbors->contacts.push_back({i-1,{-1,0,0}});
        if (i<2) neighbors->contacts.push_back({i+1,{1,0,0}});
        if (i==0 || i==2) neighbors->boundary.push_back({0,0,i==0?1.:9.});
        p.neighborhood=neighbors;weighted.pieces.push_back(p);
    }
    RefreshPieceFingerprint(weighted);peel.fraction=.34f;peel.peelNoise=0;
    Check(SelectPieces(weighted,peel,error).ids==std::vector<uint32_t>{2},"接続本数でなく共有面積と露出面積の比で順序を決める");

    settings.count=3;auto stack=MakeLayeredBoxes(settings,1,error);
    auto stackPoints=ScatterPiecePoints(stack,{16,3,1,true},error);
    auto pieces=FracturePieces(stack,stackPoints,{},20,error);
    bool separated=true;
    for (const auto& p:pieces.pieces) for (const auto& edge:p.neighborhood->contacts) {
        const auto it=std::find_if(pieces.pieces.begin(),pieces.pieces.end(),[&](const auto& q){return q.id==edge.neighbor;});
        separated &= it!=pieces.pieces.end() && it->layer==p.layer;
    }
    Check(error.empty() && separated,"親IDを付け替えても同じ板の隣接だけを維持する");
    peel.rimLayers=1;peel.rimSide=1;peel.fraction=.5f;
    auto top=SelectPieces(pieces,peel,error);bool topOnly=!top.ids.empty();
    for (auto id:top.ids) topOnly &= std::any_of(pieces.pieces.begin(),pieces.pieces.end(),[&](const auto& p){return p.id==id && p.layer==2;});
    Check(topOnly,"Peelも外側の層範囲を守る");
    auto recutPoints=ScatterPiecePoints(grid,{2,9},error);
    auto recut=FracturePieces(grid,recutPoints,{},30,error);
    Check(error.empty() && !recut.adjacencyComplete,"再分割では親をまたぐ接続の未対応を記録する");
    SelectPieces(recut,peel,error);Check(!error.empty(),"再分割後の不完全な接続で侵食しない");

    NodeGraph graph;
    auto source=graph.CreateNode(NodeKind::LayeredBoxes),scatter=graph.CreateNode(NodeKind::ScatterPoints),
         fracture=graph.CreateNode(NodeKind::VoronoiFracture),select=graph.CreateNode(NodeKind::PieceSelect);
    std::get<LayeredBoxesSettings>(graph.FindMutableNode(source)->settings)=settings;
    std::get<ScatterSettings>(graph.FindMutableNode(scatter)->settings)={16,3,1,true};
    std::get<PieceSelectSettings>(graph.FindMutableNode(select)->settings)=peel;
    const auto link=[&](int a,int b,size_t pin=0){graph.CreateLink(graph.FindNode(a)->outputs[0].id,graph.FindNode(b)->inputs[pin].id);};
    link(source,scatter);link(source,fracture);link(scatter,fracture,1);link(fracture,select);
    RockEvaluationCache cache;const auto initial=EvaluateRocks(graph,select,&cache);
    Check(initial.error.empty() && initial.selection && !initial.selection->ids.empty(),"Peelをグラフから評価する");
    if (!initial.selection) return;
    Node restored;restored.kind=NodeKind::PieceSelect;
    auto json=io::WritePieceSettings(*graph.FindNode(select));io::ReadPieceSettings(restored,json);
    Check(io::WritePieceSettings(restored)==json,"Peelのモードと保護・ばらつき設定を保存復元する");
    DocumentSnapshot before;before.graphNodes=graph.Nodes();before.graphLinks=graph.Links();
    std::get<PieceSelectSettings>(graph.FindMutableNode(select)->settings).fraction=.75f;
    const auto edited=EvaluateRocks(graph,select,&cache);
    Check(edited.pieces==initial.pieces && edited.selection->ids.size()>initial.selection->ids.size(),"削除量変更は分割と隣接のキャッシュを再利用する");
    DocumentSnapshot after;after.graphNodes=graph.Nodes();after.graphLinks=graph.Links();
    UndoHistory history;history.Push(before,0);const auto undo=history.Undo(after);graph.Replace(undo.graphNodes,undo.graphLinks);
    Check(EvaluateRocks(graph,select,&cache).selection->ids==initial.selection->ids,"Undoで侵食の選択を復元する");
    const auto redo=history.Redo(undo);graph.Replace(redo.graphNodes,redo.graphLinks);
    Check(EvaluateRocks(graph,select,&cache).selection->ids==edited.selection->ids,"Redoで侵食の選択を復元する");
}
