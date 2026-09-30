# 山グラフ（Heightmap）

このフォルダをルートとして開き、`mountain.rockmountain` を読み込む。

```text
Heightmap（ノイズの山）─┬→ Mesh Output ← Material ← Surface（Triplanar）
                         ├→ Shape Mask（上向き度・反転）─┐ Mask
                         └──────────────────────────────┤ Terrain
Rock（boulder.rockscene）────────────────────────────────┤ Rock 1
                                                         └→ Rock Scatter → Mesh Output
```

1. 200 m 四方・高さ 0〜80 m のノイズの山が出る。`Heightmap` を選ぶと、Seed・起伏の大きさ・荒さ・山の形を変えられる。種類を「画像」にすると、16bit PNG などのハイトマップを読める。
2. `Shape Mask` を選ぶと、傾斜のマスク（急な斜面が白）を地形に貼って見られる。
3. 岩を撒くには、先に岩アセットを焼く。`boulder.rockscene`（岩グラフ）を開き、`Rock Asset` を選んで「岩アセットを焼く」を押す（Material Bake も自動で先に焼く）。`mountain.rockmountain` を開き直すと、急な斜面に岩が撒かれる。焼いた結果（`boulder.rockscene.bake/`）はサンプルに含めていない。
4. `Rock Scatter` で間隔・倍率・法線に合わせる強さ・沈める量を変えられる。表示モードを「LOD（色分け）」にすると、岩の段（LOD）が色で分かる。

仕様は [Rock と Rock Scatter](../../docs/reference/rock-scatter.md)。

仕様は [山グラフと Heightmap](../../docs/reference/heightmap.md)。
