# 割れ目を彫るサンプル

アプリでこのフォルダをルートとして開き、`volume-crack.rockscene` を開く。[設計メモ](../../docs/reference/rock-shaping-nodes.md) の想定グラフのうち、形を作る前半をつないだもの。

- Random Boxes → To Volume（解像度96）：6個の直方体を重ねた塊。
- Plane Cuts（全体・5枚）：塊の外側を大きく面取りする。
- Base Shape（Box 3.4 × 3.0 × 3.2）→ Scatter Points（14点）：割れ目の配置を決める点。**Scatter Points は凸な Mesh を必要とする**ので、塊そのものではなく、塊を囲む Box の中に点を散らしている。この Box は点を置く範囲としてだけ使い、表示にはつながない。
- Volume Crack：点が作る Voronoi の境界面に沿って、表面から割れ目を彫る。
- Plane Cuts（局所）：割れ目の縁を含む稜線と角を、平面の小面で欠く。
- Volume to Mesh（Dual Contouring）→ Mesh Output：表示する。

Volume Crack の出力ピンをクリックすると、欠けを入れる前の割れ目だけを見られる。
Scatter Points の点数と Seed で割れ目の配置が、Volume Crack の Seed で「どの割れ目が太く、どれが閉じるか」が変わる。深さを 1 にすると割れ目が形を貫き、塊が分かれる。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/volume-crack.md) を参照。
