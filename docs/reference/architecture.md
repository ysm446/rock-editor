# 岩生成の設計整理

作成日時: 2026-09-20 22:06
更新日時: 2026-09-20 23:13

## 位置づけ

[v2 原仕様](../rock_generator_spec_v2.md) を既存実装へ接続するための設計案。Box の CPU メッシュ・評価・描画変換と有限亀裂パッチのガイド表示は実装済み。軸に沿う単一 Box の部分切断と Bridge の計算・表示まで実装済み。単一平面の完全分割と2片の Chunk 操作も実装済み。部分切断の任意角度化と複数切断は今後の設計。工程は [実装計画](../plan/plan.md) を参照。

## 現在のコードと追加先

| 現在のコード | 現状と追加方針 |
| --- | --- |
| `src/graph/NodeGraph.h/.cpp` | NodeKind、ValueType、variant による設定、接続検証、Revision を持つ。既存モデルを拡張し、仕様例の仮想 RockNode 階層へ全面置換しない |
| `src/app/ApplicationGraphPanel.cpp` | SyncMeshGraph は RockEvaluator の CPU 評価結果を RockMesh で描画データへ変換して転送する |
| `src/renderer/MeshData.h/.cpp` | CPU 描画データと検証を持つが道路由来の属性が残る。岩処理用トポロジーを分離し、描画境界で変換 |
| `src/renderer/`、`src/rhi/` | DX12 表示、カメラ、GPU リソース基盤を再利用 |
| `src/compositor/`、`shaders/` | 既存 Surface / PBR 評価を再利用。岩用 Triplanar は別途接続・検証 |
| `src/io/ProjectIo.cpp` | JSON 保存版は現在1。Base Rock / Crack / Fracture と片の変換・Locked を保存。Chunk 選択参照は後続 |
| `src/app/UndoHistory.*` | 既存 Undo に岩ノード設定と操作を統合 |
| `tests/`、`CMakeLists.txt` | 実処理を呼ぶジオメトリ・グラフ・保存テストを追加 |

追加候補は `src/geometry/`（CPU 形状）、`src/crack/`（有限パッチ・Bridge）、`src/fracture/`（分離・Chunk 接続）。ノード評価は既存 `src/graph/`、出力は `src/io/` に置く。新しい volume/ は SDF に着手するときに追加する。

## 状態と不変条件

| 状態 | 意味 | トポロジー/操作 |
| --- | --- | --- |
| None | 亀裂なし | 母岩 |
| Crack | 浅い表面亀裂 | 内部はつながる |
| Partial Fracture | 深い有限亀裂 | Rock Bridge が残る1連結体 |
| Full Fracture | 完全に破断 | 分離した連結成分を Chunk 化 |
| Detached but Locked | 分離済みだが拘束される | MVP は locked フラグを保持 |
| Loose Chunk | 独立して動かせる | Chunk 単位で移動・回転 |

破断段階と Locked は別の情報として保持する設計を推奨する。persistence=1 は完全貫通の候補であり、それだけで Full Fracture にしない。有限範囲・深さ・残存接続と実際のトポロジーを確認する。
Bridge の regionA / regionB は未分離領域を指す。部分亀裂の段階で架空の独立 Chunk を2つ作らない。完全分離時に領域と Chunk の対応を生成する。
Locked は破断の有無と別に保存し、明示的な解除後に Chunk Transform を許す案とする。MVP は重力・摩擦による自動判定を行わない。

## データと処理の分離

- CPU Mesh：位置、面、隣接、法線、AABB、面の由来。描画用 UV/法線の頂点分割と、形状の連結性を区別する。座標は既存の右手系 Y-up、メートルを引き継ぐ。
- JointSet：direction、spacing、spacingVariance、angleVariance、offset、count、persistence、continuity、roughness、seed。
- CrackPatch：center、normal、tangentU/V、extentU/V、depth、roughness。length / width / aperture / taper / persistence 等のノード設定から生成する。
- RockBridge：領域対、area、thickness、strength、broken。strength は MVP では表示用。形状の裏付けなしに値だけで未破断を装わない。
- RockChunk：安定 ID、CPU メッシュ参照、transform、bounds、generationLevel、parentChunkId、visible、selected、detached、locked。
- ChunkConnection：Chunk 対、jointSetId、contactArea、fractureArea、fullySeparated、locked。幾何的な接触と破断の由来を区別する。

Crack は Geometry とパッチから部分亀裂の形状・CrackField・Bridge 情報を作る。Fracture はその結果から分離を評価し ChunkSet と接続情報を生成する。単一平面分割関数だけで両処理を兼用しない。

## ノード接続案

```text
Base Rock ── Geometry ──→ Crack ── Geometry + CrackField ──→ Fracture
Joint Set ── JointSet ──→ Crack                              │
                                                        ChunkSet
                                                           ↓
                                                    Select Chunk
                                                           ↓ Selection
                                                    Chunk Transform
                                                           ↓
                                               Chip → Mesh Output
Surface ── Material ──→ 岩の材質入力
```

Selection は対象 ChunkSet の ID と評価世代に結びつける。Chunk Transform には ChunkSet と Selection を渡し、古い選択を別の Chunk へ誤適用しない。
Geometry / CrackField / JointSet / ChunkSet / Selection を既存 ValueType に追加する案。既存 Mesh / Model / Material と混同せず、Mesh Output 境界で表示データへ変換する。Merge と接続互換性も検証する。

選択は Manual / Random / Largest / Smallest / Center / ByVolume / ByGeneration / ByJointSet。抽出は Keep、除去は Remove として選択から処理する。Translate / Rotate / Scale / Spread / Separate は明示的なノード設定として残す。

## 決定性・評価・保存

ノード ID、seed、入力の安定した順序から乱数列を作り、グローバル乱数や走査順に依存しない。Chunk ID は生成元・親・分割履歴から安定させる。上流変更で対象が消えた手動選択は無効として示す。
キャッシュキーには入力の版、設定、seed、アルゴリズム版を含め、変更の下流だけ無効化する。初期段階は単純な再評価から始め、P6 で枝単位のキャッシュを検証する。

`.rockscene` と `project.reproj` の既存役割を維持する。保存するのはグラフ、設定、seed、選択参照、材質参照、ビュー状態。生成メッシュとキャッシュは原則再生成する。保存版の更新と旧版の既定値補完・非対応版の診断は実装時に決め、往復テストを追加する。

## 実装前に確定する事項

| 論点 | 判断する段階 |
| --- | --- |
| 許容誤差、極小面・極小片、安定 ID の規則 | P0/P1、分割時に P4 で検証 |
| width と aperture の差、depth と persistence の合成、taper の終端形状 | P2/P3。範囲・単位・式をテストとともに確定 |
| 非凸形状、複数パッチ交差、Bridge 面積・厚さの推定 | P3 で制約を明示し、P5 で拡張 |
| 低周波の破断面ノイズ | P7。両側境界の整合と自己交差を検証 |
| Triplanar と OBJ 材質の対応 | P7/P8。OBJ でシェーダを再現できないため、形状出力と材質再現の範囲を明示。ベイクは後続候補 |

## P1 で確定したメッシュ基盤

`geometry::Mesh` は共有位置頂点と三角形 index を保持し、描画時に面法線用の頂点を分離する。Box は原点中心、右手系 Y-up、寸法はメートル。各軸 0.001～1000 m の有限値を受け付ける。
`InspectMesh` は AABB、符号付き体積、頂点接続による連結成分、共有辺が逆向きに2回現れる閉包を調べる。縮退面は外積の二乗が辺長二乗の積の 1e-12 以下として検出する。自己交差や頂点周りの manifold 判定はまだ対象外で、亀裂実装時に拡張する。

P1 は既存 Mesh ピンを利用し、Base Rock / Merge / Mesh Output の到達可能な岩を生成する。同じ生成元は重複させない。モデルは従来どおり別経路で表示する。グラフ Revision が変わったときに再評価し、枝単位キャッシュは P6 で追加する。
不正寸法では岩の生成結果を空にし、ノード ID を含む診断を表示する。Box にノイズはなく、Seed は将来用の保存値。Triplanar・素材入力は P7 で追加する。

## P2 の有限パッチ表示規約

`crack::CrackSettings` と `CrackPatch` を追加し、Base Rock → Crack → Mesh Output でガイドを表示する。P2 は既存 Mesh ピンを用い、入力形状をそのまま渡す。CrackField と実形状の加工は P3 以降。

| 設定 | 単位・意味 |
| --- | --- |
| center | パッチ矩形の中心。ワールド座標、各軸 -10000～10000 m |
| rotationDegrees | 右手系の Z → X → Y 回転。各軸 -360～360 度 |
| extentU / extentV | ローカル U/V 方向の半幅。各 0.001～1000 m |
| depth | +V 側の辺から -V 方向へ進む距離。0～2000 m |
| persistence | 深さの進行割合。0～1 |
| aperture | 面の法線方向の全幅。0～100 m。厚みを線で表示 |
| showGuide | ガイド表示の切り替え。設定とともに保存する |

既定の基底は U=+X、V=+Y、normal=+Z。各基底を同じ回転で変換する。
表示する到達深さは `min(depth, 2 * extentV) * persistence`。中心位置を基準に +V 側の辺から進行する。0 では候補矩形だけ、1 かつ十分な depth では候補矩形全体に到達する。
この式は P2 の表示規約であり、母岩表面の交点・実際の亀裂進行・Rock Bridge の厚さを求める式ではない。P3 の部分切断では表面との交差と終端条件を別途評価する。

青は候補矩形、橙は到達矩形。母岩内部も見えるよう、深度テストを無効にした半透明面と枠を重ねる。これは透視ガイドで、岩に開口を作った結果ではない。ガイドを隠しても母岩形状は変わらない。
位置・回転は Crack の数値欄で編集する。ビューポート上のパッチ選択・専用ギズモは未実装。

描画は既存 OverlayLines シェーダを三角形リストでも利用し、既存のモデル枠・作業グリッドとは別のガイド一覧を保持する。シーン消去・入力エラー・上流プレビューへの切り替え時は古いガイドを残さない。

## P3 の部分切断と未破断部

`crack/PartialCut` の `CutBox` は、原点中心の未加工 Box と、軸に沿う単一パッチを受け取る。
`applyCut` は既定 false。旧シーンの読み込みで形状を変えない。`showBridge` は既定 true で、`showGuide` と独立して未破断断面を表示する。

パッチの到達矩形を法線方向へ aperture/2 ずつ広げた直方体と、Box の交差部分を除去する。+V 側の辺が母岩の外面まで届くことを条件に、外側から入った有限の切り込みだけを許す。P2 の表示深さには母岩外の区間も含まれ得るため、母岩内で実際に除去する V 方向の長さを「切込深さ」として別に計算する。

### メッシュ生成

Box の外面と切り込み境界の座標で各軸を最大3区間に分割する。これは直方体の差を厳密に作るための平面分割であり、均等 voxel や解像度による形状近似ではない。
除去領域に入るセルを外し、残る固体の境界だけを四角形から三角形へ変換する。全セルで同じ分割座標と頂点 ID を共有するため、T字接続、交差する面、重複した内部面を作らない。切り込みには側壁と平らな終端があり、taper・粗さ・分岐は未実装。

この方式を P3 の限定入力に採用したため、計画の汎用的な交線挿入・局所再三角形化はまだ実装していない。任意角度や一般メッシュの切断は別途拡張する。

許容誤差は対応する Box 軸寸法の 1e-6。交差幅が誤差以下なら切断せず、その理由を表示する。外面に近い U 端は外面へ揃える。90度回転の基底は成分誤差 1e-6 以内で軸へ対応づけ、斜めの切断は拒否する。
生成後は閉包、正の体積、1連結成分、計算上の除去体積との一致（元体積の 1e-5 以内）を検証する。一般メッシュの自己交差検出器は未実装。対応範囲では直交分割によって交差を避け、テストで固体境界の表裏・全頂点の面のつながりを確認する。

### Rock Bridge の意味

亀裂終端から -V 側の Box 外面まで残る領域の、亀裂面上の断面を Bridge として記録する。厚さは V 方向の残存距離、面積は切り込みの実 U 幅 × 残存厚さ。有限 U の左右にも岩が残る場合、その左右の接続面積は含まない。強度・応力や全未破断面積の推定ではない。

Bridge は Crack ノード ID に対応する評価結果として保持し、別 Chunk にはしない。計算した断面を緑で透視表示できる。実形状は常に1つの閉じた連結体。
法線方向の両側に岩が残ることと、V 方向の残存厚さが許容誤差より大きいことを条件にする。Bridge が消える深さ、開口が側面へ抜ける状態、内部空洞、加工済みメッシュ、複数 Box の同時切断は診断する。幅/深さゼロと非交差は形状を保つ。

### 評価と保存

RockEvaluator を上流からノード単位に評価する方式へ変更した。評価1回の中だけ共有上流をキャッシュし、永続的な dirty キャッシュは P6 に残す。Base Rock と加工後の Crack は別の source ID を持つため、Merge で加工前後を重ねても同じ生成元として消さない。
未加工 Box の寸法を評価結果に持ち、実切断後はその情報を外すことで、2回目の切断を拒否する。ガイドだけの Crack は引き続き連続接続できる。
保存対象は設定と表示フラグで、メッシュ・Bridge は再評価する。追加フィールドがないシーンは applyCut=false として読み込む。

## P4 の平面分割と Chunk

`fracture/PlaneSplit` は共有頂点の閉じた岩を平面の負側/正側へクリップする。元の辺ごとに交点を共有し、負側の開いた境界を反転して断面ループを組む。境界の共線頂点を保持した ear clipping で閉包を作り、正側には逆向きの同じ断面を追加する。薄すぎる耳を避け、両側の閉包・1連結性・正の体積・体積和を検証する。

- 入力はアプリが生成した未分割の岩1個。現在の入力元は Box または P3 の部分切断。任意の外部メッシュの自己交差検出や修復は含まない。
- 無限平面の中心と Z→X→Y 回転を Fracture に持つ。初期法線は +Z。Crack の有限範囲や Depth/Persistence を変更せず、渡された実形状を分割する。Crack と同じ面で残った Bridge を分けたい場合は中心と回転を一致させる。
- 断面は穴のない単純ループ1本。複数ループ、平面と既存面の重なり、非交差、分割後に片側が複数成分になる場合は出力を空にして診断する。入力上限10万三角形、断面上限4096頂点。
- 平面上判定は外接箱の最大辺長×1e-6。最小片体積は入力体積×1e-7、体積和の許容差は入力体積×1e-5。これ以下の細部を保証しない。大きく移動した片の体積はローカル基準点で積分し、変換後の誤差が1e-4を超えた場合も診断する。
- `(Fracture ノード ID, 1 または 2)` が片の ID。1 は負側、2 は正側。親は入力の生成元 ID。分割面の中心・法線・面積がこの2片の元の接続を示す。現在の接触・拘束や物理的な Locked 判定ではない。
- 片ごとの移動・回転・Locked をノード設定に保存。回転中心は分割直後の外接箱中心、回転後に移動する。Locked は現在の配置を保持して編集を抑止し、上流変更を固定するキャッシュや物理拘束ではない。
- 色分け、単一片のクリック選択/設定欄の選択、W/E ギズモ、Esc 取消に対応。倍率、複数片の一括変換、片の削除/抽出、再帰分割は後続。
- 選択中の片番号は UI の一時状態。生成 ID と変換・Locked は保存されるが、再読込時の選択復元は未対応。P6 で専用 Chunk 参照と安定 ID を拡張する。
- 分割後は上流の Crack/Bridge ガイドを除く。上流ノードのプレビューで部分切断を確認できる。移動した片に古い位置のガイドを重ねない。

Fracture の分割・変換結果はグラフ Revision ごとに再生成する。ドラッグ中の GPU 転送を含む最適化は P6。旧シーンの既存ノードは設定を変えず読み込む。
