# Rock Editor

岩をノードグラフで生成する、Windows 向けのプロシージャルオーサリングツール。
プロジェクト名は **rock-editor**、実行ファイルは **rock_editor.exe**。

母岩 → 節理 → 亀裂 → 部分破断 → 完全破断 → 岩塊 → 風化 → 表面、という岩の構造を意識した階層的生成を目指す。
仕様は [docs/rock_generator_spec_v2.md](docs/rock_generator_spec_v2.md) を出発点とする。

## 現在の状態

road-editor（旧 terrain-graph）のアプリ基盤を土台に、道路・地形に特化した実装を撤去した段階。
**Base Rock の Box 生成に対応**。ノードグラフの右クリックから Base Rock と Mesh Output を追加し、
Mesh ピンを接続すると母岩を表示する。寸法 X/Y/Z と Seed を編集でき、Undo・保存／読み込みに対応する。
Box は原点中心で、Seed は形状に影響しない。
`Base Rock → Crack → Mesh Output` で有限亀裂の候補範囲（青）・到達範囲（橙）を透視ガイド表示できる。
Crack の中心・回転・半幅 U/V・Depth・Persistence・Aperture は設定パネルから編集する。
現段階ではガイド表示のみで、母岩は切断しない。実際の部分亀裂・Joint Set・Fracture・Chunk 編集は未実装。

土台として動いているもの:

- DX12 + Dear ImGui のウィンドウ、ドックレイアウト、ビューポートと軌道カメラ
- ノードグラフ（Base Rock / Surface / Model / Transform / Merge / Mesh Output）と、
  コピー／貼り付け、アンドゥ、グラフの保存と読み込み
- PBR マテリアル合成、テクスチャ、天球（HDRI / 手続き的な空）、直接光・IBL・露出・トーンマップ
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
ルート直下に目印の `project.reproj` ができ、シーンは `.rockscene`、
マテリアル / 天球 / モデルはルート内の `.rockmat` / `.rocksky` / `.rockmodel` として共有する。
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
```

`--root` はプロジェクトのルートフォルダ（省略時は最近使ったルート、無ければ `data/`）。
`--project` にはシーンかルートのフォルダを渡せる。
`--save-project` は指定フレーム後に保存して終了する。
`--screenshot` はビュー、`--screenshot-ui` は UI を含む PNG を出力して終了する。
検証素材・プロジェクト・スクリーンショットは Git 対象外の `data/` に置く。

## ドキュメント

- [ドキュメント案内・設計と検証資料](docs/README.md)

- [岩生成エディタの仕様](docs/rock_generator_spec_v2.md)
- [目的と完成形](docs/plan/goals.md)
- [実装計画](docs/plan/plan.md)
- [進捗と検証状況](docs/plan/progress.md)
- [変更履歴](docs/changelog.md)
