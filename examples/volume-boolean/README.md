# ボリュームどうしのブーリアンのサンプル

アプリでこのフォルダをルートとして開き、`volume-boolean.rockscene` を開く。

- Random Boxes → To Volume（解像度64）：直方体の塊。Volume Boolean の A へ入れる。
- Base Shape（Ellipsoid・弱いノイズ）→ To Volume（解像度48）→ Volume Transform：B にする形。Volume Transform で塊の肩へ寄せる。
- Volume Boolean：演算は「差 (A − B)」。塊を楕円体でえぐる。
- Volume to Mesh（Dual Contouring）→ Mesh Output：結果を表示する。

Volume Boolean を選び、演算を「和」「交差」へ切り替えると形が変わる。「なめらかさ」を上げると、和ではつなぎ目が埋まり、差ではえぐった縁が丸く削れる。
B 側の Volume Transform を選ぶとビューポートにギズモが出て、えぐる位置を動かせる。
B を A から大きく離すと、和の格子が各軸192点の上限を超えて診断が出る。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/volume-boolean.md) を参照。
