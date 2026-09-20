# plan — 実装方針と優先順位

作成日時: 2026-09-20 20:05
更新日時: 2026-09-20 20:05

## 方針

- 形状は Stage 1 として **三角メッシュの平面クリップ**で作る。SDF / voxel は Stage 2。
- ジオメトリのアルゴリズムと描画を分ける。ノード評価と UI を分ける。
- 乱数は決定的。すべての生成ノードが seed を持つ。
- ジオメトリ関数には単体テストを書く（平面と三角形の交差、メッシュのクリップ、
  切断面のポリゴン生成、岩塊の体積、隣接、節理面の生成）。

## 優先順位

### 1. 土台の整理（完了）

道路・地形に特化したコードを撤去し、アプリ名と保存形式を rock-editor に揃える。
詳細は [progress.md](progress.md)。

### 2. メッシュ基盤

- `geometry/Mesh`：頂点・インデックス・法線・AABB を持つ CPU メッシュ。
- `SplitMeshByPlane()`：平面 1 枚でメッシュを 2 つに割り、切断面を張る。
- ここまでを単体テストで固める。

### 3. Base Rock ノード

Box / Sphere / Ellipsoid / RoundedBox と弱いノイズ変形。Mesh Output へ繋いで表示する。

### 4. Fracture

- Joint Set ノード（方向・間隔・ばらつき・seed）と、節理面の半透明表示。
- Fracture ノード（平面列で母岩を分割）と、岩塊ごとのデバッグ配色。
- Chunk Graph（隣接と履歴）。

### 5. Chunk の編集

Select Chunk（Random / Largest / Center / ByVolume / Manual）と Chunk Transform。

### 6. Chip / Surface / Export

角の面取り、Triplanar と Megascans 系テクスチャ、OBJ 書き出し。

## 保留（MVP 後）

Secondary Voronoi、階層的 fracture、Groove / Crack、SDF ブーリアンと浸食、
自動 UV、Dual Contouring、LOD、GPU compute。

## 引き継いだ土台で使えるもの

ノードグラフ（追加・接続・コピー・アンドゥ・保存）、Surface ノードによる PBR マテリアル合成、
天球と IBL、モデルの取り込みと配置、ルートフォルダによるアセット管理、
ビューポート（軌道カメラ、グリッド、ワイヤーフレーム、メッシュの選択と強調）。
