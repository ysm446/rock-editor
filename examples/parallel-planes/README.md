# 平行面から岩を割るサンプル

作成日時: 2026-09-26 05:13
更新日時: 2026-09-26 05:13

このフォルダをルートとして開き、次のシーンを読み込む。

- `parallel-plates.rockscene`: 1系統の平行面で板状に割る。Parallel Planes #10 を選んで向き・間隔・位置を変更する。
- `cross-joints.rockscene`: 別方向の平行面 #30 と Volume Crack #40 を加え、板を横断して割る。
- `shallow-joints.rockscene`: 割れ目の深さを0.04にし、母岩の内部を残す。外面に近い角の小片は分離することがある。

Parallel Planes の Planes 出力を、Volume Crack の Planes 入力へ接続する。Points は空にする。割れ方を比較しやすいよう、幅のばらつきとゆらぎは0、母岩は無地のBoxにした。岩らしい完成外観ではなく、構造と割れ方を確認するためのシーン。

[仕様と制限](../../docs/reference/parallel-planes.md)。
