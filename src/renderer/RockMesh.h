#pragma once
#include "geometry/Mesh.h"
#include "renderer/MeshData.h"
namespace rock::renderer {
MeshData MakeRockMeshData(const geometry::Mesh& mesh);
}
