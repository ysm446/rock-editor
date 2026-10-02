---
name: rock-graph
description: rock-editor の岩グラフ（.rockgraph）をノードで組んで岩を作る・直す手順。「〇〇な岩を作って」「この岩グラフを調整して」のように、岩の形をノードグラフとして書く依頼で使う。rock_cli で評価し、tools/rock_shot.py で撮って確かめる。
---

# 岩グラフを組む

この手順は Claude Code と Codex で共有する（正本はこのファイル。`.claude/skills/rock-graph/SKILL.md` はここを指す入口）。

岩グラフは JSON のノードグラフ。手で書きやすい表記で書き、`rock_cli` で数値を、`tools/rock_shot.py` で見た目を確かめながら直す。GPU アプリを何度も手で起動しない。

仕様の詳細は `docs/reference/ai-authoring.md`。ノードごとの範囲・単位・意味は `rock_cli catalog` が正本（この文書に書き写さない）。

## 0. 準備

```bash
cmake --build build --config Release --target rock_cli rock_editor   # 無ければ・古ければ
build/bin/Release/rock_cli.exe catalog > "$TEMP/catalog.json"       # ノードの一覧（大きいので必要な種類だけ読む）
```

catalog の各ノード: `kind`（保存名）、`inputs` / `outputs`（ピンの番号・名前・型）、`defaults`、`purpose` / `notes`（何をするか・落とし穴）、`params`（`path`・`type`・`min` / `max`・`unit`・`meaning`・`options`・`outOfRange`）。`internal: true` の項目は書かない。

## 1. 出発点を選ぶ

`examples/ai-recipes/` のレシピ（片理・塊状・板状節理・柱状節理・スレート・崖錐の岩片・河原の丸石・蜂の巣状の風化・礫岩・角礫岩・多孔質の溶岩・花崗岩のシーティング・花崗岩の岩峰・きのこ岩・フードゥー・羊背岩・黒曜石・片麻岩・大理石・丸い転石・層理の段・玉ねぎ状風化・枕状溶岩・波食ノッチ・風食・石灰岩の溶食・鉄錆の筋）から近いものを写す。作れる岩・作れない岩と、その理由は `docs/reference/rock-catalog.md`。作れない種類を頼まれたら、近いもので妥協せず、足りないノードをユーザーに伝える。README に組み方の要点がある。ゼロから組むより、レシピの数値を変える方が速く失敗しにくい。

## 2. 書く

置き場所はプロジェクトのルート（`project.reproj` のあるフォルダ。手元は `data/`、例: `data/Rock-Models/<name>.rockgraph`）。`data/` は Git の対象外。

```json
{
  "format": "rock-editor.scene",
  "version": 1,
  "graph": {
    "nodes": [
      {"id": 1, "kind": "randomBoxes", "randomBoxes": {"count": 10, "seed": 7}},
      {"id": 2, "kind": "toVolume", "toVolume": {"resolution": 112}},
      {"id": 3, "kind": "volumeEdgeWear", "volumeEdgeWear": {"amount": 0.04}},
      {"id": 4, "kind": "volumeClip", "volumeClip": {"mode": "ground", "embed": 0.2}},
      {"id": 5, "kind": "volumeToMesh", "volumeToMesh": {"method": "dualContouring"}},
      {"id": 6, "kind": "surface", "layer": {"material": "Materials/rough-rock.rockmat", "mapping": {"method": "triplanar", "repeatMeters": 2}}},
      {"id": 7, "kind": "meshOutput"}
    ],
    "links": [
      {"from": "1", "to": "2"}, {"from": "2", "to": "3"}, {"from": "3", "to": "4"}, {"from": "4", "to": "5"},
      {"from": "5", "to": "7:Geometry"}, {"from": "6", "to": "7:Material"}
    ]
  }
}
```

- ノードは `id` と `kind` だけでよい。設定は変えたい項目だけ書く（残りは既定値）。ピンの ID と位置は書かない。
- リンクは `{"from": "出力側", "to": "入力側"}`。端は `"12"`（0 番のピン）か `"12:Planes"`（ピン名）か `"12:2"`（番号）。
- 数値の列挙は名前で書ける（Piece Select の `"mode": "peel"`）。
- マテリアルはルートからのパスを Surface の `layer.material` に書く（`data/Materials/` の中から選ぶ）。
- 何のために置いたかを `note` に書くと、ユーザーがアプリで読める。

## 3. 確かめる（数値）

```bash
build/bin/Release/rock_cli.exe check <graph>                  # 一瞬。書き間違いを先に潰す
build/bin/Release/rock_cli.exe eval <graph> --node <id>       # 途中のノード（Volume to Mesh など）を評価
```

- `ok: false` なら `diagnostics`（捨てたノード・リンク、知らないキー、範囲外、型違い、無いマテリアル）か `error`（`node` と `kind` つき）を直す。終了コード 0 = 成功、1 = 失敗。
- 形は `meshes[0]` で見る。
  - `size`（m）。岩 1 個なら 1〜4 m くらい。
  - `closed`。
  - `shells.solids` と `shells.largestSolidShare`。塊 1 つ・1.0 が普通。1 未満なら岩が分かれている。
  - `shells.cavities`。閉じた空洞で、三角形の無駄になる。
- 途中の段を順に評価すると、どこで塊が分かれた・消えたかが分かる。
- Displace など GPU が要るノード（`requiresGpu`）の下流は CLI で評価できない。その手前を `--node` で見る。

## 4. 確かめる（見た目）

```bash
python tools/rock_shot.py <graph> <out.png> --views 4 [--node <id>]   # 4 方向を 2×2 の 1 枚に
python tools/rock_shot.py <graph> <out.png> [--yaw <度>] [--pitch <度>] [--ui]
```

評価の完了を待ち、形全体が入るようにカメラを引いて撮る（1 方向 1〜数秒）。正面だけでは裏側の失敗を見落とすので、仕上げの確認は `--views 4` で撮る。撮った PNG を画像として開いて見て、依頼の岩に見えるかを判断する。`--ui` は評価エラーが画面に出る。数値が良くても見た目が依頼と違えば直す。

## 4b. 岩アセットを焼く・山グラフで撒く

```bash
python tools/rock_bake.py <graph.rockgraph> [--node <Rock Asset の id>]   # 付属フォルダ <graph>.rockgraph.bake/ を作る
```

山グラフ（`.mountaingraph`。Heightmap の地形に Rock Scatter で岩を撒く）の Rock ノードは、岩グラフを**焼いた**岩アセットを読む。焼くのは GPU のアプリの仕事だが、`tools/rock_bake.py` が対話せずに焼く（Rock Asset ノードを自動で探し、上流の Material Bake も先に走る。数秒〜数十秒）。焼いた後は `rock_cli eval <山グラフ>` で段ごとの数と推定の被覆率（`rockInstanceSets`）、`rock_shot.py` で見た目を確かめる。段を分けるときは前の段の `Coverage` 出力を `Mask Filter`（`type: levels`, `invert: true`）で反転し、`Mask Combine`（`minimum`）で傾斜のマスクと合わせて次の段の `Mask` につなぐ。例は `examples/mountain/`。

## 5. 研究ページを更新する（レシピを足した・直したとき）

`examples/ai-recipes/` のレシピを足したり直したりしたら、`python tools/rock_research_shots.py <名前>` で画像を撮り直し（`docs/research/<名前>/latest.jpg` を更新し、日時付きの経過の画像も残す）、`docs/research/<名前>/README.md` の「今の状態」（状態・作り方・課題）と「経過」、`docs/research/rocks.md` の一覧の表（状態・次に直したいこと）を更新する。経過には日付と、何を変えて何が効いたか（効かなかったことも）を書く。新しい種類なら `docs/research/<名前>/` を作る（既存のページを写す）。`docs/reference/rock-catalog.md` の状態も合わせる。新しい種類のレシピを足したら `examples/ai-recipes/templates.json` にも足す（アプリの「テンプレートから作成」に出る）。

## 6. 報告する

依頼に対して何を組んだか（主な流れ）、`eval` の数値（寸法・塊・空洞）、撮った画像の所見、残っている不満を短く伝える。作りにくかったノードや表記があれば正直に書く（ユーザーはノードの方向性に活かす）。

## 落とし穴（よくある失敗）

- **To Volume の解像度は 16〜128**。セル ≒ 最長辺 ÷ 解像度。これより細い割れ目・隙間・薄片は消える。
- **比の単位**: Volume 系の幅・深さ・量の多くは「最長辺に対する比」で、メートルではない（catalog の `unit`）。
- **小片を捨てるノード**（Plane Cuts / Volume Noise / Smooth / Edge Wear / Terrace）。そのノードが削って切り離した小片と、最大の塊の 1% 未満の破片を捨てる（入力の時点で分かれていた大きな塊は残る）。割れ目で分かれた岩を 1 つの塊にしたいなら、先に Volume Close（occlusion）で繋ぐ。
- **原点中心の形**: Random Boxes・Base Shape は原点中心。Volume Clip を `"mode": "ground"`（`embed` で埋める割合）にすると、持ち上げずに地面へ据えられる。`world` のまま高さ 0 で切ると下半分が消える。
- **Volume Crack** は Points か Planes のどちらか一方だけを繋ぐ。深さ 1 や外面に近い面は岩を分ける。
- **ピース系**: Scatter Points の点数は Pieces 入力で 1 片あたり 2〜512、片の数との積が 1024 以下。Scatter Points は凸な Mesh を要る。Peel は Voronoi 直後（Piece Transform より前）に使う。Piece Filter には Pieces と Selection の両方を繋ぐ。
- **Parallel Planes** は評価範囲に 512 面まで（岩の大きさ ÷ 間隔）。片理なら Layered Boxes・Voronoi と同じ `rotation` を書く。
- **節理で割れた岩は「割ってから欠く」**: ひびを彫るだけだと後付けに見える。ブロックに割り、Piece Select の Peel（地面から生えた岩は `grounded`）で外周から抜き取る。不規則な割れ方は節理の向きに回して伸ばした Voronoi（Points）、規則的な割れ方は Parallel Planes を連結して Voronoi Fracture の Planes 入力。Piece Transform の `jitterPosition` / `jitterRotation` でブロックをずらすと石積みに見えない（`granite-buttress`）。元の形の平らな面が残ると石垣に見えるので、岩体を大きめにして侵食を進める。 塔・突起として芯を残すには Scatter Points の `clustering`（密度のむら）と Peel の `peelSize`（大きさの効き）。ブロックの面がばらばらで石積みに見えるなら、主な節理の Parallel Planes を Voronoi Fracture の Planes にも繋ぎ `"snap": true`（節理面への吸着。板 ∩ Voronoi で、節理面が複数のブロックにまたがる一枚の面になる）。一本のまとまった岩峰・岩塔なら、箱を強く侵食するより Base Shape の `convexPeak`（凸岩峰。`peakRidgeLength` で稜線の頂、`peakSlabThickness` / `peakSlabDip` で節理面と同じ向きの傾いた板）で大きな輪郭を先に決め、Peel を弱く（`fraction` 0.1 前後）、`peelEdge` 1 で稜・角だけを欠く（`granite-buttress`）。面の中央に穴が開かない。
- **割れ目が格子・石積みに見える**: Parallel Planes の割れ目は既定で端から端まで続く。Volume Crack の `extent`（割れ目の長さ）・`coverage`（割合）・`stagger`（段違い）で途中で止まる節理にする。`noise` を上げて途切れさせると点線になる。
- **Pieces to Mesh の後の空洞**: 板の隙間が閉じた空洞として大量に残る。Volume Close で埋まる。
- **ノードの C++ を変えたら `rock_editor` も作り直す**: 撮影（`rock_shot.py`）はアプリで評価する。`rock_cli` だけ作り直すと、古いアプリが知らない設定を無視した形が写り、効果を見誤る。
- **板に割れた岩は「扁平な点の Voronoi」**: 伸長の Y を小さく（0.45 など）すると不規則な板になる（`platy-joints`）。傾いた板を Piece Transform でずらすと、継ぎ目が格子 1 つ分の隙間になり縁がのこぎり状になる。薄い板は表面積が大きく、Volume Noise の量が大きいと板を削り切る。三角形が多い（数十万）ので Decimate で減らす。
- **Peel の欠ける順序は Voronoi Fracture のノード ID にも依存する**: レシピを写して ID を振り直すと形が変わる（全て崩れることもある）。写すときは ID を保つ。
- **後から足した所・削った所を別の素材で塗る**: Volume Diff Mask の Before に足す・削る前の Volume、Mesh に UV Unwrap の出力を繋ぐ（隙間を Volume Close で埋めた土、Volume Scatter で足した礫（`conglomerate` / `breccia`）、割れ目）。Before はメッシュと同じ位置にそろえる（Volume Clip の接地の後から取る）。しきい値は後段の Noise・Edge Wear の揺れより大きく。
- **水面・地面の近くなど、ワールドの高さで決まる帯**: Volume Undercut の `"reference": "world"` と `level`（m）。先に Volume Clip の `ground` で接地してから使う（波食ノッチ `wave-cut-notch`、風食の足元 `wind-erosion`）。
- **向きで削る（風上の面を後退させる・流れの溝）**: Volume Erode。`"type": "exposure"` は `direction`（来る向き）を向く面を削り陰を残す（`wind-erosion`）。`"type": "flow"` は重力で流れ下る筋に沿って溝を彫る（`limestone-rills`）。前段に Volume Smooth の向き集中（大きな半径）を置くと面の中央が皿状にくぼむので、丸めは Erode に任せる。中間ノードのプレビューは段々に見えるので、効果は最終メッシュ（Volume to Mesh の後）で判断する。
- **向きで偏らせる**: Volume Smooth / Edge Wear / Volume Noise / Volume Undercut は共通の `upwardFocus`（向きに集中）と `focusDirection`（集中する向き）を持つ。タフォニを陰の面へ、ノッチを波の当たる側へ、くぼみを風下へ（`honeycomb` / `wave-cut-notch` / `wind-erosion`）。
- **流れた筋を塗る**（鉄錆・汚れ・濡れ跡）: Flow Mask。Mesh に UV Unwrap の出力、Volume に接地後の Volume（UV 展開後のメッシュは格子に変換できない）。Noise Mask と乗算すると途切れる（`rust-streaks`）。
- **扁平な形を積む**（枕状溶岩の枕、寝た礫）: Volume Scatter の `lie`（寝かせる割合）で短い軸を上へ向ける。`blend` を上げすぎると形が溶け合って輪郭が消える（`pillow-lava` は 0.12）。
- **Debug ビルドの Remesh** はスタックオーバーフローで落ちることがある。撮影・評価は Release を使う。
