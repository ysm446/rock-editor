#include "graph/MaterialHeight.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rock::graph {
float ScalarField::Sample(float u,float v,bool wrap) const {
    if(!width || !height || pixels.size()!=size_t(width)*height || !std::isfinite(u) || !std::isfinite(v))
        return std::numeric_limits<float>::quiet_NaN();
    if(wrap) {u-=std::floor(u); v-=std::floor(v);} else {u=std::clamp(u,0.f,1.f);v=std::clamp(v,0.f,1.f);}
    const float x=u*width-.5f,y=v*height-.5f; const int ix=int(std::floor(x)),iy=int(std::floor(y));
    const float tx=x-ix,ty=y-iy;
    const auto at=[&](int a,int b) {
        if(wrap) {a=(a%int(width)+int(width))%int(width);b=(b%int(height)+int(height))%int(height);}
        else {a=std::clamp(a,0,int(width)-1);b=std::clamp(b,0,int(height)-1);}
        return pixels[size_t(b)*width+a];
    };
    return std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),tx),std::lerp(at(ix,iy+1),at(ix+1,iy+1),tx),ty);
}
// スナップショットのアドレスではなく実値で照合する。Undoや再準備でも同じ内容を再利用できる。
std::map<GraphId,std::string> MaterialHeight::CacheKeys() const {
    std::map<GraphId,std::string> keys;
    const auto fieldKey=[](const ScalarField& field) {
        uint64_t hash=14695981039346656037ull;
        const auto bytes=[&](const void* data,size_t size) {
            const auto* p=static_cast<const unsigned char*>(data);
            for(size_t i=0;i<size;++i) { hash^=p[i]; hash*=1099511628211ull; }
        };
        bytes(&field.width,sizeof(field.width)); bytes(&field.height,sizeof(field.height));
        bytes(field.pixels.data(),field.pixels.size()*sizeof(float));
        return std::string(reinterpret_cast<const char*>(&hash),sizeof(hash));
    };
    for(const auto& [id,surface]:surfaces) {
        auto& key=keys[id]; key=fieldKey(surface.field);
        const auto add=[&](const auto& value) { key.append(reinterpret_cast<const char*>(&value),sizeof(value)); };
        const auto& m=surface.mapping;
        add(m.method); add(m.repeatMeters); add(m.offset); add(m.rotationDegrees); add(m.sharpness);
        for(const auto& axis:surface.axes) add(axis);
        add(surface.channels); key+=surface.error;
    }
    for(const auto& [id,field]:masks) keys[id]=fieldKey(field);
    for(const auto& [id,error]:maskErrors) keys[id]+="error:"+error;
    return keys;
}
float MaterialHeight::HeightBlendWeight(float mask,float height,float below,float range) {
    // 差 (height - below) を 0〜1 へ（等しければ 0.5）。マスクがしきい値 (1 - mask) を動かし、その周りを range で伸ばす。
    // マスク 0 は必ず 0、マスク 1 は必ず 1。マスク 0.5 でハイトが等しければ 0.5。
    const float d=(height-below)*.5f+.5f;
    return std::clamp((d-(1-mask))/std::max(range,.01f)+mask,0.f,1.f);
}
float MaterialHeight::Sample(GraphId surface,const compositor::MaterialMask* mask,GraphId maskId,
                              geometry::Vec3 p,geometry::Vec3 n,geometry::Mesh::Uv uv,float below,bool uvWrap,
                              bool heightBlend,float heightBlendRange) const {
    const auto& s=surfaces.at(surface);
    if(!(s.channels&compositor::ChannelBit(compositor::Channel::Height))) return below;
    const auto weights=[](geometry::Vec3 normal,float sharp) {
        geometry::Vec3 w{std::pow(std::abs(normal.x),sharp),std::pow(std::abs(normal.y),sharp),std::pow(std::abs(normal.z),sharp)};
        const float sum=std::max(w.x+w.y+w.z,1e-8f); return geometry::Vec3{w.x/sum,w.y/sum,w.z/sum};
    };
    float height;
    if(s.mapping.method==compositor::MappingMethod::Triplanar) {
        const auto dot=[](geometry::Vec3 a,geometry::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
        const geometry::Vec3 q{p.x-s.mapping.offset.x,p.y-s.mapping.offset.y,p.z-s.mapping.offset.z};
        const float inv=1/s.mapping.repeatMeters;
        const geometry::Vec3 r{dot(q,s.axes[0])*inv,dot(q,s.axes[1])*inv,dot(q,s.axes[2])*inv};
        const geometry::Vec3 normal{dot(n,s.axes[0]),dot(n,s.axes[1]),dot(n,s.axes[2])};
        const auto w=weights(normal,s.mapping.sharpness);
        height=s.field.Sample(-r.z*(normal.x<0?-1:1),r.y,true)*w.x+
               s.field.Sample(r.x,-r.z*(normal.y<0?-1:1),true)*w.y+
               s.field.Sample(r.x*(normal.z<0?-1:1),r.y,true)*w.z;
    } else height=s.field.Sample(uv.u,uv.v,uvWrap);
    float alpha=mask?mask->value:1;
    if(mask && mask->texture) {
        const auto& field=masks.at(maskId); const float inv=1/mask->repeatMeters;
        if(mask->triplanar) {
            const auto w=weights(n,4);
            alpha*=field.Sample(p.z*inv,p.y*inv,true)*w.x+field.Sample(p.x*inv,p.z*inv,true)*w.y+field.Sample(p.x*inv,p.y*inv,true)*w.z;
        } else alpha*=field.Sample(uv.u*inv,uv.v*inv,true);
    }
    if(mask && mask->invert) alpha=1-alpha;
    alpha=std::clamp(alpha,0.f,1.f);
    if(heightBlend) alpha=HeightBlendWeight(alpha,height,below,heightBlendRange);
    return std::lerp(below,height,alpha);
}
}
