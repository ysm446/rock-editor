# 那須朝日岳の岩場（nasu-asahidake）— 刃の列 + 土の斜面 + 破片の 1 ユニット

作成日時: 2026-10-03 07:15
更新日時: 2026-10-03 07:15

目標の風景（`docs/references/nasu-asahidake/DSC00363`。Git の対象外）を 1 ユニット（40 m 四方）として組む山グラフ。
このフォルダをルートとして開き、`nasu.mountaingraph` を読み込む。

```text
Heightmap（Heightmaps/slope.png、40 m、尾根が最も高い斜面）─┬→ Mesh Output ← Surface（褐色の土、定数色）
                       ├→ Shape Mask（高さ: 尾根の頂）───────┐ Mask
                       ├→ Shape Mask（上向き度）──┐           │
                       ├────────────────────────│───────────┤ Terrain
Rock（blades = nasu-blades）────────────────────│───────────┤ Rock 1
                                                │           └→ 大 Rock Scatter（2 個、深く沈める）→ Mesh Output
                                                │                 └ Coverage ─┬→ Mask Filter（広げる 6 m）─┐
                                                │                             └→ Mask Filter（反転）──────┤→ Mask Combine（min）
Rock（talus ×1）・Rock（talus ×0.45）→ 崖錐 Rock Scatter（Mask = 上の Combine、間隔 0.8 m）→ Mesh Output
                                                └→ Mask Filter（レベル、白を 0.15 に）→ Mask Combine（min、反転した足元と）→ まばらな転石 Rock Scatter → Mesh Output
```

1. 岩グラフ 2 つ（`blades`（レシピ `nasu-blades` + Rock Asset の鎖）、`talus`（`talus-fragment` + 同））を焼く。焼いた結果はサンプルに含めていない。

   ```bash
   python tools/rock_bake.py examples/nasu-asahidake/blades.rockgraph
   python tools/rock_bake.py examples/nasu-asahidake/talus.rockgraph
   ```

   焼くとアプリが岩グラフを同じパスに書き戻す（長い書式になる）。書きやすい表記のまま残したいときは、焼く前に写しを取って戻す。
2. `nasu.mountaingraph` を開く。尾根に刃の列が 2 つ（互いに食い込む）、その根元 6 m に破片が密に（`Mask Filter` の「広げる」）、斜面全体にまばらな転石。
3. 破片の密度は崖錐の Rock Scatter の間隔、広がりは「広げる」の半径、刃の沈み方は大の Rock Scatter の沈める量と浮きの補正で変える。
4. `rock_cli eval examples/nasu-asahidake/nasu.mountaingraph` の `rockInstanceSets` に段ごとの数と置いた位置（8 個以下なら列挙）が出る。
5. ハイトマップ `Heightmaps/slope.png`（16bit PNG、256²）は、奥（-Z）ほど高い斜面に中央の尾根の盛り上がりを足したもの。

今の形と課題は [研究ページ](../../docs/research/nasu-asahidake/README.md)。刃の列の作り方は [nasu-blades](../../docs/research/nasu-blades/README.md)。
