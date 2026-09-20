# progress — 進捗と注意点

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 23:29

## 現在地

P5 前半の母岩4形状と弱い BaseNoise を実装した。曲面・ノイズ付き母岩も Fracture の入力にできる。次は [計画](plan.md) の P5 後半：Joint Set と複数節理。写真に対する見た目の目標は [外観目標](../reference/visual-target.md) に記録。

| 区分 | 現在の状態 |
| --- | --- |
| アプリ基盤 | DX12 / ImGui、モデル表示、グラフ編集、素材・アセット・保存基盤あり |
| 岩生成 | Base Rock の Box / RoundedBox / Sphere / Ellipsoid、丸み、分割数、弱いノイズと Seed の編集に対応。有限亀裂の表示と、軸に沿う単一の部分切断・Rock Bridge に対応。単一平面による完全分割と2片の操作に対応 |
| メッシュ接続 | RockEvaluator → RockMesh → SyncMeshGraph で表示。Merge と途中プレビューに対応 |
| 評価・保存 | Revision ごとの再評価、寸法と seed の保存/復元、Undo/Redo に対応。枝単位キャッシュは P6 |
| 次の作業 | P5 後半：Joint Set、複数系統の分割、曲面/交差亀裂 |

## 完了

### 2026-09-20 母岩形状と弱い BaseNoise（P5 前半）

- `geometry/BaseRock` に4形状と共通設定を追加。曲面は cube surface の共有頂点を投影して生成し、Sphere は直径、Ellipsoid は各軸寸法、RoundedBox は最短半辺に対する丸みで指定する。
- 面分割数は4/8/16/32（最大12,288三角形）。ノイズなしの Box と丸み0の RoundedBox は従来の Box メッシュをそのまま使う。
- seed を使った固定ハッシュと補間によるノイズを、原点からの正の放射方向変位として加える。強度0～0.15、細かさ0.5～4。強度0では Seed によらない。
- 設定パネル、保存/読込、Undo/Redo とグラフへ接続。旧シーンは Box・ノイズ0として読み込む。不明形状/不正設定は診断する。
- 全形状・全分割数の閉包、連結性、外向き面、頂点共有数、指定寸法、球/楕円体の解析曲面と体積、最小/最大寸法、最大ノイズ・丸み、Seed の再現性と変位上限を自動検証。
- 曲面の中央/斜め平面による分割、ノイズ付き母岩の分割、体積保存、曲面への部分切断拒否、Undo/Redo を検証。Debug / Release ビルドと CTest が成功。
- Release で Sphere / Ellipsoid / RoundedBox / ノイズ / Seed 違い / 斜め分割 / 旧 Box / 不明形状の画面を確認。ノイズと分割設定の保存→読込→再保存を検証。素材は `build/p5-validation/`、実行ファイルは `build/p5-release/`。

**残る範囲**：Joint Set・多片分割・交差亀裂を含む P5 全体は未完了。P3 の部分切断は引き続きノイズなし Box 専用。曲面は三角形近似で、表示は面ごとの法線。写真の主稜線・細かな亀裂・剥離・材質はこの段階で完成とはしない。


### 2026-09-20 単一平面での完全分割と Chunk 操作（P4 限定実装）

- `fracture/PlaneSplit` と Fracture ノードを追加。共有交点で両側をクリップし、断面の境界頂点を保持した三角形化で閉包を作る。両側が閉じた1連結体、体積が正、体積和が一致することを検証する。
- 入力は未分割の岩1個。単純断面1本を対象とし、任意角度の平面、Box の頂点/辺を通る平面、P3 の切り込みを含む凹断面に対応。Crack から渡された形状を分割し、Depth を自動的に完全貫通へ変更しない。
- `(ノード ID, 負側1/正側2)` の Chunk ID、親の生成元、分割面の中心・法線・面積を保持。再帰分割・複数ループ/穴・面の重なり・極小片は診断する。
- 青/茶で色分けし、クリックまたは設定欄で片を選ぶ。Locked を解除すると数値欄と W/E の移動/回転ギズモで編集できる。Locked は現在の配置を保つフラグ。物理計算はしない。
- 片の回転は分割直後の外接箱中心が基準。大きな移動でも体積積分が安定するよう基準点を変更し、変換精度が不足するときは診断する。
- 設定・変換・Locked を JSON に保存し、生成 ID を再構築する。Undo/Redo は既存スナップショットへ統合。選択中の片番号自体は UI の一時状態で、保存しない。
- Debug / Release ビルドと CTest、160組の Box 寸法・斜め平面、最小/最大寸法、辺/頂点上、法線反転、非交差、極小片、部分亀裂からの分割、断面法線・面積・体積、Merge と Undo/Redo を検証。
- Release の CLI 入力注入で、片のクリック選択、移動ギズモ、回転ギズモ、Esc による取消、Locked 中の操作抑止を確認。実マウスでの手動通し操作とは区別する。
- Box / 部分切断 / 有限長の溝 / 斜め平面 / 非交差の画面、設定の保存・再読み込みを検証。検証素材は `build/p4-validation/`、実行ファイルは `build/p4-release/`。
- ユーザー提示の `DSC02801.JPG` を外観の参考として整理。細かな分岐亀裂・剥離・材質の調整は現 MVP の機能完成と別に検証する。

**残る制約**：外部の任意メッシュの自己交差検出・修復、穴/複数断面、再帰分割、複数片一括変換、倍率・抽出・削除、選択の保存、永続的な評価キャッシュ。分割平面自体の操作は数値欄。ドラッグ中は小さなメッシュを再評価・転送する。Debug の実画面確認と手動操作は未実施。


### 2026-09-20 Box の部分切断と Rock Bridge（P3 限定実装）

- `crack/PartialCut` に、未加工 Box 1個と軸に沿う単一パッチの実切断を追加。Crack の「部分切断 (Box)」で有効にする。旧シーンは既定で無効。
- 切り込みの境界平面で Box を厳密に分割し、側壁・終端・外面を閉じた1連結体として生成する。均等 voxel や形状の解像度近似は使わない。
- +V 側の外面から入る切り込みに限定。任意角度・内部空洞・複数切断・複数 Box・Bridge 消失・開口が両側面へ抜ける設定は診断する。幅/深さゼロ・非交差は元の Box を保つ。
- 実切込深さ、未破断部の厚さと断面積を表示。Bridge の断面は緑の透視ガイドとして表示でき、亀裂の候補ガイドとは別に切り替えられる。
- 上流からノードごとに評価し、加工前と加工後の枝を別の結果として保持。評価1回の中で共有上流を再利用する。切断設定・Bridge 表示は保存/読込と Undo/Redo に対応。
- Debug / Release ビルド、既存テストを含む CTest 成功。160組の寸法・範囲・深さ、64組の90度回転、最小/最大 Box、薄い Bridge を検証した。
- 自動検証は閉包、1連結成分、各頂点周囲の面のつながり、除去体積、全三角形が固体境界にあること、側壁・終端の存在、描画データ、非破壊な枝、Undo/Redo を確認。
- Release の画面で全幅の溝、有限長の溝、Bridge 表示、90度回転、幅ゼロ、貫通時の診断、旧P2のガイド表示を確認。保存→再読込→再保存で設定と接続が一致。
- 検証素材は `build/p3-validation/`、検証用 Release は `build/p3-release/`。既存 data/ は変更していない。

**残る制約**：任意角度・一般メッシュ・複数交差への汎用切断処理、taper、粗さ、一般メッシュの自己交差検出器は未実装。P3 の対応範囲では直交分割で面の交差を避ける。専用ギズモと実マウス操作による通し検証は引き続き未対応/未実施。


### 2026-09-20 有限亀裂パッチの表示（P2）

- `crack/CrackPatch` に中心・回転・U/V 半幅・Depth・Persistence・Aperture と座標系の生成・検証を追加。
- `Base Rock → Crack → Mesh Output` で候補矩形（青）と到達矩形（橙）を半透明で透視表示する。母岩のトポロジーは変更しない。
- 設定パネルで位置・回転・各パラメータを編集し、ガイドを非表示にできる。表示深さは `min(depth, 2 * extentV) * persistence`。これは P2 の可視化規約で、母岩表面との交差や未破断部の判定はまだ行わない。
- 設定と表示フラグの保存/読込、Undo/Redo、連続した Crack、途中プレビューに対応。入力欠落・不正パラメータは診断し、古いガイドを消す。
- Debug / Release ビルド、既存と新規の CTest が成功。パッチの回転・範囲・深さの境界、開口幅、透視用描画データ、母岩不変、表示切り替え、Undo/Redo を検証。
- Release で通常の到達範囲、Persistence 0、全範囲到達、ガイド非表示を撮影して確認。実アプリの保存→読込→再保存で全パラメータと接続が一致。
- 検証素材は `build/p2-validation/`、検証用 Release は `build/p2-release/`。既存 data/ は変更しない。
- 位置・回転の編集は数値欄。パッチ専用のビューポート選択・ギズモ、およびマウス操作による通し検証は未対応/未実施。


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

- 完全分割の穴・複数ループ・多片対応。部分切断の任意角度・一般メッシュ・複数交差対応。
- Joint Set、枝単位キャッシュ、Chip、Triplanar、OBJ。
- P1 の Box は無地表示。Surface 接続と岩用 UV/Triplanar は未対応。既存の Transform ノードはモデル専用のまま。
- グラフからの寸法変更・Undo/Redo は自動テスト、Chunk ギズモは CLI 入力注入で確認。実マウス操作による一連の編集操作は未検証。

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
