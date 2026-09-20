# Rock Editor

岩をノードグラフで生成する、Windows 向けのプロシージャルオーサリングツール。
プロジェクト名は **rock-editor**、実行ファイルは **rock_editor.exe**。

母岩 → 節理 → 亀裂 → 部分破断 → 完全破断 → 岩塊 → 風化 → 表面、という岩の構造を意識した階層的生成を目指す。
仕様は [docs/rock_generator_spec_v2.md](docs/rock_generator_spec_v2.md) を出発点とする。

## 現在の状態

road-editor（旧 terrain-graph）のアプリ基盤を土台に、道路・地形に特化した実装を撤去した段階。
**Base Rock の Box / RoundedBox / Sphere / Ellipsoid 生成に対応**。ノードグラフの右クリックから Base Rock と Mesh Output を追加し、
Mesh ピンを接続すると母岩を表示する。寸法 X/Y/Z と Seed を編集でき、Undo・保存／読み込みに対応する。
母岩は原点中心。Sphere は直径、それ以外は各軸の寸法を指定する。
RoundedBox の丸み、曲面/ノイズ用の面分割数（4/8/16/32）、弱い形状ノイズの強度・細かさ・Seed を編集できる。
ノイズ強度は0～0.15（原点からの距離に対する最大変位率）。同じ設定と Seed で再現する。
ノイズが0なら Seed は形状に影響せず、従来の Box シーンの形も変わらない。
`Base Rock → Crack → Mesh Output` で有限亀裂の候補範囲（青）・到達範囲（橙）を透視ガイド表示できる。
Crack の中心・回転・半幅 U/V・Depth・Persistence・Aperture は設定パネルから編集する。
Crack の「部分切断 (Box)」をオンにすると、Box に亀裂壁・終端のある有限切り込みを作り、
奥に Rock Bridge（未破断部）を残す。現在は未加工の Box 1個に対する、軸に沿った1回の切り込みに対応する。曲面やノイズ付きの母岩への部分切断は未対応。
回転は90度単位。貫通して Bridge が消える設定は診断する。任意角度はガイド表示のみ。
「Bridge 表示」で未破断断面を緑で示し、実切込深さ・残存厚さ・断面積を確認できる。
`Base Rock → (Crack →) Fracture → Mesh Output` で、岩を平面の両側の2片へ完全分割できる。
部分亀裂の形を保ったまま残存断面を閉じる。Fracture の中心・回転は独立した無限平面で、Crack の深さを自動延長しない。
設定欄またはビューポートで Chunk を選び、Locked を解除すると数値欄と W（移動）/ E（回転）のギズモで操作できる。
Locked は現在の配置を固定する編集フラグで、物理計算はしない。設定は保存・Undo に対応する。
曲面・ノイズ付き母岩も Fracture へ接続できる。初期実装は未分割の岩1個、単純な断面ループ1本、分割後の各側が1連結体の場合。穴のある断面・再帰分割・Joint Set は未対応。
目指す外観と参考写真は [外観目標](docs/reference/visual-target.md) を参照。

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
