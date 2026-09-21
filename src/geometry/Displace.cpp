#include "geometry/Displace.h"
#include "geometry/UvUnwrap.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace rock::geometry {
namespace {
Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 Mul(Vec3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float Dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Unit(Vec3 a) { const float l=std::sqrt(Dot(a,a)); return l>1e-20f?Mul(a,1/l):Vec3{}; }
bool Cancel(std::stop_token stop,std::string& error) {
    if(!stop.stop_requested()) return false;
    error="評価をキャンセルしました"; return true;
}
}
Mesh SubdivideMesh(const Mesh& input,const SubdivideSettings& settings,std::string& error,std::stop_token stop,DetailProgress progress) {
    error.clear();
    if(Cancel(stop,error)) return {};
    size_t predicted=input.triangles.size();
    if(settings.levels<0 || settings.levels>6 || predicted>kMaxDetailTriangles) { error="細分化の設定または面数が上限を超えています"; return {}; }
    for(int i=0;i<settings.levels;++i) {
        if(predicted>kMaxDetailTriangles/4) { error="細分化後の三角形数が100万面を超えます。段階数を下げてください"; return {}; }
        predicted*=4;
    }
    MeshInfo info;
    if(!InspectMesh(input,info)) { error="細分化するメッシュが不正です"; return {}; }
    const bool uv=HasValidUvs(input);
    if(!input.cornerUvs.empty() && !uv) { error="入力UVが不正です"; return {}; }
    Mesh mesh=input;
    for(int level=0;level<settings.levels;++level) {
        Mesh next; next.positions=mesh.positions; next.uvWidth=mesh.uvWidth; next.uvHeight=mesh.uvHeight;
        next.triangles.reserve(mesh.triangles.size()*4);
        std::unordered_map<uint64_t,uint32_t> edges;
        const auto midpoint=[&](uint32_t a,uint32_t b) {
            if(a>b) std::swap(a,b);
            uint64_t key=(uint64_t(a)<<32)|b;
            if(auto it=edges.find(key);it!=edges.end()) return it->second;
            const auto id=uint32_t(next.positions.size());
            next.positions.push_back(Mul(Add(mesh.positions[a],mesh.positions[b]),.5f)); edges[key]=id; return id;
        };
        for(size_t f=0;f<mesh.triangles.size();++f) {
            if((f%1024)==0 && Cancel(stop,error)) return {};
            const auto t=mesh.triangles[f]; const auto ab=midpoint(t[0],t[1]),bc=midpoint(t[1],t[2]),ca=midpoint(t[2],t[0]);
            next.triangles.insert(next.triangles.end(),{{t[0],ab,ca},{ab,t[1],bc},{ca,bc,t[2]},{ab,bc,ca}});
            if(uv) {
                const auto u=mesh.cornerUvs[f];
                const auto mid=[](Mesh::Uv a,Mesh::Uv b){return Mesh::Uv{(a.u+b.u)*.5f,(a.v+b.v)*.5f};};
                const auto x=mid(u[0],u[1]),y=mid(u[1],u[2]),z=mid(u[2],u[0]);
                next.cornerUvs.insert(next.cornerUvs.end(),{{u[0],x,z},{x,u[1],y},{z,y,u[2]},{x,y,z}});
                if(mesh.uvCharts.size()==mesh.triangles.size()) next.uvCharts.insert(next.uvCharts.end(),4,mesh.uvCharts[f]);
            }
        }
        mesh=std::move(next);
        if(progress) progress((level+1)*100/std::max(settings.levels,1));
    }
    if(settings.levels>0 && !InspectMesh(mesh,info)) { error="細分化で面が潰れました。段階数を下げてください"; return {}; }
    return mesh;
}
Mesh DisplaceMesh(const Mesh& input,const DisplaceSettings& settings,const HeightSample& sample,std::string& error,std::stop_token stop,DetailProgress progress) {
    error.clear();
    if(Cancel(stop,error)) return {};
    if(!std::isfinite(settings.amount) || std::abs(settings.amount)>10 || !std::isfinite(settings.midpoint) ||
       settings.midpoint<0 || settings.midpoint>1 || !sample || input.triangles.size()>kMaxDetailTriangles) {
        error="変位の設定または面数が不正です（最大100万面）"; return {};
    }
    MeshInfo info;
    if(!InspectMesh(input,info)) { error="変位するメッシュが不正です"; return {}; }
    if(!input.cornerUvs.empty() && !HasValidUvs(input)) { error="入力UVが不正です"; return {}; }
    if(settings.amount==0) return input;
    std::vector<Vec3> normals(input.positions.size());
    // 角度重み付き法線。面の分割密度による境界の方向の偏りを抑える。
    const auto angle=[&](const auto& t,int c) {
        const auto a=Unit(Sub(input.positions[t[(c+1)%3]],input.positions[t[c]]));
        const auto b=Unit(Sub(input.positions[t[(c+2)%3]],input.positions[t[c]]));
        return std::acos(std::clamp(Dot(a,b),-1.f,1.f));
    };
    for(size_t f=0;f<input.triangles.size();++f) {
        if((f%1024)==0 && Cancel(stop,error)) return {};
        const auto t=input.triangles[f]; const auto n=Unit(Cross(Sub(input.positions[t[1]],input.positions[t[0]]),Sub(input.positions[t[2]],input.positions[t[0]])));
        for(int c=0;c<3;++c) normals[t[c]]=Add(normals[t[c]],Mul(n,angle(t,c)));
    }
    for(auto& n:normals) n=Unit(n);
    std::vector<double> heights(input.positions.size()),weights(input.positions.size());
    const bool uv=HasValidUvs(input);
    for(size_t f=0;f<input.triangles.size();++f) {
        if((f%1024)==0) { if(Cancel(stop,error)) return {}; if(progress) progress(int(f*90/input.triangles.size())); }
        const auto t=input.triangles[f];
        for(int c=0;c<3;++c) {
            const auto vertex=t[c]; const float w=angle(t,c);
            const float h=sample(input.positions[vertex],normals[vertex],uv?input.cornerUvs[f][c]:Mesh::Uv{});
            if(!std::isfinite(h)) { error="ハイトに非有限値があります"; return {}; }
            heights[vertex]+=double(h)*w; weights[vertex]+=w;
        }
    }
    Mesh mesh=input;
    for(size_t v=0;v<mesh.positions.size();++v) {
        if((v%4096)==0 && Cancel(stop,error)) return {};
        if(weights[v]>0) mesh.positions[v]=Add(input.positions[v],Mul(normals[v],float(heights[v]/weights[v]-settings.midpoint)*settings.amount));
    }
    if(!InspectMesh(mesh,info)) { error="変位で面が潰れました。変位量を下げてください"; return {}; }
    if(progress) progress(100);
    return mesh;
}
}
