---
name: rock-graph
description: rock-editor の岩グラフ（.rockgraph）をノードで組んで岩を作る・直す手順。「〇〇な岩を作って」「この岩グラフを調整して」のように、岩の形をノードグラフとして書く依頼で使う。rock_cli で評価し、tools/rock_shot.py で撮って確かめる。
---

# 岩グラフを組む

岩グラフは JSON のノードグラフ。手で書きやすい表記で書き、`rock_cli` で数値を、`tools/rock_shot.py` で見た目を確かめながら直す。GPU アプリを何度も手で起動しない。

仕様の詳細は `docs/reference/ai-authoring.md`。ノードごとの範囲・単位・意味は `rock_cli catalog` が正本（この文書に書き写さない）。

## 0. 準備

```bash
cmake --build build --config Release --target rock_cli rock_editor   # 無ければ・古ければ
build/bin/Release/rock_cli.exe catalog > "$TEMP/catalog.json"       # ノードの一覧（大きいので必要な種類だけ読む）
```

catalog の各ノード: `kind`（保存名）、`inputs` / `outputs`（ピンの番号・名前・型）、`defaults`、`purpose` / `notes`（何をするか・落とし穴）、`params`（`path`・`type`・`min` / `max`・`unit`・`meaning`・`options`・`outOfRange`）。`internal: true` の項目は書かない。

## 1. 出発点を選ぶ

`examples/ai-recipes/` のレシピ（片理・塊状・板状節理・柱状節理・スレート・崖錐の岩片・河原の丸石・蜂の巣状の風化・礫岩・角礫岩・多孔質の溶岩・花崗岩のシーティング・きのこ岩・フードゥー・羊背岩・丸い転石・層理の段）から近いものを写す。作れる岩・作れない岩と、その理由は `docs/reference/rock-catalog.md`。作れない種類を頼まれたら、近いもので妥協せず、足りないノードをユーザーに伝える。README に組み方の要点がある。ゼロから組むより、レシピの数値を変える方が速く失敗しにくい。

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

評価の完了を待ち、形全体が入るようにカメラを引いて撮る（1 方向 1〜数秒）。正面だけでは裏側の失敗を見落とすので、仕上げの確認は `--views 4` で撮る。撮った PNG を Read で見て、依頼の岩に見えるかを判断する。`--ui` は評価エラーが画面に出る。数値が良くても見た目が依頼と違えば直す。

## 5. 報告する

依頼に対して何を組んだか（主な流れ）、`eval` の数値（寸法・塊・空洞）、撮った画像の所見、残っている不満を短く伝える。作りにくかったノードや表記があれば正直に書く（ユーザーはノードの方向性に活かす）。

## 落とし穴（よくある失敗）

- **To Volume の解像度は 16〜128**。セル ≒ 最長辺 ÷ 解像度。これより細い割れ目・隙間・薄片は消える。
- **比の単位**: Volume 系の幅・深さ・量の多くは「最長辺に対する比」で、メートルではない（catalog の `unit`）。
- **小片を捨てるノード**（Plane Cuts / Volume Noise / Smooth / Edge Wear / Terrace）。そのノードが削って切り離した小片と、最大の塊の 1% 未満の破片を捨てる（入力の時点で分かれていた大きな塊は残る）。割れ目で分かれた岩を 1 つの塊にしたいなら、先に Volume Close（occlusion）で繋ぐ。
- **原点中心の形**: Random Boxes・Base Shape は原点中心。Volume Clip を `"mode": "ground"`（`embed` で埋める割合）にすると、持ち上げずに地面へ据えられる。`world` のまま高さ 0 で切ると下半分が消える。
- **Volume Crack** は Points か Planes のどちらか一方だけを繋ぐ。深さ 1 や外面に近い面は岩を分ける。
- **ピース系**: Scatter Points の点数は Pieces 入力で 1 片あたり 2〜512、片の数との積が 1024 以下。Scatter Points は凸な Mesh を要る。Peel は Voronoi 直後（Piece Transform より前）に使う。Piece Filter には Pieces と Selection の両方を繋ぐ。
- **Parallel Planes** は評価範囲に 512 面まで（岩の大きさ ÷ 間隔）。片理なら Layered Boxes・Voronoi と同じ `rotation` を書く。
- **Pieces to Mesh の後の空洞**: 板の隙間が閉じた空洞として大量に残る。Volume Close で埋まる。
- **Debug ビルドの Remesh** はスタックオーバーフローで落ちることがある。撮影・評価は Release を使う。
