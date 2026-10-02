# 岩の種類ごとのレシピ（LLM・手書き向け）

LLM に岩を作らせるときの出発点。どれも手で書きやすい表記（ピン ID・位置を省き、リンクをノード ID とピン名で書く）で書いてある。書き方は [LLM による岩グラフの作成](../../docs/reference/ai-authoring.md)、手順は [skill](../../.agents/skills/rock-graph/SKILL.md)。

アプリの「ファイル」→「テンプレートから作成…」（アセット欄の右クリックにもある）から、未保存の文書として開ける（保存したときにファイルを作る）。一覧は `templates.json`（日本語名・分類・状態・説明）。または、このフォルダをルートとして開くか、`rock_cli eval` / `python tools/rock_shot.py` で確かめる。形だけのグラフ（マテリアルは付けていない。`gneiss` / `marble` だけは Surface の定数色で模様を塗る）。どれも `rock_cli eval` で塊 1 つ・空洞 0・閉じたメッシュになることを確かめてある。

| ファイル | 岩 | 組み方の要点 | 評価 |
| --- | --- | --- | --- |
| `schist.rockgraph` | 片理（薄く剥がれる変成岩） | Layered Boxes の薄板を急傾斜に積み、同じ向きに伸ばした Voronoi で割って Peel で縁を欠く。片理と同じ向きの Parallel Planes で浅い細溝、直交する面で節理。Volume Close で繋ぎ直してから Edge Wear | 約 0.7 秒 |
| `blocky.rockgraph` | 塊状・角張った岩 | Random Boxes の塊を Plane Cuts（3 系統の主方向）で切って節理面を作り、Volume Clip の接地で地面へ据える。小面のノイズと Edge Wear | 約 0.1 秒 |
| `platy-joints.rockgraph` | 板状節理 | 箱の岩体を扁平に伸ばした点の Voronoi で不規則な板に割り、接地の Peel（大きさの効き・安定）で外周から抜き取る。To Volume でまとめ、Decimate で三角形を減らす | 約 1 秒 |
| `rounded-boulder.rockgraph` | 丸い転石 | 塊に大きな面を少し作ってから Volume Smooth で丸め、セル状のノイズでくぼみ | 約 0.1 秒 |
| `layered-ledges.rockgraph` | 層理の段 | Volume Terrace で水平に近い層の段を作る | 約 0.1 秒 |
| `columnar.rockgraph` | 柱状節理（玄武岩） | 縦に 16 倍伸ばした Voronoi で多角柱に割り、外周の片を除き、柱を細らせて隙間を作り、一部の柱を下げる。柱は別々の塊のまま | 約 0.5 秒 |
| `slate.rockgraph` | スレート・頁岩 | 水平に近い薄い Layered Boxes を Peel で欠き、Volume Close で繋ぐ | 約 0.5 秒 |
| `talus-fragment.rockgraph` | 崖錐の角張った岩片 | 箱を大きな平面で強く切り、局所の欠けを多数 | 約 0.1 秒 |
| `honeycomb.rockgraph` | 蜂の巣状の風化（タフォニ）・多孔質 | 丸めた塊に Volume Noise の「くぼみ」を粗・細の 2 段で掛ける | 約 0.2 秒 |
| `spheroidal-weathering.rockgraph` | 玉ねぎ状風化 | 表面を揺らした楕円体（内部に面の無い Base Shape）に「表面に沿う殻」を薄く 3 枚。外側ほど剥がれる | 約 0.5 秒 |
| `conglomerate.rockgraph` | 礫岩 | 接地した塊に Volume Scatter（楕円体・和）で礫を埋め、Volume Diff Mask（足した所）で礫だけ別の色に | 約 5 秒（UV 展開を含む） |
| `breccia.rockgraph` | 角礫岩 | Volume Scatter（箱・和）で角礫を浅く埋め、Volume Diff Mask で礫だけ別の色に | 約 5 秒（UV 展開を含む） |
| `vesicular-basalt.rockgraph` | 多孔質の溶岩 | Volume Scatter（楕円体・差）で気泡の穴を多数抜く | 約 0.2 秒 |
| `granite-sheeting.rockgraph` | 花崗岩のシーティング・剥離 | Volume Crack の割り方「表面に沿う殻」で、外側の板がまだらに剥がれた段を作る。Edge Wear は殻より前に置く（後ろに置くと剥がれた跡に細かい縞） | 約 0.3 秒 |
| `granite-buttress.rockgraph` | 花崗岩の岩峰（岩稜） | Base Shape の凸岩峰（傾いた板、稜線の頂）で大きな輪郭を先に作り、主な節理面に吸着した Voronoi で割る。Peel は進行 0.1・稜の効き 1 で稜と頂だけを欠く。Piece Transform のわずかなずれで節理の線、To Volume でまとめる | 約 0.2 秒 |
| `mushroom-rock.rockgraph` | きのこ岩（台座岩） | 接地させてから Volume Undercut で底の近くを削り、細い台座にする | 約 0.2 秒 |
| `hoodoo.rockgraph` | フードゥー | 縦長の塊に Volume Undercut の帯を 4 段重ね、Volume Terrace で層の筋 | 約 0.1 秒 |
| `roche-moutonnee.rockgraph` | 羊背岩（氷河の研磨） | 角張った塊の上流側（-X と上）だけを Volume Smooth と Edge Wear の「集中する向き」で丸める | 約 0.2 秒 |
| `obsidian.rockgraph` | 黒曜石・フリント（貝殻状断口） | 楕円体を Plane Cuts の「曲がり」で切り、えぐれた剥離痕を重ねる。ガラス質の見た目は素材で | 約 0.2 秒 |
| `gneiss.rockgraph` | 片麻岩（縞） | 同じ Parallel Planes で浅い割れ目と Structure Mask の縞を作り、暗い下地に明るい層を塗る（Surface の定数色） | 約 9 秒（UV 展開を含む） |
| `marble.rockgraph` | 大理石（脈） | 丸めた塊の白い下地に、Structure Mask の脈で暗い線を塗る。網目の感じが少し残る | 約 4 秒（UV 展開を含む） |
| `river-pebble.rockgraph` | 河原の丸石 | 平たい楕円体を弱いノイズで歪ませ、Marching Tetrahedra でなめらかに | 約 0.1 秒 |
| `pillow-lava.rockgraph` | 枕状溶岩 | 低い台に Volume Scatter の楕円体を「寝かせる割合」で扁平にして積む | 約 0.3 秒 |
| `wave-cut-notch.rockgraph` | 波食ノッチ | 接地してから Volume Undercut（帯の基準 world）で水面の高さに帯を 1 本 | 約 0.3 秒 |
| `wind-erosion.rockgraph` | 風食 | Volume Erode（さらされた面）で風上の面を後退させ、足元を world の Undercut で削る | 約 0.6 秒 |
| `limestone-rills.rockgraph` | 石灰岩の溶食（縦溝） | Volume Erode（流下）で雨水の筋に沿って溝を彫る | 約 1 秒 |
| `rust-streaks.rockgraph` | 鉄錆・汚れの流れた筋 | Flow Mask（流れの量）× Noise Mask で錆色を塗る（Surface の定数色） | 約 5 秒（UV 展開を含む） |

使い方の例（チュートリアル）。ノードの組み方を見せる例で、ノードの note に手順を書いてある。テンプレートの一覧では「使い方の例」の分類に出る。

| ファイル | 内容 | 組み方の要点 | 評価 |
| --- | --- | --- | --- |
| `soil-filled-gaps.rockgraph` | 岩の隙間を土で埋める | 割ってずらしたブロックを接地させ（ここが Before）、Volume Close（occlusion）で奥まった隙間を埋める。Volume Diff Mask（足した所）で詰めた土だけを別の色で塗る | 約 2 分（UV 展開を含む） |

種類ごとのスクリーンショットと改善の履歴は [岩の研究ページ](../../docs/research/rocks.md)。作れる岩・作れない岩の一覧と足りないノードは [岩の種類の網羅](../../docs/reference/rock-catalog.md)。

共通の注意:

- Random Boxes・Base Shape は原点中心にできる。Volume Clip の `"mode": "ground"` で地面へ据える（`embed` が埋める割合）。`world` のまま高さ 0 で切ると半分が消えて平たくなる。
- 割れ目で岩が複数の塊に分かれると、そのまま最後まで分かれて残る（Plane Cuts などは小片だけを捨てる）。1 つの塊にしたいなら、先に Volume Close で繋ぐ（`schist`）。
- 岩には Volume to Mesh の `dualContouring` を使う（面と稜線が立つ）。
- 節理で割れた形は、ひびを彫るより「岩体をブロックに割って外側から抜き取る」方が構造から割れて見える（`granite-buttress`）。不規則な割れ方は節理の向きに伸ばした Voronoi（Points）、規則的な割れ方（層理・板状節理）は Parallel Planes を連結して Voronoi Fracture の Planes 入力で割る。地面から生えた岩は Peel の接地で欠き、Piece Transform のばらつきでずらす。
- 殻の深さ・割れ目の深さ・Undercut などは距離場の内部の値を使う。To Volume は重なった箱・メッシュの内部を表面から測り直す（2026-10-01）。剥がれた跡など内部の等値面が表に出る形では、Edge Wear を先に置く。
- Parallel Planes の割れ目は既定で岩の端から端まで続き、並ぶと石積みの格子に見える。Volume Crack の `extent`（割れ目の長さ）・`coverage`（割合）・`stagger`（段違い）で途中で止まる節理にする。
