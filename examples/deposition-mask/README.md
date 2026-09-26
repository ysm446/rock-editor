# Deposition Maskのサンプル

作成日時: 2026-09-26 19:19
更新日時: 2026-09-26 19:19

このフォルダをルートにして`deposition-mask.rockscene`を開く。欠けた積層岩に、Deposition Maskで土色のSurfaceを適用する。

Deposition Mask（ID 240）を選ぶと白黒マスク、Mesh Output（ID 15）に戻すと素材適用結果を表示する。基本操作は堆積量・隙間の範囲・許容する傾斜。詳細の「隙間を優先」を0へ下げると、開いた上面も広く塗れる。

土の厚みや形状は生成しない。Noise MaskとMask Combineを追加すれば、堆積範囲にムラを加えられる。
