# LLM による岩グラフの作成（AI フレンドリー化）

作成日時: 2026-10-01 02:55
更新日時: 2026-10-03 10:41

## 目的

LLM（Claude Code などアプリの外で動くエージェント）に「片理っぽい岩」「板状節理の岩」のような依頼をして、岩グラフ（`.rockgraph`）を組ませ、検証し、調整させられるようにする。2026-10-01 のユーザー判断で、**外部の LLM がファイルを書き、CLI で検証するルート（案 A）** を整える。運用は VSCode から Claude Code または Codex に指示を出して処理させる形とし、アプリ内の対話パネル（案 B）は作らない（同日のユーザー判断）。手引きや skill は特定のエージェントに寄せすぎず、Codex からも同じ手順で使えるようにする。

あわせて、アプリ全体を **AI フレンドリーな仕様** へ寄せる。ここでの AI フレンドリーとは、LLM が推測や試行錯誤なしに、正しいグラフを書き、結果を数値と画像で確かめ、失敗の原因を特定できることを指す。人が UI で操作しやすいことと両立させ、どちらかのために他方を崩さない。

## 出発点: 実際に作って引っかかった点

2026-10-01 に、LLM（Claude Code）が `rock_flat_2` を参考に片理っぽい岩（`data/Rock-Models/rock_schist.rockgraph`）を Python でグラフとして生成した。そのときの問題を、この計画の要件の出所として残す。

| # | 問題 | 影響 | 対応する段階 |
| --- | --- | --- | --- |
| 1 | UI なしで評価できない。確認のたびに GPU アプリを起動して撮影（1回約1分）。評価の完了が分からず、待つフレーム数を勘で決めた | 試行錯誤の大半の時間 | L1 |
| 2 | 評価エラー（To Volume の解像度 16〜128 の範囲外など）が UI にしか出ず、ログにも終了コードにも出ない | 失敗に気付けない | L1 |
| 3 | 値の範囲・単位・既定値がファイルからもコードの 1 か所からも分からない（`volumeCrack.depth` はメートルか割合か、など） | 範囲外で失敗、意味の取り違え | L2 / L3 |
| 4 | ピンが番号の並びだけで決まる。Volume Crack の 3 本目が Planes、Material Bake の 2 本目が未使用、などはソースを読まないと分からない | 誤接続 | L2 / L3 |
| 5 | 列挙が数値（Piece Select の `mode: 6` が Peel）。モードと無関係な項目も全部書く必要がある | 読めない・書けない | L3 |
| 6 | 知らないノード種類・壊れたリンクを読み込みで黙って捨てる | 誤りが結果の違いとしてしか現れない | L1 |
| 7 | グラフとシーン設定（マテリアル表・テクスチャ・プレビュー）が 1 ファイルに混在し、マテリアルはシーン内の表の番号で参照する | 最小のグラフを書きにくい | L3 |
| 8 | Volume Crack の直後に Volume Edge Wear を繋ぐと、板 1 枚のような形になった（Volume Close を挟むと正常）。L1 の CLI で調べると、片理の細溝で岩が 16 個の塊に分かれ、Edge Wear が仕様どおり最大の塊（全体の 30%）だけを残していた。途中で塊が分かれたことも、捨てたことも、どこにも出ない | 原因の特定に時間がかかる | L1（塊の数を出す）。捨てる挙動の見直しは別件 |
| 9 | メッシュの連結成分数は閉じた空洞も数える（To Volume 直後で「2,678」だが塊は 1 つ） | 数値の読み違い | L1（塊と空洞を分けて出す） |

## 方針

- **正本は 1 か所。** ノードのピン名・型・既定値・範囲は、コード（定義テーブルと保存処理）から CLI で書き出す。LLM 向けの資料を手で書き写さない。手書きは「どう組むか」（レシピと考え方）に限る。
- **評価は GPU なしで回る部分を先に。** 形（Mesh / Volume / Pieces）は CPU で評価できる。Material Bake・Displace のハイト・画像の撮影は GPU が要るので後の段階にする。
- **失敗は機械可読に。** CLI は JSON を出し、終了コードで成否を返す。エラーにはノード ID と種類を付ける。
- **ファイル形式は後方互換で広げる。** 既存の `.rockgraph` は読めるまま、書きやすい表記（列挙名、ピン名での接続、省略時の既定値）を読めるようにする。

## 段階

| 段階 | 中身 | 完了条件 |
| --- | --- | --- |
| **L1 評価 CLI**（2026-10-01 実装） | グラフの読み書きを GPU から切り離す（`io/GraphIo`）。コンソールの `rock_cli` を追加し、`eval` でグラフを評価して JSON を出す（成否、エラーのノード、各出力のメッシュ数・三角形数・頂点数・範囲・閉じているか、評価時間）。読み込みで捨てたノード・リンクを診断として返す | `rock_schist` を `rock_cli eval` で数秒〜十数秒で評価でき、範囲外の値・知らない種類・壊れたリンクが JSON と終了コードに出る |
| **L2 ノードカタログ**（2026-10-01 実装） | `rock_cli catalog` で全ノードの保存名・表示名・ピン（向き・型・ラベル・番号）・既定の設定（保存処理で書いた JSON）を出す。範囲・単位・列挙の候補を設定の定義へ寄せ、カタログに載せる | LLM がカタログだけを見て、範囲内の値と正しいピンでグラフを書ける |
| **L3 書きやすい表記**（2026-10-01 実装） | 列挙を名前で書ける、リンクを `ノードID:ピン名` で書ける、省略した設定は既定値、ピン ID は省略可（読み込み時に振る）。マテリアルをパスで直接参照できる | 最小の岩グラフ（数ノード）が手書きの短い JSON で書け、アプリでそのまま開ける |
| **L4 レシピと手引き**（2026-10-01 実装） | 岩の種類ごとの最小構成（片理・板状節理・塊状・丸い転石など）を `examples/` に置き、LLM 向けの手引き（組み方の考え方、よくある失敗、確認手順）を書く。Claude Code から呼べる手順（skill）にまとめる | 「〇〇な岩を作って」と依頼すると、手引きに従って組み、CLI で確かめて返せる |
| **L5 見た目の確認**（2026-10-01 実装） | 評価の完了を待って撮影する起動引数、定まった複数の視点での撮影、UI なしの撮影。可能なら CLI から GPU の評価（Material Bake）と撮影まで | 形と見た目を LLM が画像で確かめられ、フレーム数を推測しなくてよい |

案 B（アプリ内の対話パネル）は作らない（2026-10-01 のユーザー判断）。

## L1 の設計

### グラフの読み書きの分離

`io/ProjectIo.cpp` の中にある `WriteGraph` / `ReadGraph` と、それが使う JSON の読み書き（レイヤー・ノイズ・列挙の変換）を `io/GraphIo.{h,cpp}` へ移す。`MaterialLayer` などのデータ構造は GPU に依存しないので、マテリアル・テクスチャ・モデルの参照は今と同じく関数で受ける。`ProjectIo` はこれを呼ぶだけにし、保存形式は変えない。

`ReadGraph` は、読めなかったもの（知らない種類、ID のないノード、繋がらないリンク）を診断の一覧として返せるようにする。アプリはこれまでどおり捨てて開き、CLI はそれを報告する。

### `rock_cli`

GPU とウィンドウを使わないコンソールの実行ファイル。アプリと同じ評価器（`graph::EvaluateRocks`）を呼ぶ。

```text
rock_cli eval <graph.rockgraph> [--node <id>] [--pretty]
```

- 既定では Mesh Output を評価する。`--node` で途中のノードを評価する（アプリの「プレビュー」と同じ）。
- 標準出力に JSON を 1 つ書く。終了コードは 0 = 成功、1 = 評価エラー、2 = 読み込めない・引数の誤り。
- マテリアル・テクスチャ・モデルの参照は番号のまま扱う（形の評価には使わない）。GPU が要るノード（Material Bake の焼き込み、Displace のハイト）は評価できない旨を警告に出す。

- 読み込みで捨てたノード・リンクがあれば（`diagnostics`）、評価できても `ok: false`・終了コード 1 にする。書いたグラフと評価したグラフが違うため。
- 評価器のエラーは「種類 #ID: 内容」の文字列なので、ID を取り出して `error.node` / `error.kind` に入れる。
- 上流に GPU が要るノード（今は Displace）があれば `requiresGpu` に並べる。

出力（`rock_cli eval data/Rock-Models/rock_schist.rockgraph --node 53`、数値は丸めた）:

```json
{
  "ok": true,
  "file": "data/Rock-Models/rock_schist.rockgraph",
  "target": {"node": 53, "kind": "volumeToMesh"},
  "elapsedMs": 749,
  "diagnostics": [],
  "meshes": [
    {"source": 53, "kind": "volumeToMesh", "triangles": 137246, "vertices": 68545, "hasUv": false,
     "valid": true, "closed": true, "volume": 3.656,
     "bounds": {"min": [-1.238, 0.0, -1.156], "max": [1.119, 1.841, 1.160]}, "size": [2.356, 1.841, 2.315],
     "shells": {"solids": 1, "largestSolidVolumes": [3.656], "largestSolidShare": 1.0,
                "cavities": 0, "cavityVolume": 0.0}}
  ]
}
```

`shells` は連結成分を符号付き体積で分けたもの。正が塊（外向きの殻）、負が閉じた空洞。`largestSolidShare` が 1 より小さければ、岩が複数の塊に分かれている。

失敗の例（To Volume の解像度を 160 にした）:

```json
{"ok": false, "target": {"node": 53, "kind": "volumeToMesh"}, "diagnostics": [],
 "error": {"message": "To Volume #27: ボリュームの解像度は16〜128にしてください", "node": 27, "kind": "toVolume"}}
```

読み込みの診断の例:

```json
"diagnostics": [
  {"node": 900, "message": "知らない種類のノードを捨てました: \"volumeBlur\""},
  {"node": 42, "link": 950, "message": "リンクを捨てました: toVolume#27 の出力「Volume」 を volumeCrack#42 の入力「Points」 へ繋げません（型が合わないか、循環になります）"}
]
```

### `rock_cli check`

評価せずに読み込みの診断だけを返す（一瞬で終わる）。LLM がグラフを書いた直後に使う。診断は `eval` と同じ（下の L2 の範囲・キーの診断を含む）。

### `rock_cli catalog`

全ノードについて、保存名（`kind`）・表示名・入出力のピン（番号・ラベル・型）・既定の設定・概要（`purpose` / `notes`）・項目表（`params`）を出す。既定の設定は、既定値のノードを保存処理（`WriteGraph`）で書いた JSON から取るので、保存形式と食い違わない。

## L2 の設計（2026-10-01 実装）

### 項目表 `graph/NodeParams`

ノードの設定の項目ごとに、JSON のパス（`volumeCrack.width` など）・型・範囲・単位・UI の表示名・意味・範囲外の扱い・列挙の候補・内部用かを持つ表。種類ごとの概要（何をするか・落とし穴）も持つ。GPU・UI に依存しない。

- **範囲の正本は評価のコードのまま**（各ノードの処理の中の「〜にしてください」のエラー）。表はそれを写したもので、ずれは `tests/NodeParamsTests.cpp` が検出する。
  - 表のパスがすべて保存形式にあり、保存するキーがすべて表にあり、既定値が範囲内で、列挙の既定値が候補にある。
  - 範囲外がエラーの項目（`outOfRange: "error"`）は、実際にノードを繋いで評価し、端の値で通り、外側の値でそのノードが失敗する（79 項目、約 2 秒）。範囲がほかの項目で決まるもの（Piece Select の fraction は mode が random / rim / peel のときだけ、など）と、端の値が重すぎるものは、テストの `EdgeRules` に理由つきで書く。
- 範囲外の扱いは 3 種類。`error`（評価でそのノードが失敗）、`clamp`（読み込みか評価で黙って丸める）、`none`（確かめない）。
- 表を作って分かったこと:
  - Decimate の目標の三角形数は、処理はエラーにするが評価器が先に丸めるので、実際は `clamp`。
  - Volume Close の width は width モードのときだけ、Plane Cuts の radius は global でも確かめる、など、モードで確かめる項目が変わる。
  - 最大の塊だけを残すのは Plane Cuts / Volume Noise / Volume Smooth / Volume Edge Wear / Volume Terrace だった。2026-10-01 に、入力の時点で分かれていた大きな塊は残すように直した（小片の除去の規則は [Plane Cuts](plane-cuts.md#浮いた小片の除去)）。Volume Crack と Volume Clip はもとから分かれた塊を残す。
  - UI のスライダーは範囲を描くたびに丸める。いくつかのノード（Subdivide、Displace、UV Unwrap、Apply Material、Material Mask、Material Bake、Surface、ピース系）は設定を直接編集するので、手書きで UI の範囲外（評価の範囲内）の値を書いても、ノードを選んだだけで UI の範囲へ丸められる。
  - 型が違う値（数値の所に文字列など）は黙って既定値になる（未対応。L3 で扱う）。

### 読み込みの診断の追加

- **知らない設定のキー**（`toVolume.resolutoin` など、入れ子も）。既定値のノードを保存処理で書いたキーと比べる。条件つきで書くキー（`note`、Material Bake の焼いた結果、Model の `nodeRotations`）は除く。`io::ReadGraph` が返す（アプリの読み込みの動作は変えない）。
- **範囲外の値・候補に無い列挙**（`rock_cli` が項目表と照らす）。`clamp` の項目は「丸めて使われます」と添える。
- 既存のサンプル 35 件と手元の岩グラフで誤検出が無いことを確かめた。

### 実装

- `src/io/GraphIo.{h,cpp}`、`src/io/JsonUtil.h`、`src/cli/Main.cpp`。CMake のターゲットは `rock_cli`（GPU・ImGui にリンクしない）。
- テストは `tests/GraphIoTests.cpp`（保存往復、捨てたノード・リンク・知らないキーの報告）と `tests/NodeParamsTests.cpp`（項目表と保存形式・評価の一致）。`rock_editor_tests --only NodeParams` で後者だけを走らせる（2026-10-03 に `--only <群名>[,<群名>...]` と `--list` を足した。旧 `--node-params-only` も使える）。

## L3 の設計（2026-10-01 実装）— 書きやすい表記

`.rockgraph` の読み込みに、手で書きやすい表記を足した（別の書式は作らない。アプリがそのまま開き、保存すると従来の形で書く）。従来の表記もそのまま読める。

### 最小の岩グラフ

```json
{
  "format": "rock-editor.scene",
  "version": 1,
  "graph": {
    "nodes": [
      {"id": 1, "kind": "randomBoxes", "randomBoxes": {"count": 10, "seed": 7}},
      {"id": 2, "kind": "toVolume", "toVolume": {"resolution": 96}},
      {"id": 3, "kind": "volumeEdgeWear", "volumeEdgeWear": {"amount": 0.04}},
      {"id": 4, "kind": "volumeToMesh", "volumeToMesh": {"method": "dualContouring"}},
      {"id": 5, "kind": "surface", "layer": {"material": "Materials/rough-rock.rockmat",
                                             "mapping": {"method": "triplanar", "repeatMeters": 2}}},
      {"id": 6, "kind": "meshOutput"}
    ],
    "links": [
      {"from": "1", "to": "2"},
      {"from": "2", "to": "3"},
      {"from": "3", "to": "4"},
      {"from": "4", "to": "6:Geometry"},
      {"from": "5", "to": "6:Material"}
    ]
  }
}
```

- **ヘッダ**: `format` と `version` と `graph` だけでよい。マテリアル・テクスチャ・天球の表、プレビューの設定は省ける。
- **ノード**: `id`（1 以上の整数、グラフの中で重ならない）と `kind`（`rock_cli catalog` の保存名）だけでよい。設定は変えたい項目だけ書き、残りは既定値。`inputs` / `outputs`（ピンの ID）と `position` は省ける。位置の無いノードは、上流からの深さで左から右へ並べる。
- **リンク**: `{"from": 出力側, "to": 入力側}`。端は `"12"`（0 番のピン）、`"12:Planes"`（ピンの名前。大文字小文字は区別しない）、`"12:2"`（ピンの番号）、`{"node": 12, "pin": "Planes"}` のどれでも書ける。ピンの名前と番号は `rock_cli catalog` の `inputs` / `outputs`。リンクの `id` は省ける。
- **数値の列挙**を名前で書ける（Piece Select の `"mode": "peel"`、`"rimSide": "top"` など。候補は catalog の `options`）。保存すると数値に戻る。
- **マテリアル**: Surface の `layer.material` にプロジェクトのルートからのパス（`"Materials/rough-rock.rockmat"`）を直接書ける。開くときに表へ足して番号に置き換える（同じパスは 1 つにまとめる）。表を自分で書く場合も `{"id": 1, "asset": {"path": "..."}}` だけでよい（uid は保存時に補う）。
- **置き場所**: アプリで開くには、プロジェクトのルート（`project.reproj` のあるフォルダ。手元では `data/`）の中に置く。

### 診断の追加

- 名前で書いたリンクの、無いノード・無いピン（候補つき）・候補に無い列挙の名前（候補つき）。
- 型の違う値（`"resolution": "128"` など。読み込みでは黙って既定値になる）。`rock_cli` が項目表の型と照らす。
- マテリアルのパスが実在するか（`rock_cli` がグラフのファイルから上へ `project.reproj` を探してルートを決める。無ければグラフのフォルダ）。

### 実装

- `io::ReadGraph`: 名前のリンクの解決（`ResolveEnd`）、数値の列挙の名前（項目表の `IntEnum` の候補から写す）、位置の無いノードの配置（`LayoutUnplacedNodes`）。ピン・リンクの ID は既存の「欠けた ID を最大 + 1 から振る」処理を使う。
- `io::ProjectWorkspace::Expand`: Surface の material のパスを表の番号へ置き換える。
- テストは `tests/GraphIoTests.cpp`（書きやすい表記一式）と `tests/ProjectWorkspaceTests.cpp`（マテリアルのパス）。Debug のアプリで手書きの最小グラフを開いて保存し、配置・列挙・マテリアルの解決と、保存後も CLI で同じ形になることを確かめた。

## L4 の設計（2026-10-01 実装）— レシピと手引き

- **レシピ** `examples/ai-recipes/`: 片理（`schist`）・塊状（`blocky`）・板状節理（`platy-joints`）・丸い転石（`rounded-boulder`）・層理の段（`layered-ledges`）。どれも L3 の手書きの表記で書き、`note` に組み方の意図を書いた。`rock_cli eval` で塊 1 つ・空洞 0・閉じたメッシュ、評価 0.1〜0.7 秒。形だけでマテリアルは付けていない。
- **手引き** `.agents/skills/rock-graph/SKILL.md`（skill。Codex はここを読み、Claude Code は `.claude/skills/rock-graph/SKILL.md` の入口から読む。AGENTS.md からも案内する）: 準備 → レシピを選ぶ → 書く → `rock_cli check` / `eval` で数値 → `tools/rock_shot.py` で見た目 → 報告、の手順と、書式の要約、落とし穴。範囲や意味は `rock_cli catalog` を正本とし、手引きには書き写さない。
- レシピを作って分かったこと（手引きの落とし穴に入れた）:
  - Random Boxes・Base Shape は原点中心なので、Volume Clip の前に持ち上げないと半分が消えて平たくなる（最初の塊状・転石はこれで失敗した）。
  - Base Shape の Box から始めると、節理を彫っても直方体が透けて見える（板状節理はレンガ積みのようになった）。不規則な塊から始める方が岩らしい。
  - 見た目の出来は数値（塊 1 つ・閉じている）では分からない。撮影して見ないと、平たい・ただのドーム形、といった失敗に気付けない。

## L5 の設計（2026-10-01 実装）— 見た目の確認

- アプリの撮影（`--screenshot` / `--screenshot-ui`）は、素材の評価に加えて**グラフの評価（別スレッド）の完了も待つ**ようにした。これまでは待つフレーム数を推測する必要があり、途中や前の形が写ることがあった。
- 起動引数 `--frame-all`: グラフの評価が終わるたびに、形全体が入るようにカメラを引く（キーの A と同じ。グリッドは含めない）。
- `tools/rock_shot.py <graph> <out.png> [--node <id>] [--ui]`: ルートの特定（`project.reproj` を上へ探す）、設定の隔離（一時フォルダ）、`--frame-all` と撮影をまとめる。1 本 1〜数秒（最初は 1 回約 1 分かかっていた）。
- 起動引数 `--camera-yaw <度>` / `--camera-pitch <度>`（読み込んだ視点より優先）と `--light-azimuth <度>`（作業用ライトの方位）。
- 書きやすい表記の可変の入力（2026-10-03）: Rock Scatter の `"to": "5:Rock 2"`、Merge の `"8:Input 2"` のように、名前の番号の分だけ入力を足してつなぐ（番号指定 `"8:1"` も同じ）。それまでは定義にある 1 本目にしかつなげず、2 本目以降のリンクを捨てていた。
- `rock_cli eval` の `rockInstanceSets`（山グラフ）に、撒いたノード・岩グラフごとの数・推定の被覆率・置いた位置の範囲（8 個以下なら位置の列挙）が出る（2026-10-03）。大岩が地形の縁に落ちていないかを数値で確かめられる。
- `tools/rock_bake.py <graph.rockgraph>`（2026-10-03）: 岩アセットを焼く。山グラフの Rock ノードが読む付属フォルダを、アプリの `--bake-asset` で対話せずに作る。`rock_cli eval` の `rockInstanceSets` で山グラフの段ごとの数と推定の被覆率が読める。
- `rock_shot.py` の光と露出（2026-10-03）: `--lighting ibl|atmospheric`（作業用 IBL / シーンの空）、`--light-azimuth` / `--light-elevation`（度）、`--light-illuminance`（lux）、`--exposure`（EV。自動露出なら補正、手動・物理カメラなら今の EV に足す。正で暗く、負で明るく）、`--skylight`（シーンの空の環境光の倍率）。写真と比べるときは `--lighting atmospheric --light-elevation 55` のように昼の直射日光にする。アプリ側は同名の起動引数で、今の方式の光（`PreviewRenderer::Light()`）を変える。
- `rock_shot.py --views 4`: 90 度ずつ回した 4 方向を 2×2 の 1 枚にまとめる。光はカメラの少し横から当てるので、裏側も影で潰れない（固定の光では裏側 2 枚が真っ黒だった）。`--yaw` / `--pitch` で 1 方向を指定できる。
- 照明の既定: プレビューの設定が無いグラフは作業用ライトの暗い背景になる。形は読めるので既定のままにした（大気の照明を当てると明るすぎて陰影が飛んだ）。
- 4 方向で撮ると、正面からは見えない失敗（片理のレシピの裏側のボクセルの段差など）に気付ける。
- 残り: GPU の評価（Material Bake）を CLI から行うか。

## 未決事項

- 保存するときも列挙を名前で書くか（今は読み込みだけ。古いビルドで開けなくなるので保留）。
- 範囲の正本を評価のコードから項目表へ移すか（今は写しとテストで一致を保つ）。移すと、評価のエラーと UI のスライダーも表から作れる。
- L5 の GPU を使う評価（Material Bake）を `rock_cli` に入れるか、アプリの起動引数で済ませるか（今は撮影だけアプリの起動引数で行う）。
