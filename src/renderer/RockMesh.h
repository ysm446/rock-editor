#pragma once
#include "geometry/Mesh.h"
#include "renderer/MeshData.h"
namespace rock::renderer {
// smooth が真なら、折れ角 40 度以内で向きが揃う隣の面だけを面積で重み付けして平均し、
// 岩の割れ面の角を残したまま格子由来の細かい段差をなだらかに見せる。偽なら面法線のまま。
MeshData MakeRockMeshData(const geometry::Mesh& mesh, bool smooth = false);
}
