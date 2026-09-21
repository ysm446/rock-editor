# 表面をノイズで崩すサンプル

アプリでこのフォルダをルートとして開き、`volume-noise.rockscene` を開く。`examples/volume-crack/` のグラフの最後に Volume Noise を足したもので、[設計メモ](../../docs/reference/rock-shaping-nodes.md) の 1〜4 のうち、形を作る流れが一通りつながっている。

- Random Boxes → To Volume（解像度96）→ Plane Cuts（全体・5枚）：面取りした直方体の塊。
- Base Shape（囲む Box）→ Scatter Points → Volume Crack：割れ目。
- Plane Cuts（局所）：稜線と角の欠け。
- **Volume Noise**：種類は「小面」、量 0.02、細かさ 10、重ねる数 2、歪み 0.02。歪みが割れ目と面取りの直線を波打たせ、小面のノイズが平らな面を段差のある割れ肌にする。
- Volume to Mesh（Dual Contouring）→ Mesh Output：表示する。

Volume Noise の1つ前（Plane Cuts）の出力ピンをクリックすると、ノイズを掛ける前の形と見比べられる。
種類を「セル状」「なめらか」へ切り替えると質感が変わる。歪みを 0 にすると、直線的な割れ目と平面が戻る。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [仕様](../../docs/reference/volume-noise.md) を参照。
