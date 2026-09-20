# progress — 進捗と注意点

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 20:05

## 完了

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
- レイヤーマテリアル（.tglayer）と境界マテリアル（.tgboundary）の仕組み全体
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

- 岩の生成ノードは 1 つも無い。[plan.md](plan.md) の「2. メッシュ基盤」から始める。
- `SyncMeshGraph()` はメッシュを作るノードが無いため、当面シーンを空に保つだけの実装。
  Base Rock / Fracture を入れるときにここを本実装へ置き換える。
- Merge / Mesh Output は型の受け渡しだけが生きていて、メッシュを実際に積む経路は無い。

## 注意点

- **`data/` は改名の対象外**（依頼で「触らない」と決めたため）。
  中身は road-editor 時代のまま（`project.tgproj`、`.terrain-graph/`、
  `BoundaryMaterials/`、`LayerMaterials/`、道路テクスチャなど約 1 GB）。
  新しい rock-editor はこれらを認識しないので、ルートを開くと
  `project.reproj` を新たに作る。不要になったら手で整理する。
- `assets/icons/rock_editor.ico` はファイル名だけ変えたもので、**絵柄は道路のまま**。
  差し替えが要る（`rock_editor.prompt.txt` も道路向けの生成プロンプトのまま）。
- `AGENTS.md` / `CLAUDE.md` はバージョンの基準を `package.json` と書いているが、
  このプロジェクトに `package.json` は無い。実際の基準は `CMakeLists.txt` の
  `project(rock_editor VERSION ...)`。
- 旧 road-editor の状態は Git の最初のコミット（`chore: 既存の road_editor /
  TerrainGraph 時点のスナップショット`）に残してある。
