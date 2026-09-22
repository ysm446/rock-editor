# 層状の段と、上面だけをなまらせるサンプル

アプリでこのフォルダをルートとして開き、`volume-smooth-terrace.rockscene` を開く。`examples/volume-noise/` のグラフの後ろに Volume Terrace と Volume Smooth を足したもの。

- Random Boxes → To Volume（解像度96）→ Plane Cuts → Volume Crack → Plane Cuts（局所）→ Volume Noise：`examples/volume-noise/` と同じ。
- **Volume Terrace**：段の間隔 0.12、深さ 0.025、へこむ割合 0.45、なだらかさ 0.08、ばらつき 0.5、ゆらぎ 0.3、向き (12, 0, 8) 度。少し傾いた層が交互に突き出し、堆積岩のような棚になる。
- **Volume Smooth**：なめらか、半径 0.03、量 1、上向きに集中 1。棚の上面だけが丸くなり、側面の割れ口と棚の縁の下側は鋭いまま残る。
- Volume to Mesh（Dual Contouring）→ Mesh Output：表示する。

Volume Terrace の出力ピンをクリックすると、なまらせる前の段が見える。Volume Smooth の「上向きに集中」を 0 にすると全体が丸くなり、Volume Terrace のゆらぎとばらつきを 0 にすると平らで機械的な層になる。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [Volume Terrace](../../docs/reference/volume-terrace.md) と [Volume Smooth](../../docs/reference/volume-smooth.md) を参照。
