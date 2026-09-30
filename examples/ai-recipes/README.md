# 岩の種類ごとのレシピ（LLM・手書き向け）

LLM に岩を作らせるときの出発点。どれも手で書きやすい表記（ピン ID・位置を省き、リンクをノード ID とピン名で書く）で書いてある。書き方は [LLM による岩グラフの作成](../../docs/reference/ai-authoring.md)、手順は [skill](../../.claude/skills/rock-graph/SKILL.md)。

アプリでこのフォルダをルートとして開くか、`rock_cli eval` / `python tools/rock_shot.py` で確かめる。形だけのグラフ（マテリアルは付けていない）。どれも `rock_cli eval` で塊 1 つ・空洞 0・閉じたメッシュになることを確かめてある。

| ファイル | 岩 | 組み方の要点 | 評価 |
| --- | --- | --- | --- |
| `schist.rockgraph` | 片理（薄く剥がれる変成岩） | Layered Boxes の薄板を急傾斜に積み、同じ向きに伸ばした Voronoi で割って Peel で縁を欠く。片理と同じ向きの Parallel Planes で浅い細溝、直交する面で節理。Volume Close で繋ぎ直してから Edge Wear | 約 0.7 秒 |
| `blocky.rockgraph` | 塊状・角張った岩 | Random Boxes の塊を持ち上げ、Plane Cuts（3 系統の主方向）で節理面を作る。小面のノイズと Edge Wear | 約 0.1 秒 |
| `platy-joints.rockgraph` | 板状節理 | 不規則な塊を緩く傾いた Parallel Planes で浅く彫り、直交する縦の節理を足す | 約 0.3 秒 |
| `rounded-boulder.rockgraph` | 丸い転石 | 塊に大きな面を少し作ってから Volume Smooth で丸め、セル状のノイズでくぼみ | 約 0.1 秒 |
| `layered-ledges.rockgraph` | 層理の段 | Volume Terrace で水平に近い層の段を作る | 約 0.1 秒 |

共通の注意:

- Random Boxes・Base Shape は原点中心にできる。Volume Clip（高さ 0）で接地面を切る前に Volume Transform で持ち上げる。上げないと半分が消えて平たくなる。
- 割れ目で岩が複数の塊に分かれると、そのまま最後まで分かれて残る（Plane Cuts などは小片だけを捨てる）。1 つの塊にしたいなら、先に Volume Close で繋ぐ（`schist` / `platy-joints`）。
- 岩には Volume to Mesh の `dualContouring` を使う（面と稜線が立つ）。
