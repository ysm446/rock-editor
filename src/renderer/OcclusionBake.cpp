#include "renderer/OcclusionBake.h"
#include "geometry/UvUnwrap.h"
#include "rhi/TextureReadback.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace rock::renderer {
namespace {
using V = geometry::Vec3;
struct Triangle { V a,b,c; geometry::Mesh::Uv uv0,uv1,uv2; };
struct Node { V lo; uint32_t escape; V hi; uint32_t triangle; };
static_assert(sizeof(Triangle)==60 && sizeof(Node)==32);
V Min(V a,V b) { return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)}; }
V Max(V a,V b) { return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)}; }
float Axis(V v,int a) { return a==0?v.x:a==1?v.y:v.z; }
struct Builder {
    const std::vector<Triangle>& triangles;
    std::vector<Node> nodes;
    std::vector<uint32_t> order;
    bool uv=false;
    Node Bounds(uint32_t i) const {
        const auto& t=triangles[i];
        V a=t.a,b=t.b,c=t.c;
        if(uv) { a={t.uv0.u,t.uv0.v,0}; b={t.uv1.u,t.uv1.v,0}; c={t.uv2.u,t.uv2.v,0}; }
        return {Min(a,Min(b,c)),0,Max(a,Max(b,c)),i};
    }
    void Build(size_t begin,size_t end) {
        Node n=Bounds(order[begin]);
        for(size_t i=begin+1;i<end;++i) { auto b=Bounds(order[i]); n.lo=Min(n.lo,b.lo); n.hi=Max(n.hi,b.hi); }
        const auto index=nodes.size(); nodes.push_back(n);
        if(end-begin>1) {
            nodes[index].triangle=UINT32_MAX;
            V extent{n.hi.x-n.lo.x,n.hi.y-n.lo.y,n.hi.z-n.lo.z};
            int axis=extent.y>extent.x?1:0;
            if(extent.z>Axis(extent,axis)) axis=2;
            const auto mid=(begin+end)/2;
            std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,[&](uint32_t a,uint32_t b) {
                const auto x=Bounds(a),y=Bounds(b);
                return Axis(x.lo,axis)+Axis(x.hi,axis)<Axis(y.lo,axis)+Axis(y.hi,axis);
            });
            Build(begin,mid); Build(mid,end);
        }
        nodes[index].escape=uint32_t(nodes.size());
    }
};
template<class T> bool Upload(rhi::Device& device,const std::vector<T>& data,rhi::GpuBuffer& buffer) {
    rhi::GpuBuffer upload;
    if(!device.Allocator().CreateStructuredBuffer(uint32_t(data.size()),sizeof(T),L"AO BVH",buffer) ||
       !device.Allocator().CreateUploadBuffer(data.size()*sizeof(T),L"AO upload",upload)) return false;
    void* mapped=nullptr; const D3D12_RANGE range{0,0};
    if(FAILED(upload.resource->Map(0,&range,&mapped))) { device.DeferRelease(upload); return false; }
    std::memcpy(mapped,data.data(),data.size()*sizeof(T)); upload.resource->Unmap(0,nullptr);
    const bool ok=device.ExecuteImmediate([&](auto* list) {
        auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),buffer.state,D3D12_RESOURCE_STATE_COPY_DEST);
        if(buffer.state!=D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1,&barrier);
        list->CopyBufferRegion(buffer.resource.Get(),0,upload.resource.Get(),0,data.size()*sizeof(T));
        barrier=CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1,&barrier);
    });
    buffer.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    device.DeferRelease(upload); return ok;
}
}
bool OcclusionBake::Create(rhi::Device& device,const geometry::Mesh& mesh,float distance,int samples,float strength,std::string& error) {
    Release(device);
    geometry::MeshInfo info;
    if(!geometry::HasValidUvs(mesh) || !geometry::InspectMesh(mesh,info) || mesh.triangles.empty() ||
       !mesh.uvWidth || !mesh.uvHeight || mesh.uvWidth>4096 || mesh.uvHeight>4096 ||
       !std::isfinite(distance) || distance<=0 || samples<8 || samples>128 ||
       !std::isfinite(strength) || strength<0 || strength>1) { error="GPU形状AOの入力または設定が不正です"; return false; }
    std::vector<Triangle> triangles; triangles.reserve(mesh.triangles.size());
    for(size_t i=0;i<mesh.triangles.size();++i) {
        const auto f=mesh.triangles[i]; const auto uv=mesh.cornerUvs[i];
        triangles.push_back({mesh.positions[f[0]],mesh.positions[f[1]],mesh.positions[f[2]],uv[0],uv[1],uv[2]});
    }
    Builder builder{triangles,{}, {},false};
    builder.order.resize(triangles.size()); std::iota(builder.order.begin(),builder.order.end(),0u);
    builder.nodes.reserve(triangles.size()*4);
    builder.Build(0,triangles.size()); m_uvRoot=uint32_t(builder.nodes.size());
    builder.uv=true; builder.Build(0,triangles.size());
    rhi::TextureDesc desc; desc.width=mesh.uvWidth; desc.height=mesh.uvHeight;
    desc.allowUnorderedAccess=true; desc.initialState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; desc.debugName=L"Geometry AO";
    if(!Upload(device,triangles,m_triangles) || !Upload(device,builder.nodes,m_nodes) || !device.Allocator().CreateTexture2D(desc,m_output)) {
        error="GPU形状AOのリソースを作成できません"; Release(device); return false;
    }
    m_distance=distance; m_samples=uint32_t(samples); m_strength=strength;
    m_bias=std::min(distance*1e-3f,std::max({info.maximum.x-info.minimum.x,info.maximum.y-info.minimum.y,info.maximum.z-info.minimum.z})*1e-6f);
    m_tiles=((mesh.uvWidth+127)/128)*((mesh.uvHeight+63)/64);
    return true;
}
bool OcclusionBake::Step(rhi::Device& device,rhi::PipelineCache& pipelines,std::string& error) {
    auto* pipeline=pipelines.GetCompute(L"OcclusionBake.hlsl",L"CsMain");
    if(!pipeline || !m_tiles) { error="GPU形状AOのシェーダを準備できません"; return false; }
    if(Complete()) return true;
    struct Constants { uint32_t triangles,nodes,output,uvRoot,width,height,x,y; float distance,strength,bias; uint32_t samples; };
    const uint32_t columns=(m_output.width+127)/128;
    Constants c{m_triangles.srv.index,m_nodes.srv.index,m_output.UavIndex(),m_uvRoot,m_output.width,m_output.height,
                (m_completed%columns)*128,(m_completed/columns)*64,m_distance,m_strength,m_bias,m_samples};
    if(!device.ExecuteImmediate([&](auto* list) {
        list->SetComputeRootSignature(pipelines.GlobalRootSignature()); list->SetPipelineState(pipeline);
        list->SetComputeRoot32BitConstants(0,sizeof(c)/4,&c,0);
        list->Dispatch(16,8,1);
    })) { error="GPU形状AOの計算に失敗しました"; return false; }
    ++m_completed; return true;
}
bool OcclusionBake::Read(rhi::Device& device,LdrImage& image) { return Complete() && rhi::ReadTextureRgba8(device,m_output,image); }
void OcclusionBake::Release(rhi::Device& device) {
    device.DeferRelease(m_triangles); device.DeferRelease(m_nodes); device.DeferRelease(m_output);
    m_completed=m_tiles=0;
}
}
