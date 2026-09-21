# 自動UV展開と材質ベイク

このフォルダをルートとして開き、`uv-bake.rockscene` を読み込む。

```text
Random Boxes → To Volume → Volume to Mesh → UV Unwrap → Material Bake → Mesh Output
                                                           ↑ Material
                                                        Surface
```

1. `UV Unwrap` を選択するとビューポートがUVチェッカーになる。上部の「UVビュー」タブでUV島のワイヤーフレームを確認する。ホイールでズーム、中ボタンドラッグで移動、「全体表示」で戻す。
2. `Material Bake` を選択し「ベイク実行」を押す。このサンプルでは1024×1024のPNGを4枚、ルート内の `Bakes/MaterialBake_60.../` に生成する。ベイク後のプレビューはその画像をUVで表示する。
3. シーンを保存するとベイク材質への参照も保存される。形状・UV・Surface材質・スムーズシェーディングを変更すると再ベイクが必要になる。

Mesh OutputのMaterialは未接続にする。Surfaceをそこへ接続すると、その材質でベイク結果を上書きする。

色、DirectX法線、R=Roughness / G=Metallic / B=AO、Heightを出力する。これは現在の材質をUVへ写す機能で、形状からAOを新しく計算したり、高密度メッシュから低密度メッシュへ転写したりする処理ではない。

同梱素材は `examples/triplanar` と同じ数式生成の検証画像。詳細は [仕様と制約](../../docs/reference/uv-bake.md)。
