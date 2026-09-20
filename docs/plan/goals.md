# goals — プロジェクトの目的と完成形

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 20:05

## 目的

Windows 向けのプロシージャル岩生成エディタを作る。
単純な母岩にノードを繋いで、節理で割る・岩塊を選ぶ・ずらす・欠けさせる・浸食させる、
という処理を積み重ね、ゲーム等で使えるメッシュを生成する。

詳しい仕様は [spec.md](../spec.md) にある。ここは要点だけを置く。

## 完成形

```text
Mother Rock → Joint Set → Primary Fracture → Chunk Graph
→ Chunk Selection / Offset → Secondary Fracture → Chip / Groove
→ Erosion → Mesh Cleanup → UV / Triplanar → Surface → Export
```

- 非破壊のノードグラフで、上流を変えると下流が追従する。
- 同じグラフと同じ seed なら同じ岩になる（決定的な乱数）。
- 形状はメッシュで作り、微細な凹凸はテクスチャ（Height / Normal / Roughness）で足す。

## 重視する価値

- **単なるノイズ変形にしない。** 「岩がどう割れて塊になり、風化したか」の順番を保つ。
- 途中の結果（節理面、岩塊の色分け、破断面）が目で見えること。
- まず CPU 実装。GPU 化や voxel は後段。

## 対象外（MVP）

リアルタイム物理破壊、FEM 破壊、地質シミュレーション、GPU voxel ソルバ。
ただし拡張できる設計にする。

## 土台

road-editor（旧 terrain-graph）のアプリ基盤を引き継ぐ。
DX12 レンダラ、Dear ImGui、ノードグラフ、マテリアル合成、アセット管理は流用し、
道路・地形に特化した部分は撤去した（[progress.md](progress.md)）。
