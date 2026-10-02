#include "geometry/Pieces.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>

namespace rock::geometry {
double PieceFaceArea(const Piece& p, const std::array<double,3>& a) {
    // 面積ベクトルは変換の余因子行列で変換する。非均等倍率にも対応する。
    const auto& m=p.transform;
    const double x=(m[5]*m[10]-m[6]*m[9])*a[0]+(m[6]*m[8]-m[4]*m[10])*a[1]+(m[4]*m[9]-m[5]*m[8])*a[2];
    const double y=(m[2]*m[9]-m[1]*m[10])*a[0]+(m[0]*m[10]-m[2]*m[8])*a[1]+(m[1]*m[8]-m[0]*m[9])*a[2];
    const double z=(m[1]*m[6]-m[2]*m[5])*a[0]+(m[2]*m[4]-m[0]*m[6])*a[1]+(m[0]*m[5]-m[1]*m[4])*a[2];
    return std::sqrt(x*x+y*y+z*z);
}

// 面積ベクトルを変換したときの Y 成分（面の向きの判定用。長さは PieceFaceArea と同じ尺度）。
double PieceFaceNormalY(const Piece& p, const std::array<double,3>& a) {
    const auto& m=p.transform;
    return (m[2]*m[9]-m[1]*m[10])*a[0]+(m[0]*m[10]-m[2]*m[8])*a[1]+(m[1]*m[8]-m[0]*m[9])*a[2];
}

PieceSelection PeelPieces(const PieceCollection& c, const PieceSelectSettings& s,
                          const std::vector<float>& weights, std::string& error, std::stop_token stop) {
    error.clear();
    PieceSelection out{c.producer,c.generation,c.fingerprint,{}};
    if (stop.stop_requested()) { error="評価をキャンセルしました"; return {}; }
    if (!std::isfinite(s.stability) || s.stability<0 || s.stability>1) {
        error="安定は0〜1にしてください"; return {};
    }
    if (c.pieces.empty()) return out;
    if (!c.adjacencyComplete) {
        error="Peelには初回のVoronoi分割による隣接情報が必要です。再分割をまたぐ隣接面は未対応です";
        return {};
    }
    if (weights.size()!=c.pieces.size() || !std::isfinite(s.peelNoise) || s.peelNoise<0 || s.peelNoise>1) {
        error="侵食のばらつきは0〜1にしてください"; return {};
    }
    if (!std::isfinite(s.peelSize) || s.peelSize<0 || s.peelSize>1) {
        error="大きさの効きは0〜1にしてください"; return {};
    }
    if (!std::isfinite(s.peelRetreat) || s.peelRetreat<0 || s.peelRetreat>1000) {
        error="側面の後退は0〜1000 mにしてください"; return {};
    }
    if (!std::isfinite(s.peelRetreatBase) || s.peelRetreatBase<0 || s.peelRetreatBase>1) {
        error="底の後退は0〜1にしてください"; return {};
    }
    // 大きさの効き: 体積を最大の片で割った値（0～1）を順位に足す。大きな片ほど後まで残る。
    double largest=0;
    for (const auto& p:c.pieces) largest=std::max(largest,p.volume);
    struct State {
        std::vector<std::pair<size_t,double>> neighbors;
        struct Vertical { size_t other; double area; int otherSide; };
        std::vector<Vertical> vertical;
        std::array<double,2> cap{}, covered{};
        double total=0, support=0, noise=0;
        bool exposed=false, removed=false, protectedCore=false, grounded=false, tooDeep=false;
    };
    const size_t n=c.pieces.size();
    std::vector<State> state(n);
    std::map<uint32_t,size_t> index;
    for (size_t i=0;i<n;++i) if (!c.pieces[i].neighborhood || !index.emplace(c.pieces[i].id,i).second) {
        error="ピースの隣接情報が不正です";return {};
    }
    for (size_t i=0;i<n;++i) {
        const auto& p=c.pieces[i]; auto& v=state[i];
        if (p.neighborhood->fixedLayerSupport) {
            if (p.transform!=p.neighborhood->supportTransform) {
                error="上下の支持は分割時の固定座標を使います。PeelをPiece Transformより前へ接続してください";return {};
            }
            v.cap=p.neighborhood->capAreas;
            v.total=v.cap[0]+v.cap[1];
            for (const auto& contact:p.neighborhood->vertical) {
                const auto it=index.find(contact.neighbor);
                if (it==index.end()) continue;
                v.covered[contact.side]+=contact.area;
                v.support+=contact.area;
                v.vertical.push_back({it->second,contact.area,1-contact.side});
            }
        }
        double boundary=0, ground=0;
        for (const auto& area:p.neighborhood->boundary) {
            const double size=PieceFaceArea(p,area);
            // 接地: 下向き（法線が下から 45° 以内）の外面は地面に支えられている。
            if (s.grounded && PieceFaceNormalY(p,area)<-.7071*size) ground+=size;
            else boundary+=size;
        }
        v.exposed=boundary>0;
        v.grounded=ground>0;
        v.total+=boundary+ground;
        v.support+=ground;
        for (const auto& contact:p.neighborhood->contacts) {
            const double area=PieceFaceArea(p,contact.areaVector);
            if (!std::isfinite(area) || area<=0) {error="共有面積が不正です";return {};}
            v.total+=area;
            const auto it=index.find(contact.neighbor);
            if (it==index.end()) { v.exposed=true; continue; } // Filterで除かれた隣の面を露出面へ。
            const auto& neighbor=c.pieces[it->second];
            if (p.transform!=neighbor.transform) {
                error="個別変換後の接触面の再判定は未対応です。PeelをPiece Transformより前へ接続してください";
                return {};
            }
            if (neighbor.layer!=p.layer || it->second==i) {error="異なる層への隣接情報が不正です";return {};}
            const auto& back=neighbor.neighborhood->contacts;
            if (std::none_of(back.begin(),back.end(),[&](const auto& edge){return edge.neighbor==p.id;})) {
                error="隣接情報が対称ではありません";return {};
            }
            v.neighbors.push_back({it->second,area}); v.support+=area;
        }
        if (!std::isfinite(v.total)) {error="面積が表現できません";return {};}
        // 削除量・並び順に依存しない乱数。量を増やしても同じ順序を延長する。
        uint64_t hash=(uint64_t(p.id)<<32)^s.seed^uint64_t(c.producer);
        hash+=0x9e3779b97f4a7c15ull;hash=(hash^(hash>>30))*0xbf58476d1ce4e5b9ull;
        hash=(hash^(hash>>27))*0x94d049bb133111ebull;hash^=hash>>31;
        v.noise=(double(hash>>11)*0x1.0p-53-.5)*s.peelNoise;
    }

    // 側面の後退: 周りの地面が下がるにつれて上から順に崖の面が地表に出て、出た時から横へ削られる。高い所ほど長く
    // 削られるので、元の側面から横へ削れる深さを「後退 × 高さ（0 が底、1 が頂）」までにする。深さは片の重心から、
    // 元の外面のうち横を向いた面（法線が水平から 45° 以内）までの距離。上ほど細くなり、芯は削り切らない。
    // 上限の深さはばらつき（±半分）で片ごとに揺らす。
    if (s.peelRetreat>0) {
        const auto apply=[](const Piece& p,const Vec3& v) {
            const auto& m=p.transform;
            return std::array<double,3>{m[0]*v.x+m[1]*v.y+m[2]*v.z+m[3],m[4]*v.x+m[5]*v.y+m[6]*v.z+m[7],
                                        m[8]*v.x+m[9]*v.y+m[10]*v.z+m[11]};
        };
        std::vector<std::array<double,3>> sides, centers(n);
        double low=std::numeric_limits<double>::infinity(), high=-low;
        for (size_t i=0;i<n;++i) {
            const auto& p=c.pieces[i];
            centers[i]=apply(p,p.centroid);
            low=std::min(low,centers[i][1]);high=std::max(high,centers[i][1]);
            if (!p.mesh || !p.faceOrigins || p.faceOrigins->size()!=p.mesh->triangles.size()) continue;
            for (size_t k=0;k<p.mesh->triangles.size();++k) {
                if (!(*p.faceOrigins)[k]) continue;
                const auto& t=p.mesh->triangles[k];
                const auto a=apply(p,p.mesh->positions[t[0]]), b=apply(p,p.mesh->positions[t[1]]),
                           d=apply(p,p.mesh->positions[t[2]]);
                const std::array<double,3> u{b[0]-a[0],b[1]-a[1],b[2]-a[2]}, w{d[0]-a[0],d[1]-a[1],d[2]-a[2]};
                const std::array<double,3> normal{u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]};
                const double length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
                if (length>0 && std::abs(normal[1])<.7071*length)
                    sides.push_back({(a[0]+b[0]+d[0])/3,(a[1]+b[1]+d[1])/3,(a[2]+b[2]+d[2])/3});
            }
        }
        if (!sides.empty())
            for (size_t i=0;i<n;++i) {
                if (stop.stop_requested()) {error="評価をキャンセルしました";return {};}
                double nearest=std::numeric_limits<double>::infinity();
                for (const auto& q:sides) {
                    const double x=q[0]-centers[i][0], y=q[1]-centers[i][1], z=q[2]-centers[i][2];
                    nearest=std::min(nearest,x*x+y*y+z*z);
                }
                const double height=high>low ? (centers[i][1]-low)/(high-low) : 0;
                // 底の後退: 底でも後退 × 底の後退まで削れる（底の外周の角を残さない）。
                const double reach=s.peelRetreat*(s.peelRetreatBase+(1-s.peelRetreatBase)*height);
                state[i].tooDeep=std::sqrt(nearest)>reach*(1+state[i].noise);
            }
    }

    // 各連結部分の、露出面からグラフ距離が最も遠い片を1つ保護する。
    // 同距離なら大きい片、さらに同じなら小さいIDを選ぶ。保護は量とは独立。
    if (s.protectCore) {
        std::vector<bool> visited(n);
        for (size_t start=0;start<n;++start) if (!visited[start]) {
            std::vector<size_t> component{start};visited[start]=true;
            for (size_t k=0;k<component.size();++k)
                for (auto [other,area]:state[component[k]].neighbors) {
                    (void)area;
                    if (!visited[other]) {visited[other]=true;component.push_back(other);}
                }
            std::vector<int> depth(n,-1);std::queue<size_t> pending;
            for (auto i:component) if (state[i].exposed) {depth[i]=0;pending.push(i);}
            while (!pending.empty()) {
                const auto i=pending.front();pending.pop();
                for (auto [other,area]:state[i].neighbors) {
                    (void)area;
                    if (depth[other]<0) {depth[other]=depth[i]+1;pending.push(other);}
                }
            }
            size_t core=start;
            for (auto i:component) {
                const auto& p=c.pieces[i];const auto& best=c.pieces[core];
                if (depth[i]>depth[core] || (depth[i]==depth[core] &&
                    (p.volume>best.volume || (p.volume==best.volume && p.id<best.id)))) core=i;
            }
            state[core].protectedCore=true;
        }
    }

    // 上下両面を覆われた片は、外側の支持が欠けるまで候補にしない。
    const auto candidateAvailable=[&](size_t i) {
        const auto& v=state[i];
        const bool sandwiched=v.cap[0]>0 && v.cap[1]>0 &&
            v.covered[0]>=v.cap[0]*(1-1e-6) && v.covered[1]>=v.cap[1]*(1-1e-6);
        return weights[i]>0 && v.exposed && !v.removed && !v.protectedCore && !v.tooDeep && v.total>0 && !sandwiched;
    };
    const size_t eligible=std::count_if(weights.begin(),weights.end(),[](float w){return w>=0;});
    const size_t budget=size_t(std::floor(double(eligible)*s.fraction+1e-8));
    // 全層で1片ずつ選ぶ。進行の変更は同じ削除順の先頭からの長さだけを変える。
    for (size_t step=0;step<budget;++step) {
        if (stop.stop_requested()) {error="評価をキャンセルしました";return {};}
        size_t best=n;double score=std::numeric_limits<double>::infinity();
        for (size_t i=0;i<n;++i) {
            if (!candidateAvailable(i)) continue;
            const auto& v=state[i];
            const double candidate=v.support/v.total+v.noise+
                (largest>0 ? s.peelSize*c.pieces[i].volume/largest : 0);
            if (best==n || candidate<score-1e-12 ||
                (std::abs(candidate-score)<=1e-12 && c.pieces[i].id<c.pieces[best].id)) {
                best=i;score=candidate;
            }
        }
        if (best==n) break;
        state[best].removed=true;
        out.ids.push_back(c.pieces[best].id);
        for (auto [other,area]:state[best].neighbors) {
            state[other].support=std::max(0.,state[other].support-area);
            state[other].exposed=true;
        }
        for (auto [other,area,side]:state[best].vertical) {
            state[other].support=std::max(0.,state[other].support-area);
            state[other].covered[side]=std::max(0.,state[other].covered[side]-area);
        }
    }
    // 接地: 片は地面か、支えられた片の上に載っているときだけ支えられる（下向きの接触面。法線が下から 60° 以内）。
    // 急な節理の側面で横に接するだけでは支えにならず、支えを失った片は落ちる（一緒に選ぶ）。
    // 地面に接した片が 1 つも無い形（下向きの面が無い）では落とさない。
    if (s.grounded && std::any_of(state.begin(),state.end(),[](const State& v){return v.grounded;})) {
        // 片 i が片 other の上に載る面の、水平に投影した面積（i から見た接触面が下を向くときだけ。0 なら載っていない）。
        const auto bearing=[&](size_t i,uint32_t otherId) {
            const auto& p=c.pieces[i];
            for (const auto& contact:p.neighborhood->contacts)
                if (contact.neighbor==otherId) {
                    const double area=PieceFaceArea(p,contact.areaVector);
                    const double down=-PieceFaceNormalY(p,contact.areaVector);
                    return area>0 && down>.5*area ? down : 0.;
                }
            return 0.;
        };
        // 安定: 支えられた片に載る面積の合計が、片の大きさ（体積^(2/3) × 安定 × 0.5）に足りないと転げ落ちる。
        // 狭い面で載った大きな片（頭でっかち）が落ち、上ほど小さく尖る。0 なら少しでも載れば支えられる。
        std::vector<double> carried(n,0), required(n,0);
        for (size_t i=0;i<n;++i) required[i]=s.stability*.5*std::pow(std::max(c.pieces[i].volume,0.),2./3);
        std::vector<bool> held(n,false);
        std::queue<size_t> pending;
        for (size_t i=0;i<n;++i) if (state[i].grounded && !state[i].removed) {held[i]=true;pending.push(i);}
        while (!pending.empty()) {
            const auto below=pending.front();pending.pop();
            for (auto [other,area]:state[below].neighbors) {
                (void)area;
                if (held[other] || state[other].removed) continue;
                const double rest=bearing(other,c.pieces[below].id);
                if (rest<=0) continue;
                carried[other]+=rest;
                if (carried[other]>=required[other]*(1-1e-9)) {held[other]=true;pending.push(other);}
            }
        }
        for (size_t i=0;i<n;++i) if (!held[i] && !state[i].removed && weights[i]>=0) {
            state[i].removed=true;
            out.ids.push_back(c.pieces[i].id);
        }
    }
    for (size_t i=0;i<n;++i) if (candidateAvailable(i)) out.frontier.push_back(c.pieces[i].id);
    if (s.invert) {
        out.ids.clear();out.frontier.clear();
        for (size_t i=0;i<n;++i) if (weights[i]>=0 && !state[i].removed) out.ids.push_back(c.pieces[i].id);
    }
    return out;
}
} // namespace rock::geometry
