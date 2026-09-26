# Mask Combine

作成日時: 2026-09-22 18:25
更新日時: 2026-09-26 17:59

[設計メモ](rock-shaping-nodes.md) の 6（形状からマスクを作る）の「マスクどうしの合成」。2つのマスク画像を画素ごとに合成して、1つのマスクにする。「溝のうち上面を除く」「溝と上面の両方」のように、[Shape Mask](shape-mask.md) の種類を組み合わせた条件を Apply Material の Mask へ渡すのに使う。

## 接続

```text
UV Unwrap ─┬→ Shape Mask（オクルージョン）──→ A ┐
           │                                   Mask Combine（差）──→ Apply Material の Mask
           └→ Shape Mask（上向き度）────────→ B ┘
```

- 入力は A と B（どちらも Mask）、出力は Mask。A / B には Shape Mask、[Noise Mask](noise-mask.md)、別の Mask Combine をつなぐ。直列につなげば3枚以上も合成できる。
- Material Mask（定数 / 画像。面の中心で読む）は合成できない。つなぐと診断する。UV空間の画像を持たないため。
- A と B は**同じ UV Unwrap の出力から作ったマスク**にする。UVのアトラス寸法か面数が違うと診断する。合成した画像は A のメッシュのUVで貼る。
- 出力の解像度は大きいほうの入力に合わせ、もう一方は線形補間で読む。
- 使う側（Apply Material / Subdivide / Displace / Material Bake / 描画）は Shape Mask と同じ経路で、`GeneratedRock::maskImages` に Mask Combine のノード ID で画像が乗る。

## 設定

| 設定 | 内容 |
| --- | --- |
| 演算 | 乗算（A × B）/ 最大（和）/ 最小（積）/ 差（A − B）/ 混合 |
| 混合 | 混合のみ。0 で A、1 で B |
| 下限 / 上限 | 合成した値のうち、下限以下を 0、上限以上を 1 へ伸ばす。既定 0 / 1 |
| カーブ（ガンマ） | 伸ばした値に `^ ガンマ` を掛ける。0.1〜10、対数スライダー |
| 反転 | `1 - mask`。**Shape Mask と違って画像に焼き込む**（合成は安価なので、作り直して問題ない） |

演算の使い分け:

- **乗算**は「かつ」。灰色どうしはさらに暗くなる。
- **最小**も「かつ」だが、灰色の重なりで暗くならず縁が硬い。
- **最大**は「または」。溝と上面の両方に同じ素材を載せる。
- **差**は「ただし〜以外」。溝のうち上面を除く、など。0 で止める。
- **混合**は割合での補間。

入力の Shape Mask の「反転」は合成の前に掛ける（Shape Mask の反転は画像に焼き込まれていないため、ノードの設定から読む）。

## 計算と受け渡し

- `geometry::CombineMasks`（`src/geometry/MaskCombine.cpp`）。画素ごとに独立で、解像度 1024² でも数ミリ秒。進捗表示はない。
- 評価結果は Shape Mask と同じく、A のメッシュに `previewMask` を付けたもの。`previewMaskInvert` は常に false（反転は画像に入っている）。選択時のプレビューも Shape Mask と同じ仕組みで、合成後のマスクを白黒で貼る。
- キャッシュは内容キー。設定（演算 / 混合 / 下限 / 上限 / ガンマ / 反転）と、A / B のマスクのキーに **入力の反転** を加えたもの。Shape Mask の反転を切り替えると Shape Mask は再計算されず、Mask Combine だけ作り直す。
- Apply Material より下流のキャッシュ（Subdivide / Displace / Decimate）のキーには、つないだ Mask Combine のキー（その入力の Shape Mask まで）を含める。
- `NodeGraph` の補助: `IsImageMaskNodeKind`（Shape Mask / Mask Combine）、`ImageMaskInvert`（使う側で掛ける反転。Mask Combine は false）。使う側は `std::get<ShapeMaskSettings>` ではなくこれを通す。
- 保存は `maskCombine` オブジェクト（`operation` は名前、`mix` / `low` / `high` / `gamma` / `invert`）。

## 検証

- 単体テスト（`tests/ShapeMaskTests.cpp` の `RunMaskCombineTests`）: 5演算の値、入力の反転、出力の反転の焼き込み、下限/上限/ガンマ、解像度の違い、不正設定と空入力の拒否、ノード定義と接続制約、未接続・Material Mask・別UVの診断、Apply Material への受け渡しと再利用、設定変更・入力の反転・入力の Shape Mask の変更による再計算、直列の合成、Subdivide の Mask への接続。
- サンプルは `examples/mask-combine/`（Shape Mask のサンプルの錆のマスクを「オクルージョン − 上向き度」にしたもの）。

## 制限

- Material Mask との合成は未対応。面の中心の値を画像に焼く変換を足せば受けられるが、初版では対象外。
- 出力は入力と同じくUV空間の画像で、UV Unwrap をやり直すと合わなくなる（Shape Mask と同じ診断）。
