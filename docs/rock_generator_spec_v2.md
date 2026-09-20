# Rock Generator / Fracture Editor
## spec.md — Revised Design

## 1. Goal

Windows向けのプロシージャル岩生成エディタを作る。

単にプリミティブへノイズを加えるのではなく、

> 母岩 → 節理 → 亀裂 → 部分破断 → 完全破断 → 岩塊 → 風化 → 表面ディテール

という流れで、自然岩らしい構造を作ることを目的とする。

特に重要なのは以下。

- 自然岩は必ずしも「亀裂が入った瞬間に完全分離」しない
- 亀裂が途中で止まり、奥や下部に未破断部が残ることがある
- 完全に割れていても、噛み合わせ・摩擦・重力・周囲の拘束で動かない場合がある
- 節理はランダム破砕ではなく、方向性を持つ
- Voronoi は主破砕ではなく補助・二次破砕として使う
- 岩らしさは階層的な fracture と erosion の積み重ねで作る
- 微細な岩肌は最後に Megascans 等の Height / Normal / Roughness で追加する

---

# 2. Target Environment

## Platform
- Windows 11
- x64
- Desktop application

## Language
- C++20 以降

## Graphics
- DirectX 12

## UI
- Dear ImGui
- Node editor: imgui-node-editor / ImNodes 等を使用してよい

## Development
- VS Code
- CMake
- Codex を使った反復実装を前提とする

---

# 3. Core Design Philosophy

## 3.1 Scale Separation

岩の形状は3段階に分ける。

### Macro
実ジオメトリで作る。
- 全体形状
- 主節理
- 大亀裂
- 大きな段差
- 岩塊
- 大きな欠け
- 断層的なズレ

### Meso
ジオメトリまたは SDF で作る。
- 中規模の亀裂
- 溝
- エッジ欠け
- 二次破砕
- 浸食
- 局所的な崩れ

### Micro
主にテクスチャで作る。
- 細かな凹凸
- 微細クラック
- 表面粒状感
- Normal
- Roughness
- AO
- Height displacement

---

# 4. High-Level Workflow

```text
Base / Mother Rock
        ↓
Joint Definition
        ↓
Crack Generation
        ↓
Partial Fracture
        ↓
Optional Full Split
        ↓
Chunk Graph
        ↓
Chunk Select / Remove / Offset
        ↓
Secondary Fracture
        ↓
Chip / Groove / Erosion
        ↓
Mesh Cleanup
        ↓
UV or Triplanar
        ↓
Megascans Surface Detail
        ↓
Export
```

---

# 5. Fundamental Concepts

## 5.1 Mother Rock

処理対象となる最初の岩体。

MVP では以下を用意する。
- Box
- Rounded Box
- Sphere
- Ellipsoid

将来：
- Capsule
- Convex Hull
- Imported Mesh

## 5.2 Joint

自然岩の節理を模した構造。

本ツールでは、

> 岩が割れやすい方向と位置を示す仮想的な面・領域

として扱う。

Joint は必ずしも完全な平面で岩全体を貫通しない。

## 5.3 Joint Set

似た向きを持つ節理群。

例：

```text
Joint Set A
direction = (1, 0, 0)
spacing = 1.2
angleVariance = 8 deg

Joint Set B
direction = (0.2, 0.95, 0.1)
spacing = 2.0
angleVariance = 15 deg

Bedding
direction = (0, 1, 0)
spacing = 0.35
angleVariance = 3 deg
```

---

# 6. Important Change: Crack != Full Split

このエディタでは、

> 亀裂があること  
> と  
> 完全に分離した Chunk になること

を別状態として扱う。

これは自然岩を作るうえで重要。

---

# 7. Fracture State Model

亀裂は以下の段階を持つ。

```text
0. None
1. Crack
2. Partial Fracture
3. Full Fracture
4. Detached but Locked
5. Loose Chunk
```

## 7.1 Crack

表面に亀裂が存在するが、内部で十分につながっている。

```text
██████ | ██████
██████ | ██████
████████████████
████████████████
```

## 7.2 Partial Fracture

深い亀裂があるが、一部に未破断部が残る。

未破断部を Rock Bridge と呼ぶ。

```text
██████ | ██████
██████ | ██████
██████ | ██████
████████████████
████████████████
          ↑
      rock bridge
```

## 7.3 Full Fracture

完全にトポロジーが分離している。

```text
██████   ██████
██████   ██████
██████   ██████
```

## 7.4 Detached but Locked

完全に分離しているが、
- 周囲に挟まれている
- 破断面の凹凸が噛み合っている
- 重力で支えられている
- 摩擦で動かない

などの理由でその場に残る。

MVP では物理計算せず、状態フラグとして保持する。

## 7.5 Loose Chunk

独立 Chunk として移動・回転できる。

---

# 8. Crack Parameters

各 Crack / Joint には以下を持たせる。

```text
position
direction
normal
length
depth
width
aperture
persistence
roughness
branching
taper
seed
```

特に重要：

```text
persistence
```

意味：

```text
0.0 = 表面クラックのみ
0.5 = 部分的に深く進行
1.0 = 完全貫通候補
```

---

# 9. Crack Geometry

MVP では亀裂を完全な物理シミュレーションで成長させない。

代わりに、
- fracture plane
- finite crack region
- depth
- persistence
- noise

で近似する。

---

# 10. Partial Fracture Representation

重要。

亀裂面を無限平面ではなく、

> 有限の fracture patch

として表現する。

例：

```text
center
normal
tangentU
tangentV
extentU
extentV
depth
roughness
```

これにより、
- 表面では割れている
- 内部では止まっている
- 一部だけ rock bridge が残る

を表現できる。

---

# 11. Joint Set Generation

Joint Set は以下のパラメータを持つ。

```text
direction
spacing
spacingVariance
angleVariance
offset
count
persistence
continuity
roughness
seed
```

概念：

```text
JointSet A
||||||||

JointSet B
\\\\

Combination
|\|/|\|/
```

---

# 12. Fracture Algorithm Strategy

MVP では3段階に分ける。

## Stage A: Plane / Patch Crack
- plane
- finite patch
- partial cut
- crack opening

## Stage B: Full Split
指定条件で Mesh を完全分割する。

## Stage C: Secondary Fracture
- local joint
- Voronoi
- recursive fracture

---

# 13. Voronoi Usage

Voronoi は Primary Fracture の主役にしない。

用途：
- 二次破砕
- 局所的な粉砕
- 崩れた部分
- 小さな岩片
- Debris

理由：
通常の Voronoi は等方的で、
自然岩の節理の方向性を表現しにくい。

---

# 14. Primary Fracture

主破砕は Joint Set ベース。

```text
Mother Rock
   ↓
Joint Set A
   ↓
Joint Set B
   ↓
Partial / Full Fracture
   ↓
Chunk Structure
```

---

# 15. Chunk

Full Fracture に達した部分は Chunk 化できる。

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
    bool detached;
    bool locked;
};
```

---

# 16. Rock Bridge

Partial Fracture に残る未破断部。

```cpp
struct RockBridge
{
    uint64_t id;
    uint64_t regionA;
    uint64_t regionB;
    float area;
    float strength;
    float thickness;
    bool broken;
};
```

MVPでは strength は表示用でもよい。

将来的に、
- 破壊閾値
- 応力
- 疲労
- 凍結融解

に利用できる。

---

# 17. Chunk Graph

Chunk と接触関係を保持する。

```cpp
struct ChunkConnection
{
    uint64_t chunkA;
    uint64_t chunkB;
    int jointSetId;
    float contactArea;
    float fractureArea;
    bool fullySeparated;
    bool locked;
};
```

---

# 18. Hierarchical Fracture

岩の中にさらに小さな岩構造が見える、
フラクタル的な形を作る。

完全な数学的フラクタルではなく、

> 再帰的な fracture hierarchy

で表現する。

```text
Mother Rock
 ├─ Chunk A
 │   ├─ A1
 │   ├─ A2
 │   └─ A3
 │
 ├─ Chunk B
 │   ├─ B1
 │   └─ B2
 │
 └─ Chunk C
```

推奨：

```text
Level 0 = Mother Rock
Level 1 = Primary Joint
Level 2 = Secondary Fracture
Level 3 = Small Detail
```

Level 3 以下はなるべく displacement / normal に移す。

---

# 19. Chunk Selection

Fracture 後の Chunk 群から選択できる。

Selection Modes:

```text
Manual
Random
Largest
Smallest
Center
ByVolume
ByGeneration
ByJointSet
```

Random は seed 固定。

---

# 20. Extraction Workflow

大きな母岩を節理で分割し、
その中から一部を岩アセットとして取り出す方法をサポートする。

```text
Mother Rock
↓
Joint Fracture
↓
Chunk Set
↓
Select Chunk
↓
Delete Others
↓
Secondary Detail
```

---

# 21. Chunk Transform

選択 Chunk に適用。

```text
Translate
Rotate
Scale
Spread
Separate
```

断層的表現に使う。

---

# 22. Locked Chunk Concept

完全に割れていても、
その場に噛み合って留まる状態を扱う。

MVP では：

```text
locked = true
```

だけでよい。

Stage 2 以降では、
- contact
- friction
- gravity
- collision

を評価して自動判定してもよい。

---

# 23. Chip Node

岩の角や稜線を欠けさせる。

Parameters:

```text
amount
scale
frequency
edgeBias
randomness
seed
```

将来：

```text
curvatureBased
exposedEdgeBased
contactBased
```

---

# 24. Groove / Crack Node

表面に亀裂や溝を追加。

割る処理と同じ基盤を使うが、
必ずしも完全 split はしない。

Parameters:

```text
width
depth
length
roughness
taper
noise
penetration
persistence
seed
```

`penetration` が高く、
条件を満たした場合のみ Full Fracture に昇格できる。

---

# 25. Erosion Node

初期は procedural erosion。

Parameters:

```text
strength
scale
iterations
gravityBias
direction
noiseAmount
seed
```

将来：
- thermal erosion
- freeze-thaw
- water erosion
- wind erosion

---

# 26. Fracture Surface Roughness

切断面が完全平面だと不自然。

MVP:

```text
plane + low frequency noise
```

Stage 2:

```text
SDF deformation
```

Parameters:

```text
amplitude
frequency
octaves
seed
```

---

# 27. Base Rock Node

Parameters:

```text
Shape
  Box
  RoundedBox
  Sphere
  Ellipsoid

Size
Roundness
BaseNoiseStrength
BaseNoiseScale
Seed
```

BaseNoise は弱くする。

---

# 28. Node Graph

非破壊ノードベース。

初期ノード候補：

```text
Base Rock
Joint Set
Crack
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

# 29. Important Node Separation

`Crack` と `Fracture` を分ける。

## Crack Node
- 表面亀裂
- partial fracture
- rock bridge
- aperture
- persistence

## Fracture Node
- 完全分離判定
- Chunk 化
- adjacency 生成

これにより自然岩らしい状態を作りやすくする。

---

# 30. Node Data Types

```cpp
enum class RockDataType
{
    Geometry,
    CrackField,
    JointSet,
    ChunkSet,
    Selection,
    SurfaceData,
    Scalar
};
```

---

# 31. Node Evaluation

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

dirty propagation + cache を使用。

---

# 32. Geometry Representation

MVP は Mesh ベース。

理由：
- fracture 結果を確認しやすい
- 実装が単純
- voxel resolution 問題がない
- Codex で段階的に作りやすい

---

# 33. SDF / Volume Stage

Stage 2 以降に追加。

用途：
- irregular cracks
- groove
- chip
- erosion
- boolean
- partial fracture
- fracture roughness
- remeshing

---

# 34. Volume Strategy

Dense voxel 全面採用は避ける。

将来的には：

```text
Sparse Voxel Grid
Sparse Bricks
Adaptive SDF
OpenVDB-like structure
```

を検討。

---

# 35. Meshing

SDF 採用後の候補：

```text
Marching Cubes
Dual Contouring
Surface Nets
```

最終的には岩の鋭い稜線を保持しやすい
Dual Contouring 系を検討。

---

# 36. Surface Mapping

Geometry 処理完了後に行う。

MVP：

```text
Triplanar
```

Stage 2：

```text
Auto UV
Box Projection
Imported UV
```

---

# 37. Megascans Surface

Surface Node:

```text
BaseColor
Normal
Roughness
Height
AO
```

Parameters:

```text
textureScale
heightStrength
normalStrength
roughnessMultiplier
```

---

# 38. Displacement Policy

```text
Large Shape
  Geometry

Medium Shape
  Geometry / SDF / Displacement

Small Detail
  Height / Normal
```

Megascans の Height を
大きなシルエット形成には使わない。

---

# 39. Joint Presets

プリセットは見た目だけでなく、
Joint Set のルールとして保存する。

候補：

```text
Blocky
Layered
Slab
Sheared
Shattered
Columnar
```

---

# 40. Mineral / Crystal Inspired Presets

原子シミュレーションは行わない。

結晶構造由来の
「割れやすい方向」を抽象化する。

例：

```text
Mica-like
Calcite-like
Halite-like
Quartz-like
```

それぞれ Joint Set パラメータへ変換する。

---

# 41. Deterministic Randomness

全 procedural node は seed を持つ。

```text
same graph
+ same seed
= same rock
```

---

# 42. Debug Visualization

必須。

表示モード：

```text
Shaded
Wireframe
Chunk Colors
Joint Planes
Crack Depth
Persistence
Rock Bridges
Locked Chunks
Selection
Normals
```

特に Partial Fracture は
目視できるようにする。

---

# 43. Viewport

Camera:

```text
Orbit
Pan
Zoom
Frame Selected
```

操作：

```text
Select Chunk
Select Crack
Select Joint
Transform Gizmo
```

---

# 44. Save Format

JSON ベース。

例：

```json
{
  "version": 2,
  "nodes": [],
  "connections": [],
  "materials": [],
  "viewport": {},
  "seeds": {}
}
```

---

# 45. Export

MVP：

```text
OBJ
```

Stage 2：

```text
glTF
FBX
```

---

# 46. Suggested Folder Structure

```text
RockEditor/
├─ CMakeLists.txt
├─ spec.md
├─ src/
│  ├─ app/
│  ├─ renderer/
│  ├─ geometry/
│  ├─ fracture/
│  ├─ crack/
│  ├─ volume/
│  ├─ nodes/
│  ├─ ui/
│  ├─ assets/
│  └─ export/
├─ shaders/
├─ assets/
└─ tests/
```

---

# 47. Core Classes

```text
App
RendererDX12
Viewport
Camera

Mesh
MeshBuilder
MeshClipper

Joint
JointSet
CrackPatch
RockBridge

FractureSolver

RockChunk
ChunkGraph

NodeGraph
NodeEvaluator

RockProject
Exporter
```

---

# 48. MVP Development Milestones

## Milestone 0 — App Skeleton
- DX12 window
- ImGui
- viewport
- orbit camera
- simple mesh rendering

Success:
```text
Cube が表示できる
```

## Milestone 1 — Base Rock
- Box
- Rounded Box
- Sphere
- Ellipsoid

Success:
```text
母岩を表示できる
```

## Milestone 2 — Visible Crack Plane
- Plane を表示
- position / rotate
- half-transparent debug rendering

Success:
```text
岩を切る候補面を確認できる
```

## Milestone 3 — Partial Crack

重要。

Plane を即 Full Split せず、
- finite extent
- depth
- persistence

を指定できるようにする。

Success:
```text
表面から途中までだけ入った亀裂を作れる
```

## Milestone 4 — Rock Bridge

Partial Crack の奥に
未破断部を残せる。

Success:
```text
亀裂は見えるが岩全体はまだ1つの連結体
```

## Milestone 5 — Full Split

Crack を完全分離へ変換。

Success:
```text
2つの closed mesh に分割できる
```

## Milestone 6 — Chunk Selection
- separate color
- pick
- isolate
- delete
- transform

## Milestone 7 — Joint Set

平行 / 近似平行な複数 Joint。

Success:
```text
自然な方向性を持つ割れ方を生成できる
```

## Milestone 8 — Multiple Joint Sets

2〜3系統の Joint を交差。

Success:
```text
岩塊らしい block structure を作れる
```

## Milestone 9 — Node Graph

```text
Base Rock
→ Joint Set
→ Crack
→ Fracture
→ Select
→ Transform
→ Output
```

## Milestone 10 — Chip
人工的に鋭すぎる角を崩す。

## Milestone 11 — Surface
Triplanar + Megascans。

---

# 49. Post-MVP Features

```text
Secondary Voronoi
Hierarchical fracture
SDF crack
SDF erosion
Branching cracks
Curved cracks
Fracture surface noise
Locked chunk analysis
Basic gravity
Debris generation
LOD
GPU compute
Auto UV
Dual Contouring
```

---

# 50. Explicit Non-Goals for MVP

初期段階では実装しない。

```text
FEM fracture
DEM fracture
molecular dynamics
atomic simulation
full geomechanics
real-time destruction physics
water erosion simulation
snow simulation
GPU voxel solver
```

---

# 51. First Codex Task

最初の実装では以下だけ作る。

```text
1. DX12 + ImGui viewport
2. Box rock
3. fracture plane visualization
4. finite crack patch visualization
5. crack depth / persistence parameter
6. partial crack debug representation
7. full split command
8. two resulting chunks displayed separately
9. click selection
10. transform gizmo
```

最初から Voronoi や SDF を実装しない。

---

# 52. First Prototype Acceptance Criteria

以下を満たせば最初の prototype 成功。

- Windowsで起動する
- DX12 viewport がある
- Mother Rock が表示される
- Joint / Crack Plane が表示できる
- Plane を移動・回転できる
- finite crack extent を変更できる
- crack depth を変更できる
- persistence を変更できる
- Partial Fracture を表示できる
- Rock Bridge が残る状態を作れる
- Full Split に切り替えられる
- Full Split 後は2 Chunkとして扱える
- Chunkを個別選択できる
- Chunkを移動・回転できる
- same seed で same result
- project save/load ができる

---

# 53. Coding Guidelines

- geometry と renderer を分離
- crack logic と full fracture logic を分離
- UI と node evaluation を分離
- deterministic random
- CPU-first
- debug visualization 優先
- premature optimization を避ける
- geometry functions は unit test

テスト対象：

```text
plane / triangle intersection
finite crack patch
mesh clipping
cut surface generation
rock bridge preservation
chunk adjacency
joint generation
deterministic seeds
```

---

# 54. Long-Term Terrain Integration

将来的に Terrain Editor と統合可能にする。

```text
Heightfield Terrain
        ↓
Cliff Detection
        ↓
Rock Generator
        ↓
Cliff / Rock Patch
        ↓
Snow / Erosion
```

Heightfield が苦手な、
- overhang
- vertical cliff
- cavity
- fracture
- layered rock

を Rock Generator 側で担当する。

---

# 55. Product Identity

このツールは、

> 岩をランダムに砕くツール

ではない。

また、

> ノイズで岩っぽくするツール

でもない。

目標は、

> 岩がどこで割れやすく、
> どこまで亀裂が進み、
> どこがまだつながり、
> どこが完全に分離し、
> その後どのように欠け・風化していくか

をノードとして組み立てる
プロシージャル岩生成エディタにすること。

基本原則：

```text
structure
→ joint
→ crack
→ partial fracture
→ full fracture
→ hierarchy
→ erosion
→ surface
```
