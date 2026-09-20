# Rock Generator / Fracture Editor — spec.md

## 1. Project Overview

Windows向けのプロシージャル岩生成エディタを作る。

ユーザーはまず単純な「母岩（Mother Rock）」を作り、そこにノードを接続して、

- 節理に沿って割る
- 一部の岩塊を選ぶ / 残す / 削除する
- 岩塊をずらす / 回転させる
- 欠けさせる
- 溝・亀裂を作る
- 浸食・風化させる
- 表面ディテールを追加する

という処理を積み重ね、最終的にゲーム等で利用できるメッシュを生成する。

最終段階では UV または Triplanar Mapping を利用し、
Megascans 等の岩テクスチャの Height / Normal / Roughness を使って
細かな表面ディテールを付加できるようにする。

このツールの特徴は、単なるノイズ変形ではなく、

> 「母岩 → 節理 → 岩塊 → 二次破砕 → 浸食 → 表面」

という岩の構造を意識した階層的生成を行うこと。

---

# 2. Target Environment

## Platform

- Windows 11
- x64
- Desktop application

## Language

- C++20 以降

## Rendering

- DirectX 12

## UI

- Dear ImGui
- ImNodes / imgui-node-editor 等のノードUIライブラリは使用可

## Development

- Visual Studio Build Tools / CMake
- VS Code
- Codex を利用した反復開発を前提とする

---

# 3. Design Principles

## 3.1 Geometry first, texture later

岩の形状を以下の3スケールに分離する。

### Macro scale

実メッシュ / ボリュームで作る。

- 全体シルエット
- 大きな節理
- 岩塊
- 大きな欠け
- 段差
- 断層的なズレ

### Meso scale

主にジオメトリ処理で作る。

- 中程度の欠け
- 溝
- 二次的な割れ
- エッジ摩耗
- 浸食

### Micro scale

テクスチャ / displacement / normal で作る。

- 微細な凹凸
- 岩肌
- Roughness
- 細かなクラック
- 粒状感

---

# 4. Core Concept

基本的な生成フロー：

```text
Mother Rock
    ↓
Joint Set
    ↓
Primary Fracture
    ↓
Chunk Graph
    ↓
Chunk Selection / Offset / Remove
    ↓
Secondary Fracture
    ↓
Chip / Groove / Crack
    ↓
Erosion
    ↓
Mesh Cleanup
    ↓
UV / Triplanar
    ↓
Height / Normal / Roughness
    ↓
Export
```

---

# 5. Important Terminology

## Mother Rock

生成処理の元となる大きな岩体。

例：

- Box
- Sphere
- Ellipsoid
- Rounded Box
- Capsule-like rock
- Imported mesh

---

## Joint

自然の岩石に存在する節理。

本ツールでは、

> 「岩が割れやすい方向と間隔を定義する仮想的な面」

として扱う。

---

## Joint Set

似た方向を持つ複数の節理面の集合。

例：

```text
Joint Set A
direction = (1, 0, 0)
spacing = 1.2m

Joint Set B
direction = (0.2, 0.9, 0.1)
spacing = 2.5m

Bedding
direction = (0, 1, 0)
spacing = 0.35m
```

複数の Joint Set を交差させることで、
岩塊らしい構造を作る。

---

## Chunk

節理によって分割された岩塊。

各 Chunk は独立した Transform を持てる。

```cpp
struct RockChunk
{
    uint64_t id;

    Transform transform;

    MeshHandle mesh;

    AABB bounds;

    int generationLevel;

    uint64_t parentChunkId;

    bool visible;
    bool selected;
};
```

---

# 6. Fracture Philosophy

## 6.1 Voronoi alone should NOT be the primary fracture system

通常の Voronoi fracture は、

- 爆発破壊
- コンクリート破砕
- 瓦礫
- 二次破砕

には向いている。

しかし自然岩では、

- 平行な割れ
- 層状の割れ
- 数方向の節理
- 異方性

が重要。

したがって MVP では、

> Plane-based Joint Fracture

を Primary Fracture とする。

Voronoi は Secondary Fracture として追加する。

---

# 7. Joint Set Algorithm

## Inputs

```text
direction
spacing
spacingVariance
angleVariance
offset
planeCount
continuity
roughness
seed
```

---

## Basic generation

Joint Set の基準方向を `N` とする。

平面：

```math
dot(P, N) = d
```

を一定間隔で生成する。

```cpp
for each i
{
    float spacing =
        baseSpacing +
        random(-spacingVariance, spacingVariance);

    Plane plane;

    plane.normal =
        perturbDirection(baseDirection, angleVariance);

    plane.distance =
        previousDistance + spacing;
}
```

---

## Joint Set Example

```text
JointSet
 ├─ direction
 ├─ spacing
 ├─ spacingVariance
 ├─ angleVariance
 ├─ planeCount
 ├─ roughness
 └─ seed
```

---

# 8. Primary Fracture

MVPでは母岩を複数の Plane で順番に分割する。

概念：

```text
Mother Rock

      |
      | Joint A
      |

████████████
████████████
────────────
████████████
████████████

      ↓

Chunk 0
Chunk 1
```

さらに別方向の Joint Set を加える。

```text
      │
──────┼──────
      │
```

最終的に複数 Chunk が生成される。

---

# 9. MVP Geometry Strategy

最初から高解像度 voxel simulation を実装しない。

MVP は以下を推奨する。

## Stage 1

Plane clipping による mesh fracture。

- Triangle mesh
- Plane intersection
- Cut surface generation
- Chunk generation

メリット：

- 実装が比較的単純
- fracture の結果が確認しやすい
- voxel resolution の問題がない
- Codex で段階的に作りやすい

---

## Stage 2

SDF / sparse voxel representation を追加する。

用途：

- erosion
- boolean carving
- groove
- irregular fracture surface
- remeshing
- smooth / sharpen

---

# 10. Chunk Graph

Chunk 間の関係を保持する。

```cpp
struct ChunkConnection
{
    uint64_t chunkA;
    uint64_t chunkB;

    Plane fracturePlane;

    float contactArea;

    int jointSetId;
};
```

```cpp
struct ChunkGraph
{
    std::vector<RockChunk> chunks;
    std::vector<ChunkConnection> connections;
};
```

用途：

- 隣接 Chunk 判定
- fracture 履歴
- 再帰 fracture
- chunk 選択
- 接触面情報
- 将来的な物理破壊

---

# 11. Chunk Selection

Primary Fracture 後、

```text
████ ███
 ██ ███
████ ███
```

のような Chunk 群を表示する。

ユーザーは、

- 1個を選択
- 複数選択
- 選択以外削除
- 選択削除
- ランダム選択
- 最大 Chunk
- 中央 Chunk
- 指定体積範囲

などを実行できる。

---

## Random Chunk Extraction

「大きな母岩を節理で割り、
その中から1つを岩アセットとして取り出す」

という生成方法をサポートする。

Node:

```text
Select Chunk
```

Parameters:

```text
mode:
    Random
    Largest
    Center
    ByVolume
    Manual

seed
minVolume
maxVolume
```

---

# 12. Chunk Transform

選択 Chunk に対して：

- Translate
- Rotate
- Scale
- Separate
- Spread

を行える。

特に重要：

```text
Fracture
    ↓
Offset
```

これにより断層やズレを表現する。

Parameters:

```text
translation
rotation
randomTranslation
randomRotation
falloff
seed
```

---

# 13. Secondary Fracture

選択 Chunk にさらに fracture を適用する。

```text
Level 0
Mother Rock

Level 1
Primary Joint Fracture

Level 2
Secondary Fracture

Level 3
Surface Detail
```

---

## Secondary fracture modes

### Joint

さらに Joint Set で割る。

### Voronoi

局所的に細かく砕く。

### Radial

衝撃点から放射状。

将来実装。

---

# 14. Hierarchical / Fractal Rock Structure

重要機能。

岩の中にさらに細かな岩構造があるような
フラクタル的な見た目を作る。

完全な数学的 fractal は目標にしない。

代わりに、

> Hierarchical Fracture

を使用する。

例：

```text
Mother Rock
 ├─ Chunk A
 │   ├─ Chunk A1
 │   ├─ Chunk A2
 │   └─ Chunk A3
 │
 ├─ Chunk B
 │
 └─ Chunk C
```

各 Chunk は：

```text
generationLevel
parentChunk
children
```

を持つ。

推奨最大レベル：

```text
0 : Mother Rock
1 : Primary fracture
2 : Secondary fracture
3 : Small fracture
```

通常は Level 2 程度までで十分。

それ以下は texture / displacement に移行する。

---

# 15. Chip Node

岩の角を欠けさせる。

目的：

Voronoi や plane fracture で生成された
不自然に鋭すぎる角を自然にする。

Parameters:

```text
amount
scale
frequency
edgeBias
randomness
seed
```

将来的には、

- curvature based
- exposed edge based
- impact based

を追加する。

---

# 16. Groove / Crack Node

表面に溝または亀裂を作る。

基礎概念は fracture と共有する。

入力：

```text
curve
plane
direction
```

Parameters:

```text
width
depth
roughness
taper
noise
penetration
```

`penetration = true` の場合、
完全に貫通させて fracture として扱える。

---

# 17. Erosion Node

MVPでは地質学的に厳密な侵食を行わない。

まずは procedural erosion とする。

Parameters:

```text
strength
scale
iterations
direction
gravityBias
noiseAmount
seed
```

将来：

- water erosion
- freeze-thaw
- wind erosion
- thermal erosion

を追加できる設計にする。

---

# 18. Fracture Surface Roughness

単純な plane cut では
切断面が完全な平面になり不自然。

そこで fracture surface に roughness を追加する。

MVP:

```text
plane
+
low frequency noise
```

Stage 2:

SDF ベースで

```text
D(x) += noise(x) * amplitude
```

のような変形を行う。

Parameters:

```text
amplitude
frequency
octaves
seed
```

---

# 19. Node Graph

Editor は非破壊ノードベースとする。

初期ノード：

```text
Base Rock
Joint Set
Fracture
Select Chunk
Chunk Transform
Secondary Fracture
Chip
Groove
Erosion
Mesh Output
Surface
Export
```

---

# 20. Node Data Model

```cpp
using NodeId = uint64_t;

enum class RockDataType
{
    Geometry,
    ChunkSet,
    Selection,
    Texture,
    Scalar
};
```

```cpp
class RockNode
{
public:

    NodeId id;

    virtual void Evaluate(
        const NodeContext& context,
        RockNodeOutput& output
    ) = 0;
};
```

ノードは upstream の変更がない場合
cache を使用する。

---

# 21. Evaluation / Caching

ノードグラフは毎フレーム全再計算しない。

各ノード：

```text
dirty
cache
dependencyHash
```

を持つ。

Parameter 変更：

```text
Node changed
    ↓
mark dirty
    ↓
downstream dirty
    ↓
lazy evaluation
```

---

# 22. Undo / Redo

Command pattern または parameter snapshot を使用する。

最低限：

```text
Ctrl+Z
Ctrl+Y
```

に対応。

---

# 23. Viewport

## Camera

- Orbit
- Pan
- Zoom
- Frame selected

## Display modes

- Shaded
- Wireframe
- Normals
- Chunk colors
- Joint planes
- Selection
- Fracture graph

---

# 24. Chunk Debug Visualization

Fracture の理解が重要なので、
Chunk ごとに debug color を割り当てる。

```text
Chunk 0 = Color A
Chunk 1 = Color B
Chunk 2 = Color C
...
```

本番マテリアルとは独立。

---

# 25. Joint Visualization

Joint Set を半透明 Plane として表示する。

表示切替：

```text
Show Joint Planes
Show Joint Normals
Show Joint IDs
```

---

# 26. Base Rock Node

Parameters:

```text
Shape
    Box
    Sphere
    Ellipsoid
    RoundedBox

Size

Roundness

BaseNoise
NoiseScale
NoiseStrength

Seed
```

BaseNoise は弱めにする。

目的は、
最初から岩肌を作ることではなく、
完全なプリミティブ感を消すこと。

---

# 27. Joint Set Presets

将来的に以下をプリセットとして用意する。

```text
Blocky
Layered
Slab
Shattered
Sheared
Columnar
```

ただしプリセットは単なる見た目ではなく、

```text
Joint directions
spacing
variance
continuity
```

のセットとして保存する。

---

# 28. Mineral / Crystal Presets

将来拡張。

実際の原子シミュレーションは行わない。

代わりに、

> 結晶構造由来の「割れやすい方向」

を抽象化する。

例：

```text
Mica-like
    strong planar cleavage

Calcite-like
    multiple cleavage directions

Halite-like
    cubic cleavage

Quartz-like
    weak cleavage
    irregular fracture
```

これらを Joint Set preset に変換する。

---

# 29. SDF Architecture

Stage 2 で追加する。

```cpp
class VolumeField
{
public:

    float Sample(Vector3 position) const;

    void Union(...);
    void Subtract(...);
    void Intersect(...);
};
```

将来的には dense voxel ではなく、

- sparse bricks
- sparse voxel grid
- OpenVDB-like structure

を検討する。

---

# 30. Why Not Full Dense Voxels

例えば 4m の岩を 1mm voxel で表現すると、

```text
4000³
```

となり現実的でない。

したがって：

```text
coarse volume
+
adaptive detail
+
mesh displacement
```

を基本思想とする。

---

# 31. Meshing

Stage 1:

Plane fracture 後の polygon mesh を直接使用。

Stage 2:

SDF → Mesh を追加。

候補：

```text
Marching Cubes
Dual Contouring
Surface Nets
```

最初は Marching Cubes でも可。

最終的には sharp feature を維持しやすい
Dual Contouring 系を検討する。

---

# 32. UV / Texture

Geometry 処理終了後に
surface 処理を行う。

重要：

> fracture 編集中は UV を固定しない。

最終 shape が確定してから UV を生成する。

---

## Mapping modes

```text
Triplanar
Auto UV
Box Projection
Imported UV
```

MVP は Triplanar を優先する。

理由：

- 岩と相性がよい
- fracture 後の UV 再展開問題を避けられる
- implementation が簡単

---

# 33. Megascans Surface

Surface node:

```text
BaseColor
Normal
Roughness
Height
AO
```

Height は displacement に使用可能。

Parameters:

```text
heightStrength
textureScale
normalStrength
roughnessMultiplier
```

---

# 34. Displacement

Displacement は micro / meso detail 用。

シルエットの主要形状は
fracture geometry で作る。

推奨：

```text
Large shape
    geometry

Medium detail
    geometry / displacement

Small detail
    normal / height
```

---

# 35. Export

MVP export:

```text
OBJ
```

Stage 2:

```text
glTF
FBX
```

可能であれば metadata も保存する。

---

# 36. Project Save Format

独自 JSON を使用。

例：

```json
{
  "version": 1,
  "nodes": [],
  "connections": [],
  "viewport": {},
  "materials": []
}
```

Geometry cache は別ファイルでもよい。

---

# 37. Recommended Folder Structure

```text
RockEditor/
│
├─ CMakeLists.txt
│
├─ src/
│   ├─ app/
│   ├─ renderer/
│   ├─ geometry/
│   ├─ fracture/
│   ├─ volume/
│   ├─ nodes/
│   ├─ ui/
│   ├─ assets/
│   └─ export/
│
├─ shaders/
│
├─ assets/
│
├─ tests/
│
└─ spec.md
```

---

# 38. Core Modules

```text
App
RendererDX12
Viewport
Camera

Mesh
MeshBuilder
MeshClipper

JointSet
FractureSolver

RockChunk
ChunkGraph

NodeGraph
NodeEvaluator

RockProject

Exporter
```

---

# 39. Mesh Plane Split API

MVPの最重要処理。

```cpp
struct MeshSplitResult
{
    Mesh positive;
    Mesh negative;

    Mesh cutSurface;
};

MeshSplitResult SplitMeshByPlane(
    const Mesh& mesh,
    const Plane& plane
);
```

要件：

- triangle / plane intersection
- vertex interpolation
- polygon reconstruction
- cut face generation
- normal generation
- manifold を可能な限り維持

---

# 40. Fracture Solver

```cpp
class FractureSolver
{
public:

    ChunkSet ApplyJointSet(
        const Mesh& source,
        const JointSet& jointSet
    );

    ChunkSet ApplyJointSets(
        const Mesh& source,
        std::span<const JointSet> sets
    );
};
```

---

# 41. Deterministic Randomness

Seed が同じなら結果も同じにする。

すべての procedural node に

```text
seed
```

を持たせる。

重要：

```text
same graph
+
same seed
=
same rock
```

---

# 42. MVP UI Layout

```text
+---------------------------------------------------+
| Menu                                              |
+---------------------+-----------------------------+
|                     |                             |
| Node Graph          |       3D Viewport           |
|                     |                             |
|                     |                             |
+---------------------+-----------------------------+
| Inspector           | Console / Stats             |
+---------------------+-----------------------------+
```

---

# 43. MVP Node Set

最初の完成ラインでは
以下だけ実装する。

```text
1. Base Rock
2. Joint Set
3. Fracture
4. Select Chunk
5. Chunk Transform
6. Chip
7. Mesh Output
8. Surface
9. Export
```

Erosion / Voronoi / SDF は
MVP後でもよい。

---

# 44. Development Milestones

## Milestone 0 — Project Skeleton

- CMake
- DX12 window
- ImGui
- viewport
- orbit camera
- simple mesh rendering

Success:

```text
Cube が表示できる
```

---

## Milestone 1 — Base Rock

- Box
- Sphere
- Ellipsoid
- Rounded Box
- simple noise deformation

Success:

```text
簡単な母岩を表示できる
```

---

## Milestone 2 — Single Plane Fracture

1枚の Plane で mesh を2分割する。

Success:

```text
母岩を2つの closed mesh に分割できる
```

---

## Milestone 3 — Joint Set

複数の plane を生成。

Success:

```text
平行な節理で複数 Chunk に分割できる
```

---

## Milestone 4 — Multiple Joint Sets

異なる方向の節理を組み合わせる。

Success:

```text
岩らしいブロック構造を生成できる
```

---

## Milestone 5 — Chunk Editor

- Chunk selection
- hide
- delete
- isolate
- translate
- rotate

Success:

```text
fracture 後の岩塊を編集できる
```

---

## Milestone 6 — Node Graph

処理を node 化。

Success:

```text
Base Rock
→ Joint Set
→ Fracture
→ Select
→ Transform
→ Output
```

が接続できる。

---

## Milestone 7 — Chip

角を欠けさせる。

Success:

```text
人工的な fracture edge を弱められる
```

---

## Milestone 8 — Surface

- Triplanar
- BaseColor
- Normal
- Roughness
- Height

Success:

```text
岩テクスチャを自然に貼れる
```

---

## Milestone 9 — Save / Load

Node graph を JSON へ保存。

---

# 45. Post-MVP

次に追加する。

```text
Secondary Voronoi fracture

Hierarchical fracture

Groove / Crack

SDF boolean

SDF erosion

Fracture surface noise

Automatic UV

Dual Contouring

Debris generation

LOD

GPU compute
```

---

# 46. Performance Targets

MVP目標：

```text
Primary chunks:
    10 - 200

Interactive edit:
    < 100 ms 目標

Heavy fracture:
    数秒以内なら許容
```

最初から GPU fracture は行わない。

CPU implementation を優先する。

---

# 47. Non-Goals for MVP

以下は最初は実装しない。

```text
real-time physical destruction

FEM fracture simulation

molecular simulation

atomic simulation

full geology simulation

water simulation

snow simulation

GPU voxel solver

nanite-like mesh system
```

ただし拡張可能な設計にする。

---

# 48. Long-Term Direction

将来的には Rock Generator を
Terrain Editor と統合できるようにする。

例：

```text
Heightfield Terrain
        ↓
Cliff Detection
        ↓
Rock Generator
        ↓
Rock / Cliff Patch
        ↓
Snow / Erosion
```

Heightfield では表現しにくい：

```text
overhang
vertical cliff
rock cavity
fracture
```

を Rock Generator 側で担当する。

---

# 49. First Implementation Task for Codex

まず以下だけ実装する。

```text
Step 1
DX12 + ImGui viewport

Step 2
Base Rock = Box

Step 3
任意 Plane を viewport に表示

Step 4
Box mesh を Plane で2分割

Step 5
分割された2 Chunkを別色表示

Step 6
Chunkをクリック選択

Step 7
Transform Gizmo で移動
```

ここまで出来てから Joint Set に進む。

---

# 50. Acceptance Criteria for First Prototype

最初の prototype は以下を満たせば成功。

- アプリが Windows で起動する
- DX12 で3D viewport が表示される
- 母岩が表示される
- fracture plane が見える
- plane を移動 / 回転できる
- Execute Fracture で岩が2つに割れる
- 切断面が生成される
- 2つの Chunk が別々に選択可能
- Chunk を移動・回転できる
- 同じ seed なら同じ結果になる
- project を保存 / 再読み込みできる

---

# 51. Coding Guidelines

- まず CPU implementation を優先
- premature optimization を避ける
- geometry algorithm と rendering を分離する
- node evaluation と UI を分離する
- random generator は deterministic
- geometry function は unit test を書く
- debug visualization を重視する

特に以下にはテストを書く：

```text
plane / triangle intersection

mesh clipping

cut polygon generation

chunk volume

chunk adjacency

joint plane generation
```

---

# 52. Important Product Philosophy

このエディタは、

> 「岩にノイズを加えて岩らしくするツール」

ではない。

目標は、

> 「岩がどのように割れ、塊になり、
> その後に風化していくかを
> プロシージャルに組み立てるツール」

である。

最初は物理的に完全である必要はない。

重要なのは、

```text
structure
→ fracture
→ hierarchy
→ erosion
→ surface
```

という順番を維持すること。
