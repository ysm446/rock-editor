# ドキュメント案内

作成日時: 2026-09-20 22:04
更新日時: 2026-10-01 08:32

現在の入口は [Random Boxes → To Volume](reference/box-volume.md)。ランダムに重ねた直方体から単純な塊を作り、ボリュームへ変換する。

Rock Editor は、岩の形をノードで組み立てる Windows 向け岩生成エディタ。現在はボリューム（直方体の塊・切断・ノイズ・摩耗など）とピース（Voronoi 分割・選別・欠け）で形を作り、マスクと素材で表面を付けて、UV 展開・ベイクまで行える。旧 Crack / Fracture / Joint Set は 2026-09-21 に撤去した。対応範囲と制約は進捗を参照する。

| 資料 | 役割 |
| --- | --- |
| [目的と完成形](plan/goals.md) | アプリの価値と完成時の操作 |
| [実装計画](plan/plan.md) | 依存順の作業と完了条件 |
| [進捗](plan/progress.md) | 実装済み・未実装・検証状況 |
| [設計整理](reference/architecture.md) | 状態モデル、既存コードへの接続、検討事項 |
| [外観目標](reference/visual-target.md) | 参考写真の特徴、必要な機能、MVP との差 |
| [検証計画](reference/validation.md) | プロトタイプと MVP の受け入れ条件 |
| [v2 原仕様](rock_generator_spec_v2.md) | 製品要件の原文（全55節） |
| [変更履歴](changelog.md) | 過去の変更 |

## ノードと機能の仕様

- **形とボリューム**: [直方体の塊とボリューム](reference/box-volume.md)、[Volume to Mesh の変換方式](reference/volume-meshing.md)、[Volume Boolean](reference/volume-boolean.md)、[Plane Cuts](reference/plane-cuts.md)、[Volume Crack](reference/volume-crack.md)、[Volume Crack の入力の拡張と割り方の候補（設計メモ）](reference/volume-crack-sources.md)、[Parallel Planes — 平行な構造面](reference/parallel-planes.md)、[Volume Noise](reference/volume-noise.md)、[Volume Smooth](reference/volume-smooth.md)、[Volume Terrace](reference/volume-terrace.md)、[Volume Close](reference/volume-close.md)、[Volume Edge Wear](reference/volume-edge-wear.md)、[Volume Clip](reference/volume-clip.md)
- **ピース**: [Voronoi分割とピース操作の設計](reference/voronoi-pieces.md)、[薄板の積層とピースの欠け](reference/layered-pieces.md)、[隣接面積によるピースの侵食](reference/piece-erosion.md)
- **メッシュ**: [Subdivide / Displace](reference/displace.md)、[Decimate](reference/decimate.md)、[Remesh](reference/remesh.md)、[Rock Asset（LOD）](reference/rock-asset.md)、[山グラフと Heightmap](reference/heightmap.md)、[Rock と Rock Scatter](reference/rock-scatter.md)
- **表面と素材**: [素材の適用と形状AO](reference/material-application.md)、[岩用レイヤーマテリアル](reference/layer-material.md)、[生成メッシュへの材質とTriplanar](reference/triplanar.md)、[自動UV展開・UVビュー・材質ベイク](reference/uv-bake.md)、[Shape Mask](reference/shape-mask.md)、[Noise Mask](reference/noise-mask.md)、[Deposition Mask](reference/deposition-mask.md)、[Mask Combine](reference/mask-combine.md)、[Mask Filter](reference/mask-filter.md)
- **設計メモ**: [岩らしい形と色のためのノード設計メモ](reference/rock-shaping-nodes.md)、[岩アセットと山グラフ（計画）](reference/rock-asset-mountain-graph.md)
- **LLM・自動化**: [LLM による岩グラフの作成（AI フレンドリー化・rock_cli）](reference/ai-authoring.md)、[岩の種類の網羅（作れる岩と足りないノード）](reference/rock-catalog.md)

要件の出発点は v2 原仕様。着手順と段階ごとの範囲は実装計画に整理する。設計資料の提案は実装済みの仕様ではない。原仕様内で段階の記述が異なる機能は計画に整理理由を記す。

`plan/` は進捗管理、`reference/` は設計・検証資料、既存の `references/` は参考写真の置き場所。ビルド・起動方法は [リポジトリ README](../README.md) を参照する。旧 `docs/spec.md` へのリンクは v2 原仕様へ移行した。
