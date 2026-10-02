# 岩の研究ページ

作成日時: 2026-10-01 12:50
更新日時: 2026-10-03 07:50

「あらゆる種類の岩を作れること」を目標に、岩の種類ごとに今の出来・作り方・課題・改善の経過を残す。種類ごとにフォルダ（`docs/research/<レシピ名>/`）があり、そこの `README.md` に状態・作り方・課題・経過の文、`latest.jpg` に今の形、日時付きの `.jpg` に経過の画像を置く。一つずつ改善していく。足りないノードの整理（G 番号）は [岩の種類の網羅](../reference/rock-catalog.md)、組み方の手順は [skill](../../.agents/skills/rock-graph/SKILL.md)。

## 改善の進め方

1. 下の一覧から 1 種類を選ぶ（△ や × を優先）。
2. レシピ（`examples/ai-recipes/<名前>.rockgraph`）を直す。足りないノードがあれば、ノードを足すか直す。
3. `rock_cli eval` で数値（寸法・塊・空洞）を確かめ、`python tools/rock_research_shots.py <名前>` で画像を撮り直す。新しい種類のレシピを足したら、`examples/ai-recipes/templates.json` にも足す（アプリの「テンプレートから作成」に出る。テストが一覧とレシピ・画像の食い違いを検出する）。
4. その種類のフォルダの `README.md` の「今の状態」を書き換え、「経過」に日付と何を変えたか（効いたこと・効かなかったこと）を追記する。この一覧の表の状態と「次に直したいこと」も合わせる。[網羅](../reference/rock-catalog.md) の表の状態も合わせる。

レシピはどれも、アプリの「ファイル」→「テンプレートから作成…」（アセット欄の右クリックにもある）から複製して開ける。サムネイルは各フォルダの `latest.jpg` の左上の 1 方向。

画像はどれも 4 方向（左上から 35°・125°・215°・305°、見下ろし 20°）を 2×2 にまとめたもの。素材は付けていないものが多い（`gneiss` / `marble` / `conglomerate` / `breccia` / `rust-streaks` は Surface の定数色）。

状態: ○ それらしく作れる / △ 作れるが不満が残る / × まだ作れない / 未 まだ試していない

## 一覧

| 岩 | 状態 | レシピ | 次に直したいこと |
| --- | --- | --- | --- |
| [塊状・節理で割れた岩](blocky/README.md) | ○ | `blocky` | 節理面が大味 |
| [板状節理](platy-joints/README.md) | △ | `platy-joints` | 元の箱の面が一部残る・板の縁がのこぎり状 |
| [柱状節理（玄武岩）](columnar/README.md) | ○ | `columnar` | 柱の頭の形・風化 |
| [片理（結晶片岩）](schist/README.md) | ○ | `schist` | 斜めの細溝のボクセルの段差 |
| [スレート・頁岩](slate/README.md) | ○ | `slate` | 板が浮いて見える所 |
| [層理の段](layered-ledges/README.md) | ○ | `layered-ledges` | — |
| [崖錐の角張った岩片](talus-fragment/README.md) | ○ | `talus-fragment` | — |
| [河原の丸石](river-pebble/README.md) | ○ | `river-pebble` | 形が単調 |
| [丸い転石](rounded-boulder/README.md) | ○ | `rounded-boulder` | — |
| [花崗岩のシーティング](granite-sheeting/README.md) | △ | `granite-sheeting` | 平らな面のボクセルの格子模様 |
| [花崗岩の岩峰](granite-buttress/README.md) | △ | `granite-buttress` | 凸岩峰ルートに置き換え（[比較](granite-buttress/joint-study.md)）。裾の角の小さな穴。複数の塔が根元でつながる形 |
| [黒曜石・フリント](obsidian/README.md) | ○（形） | `obsidian` | ガラス質の素材 |
| [礫岩](conglomerate/README.md) | ○ | `conglomerate` | — |
| [角礫岩](breccia/README.md) | ○ | `breccia` | 礫が基質に浮いて見える（礫どうしが接しない） |
| [多孔質の溶岩](vesicular-basalt/README.md) | ○ | `vesicular-basalt` | — |
| [蜂の巣状の風化（タフォニ）](honeycomb/README.md) | ○ | `honeycomb` | — |
| [きのこ岩](mushroom-rock/README.md) | ○ | `mushroom-rock` | — |
| [フードゥー](hoodoo/README.md) | ○ | `hoodoo` | 頂の硬い帽子岩 |
| [羊背岩](roche-moutonnee/README.md) | ○ | `roche-moutonnee` | 擦痕（素材のハイト） |
| [玉ねぎ状風化・剥離ドーム](spheroidal-weathering/README.md) | △ | `spheroidal-weathering` | 剥がれの縁の薄い板に穴が開く |
| [石灰岩の溶食（縦溝）](limestone-rills/README.md) | △ | `limestone-rills` | 溝が布のひだのよう。平行で細い溝にする |
| [風食](wind-erosion/README.md) | △ | `wind-erosion` | 風上の面がえぐれて縁が残る。流線形にする |
| [枕状溶岩](pillow-lava/README.md) | △ | `pillow-lava` | 枕の表面の放射状の割れ目・ガラス質の縁 |
| [波食ノッチ](wave-cut-notch/README.md) | ○ | `wave-cut-notch` | 岩体の節理の面 |
| [片麻岩](gneiss/README.md) | ○ | `gneiss` | 縞のシマウマ感 |
| [大理石](marble/README.md) | △ | `marble` | 網目の感じ |
| [鉄錆・汚れの流れた筋](rust-streaks/README.md) | △ | `rust-streaks` | 筋が太くまだら。細く長い筋にする |

使い方の例（チュートリアル）は[末尾](#使い方の例)。岩の種類ではなく、ノードの組み方を見せる例。

## 形の骨格（成因・節理）

- [塊状・節理で割れた岩](blocky/README.md)（○）
- [板状節理](platy-joints/README.md)（△）
- [柱状節理（玄武岩）](columnar/README.md)（○）
- [片理（結晶片岩）](schist/README.md)（○）
- [スレート・頁岩](slate/README.md)（○）
- [層理の段](layered-ledges/README.md)（○）
- [崖錐の角張った岩片](talus-fragment/README.md)（○）
- [河原の丸石](river-pebble/README.md)（○）
- [丸い転石](rounded-boulder/README.md)（○）
- [花崗岩のシーティング](granite-sheeting/README.md)（△）
- [花崗岩の岩峰](granite-buttress/README.md)（△）
- [黒曜石・フリント](obsidian/README.md)（○（形））

## 堆積・火山の組織

- [礫岩](conglomerate/README.md)（○）
- [角礫岩](breccia/README.md)（○）
- [多孔質の溶岩](vesicular-basalt/README.md)（○）

## 風化・侵食の形

- [蜂の巣状の風化（タフォニ）](honeycomb/README.md)（○）
- [きのこ岩](mushroom-rock/README.md)（○）
- [フードゥー](hoodoo/README.md)（○）
- [羊背岩](roche-moutonnee/README.md)（○）
- [玉ねぎ状風化・剥離ドーム](spheroidal-weathering/README.md)（△）
- [石灰岩の溶食（縦溝）](limestone-rills/README.md)（△）
- [風食](wind-erosion/README.md)（△）
- [枕状溶岩](pillow-lava/README.md)（△）
- [波食ノッチ](wave-cut-notch/README.md)（○）

## 表面・色（素材）

- [片麻岩](gneiss/README.md)（○）
- [大理石](marble/README.md)（△）
- [鉄錆・汚れの流れた筋](rust-streaks/README.md)（△）

## 使い方の例

岩の種類ではなく、ノードの組み方を見せる例（チュートリアル）。テンプレートの一覧では「使い方の例」の分類に出る。ノードの note に手順を書いてある。

- [岩の隙間を土で埋める](soil-filled-gaps/README.md)

## 山グラフのユニット（岩グラフではなく、焼いた岩を撒いた集合体）

- [岩の集合体（数十 m の大岩が重なり、地面があり、小石が散る）](rock-cluster/README.md)（△）
