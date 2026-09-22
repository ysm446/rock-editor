# 割れ目の奥を埋めるサンプル

アプリでこのフォルダをルートとして開き、`volume-close.rockscene` を開く。`examples/volume-noise/` のグラフの最後（Volume Noise の後ろ）に Volume Close を足したもの。

- Random Boxes → To Volume（解像度96）→ Plane Cuts → Volume Crack → Plane Cuts（局所）→ Volume Noise：`examples/volume-noise/` と同じ。
- **Volume Close**：幅 0.015。形の最長辺の 1.5%（このサンプルでは約 0.06 m）より狭い隙間を埋める。Volume Crack の V 字の割れ目は、幅がこれを下回る深さから下が埋まり、入口の V は残る。
- Volume to Mesh（Dual Contouring）→ Mesh Output：表示する。

Volume Close の1つ前（Volume Noise）の出力ピンをクリックすると、埋める前の形と見比べられる。見た目はほとんど変わらないが、割れ目の奥の壁が無くなるので三角形数が減る（ビューポートの統計表示で「メッシュ三角形」を比べる）。幅を大きくすると割れ目が浅くなり、入口まで埋まる。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/volume-close.md) を参照。
