# 山グラフ（Heightmap）

このフォルダをルートとして開き、`mountain.rockmountain` を読み込む。

```text
Heightmap（ノイズの山）─┬→ Mesh Output ← Material ← Surface（Triplanar）
                         └→ Shape Mask（上向き度・反転）
```

1. 200 m 四方・高さ 0〜80 m のノイズの山が出る。`Heightmap` を選ぶと、Seed・起伏の大きさ・荒さ・山の形を変えられる。種類を「画像」にすると、16bit PNG などのハイトマップを読める。
2. `Shape Mask` を選ぶと、傾斜のマスク（急な斜面が白）を地形に貼って見られる。M2 では、このマスクで岩を撒く場所を決める予定。

仕様は [山グラフと Heightmap](../../docs/reference/heightmap.md)。
