# 岩生成の検証・受け入れ計画

作成日時: 2026-09-20 22:06
更新日時: 2026-09-20 22:06

## 実行方針

[実装計画](../plan/plan.md) の各段階で、追加した実処理を呼ぶテストとビューポート確認を行う。今回の資料整理ではアプリのビルド・起動は再実行していない。

```powershell
cmake --preset x64
cmake --build --preset x64-debug
ctest --test-dir build -C Debug --output-on-failure
```

本リポジトリは C++ / CMake 構成で package.json はない。フロントエンド用 npm run build は使用しない。新規テスト用の一時データは既存 data/ の内容と衝突しない専用サブディレクトリに隔離する。

## 自動検証

| 対象 | 確認事項 |
| --- | --- |
| 母岩 | 有効な index、有限座標、非縮退面、法線、閉包、正の体積、指定寸法 |
| 平面交差 | 非交差、通常交差、頂点/辺を通る面、同一平面、向き反転、許容誤差付近 |
| 有限亀裂 | extent 外は不変、depth の上限、表面からの侵入、終端と taper、幅ゼロの扱い |
| Rock Bridge | 正の残存厚さ、1連結成分、閉じた表面。Bridge 消失時の状態更新 |
| 完全分割 | 2つの閉じた成分、切断面の向き、重複/穴なし。幅・ノイズなしなら分割前後の体積和が許容誤差内 |
| 複数節理 | 2～3系統の交差、極小片、接続対の重複なし、正しい親・世代 |
| 決定性 | 同じグラフ・seed・設定でメッシュ、ID、選択が一致。再読込とキャッシュ有無でも一致 |
| グラフ | 型不一致・循環を拒否、下流 dirty、未変更枝のキャッシュ、削除済み選択参照の診断 |
| 保存・Undo | 岩ノード設定、seed、Locked、選択、変換の往復。既存 Model / Surface も回帰確認 |
| OBJ | index、座標系、法線、出力範囲。別ビューアで読み直して形状確認 |

UV やハードエッジで描画頂点が分かれていても、それだけで別連結成分と判定しない。逆に同じ位置にある完全分離面を勝手に溶接しない。判定には形状トポロジーを使う。

## 最初のプロトタイプ（P4、原仕様 §52）

- [ ] Windows 上で起動し、DX12 viewport に Box の母岩が出る。
- [ ] Joint / Crack Plane を表示し、移動・回転できる。
- [ ] finite extent、depth、persistence を変更して結果を確認できる。
- [ ] 開口した部分亀裂があり、奥に Rock Bridge が残る。実際のメッシュが1連結体である。
- [ ] Full Split へ切り替えると2つの閉じた Chunk になり、個別に選択・移動・回転できる。
- [ ] Locked と Loose を区別し、設定が再読込後も維持される。
- [ ] 同じ seed と設定で再現し、保存・読込・Undo で結果が戻る。

## MVP（P8）

- [ ] Box / RoundedBox / Sphere / Ellipsoid と弱い BaseNoise が使える。
- [ ] 複数 Joint Set による方向性のある岩塊を生成できる。
- [ ] 選択、抽出、削除、Chunk Transform をノードとして再現できる。
- [ ] Chip とメッシュ整理後も形状が有効である。
- [ ] Triplanar と5種の表面入力を調整できる。
- [ ] OBJ を別ビューアで開ける。材質・UV の対応範囲が明示される。
- [ ] Shaded / Wireframe / Chunk Colors / Joint Planes / Crack Depth / Persistence / Rock Bridges / Locked Chunks / Selection / Normals の表示で内部状態を確認できる。
- [ ] 上流変更、保存復元、既存モデル・素材機能に回帰がない。

各段階の結果・制約・未実施理由は [進捗](../plan/progress.md) に追記する。スクリーンショットだけで閉包・連結性の合格とせず、数値検証と合わせて判定する。
