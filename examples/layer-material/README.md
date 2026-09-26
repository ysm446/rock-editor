# 岩用レイヤーマテリアルのサンプル

作成日時: 2026-09-26 18:43
更新日時: 2026-09-26 18:43

このフォルダをルートにして`layer-material.rockscene`を開く。

`Materials/weathered-rock.tglayer`は、`granite.rockmat`の下地に、暗いムラと明るい斑点を重ねた3層の素材。アセットをダブルクリックし、層一覧で選んだ層の被覆量・サイズ・Seedを調整する。SurfaceからApply Material経由で岩へ適用している。

ムラは素材座標の2Dノイズ。メッシュの3D位置に基づくムラは別のNoise Maskノードを使う。
