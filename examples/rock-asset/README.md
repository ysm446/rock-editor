# Rock Asset（LOD）

このフォルダをルートとして開き、`rock-asset.rockscene` を読み込む。[uv-bake](../uv-bake/README.md) のサンプルの Material Bake と Mesh Output の間に Rock Asset を挟んだもの。

```text
Random Boxes → To Volume → Volume to Mesh → UV Unwrap → Material Bake → Rock Asset → Mesh Output
                                                           ↑ Material
                                                        Surface
```

1. `Rock Asset` を選ぶと、ビューポート左上に「LOD 自動 / 0 / 1 / 2 / 3」が出る。番号を押すとその段に固定し、「LOD 自動」ではカメラから見た大きさで切り替わる。右ボタン + WASD のフライで近づいたり離れたりして、切り替わる所を確かめる。
2. 表示モードを「LOD（色分け）」にすると、LOD0 白 / LOD1 赤 / LOD2 緑 / LOD3 青で塗られる。
3. 設定欄の固定欄に段ごとの三角形数が出る（既定で 4,200 / 2,100 / 1,050 / 524 前後）。
4. `Material Bake` で「ベイク実行」すると、焼いた結果がどの段にも同じ UV で貼られる。

仕様は [Rock Asset](../../docs/reference/rock-asset.md)。
