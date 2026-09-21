# Voronoi分割とピース操作

`examples/voronoi-pieces` をルートとして開き、`voronoi-pieces.rockscene` を開く。

縦長のBoxを32分割し、X方向の外面に接する片を削除した後、残りから10片を抽出するサンプル。Base Shapeのノイズは0。Voronoi Fractureの伸長は `(1, 6, 1)`。

## 途中結果を見る

各ノードの出力ピンをクリック、またはノードをダブルクリックして比較できる。

| ノード | 結果 |
| --- | --- |
| Base Shape #1 | 元のBox |
| Scatter Points #10 | 内部の32点 |
| Voronoi Fracture #20 | 色分けした32片 |
| Piece Select #30 | +X/-Xの外面に接する片を強調 |
| Piece Filter #40 | 選択した外側の片を削除 |
| Piece Select #50 / Piece Filter #60 | Seedで一部を選び、10片を抽出 |
| Piece Transform #70 | 各片を少し縮小し、隙間を作る |
| Pieces to Mesh #80 | 配置を適用し、仕上げ用Meshにまとめる |
| UV Unwrap #90 | UV展開とチェッカー表示。UVビューで島を確認 |
| Material Bake #110 | Surface #100をUVに焼き付ける。「ベイク実行」でルートのBakesに保存 |

## 選択と配置

- Piece SelectをManualにし、「ビューポートで選択編集」をオンにするとクリックで選べる。Shiftで追加・解除、空白で解除。ID一覧からも指定できる。
- 「Keepを追加」「Deleteを追加」で、必要な2入力を接続したPiece Filterを追加する。
- Piece TransformのSelection入力を省くと全片が対象。接続する場合は、Piecesと同じ枝から作ったPiece Selectを使う。
- 全体のギズモとIDごとのギズモを選べる。W:移動、E:回転、R:均等倍率。XYZ別の倍率は数値欄で指定する。
- 上流の寸法・Seed・点数・分割方向を変更すると、手動選択とIDごとの変換は再指定が必要。診断が出たら「リセット」を使う。

初版は閉じた単一の凸形状、2〜128点に対応。凹形状や空洞、一般のSDFメッシュは未対応。Pieces to Meshは結合だけを行い、接触面の除去やブーリアンは行わない。
