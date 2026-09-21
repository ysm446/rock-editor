# Displaceサンプル

このフォルダをルートとして `displace.rockscene` を開く。

`Base Shape → Apply Material → Subdivide → Displace → UV Unwrap → Material Bake → Mesh Output`

- Surfaceは同梱の周期的なハイト画像をTriplanarで使用する。
- Subdivideは2段（16倍）、Displaceの変位量は0.08m、基準ハイトは0.5。
- Displaceをプレビューし、変位量を0にすると元の形と比較できる。
- Material Bakeで形状AOを焼き、必要なときに「テクスチャを出力…」から保存する。
- 細分化は形を丸めない。陰影を滑らかに見たい場合はプレビュー設定のスムーズシェーディングを使用する。

画像は動作を確認するための周期模様で、自然な岩の完成素材ではない。
