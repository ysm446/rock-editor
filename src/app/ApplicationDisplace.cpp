#include "app/Application.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace rock {
namespace {
bool ReadScalar(rhi::Device& device,const rhi::GpuTexture& source,graph::ScalarField& image) {
    if(source.format!=DXGI_FORMAT_R32_FLOAT) return false;
    rhi::GpuTexture texture=source;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes=0;
    const auto desc=texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    rhi::GpuBuffer buffer;
    if(!device.Allocator().CreateReadbackBuffer(bytes,L"Displace height readback",buffer)) return false;
    const auto state=texture.state;
    const bool copied=device.ExecuteImmediate([&](auto* list) {
        rhi::TransitionIfNeeded(list,texture,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION dst(buffer.resource.Get(),footprint),src(texture.resource.Get(),0);
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        rhi::TransitionIfNeeded(list,texture,state);
    });
    void* mapped=nullptr; const D3D12_RANGE range{0,SIZE_T(bytes)};
    const bool ok=copied && SUCCEEDED(buffer.resource->Map(0,&range,&mapped));
    if(ok) {
        image.width=texture.width; image.height=texture.height; image.pixels.resize(size_t(image.width)*image.height);
        for(uint32_t y=0;y<image.height;++y)
            std::memcpy(image.pixels.data()+size_t(y)*image.width,static_cast<const uint8_t*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,size_t(image.width)*sizeof(float));
        const D3D12_RANGE written{0,0}; buffer.resource->Unmap(0,&written);
    }
    device.DeferRelease(buffer); return ok;
}
}
void Application::PrepareMaterialHeights() {
    // グラフ全体の改版ではなく、参照中のハイト入力だけを照合する。
    std::unordered_set<graph::GraphId> visited;
    std::vector<graph::GraphId> todo;
    std::vector<const graph::Node*> sources;
    for(const auto& node:m_graph.Nodes()) if(node.kind==graph::NodeKind::Displace) todo.push_back(node.id);
    while(!todo.empty()) {
        const auto id=todo.back(); todo.pop_back();
        if(!visited.insert(id).second) continue;
        const auto* node=m_graph.FindNode(id); if(!node) continue;
        for(const auto& pin:node->inputs) if(const auto* parent=m_graph.FindUpstreamNodeForPin(pin.id)) todo.push_back(parent->id);
        if(node->kind==graph::NodeKind::Surface || node->kind==graph::NodeKind::MaterialMask) sources.push_back(node);
    }
    std::sort(sources.begin(),sources.end(),[](auto* a,auto* b){return a->id<b->id;});
    std::string key;
    const auto add=[&](const auto& value) { key.append(reinterpret_cast<const char*>(&value),sizeof(value)); };
    const auto textureKey=[&](compositor::TextureId id) {
        add(id);
        const auto* image=m_textureLibrary.Find(id);
        const bool valid=image && !image->missing; add(valid);
        if(image) add(image->contentRevision);
    };
    for(const auto* node:sources) {
        add(node->id);
        if(const auto* settings=std::get_if<graph::LayerNodeSettings>(&node->settings)) {
            const auto& m=settings->layer.mapping;
            add(m.method); add(m.repeatMeters); add(m.offset); add(m.rotationDegrees); add(m.sharpness);
            add(settings->layer.channelMask);
            const auto layers=m_graph.CompileLayersTo(node->id).layers; add(layers.size());
            for(const auto& layer:layers) {
                add(layer.enabled); add(layer.channelMask); add(layer.uvScale); add(layer.material);
                if(layer.material) {
                    const auto* asset=m_materialLibrary.Find(layer.material);
                    const bool valid=asset!=nullptr; add(valid);
                    if(asset) { textureKey(asset->height.texture); add(asset->height.channel); }
                } else {
                    add(layer.heightSource); add(layer.heightBase); add(layer.heightGain);
                    add(layer.heightNoise.type); add(layer.heightNoise.scale); add(layer.heightNoise.octaves); add(layer.heightNoise.offset);
                }
            }
        } else if(const auto* mask=std::get_if<graph::MaterialMaskSettings>(&node->settings)) textureKey(mask->texture);
    }
    if(m_materialHeights && m_materialHeightKey==key) return;
    auto context=std::make_shared<graph::MaterialHeight>();
    for(const auto* node:sources) {
        const auto id=node->id;
        if(const auto* settings=std::get_if<graph::LayerNodeSettings>(&node->settings); settings && node->kind==graph::NodeKind::Surface) {
            auto& output=context->surfaces[id]; output.mapping=settings->layer.mapping; output.channels=settings->layer.channelMask;
            const auto& mapping=output.mapping;
            const auto finite=[](const DirectX::XMFLOAT3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
            if(!finite(mapping.offset) || !finite(mapping.rotationDegrees) || !std::isfinite(mapping.repeatMeters) || mapping.repeatMeters<.001f || !std::isfinite(mapping.sharpness) || mapping.sharpness<1 || mapping.sharpness>64) {
                output.error="Surfaceの投影設定が不正です"; continue;
            }
            const auto rotation=DirectX::XMMatrixRotationRollPitchYaw(DirectX::XMConvertToRadians(mapping.rotationDegrees.x),DirectX::XMConvertToRadians(mapping.rotationDegrees.y),DirectX::XMConvertToRadians(mapping.rotationDegrees.z));
            for(int i=0;i<3;++i) { DirectX::XMFLOAT3 axis; DirectX::XMStoreFloat3(&axis,rotation.r[i]); output.axes[i]={axis.x,axis.y,axis.z}; }
            compositor::MaterialStack stack; stack.Layers()=m_graph.CompileLayersTo(id).layers;
            uint32_t resolution=1024;
            for(auto& layer:stack.Layers()) if(layer.material) {
                layer.heightSource=compositor::ValueSource::Texture; layer.heightBase=compositor::kHeightPivot; layer.heightGain=1;
                const auto* asset=m_materialLibrary.Find(layer.material);
                if(!asset) { output.error="参照する材質がありません"; break; }
                if(asset->height.texture) {
                    const auto* image=m_textureLibrary.Find(asset->height.texture);
                    if(!image || image->missing) { output.error="ハイトテクスチャがリンク切れです"; break; }
                    resolution=std::max(resolution,std::max(image->texture.width,image->texture.height));
                }
            }
            if(!output.error.empty()) continue;
            resolution=std::clamp(resolution,128u,4096u); stack.SetTerrainScale(mapping.repeatMeters,0);
            compositor::MaterialEvaluator evaluator;
            bool ok=false;
            if(evaluator.Create(m_device,resolution,false)) {
                const bool executed=m_device.ExecuteImmediate([&](auto* list) { ok=evaluator.Evaluate(m_device,m_pipelineCache,list,stack,m_textureLibrary,m_materialLibrary,{{0,0,resolution,resolution}}); });
                ok=executed && ok && ReadScalar(m_device,evaluator.Textures().height,output.field);
            }
            evaluator.Destroy(m_device);
            if(!ok) output.error="素材のハイトを準備できません";
        } else if(const auto* mask=std::get_if<graph::MaterialMaskSettings>(&node->settings); mask && mask->texture) {
            const auto* image=m_textureLibrary.Find(mask->texture);
            if(!image || image->missing) { context->maskErrors[id]="マスク画像がリンク切れです"; continue; }
            rhi::GpuTexture target; rhi::TextureDesc desc;
            desc.width=image->texture.width; desc.height=image->texture.height; desc.format=DXGI_FORMAT_R32_FLOAT;
            desc.allowUnorderedAccess=true; desc.initialState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            auto* pipeline=m_pipelineCache.GetCompute(L"ScalarSnapshot.hlsl",L"CsMain");
            bool ok=pipeline && m_device.Allocator().CreateTexture2D(desc,target);
            if(ok) ok=m_device.ExecuteImmediate([&](auto* list) {
                const uint32_t constants[]={m_textureLibrary.SrvIndex(mask->texture,false),target.UavIndex(),desc.width,desc.height};
                list->SetComputeRootSignature(m_pipelineCache.GlobalRootSignature()); list->SetPipelineState(pipeline);
                list->SetComputeRoot32BitConstants(0,4,constants,0); list->Dispatch(rhi::DispatchCount(desc.width),rhi::DispatchCount(desc.height),1);
            });
            graph::ScalarField field;
            if(ok && ReadScalar(m_device,target,field)) context->masks[id]=std::move(field);
            else context->maskErrors[id]="マスク画像を準備できません";
            m_device.DeferRelease(target);
        }
    }
    m_materialHeights=std::move(context);
    m_materialHeightKey=std::move(key);
}
}
