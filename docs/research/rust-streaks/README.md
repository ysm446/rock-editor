# 鉄錆・汚れの流れた筋

作成日時: 2026-10-01 12:50
更新日時: 2026-10-02 21:51

## 概要

岩に含まれる鉄分の酸化による赤褐色の着色や、水の流れに沿って残る汚れの筋を扱う。岩石の種類ではなく、露出後の表面の変化である。この研究では、割れ目や鉱物のある場所から始まり、下方へ細く伸びる筋と、周囲へ薄く広がる色のにじみを重視する。[参考：NPSの酸化と風化](https://home.nps.gov/articles/000/weathering-erosion.htm)。

[岩の研究ページ](../rocks.md) の 1 項目。分類: 表面・色（素材）。レシピは [`rust-streaks`](../../../examples/ai-recipes/rust-streaks.rockgraph)（アプリの「テンプレートから作成」から複製して開ける）。

## 今の状態

- 状態: △
- レシピ: [`rust-streaks`](../../../examples/ai-recipes/rust-streaks.rockgraph)
- 作り方: Random Boxes を Plane Cuts で割り、小面のノイズと Edge Wear、接地。Volume to Mesh → Decimate → UV Unwrap の後、灰色の下地を Apply Material し、錆色の Surface を Flow Mask（筋の長さ 0.6・幅 0.012・集中 1.5。Volume には接地後の Volume）× Noise Mask（Mask Combine の乗算、low 0.05・high 0.6）で塗る。
- 課題: 筋が太く、まだらが目立つ。実物の錆の筋は細く長く、鉄分の源（鉄の鉱物や節理）から始まる。筋の起点を指定する手段が無い。

## 今の形

![rust-streaks](latest.jpg)

4 方向（左上から 35°・125°・215°・305°、見下ろし 20°）を 2×2 にまとめたもの。`python tools/rock_research_shots.py rust-streaks` で撮り直す（latest.jpg を更新し、日時付きの経過の画像も残す）。

## 経過

何を変えて何が効いたか（効かなかったことも）。古い順。

- 2026-10-02 初版。Flow Mask を足して作った。最初は UV 展開後のメッシュを格子に変換しようとして「面の向きが不正」と診断された（→ Volume 入力を足し、接地後の Volume を繋ぐようにした）。

## 経過の画像

撮った順。コミットの日時と件名（Git の履歴から取り出したもの）。

### 2026-10-02 19:50 feat: Flow Mask を足し、Noise / Undercut に向きに集中を足し、礫を Diff Mask で色分けする

![20261002-1950.jpg](20261002-1950.jpg)
