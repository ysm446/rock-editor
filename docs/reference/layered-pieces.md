# 薄板の積層とピースの欠け

作成日時: 2026-09-26 05:44
更新日時: 2026-09-26 05:44

## 目的と流れ

`docs/references/DSC03322.JPG` を参考に、平行な薄板を個別に割り、側縁の破片を取り除いて段違いの輪郭を作る。まずメッシュで構造を保ち、必要な場合だけ最後にボリューム化する。

`Layered Boxes → Scatter Points / Voronoi Fracture → Piece Select → Piece Filter → Pieces to Mesh → Mesh Output`

Layered Boxes の Pieces は Scatter Points と Voronoi Fracture の Geometry 入力の両方へ接続する。Scatter Points の Points 出力を Voronoi Fracture へ渡す。分割後の Pieces は Piece Select と Piece Filter の両方へ、Selection は Piece Filter へ渡す。

## 設定

- **Layered Boxes**：枚数、寸法（Yが板厚）、隙間、厚さ・広さのばらつき、面内のずれ、全体の向き、Seed。下から層番号0、1…を付ける。板は閉じた独立メッシュで、平行を保つ。
- **Scatter Points**：Pieces 入力では個々のピース内部へ点を配置する。点数は1ピースあたり。面内配置をオンにすると、各ピースのローカルXZ面へ点を並べる。Voronoi の伸縮が等方的なら板厚を貫く切断になる。オフでは厚さ方向にも点をばらつかせる。
- **Voronoi Fracture**：各入力ピースを独立して分割する。異なる板の点は互いの分割へ影響しない。入力形状には既存の閉じた凸メッシュという制約がある。
- **Piece Select / Rim**：元の板の側縁（ローカル±X、±Z）に接する破片を選ぶ。上下面だけに接する内部の破片は選ばない。割合とSeedで側縁の一部だけを選べる。層番号が-1なら全層、0以上なら指定層だけを対象にする。反転も指定層内に限定する。
- **Piece Filter / Delete**：選んだ破片を除去する。

## データと安定性

Pieces は複数メッシュだけでなく、各片のID、変換、体積、層番号、親ピース、元の側縁との接触情報を持つ。フィルタや変換後もこれらを引き継ぐ。生成結果の頂点列はシーンへ保存せず、ノード設定から再生成する。

Scatter Points は入力ピースIDごとに乱数を分け、各親のローカル座標の点と、表示用のワールド座標の点を保持する。入力集合との対応を検査してから分割する。別の板を削除しても、残った板の点と分割IDは変わらない。設定変更による世代の更新では、既存の選択無効化規則を使う。

## 初版の範囲

- 板は1〜32枚。Scatter の点数は各2〜512点、入力集合全体で最大512点。分割結果は最大512片・25万三角形。
- Rim は**元の側縁**を追跡する。削除後に露出した境界から繰り返し侵食する処理や、隣接関係に基づく剥離は未実装。
- Pieces to Mesh は片をまとめるが、接する面の除去や頂点の溶接は行わない。滑らかな一体形状が必要なら、後段で To Volume を使う。薄い層や狭い隙間は解像度によって失われる。
- 面のうねり、構造に沿う風化、Voronoi以外のメッシュ分割は後続の課題。Volume Terrace はこの変更では置き換えない。

サンプルと操作手順は [examples/layered-pieces](../../examples/layered-pieces/README.md)。
