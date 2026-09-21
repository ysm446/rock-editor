# ボリュームを動かすサンプル

アプリでこのフォルダをルートとして開き、`volume-transform.rockscene` を開く。

- Random Boxes：8個の直方体を、Seed 42・回転幅25度で重ねる。
- To Volume：解像度48で一体のボリュームへ変換する。
- Volume Transform：ボリュームのまま Y 軸に30度回し、Y に 0.6m 動かし、1.25倍にする。
- Volume to Mesh：動かしたボリュームの表面を Mesh に変換する。
- Mesh Output：変換したメッシュを表示する。

Volume Transform の移動・回転・倍率を変えると、格子を作り直して形の位置・向き・大きさが変わる。
To Volume の出力ピンをクリックすると、動かす前のボリュームと比較できる。
回転すると外接箱が広がってセル数が増えるため、上流の解像度が高いと上限を超えて診断が出る。
`Skies/preview.rocksky` はこのサンプルの照明用アセット。詳しくは [使い方](../../docs/reference/box-volume.md) を参照。
