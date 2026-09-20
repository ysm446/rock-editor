# 直方体の塊とボリュームのサンプル

アプリでこのフォルダをルートとして開き、`random-boxes.rockscene` を開く。

- Random Boxes：8個の直方体を、Seed 42・回転幅25度で重ねる。
- To Volume：解像度48で一体のボリュームへ変換する。
- Volume to Mesh：ボリュームの表面を Mesh に変換する。
- Mesh Output：変換したメッシュを表示する。

Random Boxes の出力ピンをクリックすると変換前、To Volume の出力ピンでは変換後を比較できる。
Seed や回転幅を変えて形を試す。`Skies/preview.rocksky` はこのサンプルの照明用アセット。
詳しくは [使い方](../../docs/reference/box-volume.md) を参照。
