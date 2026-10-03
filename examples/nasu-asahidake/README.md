# 那須朝日岳の岩場（nasu-asahidake）— 刃の列 + 土の斜面 + 破片の 1 ユニット

作成日時: 2026-10-03 07:15
更新日時: 2026-10-03 10:41

目標の風景（`docs/references/nasu-asahidake/DSC00363`。Git の対象外）を 1 ユニット（40 m 四方）として組む山グラフ。
このフォルダをルートとして開き、`nasu.mountaingraph` を読み込む。

```text
Heightmap（Heightmaps/slope.png、40 m、尾根が最も高い斜面）→ Terrain Erode（崩れ 34°、流路 0.5 m）─┬→ Terrain Deform（Mask = 刃の Coverage を 4 m 広げたもの、-0.4 m）→ Apply Material（土）→ Apply Material（暗い土、Mask = Noise Mask）→ Apply Material（岩屑、Mask = 刃の Coverage を 4 m 広げたもの）→ Subdivide → Displace → Mesh Output
                       ├→ Shape Mask（高さ: 尾根の頂）───────┐ Mask
                       ├→ Shape Mask（上向き度）──┐           │
                       ├────────────────────────│───────────┤ Terrain
Rock（blades = nasu-blades）────────────────────│───────────┤ Rock 1
                                                │           └→ 大 Rock Scatter（2 個、深く沈める）→ Mesh Output
                                                │                 └ Coverage ─┬→ Mask Filter（広げる 6 m）─┐
                                                │                             └→ Mask Filter（反転）──────┤→ Mask Combine（min）
Rock（talus ×1）・Rock（talus ×0.45）・Rock（chips ×0.3）→ 崖錐 Rock Scatter（Mask = 上の Combine、間隔 0.8 m、沈める量 ± 70%）→ Mesh Output
                                                └→ Mask Filter（レベル、白を 0.15 に）→ Mask Combine（min、反転した足元と）→ まばらな転石 Rock Scatter → Mesh Output
```

0. 素材は Megascans（`Materials/rocky-soil.rockmat` ほか 4 つ）。テクスチャは再配布できないので Git の対象外（`examples/**/textures/`）。手元の `data/textures/` から次の 4 フォルダを `examples/nasu-asahidake/textures/` へ写す: `MI_Rocky_Soil_Ground_uhomdjolw_2K`、`beach_gravel_udlladln_2k`、`gouged_rock_cliff_vlcpcbc_2k`、`mine_rock_wall_ueijag3ew_2k`（約 140 MB）。無いと素材が読めず、定数色にならずに未解決の素材として表示される。
1. 岩グラフ 3 つ（`blades`（レシピ `nasu-blades` + Rock Asset の鎖）、`talus`（`talus-fragment` + 同）、`chips`（`blocky` + 同。角張った破片））を焼く。焼いた結果はサンプルに含めていない。

   ```bash
   for n in blades talus chips; do python tools/rock_bake.py examples/nasu-asahidake/$n.rockgraph; done
   ```

   焼くとアプリが岩グラフを同じパスに書き戻す（長い書式になる）。書きやすい表記のまま残したいときは、焼く前に写しを取って戻す。
2. `nasu.mountaingraph` を開く。尾根に刃の列が 2 つ（互いに食い込み、向き 20° ± 12° で走向をそろえる）、その根元 6 m に破片が密に（`Mask Filter` の「広げる」）、斜面全体にまばらな転石。
3. 破片の密度は崖錐の Rock Scatter の間隔、広がりは「広げる」の半径、刃の沈み方は大の Rock Scatter の沈める量と浮きの補正で変える。
4. `rock_cli eval examples/nasu-asahidake/nasu.mountaingraph --node 20`（Rock Scatter を指す）の `rockInstanceSets` に段ごとの数と置いた位置（8 個以下なら列挙）が出る。地面の Displace は GPU が要るので、Mesh Output を指すと評価できない。
6. 地形は Terrain Erode（熱侵食で崖錐の斜面、流路の溝）で侵食し、岩の配置も地面もその出力から。地面側だけ Terrain Deform で刃の根元をえぐる（岩の配置は変形前の地形で決まる）。
7. 表示はシーンの空（大気散乱。太陽 54°・120000 lux）と手動 EV 14 にしてある。撮るときは `python tools/rock_shot.py examples/nasu-asahidake/nasu.mountaingraph out.png --lighting atmospheric --light-elevation 55 --yaw 330 --pitch 18`。
8. 地面は Apply Material 3 段（土、Noise Mask でまだらの暗い土、刃の根元の岩屑）と Subdivide → Displace（0.3 m）で荒らしてある。岩屑はハイトが低いので根元が少しえぐれる。
5. ハイトマップ `Heightmaps/slope.png`（16bit PNG、256²）は、奥（-Z）ほど高い斜面に中央の尾根の盛り上がりを足したもの。

今の形と課題は [研究ページ](../../docs/research/nasu-asahidake/README.md)。刃の列の作り方は [nasu-blades](../../docs/research/nasu-blades/README.md)。
