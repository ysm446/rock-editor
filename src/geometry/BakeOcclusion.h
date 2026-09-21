#pragma once
#include "geometry/Mesh.h"
#include <string>
namespace rock::geometry {
// UVの各画素からコサイン重み付き半球へレイを飛ばす。白は遮蔽なし。
bool BakeOcclusion(const Mesh &mesh, float distance, int samples, float strength,
                   std::vector<uint8_t> &pixels, std::string &error);
} // namespace rock::geometry
