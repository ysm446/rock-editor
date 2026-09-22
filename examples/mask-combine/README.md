# マスクを合成するサンプル

アプリでこのフォルダをルートとして開き、`mask-combine.rockscene` を開く。`examples/shape-mask/` のグラフの錆のマスクを、**Mask Combine** で「オクルージョン − 上向き度」にしたもの。UV展開があるので、開いてから表示まで20〜30秒かかる。

- **Apply Material**（Surface「Weathered」、マスクなし）：全面を灰色の下地にする。
- **Apply Material**（Surface「Rust」、Mask に **Mask Combine**・演算「差（A − B）」）：A に Shape Mask「オクルージョン」、B に Shape Mask「上向き度」。割れ目と入隅のうち、上を向いた面を除いた所に錆色を載せる。
- **Apply Material**（Surface「Dust」、Mask に **Shape Mask**・種類「上向き度」）：上を向いた面に明るい埃色を載せる。上向き度の Shape Mask は Mask Combine の B と共用。
- 2つの Shape Mask の Mesh 入力は、どちらも UV Unwrap の出力から分けている。

Mask Combine のノードを選ぶと、合成したマスクを白黒で貼った状態でプレビューされる。演算を「最大」にすると溝と上面の両方が白くなり、「乗算」にすると溝の上面だけが残る。選択を外すと元の表示へ戻る。
素材は働きを見分けるための定数色。`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/mask-combine.md) を参照。
