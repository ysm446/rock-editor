# 植生（Plant）— terrain-graph と共有する植生アセットを山グラフで撒く

作成日時: 2026-10-03 11:55
更新日時: 2026-10-03 11:55

## 位置付け

山グラフに草・低木・樹木を置く。2026-10-03 のユーザー判断で、植生は岩のように生成せず、**terrain-graph と同じ仕様の植生アセット**（Blender のスクリプトで作る LOD 付き FBX + `.tgmodel` + `.tgmat`）をそのまま読んで撒く。アセットを両方のアプリで共有するため、形式は terrain-graph のものを正とし、rock-editor 側で読めるようにする。アセットの作り方の決まりごとは terrain-graph の `docs/design/vegetation-assets.md`（寸法、葉のカード、スロットの並び、法線と芯、LOD、インポスター）。

```text
Rock Scatter（Terrain、Mask）← Plant（Models/Susuki/Susuki_Var1.tgmodel）… Rock 1〜N
      └ Instances → Mesh Output
```

## 段階

| 段階 | 内容 | 状態 |
| --- | --- | --- |
| 1 | `.tgmodel` / `.tgmat` の読み込み、FBX の `_LOD<n>` 名、Plant ノード、Rock Scatter で撒く、葉のアルファ抜きで描く | 2026-10-03 に実装 |
| 2 | インポスター（最終段。八面体の 12 × 12 方向、`_Impostor_C/N/V.png`。terrain-graph の `Impostor.cpp` の移植） | 未 |
| 3 | 色むら（`colorVariation`）、ミップ段に応じたアルファの持ち上げ、風の揺れ | 未 |

## 1. アセットの共有

- rock-editor は terrain-graph のアセット形式を受ける: `"format": "terrain-graph.model-asset"`（`.tgmodel`）と `"terrain-graph.material-asset"`（`.tgmat`）。共有アセットの読み込み（`ProjectWorkspace::ReadAsset`）は `rock-editor.<kind>` と `terrain-graph.<kind>` の両方を通す。アセットブラウザも `.tgmodel` / `.tgmat` を出す。
- `.tgmodel` の `source`（FBX）と `materials`（`.tgmat`）は**プロジェクトのルートからの相対パス**（`Models/Susuki/Susuki_Var1.fbx`）。rock-editor では `.tgmodel` の位置から上へ `project.reproj`（か `project.tgproj`）を探してルートを決め、無ければ `.tgmodel` のフォルダに同名のファイルを探す（`io::ReadModelAssetInfo`）。
- FBX の段は LODGroup か、オブジェクト名の末尾 `_LOD0` / `_LOD1` … で決める（Blender は LODGroup を書き出せない。`renderer::LoadModel` に terrain-graph と同じ規約を足した。番号が飛んでも詰める）。
- `.tgmodel` の `lodScreenSizes`（[i] が LOD i+1 に替わる画面の大きさ）は Rock Asset の「LODn の切替」と同じ意味なので、そのまま使う。無ければ 0.5 / 0.25 / 0.12 …。
- `.tgmat` の `blendMode: masked` と `maskThreshold` はそのまま効く（葉のカードの抜き）。`twoSided` は読まないが、岩のインスタンス描画はもともと両面（カリング無し）。`colorVariation` と `brightness` 以外の色の項目は段階 3。
- アセットは `data/Models/<名前>/` か、山グラフのプロジェクトのルートの `Models/<名前>/` に置く。生成物はリポジトリに入れない（`.gitignore` の `examples/**/Models/`）。

### 作り方（Blender）

terrain-graph のスクリプトをそのまま使う（rock-editor にはスクリプトを写さず、正は terrain-graph に置く）。出力先と `--root` を rock-editor 側のルートにする。

```bash
"C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" -b --factory-startup --python-exit-code 1 \
  --python d:/GitHub/terrain-graph/tools/blender/make_susuki.py -- --out examples/nasu-asahidake/Models/Susuki --root examples/nasu-asahidake
```

- 使えるスクリプト: `make_haimatsu`（ハイマツ）、`make_susuki`（ススキ）、`make_sasa` / `make_hakonedake`（ササ）、`make_dakekamba`、`make_miyamahannoki`、`make_inutsuge`、`make_oshirabiso` / `make_shirabiso`、`make_sugi`、`make_buna`、`make_arve`、`make_fichte`、`make_laerche` / `make_karamatsu`。
- 2026-10-03 時点の terrain-graph の `make_haimatsu.py` は `CORE` を import していなかった（`NameError`）。`from vegetation import (CORE, LEAVES, …)` に直して動かした（terrain-graph 側の修正。未コミット）。
- 重い処理なので `python tools/run_low.py -- blender …` で低い優先度にする。1 種類 1〜3 分。

## 2. Plant ノード

入力なし、出力 **Rock**（Rock ノードと同じ、アセットの参照の型）。保存名 `plant`。

| 項目 | 内容 |
| --- | --- |
| モデル資産 | `.tgmodel`（か `.model`）のパス。シーンからの相対で保存する |
| 倍率 | モデルに掛ける（0.001〜1000、既定 1。実寸のモデルなら 1 前後） |
| 重み | Rock Scatter で選ばれる割合 |

- 評価では `RockReference` に `model = true` を立て、範囲は FBX から読む（`io::ReadFbxBounds`。大きさの間隔・浮きの補正・被覆に使う）。
- 描画は岩アセットと同じ `LoadedRockAsset` の経路（`Application::LoadPlantAsset`）。FBX を `renderer::LoadModel` で読み、スロットのマテリアルは `.tgmat` を共有アセットとしてライブラリへ読む（ルートの外なら無地）。段の切り替えは `lodScreenSizes`。GPU インスタンス描画で、段はカメラから見た大きさで選ぶ（岩と同じ）。
- Rock Scatter の設定はそのまま使える。草を岩の根元に寄せるには、岩の Coverage を Mask Filter の「広げる」で広げ、反転した Coverage と Noise Mask を合わせて Mask につなぐ（`examples/nasu-asahidake/`）。

## 制限（段階 1）

- インポスターが無い。最後のメッシュの段より遠くは、その段のメッシュのまま描く（数百株の規模では問題ない）。
- ミップ段に応じたアルファの持ち上げが無いので、遠くで葉が痩せる。
- `colorVariation`（株ごとの色むら）と風の揺れは無い。
- 影はメッシュで落とすが、影パスでアルファ抜きをしないので、葉のカードは四角い影になる（確認して直す）。
- `rock_cli eval` は FBX の範囲を読むために `ufbx` を使う（植生 1 種類あたり数十 ms）。

## 検証

`tests/RockScatterTests.cpp` の「Plant」: 出力の型、Rock Scatter への接続、未選択・無いファイルの診断、`IsModelAssetPath`。実アプリでは `examples/nasu-asahidake/` にススキ 3 変種（刃の根元の 2.5 m の帯 × まだら）とハイマツ 2 変種（斜面にまばら）を撒き、葉のアルファ抜きで描けることを画像で確認した（[研究ページ](../research/nasu-asahidake/README.md)）。
