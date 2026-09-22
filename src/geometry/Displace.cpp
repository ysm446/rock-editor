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
Mesh SubdivideMesh(const Mesh& input,const SubdivideSettings& settings,std::string& error,std::stop_token stop,DetailProgress progress,
                   const std::vector<float>& faceMask) {
    error.clear();
    if(Cancel(stop,error)) return {};
    size_t predicted=input.triangles.size();
    if(settings.levels<0 || settings.levels>6 || predicted>kMaxDetailTriangles || !std::isfinite(settings.threshold) ||
       (!faceMask.empty() && faceMask.size()!=input.triangles.size())) { error="細分化の設定または面数が上限を超えています"; return {}; }
    for(int i=0;i<settings.levels;++i) {
        if(predicted>kMaxDetailTriangles/4) { error="細分化後の三角形数が300万面を超えます。段階数を下げてください"; return {}; }
        predicted*=4;
    }
    MeshInfo info;
    if(!InspectMesh(input,info)) { error="細分化するメッシュが不正です"; return {}; }
    const bool uv=HasValidUvs(input);
    if(!input.cornerUvs.empty() && !uv) { error="入力UVが不正です"; return {}; }
    Mesh mesh=input;
    // 割る面。マスクが無ければ全面。マスクがあれば、面の中心の値がしきい値以上の面。
    std::vector<uint8_t> selected(mesh.triangles.size(),1);
    if(!faceMask.empty()) for(size_t f=0;f<faceMask.size();++f) selected[f]=faceMask[f]>=settings.threshold?1:0;
    for(int level=0;level<settings.levels;++level) {
        Mesh next; next.positions=mesh.positions; next.uvWidth=mesh.uvWidth; next.uvHeight=mesh.uvHeight;
        next.triangles.reserve(mesh.triangles.size()*4);
        std::vector<uint8_t> nextSelected; nextSelected.reserve(mesh.triangles.size()*4);
        // 先に、割る面の辺だけに中点を作る。隣の面はこの表を見て、共有辺が割れているかを知る。
        std::unordered_map<uint64_t,uint32_t> edges;
        const auto key=[](uint32_t a,uint32_t b){ if(a>b) std::swap(a,b); return (uint64_t(a)<<32)|b; };
        const auto midpoint=[&](uint32_t a,uint32_t b) {
            const auto k=key(a,b);
            if(auto it=edges.find(k);it!=edges.end()) return it->second;
            const auto id=uint32_t(next.positions.size());
            next.positions.push_back(Mul(Add(mesh.positions[a],mesh.positions[b]),.5f)); edges[k]=id; return id;
        };
        for(size_t f=0;f<mesh.triangles.size();++f) {
            if(!selected[f]) continue;
            const auto t=mesh.triangles[f]; midpoint(t[0],t[1]); midpoint(t[1],t[2]); midpoint(t[2],t[0]);
        }
        const bool charts=uv && mesh.uvCharts.size()==mesh.triangles.size();
        const auto mid=[](Mesh::Uv a,Mesh::Uv b){return Mesh::Uv{(a.u+b.u)*.5f,(a.v+b.v)*.5f};};
        const auto emit=[&](std::array<uint32_t,3> tri,std::array<Mesh::Uv,3> tuv,size_t parent,uint8_t sel) {
            next.triangles.push_back(tri); nextSelected.push_back(sel);
            if(uv) { next.cornerUvs.push_back(tuv); if(charts) next.uvCharts.push_back(mesh.uvCharts[parent]); }
        };
        for(size_t f=0;f<mesh.triangles.size();++f) {
            if((f%1024)==0 && Cancel(stop,error)) return {};
            const auto t=mesh.triangles[f];
            const std::array<Mesh::Uv,3> u=uv?mesh.cornerUvs[f]:std::array<Mesh::Uv,3>{};
            // 各辺の中点（割れていなければ UINT32_MAX）。
            std::array<uint32_t,3> m{};
            int splitCount=0;
            for(int e=0;e<3;++e) {
                const auto it=edges.find(key(t[e],t[(e+1)%3]));
                m[e]=it==edges.end()?UINT32_MAX:it->second; splitCount+=m[e]!=UINT32_MAX;
            }
            const std::array<Mesh::Uv,3> mu{mid(u[0],u[1]),mid(u[1],u[2]),mid(u[2],u[0])};
            if(splitCount==0) { emit(t,u,f,0); continue; }
            if(splitCount==3) {
                // 4分割。割る面はその子も割り続ける。隣接だけで3辺が割れた面の子は割らない。
                emit({t[0],m[0],m[2]},{u[0],mu[0],mu[2]},f,selected[f]); emit({m[0],t[1],m[1]},{mu[0],u[1],mu[1]},f,selected[f]);
                emit({m[2],m[1],t[2]},{mu[2],mu[1],u[2]},f,selected[f]); emit({m[0],m[1],m[2]},{mu[0],mu[1],mu[2]},f,selected[f]);
                continue;
            }
            // 辺 e が割れている: 頂点 e, e+1 の間。向きは保つ。
            if(splitCount==1) {
                const int e=m[0]!=UINT32_MAX?0:m[1]!=UINT32_MAX?1:2, a=e,b=(e+1)%3,c=(e+2)%3;
                emit({t[a],m[e],t[c]},{u[a],mu[e],u[c]},f,0); emit({m[e],t[b],t[c]},{mu[e],u[b],u[c]},f,0);
                continue;
            }
            // 2辺が割れている: 割れていない辺 e の向かいの頂点 c から、2つの中点へ。
            const int e=m[0]==UINT32_MAX?0:m[1]==UINT32_MAX?1:2, a=e,b=(e+1)%3,c=(e+2)%3;
            const uint32_t mb=m[b],mc=m[c]; const Mesh::Uv ub=mu[b],uc=mu[c];  // mb: b-c の中点、mc: c-a の中点
            // 四角形 a,b,mb,mc は短い方の対角線で割る（細長い三角形を避ける）。
            const auto len2=[&](uint32_t p,uint32_t q){ const auto d=Sub(next.positions[p],next.positions[q]); return Dot(d,d); };
            emit({mc,mb,t[c]},{uc,ub,u[c]},f,0);
            if(len2(t[a],mb)<=len2(t[b],mc)) { emit({t[a],t[b],mb},{u[a],u[b],ub},f,0); emit({t[a],mb,mc},{u[a],ub,uc},f,0); }
            else { emit({t[a],t[b],mc},{u[a],u[b],uc},f,0); emit({t[b],mb,mc},{u[b],ub,uc},f,0); }
        }
        mesh=std::move(next); selected=std::move(nextSelected);
        if(mesh.triangles.size()>kMaxDetailTriangles) { error="細分化後の三角形数が300万面を超えます。段階数を下げてください"; return {}; }
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
        error="変位の設定または面数が不正です（最大300万面）"; return {};
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
