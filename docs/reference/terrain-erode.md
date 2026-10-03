# Terrain Erode / Terrain Deform（山グラフの地形の侵食と変形）

作成日時: 2026-10-03 10:40
更新日時: 2026-10-03 11:05

## 位置付け

[山グラフと Heightmap](heightmap.md) の地形（ハイトの格子）を、岩を撒く前に侵食し、岩を撒いた後に岩の根元を変形する。那須朝日岳の風景（[研究ページ](../research/nasu-asahidake/README.md)）で、土の斜面が「水と重力で形が決まった」ように見えるために足した（2026-10-03 のユーザー判断）。

```text
Heightmap → Terrain Erode ─┬→ Rock Scatter（Terrain）→ … Coverage → Mask Filter（広げる）─┐
                            └→ Terrain Deform（Terrain、Mask ←──────────────────────────┘）→ Apply Material … → Mesh Output（地面）
```

- 地形の型は新設せず、M1 の判断どおり UV 付きの Mesh のまま流す。Heightmap / Terrain Erode / Terrain Deform の出力は、評価結果の `GeneratedRock::terrain`（ハイトの格子）と `terrainSettings`（寸法）を持ち、下流の地形ノードはメッシュではなくこの格子を加工してメッシュを作り直す。格子を持たないメッシュ（Base Shape など）をつなぐと診断する。
- 計算は格子の上の CPU 処理で、GPU は要らない（`rock_cli eval` で評価できる）。

## Terrain Erode

入力 Terrain（格子を持つ Mesh）、出力 Mesh。保存名 `terrainErode`。

| 項目 | 内容 |
| --- | --- |
| 安息角 (度) | これより急な斜面の土が低い隣へ崩れる（10〜80、既定 35）。崖錐は 30〜38 |
| 崩れの回数 | 熱侵食の反復（0〜500、既定 40）。0 で崩さない |
| 崩れの割合 | 1 回に崩す割合（0〜1、既定 0.5） |
| 流路の深さ (m) | 最も流れの集まる筋を彫る深さ（0〜50、既定 0.4）。0 で彫らない |
| 流路の集中 | 流れの量（対数で 0〜1）^ この値（0.1〜4、既定 0.6）。小さいほど細い筋も彫れる |
| 流路の幅 (m) | 筋をぼかす幅（0〜50、既定 1）。0 でぼかさない |

計算（`geometry::ErodeTerrain`）:

1. 格子を m に直す（高さは `minHeight + 値 × (maxHeight − minHeight)`、間隔は幅 ÷ (列数 − 1)）。
2. 熱侵食: 各点で 8 近傍への落差から `落差 − 距離 × tan(安息角)` の超過を求め、最大の超過の半分 × 割合を、超過に比例して低い隣へ配る（Musgrave の方式）。土は移すだけで総量は変わらない。回数ぶん繰り返す。
3. 水侵食: D8 の流れの量（高い順に処理し、最も急な低い隣へ上流の数を流す）を求め、`log(量) / log(最大)` を集中で累乗して深さを掛け、ガウスで幅だけぼかして引く。彫るだけで盛らない。
4. 0〜1 へ戻す（熱侵食は最高を超えないので切り詰めは起きない）。

## Terrain Deform

入力 Terrain（格子を持つ Mesh）、Mask（地形の UV の画像。Shape Mask、Rock Scatter の Coverage、Mask Filter / Combine など）、出力 Mesh。保存名 `terrainDeform`。

| 項目 | 内容 |
| --- | --- |
| 量 (m) | マスクの白い所を動かす量（-20〜20、既定 -0.3）。正で盛る、負でえぐる |
| ぼかし (m) | マスクの縁をぼかす幅（0〜50、既定 0.5） |

計算（`geometry::DeformTerrain`）: 格子の点ごとにマスクを `u = 列 / (列数 − 1)`、`v = 行 / (行数 − 1)` で読み（マスクのノードの反転も掛ける）、ガウスでぼかし、`量 × 値` を高さに足す。0〜1 の外へ出た高さは切り詰める（最高・最低の外には盛れない）。

岩の配置は変形前の地形で決める（Rock Scatter の Terrain には Terrain Erode の出力をつなぎ、Terrain Deform の出力は地面の Mesh Output 側だけにつなぐ）。変形後の地形に岩を撒き直すと Coverage が変わって循環するので、グラフでは組めない（Deform の Mask が Scatter の Coverage で、Scatter の Terrain が Deform の出力、は輪になり接続できない）。

## 制限

- 熱侵食の回数が多いと格子が大きいとき遅い（256² × 500 回で数秒）。
- 水侵食は D8 の流れの量を 1 回求めるだけで、溝が深くなると流れが変わる反復はしない。堆積もしない。那須の例（40 m・256²）では深さ 1.2 m にしても筋が細すぎて目立たなかった。見える溝には反復（彫る → 流れを求め直す）が要る。
- Terrain Deform の盛り上げは最高の高さ（`maxHeight`）で切り詰める。盛る余地が要るときは Heightmap の最高の高さを上げる。
- 変形した地形のメッシュに置いた岩の高さは、変形前の地形で決まる（根元をえぐると岩は少し浮く。沈める量で補う）。

## 検証

`tests/TerrainErodeTests.cpp`。崖を崩すと最大の勾配が安息角の近くまで下がり土の量は変わらない、回数 0 で変えない、安息角が大きいほど崩れない、範囲外の診断。谷の斜面で流路が下流ほど深く尾根と上流は浅い、彫るだけで盛らない、幅で脇も彫れる。Deform はマスクの白い所だけ盛る・えぐる、反転、ぼかし、量の範囲外。グラフ: Heightmap → Erode → Deform（Shape Mask）→ Mesh Output、Erode → Rock Scatter、格子を持たないメッシュの診断。
