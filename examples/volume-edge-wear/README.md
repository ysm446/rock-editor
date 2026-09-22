# 角だけを削るサンプル（Volume Edge Wear）

アプリでこのフォルダをルートとして開き、`volume-edge-wear.rockscene` を開く。`examples/volume-smooth-terrace/` のグラフの後ろに Volume Edge Wear を足したもの。

- Random Boxes → To Volume（解像度96）→ Plane Cuts → Volume Crack → Plane Cuts（局所）→ Volume Noise → Volume Terrace → Volume Smooth（上向きに集中）：`examples/volume-smooth-terrace/` と同じ。
- **Volume Edge Wear**：半径 0.03、量 0.025、ばらつき 0.6、細かさ 4、上向きに集中 0。Plane Cuts の切り口どうしの角と棚の縁だけが削れ、切り口の面は平らなまま残る。ばらつきで削れ方が稜線に沿って変わり、欠けた角になる。
- Volume to Mesh（Dual Contouring）→ Mesh Output：表示する。

Volume Smooth の出力ピンをクリックすると、角を削る前の形が見える。量を 0.05 にすると丸みが強くなり、ばらつきを 0 にすると全ての角が同じ幅で丸まる。「上向きに集中」を 1 にすると上側の角だけが摩耗し、下側の角は鋭いまま残る。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [Volume Edge Wear](../../docs/reference/volume-edge-wear.md) を参照。
