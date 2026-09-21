# メッシュ変換方式の比較

このフォルダをアプリのルートとして開く。

- `marching-tetrahedra.rockscene`：従来方式。
- `dual-contouring.rockscene`：Dual Contouring。

どちらも同じ8個の直方体・Seed 42・解像度48・カメラ設定を使う。`Volume to Mesh` を選択し「変換方式」を切り替えても比較できる。

上流の `To Volume` の表示方式は「プレビュー設定 → SDFプレビュー（共通）」に従う。このサンプルのノード設定を比較するときは `Volume to Mesh` またはその下流の `Mesh Output` を表示する。輪郭と面の陰影は別なので、ビューポートのスムーズシェーディング設定も揃える。

方式・設定はシーンへ保存され、Undo/Redoに対応する。Dual Contouringでも入力SDFの解像度に由来する細かな凹凸は残る。詳しくは [メッシュ変換方式](../../docs/reference/volume-meshing.md)。
