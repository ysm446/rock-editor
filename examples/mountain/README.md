# 山グラフ（Heightmap）

このフォルダをルートとして開き、`mountain.mountaingraph` を読み込む。

```text
Heightmap（ノイズの山）─┬→ Mesh Output ← Material ← Surface（Triplanar）
                         ├→ Shape Mask（傾斜）──────────┬─────────────┐ Mask
                         ├──────────────────────────────┤ Terrain     │
Rock（boulder ×5、約 16 m）──────────────────────────────┤ Rock 1      │
                                                         └→ 大 Rock Scatter → Mesh Output
                                                              └ Coverage → Mask Filter（反転）─┐
                                   Mask Combine（minimum）← 傾斜 ──────────────────────────────┘
Rock（boulder ×1.5）・Rock（stone ×3）→ 中 Rock Scatter（Mask = 上の Combine）→ Mesh Output
                                              └ Coverage → Mask Filter（反転）→ Mask Combine（大も中も無い所）
Rock（stone ×1）→ 小 Rock Scatter（Mask = 上の Combine）→ Mesh Output
```

1. 200 m 四方・高さ 0〜80 m のノイズの山が出る。`Heightmap` を選ぶと、Seed・起伏の大きさ・荒さ・山の形を変えられる。種類を「画像」にすると、16bit PNG などのハイトマップを読める。
2. `Shape Mask` を選ぶと、傾斜のマスク（急な斜面が白）を地形に貼って見られる。
3. 岩を撒くには、先に岩アセットを焼く。`python tools/rock_bake.py examples/mountain/boulder.rockgraph` と `stone.rockgraph` で焼く（アプリで岩グラフを開き `Rock Asset` の「岩アセットを焼く」を押すのと同じ。Material Bake も自動で先に焼く）。`mountain.mountaingraph` を開くと、急な斜面に岩が撒かれる。焼いた結果（`*.rockgraph.bake/`）はサンプルに含めていない。
4. `Rock Scatter` で間隔・倍率・法線に合わせる強さ・沈める量を変えられる。表示モードを「LOD（色分け）」にすると、岩の段（LOD）が色で分かる。
5. 大・中・小の 3 段に分けてある。1 段目の `Coverage` 出力を `Mask Filter`（levels・反転）に通し、`Mask Combine`（minimum）で傾斜のマスクと合わせて 2 段目の `Mask` につなぐ。2 段目の岩は 1 段目の足元を避ける。3 段目は 1 段目と 2 段目の両方の足元を避ける。「大きさの間隔」で足元の半径の和より近づけず、「浮きの補正」で急斜面の谷側の底が浮かないように沈める。`rock_cli eval examples/mountain/mountain.mountaingraph` の `rockInstanceSets` に段ごとの数と推定の被覆率が出る（大 33 個・18%、中 288 個・15%、小 2,983 個・17%）。

仕様は [Rock と Rock Scatter](../../docs/reference/rock-scatter.md)。

仕様は [山グラフと Heightmap](../../docs/reference/heightmap.md)。
