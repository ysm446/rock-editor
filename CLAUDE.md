# CLAUDE.md

このファイルは、Claude Code がこのリポジトリで作業する際のプロジェクトルールです(AGENTS.md を Claude Code 向けに再構成したもの)。

## 基本方針

- このプロジェクト固有の説明、判断基準、運用ルールは日本語で書く。
- コード、コマンド、API 名、ファイルパス、識別子は既存の表記を優先し、無理に翻訳しない。
- 既存の実装方針を確認してから変更する。
- ユーザーの未コミット変更を勝手に戻さない。
- 変更は必要な範囲に留め、無関係な整形やリファクタリングを混ぜない。

## 作業開始時の確認

作業前に、まず以下を確認する。

1. [docs/plan/goals.md](docs/plan/goals.md) — プロジェクトの目的、完成形、重視する価値。
2. [docs/plan/plan.md](docs/plan/plan.md) — 実装方針、優先順位、今後の予定。
3. [docs/plan/progress.md](docs/plan/progress.md) — 現在の進捗、完了済み作業、未完了作業、注意点。

今回の依頼が現在の計画や進捗のどこに関係するかを把握してから作業する。方針と矛盾しそうな場合は、実装前に確認する。

## ドキュメント管理

- `docs/**/*.md` を新規作成または内容更新するときは、本文の先頭付近に作成日時と更新日時を書く。
- 日時は `YYYY-MM-DD HH:MM` 形式で記録する。
- 既存ドキュメントを更新した場合は、更新日時を現在の作業日時に更新する。
- 例:
  - `作成日時: 2026-05-19 22:10`
  - `更新日時: 2026-05-19 22:10`
- `docs/changelog.md` は Git 履歴やユーザー向け変更を追うための履歴として使う(日本語で書く)。
- `docs/reference/` 配下は設計資料、仕様メモ、調査資料を置く場所として使う。
- `docs/design/` 配下は実装の設計ガイドを置く場所として使う。
- `docs/plan/` 配下(goals / plan / progress)は進捗管理用の入口として保つ。

## 検証

- **ビルド・テスト・撮影などの重いコマンドは、必ず `python tools/run_low.py -- <コマンド>` を通して実行する**（低い優先度、24 コア中 8 コアだけ）。ユーザーが同じ PC で他の作業をしているので、全コアを占有しない。
- C++ を変えたら Debug ビルドを更新し、作業中は `python tools/run_low.py -- build/bin/Debug/rock_editor_tests.exe --only <群名>[,<群名>...]`（例: `--only RockScatter,MaskFilter`。`--list` で群名）で関係する群だけを確かめる。全テストはコミット前に Release で 1 回だけ実行する。`python tools/run_low.py -- cmake --build build --config Release --target rock_editor_tests` で更新してから、`python tools/run_low.py -- ctest --test-dir build -C Release --output-on-failure` を実行する。
- Debug 固有の不具合・診断の確認が必要な場合は Debug の全テストも実行する。各群の所要時間と遅い順の一覧を表示するので、重い群を特定してから改善する。CTest から成功時も見るには `-V`。計測結果と手順は [検証計画](docs/reference/validation.md) を参照。

## バージョン管理

- アプリのバージョンは `CMakeLists.txt` の `project(rock_editor VERSION ...)` を基準にする(`vcpkg.json` の `version` も揃える)。
- ユーザー向けの明確な変更を行った場合は、必要に応じて `docs/changelog.md` に記録する。
- 未確定の変更は、必要に応じて先頭付近に「未リリース」セクションを作って記録する。
- バージョン見出しや履歴見出しに日時を書く場合は `YYYY-MM-DD HH:MM` 形式を使う。
