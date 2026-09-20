#pragma once


#include <cstdint>
#include <vector>

// パス（Path ノードの中身）。道路生成へ渡す、向き付きの実寸カーブ。
//
// x/y/z の 3 次元座標（m）を保存する。座標を 0〜1 やグリッド内へ丸めない。
//
// - 幅 / フェザー / 強さは点ごと。エッジ上では両端から補間する。
// - エッジは from → to の向きを持つ（川や氷河の流れの向き。道路では無視してよい）。
// - 分岐は許す（支流の合流、道路の交差）。1 つの点に何本のエッジが付いてもよい。
//
// UI / D3D12 には依存しない。編集操作は純粋な関数で、ビューポート側はこれを呼ぶだけ。
namespace tg::graph {

using PathElementId = int;

// 停止線。実寸 Path の点に持ち、Road がその点に最も近い実距離を控え、Lane Marking が引く。
// 進行方向の車線に引くか、対向車線に引くか（両方も可）。
enum class PathStopLine : uint8_t {
    None = 0,
    Forward = 1,
    Backward = 2,
    Both = 3,
};

struct PathPoint {
    PathElementId id = 0;
    float x = 0.5f;  // X（m）。面上のパスでは横位置
    float z = 0.5f;  // Z（m）。面上のパスでは始点からの実距離
    float widthMeters = 24.0f;    // パスの全幅（m）
    float featherMeters = 12.0f;  // 幅の外側を 0 へ落とす幅（m）
    float intensity = 1.0f;       // マスクの強さ（0〜1）
    // Y 座標（m）。面上のパスでは面からの高さ。
    float y = 0.0f;
    // 停止線（実寸 Path のみ）。曲線は点を通らないので、道路上ではこの点に最も近い位置になる。
    PathStopLine stopLine = PathStopLine::None;
};

// 鎖（分岐から分岐までのエッジの並び）をどう描くか。エッジに持ち、鎖を選んだときに
// まとめて読み書きする（鎖は導出したものなので、性質の置き場はエッジ）。
//
// **曲線は点を通らない。** 置いた点は制御点で、折れ線（ガイド）の内側を回る。
// Catmull-Rom のように点を貫く形ではない。
enum class PathCurve : uint32_t {
    Line = 0,       // 折れ線そのもの
    Quadratic = 1,  // 2 次ベジェの連結。角ごとに丸め、両端の点だけ通る（C1）
    Cubic = 2,      // 3 次 B スプライン。さらに滑らか（C2）だが折れ線からより離れる
    // クロソイド → 円弧 → クロソイド。角ごとに曲率が 0 から連続的に立ち上がる緩和曲線
    // （道路 / 鉄道の線形）。2 次と同じく両端の点だけ通る。
    Clothoid = 3,
};

struct PathEdge {
    PathElementId id = 0;
    PathElementId from = 0;
    PathElementId to = 0;
    PathCurve curve = PathCurve::Line;
    // どれだけ角を取るか（0〜1）。0 で折れ線のまま、1 で最大（2 次なら線分の中点まで）。
    float rounding = 1.0f;
    // クロソイドのとき、交角のうちクロソイド 2 本が受け持つ割合（0〜1）。
    // 0 で純粋な円弧、1 で円弧なし（緩和曲線だけ）。
    float clothoidRatio = 0.5f;

    // --- 幅の上書き ---
    // 真なら、このエッジの上では点の幅 / フェザー / 強さを補間せず、ここの値で一定にする。
    // 鎖を選んでまとめて決める（点ごとに入れて回るより速い）。点の値は捨てないので、
    // 切れば点の値に戻る。高さのずれは対象外（点ごとのまま）。
    bool overrideValues = false;
    float widthMeters = 24.0f;
    float featherMeters = 12.0f;
    float intensity = 1.0f;
};

// 鎖。エッジが 2 本だけ付いた点を通り、それ以外の点（端 / 分岐 / 交差）で止まる。
// 「A の途中に B が乗る」という形は無く、支流を繋いだ時点で本流の鎖はそこで割れる。
// 導出するものなので保存しない。
struct PathStrand {
    std::vector<PathElementId> points;  // 並んだ点（両端を含む）。閉じた輪なら末尾 = 先頭
    std::vector<PathElementId> edges;   // points[i] と points[i+1] を結ぶエッジ
    bool closed = false;                // 分岐の無い輪
};

// 曲線を割った標本。座標系は PathSettings に従う。高さと幅も補間して道路生成へ渡せる。
struct PathCurveSample {
    float x = 0.0f;
    float z = 0.0f;
    float widthMeters = 0.0f;
    float featherMeters = 0.0f;
    float intensity = 1.0f;
    float y = 0.0f;
};

// 縦断ポイント。線形の正規化位置 u に置き、その位置の高さを offset だけずらして
// 前後を縦断曲線長 vcl の放物線でつなぐ（graph/RoadProfile.h）。
struct PathVerticalPoint {
    PathElementId id = 0;
    float u = 0.0f;
    float vclMeters = 50.0f;
    float offsetMeters = 0.0f;
};

// バンク角ポイント。自動（設計速度から曲率に応じて決める）か手動（角度を直接指定）。
// 手動でないポイントの設計速度も、自動角の速度補間に使う。
struct PathBankPoint {
    PathElementId id = 0;
    float u = 0.0f;
    float designSpeedKmh = 40.0f;
    bool manual = false;
    float angleDegrees = 0.0f;  // 正で Left 側が上がる
};

struct PathSettings {
    // true: Surface 入力の道路の面の座標。x = 横位置（m、正が Left）、z = 始点からの実距離（m）、
    // y = 面からの高さ。道路を変形しても面に貼り付いたまま追従する。偽ならワールド座標（m）。
    bool surfaceSpace = false;
    std::vector<PathPoint> points;
    std::vector<PathEdge> edges;
    // --- 道路線形（面上のパスでは使わない） ---
    std::vector<PathVerticalPoint> verticalPoints;
    std::vector<PathBankPoint> bankPoints;
    // バンク角を道路へ反映するか。偽なら手動ポイントがあっても水平のまま。
    bool bankEnabled = false;
    float designSpeedKmh = 40.0f;      // ポイントの無い所の設計速度（km/h）
    float frictionCoefficient = 0.15f;  // 自動バンクの横方向摩擦係数
    bool smoothBank = false;            // 距離方向のガウス平滑化
    float bankSmoothMeters = 20.0f;
    // 新しく置く点の初期値。
    float defaultWidthMeters = 24.0f;
    float defaultFeatherMeters = 12.0f;
    float defaultIntensity = 1.0f;
    // 点とエッジの ID。パスの中で一意ならよい（グラフの ID 空間とは別）。
    PathElementId nextId = 1;

    const PathPoint* FindPoint(PathElementId id) const;
    PathPoint* FindPoint(PathElementId id);
    const PathEdge* FindEdge(PathElementId id) const;
    // a と b を繋ぐエッジ（向きは問わない）。無ければ nullptr。
    const PathEdge* FindEdgeBetween(PathElementId a, PathElementId b) const;
    // 点に付いているエッジの数。
    size_t EdgeCount(PathElementId pointId) const;
};

// --- 編集操作 -------------------------------------------------------------
// どれも成功したら真を返す。失敗しても中身は変えない。

// 点を置く。connectFrom が有効な点なら、そこから新しい点へエッジを張る。
PathElementId AddPathPoint(PathSettings& path, float u, float v, PathElementId connectFrom);
// 2 点をエッジで繋ぐ（from → to）。既に繋がっていれば何もしない（真を返す）。
bool ConnectPathPoints(PathSettings& path, PathElementId from, PathElementId to);
// エッジの途中（t: 0〜1）に点を挿入し、エッジを 2 本に割る。
// 幅 / フェザー / 強さは両端から補間する。返り値は新しい点（失敗なら 0）。
PathElementId InsertPathPointOnEdge(PathSettings& path, PathElementId edgeId, float t);
// 点を消す。鎖の途中の点（エッジがちょうど 2 本）なら、両隣を 1 本のエッジで繋ぎ直して
// から消すので線は切れない（曲線の種類 / 丸め / 向きは残ったエッジのものが続く）。
// 端や分岐（エッジが 1 本以下、または 3 本以上）は、付いているエッジも一緒に消える。
bool DeletePathPoint(PathSettings& path, PathElementId pointId);
// エッジを消す。点は残る。
bool DeletePathEdge(PathSettings& path, PathElementId edgeId);
// source を target へ合体させる。source のエッジは target に付け替わり、
// 位置と幅は target のものが残る（持っていった側が相手に合わせに行く）。
bool MergePathPoints(PathSettings& path, PathElementId source, PathElementId target);
// 点を分離する。エッジの本数ぶんに分け、各エッジの端に新しい点を作って
// そのエッジの向きに沿って少し戻した位置（offsetUv、m）へ置く。
// 出ていくエッジ（下流）は元の点に残す。新しく作った点を outCreated に返す。
// エッジが 1 本以下なら何もしない。
bool SplitPathPoint(PathSettings& path, PathElementId pointId, float offsetUv,
                    std::vector<PathElementId>* outCreated);
// エッジの片端を点から切り離し、新しい点として offsetUv だけ戻した位置に置く。
// pointId はそのエッジの from か to。返り値は新しい点（失敗なら 0）。
PathElementId DetachPathEdgeEnd(PathSettings& path, PathElementId edgeId, PathElementId pointId,
                                float offsetUv);
// エッジの向きを反転する。
bool ReversePathEdge(PathSettings& path, PathElementId edgeId);
// 点に付いているエッジを全部反転する。
bool ReversePathEdgesAt(PathSettings& path, PathElementId pointId);

// --- エッジの制御点 ---------------------------------------------------------
// エッジの制御点列（from、to）。幅を上書きするエッジなら、幅 / フェザー / 強さは
// エッジの値。端点が無ければ空。
std::vector<PathPoint> PathEdgeControlPoints(const PathSettings& path, const PathEdge& edge);

// --- まとめて動かす / コピーと貼り付け ---------------------------------------
// 点をまとめて動かす。1 つでも動いたら真。
bool MovePathPoints(PathSettings& path, const std::vector<PathElementId>& pointIds, float du,
                    float dv);
// 点の重心（XZ）。1 つも無ければ偽。
bool PathPointsCentroid(const PathSettings& path, const std::vector<PathElementId>& pointIds,
                        float& outU, float& outV);

// 切り出した部分。点と、その間のエッジ。ID はこの中でだけ一意（元のパスの ID のまま）。
struct PathClip {
    std::vector<PathPoint> points;
    std::vector<PathEdge> edges;
};
// 点とエッジから切り出す。エッジは両端の点も連れていき、指定した点どうしを結ぶエッジも拾う。
// 何も無ければ偽。
bool ExtractPathClip(const PathSettings& path, const std::vector<PathElementId>& pointIds,
                     const std::vector<PathElementId>& edgeIds, PathClip& out);
// 貼り付ける。ID を振り直し、XZ を (du, dv) だけずらす。
// 新しい点 / エッジの ID を返す。空なら偽。
bool PastePathClip(PathSettings& path, const PathClip& clip, float du, float dv,
                   std::vector<PathElementId>* outPoints, std::vector<PathElementId>* outEdges);

// --- 鎖と曲線 -------------------------------------------------------------
// 鎖を導出する。すべてのエッジがちょうど 1 つの鎖に入る。
std::vector<PathStrand> BuildPathStrands(const PathSettings& path);
// エッジが属する鎖。無ければ nullptr。
const PathStrand* FindStrandOfEdge(const std::vector<PathStrand>& strands, PathElementId edgeId);
// 鎖の曲線の種類と丸め（先頭のエッジの値）。mixed はエッジごとに値が違うとき真。
PathCurve StrandCurve(const PathSettings& path, const PathStrand& strand, float* outRounding,
                      bool* outMixed);
// 鎖のクロソイド比（先頭のエッジの値）。
float StrandClothoidRatio(const PathSettings& path, const PathStrand& strand);
// 鎖を折れ線（曲線なら細かく割ったもの）にする。samplesPerSpan は制御点の区間ごとの標本数。
// 直線の鎖は制御点そのものを返す。
std::vector<PathCurveSample> SamplePathStrand(const PathSettings& path, const PathStrand& strand,
                                              int samplesPerSpan);
// 鎖のエッジを全部反転する。
bool ReversePathStrand(PathSettings& path, const PathStrand& strand);

}  // namespace tg::graph
