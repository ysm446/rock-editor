# Changelog

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 20:05

## 未リリース

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
