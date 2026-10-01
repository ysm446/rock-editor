# Volume Diff Mask

作成日時: 2026-10-02 05:15
更新日時: 2026-10-02 05:15

後から足した所（または削った所）を白くするマスク。最後のメッシュの表面の各点で、比べる元のボリューム（足す・削る前）の距離を読む。形を推測する Deposition Mask と違い、実際に足した・削った所と一致する。

## 接続

```text
… → Volume Clip（接地）─┬→ Volume Close（隙間を土で埋める）→ Volume to Mesh → UV Unwrap ─┬→ Apply Material（岩）→ Apply Material（土）→ Mesh Output
                         │                                                               └→ Volume Diff Mask（Mesh）──────────↑ Mask
                         └──────────────────────────────────────────────────────────────→ Volume Diff Mask（Before）
```

- Mesh: UV 付きの最後のメッシュ（UV Unwrap の出力）。
- Before: 足す・削る前の Volume。メッシュと同じ位置にそろえる。Volume Clip の接地や Volume Transform で形を動かすなら、動かした後から Before を取る（動かす前と比べると全面がずれて白くなる）。

## 設定

| 設定 | 内容 |
| --- | --- |
| 比べ方 | 足した所（Before の形の外にある表面）／削った所（Before の形の内側にある表面） |
| しきい値 | Before の表面からこれより離れた所から白くする（m、既定 0.03）。Before より後で Volume Noise・Edge Wear を掛けたなら、その揺れより大きくする |
| ぼかし | 黒から白へ移る幅（m、既定 0.05） |
| マスク解像度 | 128〜4096。既定 1024 |
| 反転 | 使う側で白黒を反転する。画像の再計算は不要 |

## 使い道

- 岩と岩の隙間を埋めた土（Volume Close の前を Before に）。
- 礫岩・角礫岩の礫（Volume Scatter の和の前を Before に）。礫と基質を別の素材にできる。
- 割れ目・欠けた跡の新しい面（削った所。Volume Crack・Plane Cuts の前を Before に）。

## 判定

Shape Mask と同じ UV ラスタライズで画素ごとの表面の位置を求め、Before の距離（負が内部）を線形補間で読む。足した所は距離 d、削った所は -d を使い、しきい値からしきい値 + ぼかしの間を smoothstep で 0 から 1 へ移す。Before の格子の外は、外周の値に外へ出た距離を足して読む。
