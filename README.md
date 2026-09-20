# Road Editor

道路と道路付属物をノードグラフで構築する、Windows 向けのメッシュベースのオーサリングツール。
プロジェクト名は **road-editor**、実行ファイルは **road_editor.exe**。

terrain-graph を土台に、ハイトマップ中心の仕様を廃止して、実寸の3Dパス・道路面・メッシュへ
置き換える。[道路エディタ構想](docs/reference/road-editor-project-proposal.md)を開発の出発点とする。

## 現在の状態

改名に加え、ハイトマップに依存しない複数メッシュの PBR 表示・影・保存再読込を実装した。
**Path から一定幅の道路メッシュを生成し、道路 UV で材質を表示できる。白線は中央線と外側線を帯ポリゴンで生成する。路肩・破線・デカール・付属物は未実装**。
既存ノードはメッシュ経路の置き換えが完了したものから撤去する。

引き継いだ機能は、ノード編集、コピー／貼り付け、アンドゥ、グラフ保存、Path 編集、
PBR 合成・直接光・IBL・露出・トーンマップ、テクスチャ書き出し。
ハイトマップの加工ノードと地形用プレビューは移行中の旧実装として残っている。

## Pathの編集

新規Pathは地形から独立した実寸カーブ。ノードを選択し、ビューポートでCtrl＋クリックすると
点を追加できる。グリッド外にも配置・移動できる。X（赤）・Y（緑）・Z（青）の軸をドラッグして動かす。
Y軸で上下移動でき、ギズモ中央ではXZ平面上を移動する。XYZはプロパティの1行の入力欄で編集し、Enterで確定する。
Path選択中に空いている場所から左ドラッグするとポイントを矩形選択できる。Shiftで追加、Escで取消。
Path未選択のメッシュシーンではメッシュ単位に矩形選択する。
最初の点はY=0、選択点から延長すると同じ高さへ置く。旧形式のPathには「実寸カーブへ変換」を使う。
Road ノードへ接続すると道路メッシュを生成する（「道路メッシュの作成」を参照）。

道路線形にはプロパティの「道路線形 > 編集」から縦断ポイントとバンクポイントを置ける。縦断反映後の中心線を
Ctrl＋クリックしてポイントを追加し、線に沿ってドラッグする。バンクは「バンクを反映」を入れると、設計速度と曲率から
自動で傾く。手動にしたポイントはリングをドラッグして角度を回す。

開発時は `--project <path> --select-node <id> --screenshot-ui <path>` で選択したPathを描画確認できる。
`--profile-mode <0..2>` と `--select-path-point <id>` で縦断 / バンクの編集状態も再現できる。

## 作業グリッド

1 unit = 1 m。原点中心の XZ 平面（Y=0）に50 m × 50 mのグリッドを表示する。
1 m間隔で、5 mごとの線と中心線を強調する。ビューポートの「表示」→「グリッド」から
オン・オフでき、状態はアプリ設定に保存される。グリッド表示中は A キーで範囲全体を見渡せる。

## 目指す編集の流れ

3D中心パス → 道路面・道路メッシュ・左右境界 → 路肩・縁石・歩道・ガードレール。
同じ道路データから白線・汚れ・轍も生成し、幅やパスの変更へ追従させる。
長さ・幅・高さ・配置間隔はメートル。段差や輪郭はメッシュ、薄い表面変化は材質合成で表す。
土木設計基準の判定、交通シミュレーション、施工成果物は対象外。

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
$exe = Join-Path $PWD 'build/bin/Debug/road_editor.exe'
& $exe
```

シェーダは構成時のソースツリーを参照する。環境変数 TG_SHADER_DIR で変更できる。
アプリ設定と最近使ったファイルは `%LOCALAPPDATA%/road-editor/`、
レイアウトは作業ディレクトリの `road_editor_imgui.ini` に保存する。

## 保存と互換性

プロジェクトはルートフォルダで管理する（「ファイル > ルートフォルダを開く…」）。ルート直下に `project.tgproj` ができ、
シーンは `.tgscene`、マテリアル / 天球はルート内の `.tgmat` / `.tgsky` として共有する（[設計](docs/design/project-workspace.md)）。
旧 `.tgproj` / `.mmproj` / `.mmmat` の読み込みは維持し、保存は `.tgscene` へ行う。JSON の `terrain-graph.*` 識別子は継続する。現在の版は13。版5で scene 節の明示的なメッシュ入力、版6〜7で実寸 Path、版8〜9で Road ノードの設定と Material 入力、版10で白線ノード、版11で縦断・バンク、版12で材質スロットと Road Mask、版13で面上の Path と Decal を保存する。
生成した道路メッシュは保存せず、再読込時に再構築する。道路専用の拡張子は後続で設計する。
旧地形ノードを削除する段階では、未対応ノードを黙って捨てず、明示的な移行か読込拒否にする。

## 開発用コマンドライン

```text
road_editor.exe [--root <dir>] [--project <path>] [--save-project <path>]
                [--hdri <path>] [--texture <path>]...
                [--export <dir>]
                [--screenshot <path>] [--screenshot-ui <path>]
                [--screenshot-frame <n>]
```

`--root` はプロジェクトのルートフォルダ（省略時は最近使ったルート、無ければ `data/`）。`--project` にはシーン / 旧プロジェクト / ルートのフォルダを渡せる。
`--save-project` は指定フレーム後に保存して終了する（`.tgscene` ならルートへ分離保存、`.tgproj` なら旧形式）。
`--screenshot` はビュー、`--screenshot-ui` は UI を含む PNG を出力して終了する。
`--export` は現在の材質合成結果の画像を書き出す（メッシュ書き出しは未実装。scene 表示中はテクスチャ書き出しも未対応）。
検証素材・プロジェクト・スクリーンショットは Git 対象外の `data/` に置く。

## ドキュメント

- [目的と完成形](docs/plan/goals.md)
- [実装計画と旧ノードの撤去方針](docs/plan/plan.md)
- [進捗と検証状況](docs/plan/progress.md)
- [構想書](docs/reference/road-editor-project-proposal.md)
- [UI 設計ガイド](docs/design/design-guide.md)
- [道路材質のレイヤー構造](docs/design/road-material-layers.md)
- [保存形式](docs/reference/file-format.md)
- [変更履歴](docs/changelog.md)

検証用のメッシュシーンは `data/mesh-scene-check/scene.tgproj` を「ファイル > シーンを開く…」で確認できる。
3枚の異なる高さの面を表示する描画基盤の検証データで、道路生成の完成例ではない。

## 道路メッシュの作成

1. グラフの右クリックからPath、Road、Mesh Outputを追加する。
2. Pathの出力をRoadのPath入力へ、RoadSurfaceをMesh Outputへ接続する。
3. Pathを選択してビューポートでCtrl+クリックし、1本の開いたカーブを作る。
4. Roadの道路幅を変更する。点のXYZやカーブ形状を変えると道路も追従する。
5. SurfaceなどのResultをRoadのMaterial入力へ接続すると、道路UVで材質を表示する。未接続は単色。
6. 白線はLane Markingノードを追加し、RoadのRoadSurfaceを入力、出力のRoadSurfaceをMesh Outputへ接続する。
   中央線と左右の外側線を帯ポリゴンで生成し、線幅・端からの距離・UV反復長を設定できる。Materialに塗料の材質を接続できる。
7. 模様の帯（デカール）は、PathのSurfaceにRoadのRoadSurfaceを繋いで路面の上にパスを引き、Decalノードにその道路・Path・材質を接続する。
   Roadは材質スロットを4つ持ち、Road Maskノード（轍・端の減衰・ノイズ）で被覆率を決めてハイトで混ぜる。

ビューポートの「表示」から道路の1 mグリッドとUVチェッカーを切り替えられる。

Left / Rightは後続の部品へ渡す境界Path。路肩・縁石・歩道の専用ノードは今後追加する。分岐・閉ループ・垂直区間は未対応。
