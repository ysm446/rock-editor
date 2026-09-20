# Changelog

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 22:31

## 未リリース

- Crack ノードを追加。位置・回転・有限範囲・Depth・Persistence・Aperture を編集し、候補面と到達範囲を半透明で透視表示できる。設定の保存/読み込み、Undo/Redo、表示切り替えに対応。現段階ではガイド表示のみで、母岩の形状は変更しない。

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
