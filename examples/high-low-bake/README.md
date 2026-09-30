# ハイポリからの転写（High → Low）

このフォルダをルートとして開き、`high-low-bake.rockgraph` を読み込む。

```text
Random Boxes → To Volume → Volume Noise → Volume to Mesh ─┬───────────────────────────┐ High
                                                          └→ Decimate → UV Unwrap → Material Bake → Rock Asset → Mesh Output
                                                                                       ↑ Material
                                                                                    Surface
```

1. Volume to Mesh の出力（約 2.9 万三角形）がハイポリ、Decimate で 2,000 三角形へ減らして UV 展開したものがローポリ。
2. `Material Bake` を選び「ベイク実行」を押す。High のメッシュから法線とハイトを転写して焼く（ログに「当たった画素」の割合が出る）。
3. 表示モードを「法線（カメラ）」にすると、ローポリの三角形の段が消え、ハイポリのなめらかな面と細かい凹凸が法線に入っているのが分かる。High の接続を外して焼き直すと比べられる。
4. 焼いた結果は Rock Asset の全ての LOD で共有される。

仕様は [UV とベイク](../../docs/reference/uv-bake.md) の「ハイポリからの転写」。
