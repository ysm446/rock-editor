# 岩の種類ごとのレシピ（LLM・手書き向け）

LLM に岩を作らせるときの出発点。どれも手で書きやすい表記（ピン ID・位置を省き、リンクをノード ID とピン名で書く）で書いてある。書き方は [LLM による岩グラフの作成](../../docs/reference/ai-authoring.md)、手順は [skill](../../.claude/skills/rock-graph/SKILL.md)。

アプリでこのフォルダをルートとして開くか、`rock_cli eval` / `python tools/rock_shot.py` で確かめる。形だけのグラフ（マテリアルは付けていない）。どれも `rock_cli eval` で塊 1 つ・空洞 0・閉じたメッシュになることを確かめてある。

| ファイル | 岩 | 組み方の要点 | 評価 |
| --- | --- | --- | --- |
| `schist.rockgraph` | 片理（薄く剥がれる変成岩） | Layered Boxes の薄板を急傾斜に積み、同じ向きに伸ばした Voronoi で割って Peel で縁を欠く。片理と同じ向きの Parallel Planes で浅い細溝、直交する面で節理。Volume Close で繋ぎ直してから Edge Wear | 約 0.7 秒 |
| `blocky.rockgraph` | 塊状・角張った岩 | Random Boxes の塊を Plane Cuts（3 系統の主方向）で切って節理面を作り、Volume Clip の接地で地面へ据える。小面のノイズと Edge Wear | 約 0.1 秒 |
| `platy-joints.rockgraph` | 板状節理 | 不規則な塊を緩く傾いた Parallel Planes で浅く彫り、直交する縦の節理を足す | 約 0.3 秒 |
| `rounded-boulder.rockgraph` | 丸い転石 | 塊に大きな面を少し作ってから Volume Smooth で丸め、セル状のノイズでくぼみ | 約 0.1 秒 |
| `layered-ledges.rockgraph` | 層理の段 | Volume Terrace で水平に近い層の段を作る | 約 0.1 秒 |
| `columnar.rockgraph` | 柱状節理（玄武岩） | 縦に 16 倍伸ばした Voronoi で多角柱に割り、外周の片を除き、柱を細らせて隙間を作り、一部の柱を下げる。柱は別々の塊のまま | 約 0.5 秒 |
| `slate.rockgraph` | スレート・頁岩 | 水平に近い薄い Layered Boxes を Peel で欠き、Volume Close で繋ぐ | 約 0.5 秒 |
| `talus-fragment.rockgraph` | 崖錐の角張った岩片 | 箱を大きな平面で強く切り、局所の欠けを多数 | 約 0.1 秒 |
| `honeycomb.rockgraph` | 蜂の巣状の風化（タフォニ）・多孔質 | 丸めた塊に Volume Noise の「くぼみ」を粗・細の 2 段で掛ける | 約 0.2 秒 |
| `conglomerate.rockgraph` | 礫岩 | 丸めた塊に Volume Scatter（楕円体・和）で大小の礫を半分ほど埋める | 約 0.6 秒 |
| `breccia.rockgraph` | 角礫岩 | Volume Scatter（箱・和）で角張った礫を浅く埋める。礫と基質の色の違いが無いと弱い | 約 0.3 秒 |
| `vesicular-basalt.rockgraph` | 多孔質の溶岩 | Volume Scatter（楕円体・差）で気泡の穴を多数抜く | 約 0.2 秒 |
| `granite-sheeting.rockgraph` | 花崗岩のシーティング・剥離 | Volume Crack の割り方「表面に沿う殻」で、外側の板がまだらに剥がれた段を作る。平らな面にボクセルの格子模様が出る | 約 0.3 秒 |
| `mushroom-rock.rockgraph` | きのこ岩（台座岩） | 接地させてから Volume Undercut で底の近くを削り、細い台座にする | 約 0.2 秒 |
| `hoodoo.rockgraph` | フードゥー | 縦長の塊に Volume Undercut の帯を 4 段重ね、Volume Terrace で層の筋 | 約 0.1 秒 |
| `roche-moutonnee.rockgraph` | 羊背岩（氷河の研磨） | 角張った塊の上流側（-X と上）だけを Volume Smooth と Edge Wear の「集中する向き」で丸める。丸めた面の縁に薄いヒレが残る | 約 0.2 秒 |
| `obsidian.rockgraph` | 黒曜石・フリント（貝殻状断口） | 楕円体を Plane Cuts の「曲がり」で切り、えぐれた剥離痕を重ねる。ガラス質の見た目は素材で | 約 0.2 秒 |
| `river-pebble.rockgraph` | 河原の丸石 | 平たい楕円体を弱いノイズで歪ませ、Marching Tetrahedra でなめらかに | 約 0.1 秒 |

作れる岩・作れない岩の一覧と足りないノードは [岩の種類の網羅](../../docs/reference/rock-catalog.md)。

共通の注意:

- Random Boxes・Base Shape は原点中心にできる。Volume Clip の `"mode": "ground"` で地面へ据える（`embed` が埋める割合）。`world` のまま高さ 0 で切ると半分が消えて平たくなる。
- 割れ目で岩が複数の塊に分かれると、そのまま最後まで分かれて残る（Plane Cuts などは小片だけを捨てる）。1 つの塊にしたいなら、先に Volume Close で繋ぐ（`schist` / `platy-joints`）。
- 岩には Volume to Mesh の `dualContouring` を使う（面と稜線が立つ）。
