#pragma once
#include "geometry/Mesh.h"
#include "renderer/MeshData.h"
namespace rock::renderer {
// smooth が真なら、折れ角（既定 60 度）以内で向きが揃う隣の面だけを面積で重み付けして平均し、
// 岩の割れ面の角を残したまま格子由来の細かい段差をなだらかに見せる。偽なら面法線のまま。
// creaseDegrees はスムーズシェーディングの折れ角（度）。隣の面との角度がこれ以上の辺は法線を分けて折れ目を残す。
MeshData MakeRockMeshData(const geometry::Mesh& mesh, bool smooth = false, float creaseDegrees = 60.0f);
}
