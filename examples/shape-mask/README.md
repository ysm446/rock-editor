# 形状からマスクを作るサンプル

アプリでこのフォルダをルートとして開き、`shape-mask.rockscene` を開く。`examples/volume-noise/` のグラフの後ろに、UV展開と素材3段を足したもの。UV展開があるので、開いてから表示まで20〜30秒かかる。

- Volume to Mesh → Decimate → **UV Unwrap**（1024）：Shape Mask はUV付きのメッシュを必要とする。
- **Apply Material**（Surface「Weathered」、マスクなし）：全面を灰色の下地にする。
- **Apply Material**（Surface「Rust」、Mask に **Shape Mask**・種類「オクルージョン」）：割れ目と入隅に錆色を載せる。距離 0.25 m、下限 0.25、上限 0.7。
- **Apply Material**（Surface「Dust」、Mask に **Shape Mask**・種類「上向き度」）：上を向いた面に明るい埃色を載せる。下限 0.8、上限 0.95。
- 2つの Shape Mask の Mesh 入力は、どちらも UV Unwrap の出力から分けている。

Shape Mask のノードを選ぶと、マスクを白黒で貼った状態でプレビューされる。距離や下限・上限を動かして、どこが白くなるかを見ながら調整できる。選択を外すと元の表示へ戻る。
素材は働きを見分けるための定数色。`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/shape-mask.md) を参照。
