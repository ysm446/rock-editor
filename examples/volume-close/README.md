# 割れ目の奥を埋めるサンプル

アプリでこのフォルダをルートとして開き、`volume-close.rockscene` を開く。`examples/volume-noise/` のグラフの最後（Volume Noise の後ろ）に Volume Close を足したもの。

- Random Boxes → To Volume（解像度96）→ Plane Cuts → Volume Crack → Plane Cuts（局所）→ Volume Noise：`examples/volume-noise/` と同じ。
- **Volume Close**：モード「遮蔽」、距離 0.2、しきい値 0.75、サンプル数 32。表面近くの外部の点から全方向へレイを飛ばし、7 割 5 分以上が形に当たる点を埋める。割れ目の奥や壁の陰だけが埋まり、入口の V と表面の浅いくぼみは残る。
- Volume to Mesh（Dual Contouring）→ Mesh Output：表示する。

Volume Close の1つ前（Volume Noise）の出力ピンをクリックすると、埋める前の形と見比べられる。見た目はほとんど変わらないが、割れ目の奥の壁が無くなるので三角形数が減る（ビューポートの統計表示で「メッシュ三角形」を比べる）。しきい値を下げると割れ目が浅くなり、上げると奥の狭い所だけになる。
モードを「幅」（0.015）にすると、幅 0.06 m より狭い隙間を埋める。こちらは表面の細かいくぼみも狭ければ埋まる。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/volume-close.md) を参照。
