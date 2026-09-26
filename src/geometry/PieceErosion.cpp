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

PieceSelection PeelPieces(const PieceCollection& c, const PieceSelectSettings& s,
                          const std::vector<float>& weights, std::string& error, std::stop_token stop) {
    error.clear();
    PieceSelection out{c.producer,c.generation,c.fingerprint,{}};
    if (stop.stop_requested()) { error="評価をキャンセルしました"; return {}; }
    if (c.pieces.empty()) return out;
    if (!c.adjacencyComplete) {
        error="Peelには初回のVoronoi分割による隣接情報が必要です。再分割をまたぐ隣接面は未対応です";
        return {};
    }
    if (weights.size()!=c.pieces.size() || !std::isfinite(s.peelNoise) || s.peelNoise<0 || s.peelNoise>1) {
        error="侵食のばらつきは0〜1にしてください"; return {};
    }
    struct State {
        std::vector<std::pair<size_t,double>> neighbors;
        struct Vertical { size_t other; double area; int otherSide; };
        std::vector<Vertical> vertical;
        std::array<double,2> cap{}, covered{};
        double total=0, support=0, noise=0;
        bool exposed=false, removed=false, protectedCore=false;
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
        double boundary=0;
        for (const auto& area:p.neighborhood->boundary) boundary+=PieceFaceArea(p,area);
        v.exposed=boundary>0;
        v.total+=boundary;
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
        return weights[i]>0 && v.exposed && !v.removed && !v.protectedCore && v.total>0 && !sandwiched;
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
            const double candidate=v.support/v.total+v.noise;
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
    for (size_t i=0;i<n;++i) if (candidateAvailable(i)) out.frontier.push_back(c.pieces[i].id);
    if (s.invert) {
        out.ids.clear();out.frontier.clear();
        for (size_t i=0;i<n;++i) if (weights[i]>=0 && !state[i].removed) out.ids.push_back(c.pieces[i].id);
    }
    return out;
}
} // namespace rock::geometry
