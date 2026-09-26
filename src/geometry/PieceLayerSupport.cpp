#include "geometry/Pieces.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rock::geometry {
namespace {
struct Point { double x, y; };
using Polygon = std::vector<Point>;
double Cross(Point a, Point b, Point c) {
    return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}
Polygon Hull(Polygon points) {
    std::sort(points.begin(),points.end(),[](auto a,auto b){return a.x<b.x || (a.x==b.x && a.y<b.y);});
    points.erase(std::unique(points.begin(),points.end(),[](auto a,auto b){return a.x==b.x && a.y==b.y;}),points.end());
    if (points.size()<3) return {};
    Polygon hull;
    for (auto p:points) {
        while (hull.size()>1 && Cross(hull[hull.size()-2],hull.back(),p)<=0) hull.pop_back();
        hull.push_back(p);
    }
    const size_t lower=hull.size();
    for (size_t i=points.size()-1;i-->0;) {
        const auto p=points[i];
        while (hull.size()>lower && Cross(hull[hull.size()-2],hull.back(),p)<=0) hull.pop_back();
        hull.push_back(p);
    }
    hull.pop_back();return hull;
}
double Area(const Polygon& p) {
    double area=0;
    for (size_t i=1;i+1<p.size();++i) area+=Cross(p[0],p[i],p[i+1]);
    return std::abs(area)*.5;
}
double Overlap(Polygon subject,const Polygon& clip) {
    if (subject.empty() || clip.empty()) return 0;
    for (size_t i=0;i<clip.size();++i) {
        const auto a=clip[i],b=clip[(i+1)%clip.size()];
        Polygon result;
        for (size_t j=0;j<subject.size();++j) {
            const auto p=subject[j],q=subject[(j+1)%subject.size()];
            const double dp=Cross(a,b,p),dq=Cross(a,b,q);
            if (dp>=0) result.push_back(p);
            if ((dp>=0)!=(dq>=0)) {
                const double t=dp/(dp-dq);
                result.push_back({p.x+t*(q.x-p.x),p.y+t*(q.y-p.y)});
            }
        }
        subject=std::move(result);
        if (subject.size()<3) return 0;
    }
    return Area(subject);
}
}

bool BuildPieceLayerSupport(PieceCollection& c,std::string& error,std::stop_token stop) {
    error.clear();
    if (stop.stop_requested()) {error="評価をキャンセルしました";return false;}
    // 平行な板の上下面だけを投影する。片全体の投影では、上下面に届かない片も接続してしまう。
    const size_t n=c.pieces.size();
    std::vector<std::array<Polygon,2>> caps(n);
    struct Bounds {
        double minX=std::numeric_limits<double>::infinity(), minY=minX;
        double maxX=-std::numeric_limits<double>::infinity(), maxY=maxX;
    };
    std::vector<std::array<Bounds,2>> bounds(n);
    std::vector<std::shared_ptr<PieceNeighborhood>> neighborhoods(n);
    const auto first=std::find_if(c.pieces.begin(),c.pieces.end(),[](const auto& p){return p.layer>=0;});
    if (first==c.pieces.end()) return true;
    const auto& basis=first->transform;
    std::array<double,3> u{basis[0],basis[4],basis[8]},v{basis[2],basis[6],basis[10]};
    const auto normalize=[](auto& a) {
        const double length=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
        if (length<=0 || !std::isfinite(length)) return false;
        for (auto& x:a) x/=length;
        return true;
    };
    if (!normalize(u)) {error="積層の変換が不正です";return false;}
    const double dot=u[0]*v[0]+u[1]*v[1]+u[2]*v[2];
    for (int k=0;k<3;++k) v[k]-=dot*u[k];
    if (!normalize(v)) {error="積層の変換が不正です";return false;}
    for (size_t i=0;i<n;++i) {
        if (stop.stop_requested()) {error="評価をキャンセルしました";return false;}
        const auto& p=c.pieces[i];
        if (p.layer<0 || !p.mesh || !p.neighborhood) continue;
        for (int k:{0,1,2,4,5,6,8,9,10}) if (std::abs(p.transform[k]-basis[k])>1e-9) {
            error="上下の支持は同じ向きの平行な板に対応しています";return false;
        }
        auto nb=std::make_shared<PieceNeighborhood>(*p.neighborhood);
        nb->vertical.clear();nb->fixedLayerSupport=true;nb->supportTransform=p.transform;
        const double tolerance=std::max(1e-8,double(p.layerSize[1])*1e-5);
        for (int side=0;side<2;++side) {
            Polygon points;
            const double height=(side?1:-1)*p.layerSize[1]*.5;
            for (const auto& face:p.mesh->triangles) {
                bool on=true;
                for (auto index:face) on &= std::abs(p.mesh->positions[index].y-height)<=tolerance;
                if (!on) continue;
                for (auto index:face) {
                    const auto a=p.mesh->positions[index];const auto& m=p.transform;
                    const std::array<double,3> world{m[0]*a.x+m[1]*a.y+m[2]*a.z+m[3]-basis[3],
                        m[4]*a.x+m[5]*a.y+m[6]*a.z+m[7]-basis[7],m[8]*a.x+m[9]*a.y+m[10]*a.z+m[11]-basis[11]};
                    points.push_back({world[0]*u[0]+world[1]*u[1]+world[2]*u[2],
                                      world[0]*v[0]+world[1]*v[1]+world[2]*v[2]});
                }
            }
            caps[i][side]=Hull(std::move(points));nb->capAreas[side]=Area(caps[i][side]);
            auto& box=bounds[i][side];
            for (const auto& point:caps[i][side]) {
                box.minX=std::min(box.minX,point.x);box.maxX=std::max(box.maxX,point.x);
                box.minY=std::min(box.minY,point.y);box.maxY=std::max(box.maxY,point.y);
            }
        }
        neighborhoods[i]=std::move(nb);
    }
    for (size_t i=0;i<n;++i) {
        if (stop.stop_requested()) {error="評価をキャンセルしました";return false;}
        if (!neighborhoods[i]) continue;
        for (size_t j=0;j<n;++j) {
            if (!neighborhoods[j] || c.pieces[j].layer!=c.pieces[i].layer+1) continue;
            const auto& a=bounds[i][1];const auto& b=bounds[j][0];
            if (a.maxX<=b.minX || b.maxX<=a.minX || a.maxY<=b.minY || b.maxY<=a.minY) continue;
            const double area=Overlap(caps[i][1],caps[j][0]);
            const double threshold=std::min(neighborhoods[i]->capAreas[1],neighborhoods[j]->capAreas[0])*1e-10;
            if (area<=threshold || area<=0) continue;
            neighborhoods[i]->vertical.push_back({c.pieces[j].id,area,1});
            neighborhoods[j]->vertical.push_back({c.pieces[i].id,area,0});
        }
    }
    for (size_t i=0;i<n;++i) if (neighborhoods[i]) c.pieces[i].neighborhood=std::move(neighborhoods[i]);
    return true;
}
} // namespace rock::geometry
