# 三角形を一様に作り直すサンプル

アプリでこのフォルダをルートとして開き、`remesh.rockscene` を開く。`examples/volume-noise/` のグラフの Volume to Mesh と Mesh Output の間に Remesh を足したもの。

- Random Boxes → To Volume（解像度96）→ Plane Cuts → Volume Crack → Plane Cuts（局所）→ Volume Noise → Volume to Mesh（Dual Contouring）：`examples/volume-noise/` と同じ。
- **Remesh**：辺の長さ 0.015（形の最長辺の 1.5%）、繰り返し 5、特徴辺の角度 40 度。Dual Contouring の大きさのばらつく三角形を、一様な正三角形に近い形へ作り直す。
- Mesh Output：表示する。

Remesh の1つ前（Volume to Mesh）の出力ピンをクリックすると、作り直す前の形と見比べられる（三角形数は設定欄の「現在の出力」で分かる）。辺の長さを大きくすると面数が減り、細かい凹凸が均される。特徴辺の角度を 180 にすると稜線も丸くなる。
実際の工程では、Decimate → UV Unwrap の後ろに Remesh を置き、その後ろに Apply Material → Displace を置く。Remesh は UV を引き継ぐので UV 展開を重くせず、Subdivide なしで均一な密度で Displace できる。このサンプルは分かりやすさのため UV 展開の前に置いている。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/remesh.md) を参照。
