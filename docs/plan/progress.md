# progress — 進捗と注意点

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 22:20

## 現在地

P0 の基盤確認と P1 の Base Rock（Box）を実装した。次は [計画](plan.md) の P2：有限亀裂パッチの可視化。

| 区分 | 現在の状態 |
| --- | --- |
| アプリ基盤 | DX12 / ImGui、モデル表示、グラフ編集、素材・アセット・保存基盤あり |
| 岩生成 | Base Rock の Box、寸法 X/Y/Z、Seed の編集に対応。亀裂・破断は未実装 |
| メッシュ接続 | RockEvaluator → RockMesh → SyncMeshGraph で表示。Merge と途中プレビューに対応 |
| 評価・保存 | Revision ごとの再評価、寸法と seed の保存/復元、Undo/Redo に対応。枝単位キャッシュは P6 |
| 次の作業 | P2：中心・向き・有限範囲・深さ・persistence を持つパッチと半透明表示 |

## 完了

### 2026-09-20 Base Rock の Box（P0/P1）

- `geometry/Mesh` に共有頂点の CPU メッシュ、Box、面法線、AABB・体積・閉包・連結性の検証を追加。
- Base Rock を右クリックメニューに追加。原点中心の Box を Mesh Output / Merge / 途中プレビューから表示する。
- 寸法は各軸 0.001～1000 m。Seed は保存するが Box 形状には影響しない。寸法不正時はノード ID を含む診断を出す。
- `ProjectIo` に設定の保存/読込を追加し、既存 Undo/Redo のスナップショットに統合。
- Debug ビルド、既存と新規テストを含む CTest が成功。テストは形状・不正入力・描画変換・接続・再評価・Undo/Redo を確認。
- Release 版で 3×4×5 m、Seed 123 の Box と設定パネルを目視確認。実アプリで保存→再読み込み→再保存を行い、寸法・seed・接続の一致を確認。
- 検証資料は Git 対象外の `build/p1-validation/`（`box-ui.png`、`reloaded-ui.png`、往復保存したシーン、ログ）。既存 `data/` は変更していない。

**検証環境の補足**

- vcpkg 再実行が `C:/vcpkg/buildtrees` の書き込み権限で失敗したため、導入済み依存を利用して `cmake --preset x64 -DVCPKG_MANIFEST_INSTALL=OFF` で再構成した。リポジトリのプリセットは変更していない。
- 起動中の既存 Release アプリが通常の出力先を使用していたため、MSBuild の `OutDir` で `build/p1-release/` に検証用実行ファイルを出力した。
- Debug の GPU ベースバリデーション有効時は初回描画までの待機が長く、今回の画面確認は Release で行った。Debug の起動描画確認は未完了。


### 2026-09-20 岩をモチーフにしたアプリアイコン

- 切断面と割れ目のある岩塊を描いた透過 PNG と、16 / 24 / 32 / 48 / 64 / 128 / 256px を含む ICO を作成。
- `assets/icons/rock_editor_v2.*` に画像・生成プロンプトを保存し、`RockEditor.rc` の参照先を更新。

### 2026-09-20 rock-editor への改名と土台のクリーンアップ

road-editor（旧 terrain-graph）を岩生成エディタの土台にするための整理。

**削除したもの**（約 14,000 行）

- `src/graph/`：Road / RoadMask / RoadProfile / Path / ConnectionPrototype /
  SurfaceLayout 一式（SurfaceLayout / Editing / Evaluation / BandGeometry / PresetGraph）
- `src/io/SurfaceLayoutIo`
- `src/app/`：ApplicationPathEdit / ApplicationSurfaceLayout / ApplicationSurfacePreset /
  ApplicationBoundaryMaterial / RoadMaskUi
- `tests/`：RoadTests / SurfaceLayoutTests / ConnectionPrototypeTests
- `tools/make_connection_prototype.py`
- ノードの種類：Road / Lane Marking / Road Mask / Decal / Shoulder / Crack / Path
- 値の型：Path / RoadMask
- レイヤーマテリアル（.tglayer）と境界マテリアル（.tgboundary）の編集・アセット機能（描画用の関連型・属性には残存あり）
- 旧形式（.tgproj / .mmproj / .mmmat）の読み込み互換

**残したもの**

DX12 レンダラ（rhi / renderer）、Dear ImGui、ビューポートと軌道カメラ、
ノードグラフ（Surface / Model / Transform / Merge / Mesh Output）、
マテリアル・テクスチャ・天球・モデルのアセット基盤、
プロジェクトの保存と読み込み、アセットブラウザ、アンドゥ。

**改名**

| 対象 | 旧 | 新 |
| --- | --- | --- |
| CMake プロジェクト / ターゲット | `road_editor` | `rock_editor` |
| vcpkg マニフェスト | `road-editor` | `rock-editor` |
| リソーススクリプト | `src/app/TerrainGraph.rc` | `src/app/RockEditor.rc` |
| C++ 名前空間 | `tg::` | `rock::` |
| マクロ接頭辞 | `TG_` | `ROCK_` |
| JSON 形式識別子 | `terrain-graph.*` | `rock-editor.*` |
| 内部フォルダ | `<root>/.terrain-graph` | `<root>/.rock-editor` |
| 設定の保存先 | `%LOCALAPPDATA%/road-editor` | `%LOCALAPPDATA%/rock-editor` |
| レイアウト | `road_editor_imgui.ini` | `rock_editor_imgui.ini` |
| プロジェクトの目印 | `project.tgproj` | `project.reproj` |
| シーン | `.tgscene` | `.rockscene` |
| マテリアル | `.tgmat` | `.rockmat` |
| 天球 | `.tgsky` | `.rocksky` |
| モデル | `.tgmodel` | `.rockmodel` |

Debug ビルドとテスト（`rock_editor_tests`）が通り、アプリが起動して
ビューポート・グラフ・アセットブラウザが表示されることを確認した。

## 未完了

- P2/P3 の有限亀裂と Rock Bridge、P4 の完全分割・Chunk 操作。
- Box 以外の母岩、BaseNoise、Joint Set、枝単位キャッシュ、Chip、Triplanar、OBJ。
- P1 の Box は無地表示。Surface 接続と岩用 UV/Triplanar は未対応。既存の Transform ノードはモデル専用のまま。
- グラフからの寸法変更・Undo/Redo は自動テストで確認。実マウス操作による一連の編集操作は今回未検証。

## 注意点

- src/renderer/MeshData.h には roadUv、道路・境界材質関連属性が残る。岩用トポロジーとは分離し、残存部分の全面整理を岩生成の前提作業にはしない。

- **`data/` は改名の対象外**（依頼で「触らない」と決めたため）。
  中身は road-editor 時代のまま（`project.tgproj`、`.terrain-graph/`、
  `BoundaryMaterials/`、`LayerMaterials/`、道路テクスチャなど約 1 GB）。
  新しい rock-editor はこれらを認識しないので、ルートを開くと
  `project.reproj` を新たに作る。不要になったら手で整理する。
- 旧 `assets/icons/rock_editor.*` は道路の絵柄の資料として保持。アプリは `rock_editor_v2.ico` を使用する。
- `AGENTS.md` / `CLAUDE.md` はバージョンの基準を `package.json` と書いているが、
  このプロジェクトに `package.json` は無い。実際の基準は `CMakeLists.txt` の
  `project(rock_editor VERSION ...)`。
- 旧 road-editor の状態は Git の最初のコミット（`chore: 既存の road_editor /
  TerrainGraph 時点のスナップショット`）に残してある。
