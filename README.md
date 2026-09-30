# Rock Editor

岩をノードグラフで生成する、Windows 向けのプロシージャルオーサリングツール。
プロジェクト名は **rock-editor**、実行ファイルは **rock_editor.exe**。

母岩 → 節理 → 亀裂 → 部分破断 → 完全破断 → 岩塊 → 風化 → 表面、という岩の構造を意識した階層的生成を目指す。
仕様は [docs/rock_generator_spec_v2.md](docs/rock_generator_spec_v2.md) を出発点とする。

## 直方体から塊を作る

`Random Boxes → To Volume → Volume to Mesh → Mesh Output` で、重なる直方体の塊を作り、ボリュームへ変換して表示できる。
右クリックメニューから追加し、個数・大きさ・ばらつき・回転・Seed と、ボリュームの解像度を調整する。
[使い方と制限](docs/reference/box-volume.md) / [サンプルシーン](examples/random-boxes/random-boxes.rockgraph)。

## 現在の状態

road-editor（旧 terrain-graph）のアプリ基盤を土台に、岩の形をボリュームとピースで試す段階。
2026-09-21 に旧 Crack / Fracture / Joint Set ノードは撤去した（[目的と完成形](docs/plan/goals.md)）。
ノードはグラフの右クリックから追加する。各ノードの仕様は [docs/reference/](docs/reference/) にある。

- **形の元**: Base Shape（Box / RoundedBox / Sphere / Ellipsoid）、Random Boxes、Layered Boxes、Model、Merge、Transform
- **ボリューム**: To Volume、Volume Transform / Boolean / Noise / Smooth / Terrace / Close / Edge Wear / Clip、Plane Cuts、Volume Crack、Parallel Planes、Volume to Mesh
- **ピース**: Scatter Points、Voronoi Fracture、Piece Select / Filter / Transform、Pieces to Mesh
- **メッシュ**: Subdivide、Displace、Decimate、Remesh、UV Unwrap
- **表面**: Surface、Apply Material、Material Mask、Shape Mask、Noise Mask、Deposition Mask、Mask Combine、Mask Filter、Material Bake
- **出力**: Mesh Output

目指す外観と参考写真は [外観目標](docs/reference/visual-target.md) を参照。

土台として動いているもの:

- DX12 + Dear ImGui のウィンドウ、ドックレイアウト、ビューポートと軌道カメラ
- ノードグラフのコピー／貼り付け（別のファイルへの貼り付けを含む）、アンドゥ、ノードのメモ、グラフの保存と読み込み
- PBR マテリアル合成、レイヤーマテリアル、テクスチャ、天球（HDRI / 手続き的な空）、直接光・IBL・露出・トーンマップ
- FBX モデルの取り込みと配置、ギズモ（移動 / 回転 / 倍率）
- ルートフォルダによるアセット管理とアセットブラウザ

## 作業グリッド

1 unit = 1 m。原点中心の XZ 平面（Y=0）に 50 m × 50 m のグリッドを表示する。
1 m 間隔で、5 m ごとの線と中心線を強調する。ビューポートの「表示」→「グリッド」から
オン・オフでき、状態はアプリ設定に保存される。グリッド表示中は A キーで範囲全体を見渡せる。

## 開発環境とビルド

C++20 / CMake / vcpkg manifest / DirectX 12 / HLSL SM 6.6 / Dear ImGui docking。
構成プリセットは Visual Studio 18 2026 を使用する。vcpkg は VCPKG_ROOT、未設定なら C:/vcpkg。
GPU は Resource Binding Tier 3 と SM 6.6 が必要。
リポジトリのルートで実行する。

```powershell
cmake --preset x64
cmake --build --preset x64-debug
ctest --test-dir build -C Debug --output-on-failure
```

Release は `cmake --build --preset x64-release`。
別の場所からコピーした build のキャッシュが残っている場合は `cmake --fresh --preset x64` で再構成する。

```powershell
$exe = Join-Path $PWD 'build/bin/Debug/rock_editor.exe'
& $exe
```

シェーダは構成時のソースツリーを参照する。環境変数 ROCK_SHADER_DIR で変更できる。
アプリ設定と最近使ったファイルは `%LOCALAPPDATA%/rock-editor/`、
レイアウトは作業ディレクトリの `rock_editor_imgui.ini` に保存する。

## 保存形式

プロジェクトはルートフォルダで管理する（「ファイル > ルートフォルダを開く…」）。
ルート直下に目印の `project.reproj` ができ、シーンは `.rockgraph`、
マテリアル / 天球 / モデルはルート内の `.rockmat` / `.rocksky` / `.model` として共有する。
JSON の形式識別子は `rock-editor.*`。

road-editor 時代の `.tgproj` / `.tgscene` / `.tgmat` などの読み込み互換は持たない。

## 開発用コマンドライン

```text
rock_editor.exe [--root <dir>] [--project <path>] [--save-project <path>]
                [--hdri <path>] [--texture <path>]...
                [--import-model <path>] [--place-model <path>] [--open-asset <path>]
                [--select-node <id>] [--focus-panel <name>]
                [--screenshot <path>] [--screenshot-ui <path>]
                [--screenshot-frame <n>]
                [--test-gpu-ao] [--test-copy-to <scene>]
```

`--root` はプロジェクトのルートフォルダ（省略時は最近使ったルート、無ければ `data/`）。
`--project` にはシーンかルートのフォルダを渡せる。
`--save-project` は指定フレーム後に保存して終了する。
`--screenshot` はビュー、`--screenshot-ui` は UI を含む PNG を出力して終了する。
`--test-gpu-ao` は GPU の AO ベイクを CPU 版と比べ、結果を終了コードで返す（CTest には入れていない。GPU が要るため）。
`--test-copy-to` は先頭以外のノードをコピーして指定シーンへ貼る（`--save-project` と併用）。
検証素材・プロジェクト・スクリーンショットは Git 対象外の `data/` に置く。

### rock_cli（UI なしでグラフを評価する）

LLM などアプリの外で動く道具が、岩グラフを書いて結果を確かめるためのコンソールの実行ファイル（GPU・ウィンドウは使わない）。

```text
rock_cli eval <graph.rockgraph> [--node <id>] [--mesher dc|mt] [--pretty]
rock_cli check <graph.rockgraph> [--pretty]
rock_cli catalog [--pretty]
```

`eval` はグラフを評価し、標準出力へ JSON を書く（成否、エラーのノード、読み込みで捨てたノード・リンク、メッシュの三角形数・寸法・閉じているか・塊と空洞の数と体積）。
`--node` で途中のノードを評価する。GPU が要るノード（Displace など）の下流は評価できないので、その手前を指定する。
`check` は評価せずに読み込みの診断（捨てたノード・リンク、知らない設定のキー、範囲外の値、候補に無い列挙）だけを返す。
`catalog` は全ノードの保存名・ピン・既定の設定・概要と、項目ごとの範囲・単位・列挙の候補・意味を出す。
終了コードは 0 = 成功、1 = 評価エラーか読み込みで捨てたものがある、2 = 読み込めない・引数の誤り。
グラフは手で書きやすい表記でも書ける（ピン ID・位置を省き、リンクを `{"from": "3", "to": "6:Geometry"}` のようにノード ID とピン名で書く、列挙を名前で書く、マテリアルをパスで書く）。
仕様は [LLM による岩グラフの作成](docs/reference/ai-authoring.md)。

## ドキュメント

- [ドキュメント案内・設計と検証資料](docs/README.md)

- [岩生成エディタの仕様](docs/rock_generator_spec_v2.md)
- [目的と完成形](docs/plan/goals.md)
- [実装計画](docs/plan/plan.md)
- [進捗と検証状況](docs/plan/progress.md)
- [変更履歴](docs/changelog.md)
