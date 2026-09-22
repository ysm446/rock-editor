#include "TestSupport.h"
#include "geometry/Displace.h"
#include "geometry/UvUnwrap.h"
#include "graph/RockEvaluator.h"
#include "app/UndoHistory.h"
#include <cmath>
#include <limits>

void RunDisplaceTests() {
    using namespace rock;
    using tests::Check;
    tests::Section("Subdivide / Displace");
    const auto box=geometry::MakeBox({2,2,2});
    std::string error;
    auto split=geometry::SubdivideMesh(box,{2},error);
    geometry::MeshInfo info;
    Check(error.empty() && split.triangles.size()==192 && geometry::InspectMesh(split,info) && info.closed && std::abs(info.volume-8)<1e-5,
          "subdivision preserves shape, volume and closed shared topology");
    // マスク付きの細分化。半分の面だけ割っても閉じたまま（隣の面は共有辺に合わせて割る）。
    {
        std::vector<float> half(box.triangles.size(),0.f);
        for(size_t f=0;f<half.size();++f) if(geometry::FaceNormal(box,box.triangles[f]).y>.5f) half[f]=1;  // 上面だけ
        geometry::SubdivideSettings one; one.levels=1;
        auto partial=geometry::SubdivideMesh(box,one,error,{},{},half);
        geometry::MeshInfo partialInfo;
        // 上面2枚 → 8枚。側面8枚のうち上面と辺を共有する4枚は1辺が割れて2枚ずつ → 8枚、残り4枚と下面2枚はそのまま。
        Check(error.empty() && partial.triangles.size()==8+8+4+2 && geometry::InspectMesh(partial,partialInfo) && partialInfo.closed &&
                  std::abs(partialInfo.volume-8)<1e-5,"masked subdivision splits selected faces and keeps the mesh closed");
        geometry::SubdivideSettings two; two.levels=2;
        auto deeper=geometry::SubdivideMesh(box,two,error,{},{},half);
        // 2段目は1段目の上面の子8枚だけを割る（32枚）。隣の面の子は割らない。
        Check(error.empty() && geometry::InspectMesh(deeper,partialInfo) && partialInfo.closed && deeper.triangles.size()<box.triangles.size()*16 &&
                  deeper.triangles.size()>=32+8+2,"only the descendants of selected faces keep splitting");
        geometry::UvUnwrapSettings uvSettings; uvSettings.resolution=128;
        auto unwrappedHalf=geometry::UnwrapMesh(box,uvSettings,error);
        std::vector<float> uvHalf(unwrappedHalf.triangles.size(),0.f);
        for(size_t f=0;f<uvHalf.size();++f) if(geometry::FaceNormal(unwrappedHalf,unwrappedHalf.triangles[f]).y>.5f) uvHalf[f]=1;
        auto partialUv=geometry::SubdivideMesh(unwrappedHalf,one,error,{},{},uvHalf);
        Check(error.empty() && geometry::HasValidUvs(partialUv) && partialUv.uvCharts.size()==partialUv.triangles.size(),
              "masked subdivision keeps corner UVs and charts");
        std::vector<float> none(box.triangles.size(),0.f);
        auto untouched=geometry::SubdivideMesh(box,one,error,{},{},none);
        Check(error.empty() && untouched.triangles==box.triangles,"all-black mask leaves the mesh unchanged");
        geometry::SubdivideSettings strict=one; strict.threshold=1.1f;
        Check(geometry::SubdivideMesh(box,strict,error,{},{},half).triangles==box.triangles,"threshold above every value selects nothing");
        Check(geometry::SubdivideMesh(box,one,error,{},{},std::vector<float>(3,1.f)).positions.empty() && !error.empty(),"mask size mismatch is rejected");
    }
    const auto same=geometry::SubdivideMesh(box,{0},error);
    Check(same.positions==box.positions && same.triangles==box.triangles,"zero subdivision is identity");
    auto oversized=split; oversized.triangles.resize(1000001,split.triangles.front());  // 4倍で400万面を超える
    Check(geometry::SubdivideMesh(oversized,{1},error).positions.empty() && !error.empty(),"subdivision rejects predicted count before allocation");
    geometry::UvUnwrapSettings uv; uv.resolution=128;
    auto unwrapped=geometry::UnwrapMesh(box,uv,error);
    auto detail=geometry::SubdivideMesh(unwrapped,{2},error);
    Check(error.empty() && geometry::HasValidUvs(detail) && detail.uvWidth==unwrapped.uvWidth && detail.uvCharts.size()==detail.triangles.size(),
          "subdivision retains atlas, corner UVs and charts");
    const auto neutral=[](auto,auto,auto){return .5f;};
    auto displaced=geometry::DisplaceMesh(detail,{.2f,.5f},neutral,error);
    Check(error.empty() && displaced.positions==detail.positions && displaced.cornerUvs==detail.cornerUvs,"midpoint preserves exact mesh and UVs");
    const auto raised=[](auto,auto,auto){return 1.f;};
    displaced=geometry::DisplaceMesh(detail,{.2f,.5f},raised,error);
    bool distance=true;
    for(size_t i=0;i<detail.positions.size();++i) {
        const auto a=detail.positions[i],b=displaced.positions[i];
        const float d=std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));
        distance &= std::abs(d-.1f)<1e-5f;
    }
    Check(error.empty() && distance && geometry::InspectMesh(displaced,info) && info.closed,"height displaces shared vertices by physical distance without cracks");
    displaced=geometry::DisplaceMesh(detail,{.05f,.5f},[](auto,auto,auto u){return u.u;},error);
    Check(error.empty() && displaced.positions.size()==detail.positions.size() && geometry::InspectMesh(displaced,info) && info.closed,
          "different chart heights are reconciled at shared seam vertices");
    Check(geometry::DisplaceMesh(detail,{.1f,.5f},[](auto,auto,auto){return std::numeric_limits<float>::quiet_NaN();},error).positions.empty(),"nonfinite height is rejected");
    std::stop_source stop; stop.request_stop();
    Check(geometry::SubdivideMesh(box,{2},error,stop.get_token()).positions.empty() && !error.empty(),"subdivide cancellation");
    Check(geometry::DisplaceMesh(box,{.1f,.5f},raised,error,stop.get_token()).positions.empty() && !error.empty(),"displace cancellation");

    graph::ScalarField field{2,1,{0,1}};
    Check(std::abs(field.Sample(0,0,true)-.5f)<1e-6 && field.Sample(0,0,false)==0 && field.Sample(1,1,false)==1,"linear height sampling wraps or clamps at texel borders");
    graph::NodeGraph g;
    auto base=g.CreateNode(graph::NodeKind::BaseRock),apply=g.CreateNode(graph::NodeKind::ApplyMaterial),surface=g.CreateNode(graph::NodeKind::Surface),
         sub=g.CreateNode(graph::NodeKind::Subdivide),disp=g.CreateNode(graph::NodeKind::Displace);
    const auto link=[&](auto a,auto b,int pin=0){return g.CreateLink(g.FindNode(a)->outputs[0].id,g.FindNode(b)->inputs[pin].id);};
    Check(link(base,apply) && link(surface,apply,1) && link(apply,sub) && link(sub,disp),"detail nodes expose mesh input and output");
    Check(g.FindNode(sub)->inputs.size()==2 && g.FindNode(sub)->inputs[1].valueType==graph::ValueType::Mask,"Subdivide has a Mask input");
    graph::MaterialHeight heights;
    auto& material=heights.surfaces[surface]; material.field={1,1,{1}}; material.mapping.method=compositor::MappingMethod::Triplanar;
    material.axes={geometry::Vec3{1,0,0},geometry::Vec3{0,1,0},geometry::Vec3{0,0,1}};
    graph::RockEvaluationCache cache;
    auto evaluate=[&](auto id){return graph::EvaluateRocks(g,id,&cache,geometry::VolumeMeshingMethod::MarchingTetrahedra,{},nullptr,&heights);};
    auto r=evaluate(disp);
    Check(r.error.empty() && r.rocks.size()==1 && r.rocks[0].materials.size()==1 && !r.rocks[0].boxes && !r.rocks[0].bakeSource,
          "displace retains material and invalidates analytic shape shortcut");
    const auto dec=g.CreateNode(graph::NodeKind::Decimate);
    link(disp,dec);
    auto simplified=evaluate(dec);
    Check(simplified.error.empty() && cache.entries.contains(disp) && cache.entries.contains(dec),"displace and downstream decimate retain persistent results");
    const auto displaceRuns=cache.computations[disp],decimateRuns=cache.computations[dec];
    evaluate(dec);
    g.CreateNode(graph::NodeKind::BaseRock);
    evaluate(dec);
    auto identicalHeights=heights;
    graph::EvaluateRocks(g,dec,&cache,geometry::VolumeMeshingMethod::MarchingTetrahedra,{},nullptr,&identicalHeights);
    Check(cache.computations[disp]==displaceRuns && cache.computations[dec]==decimateRuns,"unchanged inputs, unrelated edits and identical snapshots skip detail computation");
    std::get<geometry::DecimateSettings>(g.FindMutableNode(dec)->settings).targetTriangles=24;
    evaluate(dec);
    Check(cache.computations[disp]==displaceRuns && cache.computations[dec]==decimateRuns+1,"downstream edits reuse displacement");
    auto first=r.rocks[0].mesh.positions;
    material.field.pixels[0]=0;
    r=evaluate(disp);
    Check(r.error.empty() && r.rocks[0].mesh.positions!=first,"height changes invalidate displaced geometry even with cached subdivision");
    evaluate(dec);
    Check(cache.computations[disp]==displaceRuns+1 && cache.computations[dec]==decimateRuns+2,"height edits invalidate displacement and downstream decimation");
    const auto missing=graph::EvaluateRocks(g,disp,&cache);
    Check(!missing.error.empty() && !cache.entries.contains(disp),"missing height context cannot reuse previous displacement");
    std::get<graph::LayerNodeSettings>(g.FindMutableNode(surface)->settings).layer.enabled=false;
    Check(!evaluate(disp).error.empty(),"disabled material invalidates cached bindings below subdivision");
    std::get<graph::LayerNodeSettings>(g.FindMutableNode(surface)->settings).layer.enabled=true;
    material.mapping.method=compositor::MappingMethod::UV;
    Check(!evaluate(disp).error.empty(),"UV projection without UV is diagnosed");
    material.mapping.method=compositor::MappingMethod::Triplanar;
    const auto other=g.CreateNode(graph::NodeKind::Surface),mask=g.CreateNode(graph::NodeKind::MaterialMask),top=g.CreateNode(graph::NodeKind::ApplyMaterial);
    link(sub,top);link(other,top,1);link(mask,top,2);link(top,disp);
    heights.surfaces[other]=material; heights.surfaces[other].field.pixels[0]=1;
    std::get<graph::MaterialMaskSettings>(g.FindMutableNode(mask)->settings).value=.5f;
    r=evaluate(disp); auto before=evaluate(sub);
    Check(r.error.empty() && r.rocks[0].mesh.positions==before.rocks[0].mesh.positions,"masked heights blend before displacement");
    heights.surfaces[other].channels=1;
    r=evaluate(disp);
    Check(r.error.empty() && r.rocks[0].mesh.positions!=before.rocks[0].mesh.positions,"disabled height channel preserves lower material height");
    compositor::MaterialMask imageMask; imageMask.texture=1;
    heights.masks[mask]={1,1,{.25f}};
    material.field.pixels[0]=1;
    const float sampled=heights.Sample(surface,&imageMask,mask,{0,0,0},{0,1,0},{0,0},0,false);
    imageMask.invert=true;
    Check(std::abs(sampled-.25f)<1e-6 && std::abs(heights.Sample(surface,&imageMask,mask,{0,0,0},{0,1,0},{0,0},0,false)-.75f)<1e-6,
          "image mask and inversion affect height blend");
    DocumentSnapshot original; original.graphNodes=g.Nodes(); original.graphLinks=g.Links();
    std::get<geometry::DisplaceSettings>(g.FindMutableNode(disp)->settings).amount=.3f;
    DocumentSnapshot edited; edited.graphNodes=g.Nodes(); edited.graphLinks=g.Links();
    UndoHistory history; history.Push(original,0);
    const auto restored=history.Undo(edited); g.Replace(restored.graphNodes,restored.graphLinks);
    Check(std::get<geometry::DisplaceSettings>(g.FindNode(disp)->settings).amount==.05f,"displace settings undo");
    auto newBase=g.CreateNode(graph::NodeKind::BaseRock),merge=g.CreateNode(graph::NodeKind::Merge),multi=g.CreateNode(graph::NodeKind::Subdivide),output=g.CreateNode(graph::NodeKind::MeshOutput);
    link(base,merge,0);link(newBase,merge,1);link(merge,multi);link(multi,output);
    Check(evaluate(output).rocks.size()==2,"subdivided branches survive output deduplication");
}
