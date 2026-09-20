# Changelog

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 23:29

## 未リリース

- Base Rock に RoundedBox / Sphere / Ellipsoid を追加。丸み・面分割数・弱い形状ノイズの強度/細かさを編集でき、Seed で凹凸を再現する。
- 曲面とノイズ付き母岩を Fracture の入力へ接続できる。形状とノイズ設定は保存/読込・Undo/Redo に対応。旧シーンはノイズなしの Box として維持する。
- 部分切断は引き続きノイズなしの Box 専用。Joint Set と複数系統の分割は次の作業。

- Fracture ノードを追加。Box または部分切断済みの岩を平面で2つの閉じた Chunk に分ける。斜めの平面と凹んだ単純断面に対応し、穴・複数ループ・極小片・面の重なりは診断する。
- Chunk を色分けし、クリック/設定欄で選択、移動・回転のギズモと数値編集、Locked による配置固定に対応。Chunk ID・親・分割面の関係を生成し、設定は保存/読込・Undo/Redo で復元する。
- 参考写真 DSC02801.JPG に対する外観目標と、細かな分岐亀裂・剥離などの不足を資料に記録。

- Crack の「部分切断 (Box)」で、軸に沿った単一の有限切り込みを実際のメッシュに生成できるようにした。亀裂壁・終端を閉じ、奥の Rock Bridge を残す。未破断断面の緑色表示と厚さ・面積の確認に対応。
- 加工前の Box と加工後の枝を分離して評価する。切断・Bridge 表示の設定は保存/読込と Undo/Redo に対応。旧シーンはガイド表示を維持する。
- 部分切断は未加工 Box 1個・90度単位の向き・切り込み1回に限定。任意角度、内部空洞、Bridge の消失、複数切断は診断する。

- Crack ノードを追加。位置・回転・有限範囲・Depth・Persistence・Aperture を編集し、候補面と到達範囲を半透明で透視表示できる。設定の保存/読み込み、Undo/Redo、表示切り替えに対応。ガイドのみのモードでは母岩の形状を変更しない。

- Base Rock ノードを追加。原点中心の Box を生成し、寸法 X/Y/Z（0.001～1000 m）と Seed を設定・保存できる。現段階では Seed は Box 形状に影響しない。
- Base Rock → Mesh Output、および Merge を介した表示と途中プレビューに対応。寸法変更の Undo / Redo と不正寸法の診断を追加。

- アプリアイコンを、岩の切断面と割れ目を表現したデザインに変更。Windows 用 ICO は 16～256px の 7 サイズを収録。

### 2026-09-20 rock-editor へ改名し、道路・地形特化の実装を撤去

- アプリ名を **Rock Editor**（`rock-editor` / `rock_editor.exe`）に変更した。
- 保存形式を改めた。プロジェクトの目印は `project.reproj`、
  シーンは `.rockscene`、共有アセットは `.rockmat` / `.rocksky` / `.rockmodel`。
  JSON の形式識別子は `rock-editor.*`、ルート内の作業フォルダは `.rock-editor`。
- 旧形式（`.tgproj` / `.tgscene` / `.tgmat` / `.tgsky` / `.tgmodel` / `.mmproj` / `.mmmat`）は
  読み込めなくなった。
- 道路・地形に特化したノードと UI を削除した
  （Road / Lane Marking / Road Mask / Decal / Shoulder / Crack / Path、
  レイヤーマテリアルと境界マテリアル、パス編集、沿道の配置データ）。
- アプリ設定と最近使ったファイルの保存先を `%LOCALAPPDATA%/rock-editor/` に、
  レイアウトを `rock_editor_imgui.ini` に変更した。以前の設定は引き継がない。
- 岩の生成ノードは未実装のため、ビューポートにはグリッドと配置したモデルだけが出る。
