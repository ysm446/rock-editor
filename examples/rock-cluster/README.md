# 岩の集合体（rock cluster）— 大岩が数個重なり、地面があり、小石が散らばる 1 ユニット

作成日時: 2026-10-03 07:45
更新日時: 2026-10-03 07:45

数十 m の岩の集合体を 1 ユニットとして作る例。山グラフ（`cluster.mountaingraph`）を 60 m 四方の小さい地形で使い、
焼いた岩アセットを大・中・小の 3 段で撒く。このフォルダをルートとして開く。

```text
Heightmap（60 m、中央が高い丘）─┬→ Mesh Output ← Surface（定数色）           … 地面
                               ├→ Shape Mask（高さ: 頂だけ白）──┐ Mask
                               ├───────────────────────────────┤ Terrain
Rock（buttress / blocky ×4 / boulder ×4）───────────────────────┤ Rock 1〜3
                                                               └→ 大 Rock Scatter → Mesh Output   … 大岩 5 個を頂に重ねる
                                                                    └ Coverage → Mask Filter（反転）─┐
                               Shape Mask（高さ: 丘の広い範囲）→ Mask Combine（minimum）←────────────┘
Rock（blocky ×1.6 / boulder ×1.8）→ 中 Rock Scatter（Mask = 上の Combine）→ Mesh Output             … 大岩の周りの数 m の岩
                                        └ Coverage → Mask Filter（反転）→ Mask Combine（大も中も無い所）→ Mask Combine（丘の周り）
Rock（talus / boulder ×0.5）→ 小 Rock Scatter（Mask = 上の Combine）→ Mesh Output                   … 小石
```

1. 岩グラフ 4 つ（`buttress`（granite-buttress）、`boulder`（rounded-boulder）、`blocky`、`talus`（talus-fragment）。
   `examples/ai-recipes/` のレシピに Decimate → UV Unwrap → Material Bake → Rock Asset の鎖と定数色の Surface を足したもの）を焼く。
   焼いた結果（`*.rockgraph.bake/`）はサンプルに含めていない。

   ```bash
   for n in buttress boulder blocky talus; do python tools/rock_bake.py examples/rock-cluster/$n.rockgraph; done
   ```

   注意: 焼くとアプリが岩グラフを同じパスに書き戻す（長い書式になる）。書きやすい表記のまま残したいときは、焼く前に写しを取って戻す。
2. `cluster.mountaingraph` を開く。大岩 5 個（岩峰 1、角張った岩と丸い岩）が丘の頂に重なり、周りに中くらいの岩、裾に小石が散る。
3. 重なり具合は大の Rock Scatter の「大きさの間隔」（0.45。1 で足元の円が接し、小さいほど食い込む）と「間隔」で、
   寄せ方は Shape Mask（高さ）の下限・上限で変える。中・小の寄せ方は 2 つ目の Shape Mask の下限。
4. `rock_cli eval examples/rock-cluster/cluster.mountaingraph` の `rockInstanceSets` に段ごとの数が出る（大 5、中 14、小 213）。

今の形は [研究ページ](../../docs/research/rock-cluster/README.md)。仕様は [Rock と Rock Scatter](../../docs/reference/rock-scatter.md)、
[岩アセットと山グラフ](../../docs/reference/rock-asset-mountain-graph.md)。
