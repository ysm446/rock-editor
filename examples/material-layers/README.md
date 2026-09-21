# Apply Materialと形状AOのサンプル

`examples/material-layers`をルートとして開き、`material-layers.rockscene`を開きます。

- 2つのApply Materialで茶色の下地と緑の素材を重ねます。
- Material Maskの画像を外すと定数マスクになり、値0で下地、1で上の素材を確認できます。
- 2つ目のApply MaterialのMask接続を外すと、上の素材で全面を置換します。
- Material Bakeの「ベイク実行」で、合成した素材と形状AOをUVへ焼き付けます。画像はこのルートのBakesへ生成されます。
- AOの距離・強度を変えたら再ベイクします。既定の512画素・16サンプルで試せます。

素材は合成を確認するための定数色です。付属マスクは手続き的な白黒模様、空はプロシージャル設定で、外部の素材ダウンロードは不要です。

詳しくは[設計と制限](../../docs/reference/material-application.md)を参照してください。
